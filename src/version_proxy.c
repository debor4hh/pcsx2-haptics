/*
 * version.dll proxy: how the mod gets into PCSX2.
 *
 * pcsx2-qt.exe imports VERSION.dll, and Windows looks for it in the exe's folder
 * first, so this file is loaded at startup. Every version.dll function jumps
 * straight to the real one in System32; then a startup thread installs the
 * rumble hooks (hooks.c) and starts the bridge and the tray icon.
 */
#include <windows.h>
#include "bridge.h"

void hooks_install(void);
void hooks_describe(char *out, int len);

#define VERSION_FUNCS(X) \
	X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) X(GetFileVersionInfoExW) \
	X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA) X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) \
	X(GetFileVersionInfoW) X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW) \
	X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

/* Real function pointers, and thunks that jump to them with every register untouched. */
#define DECLARE_PTR(n) FARPROC real_##n;
VERSION_FUNCS(DECLARE_PTR)
#define THUNK(n) __asm__(".globl " #n "\n" #n ":\n\tjmp *real_" #n "(%rip)\n");
VERSION_FUNCS(THUNK)

static HMODULE g_self;

static int is_pcsx2(void)
{
	WCHAR exe[MAX_PATH];
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	const WCHAR *base = wcsrchr(exe, L'\\');
	base = base ? base + 1 : exe;
	return !_wcsnicmp(base, L"pcsx2", 5) && _wcsnicmp(base, L"pcsx2-updater", 13); /* not the updater */
}

static DWORD WINAPI startup(LPVOID p)
{
	(void)p;
	ui_start(g_self);    /* first: the bridge may post notifications right away */
	bridge_start(g_self);
	hooks_install();     /* PCSX2 is still starting up; its input sources come later */
	char msg[128];
	hooks_describe(msg, sizeof msg);
	bridge_log(msg);
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = inst;
		DisableThreadLibraryCalls(inst);
		WCHAR path[MAX_PATH];
		UINT n = GetSystemDirectoryW(path, MAX_PATH);
		lstrcpyW(path + n, L"\\version.dll");
		HMODULE real = LoadLibraryW(path);
		if (!real) return FALSE;
#define RESOLVE(f) real_##f = GetProcAddress(real, #f);
		VERSION_FUNCS(RESOLVE)
		/* runs once the loader is done, i.e. after this DllMain returns */
		if (is_pcsx2()) CloseHandle(CreateThread(NULL, 0, startup, NULL, 0, NULL));
	} else if (reason == DLL_PROCESS_DETACH && reserved) {
		ui_shutdown();     /* process is exiting: remove the tray icon */
		bridge_shutdown(); /* and let the engine drop its Bluetooth connection cleanly */
	}
	return TRUE;
}
