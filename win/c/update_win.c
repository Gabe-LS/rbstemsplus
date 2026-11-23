/* update_net and update_os on Windows (update.h): WinHTTP downloads, processes, the watcher's stop
 * event, a folder tree's deletion and the Apps & features DisplayVersion. KERNEL32, ADVAPI32
 * (the registry) and USER32 (AllowSetForegroundWindow) are imported, and plat_win.c's own (ntdll,
 * SHELL32, ole32: the core DLL's allowlist) for the tree's deletion; winhttp.dll isn't a KnownDLL,
 * so it is loaded at the first download by its full System32 path with LOAD_LIBRARY_SEARCH_SYSTEM32
 * (app.md §7's allowlist), and no WinHttp function is referenced directly (run.sh's llvm-nm gate).
 * In the core DLL build only (CORE_NATIVE_WIN), the WPF shell's entries too (update_shell.h): the
 * update's start and --swap, after the first render, the minimum, the AppUserModelID, the flow's
 * quit and the device events. Unverified until the VM run (W13a): every call here. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <winternl.h>

#include "aumid.h"
#include "plat_win.h"
#include "update.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(UPDATE_ERR_ACCESS_DENIED == ERROR_ACCESS_DENIED && UPDATE_ERR_INVALID_PARAMETER == ERROR_INVALID_PARAMETER,
               "update_open_skips' codes are Win32's");

/* ------------------------------------------------------------------ UTF-8 and UTF-16 */

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static int narrow(const wchar_t *w, char *out, size_t cap) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, (int)cap, NULL, NULL);
    if (n <= 0) {
        if (cap) out[0] = 0;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ WinHTTP, loaded at run time */

typedef HINTERNET(WINAPI *open_fn)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef BOOL(WINAPI *crack_fn)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
typedef HINTERNET(WINAPI *connect_fn)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI *request_fn)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL(WINAPI *setopt_fn)(HINTERNET, DWORD, LPVOID, DWORD);
typedef BOOL(WINAPI *queryopt_fn)(HINTERNET, DWORD, LPVOID, LPDWORD);
typedef BOOL(WINAPI *timeouts_fn)(HINTERNET, int, int, int, int);
typedef BOOL(WINAPI *send_fn)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL(WINAPI *receive_fn)(HINTERNET, LPVOID);
typedef BOOL(WINAPI *headers_fn)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *avail_fn)(HINTERNET, LPDWORD);
typedef BOOL(WINAPI *read_fn)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI *close_fn)(HINTERNET);

static struct {
    open_fn open;
    crack_fn crack;
    connect_fn connect;
    request_fn request;
    setopt_fn setopt;
    queryopt_fn queryopt;
    timeouts_fn timeouts;
    send_fn send;
    receive_fn receive;
    headers_fn headers;
    avail_fn avail;
    read_fn read;
    close_fn close;
} wh;
static INIT_ONCE wh_once = INIT_ONCE_STATIC_INIT;

#define PROC(m, name) ((void (*)(void))GetProcAddress((m), (name)))

static BOOL WINAPI wh_load(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once, (void)param, (void)ctx;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    static const wchar_t file[] = L"\\winhttp.dll";
    if (n == 0 || n + sizeof file / sizeof file[0] > MAX_PATH) return TRUE;
    memcpy(path + n, file, sizeof file);
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return TRUE;
    open_fn o = (open_fn)PROC(m, "WinHttpOpen");
    crack_fn cr = (crack_fn)PROC(m, "WinHttpCrackUrl");
    connect_fn co = (connect_fn)PROC(m, "WinHttpConnect");
    request_fn rq = (request_fn)PROC(m, "WinHttpOpenRequest");
    setopt_fn so = (setopt_fn)PROC(m, "WinHttpSetOption");
    queryopt_fn qo = (queryopt_fn)PROC(m, "WinHttpQueryOption");
    timeouts_fn to = (timeouts_fn)PROC(m, "WinHttpSetTimeouts");
    send_fn se = (send_fn)PROC(m, "WinHttpSendRequest");
    receive_fn re = (receive_fn)PROC(m, "WinHttpReceiveResponse");
    headers_fn he = (headers_fn)PROC(m, "WinHttpQueryHeaders");
    avail_fn av = (avail_fn)PROC(m, "WinHttpQueryDataAvailable");
    read_fn rd = (read_fn)PROC(m, "WinHttpReadData");
    close_fn cl = (close_fn)PROC(m, "WinHttpCloseHandle");
    if (o && cr && co && rq && so && qo && to && se && re && he && av && rd && cl)
        wh.open = o, wh.crack = cr, wh.connect = co, wh.request = rq, wh.setopt = so, wh.queryopt = qo, wh.timeouts = to, wh.send = se,
        wh.receive = re, wh.headers = he, wh.avail = av, wh.read = rd, wh.close = cl;
    return TRUE;
}

/* a header's text as UTF-8 ("" when absent) */
static void header(HINTERNET req, DWORD which, char *out, size_t cap) {
    wchar_t buf[256];
    DWORD len = sizeof buf;
    out[0] = 0;
    if (wh.headers(req, which, WINHTTP_HEADER_NAME_BY_INDEX, buf, &len, WINHTTP_NO_HEADER_INDEX)) narrow(buf, out, cap);
}

