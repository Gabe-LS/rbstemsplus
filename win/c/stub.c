/* The start stub (stub.h): `RB Stems Plus.exe` in %LOCALAPPDATA%\Programs\RB Stems Plus\. Built
 * with -municode -mwindows, x64 and arm64 (the machine's build is installed, I82). Its imports:
 * KERNEL32, USER32 and the UCRT only (J21; run.sh's gate). Unverified until the VM run (W13a). */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>

#include "aumid.h"
#include "stub.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#  define STUB_ARCH L"arm64"
#else
#  define STUB_ARCH L"x64"
#endif

/* The two lines the stub can say (W14a's table of Windows texts): only what this start does. */
#define TITLE L"RB Stems Plus"
#define TOO_OLD L"RB Stems Plus needs Windows 11, version 22H2 or later."
#define BROKEN L"RB Stems Plus couldn't be opened because its installation is incomplete. Install it again."

typedef LONG(WINAPI *rtl_get_version_fn)(OSVERSIONINFOW *);

/* the Windows build (RtlGetVersion: what the system is, whatever the manifest says), 0 unknown */
static DWORD windows_build(void) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");   /* always mapped: no load */
    rtl_get_version_fn get = nt ? (rtl_get_version_fn)(void (*)(void))GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v;
    ZeroMemory(&v, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    return get && get(&v) == 0 ? v.dwBuildNumber : 0;
}

/* `current` next to the stub, never through a link: 1 with the version, else 0 */
static int read_current(const wchar_t *dir, wchar_t version[STUB_VERSION_MAX]) {
    static wchar_t path[32768];
    if (wcslen(dir) + 9 >= sizeof path / sizeof path[0]) return 0;
    wcscpy(path, dir);
    wcscat(path, L"\\current");
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info;
    char buf[256];
    DWORD got = 0;
    int ok = GetFileInformationByHandle(h, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
             info.nFileSizeHigh == 0 && info.nFileSizeLow < sizeof buf && ReadFile(h, buf, sizeof buf, &got, NULL) &&
             stub_current(buf, got, version);
    CloseHandle(h);
    return ok;
}

static int run(int mode, const wchar_t *args) {
    static wchar_t self[32768], target[32768], line[32768];
    DWORD n = GetModuleFileNameW(NULL, self, sizeof self / sizeof self[0]);
    if (n == 0 || n >= sizeof self / sizeof self[0]) return 0;
    wchar_t *slash = wcsrchr(self, L'\\');
    if (!slash) return 0;
    *slash = 0;   /* self is now the stub's folder */
    wchar_t version[STUB_VERSION_MAX];
    if (!read_current(self, version) || stub_target(self, version, mode, STUB_ARCH, target, sizeof target / sizeof target[0]) ||
        stub_cmdline(target, mode == STUB_APP ? args : L"", line, sizeof line / sizeof line[0]))
        return 0;
    DWORD a = GetFileAttributesW(target);
    if (a == INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return 0;
    /* the version folder is the child's working folder */
    wchar_t *cut = wcsrchr(target, L'\\');
    *cut = 0;
    wchar_t folder[32768];
    wcscpy(folder, target);
    *cut = L'\\';
    /* the shortcut's startup info, with its AppUserModelID (STARTF_TITLEISAPPID) and show state,
     * goes to the app, so its window groups with the pin; started any other way (the Run value, a
     * link without an ID), the stub passes the app's own ID instead (aumid.h). TITLEISLINKNAME
     * shares lpTitle with the ID, so it is dropped then. */
    static wchar_t aumid[] = RBSP_AUMID;
    STARTUPINFOW mine, si;
    GetStartupInfoW(&mine);
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = mine.dwFlags & (STARTF_USESHOWWINDOW | STARTF_PREVENTPINNING);
    si.wShowWindow = mine.wShowWindow;
    if ((mine.dwFlags & STARTF_TITLEISAPPID) && mine.lpTitle && mine.lpTitle[0]) {
        si.dwFlags |= STARTF_TITLEISAPPID;
        si.lpTitle = mine.lpTitle;
    } else {
        si.dwFlags |= STARTF_TITLEISAPPID;
        si.lpTitle = aumid;
    }
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(target, line, NULL, NULL, FALSE, 0, NULL, folder, &si, &pi)) return 0;
    /* the stub holds the foreground rights it was started with; only it can pass them on (I88) */
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* --after-exit (stub.h, afterexit.h): only from a temp copy (its folder named rbsp-uninstall-<16
 * hex>, no `current` beside it; the core DLL checks the rest), the core DLL beside it called with
 * the arguments; its outcome is the exit code (10 not from a copy, 11 no DLL or no entry, 12 the
 * arguments aren't UTF-16 it can pass on). The stub shows nothing; the core DLL's one dialog (an
 * uninstall kept because part of it still runs: T_WIN_UNINSTALL_KEPT, afterexit.h) is its own. */
typedef int32_t(__cdecl *after_exit_fn)(const char *args);
static int after_exit(const wchar_t *args) {
    static wchar_t dir[32768], path[32768];
    static char a8[32768];
    DWORD n = GetModuleFileNameW(NULL, dir, sizeof dir / sizeof dir[0]);
    if (n == 0 || n >= sizeof dir / sizeof dir[0] - 32) return 10;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return 10;
    *slash = 0;
    const wchar_t *own = wcsrchr(dir, L'\\');
    if (!own || !stub_after_exit_dir(own + 1)) return 10;   /* only the uninstall's temp folder (review UN1) */
    /* an install's stub has `current` beside it: never the after-exit there */
    wcscpy(path, dir);
    wcscat(path, L"\\current");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return 10;
    wcscpy(path, dir);
    wcscat(path, L"\\rbsp-core.dll");
    HMODULE core = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    after_exit_fn fn = core ? (after_exit_fn)(void (*)(void))GetProcAddress(core, "core_win_after_exit") : NULL;
    if (!fn) return 11;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, args, -1, a8, (int)sizeof a8, NULL, NULL) <= 0) return 12;
    return (int)fn(a8);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)inst, (void)prev, (void)cmd, (void)show;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    const wchar_t *args = stub_args(GetCommandLineW());
    int mode = stub_mode(args);
    if (windows_build() < STUB_MIN_BUILD) {
        if (mode == STUB_APP) MessageBoxW(NULL, TOO_OLD, TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (mode == STUB_AFTER_EXIT) return after_exit(args);
    if (run(mode, args)) return 0;
    if (mode == STUB_APP) MessageBoxW(NULL, BROKEN, TITLE, MB_OK | MB_ICONERROR);
    return 1;
}
