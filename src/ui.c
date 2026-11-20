/*
 * Tray icon + settings window. Runs on its own thread inside PCSX2, so the icon
 * exists exactly as long as the emulator does.
 *   Left click  -> settings window (live sliders)
 *   Right click -> quick menu
 */
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <shobjidl.h>
#include <propsys.h>
#include <stdio.h>
#include "bridge.h"

#define WM_TRAY    (WM_APP + 1)
#define WM_NOTE    (WM_APP + 2)
#define WM_REFRESH (WM_APP + 3)
#define WM_REBUILD (WM_APP + 4)
#define WM_ENDRENAME (WM_APP + 5)

enum { IDM_STATUS = 100, IDM_ENABLED, IDM_FORWARD, IDM_SCAN, IDM_TEST, IDM_SETTINGS, IDM_LOG, IDM_RESTART, IDM_OVERLAY,
       IDM_SCALE = 120, IDM_MODE = 140 };
enum { IDC_ENABLED = 200, IDC_FORWARD, IDC_OVERLAY, IDC_CSV, IDC_NOTIFY, IDC_MODE, IDC_STATUS,
       IDC_TEST, IDC_SCAN, IDC_LOG, IDC_CLOSE, IDC_SLIDER = 220, IDC_VALUE = 240, IDC_HELPICON = 260 };

static HMODULE g_mod;
static HWND g_tray, g_win;
static NOTIFYICONDATAW g_nid;
static UINT g_taskbar_created;
static HFONT g_font, g_font_bold;
static HICON g_icon_small, g_icon_big, g_icon_help;
static HWND g_tip;
static int g_dpi = 96;
static CRITICAL_SECTION g_note_cs;
static WCHAR g_note_title[64], g_note_text[256];

static const int scale_steps[] = {25, 50, 75, 100, 125, 150};
static const WCHAR *mode_labels[] = {L"Strongest motor", L"Both motors added", L"Large motor only", L"Small motor only"};
static const struct { const WCHAR *label; float *val; int max; } sliders[] = {
	{L"Intensity", &g_cfg.scale, 200},
	{L"Minimum intensity while rumbling", &g_cfg.min_i, 100},
	{L"Large motor weight", &g_cfg.large_w, 100},
	{L"Small motor weight", &g_cfg.small_w, 100},
};
#define NSLIDERS ((int)(sizeof sliders / sizeof *sliders))

/* Text for the "?" icons. */
static const WCHAR *help_enabled =
	L"Turns the toy on or off. The game and the controller are not affected.";
static const WCHAR *help_forward =
	L"Whether the gamepad keeps vibrating as usual. Turn it off to feel the rumble only on the toy.";
static const WCHAR *help_map =
	L"Choose which PS2 controller motor drives each of the toy's vibrators:\n\n"
	L"\x2022 Large: heavy rumble (explosions, crashes, engines).\n"
	L"\x2022 Small: quick buzz (shots, hits).\n"
	L"\x2022 Both: both motors combined, using the Mode below.\n"
	L"\x2022 Off: this vibrator is not used.\n\n"
	L"Remembered for each toy model.";
static const WCHAR *help_notify =
	L"Shows a small popup in the bottom-right corner when a toy connects or disconnects, "
	L"or when something needs your attention.";
static const WCHAR *help_overlay =
	L"A small graph that floats over the game, like PCSX2's performance graph:\n\n"
	L"\x2022 Blue and green: the controller's large and small motors, as the game sends them.\n"
	L"\x2022 Pink (and other colors): each toy vibrator, labeled with the motor that drives it.\n\n"
	L"Drag it anywhere, drag its bottom-right corner to resize it, right-click it to hide it. "
	L"Like notifications, it can't show over exclusive fullscreen.";
static const WCHAR *help_csv =
	L"Saves every rumble change to a CSV file you can open with Excel or Google Sheets: the time, "
	L"the controller's large and small motors, and each toy vibrator (with the motor that drives it), in percent.\n\n"
	L"A new file each time PCSX2 starts, in the \"rumble-logs\" folder next to PCSX2.";
static const WCHAR *help_logs =
	L"Where the mod writes its files:\n\n"
	L"\x2022 General log: pcsx2-haptics.log and intiface-engine.log, useful when something doesn't work.\n"
	L"\x2022 Rumble CSV: the files from \"Record rumble to a CSV file\" (only shown when that option is on).\n\n"
	L"The folder must allow writing; folders inside Program Files usually don't. Click a path to open it.";
static const WCHAR *help_mode =
	L"A PS2 controller has two motors: a large one for heavy rumble (explosions, crashes, engines) "
	L"and a small one for a quick buzz (shots, hits). Most toys have one vibrator, so both motors "
	L"are combined into one level:\n\n"
	L"\x2022 Strongest: whichever motor is stronger at that moment. The most natural feel.\n"
	L"\x2022 Both added: both together, so it gets stronger when both rumble at once.\n"
	L"\x2022 Large only / Small only: ignore the other motor.";
static const WCHAR *help_sliders[] = {
	L"Overall strength, applied after the motors are combined.\n\n"
	L"100% = exactly what the game sends. 200% doubles everything: a 40% rumble becomes 80%, and "
	L"anything above 50% reaches the maximum, so medium and strong rumble start to feel the same.\n\n"
	L"Tip: use the weights to balance the two motors, and this slider for the overall level.",
	L"A floor for weak rumble. With 20%, any rumble makes the toy vibrate at 20% or more, so faint "
	L"effects are not lost.\n\n"
	L"When the game is not rumbling, the toy stays off.",
	L"How much the large motor (heavy rumble) counts before the motors are combined. "
	L"Lower it if explosions and crashes feel too strong.",
	L"How much the small motor (quick buzz) counts before the motors are combined.\n\n"
	L"On a PS2 controller this motor is only on or off, so at 100% every shot or hit is a full-strength "
	L"buzz. That is why it starts at 60%.",
};

/* Settings-window zoom; 1 everywhere else (see win_scale_enter). */
static float g_cur_scale = 1.0f;
static int g_scale_depth;
static HFONT g_saved_font, g_saved_bold; /* the normal fonts while the settings zoom is active */
static int S(int v) { return (int)(v * g_dpi / 96.0f * g_cur_scale + (v >= 0 ? 0.5f : -0.5f)); }

/* While the settings window is being dragged or resized, Windows runs a modal loop inside
 * its window procedure, so other windows (graph, toast, tray) get messages while the
 * settings zoom is active. Their procedures run in this "normal size" scope. */
typedef struct { float scale; HFONT font, bold; } ScaleScope;
static ScaleScope normal_size_enter(void)
{
	ScaleScope old = {g_cur_scale, g_font, g_font_bold};
	if (g_scale_depth) {
		g_cur_scale = 1.0f;
		g_font = g_saved_font;
		g_font_bold = g_saved_bold;
	}
	return old;
}
static void normal_size_leave(ScaleScope old)
{
	g_cur_scale = old.scale;
	g_font = old.font;
	g_font_bold = old.bold;
}

static void status_text(WCHAR *out, int len)
{
	char st[128];
	bridge_status(st, sizeof st);
	MultiByteToWideChar(CP_UTF8, 0, st, -1, out, len);
}

/* ---------- tray icon ---------- */

