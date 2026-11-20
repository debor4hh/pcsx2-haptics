/*
 * Intiface bridge: Buttplug protocol v3 over a WinHTTP WebSocket.
 *
 * Threads:
 *   worker - starts intiface-engine.exe if no server answers, connects, does the
 *            handshake and reads server messages; reconnects on failure.
 *   sender - every UpdateIntervalMs turns the latest rumble into ScalarCmd messages.
 * The XInput hook only stores two numbers, so it never blocks the emulator.
 */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "bridge.h"

#define MAX_DEVICES 16
#define MAX_VIB 8

Settings g_cfg;
volatile LONG g_cfg_loaded;
WCHAR g_dir[MAX_PATH];
static WCHAR g_ini[MAX_PATH];

typedef struct {
	int used, index, nvib;
	int vib[MAX_VIB];      /* ScalarCmd indices whose ActuatorType is Vibrate */
	float last[MAX_VIB];
	char name[64];
	int bat_sensor;        /* SensorReadCmd index of the battery sensor, -1 if none */
	int battery;           /* 0..100, -1 until the first reading */
	int low_warned;
	DWORD bat_asked;
	int map[MAX_VIB];      /* MAP_* source for each vibrator */
	char addr[24];         /* Bluetooth address, when we could learn it from the engine log */
	char alias[64];        /* user's name for this particular toy */
} Device;

/* Alias if the user gave one, else the model name. */
static const char *dev_label(const Device *d) { return d->alias[0] ? d->alias : d->name; }

static const WCHAR *map_names[] = {L"large", L"small", L"both", L"off"};

/* Per-model settings: [Toy: Lovense Edge]. Per-toy settings, when the address is known:
 * [Toy 6C:5C:B1:E8:F7:EE] with Alias, Model and VibratorN. Per-toy wins over per-model. */
static void model_section(const char *name, WCHAR *sec)
{
	WCHAR wname[64];
	MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
	wsprintfW(sec, L"Toy: %s", wname);
}

/* Where this toy's own settings are saved. */
static void toy_section(const Device *d, WCHAR *sec)
{
	if (d->addr[0]) wsprintfW(sec, L"Toy %S", d->addr);
	else model_section(d->name, sec);
}

static int read_map(const WCHAR *sec, int vib)
{
	WCHAR key[16], val[16];
	wsprintfW(key, L"Vibrator%d", vib + 1);
	GetPrivateProfileStringW(sec, key, L"", val, 16, g_ini);
	for (int m = 0; m < 4; m++)
		if (!lstrcmpiW(val, map_names[m])) return m;
	return -1;
}

static void load_toy_settings(Device *d)
{
	WCHAR own[80], model[80], walias[64];
	toy_section(d, own);
	model_section(d->name, model);
	for (int i = 0; i < d->nvib; i++) {
		/* defaults: one vibrator follows both motors; two follow one motor each; extras follow both */
		int m = read_map(own, i);
		if (m < 0) m = read_map(model, i);
		d->map[i] = m >= 0 ? m : d->nvib >= 2 && i < 2 ? i : MAP_BOTH;
	}
	d->alias[0] = 0;
	if (d->addr[0]) {
		GetPrivateProfileStringW(own, L"Alias", L"", walias, 64, g_ini);
		WideCharToMultiByte(CP_UTF8, 0, walias, -1, d->alias, sizeof d->alias, NULL, NULL);
	}
}

static Device g_dev[MAX_DEVICES];
static CRITICAL_SECTION g_dev_cs, g_send_cs, g_log_cs;
static HINTERNET g_ws;
static volatile LONG g_connected, g_large, g_small, g_msg_id, g_test, g_scanning;
static int g_max_ping;
static char g_server[64];
static FILE *g_log;
static HANDLE g_engine, g_job;
static DWORD g_engine_pid;
static LONGLONG g_log_pos;
static volatile LONG g_spawns, g_restarts;

/* ---------- logging ---------- */

static void log_line(const char *fmt, ...)
{
	if (!g_cfg.log) return;
	EnterCriticalSection(&g_log_cs);
	if (!g_log) {
		WCHAR path[MAX_PATH];
		wsprintfW(path, L"%s\\pcsx2-haptics.log", g_cfg.log_dir);
		g_log = _wfopen(path, L"w");
	}
	if (g_log) {
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list ap;
		va_start(ap, fmt);
		vfprintf(g_log, fmt, ap);
		va_end(ap);
		fputc('\n', g_log);
		fflush(g_log);
	}
	LeaveCriticalSection(&g_log_cs);
}

/* ---------- config ---------- */


/* Locale-independent float parser (the host app may change the C locale). */
static float parse_float(const WCHAR *s, float def)
{
	while (*s == ' ' || *s == '\t') s++;
	int neg = 0, any = 0;
	if (*s == '-') { neg = 1; s++; }
	float v = 0, frac = 0.1f;
	for (; *s >= '0' && *s <= '9'; s++, any = 1) v = v * 10 + (*s - '0');
	if (*s == '.' || *s == ',')
		for (s++; *s >= '0' && *s <= '9'; s++, any = 1, frac *= 0.1f) v += (*s - '0') * frac;
	return any ? (neg ? -v : v) : def;
}

static float ini_float(const WCHAR *sec, const WCHAR *key, float def)
{
	WCHAR buf[64];
	GetPrivateProfileStringW(sec, key, L"", buf, 64, g_ini);
	return parse_float(buf, def);
}

static void ini_put_int(const WCHAR *sec, const WCHAR *key, int v)
{
	WCHAR buf[32];
	wsprintfW(buf, L"%d", v);
	WritePrivateProfileStringW(sec, key, buf, g_ini);
}

static void ini_put_float(const WCHAR *sec, const WCHAR *key, float v)
{
	WCHAR buf[32];
	int h = (int)(v * 100 + 0.5f);
	wsprintfW(buf, L"%d.%02d", h / 100, h % 100);
	WritePrivateProfileStringW(sec, key, buf, g_ini);
}