static int win_get(void *ctx, const char *url, int64_t from, int64_t max, const update_sink *sink, int *status, int *err) {
    (void)ctx;
    *status = 0;
    *err = 0;
    if (strncmp(url, "https://", 8)) return UPDATE_NET_OFFLINE;   /* HTTPS only */
    if (from > max) return UPDATE_NET_TOO_BIG;
    InitOnceExecuteOnce(&wh_once, wh_load, NULL, NULL);
    if (!wh.open) return UPDATE_NET_OFFLINE;
    wchar_t *w = wide(url);
    if (!w) return UPDATE_NET_OFFLINE;
    wchar_t host[256];
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = sizeof host / sizeof host[0];
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    int r = UPDATE_NET_OFFLINE;
    HINTERNET session = NULL, conn = NULL, req = NULL;
    unsigned char *buf = NULL;
    if (!wh.crack(w, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) goto out;
    /* the path and the query, one string (both point into w) */
    size_t plen = uc.dwUrlPathLength + uc.dwExtraInfoLength;
    wchar_t *path = malloc((plen + 2) * sizeof *path);
    if (!path) goto out;
    if (plen) memcpy(path, uc.lpszUrlPath, plen * sizeof *path);
    else path[plen++] = L'/';
    path[plen] = 0;
    session = wh.open(L"" UPDATE_USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        wh.setopt(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        wh.setopt(session, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        /* name resolution, connect, send, receive: a stalled connection gives up after a minute, and
         * none waits past the sink's deadline */
        int t[4] = {15000, 15000, 30000, 60000};
        if (sink->deadline_ns) {
            uint64_t now = plat_now_mono_ns();
            int left = now >= sink->deadline_ns ? 1 : (sink->deadline_ns - now) / 1000000u > 60000u ? 60000 : (int)((sink->deadline_ns - now) / 1000000u);
            for (int i = 0; i < 4; i++)
                if (t[i] > left) t[i] = left > 0 ? left : 1;
        }
        wh.timeouts(session, t[0], t[1], t[2], t[3]);
        conn = wh.connect(session, host, uc.nPort, 0);
    }
    if (conn) req = wh.request(conn, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    free(path);
    if (!req) goto out;
    {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        wh.setopt(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    }
    wchar_t range[64];
    if (from > 0) {
        char r8[64];
        snprintf(r8, sizeof r8, "Range: bytes=%lld-\r\n", (long long)from);
        if (MultiByteToWideChar(CP_UTF8, 0, r8, -1, range, (int)(sizeof range / sizeof range[0])) <= 0) goto out;
    }
    if (!wh.send(req, from > 0 ? range : WINHTTP_NO_ADDITIONAL_HEADERS, from > 0 ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !wh.receive(req, NULL))
        goto out;
    /* where the redirects ended: https only */
    wchar_t final_url[2048];
    DWORD flen = sizeof final_url;
    if (!wh.queryopt(req, WINHTTP_OPTION_URL, final_url, &flen) || _wcsnicmp(final_url, L"https://", 8)) goto out;
    DWORD code = 0, clen = sizeof code;
    if (!wh.headers(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &clen, WINHTTP_NO_HEADER_INDEX))
        goto out;
    *status = (int)code;
    if (code != 200 && code != 206) {
        r = UPDATE_NET_HTTP;
        goto out;
    }
    char cr[128], cl[32];
    header(req, WINHTTP_QUERY_CONTENT_RANGE, cr, sizeof cr);
    header(req, WINHTTP_QUERY_CONTENT_LENGTH, cl, sizeof cl);
    int64_t length = -1;
    if (cl[0]) {
        char *end;
        long long v = strtoll(cl, &end, 10);
        if (*end == 0 && v >= 0) length = v;
    }
    int64_t total, end, at = update_range_start(from, (int)code, cr[0] ? cr : NULL, length, &total, &end);
    if (at < 0) {
        r = UPDATE_NET_RANGE;
        goto out;
    }
    if (total > max || end > max) {
        r = UPDATE_NET_TOO_BIG;
        goto out;
    }
    if ((*err = sink->begin(sink->ctx, at, total))) {
        r = UPDATE_NET_SINK;
        goto out;
    }
    enum { BUF = 64 * 1024 };
    if (!(buf = malloc(BUF))) goto out;
    int64_t got = at;
    for (;;) {
        if (sink->cancelled && sink->cancelled(sink->ctx)) {
            r = UPDATE_NET_CANCELLED;
            goto out;
        }
        DWORD avail = 0, n = 0;
        if (!wh.avail(req, &avail)) goto out;
        if (!avail) break;
        if (!wh.read(req, buf, avail < BUF ? avail : (DWORD)BUF, &n)) goto out;
        if (!n) break;
        if (got + (int64_t)n > max) {   /* max caps the file, not only this body */
            r = UPDATE_NET_TOO_BIG;
            goto out;
        }
        if ((*err = sink->write(sink->ctx, buf, n))) {
            r = UPDATE_NET_SINK;
            goto out;
        }
        got += n;
    }
    /* a body cut short (or longer than its range): the connection dropped */
    r = (end >= 0 && got != end) || (total >= 0 && got != total) ? UPDATE_NET_OFFLINE : UPDATE_NET_OK;
out:
    free(buf);
    if (req) wh.close(req);
    if (conn) wh.close(conn);
    if (session) wh.close(session);
    free(w);
    return r;
}

static const update_net windows_net = {win_get, NULL};
const update_net *update_net_windows(void) { return &windows_net; }

/* ------------------------------------------------------------------ processes */

static int win_self_path(void *ctx, char *out, size_t n) {
    (void)ctx;
    wchar_t path[32768];
    DWORD k = GetModuleFileNameW(NULL, path, sizeof path / sizeof path[0]);
    if (k == 0 || k >= sizeof path / sizeof path[0]) return EIO;
    return narrow(path, out, n) ? ENAMETOOLONG : 0;
}

static int win_spawn(void *ctx, const char *exe, const char *const *args) {
    (void)ctx;
    char line[4096] = "";
    if (update_quote_arg(exe, line, sizeof line)) return E2BIG;
    for (const char *const *a = args; *a; a++) {
        size_t k = strlen(line);
        if (k + 2 >= sizeof line) return E2BIG;
        line[k] = ' ', line[k + 1] = 0;
        if (update_quote_arg(*a, line, sizeof line)) return E2BIG;
    }
    wchar_t *wexe = wide(exe), *wline = wide(line), *dir = wide(exe);
    int r = EINVAL;
    if (wexe && wline && dir) {
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        /* the app's AppUserModelID, so the new app's window groups with the pin */
        static wchar_t aumid[] = RBSP_AUMID;
        si.dwFlags = STARTF_TITLEISAPPID;
        si.lpTitle = aumid;
        if (CreateProcessW(wexe, wline, NULL, NULL, FALSE, 0, NULL, slash ? dir : NULL, &si, &pi)) {
            /* only a process that holds the foreground rights can pass them on (I88) */
            AllowSetForegroundWindow(pi.dwProcessId);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            r = 0;
        } else {
            DWORD e = GetLastError();
            r = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : e == ERROR_ACCESS_DENIED ? EACCES : EIO;
        }
    }
    free(wexe);
    free(wline);
    free(dir);
    return r;
}

static int64_t unix_seconds(FILETIME ft) {
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return t < 116444736000000000ULL ? 0 : (int64_t)((t - 116444736000000000ULL) / 10000000ULL);
}

/* whether h started at `started` (within a second; 0: any) */
static int same_start(HANDLE h, int64_t started) {
    FILETIME c, x, k, u;
    if (!started) return 1;
    if (!GetProcessTimes(h, &c, &x, &k, &u)) return 0;
    int64_t s = unix_seconds(c);
    return s - started <= 1 && started - s <= 1;
}

static int win_wait_exit(void *ctx, uint32_t pid, int64_t started, uint32_t ms) {
    (void)ctx;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;   /* gone (or never there) */
    int r = 0;
    if (same_start(h, started)) r = WaitForSingleObject(h, ms) == WAIT_OBJECT_0 ? 0 : ETIMEDOUT;
    CloseHandle(h);
    return r;
}

/* whether path is a file directly in dir (case-blind, as NTFS names compare) whose name starts
 * with "rbsp-watch" */
static int watcher_in(const wchar_t *path, const wchar_t *dir) {
    size_t d = wcslen(dir), p = wcslen(path);
    if (p <= d + 1 || path[d] != L'\\' || CompareStringOrdinal(path, (int)d, dir, (int)d, TRUE) != CSTR_EQUAL) return 0;
    const wchar_t *name = path + d + 1;
    if (wcschr(name, L'\\')) return 0;
    return wcslen(name) >= 10 && CompareStringOrdinal(name, 10, L"rbsp-watch", 10, TRUE) == CSTR_EQUAL;
}

/* a listed process whose path couldn't be read (skip: a refused OpenProcess that is never ours,
 * update_open_skips): 1 when it may be one of ours by its snapshot name (update_image_kind;
 * watcher_only: an rbsp-watch exe only), so the scan is unknown (-1) unless one is found (review
 * UQ's GPT re-run, G2, GH1). A name that isn't UTF-16 can't be ours. */
static int image_kind(const PROCESSENTRY32W *pe) {
    char name[MAX_PATH * 3];
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pe->szExeFile, -1, name, (int)sizeof name, NULL, NULL) <= 0) return UPDATE_IMAGE_OTHER;
    return update_image_kind(name);
}
static int scan_unknown(const PROCESSENTRY32W *pe, int skip, int watcher_only) {
    if (skip) return 0;
    int k = image_kind(pe);
    return watcher_only ? k == UPDATE_IMAGE_WATCHER : k != UPDATE_IMAGE_OTHER;
}

static int scan_opened_unknown(HANDLE h, const PROCESSENTRY32W *pe, int watcher_only) {
    DWORD code;
    if (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE) return 0;
    return scan_unknown(pe, 0, watcher_only);
}

/* ------------------------------------------------------------------ a refused process's path (review K2) */

enum { NT_MAX = 32768 };
/* a scan's NT paths: the folder's two forms, a listed process's image and its alias (an 8.3-spelled
 * image's \\?\GLOBALROOT name, then its final path); absent: the folder doesn't exist (nothing
 * runs from it) */
typedef struct {
    wchar_t form[2][NT_MAX];
    wchar_t image[NT_MAX];
    wchar_t alias[NT_MAX + 16];
    int absent;
} nt_scan;

/* SystemProcessIdInformation (88): a process's image path, NT form (\Device\HarddiskVolumeN\...),
 * from the system's process list: no access to the process needed, any session, any account */
#define SYSTEM_PROCESS_ID_INFORMATION_ ((SYSTEM_INFORMATION_CLASS)88)
#define STATUS_INVALID_CID_ ((LONG)0xC000000B)
typedef struct {
    HANDLE ProcessId;
    UNICODE_STRING ImageName;
} sys_pid_image;

/* pid's image path into out (NT_MAX): 1 read, 0 the process is gone, -1 it couldn't be read */
static int listed_image(DWORD pid, wchar_t *out) {
    sys_pid_image i;
    memset(&i, 0, sizeof i);
    i.ProcessId = (HANDLE)(ULONG_PTR)pid;
    i.ImageName.MaximumLength = (USHORT)((NT_MAX - 1) * sizeof(wchar_t) > 0xfffe ? 0xfffe : (NT_MAX - 1) * sizeof(wchar_t));
    i.ImageName.Buffer = out;
    LONG s = NtQuerySystemInformation(SYSTEM_PROCESS_ID_INFORMATION_, &i, sizeof i, NULL);
    if (s == STATUS_INVALID_CID_) return 0;
    size_t n = i.ImageName.Length / sizeof(wchar_t);
    if (s < 0 || i.ImageName.Buffer != out || n == 0 || n >= NT_MAX) return -1;
    out[n] = 0;
    return 1;
}

/* dir's NT forms, to compare listed_image's paths with: [0] dir itself, its drive (or UNC share)
 * mapped to its device (QueryDosDeviceW; \Device\Mup for a share), [1] the folder's final path by
 * handle (GetFinalPathNameByHandleW, VOLUME_NAME_NT: links resolved, long names). A form that
 * can't be had is empty. A mismatch counts as elsewhere only against form[1] (or when the folder
 * doesn't exist): form[0] keeps the junctions and subst drives of dir's spelling, which an image
 * path has resolved (update_nt_path, review K2's undefined1). A junction removed from dir's
 * spelling while a process runs below its target is elsewhere too: it no longer runs from dir
 * (review K2F2) */
static void nt_forms(const wchar_t *dir, nt_scan *s) {
    s->form[0][0] = s->form[1][0] = 0;
    s->absent = 0;
    const wchar_t *p = dir;
    if (!wcsncmp(p, L"\\\\?\\", 4)) {
        p += 4;
        if (!_wcsnicmp(p, L"UNC\\", 4)) {
            if (_snwprintf(s->form[0], NT_MAX, L"\\Device\\Mup\\%ls", p + 4) < 0) s->form[0][0] = 0;
            p = NULL;
        }
    }
    if (p && p[0] == L'\\' && p[1] == L'\\') {
        if (_snwprintf(s->form[0], NT_MAX, L"\\Device\\Mup\\%ls", p + 2) < 0) s->form[0][0] = 0;
    } else if (p && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && p[2] == L'\\') {
        wchar_t drive[3] = {p[0], L':', 0}, dev[MAX_PATH];
        DWORD n = QueryDosDeviceW(drive, dev, MAX_PATH);
        /* a device, not a subst drive's "\??\..." (the final path covers that) */
        if (n > 0 && n < MAX_PATH && !wcsncmp(dev, L"\\Device\\", 8) && _snwprintf(s->form[0], NT_MAX, L"%ls%ls", dev, p + 2) < 0) s->form[0][0] = 0;
    }
    s->form[0][NT_MAX - 1] = 0;
    HANDLE h = CreateFileW(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD n = GetFinalPathNameByHandleW(h, s->form[1], NT_MAX, VOLUME_NAME_NT | FILE_NAME_NORMALIZED);
        if (n == 0 || n >= NT_MAX) s->form[1][0] = 0;
        CloseHandle(h);
    } else {
        DWORD e = GetLastError();
        s->absent = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    }
    for (int i = 0; i < 2; i++) {
        size_t l = wcslen(s->form[i]);
        while (l && s->form[i][l - 1] == L'\\') s->form[i][--l] = 0;
    }
}

/* whether path is under dir, at any depth (case-blind) */
static int path_under(const wchar_t *path, const wchar_t *dir) {
    size_t d = wcslen(dir);
    return wcslen(path) > d + 1 && path[d] == L'\\' && CompareStringOrdinal(path, (int)d, dir, (int)d, TRUE) == CSTR_EQUAL;
}

/* whether image (NT form) matches one of the folder's NT forms: under it (running_under's) or,
 * watcher_only, a watcher directly in it (stop_watchers' and watchers_in's) */
static int nt_form_match(const wchar_t *image, const nt_scan *nt, int watcher_only) {
    for (int i = 0; i < 2; i++)
        if (nt->form[i][0] && (watcher_only ? watcher_in(image, nt->form[i]) : path_under(image, nt->form[i]))) return 1;
    return 0;
}

/* an image spelled with an 8.3 component: the NT image name keeps the launcher's spelling
 * (reparse points are resolved, short names aren't expanded), while both folder forms are long
 * names (review K2's undefined2). Its final path by handle (\\?\GLOBALROOT + the image, long
 * names), matched against the folder's forms: 1 it matches, 0 it is elsewhere, -1 it couldn't be
 * resolved (ERROR_ACCESS_DENIED on another account's included: unknown by our names) */
static int nt_alias_match(const wchar_t *image, nt_scan *nt, int watcher_only) {
    size_t cap = sizeof nt->alias / sizeof nt->alias[0];
    if (_snwprintf(nt->alias, cap, L"\\\\?\\GLOBALROOT%ls", image) < 0) return -1;
    nt->alias[cap - 1] = 0;
    HANDLE h = CreateFileW(nt->alias, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD n = GetFinalPathNameByHandleW(h, nt->alias, NT_MAX, VOLUME_NAME_NT | FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= NT_MAX) return -1;
    return nt_form_match(nt->alias, nt, watcher_only);
}

/* image (NT form) against the folder, the one match of the opened and the refused processes
 * (review K2's undefined3): form[0], form[1], then the alias step for an image with a '~'
 * component. 1 the folder's, 0 elsewhere, -1 can't be told (update_nt_path: no canonical form, or
 * an alias that couldn't be resolved) */
static int nt_matches(const wchar_t *image, nt_scan *nt, int watcher_only) {
    int matched = nt_form_match(image, nt, watcher_only), alias = 0;
    if (!matched && wcschr(image, L'~')) {
        int a = nt_alias_match(image, nt, watcher_only);
        matched = a == 1;
        alias = a < 0 ? -1 : 1;
    }
    return update_nt_path(1, matched, nt->form[1][0] || nt->absent, alias);
}

/* an opened process against the folder: its Win32 path against dir as spelled (as before), else
 * its NT path (PROCESS_NAME_NATIVE) through nt_matches, so a junction above the folder, a subst
 * drive or an 8.3 spelling matches as for a refused one (review K2's undefined3). path holds
 * NT_MAX. 1 the folder's, 0 elsewhere, -1 can't be told (unknown by our names) */
static int opened_match(HANDLE h, const wchar_t *wdir, nt_scan *nt, wchar_t *path, int watcher_only) {
    DWORD len = NT_MAX;
    if (QueryFullProcessImageNameW(h, 0, path, &len) && (watcher_only ? watcher_in(path, wdir) : path_under(path, wdir))) return 1;
    len = NT_MAX;
    if (!QueryFullProcessImageNameW(h, PROCESS_NAME_NATIVE, nt->image, &len)) return -1;
    return nt_matches(nt->image, nt, watcher_only);
}

/* a listed process OpenProcess refused with err: update_listed_verdict on its path from the
 * system's list, matched by nt_matches as running_under's (under the folder) or, watcher_only, as
 * stop_watchers' (a watcher in it) */
static int refused_verdict(const PROCESSENTRY32W *pe, DWORD err, nt_scan *nt, int watcher_only) {
    int path = 0, match = 0;
    if (err != ERROR_INVALID_PARAMETER) {
        path = listed_image(pe->th32ProcessID, nt->image);
        if (path == 1) {
            int m = nt_matches(nt->image, nt, watcher_only);
            match = m == 1;
            if (m < 0) path = -1;
        }
    }
    return update_listed_verdict(0, err, path, match, image_kind(pe), watcher_only);
}

/* The session decision and the result are update_rules.c's (update_watcher_action,
 * update_stop_result: Mac-tested). Only the VM (W13a) can prove the Windows side of review UN7:
 *   - the same account logged on twice (fast user switching): from session 1, a watcher in
 *     session 2 is listed (the snapshot covers every session), gets ProcessIdToSessionId's 2, is
 *     left running (not ended: its act_lock writes finish) and the update or uninstall keeps that
 *     version folder (stop_watchers -1);
 *   - a watcher in our own session gets its Local\ stop event (OpenEventW finds it) and exits
 *     within the 10 s, so TerminateProcess is never reached;
 *   - ProcessIdToSessionId succeeds on our own pid and on a same-account watcher's (no access
 *     beyond what the token gives), and a failure on either leaves the watcher (never ends it);
 *   - another account's watcher (its own install, its own folder) isn't matched by path, and an
 *     OpenProcess that fails for it skips it without a stop or an end;
 *   - review K2: this account's watcher in session 0 (a password-logon task) refuses OpenProcess
 *     (ERROR_ACCESS_DENIED) from session 1; SystemProcessIdInformation still gives its NT path,
 *     which matches one of the folder's NT forms (nt_forms): left running and reported (-1),
 *     and running_under counts it, so the uninstall keeps the app;
 *   - the same when its task started it through the 8.3 path (C:\Users\GABRIE~1\...): its NT
 *     image keeps that spelling, and nt_alias_match's final path by handle matches the folder
 *     (tests/gate_update.c's winc-update-wintest.exe checks the match itself). */
static int win_stop_watchers(void *ctx, const char *dir) {
    (void)ctx;
    wchar_t *wdir = wide(dir);
    if (!wdir) return -1;
    size_t dl = wcslen(wdir);
    if (dl && wdir[dl - 1] == L'\\') wdir[--dl] = 0;
    nt_scan *nt = malloc(sizeof *nt);
    if (nt) nt_forms(wdir, nt);
    HANDLE snap = nt ? CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) : INVALID_HANDLE_VALUE;
    if (snap == INVALID_HANDLE_VALUE) {
        free(nt);
        free(wdir);
        return -1;
    }
    int found = 0, elsewhere = 0, unknown = 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    DWORD self = GetCurrentProcessId(), my_session = 0;
    int my_known = ProcessIdToSessionId(self, &my_session) != 0;
    wchar_t path[NT_MAX];
    BOOL more;
    for (more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || pe.th32ProcessID == 0) continue;
        /* by full path, never by image name (app.md §5.5: two programs share a name). One that
         * can't be inspected and may be a watcher makes the result -1, as watchers_in's scan
         * (review GH4, GH1's skip) */
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) {
            DWORD err = GetLastError();
            /* An elevated watcher may permit a query but refuse termination (review UF2). */
            if (err == ERROR_ACCESS_DENIED) {
                HANDLE query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (query) {
                    int m = opened_match(query, wdir, nt, path, 1);
                    CloseHandle(query);
                    if (m == 1) elsewhere = 1;
                    else if (m < 0 && scan_unknown(&pe, 0, 1)) unknown = 1;
                    if (m >= 0) continue;
                }
            }
            /* gone, another account's or the system's, or one of this account's in another logon
             * session (review K2): its path from the system's list. A watcher of ours there can't
             * be asked to stop or ended without a handle: left running and reported (-1), so its
             * files are kept, as UN7's */
            int v = refused_verdict(&pe, err, nt, 1);
            if (v == UPDATE_LISTED_FOUND) elsewhere = 1;
            else if (v == UPDATE_LISTED_UNKNOWN) unknown = 1;
            continue;
        }
        int m = opened_match(h, wdir, nt, path, 1);
        if (m < 0) {
            if (scan_unknown(&pe, 0, 1)) unknown = 1;
        } else if (m == 1) {
            /* its stop event is Local\ to its own session: one in another session (the same
             * account logged on twice) can't be asked to stop from here, and ending it would skip
             * its act_lock. It is left running and reported (-1), so its files are kept as for
             * one that runs on (review UN7) */
            DWORD session = 0;
            int theirs_known = ProcessIdToSessionId(pe.th32ProcessID, &session) != 0;
            if (update_watcher_action(my_known, my_session, theirs_known, session) != UPDATE_WATCHER_STOP) {
                elsewhere = 1;
                CloseHandle(h);
                continue;
            }
            found = 1;
            char name[96];
            wchar_t wname[96];
            HANDLE ev = NULL;
            if (update_watcher_event_name(pe.th32ProcessID, name, sizeof name) &&
                MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, (int)(sizeof wname / sizeof wname[0])) > 0)
                ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, wname);
            if (ev) {
                SetEvent(ev);
                CloseHandle(ev);
            }
            /* it stops on its own within a few seconds; one that doesn't is ended */
            if (WaitForSingleObject(h, ev ? 10000 : 0) != WAIT_OBJECT_0) {
                TerminateProcess(h, 1);
                WaitForSingleObject(h, 5000);
            }
        }
        CloseHandle(h);
    }
    int ended = !more && GetLastError() == ERROR_NO_MORE_FILES;   /* else the list cut short */
    CloseHandle(snap);
    free(nt);
    free(wdir);
    return update_stop_result(ended && !unknown, found, elsewhere);
}

