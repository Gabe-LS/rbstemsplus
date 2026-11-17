/* afterexit_win.c: the after-exit on Windows (afterexit.h): SELF_DELETE's launch of the stub copy,
 * the ops ax_run asks (update_os's watcher, process and tree calls; the records through
 * lifecycle.h; cmd.exe or RunOnce for the temp folder), and the core DLL's core_win_after_exit,
 * which the stub copy calls. In the core DLL only (core/build.sh's DLL_WINC). Unverified until
 * the VM run (W13a). */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "afterexit.h"
#include "core.h"   /* core_text: the kept dialog's words */
#include "lifecycle.h"
#include "plat.h"
#include "update.h"
#include "update_shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ADVAPI32's RtlGenRandom (the core DLL imports ADVAPI32 already; no bcrypt) */
__declspec(dllimport) BOOLEAN WINAPI SystemFunction036(PVOID buffer, ULONG length);

static int to_wide(const char *s, wchar_t *out, int cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 0 ? 0 : -1;
}
static int to_utf8(const wchar_t *w, char *out, int cap) {
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, cap, NULL, NULL) > 0 ? 0 : -1;
}

/* the temp folder's files and the folder itself, quietly */
static void remove_launch_folder(const wchar_t *dir) {
    static wchar_t p[AX_PATH_MAX + 32];
    _snwprintf(p, AX_PATH_MAX + 32, L"%ls\\" L"RB Stems Plus.exe", dir);
    p[AX_PATH_MAX + 31] = 0;
    DeleteFileW(p);
    _snwprintf(p, AX_PATH_MAX + 32, L"%ls\\" L"rbsp-core.dll", dir);
    p[AX_PATH_MAX + 31] = 0;
    DeleteFileW(p);
    RemoveDirectoryW(dir);
}

int afterexit_launch(const char *apps, const char *own, const char *arch) {
    char local[AX_PATH_MAX], home[AX_PATH_MAX], tmp8[AX_PATH_MAX], name[64], dir8[AX_PATH_MAX], src[AX_PATH_MAX], dst[AX_PATH_MAX],
        line[AX_LINE_MAX];
    static wchar_t tmpw[AX_PATH_MAX], dirw[AX_PATH_MAX], srcw[AX_PATH_MAX], dstw[AX_PATH_MAX], linew[AX_LINE_MAX];
    if (!apps || !own || !own[0] || !arch || (strcmp(arch, "x64") && strcmp(arch, "arm64")) || ax_local_of(apps, local, sizeof local)) return EINVAL;
    /* only this account's own install: the copy refuses any other %LOCALAPPDATA% (review UN11) */
    if (plat_home(home, sizeof home) || !ax_same_dir(local, home)) return EINVAL;
    DWORD tn = GetTempPathW(AX_PATH_MAX, tmpw);   /* ends with '\' */
    if (tn == 0 || tn >= AX_PATH_MAX || to_utf8(tmpw, tmp8, sizeof tmp8)) return EIO;
    int made = 0;
    for (int i = 0; i < 4 && !made; i++) {
        unsigned char rnd[8];
        if (!SystemFunction036(rnd, sizeof rnd) || ax_temp_name(rnd, name, sizeof name)) return EIO;
        int k = snprintf(dir8, sizeof dir8, "%s%s", tmp8, name);
        if (k < 0 || (size_t)k >= sizeof dir8 || to_wide(dir8, dirw, AX_PATH_MAX)) return ENAMETOOLONG;
        if (CreateDirectoryW(dirw, NULL)) made = 1;
        else if (GetLastError() != ERROR_ALREADY_EXISTS) return EIO;
    }
    if (!made) return EEXIST;
    /* the stub and the core DLL of the machine's architecture, from the app's own version folder */
    static const char *const rel[2][2] = {{"stub", AX_EXE}, {NULL, AX_DLL}};
    for (int i = 0; i < 2; i++) {
        int k1 = rel[i][0] ? snprintf(src, sizeof src, "%s\\%s\\%s\\%s\\%s", apps, own, rel[i][0], arch, rel[i][1])
                           : snprintf(src, sizeof src, "%s\\%s\\%s\\%s", apps, own, arch, rel[i][1]);
        int k2 = snprintf(dst, sizeof dst, "%s\\%s", dir8, rel[i][1]);
        if (k1 < 0 || (size_t)k1 >= sizeof src || k2 < 0 || (size_t)k2 >= sizeof dst || to_wide(src, srcw, AX_PATH_MAX) || to_wide(dst, dstw, AX_PATH_MAX)
            || !CopyFileW(srcw, dstw, TRUE)) {
            remove_launch_folder(dirw);
            return EIO;
        }
    }
    snprintf(dst, sizeof dst, "%s\\%s", dir8, AX_EXE);
    /* this process, for the copy to wait on: SYNCHRONIZE only, inherited by it alone */
    HANDLE self = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, SYNCHRONIZE, TRUE, 0)) {
        remove_launch_folder(dirw);
        return EIO;
    }
    int r = EIO;
    SIZE_T asz = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    if (ax_cmdline(dst, (uint64_t)(uintptr_t)self, local, line, sizeof line) || to_wide(line, linew, AX_LINE_MAX) || to_wide(dst, dstw, AX_PATH_MAX)) {
        r = ENAMETOOLONG;
        goto done;
    }
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    attrs = asz ? HeapAlloc(GetProcessHeap(), 0, asz) : NULL;
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &asz)) {
        if (attrs) HeapFree(GetProcessHeap(), 0, attrs);
        attrs = NULL;
        goto done;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self, sizeof self, NULL, NULL)) goto done;
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi;
    /* its working folder is %TEMP%: never the app's folder (it would hold it), never its own */
    if (CreateProcessW(dstw, linew, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, tmpw, &si.StartupInfo, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        r = 0;
    }
done:
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        HeapFree(GetProcessHeap(), 0, attrs);
    }
    CloseHandle(self);   /* the copy has its own */
    if (r) remove_launch_folder(dirw);
    return r;
}