static void tray_update_tip(void)
{
	WCHAR st[100];
	status_text(st, 100);
	_snwprintf(g_nid.szTip, 128, L"PCSX2 Haptics " TEXT(PCSX2_HAPTICS_VERSION) L"\n%s", st);
	g_nid.uFlags = NIF_TIP;
	Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void tray_add(void)
{
	g_nid.cbSize = sizeof g_nid;
	g_nid.hWnd = g_tray;
	g_nid.uID = 1;
	g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	g_nid.uCallbackMessage = WM_TRAY;
	g_nid.hIcon = g_icon_small;
	lstrcpyW(g_nid.szTip, L"PCSX2 Haptics");
	Shell_NotifyIconW(NIM_ADD, &g_nid);
	tray_update_tip();
}

/* ---------- toast popup ----------
 * Our own small popup instead of a Windows notification: those are easy to lose to
 * Do Not Disturb, game mode or per-app notification settings. Topmost, never takes
 * focus from the game, fades out after a few seconds, click to dismiss. */

static HWND g_toast;
static HFONT g_font_icon; /* settings window only: Windows' icon font, for the pencil */
static int g_toast_alpha;
static WCHAR g_toast_title[64], g_toast_text[256];

static void open_settings(void);
static int g_toast_hover, g_toast_xhover;
/* drawing helpers, defined with the settings window */
typedef void GpGraphics, GpPen;
static GpGraphics *gfx(HDC dc);
static GpPen *pen(DWORD argb, float w);
static void fill_circle(GpGraphics *g, float cx, float cy, float r, DWORD argb);
int WINAPI GdipDrawLine(GpGraphics *, GpPen *, float, float, float, float);
int WINAPI GdipDeletePen(GpPen *);
int WINAPI GdipDeleteGraphics(GpGraphics *);

/* The close "x" in the top-right corner. */
static RECT toast_close_rect(HWND h)
{
	RECT r;
	GetClientRect(h, &r);
	RECT x = {r.right - S(30), S(6), r.right - S(6), S(30)};
	return x;
}

static LRESULT toast_proc_inner(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK toast_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	ScaleScope old = normal_size_enter(); /* never drawn with the settings zoom */
	LRESULT r = toast_proc_inner(h, msg, wp, lp);
	normal_size_leave(old);
	return r;
}

static LRESULT toast_proc_inner(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(h, &ps);
		RECT r;
		GetClientRect(h, &r);
		HBRUSH bg = CreateSolidBrush(RGB(32, 32, 36)), accent = CreateSolidBrush(RGB(214, 51, 132));
		FillRect(dc, &r, bg);
		RECT bar = {0, 0, S(4), r.bottom};
		FillRect(dc, &bar, accent);
		DeleteObject(bg);
		DeleteObject(accent);
		DrawIconEx(dc, S(16), (r.bottom - S(32)) / 2, g_icon_big, S(32), S(32), 0, NULL, DI_NORMAL);
		SetBkMode(dc, TRANSPARENT);
		RECT t = {S(60), S(12), r.right - S(36), S(32)};
		SelectObject(dc, g_font_bold);
		SetTextColor(dc, RGB(255, 255, 255));
		DrawTextW(dc, g_toast_title, -1, &t, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		RECT b = {S(60), S(34), r.right - S(36), r.bottom - S(8)};
		SelectObject(dc, g_font);
		SetTextColor(dc, RGB(200, 200, 205));
		DrawTextW(dc, g_toast_text, -1, &b, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);

		/* close button: a soft circle on hover, and the x */
		RECT xr = toast_close_rect(h);
		GpGraphics *g = gfx(dc);
		float cx = (xr.left + xr.right) / 2.0f, cy = (xr.top + xr.bottom) / 2.0f, a = (float)S(4);
		if (g_toast_xhover) fill_circle(g, cx, cy, (float)S(11), 0xFF3C3C44);
		GpPen *p = pen(g_toast_xhover ? 0xFFFFFFFF : 0xFFA0A0A8, (float)S(1) * 1.5f);
		GdipDrawLine(g, p, cx - a, cy - a, cx + a, cy + a);
		GdipDrawLine(g, p, cx - a, cy + a, cx + a, cy - a);
		GdipDeletePen(p);
		GdipDeleteGraphics(g);
		EndPaint(h, &ps);
		return 0;
	}
	case WM_TIMER:
		if (wp == 1) { /* shown long enough: start fading */
			KillTimer(h, 1);
			SetTimer(h, 2, 30, NULL);
		} else if ((g_toast_alpha -= 20) <= 0) {
			DestroyWindow(h);
		} else {
			SetLayeredWindowAttributes(h, 0, (BYTE)g_toast_alpha, LWA_ALPHA);
		}
		return 0;
	case WM_MOUSEMOVE: { /* hovering keeps it on screen and highlights the x */
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		RECT xr = toast_close_rect(h);
		int xh = PtInRect(&xr, pt);
		if (!g_toast_hover) {
			TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, h, 0};
			TrackMouseEvent(&tme);
			g_toast_hover = 1;
			KillTimer(h, 1);
			KillTimer(h, 2);
			g_toast_alpha = 245;
			SetLayeredWindowAttributes(h, 0, (BYTE)g_toast_alpha, LWA_ALPHA);
		}
		if (xh != g_toast_xhover) { g_toast_xhover = xh; InvalidateRect(h, NULL, FALSE); }
		return 0;
	}
	case WM_MOUSELEAVE:
		g_toast_hover = g_toast_xhover = 0;
		InvalidateRect(h, NULL, FALSE);
		SetTimer(h, 1, 2000, NULL); /* fade a little after the mouse leaves */
		return 0;
	case WM_LBUTTONUP: { /* x closes it; anywhere else opens the settings */
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		RECT xr = toast_close_rect(h);
		int close_only = PtInRect(&xr, pt);
		DestroyWindow(h);
		if (!close_only) open_settings();
		return 0;
	}
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_DESTROY:
		g_toast = NULL;
		g_toast_hover = g_toast_xhover = 0;
		return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

static void toast_show(const WCHAR *title, const WCHAR *text)
{
	lstrcpynW(g_toast_title, title, 64);
	lstrcpynW(g_toast_text, text, 256);
	int w = S(320), hgt = S(72);
	if (!g_toast) {
		RECT work;
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		g_toast = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
		                          L"PCSX2HapticsToast", L"", WS_POPUP,
		                          work.right - w - S(12), work.bottom - hgt - S(12), w, hgt, NULL, NULL, g_mod, NULL);
		/* rounded corners on Windows 11 (DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND) */
		HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
		typedef HRESULT(WINAPI * DwmSet_t)(HWND, DWORD, LPCVOID, DWORD);
		DwmSet_t set = dwm ? (DwmSet_t)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
		int round = 2;
		if (set) set(g_toast, 33, &round, sizeof round);
	}
	g_toast_alpha = 245;
	SetLayeredWindowAttributes(g_toast, 0, (BYTE)g_toast_alpha, LWA_ALPHA);
	KillTimer(g_toast, 2);
	SetTimer(g_toast, 1, 4000, NULL);
	InvalidateRect(g_toast, NULL, TRUE);
	SetWindowPos(g_toast, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void tray_show_note(void)
{
	WCHAR title[64], text[256];
	EnterCriticalSection(&g_note_cs);
	lstrcpynW(title, g_note_title, 64);
	lstrcpynW(text, g_note_text, 256);
	g_note_title[0] = 0;
	LeaveCriticalSection(&g_note_cs);
	if (title[0] && g_cfg.notify) toast_show(title, text);
}

/* ---------- drawing helpers (dark theme, anti-aliased via GDI+ flat API) ---------- */

typedef void GpGraphics, GpBrush, GpPen, GpPath;
typedef struct { UINT32 version; void *debug_cb; BOOL no_bg_thread, no_codecs; } GpStartupInput;
int WINAPI GdiplusStartup(ULONG_PTR *, const GpStartupInput *, void *);
int WINAPI GdipCreateFromHDC(HDC, GpGraphics **);
int WINAPI GdipDeleteGraphics(GpGraphics *);
int WINAPI GdipSetSmoothingMode(GpGraphics *, int);
int WINAPI GdipCreateSolidFill(DWORD, GpBrush **);
int WINAPI GdipDeleteBrush(GpBrush *);
int WINAPI GdipCreatePen1(DWORD, float, int, GpPen **);
int WINAPI GdipSetPenStartCap(GpPen *, int);
int WINAPI GdipSetPenEndCap(GpPen *, int);
int WINAPI GdipSetPenLineJoin(GpPen *, int);
int WINAPI GdipDeletePen(GpPen *);
int WINAPI GdipCreatePath(int, GpPath **);
int WINAPI GdipDeletePath(GpPath *);
int WINAPI GdipAddPathArc(GpPath *, float, float, float, float, float, float);
int WINAPI GdipClosePathFigure(GpPath *);
int WINAPI GdipAddPathRectangle(GpPath *, float, float, float, float);
int WINAPI GdipFillPath(GpGraphics *, GpBrush *, GpPath *);
int WINAPI GdipDrawPath(GpGraphics *, GpPen *, GpPath *);
int WINAPI GdipFillEllipse(GpGraphics *, GpBrush *, float, float, float, float);
int WINAPI GdipDrawLine(GpGraphics *, GpPen *, float, float, float, float);
int WINAPI GdipDrawArc(GpGraphics *, GpPen *, float, float, float, float, float, float);
int WINAPI GdipDrawEllipse(GpGraphics *, GpPen *, float, float, float, float);

/* Same palette as the toast popup. ARGB for GDI+, COLORREF for GDI. */
#define C_BG        0xFF202024
#define C_SURFACE   0xFF34343B
#define C_SURFACE_P 0xFF2A2A30
#define C_BORDER    0xFF5A5A64
#define C_ACCENT    0xFFD63384
#define C_ACCENT_P  0xFFB0256A
#define C_WHITE     0xFFFFFFFF
#define RGB_BG      RGB(32, 32, 36)
#define RGB_TEXT    RGB(235, 235, 240)
#define RGB_SUB     RGB(160, 160, 168)
#define RGB_DIM     RGB(110, 110, 118)
#define RGB_ACCENT  RGB(234, 92, 160)

static HBRUSH g_bg_brush;

static GpGraphics *gfx(HDC dc)
{
	GpGraphics *g;
	GdipCreateFromHDC(dc, &g);
	GdipSetSmoothingMode(g, 4 /* anti-alias */);
	return g;
}

static GpPath *round_path(float x, float y, float w, float h, float r)
{
	GpPath *p;
	float d = r * 2;
	GdipCreatePath(0, &p);
	if (r <= 0) { /* zero-size arcs draw nothing: plain rectangle (grid lines, square fills) */
		GdipAddPathRectangle(p, x, y, w, h);
		return p;
	}
	GdipAddPathArc(p, x, y, d, d, 180, 90);
	GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
	GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
	GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
	GdipClosePathFigure(p);
	return p;
}

static void fill_round(GpGraphics *g, float x, float y, float w, float h, float r, DWORD argb)
{
	GpPath *p = round_path(x, y, w, h, r);
	GpBrush *b;
	GdipCreateSolidFill(argb, &b);
	GdipFillPath(g, b, p);
	GdipDeleteBrush(b);
	GdipDeletePath(p);
}

static void stroke_round(GpGraphics *g, float x, float y, float w, float h, float r, DWORD argb, float width)
{
	GpPath *p = round_path(x, y, w, h, r);
	GpPen *pen;
	GdipCreatePen1(argb, width, 2 /* pixels */, &pen);
	GdipDrawPath(g, pen, p);
	GdipDeletePen(pen);
	GdipDeletePath(p);
}

static void fill_circle(GpGraphics *g, float cx, float cy, float r, DWORD argb)
{
	GpBrush *b;
	GdipCreateSolidFill(argb, &b);
	GdipFillEllipse(g, b, cx - r, cy - r, r * 2, r * 2);
	GdipDeleteBrush(b);
}

static void draw_text(HDC dc, const WCHAR *t, RECT *r, COLORREF c, UINT fmt)
{
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, c);
	SelectObject(dc, g_font);
	DrawTextW(dc, t, -1, r, fmt | DT_NOPREFIX);
}

/* ---------- settings window ---------- */

enum { IDC_MODEBTN = 280, IDC_CARD = 300, IDC_MAP = 400, IDC_MAPLABEL = 540,
       IDC_LOGDIR = 600, IDC_CSVDIR, IDC_LOGPATH, IDC_CSVPATH, IDC_LASTCSV, IDC_RENAME = 620, IDC_ALIASEDIT = 630, IDC_WARN = 640 };
static int g_log_ok = 1, g_csv_ok = 1; /* folder writability, shown under Logs */
#define MAX_TOYS 4
static const WCHAR *map_labels[] = {L"Large", L"Small", L"Both", L"Off"};
static ToyInfo g_toys[MAX_TOYS]; /* what the window was built for */
static int g_ntoys, g_help_n;
static const WCHAR *seg_labels[] = {L"Strongest", L"Both added", L"Large only", L"Small only"};

static int *check_value(int id)
{
	switch (id) {
	case IDC_ENABLED: return &g_cfg.enabled;
	case IDC_FORWARD: return &g_cfg.forward;
	case IDC_NOTIFY: return &g_cfg.notify;
	case IDC_OVERLAY: return &g_cfg.overlay;
	case IDC_CSV: return &g_cfg.rumble_log;
	}
	return NULL;
}

static HWND ctl(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
	HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
	                         parent, (HMENU)(INT_PTR)id, g_mod, NULL);
	SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
	return c;
}

static void set_value_label(int i)
{
	WCHAR buf[16];
	wsprintfW(buf, L"%d%%", (int)(*sliders[i].val * 100 + 0.5f));
	SetDlgItemTextW(g_win, IDC_VALUE + i, buf);
}

static void sync_window(void)
{
	if (!g_win) return;
	for (int i = 0; i < NSLIDERS; i++) {
		SendDlgItemMessageW(g_win, IDC_SLIDER + i, TBM_SETPOS, TRUE, (LPARAM)(*sliders[i].val * 100 + 0.5f));
		set_value_label(i);
	}
	InvalidateRect(g_win, NULL, TRUE); /* checkboxes and mode buttons draw from g_cfg */
}

static DWORD g_scan_until; /* the Scan button shows "Scanning..." until then */

static void build_controls(HWND h);
static void set_app_id(HWND h, const WCHAR *id);
static int last_csv(WCHAR *out);
static void ovl_apply(void);