static int win_watchers_in(void *ctx, const char *dir) {
    (void)ctx;
    wchar_t *wdir = wide(dir);
    if (!wdir) return -1;
    size_t dl = wcslen(wdir);
    if (dl && wdir[dl - 1] == L'\\') wdir[--dl] = 0;
    nt_scan *nt = malloc(sizeof *nt);
    wchar_t *path = malloc(NT_MAX * sizeof *path);
    if (nt && path) nt_forms(wdir, nt);
    HANDLE snap = nt && path ? CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) : INVALID_HANDLE_VALUE;
    if (snap == INVALID_HANDLE_VALUE) {
        free(path);
        free(nt);
        free(wdir);
        return -1;
    }
    int found = 0, unknown = 0, ended = 1;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    DWORD self = GetCurrentProcessId(), my_session = 0;
    int my_known = ProcessIdToSessionId(self, &my_session) != 0;
    BOOL more;
    for (more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || pe.th32ProcessID == 0) continue;
        /* the same match as win_stop_watchers; nothing is stopped */
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) {
            DWORD oe = GetLastError();   /* gone, or another account's or the system's: never ours */
            if (scan_unknown(&pe, update_open_skips(oe), 1)) unknown = 1;
            continue;
        }
        int m = opened_match(h, wdir, nt, path, 1);
        if (m < 0) {
            if (scan_opened_unknown(h, &pe, 1)) unknown = 1;
        } else if (m == 1) {
            /* only one win_stop_watchers would stop: another session's is neither noted nor taken
             * for this session's new one (review GR1); a session that can't be read is unknown */
            DWORD session = 0;
            int theirs_known = ProcessIdToSessionId(pe.th32ProcessID, &session) != 0;
            if (update_watcher_action(my_known, my_session, theirs_known, session) == UPDATE_WATCHER_STOP) found = 1;
            else if (!my_known || !theirs_known) unknown = 1;
        }
        CloseHandle(h);
    }
    if (!more && GetLastError() != ERROR_NO_MORE_FILES) ended = 0;   /* the list cut short */
    CloseHandle(snap);
    free(path);
    free(nt);
    free(wdir);
    return update_scan_result(found, unknown, ended);
}

