/* rbsp-watch.exe (app.md §5.7): the resident watcher's Win32 side. watch.c decides; here are its
 * operations: ReadDirectoryChangesW on handles opened with every share mode (so rekordbox's
 * updater and uninstaller are never blocked), the main loop, the check's thread, the dialog
 * (TaskDialogIndirect, comctl32 v6 through the manifest), the notification (Shell_NotifyIconW with
 * NIF_INFO), RtlGetVersion and the .NET Framework release, the stop event the updater sets
 * (app.md §5.4 "As built"), the one-copy mutex and the log.
 *
 * Built with -municode -mwindows, x64 and arm64, the core's watcher decisions and plat_win.c
 * linked statically. Imports (watch.sh's gate): KERNEL32, ADVAPI32, USER32, SHELL32, ole32,
 * COMCTL32 and the UCRT; bcrypt (the payload's signature, the files' sha256) and wintrust and
 * crypt32 (the AlphaTheta pin) are loaded from System32 at their first use by the modules that
 * use them. Unverified until the VM run (W13a): every call here. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include "watch.h"
#include "core.h"   /* core_text: the dialog's window title */
#include "plat_win.h"
#include "win_rules.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#  define ARCH "arm64"
#else
#  define ARCH "x64"
#endif
#ifndef RBSP_VERSION
#  define RBSP_VERSION "0.0.0"
#endif

/* a note is at least 12 bytes (FILE_NOTIFY_INFORMATION with an empty name), DWORD-aligned: a
 * read's every note fits; each UTF-16 unit is at most 3 UTF-8 bytes */
enum { BUF = 32 * 1024, NOTES_MAX = BUF / 12 + 1, NAMES_MAX = BUF / 2 * 3 + NOTES_MAX, WM_TRAY = WM_APP + 1, WM_NOTIFY_SHOW = WM_APP + 2, TRAY_ID = 1 };

/* ------------------------------------------------------------------ UTF-8 and UTF-16 */

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
        free(w);
        w = NULL;
    }
    return w;
}

static int utf8(const wchar_t *w, int wn, char *out, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wn, out, cap - 1, NULL, NULL);
    if (n <= 0) {
        if (cap) out[0] = 0;
        return 0;
    }
    out[n] = 0;
    return 1;
}

/* ------------------------------------------------------------------ the log */

static SRWLOCK log_lock = SRWLOCK_INIT;
static wchar_t log_path[WATCH_PATH_MAX];

static void wlog(void *ctx, const char *line) {
    (void)ctx;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[4200];
    size_t n = watch_log_line(b, sizeof b, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, line);
    AcquireSRWLockExclusive(&log_lock);
    /* appended, never through a link (appendLog's O_NOFOLLOW) */
    HANDLE h = log_path[0] ? CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL)
                           : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, b, (DWORD)n, &w, NULL);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&log_lock);
}

/* ------------------------------------------------------------------ the folders */

typedef struct {
    HANDLE dir, ev;
    OVERLAPPED ov;
    int armed;       /* a read is outstanding */
    DWORD filter;
    BOOL subtree;
    int has_id;      /* the folder's identity could be read (FILE_READ_ATTRIBUTES) */
    DWORD vol, idx_hi, idx_lo;
    DWORD *buf;      /* DWORD-aligned, BUF bytes */
} slot_t;

static slot_t slots[WATCH_SLOTS];
static watch_engine engine;
static watch_sched sched;
static SRWLOCK state_lock = SRWLOCK_INIT;   /* the engine and sched, shared with the check's thread */

static int arm(slot_t *s) {
    ResetEvent(s->ev);
    memset(&s->ov, 0, sizeof s->ov);
    s->ov.hEvent = s->ev;
    if (!ReadDirectoryChangesW(s->dir, s->buf, BUF, s->subtree, s->filter, NULL, &s->ov, NULL)) return (int)GetLastError();
    s->armed = 1;
    return 0;
}

static int errno_of(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DIRECTORY:
    case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED: return EACCES;
    default: return EIO;
    }
}

static HANDLE open_dir(const wchar_t *w, DWORD access) {
    return CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
}