static int same_toys(const ToyInfo *a, int na, const ToyInfo *b, int nb)
{
	if (na != nb) return 0;
	for (int i = 0; i < na; i++)
		if (a[i].index != b[i].index || a[i].nvib != b[i].nvib) return 0;
	return 1;
}

static void update_status(void)
{
	if (!g_win) return;
	ToyInfo now[MAX_TOYS];
	int n = bridge_toys(now, MAX_TOYS);
	if (!same_toys(now, n, g_toys, g_ntoys)) {
		if (GetCapture()) return; /* don't rebuild under a slider drag; next tick will */
		build_controls(g_win);
		return;
	}
	memcpy(g_toys, now, sizeof now); /* battery and mapping updates */
	for (int k = 0; k < (n ? n : 1); k++) InvalidateRect(GetDlgItem(g_win, IDC_CARD + k), NULL, FALSE);
	int scanning = (LONG)(GetTickCount() - g_scan_until) < 0;
	HWND scan = GetDlgItem(g_win, IDC_SCAN);
	WCHAR cur[32];
	const WCHAR *want = scanning ? L"Scanning..." : L"Scan for toys";
	GetWindowTextW(scan, cur, 32);
	if (lstrcmpW(cur, want)) { SetWindowTextW(scan, want); InvalidateRect(scan, NULL, FALSE); }
	EnableWindow(scan, bridge_connected() && !scanning);
	EnableWindow(GetDlgItem(g_win, IDC_TEST), n > 0);
	EnableWindow(GetDlgItem(g_win, IDC_LASTCSV), last_csv(NULL)); /* greyed until a rumble CSV exists */
}

/* Newest rumble-*.csv in the rumble folder; 0 if there is none. */
static int last_csv(WCHAR *out)
{
	WCHAR pat[MAX_PATH];
	WIN32_FIND_DATAW fd;
	FILETIME best = {0, 0};
	int found = 0;
	wsprintfW(pat, L"%s\\rumble-*.csv", g_cfg.csv_dir);
	HANDLE f = FindFirstFileW(pat, &fd);
	if (f == INVALID_HANDLE_VALUE) return 0;
	do {
		if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
			best = fd.ftLastWriteTime;
			if (out) wsprintfW(out, L"%s\\%s", g_cfg.csv_dir, fd.cFileName);
			found = 1;
		}
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return found;
}

static void open_last_csv(void)
{
	WCHAR p[MAX_PATH];
	if (last_csv(p)) ShellExecuteW(NULL, L"open", p, NULL, NULL, SW_SHOWNORMAL);
}

static void open_log(void)
{
	WCHAR p[MAX_PATH];
	wsprintfW(p, L"%s\\pcsx2-haptics.log", g_cfg.log_dir);
	ShellExecuteW(NULL, L"open", p, NULL, NULL, SW_SHOWNORMAL);
}

/* Width of a label in 96-DPI units, to put its "?" right after it. */
static int text_w(HWND h, const WCHAR *t)
{
	HDC dc = GetDC(h);
	HGDIOBJ old = SelectObject(dc, g_font);
	SIZE sz;
	GetTextExtentPoint32W(dc, t, lstrlenW(t), &sz);
	SelectObject(dc, old);
	ReleaseDC(h, dc);
	return (int)(sz.cx * 96.0f / (g_dpi * g_cur_scale) + 0.5f);
}