static int win_running_under(void *ctx, const char *dir) {
    (void)ctx;
    wchar_t *wdir = wide(dir);
    if (!wdir) return -1;
    size_t dl = wcslen(wdir);
    if (dl && wdir[dl - 1] == L'\\') wdir[--dl] = 0;
    nt_scan *nt = malloc(sizeof *nt);
    if (nt) nt_forms(wdir, nt);
    HANDLE snap = nt ? CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) : INVALID_HANDLE_VALUE;
    if (snap == INVALID_HANDLE_VALUE) {
        free(nt);
        free(wdir);
        return -1;
    }
    static wchar_t path[NT_MAX];   /* update_at_start runs on one thread at a time */
    int found = 0, unknown = 0, ended = 1;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    DWORD self = GetCurrentProcessId();
    BOOL more;
    for (more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || pe.th32ProcessID == 0) continue;
        /* by full path, as win_stop_watchers; nothing is stopped */
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) {
            /* gone, another account's or the system's, or one of this account's in another logon
             * session (a password-logon task in session 0, a second logon: review K2), which
             * counts: its path from the system's list */
            int v = refused_verdict(&pe, GetLastError(), nt, 0);
            if (v == UPDATE_LISTED_FOUND) found = 1;
            else if (v == UPDATE_LISTED_UNKNOWN) unknown = 1;
            continue;
        }
        int m = opened_match(h, wdir, nt, path, 0);
        if (m == 1) found = 1;
        else if (m < 0 && scan_opened_unknown(h, &pe, 0)) unknown = 1;
        CloseHandle(h);
    }
    if (!more && GetLastError() != ERROR_NO_MORE_FILES) ended = 0;   /* the list cut short */
    CloseHandle(snap);
    free(nt);
    free(wdir);
    return update_scan_result(found, unknown, ended);
}