static int f_open(void *ctx, int i, const char *path, int subtree, int filter) {
    (void)ctx;
    slot_t *s = &slots[i];
    wchar_t *w = wide(path);
    if (!w) return ENOENT;
    /* every share mode: rekordbox's updater and uninstaller (and our worker) may delete or rename
     * it; FILE_READ_ATTRIBUTES for its identity, where the folder's ACL grants it (our admin-only
     * root may give Users only the listing) */
    s->dir = open_dir(w, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    if (s->dir == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) s->dir = open_dir(w, FILE_LIST_DIRECTORY);
    free(w);
    if (s->dir == INVALID_HANDLE_VALUE) {
        s->dir = NULL;
        return errno_of(GetLastError());
    }
    BY_HANDLE_FILE_INFORMATION info;
    s->has_id = GetFileInformationByHandle(s->dir, &info);
    if (s->has_id && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(s->dir);
        s->dir = NULL;
        return ENOENT;
    }
    s->vol = s->has_id ? info.dwVolumeSerialNumber : 0;
    s->idx_hi = s->has_id ? info.nFileIndexHigh : 0;
    s->idx_lo = s->has_id ? info.nFileIndexLow : 0;
    s->subtree = subtree ? TRUE : FALSE;
    s->filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    if (filter & WATCH_F_WRITES) s->filter |= FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    s->armed = 0;
    int e = arm(s);
    if (e) {
        CloseHandle(s->dir);
        s->dir = NULL;
        return errno_of((DWORD)e);
    }
    return 0;
}

static void f_close(void *ctx, int i) {
    (void)ctx;
    slot_t *s = &slots[i];
    if (!s->dir) return;
    /* an outstanding read is cancelled and waited for, so the buffer is never written after this */
    DWORD got;
    if (s->armed) {
        /* waited for whatever CancelIoEx answers: a read that completed meanwhile has signalled */
        CancelIoEx(s->dir, &s->ov);
        GetOverlappedResult(s->dir, &s->ov, &got, TRUE);
    }
    s->armed = 0;
    CloseHandle(s->dir);
    s->dir = NULL;
    ResetEvent(s->ev);
}

static int f_same(void *ctx, int i, const char *path) {
    (void)ctx;
    slot_t *s = &slots[i];
    if (!s->dir) return 0;
    FILE_STANDARD_INFO st;
    if (GetFileInformationByHandleEx(s->dir, FileStandardInfo, &st, sizeof st) && st.DeletePending) return 0;
    wchar_t *w = wide(path);
    if (!w) return 0;
    int same;
    if (!s->has_id) {
        /* no identity to compare: still a folder at that path */
        DWORD a = GetFileAttributesW(w);
        same = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    } else {
        HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        same = h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &info) && info.dwVolumeSerialNumber == s->vol &&
               info.nFileIndexHigh == s->idx_hi && info.nFileIndexLow == s->idx_lo;
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    free(w);
    return same;
}

static const watch_fs win_fs = {NULL, f_open, f_close, f_same, wlog};

/* A completed read on slot i: its notes to the engine, then the next read armed (unless the
 * engine closed the slot, or opened it anew, which arms its own read). The buffer is parsed whole
 * before the engine sees anything: the engine may close the handle and open another. */
static void on_read(int i) {
    slot_t *s = &slots[i];
    DWORD got = 0;
    int status = WATCH_READ_OK;
    if (!GetOverlappedResult(s->dir, &s->ov, &got, FALSE)) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_INCOMPLETE) return;
        status = e == ERROR_NOTIFY_ENUM_DIR ? WATCH_READ_OVERFLOW
               : e == ERROR_ACCESS_DENIED || e == ERROR_INVALID_HANDLE || e == ERROR_NETNAME_DELETED ? WATCH_READ_GONE
                                                                                                       : WATCH_READ_ERROR;
    } else if (got == 0) {
        status = WATCH_READ_OVERFLOW;   /* more than the buffer holds: what changed is unknown */
    }
    s->armed = 0;
    static watch_note notes[NOTES_MAX];
    static char names[NAMES_MAX];
    int n = 0, more = 0;
    size_t used = 0;
    if (status == WATCH_READ_OK) {
        const BYTE *p = (const BYTE *)s->buf, *end = p + got;
        while ((size_t)(end - p) >= offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
            const FILE_NOTIFY_INFORMATION *fi = (const FILE_NOTIFY_INFORMATION *)(const void *)p;
            if (fi->FileNameLength > (size_t)(end - (const BYTE *)fi->FileName)) break;
            size_t room = NAMES_MAX - used < WATCH_PATH_MAX ? NAMES_MAX - used : WATCH_PATH_MAX;
            if (n == NOTES_MAX || room < 2) more = 1;   /* never with the sizes above: the folder's every flag */
            else if (utf8(fi->FileName, (int)(fi->FileNameLength / sizeof(WCHAR)), names + used, (int)room)) {
                notes[n].action = (int)fi->Action;
                notes[n].name = names + used;
                used += strlen(names + used) + 1;
                n++;
            }
            if (!fi->NextEntryOffset || fi->NextEntryOffset > (size_t)(end - p)) break;
            p += fi->NextEntryOffset;
        }
    }
    AcquireSRWLockExclusive(&state_lock);
    if (more) {
        char line[WATCH_PATH_MAX + 32];
        snprintf(line, sizeof line, "%s: too many changes to list", engine.path[i % WD_COUNT]);
        wlog(NULL, line);
    }
    int flags = more ? watch_kind_flags(i % WD_COUNT) : 0;
    flags |= watch_engine_read(&engine, i, status, notes, n);
    if (watch_engine_slot_open(&engine, i) && !s->armed && arm(s))
        flags |= watch_engine_read(&engine, i, WATCH_READ_GONE, NULL, 0);   /* it can't be read again: gone */
    watch_sched_note(&sched, flags);
    ReleaseSRWLockExclusive(&state_lock);
}