static void add_help(HWND parent, int x, int y, const WCHAR *text)
{
	HWND icon = ctl(parent, L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, x, y, 16, 16, IDC_HELPICON + g_help_n++ % 20);
	TOOLINFOW ti = {sizeof ti};
	ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
	ti.hwnd = parent;
	ti.uId = (UINT_PTR)icon;
	ti.lpszText = (LPWSTR)text;
	SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void draw_card(HDC dc, RECT r, const ToyInfo *toy);

static void draw_owner(DRAWITEMSTRUCT *di)
{
	HDC dc = di->hDC;
	RECT r = di->rcItem;
	int id = (int)di->CtlID;
	float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
	FillRect(dc, &r, g_bg_brush);

	if (id >= IDC_HELPICON && id < IDC_HELPICON + 20) {
		DrawIconEx(dc, r.left, r.top, g_icon_help, (int)w, (int)h, 0, NULL, DI_NORMAL);
		return;
	}
	if (id >= IDC_CARD && id < IDC_CARD + MAX_TOYS) { /* redrawn every second: double-buffer to avoid flicker */
		int k = id - IDC_CARD;
		HDC mem = CreateCompatibleDC(dc);
		HBITMAP bmp = CreateCompatibleBitmap(dc, r.right - r.left, r.bottom - r.top), old = SelectObject(mem, bmp);
		RECT local = {0, 0, r.right - r.left, r.bottom - r.top};
		FillRect(mem, &local, g_bg_brush);
		draw_card(mem, local, k < g_ntoys ? &g_toys[k] : NULL);
		BitBlt(dc, r.left, r.top, local.right, local.bottom, mem, 0, 0, SRCCOPY);
		SelectObject(mem, old);
		DeleteObject(bmp);
		DeleteDC(mem);
		return;
	}
	GpGraphics *g = gfx(dc);
	int pressed = di->itemState & ODS_SELECTED, focus = (di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT);
	int disabled = di->itemState & ODS_DISABLED;
	WCHAR text[64];
	GetWindowTextW(di->hwndItem, text, 64);

	int *chk = check_value(id);
	if (id >= IDC_RENAME && id < IDC_RENAME + MAX_TOYS) { /* pencil on a toy card */
		fill_round(g, 0, 0, w, h, 0, 0xFF2A2A30); /* card colour behind it */
		if (pressed || focus) fill_round(g, 1, 1, w - 2, h - 2, (float)S(6), 0xFF3C3C44);
		SetBkMode(dc, TRANSPARENT);
		SelectObject(dc, g_font_icon);
		SetTextColor(dc, disabled ? RGB_DIM : RGB_ACCENT);
		DrawTextW(dc, L"\xE70F", 1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX); /* Edit glyph */
		GdipDeleteGraphics(g);
		return;
	}
	if (chk) { /* checkbox: rounded box + label */
		float b = (float)S(18), y = (h - b) / 2;
		if (*chk) {
			fill_round(g, 1, y, b, b, (float)S(4), pressed ? C_ACCENT_P : C_ACCENT);
			GpPen *pen;
			GdipCreatePen1(C_WHITE, (float)S(2), 2, &pen);
			GdipSetPenStartCap(pen, 2);
			GdipSetPenEndCap(pen, 2);
			GdipSetPenLineJoin(pen, 2);
			float s = b / 18;
			GdipDrawLine(g, pen, 1 + 4.5f * s, y + 9.5f * s, 1 + 7.5f * s, y + 12.5f * s);
			GdipDrawLine(g, pen, 1 + 7.5f * s, y + 12.5f * s, 1 + 13.5f * s, y + 5.5f * s);
			GdipDeletePen(pen);
		} else {
			fill_round(g, 1, y, b, b, (float)S(4), pressed ? C_SURFACE_P : C_SURFACE);
			stroke_round(g, 1.5f, y + 0.5f, b - 1, b - 1, (float)S(4), focus ? C_ACCENT : C_BORDER, 1);
		}
		RECT t = {r.left + S(28), r.top, r.right, r.bottom};
		draw_text(dc, text, &t, focus ? RGB_ACCENT : RGB_TEXT, DT_SINGLELINE | DT_VCENTER);
	} else if ((id >= IDC_MODEBTN && id < IDC_MODEBTN + 4) || (id >= IDC_MAP && id < IDC_MAP + MAX_TOYS * 32)) {
		/* segmented selectors: Mode, and each vibrator's source */
		int rel = id - IDC_MAP;
		int on = id < IDC_MAP ? g_cfg.mode == id - IDC_MODEBTN : g_toys[rel / 32].map[rel / 4 % 8] == rel % 4;
		fill_round(g, 0.5f, 0.5f, w - 1, h - 1, (float)S(6), on ? C_ACCENT : pressed ? C_SURFACE_P : C_SURFACE);
		if (focus) stroke_round(g, 1, 1, w - 2, h - 2, (float)S(6), on ? C_WHITE : C_ACCENT, 1);
		draw_text(dc, text, &r, on ? RGB(255, 255, 255) : RGB_TEXT, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
	} else { /* push button; Test is the accented one */
		int primary = id == IDC_TEST && !disabled;
		DWORD fill = primary ? (pressed ? C_ACCENT_P : C_ACCENT) : (pressed ? C_SURFACE_P : C_SURFACE);
		fill_round(g, 0.5f, 0.5f, w - 1, h - 1, (float)S(6), fill);
		if (focus) stroke_round(g, 1, 1, w - 2, h - 2, (float)S(6), primary ? C_WHITE : C_ACCENT, 1);
		draw_text(dc, text, &r, disabled ? RGB_DIM : primary ? RGB(255, 255, 255) : RGB_TEXT,
		          DT_SINGLELINE | DT_VCENTER | DT_CENTER);
	}
	GdipDeleteGraphics(g);
}

static GpPen *pen(DWORD argb, float w)
{
	GpPen *p;
	GdipCreatePen1(argb, w, 2, &p);
	GdipSetPenStartCap(p, 2);
	GdipSetPenEndCap(p, 2);
	return p;
}

/* Toy card: avatar, name, connection state and battery, in the spirit of Lovense's apps. */
static void draw_card(HDC dc, RECT r, const ToyInfo *toy)
{
	int has = toy != NULL;
	float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
	GpGraphics *g = gfx(dc);
	fill_round(g, 0.5f, 0.5f, w - 1, h - 1, (float)S(10), 0xFF2A2A30);
	stroke_round(g, 0.5f, 0.5f, w - 1, h - 1, (float)S(10), has ? 0x66D63384 : 0xFF3A3A42, 1);

	/* avatar: ring + the same "vibration" glyph as the tray icon */
	DWORD ac = has ? C_ACCENT : 0xFF6E6E76;
	float cx = (float)S(31), cy = h / 2, rr = (float)S(17);
	GpPen *p = pen(ac, (float)S(2));
	GdipDrawEllipse(g, p, cx - rr, cy - rr, rr * 2, rr * 2);
	GdipDeletePen(p);
	fill_circle(g, cx, cy, (float)S(3), ac);
	p = pen(ac, (float)S(2));
	for (int k = 1; k <= 2; k++) {
		float a = (float)S(4 + 4 * k);
		GdipDrawArc(g, p, cx - a, cy - a, a * 2, a * 2, 140, 80);
		GdipDrawArc(g, p, cx - a, cy - a, a * 2, a * 2, -40, 80);
	}
	GdipDeletePen(p);

	WCHAR name[96], line2[64];
	int x = r.left + S(60);
	if (has) {
		MultiByteToWideChar(CP_UTF8, 0, toy->name, -1, name, 64);
		lstrcpyW(line2, L"Connected");
	} else {
		lstrcpyW(name, L"No toy connected");
		status_text(line2, 64);
	}
	RECT t1 = {x, r.top + S(11), r.right - S(has ? 48 : 10), r.top + S(31)}; /* right: room for the pencil */
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_font_bold);
	SetTextColor(dc, has ? RGB(255, 255, 255) : RGB_SUB);
	DrawTextW(dc, name, -1, &t1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	if (has && toy->alias[0]) { /* like Lovense's apps: "Lovense Hush (Alex's Hush)" */
		SIZE sz;
		GetTextExtentPoint32W(dc, name, lstrlenW(name), &sz);
		WCHAR al[80], wal[64];
		MultiByteToWideChar(CP_UTF8, 0, toy->alias, -1, wal, 64);
		_snwprintf(al, 80, L" (%s)", wal);
		RECT ta = {t1.left + sz.cx, t1.top, t1.right, t1.bottom};
		SelectObject(dc, g_font);
		SetTextColor(dc, RGB_SUB);
		DrawTextW(dc, al, -1, &ta, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	}
	RECT t2 = {x, r.top + S(32), r.right - S(10), r.top + S(52)};
	SelectObject(dc, g_font);
	SetTextColor(dc, RGB_SUB);
	DrawTextW(dc, line2, -1, &t2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	if (has && toy->battery >= 0) {
		/* battery glyph + percentage, colored by level */
		SIZE sz;
		GetTextExtentPoint32W(dc, line2, lstrlenW(line2), &sz);
		float bx = (float)(x + sz.cx + S(12)), by = (float)(t2.top + S(5)), bw = (float)S(22), bh = (float)S(11);
		int lv = toy->battery > 100 ? 100 : toy->battery;
		DWORD col = lv > 50 ? 0xFF34C759 : lv > 20 ? 0xFFFFB020 : 0xFFFF453A;
		stroke_round(g, bx, by, bw, bh, (float)S(3), col, 1.2f);
		fill_round(g, bx + bw + 1, by + bh * 0.3f, (float)S(2), bh * 0.4f, 1, col);
		float fw = (bw - S(4)) * lv / 100.0f;
		if (fw > 1) fill_round(g, bx + S(2), by + S(2), fw, bh - S(4), (float)S(1), col);
		WCHAR pct[8];
		wsprintfW(pct, L"%d%%", lv);
		RECT t3 = {(int)(bx + bw + S(8)), t2.top, r.right - S(10), t2.bottom};
		SetTextColor(dc, RGB(col >> 16 & 0xFF, col >> 8 & 0xFF, col & 0xFF));
		DrawTextW(dc, pct, -1, &t3, DT_SINGLELINE | DT_NOPREFIX);
	}
	GdipDeleteGraphics(g);
}

/* Trackbar: thin track, pink fill up to the thumb, round pink thumb. */
static LRESULT draw_slider(NMCUSTOMDRAW *cd)
{
	if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
	if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
	if (cd->dwItemSpec == TBCD_CHANNEL) {
		RECT ch, th, cl;
		SendMessageW(cd->hdr.hwndFrom, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
		SendMessageW(cd->hdr.hwndFrom, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
		GetClientRect(cd->hdr.hwndFrom, &cl);
		FillRect(cd->hdc, &cl, g_bg_brush);
		GpGraphics *g = gfx(cd->hdc);
		float t = (float)S(4), cy = (th.top + th.bottom) / 2.0f, x0 = ch.left + (float)S(4), x1 = ch.right - (float)S(4);
		float tx = (th.left + th.right) / 2.0f;
		fill_round(g, x0, cy - t / 2, x1 - x0, t, t / 2, C_SURFACE);
		if (tx > x0) fill_round(g, x0, cy - t / 2, tx - x0, t, t / 2, C_ACCENT);
		GdipDeleteGraphics(g);
	} else if (cd->dwItemSpec == TBCD_THUMB) {
		GpGraphics *g = gfx(cd->hdc);
		float cx = (cd->rc.left + cd->rc.right) / 2.0f, cy = (cd->rc.top + cd->rc.bottom) / 2.0f;
		int active = (cd->uItemState & CDIS_SELECTED) || GetFocus() == cd->hdr.hwndFrom;
		fill_circle(g, cx, cy, (float)S(active ? 9 : 8), C_ACCENT);
		fill_circle(g, cx, cy, (float)S(3), C_BG);
		GdipDeleteGraphics(g);
	}
	return CDRF_SKIPDEFAULT;
}

static void dark_title_bar(HWND h)
{
	HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
	typedef HRESULT(WINAPI * DwmSet_t)(HWND, DWORD, LPCVOID, DWORD);
	DwmSet_t set = dwm ? (DwmSet_t)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
	if (!set) return;
	BOOL dark = TRUE;
	COLORREF caption = RGB_BG, border = RGB(214, 51, 132), text = RGB_TEXT;
	set(h, 20, &dark, sizeof dark);       /* DWMWA_USE_IMMERSIVE_DARK_MODE */
	set(h, 35, &caption, sizeof caption); /* DWMWA_CAPTION_COLOR (Windows 11) */
	set(h, 34, &border, sizeof border);   /* DWMWA_BORDER_COLOR */
	set(h, 36, &text, sizeof text);       /* DWMWA_TEXT_COLOR */
}

/* The settings window can be zoomed by resizing it. Everything it draws goes through
 * S() and g_font/g_font_bold, so while handling its messages we swap in its scale and
 * its own fonts; the toast and the graph keep the normal size. */
static HFONT g_font_win, g_font_bold_win;
static int g_building, g_content_h = 600;

static void make_win_fonts(void)
{
	if (g_font_win) DeleteObject(g_font_win);
	if (g_font_bold_win) DeleteObject(g_font_bold_win);
	float px = g_dpi / 72.0f * g_cfg.ui_scale;
	g_font_win = CreateFontW(-(int)(9 * px + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
	                         CLEARTYPE_QUALITY, 0, L"Segoe UI");
	g_font_bold_win = CreateFontW(-(int)(10 * px + 0.5f), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
	                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
	if (g_font_icon) DeleteObject(g_font_icon);
	/* Windows' icon font: "Segoe Fluent Icons" on 11, "Segoe MDL2 Assets" on 10; same code points */
	g_font_icon = CreateFontW(-(int)(10 * px + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
	                          CLEARTYPE_QUALITY, 0, L"Segoe Fluent Icons");
	LOGFONTW lf;
	HDC dc = GetDC(NULL);
	HGDIOBJ old = SelectObject(dc, g_font_icon);
	WCHAR face[LF_FACESIZE];
	GetTextFaceW(dc, LF_FACESIZE, face);
	SelectObject(dc, old);
	ReleaseDC(NULL, dc);
	if (lstrcmpiW(face, L"Segoe Fluent Icons")) { /* not installed: fall back */
		GetObjectW(g_font_icon, sizeof lf, &lf);
		lstrcpyW(lf.lfFaceName, L"Segoe MDL2 Assets");
		DeleteObject(g_font_icon);
		g_font_icon = CreateFontIndirectW(&lf);
	}
}

static void win_scale_enter(void)
{
	if (g_scale_depth++) return;
	if (!g_font_win) make_win_fonts();
	g_saved_font = g_font;
	g_saved_bold = g_font_bold;
	g_font = g_font_win;
	g_font_bold = g_font_bold_win;
	g_cur_scale = g_cfg.ui_scale;
}

static void win_scale_leave(void)
{
	if (--g_scale_depth) return;
	g_font = g_saved_font;
	g_font_bold = g_saved_bold;
	g_cur_scale = 1.0f;
}

/* After the zoom changed while inside win_scale_enter. */
static void win_scale_refresh(void)
{
	make_win_fonts();
	g_font = g_font_win;
	g_font_bold = g_font_bold_win;
	g_cur_scale = g_cfg.ui_scale;
}

/* Which settings matter for the connected toys' vibrator mapping (all, when no toy is known). */
enum { NEED_MODE = 1, NEED_LARGE = 2, NEED_SMALL = 4 };
static int g_built_needs;

static int needed_settings(void)
{
	if (!g_ntoys) return NEED_MODE | NEED_LARGE | NEED_SMALL;
	int both = 0, large = 0, small = 0;
	for (int k = 0; k < g_ntoys; k++)
		for (int i = 0; i < g_toys[k].nvib; i++) {
			int m = g_toys[k].map[i];
			both |= m == MAP_BOTH;
			large |= m == MAP_LARGE;
			small |= m == MAP_SMALL;
		}
	/* "Both" combines the motors according to Mode, which may itself ignore one of them */
	large |= both && g_cfg.mode != MODE_SMALL;
	small |= both && g_cfg.mode != MODE_LARGE;
	return (both ? NEED_MODE : 0) | (large ? NEED_LARGE : 0) | (small ? NEED_SMALL : 0);
}

/* Settings that leave the toy stuck at one level: the minimum intensity is at or above the
 * strongest output the used motors can ever produce. Returns 1 and fills `out` if so. */
static int g_built_warn;
static int settings_warning(WCHAR *out, int len)
{
	int needs = needed_settings();
	float floor = g_cfg.min_i, best = 0;
	float lw = g_cfg.large_w * g_cfg.scale, sw = g_cfg.small_w * g_cfg.scale;
	int large = needs & NEED_LARGE, small = needs & NEED_SMALL;
	if (large) best = lw;
	if (small && sw > best) best = sw;
	if (large && small && g_cfg.mode == MODE_MIX) best = lw + sw; /* "Both added" can reach their sum */
	if (best > 1) best = 1;
	if (floor <= 0 || (!large && !small) || best > floor + 0.005f) return 0;
	if (!out) return 1;
	const WCHAR *what = large && small ? L"the motors never get" : large ? L"the large motor never gets" : L"the small motor never gets";
	_snwprintf(out, len,
	           L"\x26A0 With these settings the toy can only be off or at %d%%: %s above %d%%, "
	           L"which is not more than the minimum intensity.",
	           (int)(floor * 100 + 0.5f), what, (int)(best * 100 + 0.5f));
	return 1;
}

/* Creates every control for the current set of toys; called again when toys come or go,
 * and when the window is resized (resize_window = 0 then: the user is sizing it). */
static void build_controls_scaled(HWND h, int resize_window);
static void build_controls_ex(HWND h, int resize_window)
{
	win_scale_enter();
	g_building = 1;
	SendMessageW(h, WM_SETREDRAW, FALSE, 0);
	build_controls_scaled(h, resize_window);
	SendMessageW(h, WM_SETREDRAW, TRUE, 0);
	RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
	g_building = 0;
	win_scale_leave();
}

static void build_controls(HWND h) { build_controls_ex(h, 1); }

static void path_label(WCHAR *out, const WCHAR *dir, int ok)
{
	wsprintfW(out, ok ? L"%s" : L"Read-only: %s", dir);
}

/* ---------- renaming a toy (inline edit over its card) ---------- */

static HWND g_alias_edit;
static int g_alias_k;
static HBRUSH g_edit_brush;

static LRESULT CALLBACK alias_edit_proc(HWND e, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
	(void)id; (void)ref;
	if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS; /* keep Enter/Esc away from IsDialogMessage */
	if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
		PostMessageW(GetParent(e), WM_ENDRENAME, wp == VK_RETURN, 0);
		return 0;
	}
	if (msg == WM_CHAR && (wp == '\r' || wp == 27)) return 0; /* no beep */
	if (msg == WM_KILLFOCUS) PostMessageW(GetParent(e), WM_ENDRENAME, 1, 0); /* clicking away saves */
	return DefSubclassProc(e, msg, wp, lp);
}

static void start_rename(HWND h, int k)
{
	if (g_alias_edit || k >= g_ntoys || !g_toys[k].addr[0]) return;
	RECT rc;
	GetWindowRect(GetDlgItem(h, IDC_CARD + k), &rc);
	MapWindowPoints(NULL, h, (POINT *)&rc, 2);
	WCHAR cur[64];
	MultiByteToWideChar(CP_UTF8, 0, g_toys[k].alias, -1, cur, 64);
	g_alias_k = k;
	g_alias_edit = CreateWindowExW(0, L"EDIT", cur, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
	                               rc.left + S(60), rc.top + S(10), rc.right - rc.left - S(60) - S(50), S(22),
	                               h, (HMENU)(INT_PTR)IDC_ALIASEDIT, g_mod, NULL);
	SendMessageW(g_alias_edit, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
	SendMessageW(g_alias_edit, EM_SETLIMITTEXT, 40, 0);
	SendMessageW(g_alias_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Name this toy");
	SendMessageW(g_alias_edit, EM_SETSEL, 0, -1);
	SetWindowSubclass(g_alias_edit, alias_edit_proc, 1, 0);
	SetWindowPos(g_alias_edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	SetFocus(g_alias_edit);
}

static void end_rename(HWND h, int save)
{
	HWND e = g_alias_edit;
	if (!e) return;
	g_alias_edit = NULL; /* first: destroying it sends WM_KILLFOCUS */
	if (save) {
		WCHAR t[64];
		GetWindowTextW(e, t, 64);
		WCHAR *a = t, *z = t + lstrlenW(t);
		while (*a == L' ') a++;
		while (z > a && z[-1] == L' ') *--z = 0;
		int k = g_alias_k;
		if (k < g_ntoys && bridge_set_alias(g_toys[k].index, a))
			WideCharToMultiByte(CP_UTF8, 0, a, -1, g_toys[k].alias, sizeof g_toys[k].alias, NULL, NULL);
	}
	DestroyWindow(e);
	InvalidateRect(GetDlgItem(h, IDC_CARD + g_alias_k), NULL, FALSE);
}

/* Windows' standard folder picker. */
static int pick_folder(HWND owner, const WCHAR *start, WCHAR *out)
{
	IFileOpenDialog *dlg;
	int ok = 0;
	if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
		return 0;
	dlg->lpVtbl->SetOptions(dlg, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
	IShellItem *cur;
	if (SUCCEEDED(SHCreateItemFromParsingName(start, NULL, &IID_IShellItem, (void **)&cur))) {
		dlg->lpVtbl->SetFolder(dlg, cur);
		cur->lpVtbl->Release(cur);
	}
	IShellItem *item;
	if (SUCCEEDED(dlg->lpVtbl->Show(dlg, owner)) && SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item))) {
		WCHAR *path;
		if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
			lstrcpynW(out, path, MAX_PATH);
			CoTaskMemFree(path);
			ok = 1;
		}
		item->lpVtbl->Release(item);
	}
	dlg->lpVtbl->Release(dlg);
	return ok;
}

static void change_folder(HWND h, int csv)
{
	WCHAR dir[MAX_PATH];
	if (!pick_folder(h, csv ? g_cfg.csv_dir : g_cfg.log_dir, dir)) return;
	int ok = csv ? bridge_set_csv_dir(dir) : bridge_set_log_dir(dir);
	if (csv) g_csv_ok = ok; else g_log_ok = ok;
	WCHAR shown[MAX_PATH + 32];
	path_label(shown, dir, ok);
	SetDlgItemTextW(h, csv ? IDC_CSVPATH : IDC_LOGPATH, shown);
	InvalidateRect(GetDlgItem(h, csv ? IDC_CSVPATH : IDC_LOGPATH), NULL, TRUE);
	if (!ok) ui_notify("Can't write there", "That folder is read-only. Choose another one.");
}

static void build_controls_scaled(HWND h, int resize_window)
{
	HWND c;
	g_alias_edit = NULL;
	while ((c = GetWindow(h, GW_CHILD))) DestroyWindow(c);
	if (g_tip) DestroyWindow(g_tip);
	g_help_n = 0;
	g_ntoys = bridge_toys(g_toys, MAX_TOYS);

	g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
	                        0, 0, 0, 0, h, NULL, g_mod, NULL);
	SetWindowTheme(g_tip, L" ", L" "); /* unthemed, so our colors apply */
	SendMessageW(g_tip, TTM_SETTIPBKCOLOR, RGB(45, 45, 52), 0);
	SendMessageW(g_tip, TTM_SETTIPTEXTCOLOR, RGB_TEXT, 0);
	RECT m = {S(6), S(4), S(6), S(4)};
	SendMessageW(g_tip, TTM_SETMARGIN, 0, (LPARAM)&m);
	SendMessageW(g_tip, WM_SETFONT, (WPARAM)g_font, FALSE);
	SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, S(340));
	SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_INITIAL, 150);
	SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 60000);

	int y = 14;
	/* one card per toy, each followed by its vibrator -> motor rows */
	for (int k = 0; k < (g_ntoys ? g_ntoys : 1); k++) {
		ctl(h, L"STATIC", L"", SS_OWNERDRAW | WS_CLIPSIBLINGS, 14, y, 332, 62, IDC_CARD + k);
		if (k < g_ntoys) { /* pencil: rename this toy */
			HWND pen_btn = ctl(h, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 14 + 332 - 42, y + 8, 30, 28, IDC_RENAME + k);
			SetWindowPos(pen_btn, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			EnableWindow(pen_btn, g_toys[k].addr[0] != 0);
			TOOLINFOW ti = {sizeof ti};
			ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
			ti.hwnd = h;
			ti.uId = (UINT_PTR)pen_btn;
			ti.lpszText = g_toys[k].addr[0] ? L"Give this toy a name"
			                                : L"Naming toys needs the built-in server (it isn't available with Intiface Central)";
			SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
		}
		y += 70;
		int nv = k < g_ntoys ? g_toys[k].nvib : 0;
		for (int i = 0; i < nv && i < 8; i++, y += 34) {
			WCHAR label[24];
			wsprintfW(label, L"Vibrator %d", i + 1);
			ctl(h, L"STATIC", label, 0, 14, y + 5, text_w(h, label) + 2, 20, IDC_MAPLABEL + k * 8 + i);
			if (i == 0) add_help(h, 14 + text_w(h, label) + 6, y + 7, help_map);
			for (int j = 0; j < 4; j++)
				ctl(h, L"BUTTON", map_labels[j], BS_OWNERDRAW | WS_TABSTOP, 126 + j * 55, y, 51, 28,
				    IDC_MAP + (k * 8 + i) * 4 + j);
		}
		if (nv) y += 4;
	}
	ctl(h, L"BUTTON", L"Test connected toy", BS_OWNERDRAW | WS_TABSTOP, 14, y, 164, 32, IDC_TEST);
	ctl(h, L"BUTTON", L"Scan for toys", BS_OWNERDRAW | WS_TABSTOP, 182, y, 164, 32, IDC_SCAN);
	y += 46;
	static const struct { const WCHAR *label; int id; const WCHAR **help; } checks[] = {
		{L"Toy vibration", IDC_ENABLED, &help_enabled},
		{L"Controller vibration", IDC_FORWARD, &help_forward},
		{L"Notify when a toy connects", IDC_NOTIFY, &help_notify},
		{L"Show rumble graph", IDC_OVERLAY, &help_overlay},
		{L"Record rumble to a CSV file", IDC_CSV, &help_csv},
	};
	for (int i = 0; i < 5; i++, y += 28) {
		int w = 30 + text_w(h, checks[i].label);
		ctl(h, L"BUTTON", checks[i].label, BS_OWNERDRAW | WS_TABSTOP, 14, y, w, 24, checks[i].id);
		add_help(h, 14 + w + 4, y + 4, *checks[i].help);
	}
	y += 8;
	/* only what affects the current mapping: e.g. a vibrator on "Small" hides Mode and the large-motor weight */
	int needs = g_built_needs = needed_settings();
	if (needs & NEED_MODE) {
		ctl(h, L"STATIC", L"Mode", 0, 14, y, text_w(h, L"Mode") + 2, 20, -1);
		add_help(h, 14 + text_w(h, L"Mode") + 6, y + 2, help_mode);
		y += 24;
		for (int i = 0; i < 4; i++)
			ctl(h, L"BUTTON", seg_labels[i], BS_OWNERDRAW | WS_TABSTOP, 14 + i * 84, y, 80, 30, IDC_MODEBTN + i);
		y += 44;
	} else {
		y += 6;
	}
	for (int i = 0; i < NSLIDERS; i++) {
		if ((i == 2 && !(needs & NEED_LARGE)) || (i == 3 && !(needs & NEED_SMALL))) continue;
		ctl(h, L"STATIC", sliders[i].label, 0, 14, y, text_w(h, sliders[i].label) + 2, 20, -1);
		add_help(h, 14 + text_w(h, sliders[i].label) + 6, y + 2, help_sliders[i]);
		ctl(h, L"STATIC", L"", SS_RIGHT, 286, y, 60, 20, IDC_VALUE + i);
		HWND tb = ctl(h, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH | WS_TABSTOP,
		              4, y + 20, 352, 28, IDC_SLIDER + i);
		SendMessageW(tb, TBM_SETTHUMBLENGTH, S(22), 0);
		SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(0, sliders[i].max));
		SendMessageW(tb, TBM_SETPAGESIZE, 0, 10);
		y += 54;
	}
	/* warning about settings that flatten the vibration (amber, only when it applies) */
	WCHAR warn[256];
	g_built_warn = settings_warning(warn, 256);
	if (g_built_warn) {
		RECT calc = {0, 0, S(332), 0};
		HDC dc = GetDC(h);
		HGDIOBJ old = SelectObject(dc, g_font);
		DrawTextW(dc, warn, -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
		SelectObject(dc, old);
		ReleaseDC(h, dc);
		int hh = (int)(calc.bottom * 96.0f / (g_dpi * g_cur_scale)) + 4;
		ctl(h, L"STATIC", warn, SS_LEFT | SS_NOPREFIX, 14, y - 6, 332, hh, IDC_WARN);
		y += hh + 6;
	}

	/* Logs: where each file goes, with a button to change it */
	g_log_ok = dir_writable(g_cfg.log_dir);
	g_csv_ok = !g_cfg.rumble_log || dir_writable(g_cfg.csv_dir);
	y += 4;
	ctl(h, L"STATIC", L"Logs", 0, 14, y, text_w(h, L"Logs") + 2, 20, -1);
	add_help(h, 14 + text_w(h, L"Logs") + 6, y + 2, help_logs);
	y += 26;
	for (int csv = 0; csv <= (g_cfg.rumble_log ? 1 : 0); csv++) {
		ctl(h, L"STATIC", csv ? L"Rumble CSV" : L"General log", 0, 14, y + 4, 120, 20, -1);
		ctl(h, L"BUTTON", L"Change folder", BS_OWNERDRAW | WS_TABSTOP, 236, y, 110, 28, csv ? IDC_CSVDIR : IDC_LOGDIR);
		WCHAR shown[MAX_PATH + 32];
		path_label(shown, csv ? g_cfg.csv_dir : g_cfg.log_dir, csv ? g_csv_ok : g_log_ok);
		ctl(h, L"STATIC", shown, SS_PATHELLIPSIS | SS_NOTIFY, 14, y + 30, 332, 20, csv ? IDC_CSVPATH : IDC_LOGPATH);
		y += 58;
	}
	y += 4;
	ctl(h, L"BUTTON", L"Open general log", BS_OWNERDRAW | WS_TABSTOP, 14, y, 164, 32, IDC_LOG);
	ctl(h, L"BUTTON", L"Open last rumble log", BS_OWNERDRAW | WS_TABSTOP, 182, y, 164, 32, IDC_LASTCSV);
	y += 40;
	ctl(h, L"BUTTON", L"Close", BS_OWNERDRAW | WS_TABSTOP, 14, y, 332, 32, IDC_CLOSE);
	y += 32 + 16; /* bottom margin, a bit more than the top one */

	g_content_h = y;

	if (resize_window) { /* size the window to its content, keeping the bottom-right corner where it was */
		RECT wr, cr = {0, 0, S(360), S(y)};
		GetWindowRect(h, &wr);
		AdjustWindowRectEx(&cr, GetWindowLongW(h, GWL_STYLE), FALSE, GetWindowLongW(h, GWL_EXSTYLE));
		int w = cr.right - cr.left, hgt = cr.bottom - cr.top;
		SetWindowPos(h, NULL, wr.right - w, wr.bottom - hgt, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	sync_window();
	update_status();
}

static LRESULT settings_proc_inner(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		g_win = h;
		dark_title_bar(h);
		build_controls(h);
		SetTimer(h, 1, 1000, NULL);
		return 0;
	case WM_HSCROLL: {
		int i = GetDlgCtrlID((HWND)lp) - IDC_SLIDER;
		if (i < 0 || i >= NSLIDERS) break;
		*sliders[i].val = (float)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0) / 100.0f; /* applies immediately */
		set_value_label(i);
		InvalidateRect((HWND)lp, NULL, TRUE); /* our round thumb is wider than the stock one */
		{
			WCHAR warn[256];
			int now = settings_warning(warn, 256);
			if (now && g_built_warn) SetDlgItemTextW(h, IDC_WARN, warn); /* numbers follow the slider */
			if (LOWORD(wp) == TB_ENDTRACK && now != g_built_warn) PostMessageW(h, WM_REBUILD, 0, 0);
		}
		if (LOWORD(wp) == TB_ENDTRACK) bridge_save_config();
		return 0;
	}
	case WM_NOTIFY: {
		NMHDR *nh = (NMHDR *)lp;
		if (nh->code == NM_CUSTOMDRAW && nh->idFrom >= IDC_SLIDER && nh->idFrom < IDC_SLIDER + NSLIDERS)
			return draw_slider((NMCUSTOMDRAW *)lp);
		break;
	}
	case WM_COMMAND: {
		int id = LOWORD(wp), code = HIWORD(wp);
		if (id >= IDC_HELPICON && id < IDC_HELPICON + 20) { /* clicking a "?" shows its text too */
			SendMessageW(g_tip, TTM_POPUP, 0, 0);
			return 0;
		}
		if (code != BN_CLICKED && code != BN_DOUBLECLICKED) return 0; /* owner-draw buttons report fast clicks as double clicks */
		if (id >= IDC_RENAME && id < IDC_RENAME + MAX_TOYS) {
			start_rename(h, id - IDC_RENAME);
			return 0;
		}
		if (id == IDC_LOGPATH || id == IDC_CSVPATH) {
			ShellExecuteW(NULL, L"open", id == IDC_LOGPATH ? g_cfg.log_dir : g_cfg.csv_dir, NULL, NULL, SW_SHOWNORMAL);
			return 0;
		}
		int *chk = check_value(id);
		if (chk) {
			*chk = !*chk;
			InvalidateRect((HWND)lp, NULL, TRUE);
			if (id == IDC_OVERLAY) ovl_apply();
			if (id == IDC_CSV) {
				bridge_csv_reset();
				if (g_cfg.rumble_log && !dir_writable(g_cfg.csv_dir))
					ui_notify("Can't write the rumble CSV", "That folder is read-only. Choose another one under Logs.");
				PostMessageW(h, WM_REBUILD, 0, 0); /* show or hide its folder row */
			}
		} else if (id >= IDC_MAP && id < IDC_MAP + MAX_TOYS * 32) {
			int rel = id - IDC_MAP, k = rel / 32, vi = rel / 4 % 8;
			if (k >= g_ntoys) return 0;
			g_toys[k].map[vi] = rel % 4;
			bridge_set_map(g_toys[k].index, vi, rel % 4);
			for (int j = 0; j < 4; j++) InvalidateRect(GetDlgItem(h, IDC_MAP + rel / 4 * 4 + j), NULL, TRUE);
			if (needed_settings() != g_built_needs || settings_warning(NULL, 0) != g_built_warn) PostMessageW(h, WM_REBUILD, 0, 0);
			return 0;
		} else if (id >= IDC_MODEBTN && id < IDC_MODEBTN + 4) {
			g_cfg.mode = id - IDC_MODEBTN;
			for (int i = 0; i < 4; i++) InvalidateRect(GetDlgItem(h, IDC_MODEBTN + i), NULL, TRUE);
			if (needed_settings() != g_built_needs || settings_warning(NULL, 0) != g_built_warn) PostMessageW(h, WM_REBUILD, 0, 0);
		} else {
			switch (id) {
			case IDC_TEST: bridge_test(); return 0;
			case IDC_SCAN: /* already-connected toys don't reappear, so show that a scan is running */
				bridge_scan();
				g_scan_until = GetTickCount() + 8000;
				update_status();
				return 0;
			case IDC_LOG: open_log(); return 0;
			case IDC_LASTCSV: open_last_csv(); return 0;
			case IDC_LOGDIR: change_folder(h, 0); return 0;
			case IDC_CSVDIR: change_folder(h, 1); return 0;
			case IDC_CLOSE: case IDCANCEL: DestroyWindow(h); return 0;
			}
			return 0;
		}
		bridge_save_config();
		return 0;
	}
	case WM_DRAWITEM:
		draw_owner((DRAWITEMSTRUCT *)lp);
		return TRUE;
	case WM_SETCURSOR: {
		int id = GetDlgCtrlID((HWND)wp);
		if ((id >= IDC_HELPICON && id < IDC_HELPICON + 20) || id == IDC_LOGPATH || id == IDC_CSVPATH) {
			SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
			return TRUE;
		}
		break;
	}
	case WM_TIMER:
		update_status();
		return 0;
	case WM_SIZING: { /* keep the proportions: the content just gets bigger or smaller */
		RECT *r = (RECT *)lp, nc = {0, 0, 0, 0};
		AdjustWindowRectEx(&nc, GetWindowLongW(h, GWL_STYLE), FALSE, GetWindowLongW(h, GWL_EXSTYLE));
		int ncw = nc.right - nc.left, nch = nc.bottom - nc.top;
		float base = g_dpi / 96.0f, k;
		if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM) k = (r->bottom - r->top - nch) / (g_content_h * base);
		else k = (r->right - r->left - ncw) / (360 * base);
		k = k < 0.85f ? 0.85f : k > 2.5f ? 2.5f : k;
		int w = (int)(360 * base * k + 0.5f) + ncw, hh = (int)(g_content_h * base * k + 0.5f) + nch;
		if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) r->left = r->right - w;
		else r->right = r->left + w;
		if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) r->top = r->bottom - hh;
		else r->bottom = r->top + hh;
		return TRUE;
	}
	case WM_SIZE:
		if (wp == SIZE_RESTORED && !g_building) {
			float k = LOWORD(lp) / (360 * g_dpi / 96.0f);
			if (k > 0.5f && (k - g_cfg.ui_scale > 0.01f || g_cfg.ui_scale - k > 0.01f)) {
				g_cfg.ui_scale = k;
				win_scale_refresh();
				build_controls_ex(h, 0);
			}
		}
		return 0;
	case WM_EXITSIZEMOVE:
		bridge_save_config();
		return 0;
	case WM_REBUILD:
		build_controls(h);
		return 0;
	case WM_ENDRENAME:
		end_rename(h, (int)wp);
		return 0;
	case WM_CTLCOLOREDIT: /* the rename box */
		if (!g_edit_brush) g_edit_brush = CreateSolidBrush(RGB(48, 48, 56));
		SetBkColor((HDC)wp, RGB(48, 48, 56));
		SetTextColor((HDC)wp, RGB(255, 255, 255));
		return (LRESULT)g_edit_brush;
	case WM_CTLCOLORSTATIC: {
		int id = GetDlgCtrlID((HWND)lp);
		SetBkColor((HDC)wp, RGB_BG);
		if (id >= IDC_RENAME && id < IDC_RENAME + MAX_TOYS) {
			start_rename(h, id - IDC_RENAME);
			return 0;
		}
		if (id == IDC_WARN) {
			SetBkColor((HDC)wp, RGB_BG);
			SetTextColor((HDC)wp, RGB(255, 176, 32));
			return (LRESULT)g_bg_brush;
		}
		if (id == IDC_LOGPATH || id == IDC_CSVPATH) { /* red when that folder can't be written */
			SetBkColor((HDC)wp, RGB_BG);
			SetTextColor((HDC)wp, (id == IDC_LOGPATH ? g_log_ok : g_csv_ok) ? RGB_SUB : RGB(255, 99, 88));
			return (LRESULT)g_bg_brush;
		}
		SetTextColor((HDC)wp, id >= IDC_VALUE && id < IDC_VALUE + NSLIDERS ? RGB_ACCENT
		                      : id == IDC_STATUS + 50                       ? RGB_SUB
		                                                                    : RGB_TEXT);
		return (LRESULT)g_bg_brush;
	}
	case WM_CLOSE:
		DestroyWindow(h);
		return 0;
	case WM_DESTROY:
		bridge_save_config();
		if (g_tip) DestroyWindow(g_tip);
		g_tip = NULL;
		set_app_id(h, NULL); /* Windows asks to clear it before the window goes away */
		g_win = NULL;
		return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK settings_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	win_scale_enter();
	LRESULT r = settings_proc_inner(h, msg, wp, lp);
	win_scale_leave();
	return r;
}

/* Own taskbar identity for the settings window. Without it Windows groups it with PCSX2
 * (same process), and the group can end up showing our icon instead of PCSX2's. */
static void set_app_id(HWND h, const WCHAR *id)
{
	static const PROPERTYKEY key_app_id = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
	IPropertyStore *ps;
	if (FAILED(SHGetPropertyStoreForWindow(h, &IID_IPropertyStore, (void **)&ps))) return;
	PROPVARIANT pv;
	PropVariantInit(&pv);
	if (id) {
		pv.vt = VT_LPWSTR;
		pv.pwszVal = (LPWSTR)id; /* only read during SetValue */
	}
	ps->lpVtbl->SetValue(ps, &key_app_id, &pv); /* VT_EMPTY removes it */
	ps->lpVtbl->Release(ps);
}

static void open_settings_scaled(void)
{
	if (g_win) {
		ShowWindow(g_win, SW_RESTORE);
		SetForegroundWindow(g_win);
		return;
	}
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME; /* resizable: zooms */
	RECT r = {0, 0, S(360), S(496)};
	AdjustWindowRectEx(&r, style, FALSE, 0);
	int w = r.right - r.left, hgt = r.bottom - r.top;
	/* place it above the taskbar, near the tray */
	RECT work;
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	HWND h = CreateWindowExW(WS_EX_TOPMOST, L"PCSX2HapticsSettings", L"PCSX2 Haptics", style,
	                         work.right - w - S(16), work.bottom - hgt - S(16), w, hgt, NULL, NULL, g_mod, NULL);
	SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)g_icon_small);
	SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)g_icon_big);
	set_app_id(h, L"PCSX2Haptics.Settings"); /* before it is shown */
	ShowWindow(h, SW_SHOW);
	SetForegroundWindow(h);
}