/* ------------------------------------------------------------------ ax_run's ops */

typedef struct {
    const update_os *u;
    wchar_t self_dir[AX_PATH_MAX];   /* the temp folder this process runs from */
    wchar_t parent[AX_PATH_MAX];     /* its parent, with a trailing '\\': cmd's working folder */
    char name[64];                   /* its name (ax_is_temp_name's, once self_ok said so) */
} win_ax;

/* review UN1: this copy runs from <GetTempPathW>\rbsp-uninstall-<16 hex>, a plain folder: the
 * name here, the folder and its parent in afterexit_self_win.c (also run by the Windows test exe) */
static int w_self_ok(void *ctx) {
    win_ax *w = ctx;
    const wchar_t *base = wcsrchr(w->self_dir, L'\\');
    if (!base || to_utf8(base + 1, w->name, sizeof w->name) || !ax_is_temp_name(w->name)) return 0;
    return ax_win_dir_ok(w->self_dir);
}
static int w_home(void *ctx, char *out, size_t n) {
    (void)ctx;
    return plat_home(out, n);
}

static int w_wait_parent(void *ctx, uint64_t parent, uint32_t ms) {
    (void)ctx;
    return WaitForSingleObject((HANDLE)(uintptr_t)parent, ms) == WAIT_OBJECT_0 ? 0 : 1;
}
static int w_stop_watchers(void *ctx, const char *dir) {
    win_ax *w = ctx;
    return w->u->stop_watchers(w->u->ctx, dir);
}
static int w_running_under(void *ctx, const char *dir) {
    win_ax *w = ctx;
    return w->u->running_under(w->u->ctx, dir);
}
static int w_remove_tree(void *ctx, const char *path) {
    win_ax *w = ctx;
    return w->u->remove_tree(w->u->ctx, path);
}