#ifdef UPDATE_WIN_TEST_HOOKS
/* tests/gate_update.c's Windows checks (review K2's undefined4) */
_Static_assert(UPDATE_WIN_NT_MAX == NT_MAX, "the test's buffers are NT_MAX");
int update_win_test_listed_image(unsigned long pid, wchar_t *out) { return listed_image((DWORD)pid, out); }
int update_win_test_nt_matches(const wchar_t *dir, const wchar_t *image, int watcher_only, int by_form[2]) {
    nt_scan *nt = malloc(sizeof *nt);
    if (!nt) return -2;
    nt_forms(dir, nt);
    for (int i = 0; i < 2; i++)
        by_form[i] = nt->form[i][0] && (watcher_only ? watcher_in(image, nt->form[i]) : path_under(image, nt->form[i]));
    int r = nt_matches(image, nt, watcher_only);
    free(nt);
    return r;
}
#endif

/* ------------------------------------------------------------------ files, the registry */

/* the tree by handle, never following a reparse point (common/plat_win.c, T3b) */
static int win_remove_tree(void *ctx, const char *path) {
    (void)ctx;
    return plat_remove_tree(path);
}

static int win_set_display_version(void *ctx, const char *version) {
    (void)ctx;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RBStemsPlus", 0, KEY_SET_VALUE, &k))
        return ENOENT;   /* no entry (W6a/W12 write it): nothing to follow */
    wchar_t v[PAYLOAD_VERSION_MAX];
    int r = EINVAL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, version, -1, v, PAYLOAD_VERSION_MAX) > 0)
        r = RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof *v)) ? EIO : 0;
    RegCloseKey(k);
    return r;
}

