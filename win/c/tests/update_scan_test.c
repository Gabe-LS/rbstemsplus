#include "check.h"
#include "../update.h"

#include <wchar.h>

int checks, failures;

typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef struct { size_t dwSize; DWORD th32ProcessID; } PROCESSENTRY32W;
typedef struct { int unused; } nt_scan;
enum {
    FALSE, ERROR_ACCESS_DENIED = 5, ERROR_NO_MORE_FILES = 18, STILL_ACTIVE = 259,
    TH32CS_SNAPPROCESS = 2, PROCESS_QUERY_LIMITED_INFORMATION = 0x1000, SYNCHRONIZE = 0x100000,
    PROCESS_TERMINATE = 1, EVENT_MODIFY_STATE = 2, CP_UTF8 = 65001, WAIT_OBJECT_0 = 0, NT_MAX = 16
};
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

static DWORD last_error, exit_code;
static BOOL exit_code_ok;
static int snapshot, process;
static int deny_terminate, query_match = -1, process_closes;

static wchar_t *wide(const char *dir) {
    wchar_t *p = malloc(NT_MAX * sizeof *p);
    if (p) mbstowcs(p, dir, NT_MAX);
    return p;
}
static void nt_forms(const wchar_t *dir, nt_scan *nt) { (void)dir; (void)nt; }
static DWORD GetLastError(void) { return last_error; }
static DWORD GetCurrentProcessId(void) { return 1; }
static HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid) { (void)flags; (void)pid; return &snapshot; }
static BOOL Process32FirstW(HANDLE snap, PROCESSENTRY32W *pe) { (void)snap; pe->th32ProcessID = 2; return 1; }
static BOOL Process32NextW(HANDLE snap, PROCESSENTRY32W *pe) {
    (void)snap;
    (void)pe;
    last_error = ERROR_NO_MORE_FILES;
    return 0;
}
static BOOL ProcessIdToSessionId(DWORD pid, DWORD *session) { (void)pid; *session = 1; return 1; }
static HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    (void)inherit;
    (void)pid;
    if (deny_terminate && (access & PROCESS_TERMINATE)) {
        last_error = ERROR_ACCESS_DENIED;
        return NULL;
    }
    return &process;
}
static void CloseHandle(HANDLE h) { if (h == &process) process_closes++; }
static BOOL GetExitCodeProcess(HANDLE h, DWORD *code) { (void)h; *code = exit_code; return exit_code_ok; }
/* Attempting any stop operation would be wrong in these query-only tests. */
static int MultiByteToWideChar(DWORD cp, DWORD flags, const char *name, int len, wchar_t *out, int cap) {
    (void)cp; (void)flags; (void)name; (void)len; (void)out; (void)cap;
    abort();
}
static HANDLE OpenEventW(DWORD access, BOOL inherit, const wchar_t *name) { (void)access; (void)inherit; (void)name; abort(); }
static void SetEvent(HANDLE ev) { (void)ev; abort(); }
static DWORD WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; abort(); }
static void TerminateProcess(HANDLE h, unsigned int code) { (void)h; (void)code; abort(); }
static int opened_match(HANDLE h, const wchar_t *dir, nt_scan *nt, wchar_t *path, int watcher_only) {
    (void)h;
    (void)dir;
    (void)nt;
    (void)path;
    (void)watcher_only;
    return query_match;
}
static int scan_unknown(const PROCESSENTRY32W *pe, int skip, int watcher_only) {
    (void)pe;
    (void)watcher_only;
    return !skip;   /* the snapshot names one of our watchers */
}
static int refused_verdict(const PROCESSENTRY32W *pe, DWORD err, nt_scan *nt, int watcher_only) {
    (void)pe;
    (void)err;
    (void)nt;
    (void)watcher_only;
    return UPDATE_LISTED_SKIP;
}

#include "update_scan.inc"

int main(void) {
    const char *dir = "C:\\apps";
    exit_code_ok = 1;
    exit_code = 1;
    CHECK(win_watchers_in(NULL, dir) == 0, "GH5: an exited watcher with an unreadable path is gone");
    CHECK(win_running_under(NULL, dir) == 0, "GH5: an exited process with an unreadable path is gone");
    exit_code = STILL_ACTIVE;
    CHECK(win_watchers_in(NULL, dir) == -1 && win_running_under(NULL, dir) == -1,
          "GH5: a still-active watcher with an unreadable path is unknown");
    exit_code_ok = 0;
    CHECK(win_watchers_in(NULL, dir) == -1 && win_running_under(NULL, dir) == -1,
          "GH5: an unreadable exit code doesn't prove the process is gone");
    deny_terminate = 1;
    query_match = 1;
    process_closes = 0;
    CHECK(win_stop_watchers(NULL, dir) == -1 && process_closes == 1,
          "UF2: a query-only watcher in the folder is reported as left running");
    query_match = -1;
    process_closes = 0;
    CHECK(win_stop_watchers(NULL, dir) == -1 && process_closes == 1,
          "UF2: a query-only watcher whose path can't be resolved leaves the scan unknown");
    printf("update scan: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
