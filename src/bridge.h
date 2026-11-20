#pragma once
#include <windows.h>

#define PCSX2_HAPTICS_VERSION "1.0.0"

enum { MODE_MAX, MODE_MIX, MODE_LARGE, MODE_SMALL };
enum { MAP_LARGE, MAP_SMALL, MAP_BOTH, MAP_OFF }; /* what drives each toy vibrator */

typedef struct {
	/* [Intiface] */
	WCHAR host[128];
	int port;
	int auto_start;          /* launch intiface-engine.exe if no server is running */
	WCHAR server_path[MAX_PATH];
	WCHAR server_args[256];  /* extra engine arguments */
	/* [Haptics] — editable live from the tray */
	int enabled, controller, mode, interval_ms;
	float large_w, small_w, scale, min_i;
	/* [Controller] */
	int forward;
	/* [Notifications] */
	int notify;
	/* [Overlay] rumble graph */
	int overlay, ovl_x, ovl_y;
	float ovl_scale;         /* graph zoom, set by dragging its corner */
	/* [Debug] */
	int log, log_rumble;
	int rumble_log;          /* CSV of every rumble change in rumble-logs\ */
	/* [Paths] */
	WCHAR log_dir[MAX_PATH]; /* pcsx2-haptics.log and intiface-engine.log */
	WCHAR csv_dir[MAX_PATH]; /* rumble CSV files */
	/* [Window] */
	float ui_scale;          /* settings window zoom, set by resizing it */
} Settings;

extern Settings g_cfg;
extern volatile LONG g_cfg_loaded;
extern WCHAR g_dir[MAX_PATH];  /* folder of this DLL (the PCSX2 folder) */

void bridge_start(HMODULE self);
int dir_writable(const WCHAR *dir);
/* Change where logs go; saved to the ini. Return whether the folder is writable. */
int bridge_set_log_dir(const WCHAR *dir);
int bridge_set_csv_dir(const WCHAR *dir);
void bridge_csv_reset(void);
void bridge_log(const char *msg);
void bridge_rumble(DWORD user_index, WORD large, WORD small);
void bridge_save_config(void);
void bridge_scan(void);
void bridge_test(void);
void bridge_status(char *out, int len);
int bridge_connected(void);
int bridge_owns_server(void);
void bridge_restart_server(void);
void bridge_shutdown(void);  /* process exit: stop the engine cleanly */
typedef struct {
	char name[64];  /* model, e.g. "Lovense Hush" */
	char alias[64]; /* user's name for this toy, "" if none */
	char addr[24];  /* Bluetooth address, "" if unknown (aliases need it) */
	int battery;  /* -1 = unknown */
	int index;    /* Buttplug device index */
	int nvib;
	int map[8];   /* MAP_* per vibrator */
} ToyInfo;
void bridge_set_map(int dev_index, int vib, int source);
/* Names one toy; "" removes the name. Returns 0 if its address is unknown. */
int bridge_set_alias(int dev_index, const WCHAR *alias);

/* Live levels for the rumble graph: controller motors (0..1, as the game sends them)
 * and what each toy vibrator was last told (0..1). */
typedef struct {
	float large, small;
	int ntoys;
	struct { char name[64]; int nvib; int map[8]; float out[8]; } toy[4];
} Levels;
void bridge_levels(Levels *lv);
/* Connected toys that can vibrate; returns how many were written. */
int bridge_toys(ToyInfo *out, int max);

/* ui.c */
void ui_start(HMODULE self);
void ui_notify(const char *title, const char *text);
void ui_refresh(void);
void ui_shutdown(void);