static const WCHAR *mode_names[] = {L"max", L"mix", L"large", L"small"};

int dir_writable(const WCHAR *dir)
{
	WCHAR p[MAX_PATH];
	CreateDirectoryW(dir, NULL); /* fine if it exists; needed for a new rumble-logs folder */
	wsprintfW(p, L"%s\\.pcsx2-haptics-write-test", dir);
	HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
	if (f == INVALID_HANDLE_VALUE) return 0;
	CloseHandle(f);
	return 1;
}

/* %APPDATA%\PCSX2 Haptics: for settings when the PCSX2 folder is read-only (e.g. Program Files). */
static void appdata_dir(WCHAR *out)
{
	WCHAR base[MAX_PATH] = L".";
	GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
	wsprintfW(out, L"%s\\PCSX2 Haptics", base);
	CreateDirectoryW(out, NULL);
}

/* Settings live next to PCSX2 (portable installs) unless that folder is read-only. */
static void pick_ini(void)
{
	WCHAR local[MAX_PATH], roaming[MAX_PATH];
	wsprintfW(local, L"%s\\pcsx2-haptics.ini", g_dir);
	appdata_dir(roaming);
	lstrcatW(roaming, L"\\pcsx2-haptics.ini");
	if (GetFileAttributesW(roaming) != INVALID_FILE_ATTRIBUTES || !dir_writable(g_dir)) {
		CopyFileW(local, roaming, TRUE); /* start from the shipped ini, if any */
		lstrcpyW(g_ini, roaming);
	} else {
		lstrcpyW(g_ini, local);
	}
}

static void load_config(void)
{
	Settings *c = &g_cfg;
	WCHAR mode[16];
	pick_ini();

	GetPrivateProfileStringW(L"Intiface", L"Host", L"127.0.0.1", c->host, 128, g_ini);
	c->port = GetPrivateProfileIntW(L"Intiface", L"Port", 12345, g_ini);
	c->auto_start = GetPrivateProfileIntW(L"Intiface", L"AutoStartServer", 1, g_ini);
	GetPrivateProfileStringW(L"Intiface", L"ServerPath", L"intiface-engine.exe", c->server_path, MAX_PATH, g_ini);
	GetPrivateProfileStringW(L"Intiface", L"ServerArgs", L"", c->server_args, 256, g_ini);

	c->enabled = GetPrivateProfileIntW(L"Haptics", L"Enabled", 1, g_ini);
	c->controller = GetPrivateProfileIntW(L"Haptics", L"Controller", 0, g_ini);
	c->interval_ms = GetPrivateProfileIntW(L"Haptics", L"UpdateIntervalMs", 60, g_ini);
	if (c->interval_ms < 20) c->interval_ms = 20;
	c->large_w = ini_float(L"Haptics", L"LargeMotorWeight", 1.0f);
	c->small_w = ini_float(L"Haptics", L"SmallMotorWeight", 0.6f);
	c->scale = ini_float(L"Haptics", L"Scale", 1.0f);
	c->min_i = ini_float(L"Haptics", L"MinIntensity", 0.0f);

	GetPrivateProfileStringW(L"Haptics", L"Mode", L"max", mode, 16, g_ini);
	c->mode = MODE_MAX;
	for (int i = 0; i < 4; i++)
		if (!lstrcmpiW(mode, mode_names[i])) c->mode = i;

	c->forward = GetPrivateProfileIntW(L"Controller", L"ForwardRumble", 1, g_ini);
	c->notify = GetPrivateProfileIntW(L"Notifications", L"Enabled", 1, g_ini);
	c->overlay = GetPrivateProfileIntW(L"Overlay", L"Enabled", 0, g_ini);
	c->ovl_x = GetPrivateProfileIntW(L"Overlay", L"X", -32768, g_ini);
	c->ovl_y = GetPrivateProfileIntW(L"Overlay", L"Y", -32768, g_ini);
	c->ovl_scale = ini_float(L"Overlay", L"Scale", 1.0f);
	if (c->ovl_scale < 0.7f || c->ovl_scale > 3.0f) c->ovl_scale = 1.0f;
	c->log = GetPrivateProfileIntW(L"Debug", L"Log", 1, g_ini);
	c->log_rumble = GetPrivateProfileIntW(L"Debug", L"LogRumble", 0, g_ini);
	c->rumble_log = GetPrivateProfileIntW(L"Debug", L"RumbleCsv", 0, g_ini);
	c->ui_scale = ini_float(L"Window", L"Scale", 1.0f);

	/* default log folder: next to PCSX2, or next to the ini when PCSX2's folder is read-only */
	WCHAR def[MAX_PATH];
	lstrcpyW(def, g_ini);
	*wcsrchr(def, L'\\') = 0;
	GetPrivateProfileStringW(L"Paths", L"LogFolder", def, c->log_dir, MAX_PATH, g_ini);
	lstrcatW(def, L"\\rumble-logs");
	GetPrivateProfileStringW(L"Paths", L"RumbleFolder", def, c->csv_dir, MAX_PATH, g_ini);
	if (c->ui_scale < 0.85f || c->ui_scale > 2.5f) c->ui_scale = 1.0f;
}

/* Saves the settings that can be changed from the tray. */
void bridge_save_config(void)
{
	Settings *c = &g_cfg;
	ini_put_int(L"Haptics", L"Enabled", c->enabled);
	WritePrivateProfileStringW(L"Haptics", L"Mode", mode_names[c->mode], g_ini);
	ini_put_float(L"Haptics", L"LargeMotorWeight", c->large_w);
	ini_put_float(L"Haptics", L"SmallMotorWeight", c->small_w);
	ini_put_float(L"Haptics", L"Scale", c->scale);
	ini_put_float(L"Haptics", L"MinIntensity", c->min_i);
	ini_put_int(L"Controller", L"ForwardRumble", c->forward);
	ini_put_int(L"Notifications", L"Enabled", c->notify);
	ini_put_int(L"Overlay", L"Enabled", c->overlay);
	ini_put_float(L"Overlay", L"Scale", c->ovl_scale);
	ini_put_int(L"Debug", L"RumbleCsv", c->rumble_log);
	ini_put_float(L"Window", L"Scale", c->ui_scale);
	if (c->ovl_x != -32768) {
		ini_put_int(L"Overlay", L"X", c->ovl_x);
		ini_put_int(L"Overlay", L"Y", c->ovl_y);
	}
}