/* ------------------------------------------------------------------ the check's operations */

static watch_env env;
static watch_reader reader;
static watch_state wstate;
static SRWLOCK act_lock = SRWLOCK_INIT;   /* an answer's writes: never cut short by the stop event */
static HWND tray_hwnd;
static wchar_t self_path[WATCH_PATH_MAX];

static int o_facts(void *c, watch_facts *f) {
    (void)c;
    return watch_read_facts(&env, &reader, f);
}

static int procs(compat_procs *p) { return compat_processes(env.os, NULL, p); }
static int32_t o_updating(void *c) {
    (void)c;
    compat_procs p;
    return procs(&p) == 0 && p.updating;
}
static int32_t o_open(void *c) {
    (void)c;
    compat_procs p;
    return procs(&p) == 0 && p.running;
}
static int32_t o_busy(void *c) {
    (void)c;
    return watch_app_busy(&env);
}
static int32_t o_half(void *c, char *id, size_t cap) {
    (void)c;
    return watch_half_uninstalled(&env, id, cap);
}
static void o_pause(void *c, int32_t s) {
    (void)c;
    Sleep((DWORD)s * 1000u);
}
/* the prepared playlists' check comes with Prepare (rbsp-prepare.exe isn't in the first release) */
static void o_session(void *c) { (void)c; }
static int o_take(void *c) {
    (void)c;
    AcquireSRWLockExclusive(&state_lock);
    int f = watch_sched_take(&sched);
    ReleaseSRWLockExclusive(&state_lock);
    return f;
}
static void o_give_back(void *c, int flags) {
    (void)c;
    AcquireSRWLockExclusive(&state_lock);
    watch_sched_note(&sched, flags);
    ReleaseSRWLockExclusive(&state_lock);
}

static HRESULT CALLBACK td_callback(HWND h, UINT msg, WPARAM w, LPARAM l, LONG_PTR ref) {
    (void)w, (void)l, (void)ref;
    if (msg == TDN_CREATED) SetForegroundWindow(h);   /* refused in the background: the taskbar flashes */
    return S_OK;
}

static int o_ask(void *c, const char *title, const char *message, const char *const *buttons, int n) {
    (void)c;
    char app[64];
    size_t len;
    if (core_text(CORE_OS_WIN, T_APP_NAME, NULL, 0, app, sizeof app, &len) != CORE_OK) return -1;
    wchar_t *wt = wide(title), *wm = wide(message), *wa = wide(app), *wb[3] = {NULL, NULL, NULL};
    TASKDIALOG_BUTTON b[3];
    int ok = wt && wm && wa && n > 0 && n <= 3;
    for (int i = 0; ok && i < n; i++) {
        wb[i] = wide(buttons[i]);
        b[i].nButtonID = 100 + i;
        b[i].pszButtonText = wb[i];
        ok = wb[i] != NULL;
    }
    int pressed = -1;
    if (ok) {
        TASKDIALOGCONFIG cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.cbSize = sizeof cfg;
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        cfg.pszWindowTitle = wa;
        cfg.pszMainInstruction = wt;
        cfg.pszContent = wm;
        cfg.pButtons = b;
        cfg.cButtons = (UINT)n;
        cfg.nDefaultButton = 100;
        cfg.pfCallback = td_callback;
        int id = 0;
        HRESULT hr = TaskDialogIndirect(&cfg, &id, NULL, NULL);
        if (SUCCEEDED(hr) && id >= 100 && id < 100 + n) pressed = id - 100;
        else if (FAILED(hr)) wlog(NULL, "the dialog couldn't be shown");
    }
    for (int i = 0; i < 3; i++) free(wb[i]);
    free(wt);
    free(wm);
    free(wa);
    return pressed;
}