static int win_remove_tree_at(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    return plat_remove_tree_at(d, name);
}

static const update_os windows_os = {win_self_path,     win_spawn,       win_wait_exit,           win_stop_watchers, win_running_under,
                                     win_watchers_in,   win_remove_tree, win_set_display_version, win_remove_tree_at, NULL};
const update_os *update_os_windows(void) { return &windows_os; }

/* ------------------------------------------------------------------ the shell's entries (update_shell.h) */

#ifdef CORE_NATIVE_WIN
#include <shobjidl.h>

#include "compat.h"              /* compat_os_windows: the native machine */
#include "core_native_admin.h"   /* native_key_texts: the compiled-in keys */
#include "lifecycle.h"           /* LIFE_MIN_BUILD, LIFE_MIN_NET_RELEASE, LIFE_OS_* */
#include "update_shell.h"
#include "core_native_steps.h"
#include "volumes.h"

enum { SHELL_KEYS_MAX = 4 };
_Static_assert(CORE_WIN_SWAP_READY == 0 && CORE_WIN_SWAP_NONE == 1 && CORE_WIN_SWAP_OLD_RUNS == 2, "update_swap_answer's values");

typedef struct {
    update_env e;
    sig_pubkey keys[SHELL_KEYS_MAX];
    char apps[UPDATE_PATH_MAX];
    core_win_log_fn log;
    void *log_ctx;
} shell_env;