/* ---------- minimal JSON scanning ---------- */

/* p points at '{' or '['; returns the matching closer. */
static const char *match(const char *p, const char *e)
{
	int depth = 0, in_str = 0;
	for (; p < e; p++) {
		if (in_str) {
			if (*p == '\\') p++;
			else if (*p == '"') in_str = 0;
		} else if (*p == '"') in_str = 1;
		else if (*p == '{' || *p == '[') depth++;
		else if ((*p == '}' || *p == ']') && --depth == 0) return p;
	}
	return NULL;
}

static const char *find_in(const char *b, const char *e, const char *needle)
{
	size_t n = strlen(needle);
	for (; b + n <= e; b++)
		if (!memcmp(b, needle, n)) return b;
	return NULL;
}

/* Returns a pointer to the value of "key" inside [b,e), or NULL. */
static const char *find_key(const char *b, const char *e, const char *key)
{
	char q[64];
	snprintf(q, sizeof q, "\"%s\"", key);
	const char *p = find_in(b, e, q);
	if (!p) return NULL;
	p += strlen(q);
	while (p < e && (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
	return p < e ? p : NULL;
}

static const char *next_obj(const char *p, const char *e)
{
	for (; p < e; p++) {
		if (*p == '{') return p;
		if (*p == ']') return NULL;
	}
	return NULL;
}

/* Copies a JSON string value (v points at the opening quote). */
static void copy_str(char *dst, int cap, const char *v, const char *e)
{
	int i = 0;
	if (v && *v == '"')
		for (v++; v < e && *v != '"' && i < cap - 1; v++) dst[i++] = *v;
	dst[i] = 0;
}

/* ---------- devices ---------- */

/* Intiface Central can expose Xbox pads as devices; never drive the pad we are reading. */
static int is_gamepad(const char *name)
{
	char low[64];
	int i = 0;
	for (; name[i] && i < 63; i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
	low[i] = 0;
	return strstr(low, "xinput") || strstr(low, "gamepad") || strstr(low, "xbox");
}

/* The protocol doesn't carry Bluetooth addresses, but our engine logs them when it assigns
 * a device index:  ...address: "PeripheralId(6C:5C:B1:E8:F7:EE)" }}: ...Assigning index 0 to Lovense Hush
 * If the format ever changes we just don't find it, and the toy falls back to per-model settings. */
static void find_address(int index, char *out, int cap)
{
	out[0] = 0;
	if (!g_engine) return; /* someone else's server (Intiface Central): no log to read */
	WCHAR p[MAX_PATH];
	wsprintfW(p, L"%s\\intiface-engine.log", g_cfg.log_dir);
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD size = GetFileSize(f, NULL), got = 0;
	if (size > 16 << 20) size = 16 << 20;
	char *buf = malloc(size + 1);
	if (buf && ReadFile(f, buf, size, &got, NULL)) {
		buf[got] = 0;
		char needle[48];
		snprintf(needle, sizeof needle, "Assigning index %d to ", index);
		char *hit = NULL, *q = buf;
		while ((q = strstr(q, needle))) hit = q++; /* the latest registration wins */
		if (hit) {
			char *line = hit;
			while (line > buf && line[-1] != '\n') line--;
			char *a = NULL;
			for (char *r = line; r < hit && (r = strstr(r, "address: \"")) && r < hit; r++) a = r;
			if (a) {
				a += 10;
				if (!strncmp(a, "PeripheralId(", 13)) a += 13;
				int n = 0;
				while (a[n] && a[n] != '"' && a[n] != ')' && n < cap - 1) n++;
				memcpy(out, a, n);
				out[n] = 0;
			}
		}
	}
	free(buf);
	CloseHandle(f);
}

static void add_device(const char *b, const char *e)
{
	const char *v = find_key(b, e, "DeviceIndex");
	if (!v) return;
	Device d = {0};
	d.used = 1;
	d.index = atoi(v);
	copy_str(d.name, sizeof d.name, find_key(b, e, "DeviceName"), e);
	const char *sc = find_key(b, e, "ScalarCmd");
	if (sc && *sc == '[' && !is_gamepad(d.name)) {
		const char *sc_end = match(sc, e);
		int i = 0;
		for (const char *o = next_obj(sc + 1, sc_end); o; i++) {
			const char *oe = match(o, sc_end);
			if (!oe) break;
			if (find_in(o, oe, "\"Vibrate\"") && d.nvib < MAX_VIB) d.vib[d.nvib++] = i;
			o = next_obj(oe + 1, sc_end);
		}
	}
	for (int i = 0; i < MAX_VIB; i++) d.last[i] = -1;
	d.bat_sensor = d.battery = -1;
	find_address(d.index, d.addr, sizeof d.addr);
	load_toy_settings(&d);
	const char *sr = find_key(b, e, "SensorReadCmd");
	if (sr && *sr == '[') {
		const char *sr_end = match(sr, e);
		int i = 0;
		for (const char *o = next_obj(sr + 1, sr_end); o; i++) {
			const char *oe = match(o, sr_end);
			if (!oe) break;
			if (find_in(o, oe, "\"Battery\"")) { d.bat_sensor = i; break; }
			o = next_obj(oe + 1, sr_end);
		}
	}

	EnterCriticalSection(&g_dev_cs);
	int slot = -1;
	for (int i = 0; i < MAX_DEVICES; i++)
		if (g_dev[i].used && g_dev[i].index == d.index) { slot = i; break; }
	for (int i = 0; slot < 0 && i < MAX_DEVICES; i++)
		if (!g_dev[i].used) slot = i;
	if (slot >= 0) g_dev[slot] = d;
	LeaveCriticalSection(&g_dev_cs);

	if (d.nvib) {
		log_line("Device ready: [%d] %s%s%s (%d vibrator%s, battery %s, address %s)", d.index, d.name,
		         d.alias[0] ? " / " : "", d.alias, d.nvib, d.nvib == 1 ? "" : "s",
		         d.bat_sensor >= 0 ? "reported" : "not reported", d.addr[0] ? d.addr : "unknown");
		char shown[140];
		snprintf(shown, sizeof shown, d.alias[0] ? "%s (%s)" : "%s", d.alias[0] ? d.alias : d.name, d.name);
		ui_notify("Toy connected", shown);
	} else {
		log_line("Device ignored (no vibrators or a gamepad): [%d] %s", d.index, d.name);
	}
	ui_refresh();
}

static void remove_device(int index)
{
	EnterCriticalSection(&g_dev_cs);
	for (int i = 0; i < MAX_DEVICES; i++)
		if (g_dev[i].used && g_dev[i].index == index) {
			log_line("Device removed: [%d] %s", index, g_dev[i].name);
			if (g_dev[i].nvib) ui_notify("Toy disconnected", dev_label(&g_dev[i]));
			g_dev[i].used = 0;
		}
	LeaveCriticalSection(&g_dev_cs);
	ui_refresh();
}

static void on_battery(int index, int level)
{
	char warn[64] = "";
	EnterCriticalSection(&g_dev_cs);
	for (int i = 0; i < MAX_DEVICES; i++) {
		Device *d = &g_dev[i];
		if (!d->used || d->index != index) continue;
		if (d->battery != level) log_line("Battery: %s %d%%", d->name, level);
		d->battery = level;
		if (level <= 20 && !d->low_warned) {
			d->low_warned = 1;
			snprintf(warn, sizeof warn, "%s is at %d%%.", dev_label(d), level);
		}
	}
	LeaveCriticalSection(&g_dev_cs);
	if (warn[0]) ui_notify("Low battery", warn);
	ui_refresh();
}

static int vib_device_count(void)
{
	int n = 0;
	EnterCriticalSection(&g_dev_cs);
	for (int i = 0; i < MAX_DEVICES; i++) n += g_dev[i].used && g_dev[i].nvib;
	LeaveCriticalSection(&g_dev_cs);
	return n;
}

/* ---------- websocket ---------- */

static BOOL ws_send(const char *s)
{
	BOOL ok = FALSE;
	EnterCriticalSection(&g_send_cs);
	if (g_ws) ok = WinHttpWebSocketSend(g_ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)s, (DWORD)strlen(s)) == NO_ERROR;
	LeaveCriticalSection(&g_send_cs);
	return ok;
}

static LONG next_id(void) { return InterlockedIncrement(&g_msg_id); }

static void send_simple(const char *type)
{
	char m[96];
	snprintf(m, sizeof m, "[{\"%s\":{\"Id\":%ld}}]", type, (long)next_id());
	ws_send(m);
}

static void handle_message(const char *buf, size_t len)
{
	const char *e = buf + len;
	if (g_cfg.log_rumble) log_line("<< %.*s", (int)(len > 400 ? 400 : len), buf);

	for (const char *o = next_obj(buf, e); o; ) {
		const char *oe = match(o, e);
		if (!oe) break;
		const char *v;
		if (find_key(o, oe, "ServerInfo")) {
			if ((v = find_key(o, oe, "MaxPingTime"))) g_max_ping = atoi(v);
			copy_str(g_server, sizeof g_server, find_key(o, oe, "ServerName"), oe);
			log_line("Connected to %s (max ping %d ms)", g_server, g_max_ping);
			InterlockedExchange(&g_connected, 1);
			send_simple("RequestDeviceList");
			send_simple("StartScanning");
			InterlockedExchange(&g_scanning, 1);
			ui_refresh();
		} else if (find_key(o, oe, "DeviceList")) {
			const char *arr = find_key(o, oe, "Devices");
			const char *arr_end = arr && *arr == '[' ? match(arr, oe) : NULL;
			for (const char *d = arr_end ? next_obj(arr + 1, arr_end) : NULL; d; ) {
				const char *de = match(d, arr_end);
				if (!de) break;
				add_device(d, de + 1);
				d = next_obj(de + 1, arr_end);
			}
		} else if ((v = find_key(o, oe, "DeviceAdded"))) {
			add_device(v, oe);
		} else if ((v = find_key(o, oe, "DeviceRemoved"))) {
			const char *ix = find_key(v, oe, "DeviceIndex");
			if (ix) remove_device(atoi(ix));
		} else if ((v = find_key(o, oe, "SensorReading"))) {
			const char *ix = find_key(v, oe, "DeviceIndex"), *data = find_key(v, oe, "Data");
			if (ix && data && *data == '[') on_battery(atoi(ix), atoi(data + 1));
		} else if (find_key(o, oe, "ScanningFinished")) {
			InterlockedExchange(&g_scanning, 0);
		} else if (find_key(o, oe, "Error")) {
			char msg[160];
			copy_str(msg, sizeof msg, find_key(o, oe, "ErrorMessage"), oe);
			log_line("Server error: %s", msg);
		}
		o = next_obj(oe + 1, e);
	}
}

static HINTERNET ws_open(HINTERNET *sess)
{
	HINTERNET conn = NULL, req = NULL, ws = NULL;
	*sess = WinHttpOpen(L"pcsx2-haptics/1.1", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
	if (!*sess) return NULL;
	/* receive timeout 0 = infinite: the socket idles while nothing rumbles */
	WinHttpSetTimeouts(*sess, 2000, 2000, 2000, 0);
	if ((conn = WinHttpConnect(*sess, g_cfg.host, (INTERNET_PORT)g_cfg.port, 0)) &&
	    (req = WinHttpOpenRequest(conn, L"GET", L"/", NULL, NULL, NULL, 0)) &&
	    WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) &&
	    WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
	    WinHttpReceiveResponse(req, NULL))
		ws = WinHttpWebSocketCompleteUpgrade(req, 0);
	if (req) WinHttpCloseHandle(req);
	if (conn) WinHttpCloseHandle(conn);
	if (!ws) { WinHttpCloseHandle(*sess); *sess = NULL; }
	return ws;
}

static void clear_devices(void)
{
	EnterCriticalSection(&g_dev_cs);
	ZeroMemory(g_dev, sizeof g_dev);
	LeaveCriticalSection(&g_dev_cs);
	ui_refresh();
}

/* ---------- intiface-engine.exe ---------- */

static int engine_running(void) { return g_engine && WaitForSingleObject(g_engine, 0) == WAIT_TIMEOUT; }

static int engine_spawn(void)
{
	WCHAR exe[MAX_PATH], cmd[1024], logp[MAX_PATH];
	if (g_cfg.server_path[0] && g_cfg.server_path[1] == L':')
		lstrcpyW(exe, g_cfg.server_path);
	else
		wsprintfW(exe, L"%s\\%s", g_dir, g_cfg.server_path);
	if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return 0;

	if (!g_job) {
		/* The engine lives in a job that dies with PCSX2, even if PCSX2 crashes. */
		g_job = CreateJobObjectW(NULL, NULL);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
	}

	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	wsprintfW(logp, L"%s\\intiface-engine.log", g_cfg.log_dir);
	HANDLE out = CreateFileW(logp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, NULL);

	wsprintfW(cmd, L"\"%s\" --websocket-port %d --use-bluetooth-le --server-name \"PCSX2 Haptics\" --log info %s",
	          exe, g_cfg.port, g_cfg.server_args);
	SetEnvironmentVariableW(L"NO_COLOR", L"1"); /* plain text log: we read it back for addresses */
	STARTUPINFOW si = {sizeof si};
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = si.hStdError = out;
	PROCESS_INFORMATION pi;
	BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, g_dir, &si, &pi);
	if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
	if (!ok) {
		log_line("Could not start %ls (error %lu)", exe, GetLastError());
		return 0;
	}
	AssignProcessToJobObject(g_job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	if (g_engine) CloseHandle(g_engine);
	g_engine = pi.hProcess;
	g_engine_pid = pi.dwProcessId;
	g_log_pos = 0;
	log_line("Started %ls", cmd);
	return 1;
}

/* Stops the engine like Ctrl+C would, so it can drop its Bluetooth connections cleanly.
 * A toy whose connection was cut abruptly can refuse to reconnect on the next start. */
static void engine_stop(DWORD wait_ms)
{
	if (!engine_running()) return;
	if (!GetConsoleWindow() && AttachConsole(g_engine_pid)) { /* skip if PCSX2 has its own log console */
		SetConsoleCtrlHandler(NULL, TRUE); /* don't Ctrl+C ourselves */
		GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
		FreeConsole();
	}
	if (WaitForSingleObject(g_engine, wait_ms) == WAIT_TIMEOUT) TerminateProcess(g_engine, 1);
	log_line("Stopped intiface-engine");
}

/* Counts new "failed to connect" lines in intiface-engine.log since the last call. */
static int engine_connect_errors(void)
{
	WCHAR p[MAX_PATH];
	wsprintfW(p, L"%s\\intiface-engine.log", g_cfg.log_dir);
	HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) return 0;
	LARGE_INTEGER pos = {.QuadPart = g_log_pos};
	SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
	static char buf[64 * 1024 + 1];
	DWORD got;
	int n = 0;
	while (ReadFile(f, buf, sizeof buf - 1, &got, NULL) && got) {
		buf[got] = 0;
		/* a line split across reads is counted on the next pass at worst */
		for (char *q = buf; (q = strstr(q, "errored while trying to connect")); q++) n++;
		g_log_pos += got;
	}
	CloseHandle(f);
	return n;
}

static DWORD WINAPI worker(LPVOID p)
{
	(void)p;
	size_t cap = 64 * 1024;
	char *buf = malloc(cap);
	int warned = 0;

	for (;;) {
		HINTERNET sess, ws = ws_open(&sess);
		if (!ws) {
			if (g_cfg.auto_start && !engine_running() && g_spawns < 3) {
				if (engine_spawn()) { InterlockedIncrement(&g_spawns); Sleep(1500); continue; }
			}
			if (!warned) {
				log_line("No Intiface server at %ls:%d; retrying", g_cfg.host, g_cfg.port);
				if (!engine_running())
					ui_notify("Intiface not available",
					          g_spawns >= 3 ? "intiface-engine.exe keeps closing. See intiface-engine.log."
					                      : "Put intiface-engine.exe in the PCSX2 folder or start Intiface Central.");
			}
			warned = 1;
			Sleep(engine_running() ? 1000 : 3000);
			continue;
		}
		warned = 0;
		EnterCriticalSection(&g_send_cs);
		g_ws = ws;
		LeaveCriticalSection(&g_send_cs);

		char hello[160];
		snprintf(hello, sizeof hello,
		         "[{\"RequestServerInfo\":{\"Id\":%ld,\"ClientName\":\"PCSX2 Haptics\",\"MessageVersion\":3}}]", (long)next_id());
		ws_send(hello);

		size_t len = 0;
		for (;;) {
			if (cap - len < 4096) {
				char *nb = realloc(buf, cap * 2);
				if (!nb) break;
				buf = nb;
				cap *= 2;
			}
			DWORD got = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
			if (WinHttpWebSocketReceive(ws, buf + len, (DWORD)(cap - len - 1), &got, &type) != NO_ERROR) break;
			if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
			len += got;
			if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
				buf[len] = 0;
				handle_message(buf, len);
				len = 0;
			}
		}

		if (InterlockedExchange(&g_connected, 0)) log_line("Disconnected from Intiface");
		EnterCriticalSection(&g_send_cs);
		g_ws = NULL;
		LeaveCriticalSection(&g_send_cs);
		WinHttpCloseHandle(ws);
		WinHttpCloseHandle(sess);
		clear_devices();
		Sleep(1000);
	}
	return 0;
}