static int o_remember(void *c, const char *t, size_t n) {
    (void)c;
    AcquireSRWLockExclusive(&act_lock);
    int e = watch_write_support(&env, "dismissed", t, n);
    ReleaseSRWLockExclusive(&act_lock);
    return e;
}

/* RB Stems Plus through this install's start stub; act_lock held by the caller. After "Reinstall
 * Now" this process holds the foreground right the user's click gave its dialog, and passes it on.
 * On R1 there was no dialog: the watcher has no foreground right to pass, so the app's window may
 * open behind the active one (a W13a check, at logon and mid-session). */
static int open_app_locked(void) {
    char self8[WATCH_PATH_MAX], stub8[WATCH_PATH_MAX];
    if (!utf8(self_path, -1, self8, sizeof self8) || !watch_stub_path(self8, stub8, sizeof stub8)) return ENOENT;
    wchar_t *stub = wide(stub8);
    if (!stub) return ENOENT;
    wchar_t line[WATCH_PATH_MAX + 3], dir[WATCH_PATH_MAX];
    _snwprintf(line, sizeof line / sizeof line[0], L"\"%ls\"", stub);
    line[sizeof line / sizeof line[0] - 1] = 0;
    wcsncpy(dir, stub, sizeof dir / sizeof dir[0] - 1);
    dir[sizeof dir / sizeof dir[0] - 1] = 0;
    wchar_t *cut = wcsrchr(dir, L'\\');
    if (cut) *cut = 0;
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(stub, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi);
    if (ok) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    free(stub);
    return ok ? 0 : errno_of(GetLastError());
}

static int open_app(void) {
    AcquireSRWLockExclusive(&act_lock);
    int e = open_app_locked();
    ReleaseSRWLockExclusive(&act_lock);
    return e;
}

/* "Reinstall Now": the request, then the app, under one hold of act_lock (SRW locks aren't
 * recursive: open_app_locked takes nothing) */
static int o_reinstall(void *c) {
    (void)c;
    AcquireSRWLockExclusive(&act_lock);
    int r = watch_write_support(&env, "request", "reinstall\n", 10) ? WATCH_NO_REQUEST : open_app_locked() ? WATCH_NO_APP : 0;
    ReleaseSRWLockExclusive(&act_lock);
    return r;
}

static int o_open_app(void *c) {
    (void)c;
    return open_app();
}

static void o_current(void *c, const char *name) {
    (void)c;
    AcquireSRWLockExclusive(&state_lock);
    watch_engine_set_current(&engine, name);
    ReleaseSRWLockExclusive(&state_lock);
}

static const watch_ops ops = {NULL, CORE_OS_WIN, o_facts, o_updating, o_busy, o_open, o_half, o_pause, o_session, o_take, o_give_back,
                              o_ask, o_remember, o_reinstall, o_open_app, o_current, wlog};

static HANDLE rounds_done;

static DWORD WINAPI rounds_thread(LPVOID p) {
    (void)p;
    watch_rounds(&ops, &wstate);
    SetEvent(rounds_done);
    return 0;
}

/* ------------------------------------------------------------------ the notification */

typedef struct {
    wchar_t title[64], body[256];
} notice;

int watch_notify(const char *title, const char *body) {
    notice *n = calloc(1, sizeof *n);
    if (!n || !tray_hwnd) {
        free(n);
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, title, -1, n->title, (int)(sizeof n->title / sizeof n->title[0]) - 1);
    MultiByteToWideChar(CP_UTF8, 0, body, -1, n->body, (int)(sizeof n->body / sizeof n->body[0]) - 1);
    if (!PostMessageW(tray_hwnd, WM_NOTIFY_SHOW, 0, (LPARAM)n)) {
        free(n);
        return -1;
    }
    return 0;
}