static void open_settings(void)
{
	win_scale_enter(); /* the first size must already be the zoomed one */
	open_settings_scaled();
	win_scale_leave();
}

/* ---------- rumble graph overlay ----------
 * A small always-on-top graph, like PCSX2's frame-time graph: the controller's two
 * motors and each toy vibrator over the last few seconds. Drag to move, right-click
 * to hide. Never takes focus from the game. */

#define HIST 120 /* samples; one every 50 ms = 6 seconds */
#define MAX_SERIES 6
#define C_LARGE 0xFF4FA3FF
#define C_SMALL 0xFF5BD75B
typedef struct { float x, y; } PointF;
int WINAPI GdipDrawLines(GpGraphics *, GpPen *, const PointF *, int);

static HWND g_ovl;
static HFONT g_font_small;
static float g_hist[MAX_SERIES][HIST];
static int g_hist_pos, g_nseries;
static WCHAR g_series_label[MAX_SERIES][64];
static DWORD g_series_color[MAX_SERIES];
static POINT g_drag;
static const DWORD out_colors[] = {C_ACCENT, 0xFFFF9F43, 0xFFB57BFF, 0xFFFFD84A};

static void ovl_sample(void)
{
	Levels lv;
	bridge_levels(&lv);
	float v[MAX_SERIES];
	int n = 0;
	v[n] = lv.large; g_series_color[n] = C_LARGE; lstrcpyW(g_series_label[n++], L"Large motor");
	v[n] = lv.small; g_series_color[n] = C_SMALL; lstrcpyW(g_series_label[n++], L"Small motor");
	for (int t = 0; t < lv.ntoys; t++)
		for (int i = 0; i < lv.toy[t].nvib && n < MAX_SERIES; i++) {
			WCHAR name[64];
			MultiByteToWideChar(CP_UTF8, 0, lv.toy[t].name, -1, name, 64);
			const WCHAR *src = map_labels[lv.toy[t].map[i]];
			if (lv.toy[t].nvib > 1)
				_snwprintf(g_series_label[n], 64, L"%s %d (%s)", name, i + 1, src);
			else
				_snwprintf(g_series_label[n], 64, L"%s (%s)", name, src);
			g_series_color[n] = out_colors[(n - 2) % 4];
			v[n++] = lv.toy[t].out[i];
		}
	g_nseries = n;
	for (int s = 0; s < MAX_SERIES; s++) g_hist[s][g_hist_pos] = s < n ? v[s] : 0;
	g_hist_pos = (g_hist_pos + 1) % HIST;
}