/* ---------- rumble -> scalars ---------- */

static float shape(float x)
{
	if (x <= 0.001f) return 0;
	x *= g_cfg.scale;
	if (x < g_cfg.min_i) x = g_cfg.min_i;
	if (x > 1) x = 1;
	return (float)((int)(x * 100 + 0.5f)) / 100; /* 0.01 steps: fewer redundant commands */
}

/* ---------- rumble CSV ----------
 * One row whenever a value changes: the controller motors as the game sends them and
 * what each toy vibrator was told, in percent. A new file per PCSX2 session in rumble-logs\.
 * The header is written again if toys come or go. Called with g_dev_cs held. */

static FILE *g_csv;
static int g_csv_failed;
static char g_csv_header[1024], g_csv_last[512];
static const char *map_label[] = {"Large", "Small", "Both", "Off"};

static void csv_row(LONG rl, LONG rs)
{
	if (!g_cfg.rumble_log) {
		if (g_csv) fclose(g_csv);
		g_csv = NULL;
		return;
	}
	char head[1024], vals[512];
	int hn = snprintf(head, sizeof head, "timestamp,large_motor,small_motor");
	int vn = snprintf(vals, sizeof vals, "%d,%d", (int)(rl * 100 / 65535), (int)(rs * 100 / 65535));
	for (int d = 0; d < MAX_DEVICES; d++) {
		Device *dev = &g_dev[d];
		if (!dev->used || !dev->nvib) continue;
		for (int i = 0; i < dev->nvib; i++) {
			if (dev->nvib > 1)
				hn += snprintf(head + hn, sizeof head - hn, ",%s %d (%s)", dev_label(dev), i + 1, map_label[dev->map[i]]);
			else
				hn += snprintf(head + hn, sizeof head - hn, ",%s (%s)", dev_label(dev), map_label[dev->map[i]]);
			vn += snprintf(vals + vn, sizeof vals - vn, ",%d", dev->last[i] > 0 ? (int)(dev->last[i] * 100 + 0.5f) : 0);
		}
	}
	if (g_csv && !strcmp(vals, g_csv_last) && !strcmp(head, g_csv_header)) return;

	SYSTEMTIME t;
	GetLocalTime(&t);
	if (!g_csv) {
		if (g_csv_failed) return; /* until the folder changes */
		WCHAR path[MAX_PATH];
		CreateDirectoryW(g_cfg.csv_dir, NULL);
		wsprintfW(path, L"%s\\rumble-%04d-%02d-%02d_%02d-%02d-%02d.csv", g_cfg.csv_dir,
		          t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		if (!(g_csv = _wfopen(path, L"w"))) {
			g_csv_failed = 1;
			log_line("Can't write the rumble CSV to %ls", g_cfg.csv_dir);
			return;
		}
		g_csv_header[0] = 0;
		log_line("Recording rumble to %ls", path);
	}
	if (strcmp(head, g_csv_header)) {
		fprintf(g_csv, "%s\n", head);
		lstrcpynA(g_csv_header, head, sizeof g_csv_header);
	}
	/* ISO 8601 with the local UTC offset, e.g. 2026-11-19T01:20:13.482-05:00 */
	TIME_ZONE_INFORMATION tz;
	DWORD zone = GetTimeZoneInformation(&tz);
	LONG bias = tz.Bias + (zone == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : zone == TIME_ZONE_ID_STANDARD ? tz.StandardBias : 0);
	LONG off = -bias; /* minutes east of UTC */
	fprintf(g_csv, "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02ld:%02ld,%s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
	        t.wSecond, t.wMilliseconds, off < 0 ? '-' : '+', labs(off) / 60, labs(off) % 60, vals);
	fflush(g_csv);
	lstrcpynA(g_csv_last, vals, sizeof g_csv_last);
}

/* One sender step: turn the latest rumble into ScalarCmd messages for every device. */
static void send_tick(void)
{
	static DWORD last_ping, last_scan;
	static LONG prev_l = -1, prev_s = -1;
	static char msg[4096];

	if (!g_connected) { prev_l = prev_s = -1; return; }

	DWORD now = GetTickCount();
	if (g_max_ping > 0 && now - last_ping >= (DWORD)g_max_ping / 2) {
		send_simple("Ping");
		last_ping = now;
	}
	/* Bluetooth scans time out; keep looking while no toy is connected. */
	if (!g_scanning && now - last_scan > 5000 && !vib_device_count()) {
		send_simple("StartScanning");
		InterlockedExchange(&g_scanning, 1);
		last_scan = now;
	}
	/* Toy seen but every connection attempt fails (stale Bluetooth state): restart our engine. */
	static DWORD last_check;
	static int errors;
	if (g_engine && now - last_check > 2000) {
		last_check = now;
		if (vib_device_count()) {
			errors = 0;
			InterlockedExchange(&g_spawns, 0);
		} else if ((errors += engine_connect_errors()) >= 6) {
			errors = 0;
			if (g_restarts < 3) {
				InterlockedIncrement(&g_restarts);
				log_line("Toy found but it keeps failing to connect; restarting intiface-engine");
				InterlockedExchange(&g_spawns, 0);
				engine_stop(3000);
			} else if (g_restarts == 3) {
				InterlockedIncrement(&g_restarts);
				ui_notify("Toy won't connect", "It is visible but refuses the connection. Turn the toy and the PC's Bluetooth off and on again.");
			}
		}
	}

	/* battery: right after connecting, then once a minute */
	EnterCriticalSection(&g_dev_cs);
	for (int d = 0; d < MAX_DEVICES; d++) {
		Device *dev = &g_dev[d];
		if (!dev->used || dev->bat_sensor < 0 || (dev->bat_asked && now - dev->bat_asked < 60000)) continue;
		dev->bat_asked = now | 1;
		snprintf(msg, sizeof msg,
		         "[{\"SensorReadCmd\":{\"Id\":%ld,\"DeviceIndex\":%d,\"SensorIndex\":%d,\"SensorType\":\"Battery\"}}]",
		         (long)next_id(), dev->index, dev->bat_sensor);
		ws_send(msg);
	}
	LeaveCriticalSection(&g_dev_cs);

	LONG rl = g_large, rs = g_small;
	if (g_cfg.log_rumble && (rl != prev_l || rs != prev_s)) log_line("Rumble large=%ld small=%ld", (long)rl, (long)rs);
	prev_l = rl; prev_s = rs;

	float L = rl / 65535.0f * g_cfg.large_w, S = rs / 65535.0f * g_cfg.small_w;
	if (!g_cfg.enabled) L = S = 0;
	float both = g_cfg.mode == MODE_LARGE ? L
	           : g_cfg.mode == MODE_SMALL ? S
	           : g_cfg.mode == MODE_MIX ? L + S
	           : (L > S ? L : S);
	both = shape(both);
	float test = g_test / 1000.0f; /* tray "Test" pattern, ignores weights and scale */

	EnterCriticalSection(&g_dev_cs);
	for (int d = 0; d < MAX_DEVICES; d++) {
		Device *dev = &g_dev[d];
		if (!dev->used || !dev->nvib) continue;
		float val[MAX_VIB];
		int changed = 0;
		for (int i = 0; i < dev->nvib; i++) {
			int m = dev->map[i];
			val[i] = m == MAP_LARGE ? shape(L) : m == MAP_SMALL ? shape(S) : m == MAP_BOTH ? both : 0;
			if (test > val[i]) val[i] = test;
			if (val[i] != dev->last[i]) changed = 1;
		}
		if (!changed) continue;

		int n = snprintf(msg, sizeof msg, "[{\"ScalarCmd\":{\"Id\":%ld,\"DeviceIndex\":%d,\"Scalars\":[", (long)next_id(), dev->index);
		for (int i = 0; i < dev->nvib; i++) {
			int h = (int)(val[i] * 100 + 0.5f); /* hundredths; avoids locale-dependent %f */
			n += snprintf(msg + n, sizeof msg - n, "%s{\"Index\":%d,\"Scalar\":%d.%02d,\"ActuatorType\":\"Vibrate\"}",
			              i ? "," : "", dev->vib[i], h / 100, h % 100);
		}
		snprintf(msg + n, sizeof msg - n, "]}}]");
		if (ws_send(msg))
			for (int i = 0; i < dev->nvib; i++) dev->last[i] = val[i];
	}
	csv_row(rl, rs);
	LeaveCriticalSection(&g_dev_cs);
}

static DWORD WINAPI sender(LPVOID p)
{
	(void)p;
	for (;;) {
		Sleep(g_cfg.interval_ms);
		send_tick();
	}
	return 0;
}

static DWORD WINAPI test_pattern(LPVOID p)
{
	(void)p;
	for (int i = 0; i <= 20; i++) { InterlockedExchange(&g_test, i * 50); Sleep(60); }
	Sleep(300);
	for (int i = 0; i < 3; i++) {
		InterlockedExchange(&g_test, 0); Sleep(200);
		InterlockedExchange(&g_test, 700); Sleep(200);
	}
	InterlockedExchange(&g_test, 0);
	return 0;
}

/* ---------- public ---------- */

void bridge_start(HMODULE self)
{
	InitializeCriticalSection(&g_dev_cs);
	InitializeCriticalSection(&g_send_cs);
	InitializeCriticalSection(&g_log_cs);

	GetModuleFileNameW(self, g_dir, MAX_PATH);
	WCHAR *slash = wcsrchr(g_dir, L'\\');
	if (slash) *slash = 0;

	load_config();
	InterlockedExchange(&g_cfg_loaded, 1);

	/* tell the user right away if a log can't be written (one notification for both) */
	int log_bad = g_cfg.log && !dir_writable(g_cfg.log_dir), csv_bad = g_cfg.rumble_log && !dir_writable(g_cfg.csv_dir);
	if (log_bad || csv_bad)
		ui_notify(log_bad && csv_bad ? "Can't write the logs" : log_bad ? "Can't write the log" : "Can't write the rumble CSV",
		          "That folder is read-only. Open the settings and choose another one under Logs.");
	log_line("PCSX2 Haptics " PCSX2_HAPTICS_VERSION " started (enabled=%d, controller=%d, mode=%ls, forward=%d)",
	         g_cfg.enabled, g_cfg.controller, mode_names[g_cfg.mode], g_cfg.forward);

	CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
	CloseHandle(CreateThread(NULL, 0, sender, NULL, 0, NULL));
}

void bridge_log(const char *msg) { log_line("%s", msg); }

int bridge_set_log_dir(const WCHAR *dir)
{
	int ok = dir_writable(dir);
	EnterCriticalSection(&g_log_cs);
	if (g_log) fclose(g_log); /* the next line opens the log in the new folder */
	g_log = NULL;
	lstrcpynW(g_cfg.log_dir, dir, MAX_PATH);
	LeaveCriticalSection(&g_log_cs);
	WritePrivateProfileStringW(L"Paths", L"LogFolder", dir, g_ini);
	log_line("Log folder: %ls", dir);
	return ok;
}

int bridge_set_csv_dir(const WCHAR *dir)
{
	int ok = dir_writable(dir);
	EnterCriticalSection(&g_dev_cs);
	if (g_csv) fclose(g_csv); /* next rumble change starts a file in the new folder */
	g_csv = NULL;
	g_csv_failed = 0;
	lstrcpynW(g_cfg.csv_dir, dir, MAX_PATH);
	LeaveCriticalSection(&g_dev_cs);
	WritePrivateProfileStringW(L"Paths", L"RumbleFolder", dir, g_ini);
	return ok;
}

/* Called when the CSV option is switched on. */
void bridge_csv_reset(void)
{
	EnterCriticalSection(&g_dev_cs);
	g_csv_failed = 0;
	LeaveCriticalSection(&g_dev_cs);
}

void bridge_rumble(DWORD user_index, WORD large, WORD small)
{
	if (g_cfg.controller >= 0 && user_index != (DWORD)g_cfg.controller) return;
	InterlockedExchange(&g_large, large);
	InterlockedExchange(&g_small, small);
}

void bridge_scan(void)
{
	if (!g_connected) return;
	send_simple("StartScanning");
	InterlockedExchange(&g_scanning, 1);
}

void bridge_test(void)
{
	ui_notify("Testing vibration", "A ramp, then three pulses.");
	CloseHandle(CreateThread(NULL, 0, test_pattern, NULL, 0, NULL));
}

int bridge_connected(void) { return g_connected != 0; }

int bridge_owns_server(void) { return engine_running(); }

void bridge_restart_server(void)
{
	if (!engine_running()) return;
	InterlockedExchange(&g_spawns, 0);
	InterlockedExchange(&g_restarts, 0);
	engine_stop(3000); /* the worker notices the closed socket and starts a fresh engine */
}

void bridge_shutdown(void) { engine_stop(1500); }

/* Saves under this toy's own section, noting the model so the ini stays readable. */
static void save_toy_key(const Device *d, const WCHAR *key, const WCHAR *val)
{
	WCHAR sec[80], model[64];
	toy_section(d, sec);
	WritePrivateProfileStringW(sec, key, val, g_ini);
	if (d->addr[0]) {
		MultiByteToWideChar(CP_UTF8, 0, d->name, -1, model, 64);
		WritePrivateProfileStringW(sec, L"Model", model, g_ini);
	}
}

void bridge_set_map(int dev_index, int vib, int source)
{
	WCHAR key[16];
	wsprintfW(key, L"Vibrator%d", vib + 1);
	EnterCriticalSection(&g_dev_cs);
	for (int i = 0; i < MAX_DEVICES; i++) {
		Device *d = &g_dev[i];
		if (!d->used || d->index != dev_index || vib >= d->nvib) continue;
		d->map[vib] = source;
		d->last[vib] = -1; /* resend now */
		save_toy_key(d, key, map_names[source]);
	}
	LeaveCriticalSection(&g_dev_cs);
}

int bridge_set_alias(int dev_index, const WCHAR *alias)
{
	int ok = 0;
	EnterCriticalSection(&g_dev_cs);
	for (int i = 0; i < MAX_DEVICES; i++) {
		Device *d = &g_dev[i];
		if (!d->used || d->index != dev_index || !d->addr[0]) continue; /* aliases need the address */
		WideCharToMultiByte(CP_UTF8, 0, alias, -1, d->alias, sizeof d->alias, NULL, NULL);
		save_toy_key(d, L"Alias", alias[0] ? alias : NULL); /* empty name removes the alias */
		log_line("Alias for %s: %s", d->addr, d->alias[0] ? d->alias : "(none)");
		ok = 1;
	}
	LeaveCriticalSection(&g_dev_cs);
	ui_refresh();
	return ok;
}

void bridge_levels(Levels *lv)
{
	lv->large = g_large / 65535.0f;
	lv->small = g_small / 65535.0f;
	lv->ntoys = 0;
	EnterCriticalSection(&g_dev_cs);
	for (int i = 0; i < MAX_DEVICES && lv->ntoys < 4; i++) {
		Device *d = &g_dev[i];
		if (!d->used || !d->nvib) continue;
		int t = lv->ntoys++;
		lstrcpynA(lv->toy[t].name, dev_label(d), 64);
		lv->toy[t].nvib = d->nvib;
		for (int v = 0; v < d->nvib; v++) {
			lv->toy[t].map[v] = d->map[v];
			lv->toy[t].out[v] = g_connected && d->last[v] > 0 ? d->last[v] : 0;
		}
	}
	LeaveCriticalSection(&g_dev_cs);
}

int bridge_toys(ToyInfo *out, int max)
{
	int n = 0;
	EnterCriticalSection(&g_dev_cs);
	for (int i = 0; i < MAX_DEVICES && n < max; i++)
		if (g_dev[i].used && g_dev[i].nvib) {
			lstrcpynA(out[n].name, g_dev[i].name, 64);
			lstrcpynA(out[n].alias, g_dev[i].alias, 64);
			lstrcpynA(out[n].addr, g_dev[i].addr, 24);
			out[n].battery = g_dev[i].battery;
			out[n].index = g_dev[i].index;
			out[n].nvib = g_dev[i].nvib;
			memcpy(out[n].map, g_dev[i].map, sizeof out[n].map);
			n++;
		}
	LeaveCriticalSection(&g_dev_cs);
	return n;
}

void bridge_status(char *out, int len)
{
	ToyInfo t[4];
	int n = bridge_toys(t, 4);
	if (!g_connected)
		snprintf(out, len, "%s", engine_running() ? "Starting Intiface..." : "Intiface not connected");
	else if (!n)
		snprintf(out, len, "Searching for toys...");
	else if (t[0].battery >= 0)
		snprintf(out, len, "%s \xC2\xB7 %d%%%s", t[0].alias[0] ? t[0].alias : t[0].name, t[0].battery, n > 1 ? " (+more)" : "");
	else
		snprintf(out, len, "%s%s", t[0].alias[0] ? t[0].alias : t[0].name, n > 1 ? " (+more)" : "");
}