static void shell_log(void *ctx, const char *line) {
    shell_env *s = ctx;
    if (s->log && line) s->log(s->log_ctx, line, strlen(line));
}

/* the updater's env for the shell's paths: the apps folder is the version folder's parent. 0, or -1 */
static int shell_env_init(shell_env *s, const char *support, const char *app, const char *own, core_win_log_fn log, void *ctx) {
    memset(s, 0, sizeof *s);
    s->log = log;
    s->log_ctx = ctx;
    if (!support || !support[0] || !app || !app[0] || !own || !update_is_version(own)) return -1;
    if (update_apps_of(app, s->apps, sizeof s->apps)) return -1;
    size_t nkeys = 0;
    for (int i = 0; i < native_n_key_texts && nkeys < SHELL_KEYS_MAX; i++)
        if (!sig_key_from_text(&native_key_texts[i], &s->keys[nkeys])) nkeys++;
    s->e.support = support;
    s->e.apps = s->apps;
    s->e.own = own;
    /* the machine's stub and watcher, not this build's (an x64 app under emulation on ARM64) */
    const compat_os *cos = compat_os_windows();
#if defined(__aarch64__)
    s->e.arch = update_arch_of(cos && cos->native_machine ? cos->native_machine(cos->ctx) : 0, 1);
#else
    s->e.arch = update_arch_of(cos && cos->native_machine ? cos->native_machine(cos->ctx) : 0, 0);
#endif
    s->e.keys = s->keys;
    s->e.nkeys = nkeys;
    s->e.net = update_net_windows();
    s->e.os = update_os_windows();
    s->e.log = shell_log;
    s->e.ctx = s;
#ifdef CORE_TEST_HOOKS
    s->e.base = update_test_base();   /* NULL: UPDATE_BASE */
#endif
    (void)update_self_capture(&s->e);   /* the running exe's id, taken at start (review UH1): first call wins */
    return 0;
}