static int w_remove_run(void *ctx) {
    (void)ctx;
    return life_run_remove();
}
static int w_remove_records(void *ctx) {
    (void)ctx;
    int left = 0;
    char lnk[LIFE_PATH_MAX];
    if (life_entry_remove()) left++;
    if (life_shortcut_path(lnk, sizeof lnk) || life_shortcut_remove(lnk)) left++;
    if (life_records_remove(LIFE_AUMID)) left++;
    return left;
}

/* cmd.exe's full path (Windows' own, from System32), wide and UTF-8: 0, or -1 */
static int w_cmd_path(wchar_t *sys, wchar_t *cmdw, char *sys8, size_t sys8n, char *cmd8, size_t cmd8n) {
    UINT sn = GetSystemDirectoryW(sys, MAX_PATH);
    if (sn == 0 || sn >= MAX_PATH - 9 || to_utf8(sys, sys8, sys8n)) return -1;
    _snwprintf(cmdw, MAX_PATH + 16, L"%ls\\cmd.exe", sys);
    cmdw[MAX_PATH + 15] = 0;
    return to_utf8(cmdw, cmd8, cmd8n) ? -1 : 0;
}

/* HKCU RunOnce deleting the temp folder at the next logon, its registration read back (the trace
 * audit lists it as a disclosed record): 1 registered, -1 not. clean_later's fallback, and
 * clean_at_logon before the kept dialog. Only after self_ok (w->name checked) */
static int w_runonce(void *ctx) {
    win_ax *w = ctx;
    static wchar_t sys[MAX_PATH], cmdw[MAX_PATH + 16], data[AX_LINE_MAX], back[AX_LINE_MAX], name[96];
    char dir8[AX_PATH_MAX], sys8[MAX_PATH * 3], cmd8[MAX_PATH * 3], d8[AX_LINE_MAX], n8[96];
    if (w_cmd_path(sys, cmdw, sys8, sizeof sys8, cmd8, sizeof cmd8) || to_utf8(w->self_dir, dir8, sizeof dir8)) return -1;
    if (ax_runonce_data(cmd8, dir8, d8, sizeof d8) || to_wide(d8, data, AX_LINE_MAX)) return -1;
    /* its own value per temp folder: a pending one is never replaced (review UN13) */
    if (ax_runonce_name(w->name, n8, sizeof n8) || to_wide(n8, name, 96)) return -1;
    if (!ax_runonce_fits(d8)) return -1;   /* RunOnce runs no command longer than 260 characters (reviews UN12, UQ5) */
    static const wchar_t key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return -1;
    DWORD bytes = (DWORD)((wcslen(data) + 1) * sizeof(wchar_t)), got = sizeof back, type = 0;
    int ok = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)data, bytes) == ERROR_SUCCESS
             && RegQueryValueExW(k, name, NULL, &type, (BYTE *)back, &got) == ERROR_SUCCESS && type == REG_SZ && got == bytes
             && !memcmp(back, data, bytes);
    RegCloseKey(k);
    return ok ? 1 : -1;
}

/* the temp folder after this process: cmd.exe (Windows' own, from System32) with no window,
 * deleting it once this process is gone, by its relative name from %TEMP% (ax_cleanup_args);
 * else HKCU RunOnce (w_runonce). Only after self_ok (w->name checked). */
