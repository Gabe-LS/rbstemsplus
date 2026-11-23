/* report.h on Windows: WER's folders, the Desktop, and the Application event log. Imports
 * KERNEL32 and SHELL32 (known folders) and ole32 (CoTaskMemFree); wevtapi.dll isn't a KnownDLL
 * and is not on the static allowlist, so it is loaded at the first call by its full System32 path
 * with LOAD_LIBRARY_SEARCH_SYSTEM32 (app.md §7). The account-name DLLs load the same way
 * to stay off the static allowlist. Unverified until the VM run (W13a). */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#define SECURITY_WIN32
#include <windows.h>
#include <lm.h>
#include <secext.h>
#include <shlobj.h>
#include <winevt.h>

#include "report.h"
#include "win_rules.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PROC(m, name) ((void (*)(void))GetProcAddress((m), (name)))

static HMODULE load_system32(const wchar_t *file) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    size_t k = wcslen(file) + 1;
    if (!n || n + 1 + k > MAX_PATH) return NULL;
    path[n++] = L'\\';
    memcpy(path + n, file, k * sizeof *path);
    return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

/* ------------------------------------------------------------------ folders */

static const GUID local_app_data = {0xf1b32785, 0x6fba, 0x4fcf, {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}};
static const GUID program_data = {0x62ab5d82, 0xfdc1, 0x4dc3, {0xa9, 0xdd, 0x07, 0x0d, 0x1d, 0x49, 0x5d, 0x97}};
static const GUID desktop = {0xb4bfcc3a, 0xdb2c, 0x424c, {0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41}};
static const GUID profile = {0x5e6c858f, 0x0e22, 0x4760, {0x9a, 0xfe, 0xea, 0x33, 0x17, 0xb6, 0x71, 0x73}};

/* a known folder's path + tail as UTF-8; 0, or -1 */
static int known(const GUID *id, const char *tail, char *out, size_t n) {
    PWSTR p = NULL;
    int r = -1;
    if (SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, NULL, &p) == S_OK && p) {
        long k = wr_utf16_to_8((const uint16_t *)p, wcslen(p), out, n);
        size_t t = strlen(tail);
        if (k > 0 && (size_t)k + t + 1 <= n) {
            memcpy(out + k, tail, t + 1);
            r = 0;
        }
    }
    if (p) CoTaskMemFree(p);
    if (r && n) out[0] = 0;
    return r;
}

int report_wer_roots(char roots[REPORT_WER_ROOTS][REPORT_PATH_MAX]) {
    char local[REPORT_PATH_MAX], data[REPORT_PATH_MAX];
    const char *bases[2] = {known(&local_app_data, "", local, sizeof local) ? NULL : local,
                            known(&program_data, "", data, sizeof data) ? NULL : data};
    return report_wer_root_paths(bases, 2, roots, REPORT_WER_ROOTS);
}

int report_desktop(char *out, size_t n) { return known(&desktop, "", out, n); }

int report_profile(char *out, size_t n) { return known(&profile, "", out, n); }

int report_short_profile(const char *home, char *out, size_t n) {
    wchar_t long_path[REPORT_PATH_MAX], short_path[REPORT_PATH_MAX];
    if (n) out[0] = 0;
    if (wr_utf8_to_16(home, strlen(home), (uint16_t *)long_path, REPORT_PATH_MAX) <= 0) return -1;
    DWORD k = GetShortPathNameW(long_path, short_path, REPORT_PATH_MAX);
    if (!k || k >= REPORT_PATH_MAX || !wcscmp(long_path, short_path)) return -1;
    if (wr_utf16_to_8((const uint16_t *)short_path, k, out, n) > 0) return 0;
    if (n) out[0] = 0;
    return -1;
}

int report_user_name(char *out, size_t n) {
    wchar_t w[257];
    DWORD k = sizeof w / sizeof *w;
    if (n) out[0] = 0;
    if (!GetUserNameW(w, &k) || k < 2) return -1;   /* k counts the NUL */
    return wr_utf16_to_8((const uint16_t *)w, k - 1, out, n) > 0 ? 0 : -1;
}

typedef NET_API_STATUS(WINAPI *user_info_fn)(LPCWSTR, LPCWSTR, DWORD, LPBYTE *);
typedef NET_API_STATUS(WINAPI *buffer_free_fn)(LPVOID);
typedef BOOLEAN(SEC_ENTRY *user_name_ex_fn)(EXTENDED_NAME_FORMAT, LPWSTR, PULONG);

int report_full_name(const char *user, char *out, size_t n) {
    wchar_t w[260];
    if (n) out[0] = 0;
    /* The local account database has the full name even when NameDisplay is unavailable. */
    if (wr_utf8_to_16(user, strlen(user), (uint16_t *)w, sizeof w / sizeof *w) > 0) {
        HMODULE m = load_system32(L"netapi32.dll");
        if (m) {
            user_info_fn get = (user_info_fn)PROC(m, "NetUserGetInfo");
            buffer_free_fn release = (buffer_free_fn)PROC(m, "NetApiBufferFree");
            LPBYTE info = NULL;
            long k = -1;
            if (get && release && get(NULL, w, 10, &info) == NERR_Success && info) {
                const wchar_t *full = ((const USER_INFO_10 *)info)->usri10_full_name;
                if (full) k = wr_utf16_to_8((const uint16_t *)full, wcslen(full), out, n);
            }
            if (info) release(info);
            FreeLibrary(m);
            if (k > 0) return 0;
        }
    }
    HMODULE m = load_system32(L"secur32.dll");
    if (m) {
        user_name_ex_fn get = (user_name_ex_fn)PROC(m, "GetUserNameExW");
        ULONG cap = sizeof w / sizeof *w;
        long k = get && get(NameDisplay, w, &cap) ? wr_utf16_to_8((const uint16_t *)w, wcslen(w), out, n) : -1;
        FreeLibrary(m);
        if (k > 0) return 0;
    }
    if (n) out[0] = 0;
    return -1;
}