static void tray_remove(HWND h) {
    NOTIFYICONDATAW d;
    memset(&d, 0, sizeof d);
    d.cbSize = sizeof d;
    d.hWnd = h;
    d.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &d);
}

static LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NOTIFY_SHOW) {
        notice *n = (notice *)l;
        NOTIFYICONDATAW d;
        memset(&d, 0, sizeof d);
        d.cbSize = sizeof d;
        d.hWnd = h;
        d.uID = TRAY_ID;
        d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
        d.uCallbackMessage = WM_TRAY;
        d.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
        if (!d.hIcon) d.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
        wcsncpy(d.szTip, n->title, sizeof d.szTip / sizeof d.szTip[0] - 1);
        wcsncpy(d.szInfoTitle, n->title, sizeof d.szInfoTitle / sizeof d.szInfoTitle[0] - 1);
        wcsncpy(d.szInfo, n->body, sizeof d.szInfo / sizeof d.szInfo[0] - 1);
        d.dwInfoFlags = NIIF_INFO;
        tray_remove(h);
        if (Shell_NotifyIconW(NIM_ADD, &d)) {
            d.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &d);
        } else {
            wlog(NULL, "the notification couldn't be shown");
        }
        free(n);
        return 0;
    }
    if (msg == WM_TRAY) {
        switch (LOWORD(l)) {
        case NIN_BALLOONUSERCLICK:
            /* clicked: RB Stems Plus says why (the Mac's notification opens the app too) */
            tray_remove(h);
            if (open_app()) wlog(NULL, "couldn't open RB Stems Plus");
            break;
        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONHIDE: tray_remove(h); break;
        default: break;
        }
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ------------------------------------------------------------------ start */

typedef LONG(WINAPI *rtl_get_version_fn)(OSVERSIONINFOW *);

/* RtlGetVersion: what the system is, whatever the manifest says (I73); 0 unknown */
static DWORD windows_build(void) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");   /* always mapped: no load */
    rtl_get_version_fn get = nt ? (rtl_get_version_fn)(void (*)(void))GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    return get && get(&v) == 0 ? v.dwBuildNumber : 0;
}

/* the .NET Framework 4.x release (NDP v4 Full's Release, the 64-bit view); 0 none */
static DWORD net_release(void) {
    HKEY k;
    DWORD v = 0, size = sizeof v;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k))
        return 0;
    if (RegGetValueW(k, NULL, L"Release", RRF_RT_REG_DWORD, NULL, &v, &size) != ERROR_SUCCESS) v = 0;
    RegCloseKey(k);
    return v;
}

/* a known folder, in UTF-8 */
static int known(REFKNOWNFOLDERID id, const char *test_env, char *out, size_t cap) {
#ifdef WATCH_TEST_PATHS
    /* test builds only: the VM's fake trees (app.md §7) */
    const char *t = getenv(test_env);
    if (t && *t) {
        snprintf(out, cap, "%s", t);
        return 1;
    }
#else
    (void)test_env;
#endif
    PWSTR w = NULL;
    int ok = SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, NULL, &w)) && utf8(w, -1, out, (int)cap);
    CoTaskMemFree(w);
    return ok;
}

static int64_t process_start(void *ctx, int32_t pid) {
    (void)ctx;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return -1;
    FILETIME c, e, k, u;
    int64_t r = -1;
    if (GetProcessTimes(h, &c, &e, &k, &u)) {
        DWORD code;
        uint64_t t = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
        /* a process that has exited but whose handle is still held is no writer */
        if (!GetExitCodeProcess(h, &code) || code == STILL_ACTIVE) r = (int64_t)((t - 116444736000000000ull) / 10000000ull);
    }
    CloseHandle(h);
    return r;
}

static char p_rekordbox[WATCH_PATH_MAX], p_root[WATCH_PATH_MAX], p_pioneer[WATCH_PATH_MAX], p_models[WATCH_PATH_MAX],
    p_support[WATCH_PATH_MAX];
static sig_pubkey keys[8];

/* The folders below Pioneer\ that count (review VF3): one that is a junction or a symbolic link
 * isn't seen through Pioneer\'s subtree watch (changes inside a link's target aren't reported
 * there). Looked at when the watcher starts and at every minute's tick; logged once each time one
 * becomes a link. */