static int w_clean_later(void *ctx) {
    win_ax *w = ctx;
    static wchar_t sys[MAX_PATH], cmdw[MAX_PATH + 16], linew[AX_LINE_MAX];
    char dir8[AX_PATH_MAX], parent8[AX_PATH_MAX], sys8[MAX_PATH * 3], cmd8[MAX_PATH * 3], args[AX_LINE_MAX], line[AX_LINE_MAX];
    if (w_cmd_path(sys, cmdw, sys8, sizeof sys8, cmd8, sizeof cmd8) || to_utf8(w->self_dir, dir8, sizeof dir8)
        || to_utf8(w->parent, parent8, sizeof parent8))
        return -1;
    /* cmd's working folder: the temp folder's parent (%TEMP%), so it never holds the folder it
     * deletes and only ever sees its relative name. cmd takes no UNC working folder (it would run
     * in %SystemRoot%): RunOnce then (review UQ4) */
    if (ax_cmd_cwd_ok(parent8) && !ax_cleanup_args(sys8, w->name, args, sizeof args)) {
        int k = snprintf(line, sizeof line, "\"%s\" %s", cmd8, args);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        if (k > 0 && (size_t)k < sizeof line && !to_wide(line, linew, AX_LINE_MAX)
            && CreateProcessW(cmdw, linew, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, w->parent, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 0;
        }
    }
    /* the fallback: RunOnce at the next logon */
    return w_runonce(w);
}

static uint64_t w_now_ms(void *ctx) {
    (void)ctx;
    return GetTickCount64();
}
static void w_sleep_ms(void *ctx, uint32_t ms) {
    (void)ctx;
    Sleep(ms);
}
/* no log file is left behind: the lines go to the debugger's output (DebugView in the VM) */
static void w_log(void *ctx, const char *line) {
    (void)ctx;
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}

/* the kept dialog (afterexit.h's kept_dialog): a plain MessageBoxW in this copy's session, the
 * user's (the app started it), its words from the core's Windows column; a text that can't be had
 * is logged, never replaced */
static void w_kept_dialog(void *ctx) {
    static char msg[1024], app[64];
    static wchar_t wm[1024], wa[64];
    size_t len;
    if (core_text(CORE_OS_WIN, T_WIN_UNINSTALL_KEPT, NULL, 0, msg, sizeof msg, &len) != CORE_OK
        || core_text(CORE_OS_WIN, T_APP_NAME, NULL, 0, app, sizeof app, &len) != CORE_OK || to_wide(msg, wm, 1024) || to_wide(app, wa, 64)) {
        w_log(ctx, "after-exit: the kept dialog's text couldn't be had: not shown");
        return;
    }
    /* topmost: it opens 60 s or more after the app quit, its starter gone, and the foreground lock
     * can defeat MB_SETFOREGROUND (it would stay behind Settings) */
    if (!MessageBoxW(NULL, wm, wa, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST)) {
        DWORD e = GetLastError();
        char l[96];
        snprintf(l, sizeof l, "after-exit: the kept dialog couldn't be shown (error %lu)", (unsigned long)e);
        w_log(ctx, l);
    }
}

CORE_API int32_t CORE_CALL core_win_after_exit(const char *args) {
    ax_args a;
    if (!args || ax_parse(args, &a)) return AX_BAD_ARGS;
    static win_ax w;
    memset(&w, 0, sizeof w);
    w.u = update_os_windows();
    DWORD n = GetModuleFileNameW(NULL, w.self_dir, AX_PATH_MAX);
    if (!w.u || n == 0 || n >= AX_PATH_MAX) return AX_BAD_ARGS;
    wchar_t *cut = wcsrchr(w.self_dir, L'\\');
    if (!cut) return AX_BAD_ARGS;
    *cut = 0;
    cut = wcsrchr(w.self_dir, L'\\');   /* the temp folder's parent, its '\\' kept */
    if (!cut || (size_t)(cut - w.self_dir) + 2 > AX_PATH_MAX) return AX_BAD_ARGS;
    wmemcpy(w.parent, w.self_dir, (size_t)(cut - w.self_dir) + 1);
    w.parent[cut - w.self_dir + 1] = 0;
    /* ax_run checks self_ok and home before anything (reviews UN1, UN11) */
    ax_os os = {w_self_ok,    w_home,           w_wait_parent, w_stop_watchers, w_running_under, w_remove_tree,
                w_clean_later, w_remove_run,    w_remove_records, w_now_ms,    w_sleep_ms,      w_log,
                w_runonce,     w_kept_dialog,   &w};
    return ax_run(&os, &a);
}