char report_system_drive(void) {
    wchar_t w[MAX_PATH];
    UINT k = GetWindowsDirectoryW(w, MAX_PATH);
    if (k < 2 || k >= MAX_PATH || w[1] != L':') return 0;
    wchar_t c = w[0];
    if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
    return c >= L'A' && c <= L'Z' ? (char)c : 0;
}

uint64_t report_memory(void) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
}

void report_locale(char *out, size_t n) {
    wchar_t w[LOCALE_NAME_MAX_LENGTH];
    int k = GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH);
    if (n) out[0] = 0;
    if (k > 1 && wr_utf16_to_8((const uint16_t *)w, (size_t)(k - 1), out, n) <= 0 && n) out[0] = 0;
}

/* ------------------------------------------------------------------ the event log (wevtapi, loaded at first use) */

typedef EVT_HANDLE(WINAPI *query_fn)(EVT_HANDLE, LPCWSTR, LPCWSTR, DWORD);
typedef BOOL(WINAPI *next_fn)(EVT_HANDLE, DWORD, EVT_HANDLE *, DWORD, DWORD, PDWORD);
typedef BOOL(WINAPI *render_fn)(EVT_HANDLE, EVT_HANDLE, DWORD, DWORD, PVOID, PDWORD, PDWORD);
typedef BOOL(WINAPI *close_fn)(EVT_HANDLE);

static struct {
    query_fn query;
    next_fn next;
    render_fn render;
    close_fn close;
} ev;
static INIT_ONCE ev_once = INIT_ONCE_STATIC_INIT;

static BOOL WINAPI ev_load(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    HMODULE m = load_system32(L"wevtapi.dll");
    if (!m) return TRUE;
    query_fn q = (query_fn)PROC(m, "EvtQuery");
    next_fn x = (next_fn)PROC(m, "EvtNext");
    render_fn r = (render_fn)PROC(m, "EvtRender");
    close_fn c = (close_fn)PROC(m, "EvtClose");
    if (q && x && r && c) ev.query = q, ev.next = x, ev.render = r, ev.close = c;
    return TRUE;
}

/* one event as cleaned UTF-8 XML, on one line; NULL when it can't be rendered or isn't wanted */
static char *render(EVT_HANDLE e, void **buf, DWORD *cap, size_t *len) {
    DWORD used = 0, props = 0;
    if (!ev.render(NULL, e, EvtRenderEventXml, *cap, *buf, &used, &props)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return NULL;
        void *b = realloc(*buf, used);
        if (!b) return NULL;
        *buf = b;
        *cap = used;
        if (!ev.render(NULL, e, EvtRenderEventXml, *cap, *buf, &used, &props)) return NULL;
    }
    char *x = report_text_utf8(*buf, used, len);           /* UTF-16LE: "<Event ..." */
    if (!x) return NULL;
    for (size_t i = 0; i < *len; i++)
        if (x[i] == '\r' || x[i] == '\n') x[i] = ' ';     /* one event per line */
    *len = report_event_clean(x, *len);                    /* first: the PC's name never decides */
    if (!report_event_wanted(x, *len)) {
        free(x);
        *len = 0;
        return NULL;
    }
    return x;
}

int report_event_log(uint32_t minutes, int lines, size_t max, char **out, size_t *len) {
    *out = NULL;
    *len = 0;
    InitOnceExecuteOnce(&ev_once, ev_load, NULL, NULL);
    if (!ev.query) return ENOENT;
    char q8[160];
    wchar_t q[160];
    if (report_event_query(minutes, q8, sizeof q8) || wr_utf8_to_16(q8, strlen(q8), (uint16_t *)q, 160) < 0) return EINVAL;
    EVT_HANDLE h = ev.query(NULL, L"Application", q, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!h) return wr_errno_win32((uint32_t)GetLastError(), WR_OP_OPEN);
    char **got = calloc(lines > 0 ? (size_t)lines : 1, sizeof *got);
    size_t *lens = calloc(lines > 0 ? (size_t)lines : 1, sizeof *lens), total = 0;
    DWORD cap = 1 << 15;
    void *buf = malloc(cap);
    int n = 0, e = got && lens && buf ? 0 : ENOMEM, full = 0;
    while (!e && !full) {                                  /* newest first */
        EVT_HANDLE batch[16];
        DWORD k = 0;
        if (!ev.next(h, 16, batch, 1000, 0, &k)) {
            DWORD w = GetLastError();
            if (w != ERROR_NO_MORE_ITEMS && w != ERROR_TIMEOUT) e = wr_errno_win32(w, WR_OP_OPEN);
            break;
        }
        for (DWORD i = 0; i < k; i++) {
            size_t l = 0;
            char *x = full ? NULL : render(batch[i], &buf, &cap, &l);
            if (x && (n >= lines || total + l + 1 > max)) {
                free(x);
                full = 1;
            } else if (x) {
                got[n] = x;
                lens[n++] = l;
                total += l + 1;
            }
            ev.close(batch[i]);
        }
    }
    ev.close(h);
    free(buf);
    if (!e) {
        *out = malloc(total + 1);
        if (!*out) e = ENOMEM;
    }
    if (!e) {                                              /* oldest first */
        size_t k = 0;
        for (int i = n - 1; i >= 0; i--) {
            memcpy(*out + k, got[i], lens[i]);
            k += lens[i];
            (*out)[k++] = '\n';
        }
        (*out)[k] = 0;
        *len = k;
    }
    for (int i = 0; i < n; i++) free(got[i]);
    free(got);
    free(lens);
    return e;
}