static int ovl_height(void)
{
	int outs = g_nseries > 2 ? g_nseries - 2 : 1;
	return S(120 + 18 * (outs > 2 ? outs : 2));
}

static void ovl_paint(HDC dc, int w, int h)
{
	RECT all = {0, 0, w, h};
	FillRect(dc, &all, g_bg_brush);
	GpGraphics *g = gfx(dc);
	float gx = (float)S(34), gy = (float)S(22), gw = w - gx - S(12), gh = (float)S(78); /* gy: room above the 100 label */

	/* grid + axis labels */
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_font_small);
	SetTextColor(dc, RGB_SUB);
	for (int k = 0; k <= 2; k++) {
		float y = gy + gh * k / 2;
		fill_round(g, gx, y, gw, 1, 0, 0xFF3A3A42);
		RECT r = {0, (int)y - S(7), (int)gx - S(6), (int)y + S(7)};
		DrawTextW(dc, k == 0 ? L"100" : k == 1 ? L"50" : L"0", -1, &r, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
	}

	/* lines: controller motors thin, toy vibrators thicker and on top */
	PointF pts[HIST];
	for (int s = 0; s < g_nseries; s++) {
		for (int k = 0; k < HIST; k++) {
			float v = g_hist[s][(g_hist_pos + k) % HIST];
			pts[k].x = gx + gw * k / (HIST - 1);
			pts[k].y = gy + gh * (1 - (v > 1 ? 1 : v));
		}
		GpPen *p = pen(g_series_color[s], (float)S(s < 2 ? 1 : 2));
		GdipDrawLines(g, p, pts, HIST);
		GdipDeletePen(p);
	}

	/* legend: controller on the left, toy on the right, with current values */
	int ly = (int)(gy + gh) + S(12);
	for (int s = 0; s < g_nseries; s++) {
		int col = s < 2 ? 0 : 1, row = s < 2 ? s : s - 2;
		int x = (int)gx + col * S(118), y = ly + row * S(18);
		fill_round(g, (float)x, (float)y + S(4), (float)S(9), (float)S(9), (float)S(2), g_series_color[s]);
		WCHAR t[96];
		int pct = (int)(g_hist[s][(g_hist_pos + HIST - 1) % HIST] * 100 + 0.5f);
		_snwprintf(t, 96, L"%s  %d%%", g_series_label[s], pct);
		RECT r = {x + S(14), y, col ? w - S(8) : x + S(116), y + S(18)};
		SetTextColor(dc, RGB_TEXT);
		DrawTextW(dc, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	}
	{ /* resize grip: three short diagonal strokes in the bottom-right corner */
		GpPen *p = pen(0xFF55555E, (float)S(1) * 1.2f);
		for (int k = 1; k <= 3; k++) {
			float d = (float)S(4 * k);
			GdipDrawLine(g, p, w - S(4) - d, h - (float)S(4), w - (float)S(4), h - S(4) - d);
		}
		GdipDeletePen(p);
	}
	if (g_nseries <= 2) {
		RECT r = {(int)gx + S(118) + S(14), ly, w - S(8), ly + S(18)};
		SetTextColor(dc, RGB_SUB);
		DrawTextW(dc, L"No toy connected", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
	}
	GdipDeleteGraphics(g);
}

static void ovl_apply(void);
static int g_ovl_resizing;

static int ovl_in_grip(HWND h)
{
	POINT pt;
	RECT r;
	GetCursorPos(&pt);
	ScreenToClient(h, &pt);
	GetClientRect(h, &r);
	return pt.x >= r.right - S(18) && pt.y >= r.bottom - S(18);
}

static LRESULT ovl_proc_inner(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static void make_ovl_font(void)
{
	if (g_font_small) DeleteObject(g_font_small);
	g_font_small = CreateFontW(-(int)(8 * g_dpi / 72.0f * g_cfg.ovl_scale + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0,
	                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static LRESULT CALLBACK ovl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	ScaleScope old = normal_size_enter(); /* never drawn with the settings zoom... */
	g_cur_scale = g_cfg.ovl_scale;        /* ...but with its own */
	LRESULT r = ovl_proc_inner(h, msg, wp, lp);
	normal_size_leave(old);
	return r;
}

static LRESULT ovl_proc_inner(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_TIMER: {
		int before = g_nseries;
		ovl_sample();
		if (g_nseries != before) { /* toys came or went: grow or shrink the legend */
			RECT r;
			GetWindowRect(h, &r);
			SetWindowPos(h, NULL, 0, 0, r.right - r.left, ovl_height(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		InvalidateRect(h, NULL, FALSE);
		return 0;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(h, &ps);
		RECT r;
		GetClientRect(h, &r);
		HDC mem = CreateCompatibleDC(dc);
		HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom), old = SelectObject(mem, bmp);
		ovl_paint(mem, r.right, r.bottom);
		BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
		SelectObject(mem, old);
		DeleteObject(bmp);
		DeleteDC(mem);
		EndPaint(h, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_SETCURSOR: /* corner: resize; anywhere else: move */
		SetCursor(LoadCursorW(NULL, (LPCWSTR)(ovl_in_grip(h) ? IDC_SIZENWSE : IDC_SIZEALL)));
		return TRUE;
	case WM_LBUTTONDOWN: { /* drag anywhere to move it, or its bottom-right corner to resize it */
		POINT pt;
		RECT r;
		GetCursorPos(&pt);
		GetWindowRect(h, &r);
		g_ovl_resizing = ovl_in_grip(h);
		g_drag.x = g_ovl_resizing ? pt.x - (r.right - r.left) : pt.x - r.left;
		g_drag.y = pt.y - r.top;
		SetCapture(h);
		return 0;
	}
	case WM_MOUSEMOVE:
		if (GetCapture() == h) {
			POINT pt;
			GetCursorPos(&pt);
			if (g_ovl_resizing) { /* keep the proportions: everything inside just gets bigger */
				float k = (pt.x - g_drag.x) / (340 * g_dpi / 96.0f);
				k = k < 0.7f ? 0.7f : k > 3.0f ? 3.0f : k;
				if (k - g_cfg.ovl_scale > 0.005f || g_cfg.ovl_scale - k > 0.005f) {
					g_cfg.ovl_scale = g_cur_scale = k;
					make_ovl_font();
					SetWindowPos(h, NULL, 0, 0, S(340), ovl_height(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
					InvalidateRect(h, NULL, FALSE);
				}
			} else {
				SetWindowPos(h, NULL, pt.x - g_drag.x, pt.y - g_drag.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			}
		}
		return 0;
	case WM_LBUTTONUP:
		if (GetCapture() == h) {
			RECT r;
			ReleaseCapture();
			GetWindowRect(h, &r);
			g_cfg.ovl_x = r.left;
			g_cfg.ovl_y = r.top;
			bridge_save_config();
		}
		return 0;
	case WM_RBUTTONUP: /* right-click hides it */
		g_cfg.overlay = 0;
		bridge_save_config();
		ovl_apply();
		sync_window();
		return 0;
	case WM_DESTROY:
		g_ovl = NULL;
		return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

/* Shows or hides the graph to match g_cfg.overlay. */
static void ovl_apply_scaled(void);
static void ovl_apply(void)
{
	float saved = g_cur_scale; /* may be called from the zoomed settings window */
	g_cur_scale = g_cfg.ovl_scale;
	ovl_apply_scaled();
	g_cur_scale = saved;
}

static void ovl_apply_scaled(void)
{
	if (!g_cfg.overlay) {
		if (g_ovl) DestroyWindow(g_ovl);
		return;
	}
	if (g_ovl) return;
	ovl_sample();
	int w = S(340), h = ovl_height(), x = g_cfg.ovl_x, y = g_cfg.ovl_y;
	POINT probe = {x + S(20), y + S(20)};
	if (x == -32768 || !MonitorFromPoint(probe, MONITOR_DEFAULTTONULL)) { /* first time, or its monitor is gone */
		RECT work;
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		x = work.left + S(16);
		y = work.top + S(16);
	}
	g_ovl = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
	                        L"PCSX2HapticsGraph", L"PCSX2 Haptics graph", WS_POPUP, x, y, w, h, NULL, NULL, g_mod, NULL);
	SetLayeredWindowAttributes(g_ovl, 0, 230, LWA_ALPHA);
	HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
	typedef HRESULT(WINAPI * DwmSet_t)(HWND, DWORD, LPCVOID, DWORD);
	DwmSet_t set = dwm ? (DwmSet_t)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
	int round = 2;
	if (set) set(g_ovl, 33, &round, sizeof round);
	SetTimer(g_ovl, 1, 50, NULL);
	ShowWindow(g_ovl, SW_SHOWNOACTIVATE);
}

/* ---------- quick menu ---------- */

static void handle_menu(int cmd)
{
	if (!cmd) return;
	if (cmd == IDM_ENABLED) g_cfg.enabled = !g_cfg.enabled;
	else if (cmd == IDM_FORWARD) g_cfg.forward = !g_cfg.forward;
	else if (cmd == IDM_OVERLAY) { g_cfg.overlay = !g_cfg.overlay; ovl_apply(); }
	else if (cmd >= IDM_SCALE && cmd < IDM_SCALE + 6) g_cfg.scale = scale_steps[cmd - IDM_SCALE] / 100.0f;
	else if (cmd >= IDM_MODE && cmd < IDM_MODE + 4) g_cfg.mode = cmd - IDM_MODE;
	else {
		if (cmd == IDM_SCAN) bridge_scan();
		else if (cmd == IDM_TEST) bridge_test();
		else if (cmd == IDM_SETTINGS) open_settings();
		else if (cmd == IDM_LOG) open_log();
		else if (cmd == IDM_RESTART) bridge_restart_server();
		return;
	}
	bridge_save_config();
	sync_window();
	if (g_win && (needed_settings() != g_built_needs || settings_warning(NULL, 0) != g_built_warn))
		PostMessageW(g_win, WM_REBUILD, 0, 0);
}

static void show_menu(void)
{
	HMENU m = CreatePopupMenu(), scale = CreatePopupMenu(), mode = CreatePopupMenu();
	WCHAR st[100], head[140];
	ToyInfo toy;
	status_text(st, 100);
	_snwprintf(head, 140, L"PCSX2 Haptics: %s", st);

	AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_STATUS, head);
	AppendMenuW(m, MF_SEPARATOR, 0, NULL);
	AppendMenuW(m, MF_STRING | (g_cfg.enabled ? MF_CHECKED : 0), IDM_ENABLED, L"Toy vibration");
	AppendMenuW(m, MF_STRING | (g_cfg.forward ? MF_CHECKED : 0), IDM_FORWARD, L"Controller vibration");
	AppendMenuW(m, MF_STRING | (g_cfg.overlay ? MF_CHECKED : 0), IDM_OVERLAY, L"Show rumble graph");
	for (int i = 0; i < 6; i++) {
		WCHAR t[16];
		wsprintfW(t, L"%d%%", scale_steps[i]);
		int on = (int)(g_cfg.scale * 100 + 0.5f) == scale_steps[i];
		AppendMenuW(scale, MF_STRING | (on ? MF_CHECKED : 0), IDM_SCALE + i, t);
	}
	AppendMenuW(m, MF_POPUP, (UINT_PTR)scale, L"Intensity");
	for (int i = 0; i < 4; i++)
		AppendMenuW(mode, MF_STRING | (g_cfg.mode == i ? MF_CHECKED : 0), IDM_MODE + i, mode_labels[i]);
	AppendMenuW(m, MF_POPUP, (UINT_PTR)mode, L"Mode");
	AppendMenuW(m, MF_SEPARATOR, 0, NULL);
	AppendMenuW(m, MF_STRING | (bridge_connected() ? 0 : MF_GRAYED), IDM_SCAN, L"Scan for toys");
	AppendMenuW(m, MF_STRING | (bridge_toys(&toy, 1) ? 0 : MF_GRAYED), IDM_TEST, L"Test connected toy");
	AppendMenuW(m, MF_STRING | (bridge_owns_server() ? 0 : MF_GRAYED), IDM_RESTART, L"Restart server");
	AppendMenuW(m, MF_SEPARATOR, 0, NULL);
	AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings...");
	AppendMenuW(m, MF_STRING, IDM_LOG, L"Open log");
	SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);

	POINT pt;
	GetCursorPos(&pt);
	SetForegroundWindow(g_tray); /* otherwise the menu won't close when clicking elsewhere */
	int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_tray, NULL);
	PostMessageW(g_tray, WM_NULL, 0, 0);
	DestroyMenu(m);
	handle_menu(cmd);
}

/* ---------- thread ---------- */

static LRESULT tray_proc_inner(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	ScaleScope old = normal_size_enter(); /* never drawn with the settings zoom */
	LRESULT r = tray_proc_inner(h, msg, wp, lp);
	normal_size_leave(old);
	return r;
}

static LRESULT tray_proc_inner(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_TRAY) {
		if (LOWORD(lp) == WM_LBUTTONUP) open_settings();
		else if (LOWORD(lp) == WM_RBUTTONUP) show_menu();
		return 0;
	}
	if (msg == WM_NOTE) { tray_show_note(); return 0; }
	if (msg == WM_REFRESH) { tray_update_tip(); if (g_win) update_status(); return 0; }
	if (msg == g_taskbar_created) { tray_add(); return 0; } /* Explorer restarted */
	return DefWindowProcW(h, msg, wp, lp);
}

static DWORD WINAPI ui_thread(LPVOID p)
{
	(void)p;
	/* Use our manifest so common controls get the modern (v6) look. */
	WCHAR self_path[MAX_PATH];
	GetModuleFileNameW(g_mod, self_path, MAX_PATH);
	ACTCTXW ac = {sizeof ac};
	ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
	ac.hModule = g_mod;
	ac.lpResourceName = MAKEINTRESOURCEW(2);
	ac.lpSource = self_path;
	HANDLE actx = CreateActCtxW(&ac);
	ULONG_PTR cookie;
	if (actx != INVALID_HANDLE_VALUE) ActivateActCtx(actx, &cookie);

	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); /* folder picker */
	ULONG_PTR gdip_token;
	GpStartupInput gsi = {1, NULL, FALSE, FALSE};
	GdiplusStartup(&gdip_token, &gsi, NULL);
	g_bg_brush = CreateSolidBrush(RGB_BG);

	INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&icc);

	typedef UINT(WINAPI * GetDpiForSystem_t)(void);
	GetDpiForSystem_t get_dpi = (GetDpiForSystem_t)GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForSystem");
	if (get_dpi) g_dpi = (int)get_dpi();
	g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
	                     CLEARTYPE_QUALITY, 0, L"Segoe UI");
	g_font_bold = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
	                          CLEARTYPE_QUALITY, 0, L"Segoe UI");
	g_icon_small = LoadImageW(g_mod, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	for (int i = 0; i < 200 && !g_cfg_loaded; i++) Sleep(10); /* the graph's zoom comes from the settings */
	make_ovl_font();
	g_icon_help = LoadImageW(g_mod, MAKEINTRESOURCEW(3), IMAGE_ICON, S(16), S(16), 0);
	g_icon_big = LoadImageW(g_mod, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);

	WNDCLASSW wc = {0};
	wc.lpfnWndProc = tray_proc;
	wc.hInstance = g_mod;
	wc.lpszClassName = L"PCSX2HapticsTray";
	RegisterClassW(&wc);
	wc.lpfnWndProc = settings_proc;
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.hbrBackground = g_bg_brush;
	wc.lpszClassName = L"PCSX2HapticsSettings";
	RegisterClassW(&wc);
	wc.lpfnWndProc = ovl_proc;
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL);
	wc.hbrBackground = NULL;
	wc.lpszClassName = L"PCSX2HapticsGraph";
	RegisterClassW(&wc);
	wc.lpfnWndProc = toast_proc;
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
	wc.hbrBackground = NULL;
	wc.lpszClassName = L"PCSX2HapticsToast";
	RegisterClassW(&wc);

	g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
	g_tray = CreateWindowExW(0, L"PCSX2HapticsTray", L"", 0, 0, 0, 0, 0, NULL, NULL, g_mod, NULL);
	tray_add();
	tray_show_note(); /* anything reported before the icon existed */
	for (int i = 0; i < 200 && !g_cfg_loaded; i++) Sleep(10); /* the bridge loads the settings in parallel */
	ovl_apply();

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		if (g_win && IsDialogMessageW(g_win, &msg)) continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return 0;
}

void ui_start(HMODULE self)
{
	g_mod = self;
	InitializeCriticalSection(&g_note_cs);
	CloseHandle(CreateThread(NULL, 0, ui_thread, NULL, 0, NULL));
}

void ui_notify(const char *title, const char *text)
{
	EnterCriticalSection(&g_note_cs);
	MultiByteToWideChar(CP_UTF8, 0, title, -1, g_note_title, 64);
	MultiByteToWideChar(CP_UTF8, 0, text, -1, g_note_text, 256);
	LeaveCriticalSection(&g_note_cs);
	if (g_tray) PostMessageW(g_tray, WM_NOTE, 0, 0);
}

void ui_refresh(void)
{
	if (g_tray) PostMessageW(g_tray, WM_REFRESH, 0, 0);
}

void ui_shutdown(void)
{
	if (g_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_nid);
}
