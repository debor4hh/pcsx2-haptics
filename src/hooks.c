/*
 * Rumble hooks, installed by patching pcsx2-qt.exe's import table (no code patching).
 *
 * PCSX2 has two Windows gamepad paths that can rumble:
 *   XInput - loaded at runtime with LoadLibrary + GetProcAddress, so we hook
 *            GetProcAddress and hand out our own XInputSetState.
 *   SDL    - SDL3.dll is a normal import; we hook SDL_RumbleGamepad and the
 *            haptic fallbacks PCSX2 uses for pads without gamepad rumble.
 * Every hook stores the motor levels for the bridge, then calls the real function
 * (with zeros when "Controller vibration" is off), so the pad keeps working.
 */
#include <windows.h>
#include <string.h>
#include "bridge.h"

/* ---------- import table patching ---------- */

static void *iat_hook(HMODULE mod, const char *dll, const char *func, void *hook)
{
	BYTE *base = (BYTE *)mod;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) return NULL;
	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
		if (lstrcmpiA((char *)base + d->Name, dll) || !d->OriginalFirstThunk) continue;
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
		IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, slots++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
			IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
			if (strcmp((char *)n->Name, func)) continue;
			void *old = (void *)slots->u1.Function;
			DWORD prot;
			VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &prot);
			slots->u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&slots->u1.Function, sizeof(void *), prot, &prot);
			return old;
		}
	}
	return NULL;
}

/* ---------- XInput ---------- */

typedef struct { WORD left, right; } XVibration; /* XINPUT_VIBRATION */
typedef DWORD(WINAPI *XSetState_t)(DWORD, XVibration *);
typedef FARPROC(WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);

static XSetState_t real_xinput_set_state;
static GetProcAddress_t real_get_proc_address;

static DWORD WINAPI hook_xinput_set_state(DWORD user, XVibration *v)
{
	if (v) bridge_rumble(user, v->left, v->right);
	if (g_cfg.forward || !v) return real_xinput_set_state(user, v);
	XVibration off = {0, 0};
	return real_xinput_set_state(user, &off);
}

static FARPROC WINAPI hook_get_proc_address(HMODULE mod, LPCSTR name)
{
	FARPROC p = real_get_proc_address(mod, name);
	if (p && HIWORD((ULONG_PTR)name) && !strcmp(name, "XInputSetState")) {
		real_xinput_set_state = (XSetState_t)p;
		return (FARPROC)hook_xinput_set_state;
	}
	return p;
}

/* ---------- SDL3 ---------- */

typedef BOOL bool32; /* SDL3 functions return a C bool; only the low byte matters */
typedef bool32(__cdecl *RumbleGamepad_t)(void *, WORD, WORD, DWORD);
typedef int(__cdecl *GetPlayerIndex_t)(void *);
typedef bool32(__cdecl *UpdateHapticEffect_t)(void *, int, const void *);
typedef bool32(__cdecl *RunHapticEffect_t)(void *, int, DWORD);
typedef bool32(__cdecl *StopHapticEffect_t)(void *, int);
typedef bool32(__cdecl *PlayHapticRumble_t)(void *, float, DWORD);
typedef bool32(__cdecl *StopHapticRumble_t)(void *);

static RumbleGamepad_t real_rumble_gamepad;
static GetPlayerIndex_t get_player_index;
static UpdateHapticEffect_t real_update_effect;
static RunHapticEffect_t real_run_effect;
static StopHapticEffect_t real_stop_effect;
static PlayHapticRumble_t real_play_rumble;
static StopHapticRumble_t real_stop_rumble;
static WORD g_haptic_large, g_haptic_small;

static bool32 __cdecl hook_rumble_gamepad(void *pad, WORD low, WORD high, DWORD ms)
{
	int player = get_player_index ? get_player_index(pad) : 0;
	bridge_rumble(player < 0 ? 0 : (DWORD)player, low, high); /* low frequency = large motor */
	return g_cfg.forward ? real_rumble_gamepad(pad, low, high, ms) : real_rumble_gamepad(pad, 0, 0, ms);
}

/* Pads without gamepad rumble: PCSX2 uses one left/right haptic effect per pad.
 * SDL_HapticLeftRight is {Uint16 type; Uint32 length; Uint16 large; Uint16 small}. */
static bool32 __cdecl hook_update_effect(void *haptic, int effect, const void *data)
{
	const BYTE *d = data;
	if (d) {
		g_haptic_large = *(const WORD *)(d + 8);
		g_haptic_small = *(const WORD *)(d + 10);
	}
	if (g_cfg.forward || !d) return real_update_effect(haptic, effect, data);
	BYTE quiet[64];
	memcpy(quiet, d, sizeof quiet);
	*(WORD *)(quiet + 8) = *(WORD *)(quiet + 10) = 0;
	return real_update_effect(haptic, effect, quiet);
}

static bool32 __cdecl hook_run_effect(void *haptic, int effect, DWORD iterations)
{
	bridge_rumble(0, g_haptic_large, g_haptic_small);
	return real_run_effect(haptic, effect, iterations);
}

static bool32 __cdecl hook_stop_effect(void *haptic, int effect)
{
	bridge_rumble(0, 0, 0);
	return real_stop_effect(haptic, effect);
}

/* Last resort: plain rumble with one strength. */
static bool32 __cdecl hook_play_rumble(void *haptic, float strength, DWORD ms)
{
	WORD v = (WORD)(strength * 65535.0f);
	bridge_rumble(0, v, v);
	return real_play_rumble(haptic, g_cfg.forward ? strength : 0.0f, ms);
}

static bool32 __cdecl hook_stop_rumble(void *haptic)
{
	bridge_rumble(0, 0, 0);
	return real_stop_rumble(haptic);
}

/* ---------- install ---------- */

void hooks_install(void)
{
	HMODULE exe = GetModuleHandleW(NULL);
	real_get_proc_address = iat_hook(exe, "KERNEL32.dll", "GetProcAddress", hook_get_proc_address);

	HMODULE sdl = GetModuleHandleW(L"SDL3.dll");
	if (sdl) get_player_index = (GetPlayerIndex_t)GetProcAddress(sdl, "SDL_GetGamepadPlayerIndex");
	real_rumble_gamepad = iat_hook(exe, "SDL3.dll", "SDL_RumbleGamepad", hook_rumble_gamepad);
	real_update_effect = iat_hook(exe, "SDL3.dll", "SDL_UpdateHapticEffect", hook_update_effect);
	real_run_effect = iat_hook(exe, "SDL3.dll", "SDL_RunHapticEffect", hook_run_effect);
	real_stop_effect = iat_hook(exe, "SDL3.dll", "SDL_StopHapticEffect", hook_stop_effect);
	real_play_rumble = iat_hook(exe, "SDL3.dll", "SDL_PlayHapticRumble", hook_play_rumble);
	real_stop_rumble = iat_hook(exe, "SDL3.dll", "SDL_StopHapticRumble", hook_stop_rumble);
}

/* For the log: which hooks took. */
void hooks_describe(char *out, int len)
{
	wsprintfA(out, "XInput %s, SDL rumble %s, SDL haptics %s",
	          real_get_proc_address ? "hooked" : "not found",
	          real_rumble_gamepad ? "hooked" : "not found",
	          real_update_effect && real_run_effect ? "hooked" : "not found");
	(void)len;
}