static void links_tick(void) {
    static int was[3];
    char p[3][WATCH_PATH_MAX + 32];
    snprintf(p[0], sizeof p[0], "%s\\" WATCH_SETTINGS_BELOW, p_pioneer);
    snprintf(p[1], sizeof p[1], "%s\\" WATCH_SETTINGS_BELOW "\\models", p_pioneer);
    snprintf(p[2], sizeof p[2], "%s", p_models);
    for (int i = 0; i < 3; i++) {
        wchar_t *w = wide(p[i]);
        DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
        free(w);
        int link = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
        if (link && !was[i]) {
            char line[WATCH_PATH_MAX + 64];
            snprintf(line, sizeof line, "%s is a link: changes inside it aren't seen", p[i]);
            wlog(NULL, line);
        }
        was[i] = link;
    }
}

static int paths(void) {
    char roaming[WATCH_PATH_MAX], local[WATCH_PATH_MAX];
    /* rekordbox's folder and our root under Windows' own Program Files (plat_program_files: the
     * worker's, the core DLL's and the bridge's rule; its test override is RBSP_TEST_PROGRAMS in
     * a test build); unreadable, the watcher doesn't start */
    if (plat_program_files(WR_PF_REKORDBOX, p_rekordbox, sizeof p_rekordbox) || plat_program_files(WR_PF_ROOT, p_root, sizeof p_root) ||
        !known(&FOLDERID_RoamingAppData, "RBSP_TEST_APPDATA", roaming, sizeof roaming) ||
        !known(&FOLDERID_LocalAppData, "RBSP_TEST_LOCALAPPDATA", local, sizeof local))
        return 0;
    int k = snprintf(p_pioneer, sizeof p_pioneer, "%s\\Pioneer", roaming);
    k |= snprintf(p_models, sizeof p_models, "%s\\" WATCH_MODELS_BELOW, p_pioneer);
    k |= snprintf(p_support, sizeof p_support, "%s\\rbstemsplus", local);
    if (k < 0 || strlen(p_models) >= WATCH_PATH_MAX - 1 || strlen(p_support) >= WATCH_PATH_MAX - 20) return 0;
    char logs[WATCH_PATH_MAX + 32];
    snprintf(logs, sizeof logs, "%s\\Logs", p_support);
    wchar_t *ws = wide(p_support), *w = wide(logs);
    if (!w || !ws) {
        free(w);
        free(ws);
        return 0;
    }
    /* the app makes them; one start before it ever ran still logs (KERNEL32 only: no
     * SHCreateDirectoryExW) */
    CreateDirectoryW(ws, NULL);
    CreateDirectoryW(w, NULL);
    free(ws);
    _snwprintf(log_path, sizeof log_path / sizeof log_path[0], L"%ls\\watcher.log", w);
    log_path[sizeof log_path / sizeof log_path[0] - 1] = 0;
    free(w);
    return 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev, (void)cmd, (void)show;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    DWORD sn = GetModuleFileNameW(NULL, self_path, sizeof self_path / sizeof self_path[0]);
    if (sn == 0 || sn >= sizeof self_path / sizeof self_path[0]) return 1;
    if (!paths()) return 1;
    char line[256];
    snprintf(line, sizeof line, "start: RB Stems Plus Watcher %s (%s), pid %lu", RBSP_VERSION, ARCH, (unsigned long)GetCurrentProcessId());
    wlog(NULL, line);

    /* the minimum (app.md §5.9): refused in one line */
    const char *no = watch_os_refusal(windows_build(), net_release());
    if (no) {
        wlog(NULL, no);
        return 0;
    }
    /* one watcher per session (a second start, from the Run value or the updater, stops) */
    HANDLE one = CreateMutexW(NULL, FALSE, L"Local\\io.github.rbstemsplus.watcher");
    if (!one || GetLastError() == ERROR_ALREADY_EXISTS) {
        wlog(NULL, "another watcher is running: this one stops");
        return 0;
    }
    /* the updater's stop (W3a): a manual-reset event named for this pid */
    char evname[96];
    wchar_t wev[96];
    HANDLE stop = NULL;
    if (watch_stop_event_name(GetCurrentProcessId(), evname, sizeof evname) &&
        MultiByteToWideChar(CP_UTF8, 0, evname, -1, wev, (int)(sizeof wev / sizeof wev[0])) > 0)
        stop = CreateEventW(NULL, TRUE, FALSE, wev);
    if (!stop) wlog(NULL, "the stop event couldn't be made: an update ends this watcher instead");

    int nk = 0;
    for (int i = 0; i < watch_n_compiled_keys && nk < 8; i++)
        if (!sig_key_from_text(&watch_compiled_keys[i], &keys[nk])) nk++;
    watch_env e = {compat_os_windows(), p_rekordbox, p_root, p_models, p_support, keys, (size_t)nk, RBSP_VERSION, process_start, wlog,
                   NULL};
    env = e;

    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"io.github.rbstemsplus.watcher";
    if (RegisterClassW(&wc)) tray_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
#ifdef WATCH_TEST_PATHS
    /* test builds only: the notification on its own (VM check) */
    if (wcsstr(GetCommandLineW(), L"--notify-test")) watch_notify("RB Stems Plus", "A notification from the watcher's test build.");
#endif

    for (int i = 0; i < WATCH_SLOTS; i++) {
        slots[i].ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        slots[i].buf = malloc(BUF);
        if (!slots[i].ev || !slots[i].buf) {
            wlog(NULL, "not running: out of memory");
            return 1;
        }
    }
    rounds_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    const char *paths_[WD_COUNT];
    paths_[WD_REKORDBOX] = p_rekordbox;
    paths_[WD_PIONEER] = p_pioneer;
    paths_[WD_ROOT] = p_root;
    AcquireSRWLockExclusive(&state_lock);
    watch_engine_init(&engine, &win_fs, paths_);
    watch_engine_tick(&engine);
    /* a check at start (a logon, or the update that started this watcher): R1's included */
    watch_sched_note(&sched, WATCH_ROUND | WATCH_R1);
    ReleaseSRWLockExclusive(&state_lock);
    links_tick();
    ULONGLONG next_tick = GetTickCount64() + WATCH_TICK_MS;

    for (;;) {
        HANDLE hs[2 + WATCH_SLOTS];
        int map[2 + WATCH_SLOTS], nh = 0;
        if (stop) map[nh] = -1, hs[nh++] = stop;
        map[nh] = -2, hs[nh++] = rounds_done;
        AcquireSRWLockExclusive(&state_lock);
        for (int i = 0; i < WATCH_SLOTS; i++)
            if (watch_engine_slot_open(&engine, i)) map[nh] = i, hs[nh++] = slots[i].ev;
        int64_t wait = -1;
        ULONGLONG now = GetTickCount64();
        if (watch_sched_poll(&sched, (int64_t)now, &wait)) {
            HANDLE t = CreateThread(NULL, 0, rounds_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
            else {
                watch_sched_done(&sched);
                wlog(NULL, "the check couldn't start");
            }
        }
        ReleaseSRWLockExclusive(&state_lock);
        DWORD timeout = next_tick > now ? (DWORD)(next_tick - now) : 0;
        if (wait >= 0 && (DWORD)wait < timeout) timeout = (DWORD)wait;
        DWORD r = MsgWaitForMultipleObjectsEx((DWORD)nh, hs, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r < WAIT_OBJECT_0 + (DWORD)nh) {
            int which = map[r - WAIT_OBJECT_0];
            if (which == -1) {
                /* the updater stops this watcher: an answer being written is finished first */
                wlog(NULL, "stopped: RB Stems Plus is being updated or removed");
                AcquireSRWLockExclusive(&act_lock);
                /* nothing is buffered: ended at once, without running other threads' detach code */
                TerminateProcess(GetCurrentProcess(), 0);
            }
            if (which == -2) {
                AcquireSRWLockExclusive(&state_lock);
                watch_sched_done(&sched);
                ReleaseSRWLockExclusive(&state_lock);
            } else {
                on_read(which);
            }
        } else if (r == WAIT_OBJECT_0 + (DWORD)nh) {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        } else if (r == WAIT_FAILED) {
            Sleep(1000);
        }
        if (GetTickCount64() >= next_tick) {
            AcquireSRWLockExclusive(&state_lock);
            watch_sched_note(&sched, watch_engine_tick(&engine));
            ReleaseSRWLockExclusive(&state_lock);
            links_tick();
            next_tick = GetTickCount64() + WATCH_TICK_MS;
        }
    }
}