CORE_API int32_t CORE_CALL core_win_swap_wait(const char *support, const char *app, const char *own, uint32_t ms, core_win_log_fn log,
                                              void *ctx) {
    shell_env s;
    if (shell_env_init(&s, support, app, own, log, ctx)) return -1;
    return update_swap_answer(update_swap_wait(&s.e, ms));
}

CORE_API int32_t CORE_CALL core_win_swap_failed(const char *support, const char *app, const char *own, core_win_log_fn log, void *ctx) {
    shell_env s;
    if (shell_env_init(&s, support, app, own, log, ctx)) return -1;
    return update_swap_failed(&s.e, UPDATE_FAIL_SWAP);
}

CORE_API int32_t CORE_CALL core_win_update_at_start(const char *support, const char *app, const char *own, core_win_log_fn log, void *ctx) {
    shell_env s;
    if (shell_env_init(&s, support, app, own, log, ctx)) {
        shell_log(&s, "update: the start's paths aren't usable: no update is looked at");
        return UPDATE_FAIL_NONE;
    }
    int key = update_at_start(&s.e);
    update_result_keep(key);
    return key;
}

CORE_API int32_t CORE_CALL core_win_update_after_render(const char *support, const char *app, const char *own, core_win_log_fn log,
                                                        void *ctx) {
    shell_env s;
    if (shell_env_init(&s, support, app, own, log, ctx)) return -1;
    int r = update_after_render(&s.e);
    nw_steps_installed_changed(support, own);
    return r;
}

typedef LONG(WINAPI *shell_rtl_version_fn)(OSVERSIONINFOW *);

CORE_API int32_t CORE_CALL core_win_os_check(void) {
    /* RtlGetVersion: what the system is, whatever the manifest says (I73); ntdll is always mapped */
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    shell_rtl_version_fn get = nt ? (shell_rtl_version_fn)(void (*)(void))GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (!get || get(&v) != 0) v.dwMajorVersion = v.dwBuildNumber = 0;
    DWORD rel = 0, size = sizeof rel;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full", L"Release",
                     RRF_RT_REG_DWORD, NULL, &rel, &size) != ERROR_SUCCESS)
        rel = 0;
    /* life_os_verdict's rule (lifecycle.c is not in the core DLL) */
    if (v.dwMajorVersion < 10 || (v.dwMajorVersion == 10 && v.dwBuildNumber < LIFE_MIN_BUILD)) return LIFE_OS_WINDOWS_OLD;
    if (rel < LIFE_MIN_NET_RELEASE) return LIFE_OS_NET_OLD;
    return LIFE_OS_OK;
}

CORE_API int32_t CORE_CALL core_win_set_aumid(void) { return FAILED(SetCurrentProcessExplicitAppUserModelID(RBSP_AUMID)) ? -1 : 0; }

#ifdef CORE_TEST_HOOKS
/* the TEST build's base URL (update_shell.h): written once by the shell before any update or
 * flow runs, read by every env made after it; a copy that never moves once set */
static SRWLOCK test_base_lock = SRWLOCK_INIT;
static char test_base[256];

CORE_API int32_t CORE_CALL core_test_update_base(const char *url) {
    if (url && url[0] && (strncmp(url, "https://", 8) || strlen(url) >= sizeof test_base)) return -1;
    AcquireSRWLockExclusive(&test_base_lock);
    snprintf(test_base, sizeof test_base, "%s", url ? url : "");
    ReleaseSRWLockExclusive(&test_base_lock);
    return 0;
}

const char *update_test_base(void) {
    AcquireSRWLockShared(&test_base_lock);
    const char *r = test_base[0] ? test_base : NULL;
    ReleaseSRWLockShared(&test_base_lock);
    return r;
}
#endif

/* the shell's quit: set on main before core_new, read on a flow thread */
static SRWLOCK shell_quit_lock = SRWLOCK_INIT;
static core_win_quit_fn shell_quit_fn;
static void *shell_quit_ctx;

CORE_API void CORE_CALL core_win_set_quit(core_win_quit_fn quit, void *ctx) {
    AcquireSRWLockExclusive(&shell_quit_lock);
    shell_quit_fn = quit;
    shell_quit_ctx = ctx;
    ReleaseSRWLockExclusive(&shell_quit_lock);
}

int update_shell_can_quit(void) {
    AcquireSRWLockShared(&shell_quit_lock);
    int r = shell_quit_fn != NULL;
    ReleaseSRWLockShared(&shell_quit_lock);
    return r;
}

void update_shell_quit(void *ctx) {
    (void)ctx;
    AcquireSRWLockShared(&shell_quit_lock);
    core_win_quit_fn q = shell_quit_fn;
    void *qctx = shell_quit_ctx;
    ReleaseSRWLockShared(&shell_quit_lock);
    if (q) q(qctx);
}

void update_shell_quit_uninstall(void) {
    AcquireSRWLockShared(&shell_quit_lock);
    core_win_quit_fn q = shell_quit_fn;
    ReleaseSRWLockShared(&shell_quit_lock);
    if (q) q(CORE_WIN_QUIT_UNINSTALL);
}

CORE_API int32_t CORE_CALL core_win_device_events(uint32_t event, uint32_t devtype, uint32_t unitmask, const char *iface, char *out,
                                                  size_t n) {
    if (!out && n) return -1;
    return vol_device_events(event, devtype, unitmask, iface, out, n);
}
#endif
