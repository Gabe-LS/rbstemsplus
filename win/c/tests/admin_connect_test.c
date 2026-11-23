/* Run the real admin launch and connect path against a worker that stalls before opening the pipe. */
#include "check.h"
#include "../admin_win.h"
#include <wchar.h>

int checks, failures;

typedef unsigned long DWORD, ULONG;
typedef unsigned int UINT;
typedef void *HANDLE, *HWND, *LPVOID;
typedef struct { HANDLE hEvent; } OVERLAPPED;
typedef struct {
    const wchar_t *exe, *params, *dir;
    HWND owner;
    HANDLE process;
    DWORD err;
} runas_job;

enum {
    FALSE = 0, TRUE = 1, MAX_PATH = 260,
    PIPE_ACCESS_INBOUND = 1, FILE_FLAG_FIRST_PIPE_INSTANCE = 2, FILE_FLAG_OVERLAPPED = 4,
    PIPE_TYPE_BYTE = 8, PIPE_READMODE_BYTE = 16, PIPE_WAIT = 32, PIPE_REJECT_REMOTE_CLIENTS = 64,
    ERROR_CANCELLED = 1223, ERROR_IO_PENDING = 997, ERROR_PIPE_CONNECTED = 535,
    ERROR_OPERATION_ABORTED = 995, ERROR_TIMEOUT = 1460, ERROR_PIPE_NOT_CONNECTED = 233,
    WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258
};
#define INFINITE 0xffffffffUL
#define WAIT_FAILED 0xffffffffUL
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

static int pipe_handle, event, thread, process;
static int pending, cancelled, connected_at_deadline, read_count, closed, terminated, released, worker_waits;
static DWORD last_error, wait_ms;

static wchar_t *wide(const char *s) {
    wchar_t *w = calloc(strlen(s) + 1, sizeof *w);
    if (w) mbstowcs(w, s, strlen(s));
    return w;
}
static UINT GetSystemDirectoryW(wchar_t *d, UINT n) { (void)n; wcscpy(d, L"C:\\Windows\\System32"); return (UINT)wcslen(d); }
static HANDLE CreateNamedPipeW(const wchar_t *name, DWORD flags, DWORD mode, DWORD instances, DWORD out, DWORD in, DWORD ms, void *security) {
    (void)name; (void)flags; (void)mode; (void)instances; (void)out; (void)in; (void)ms; (void)security;
    return &pipe_handle;
}
static HANDLE CreateEventW(void *attr, int manual, int set, const wchar_t *name) {
    (void)attr; (void)manual; (void)set; (void)name;
    return &event;
}
static int ConnectNamedPipe(HANDLE h, OVERLAPPED *ov) {
    CHECK(h == &pipe_handle && ov->hEvent == &event, "connect uses the pipe and its event");
    pending = 1;
    last_error = ERROR_IO_PENDING;
    return 0;
}
static DWORD GetLastError(void) { return last_error; }
static DWORD runas_thread(LPVOID p) { ((runas_job *)p)->process = &process; return 0; }
static HANDLE CreateThread(void *attr, size_t stack, DWORD (*run)(LPVOID), LPVOID job, DWORD flags, DWORD *tid) {
    (void)attr; (void)stack; (void)flags; (void)tid;
    run(job);
    return &thread;
}
static DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    (void)ms;
    if (h == &process) worker_waits++;
    return WAIT_OBJECT_0;
}
static DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, int all, DWORD ms) {
    CHECK(n == 2 && hs[0] == &event && hs[1] == &process && !all && released == 1, "connection wait follows launch and release");
    wait_ms = ms;
    return WAIT_TIMEOUT;
}
static void CancelIo(HANDLE h) { CHECK(h == &pipe_handle && pending, "cancel pending connection"); cancelled++; }
static int GetOverlappedResult(HANDLE h, OVERLAPPED *ov, DWORD *got, int wait) {
    CHECK(h == &pipe_handle && ov->hEvent == &event && wait && cancelled == 1, "drain the cancelled connect before returning");
    pending = 0;
    *got = 0;
    last_error = ERROR_OPERATION_ABORTED;
    return connected_at_deadline;
}
static int GetExitCodeProcess(HANDLE h, DWORD *code) { (void)h; *code = worker_waits ? 0 : 259; return 1; }
static DWORD GetProcessId(HANDLE h) { (void)h; return 42; }
static int GetNamedPipeClientProcessId(HANDLE h, ULONG *id) { (void)h; *id = 42; return 1; }
static void __attribute__((unused)) DisconnectNamedPipe(HANDLE h) { (void)h; abort(); }
static void CloseHandle(HANDLE h) {
    CHECK(!pending, "no handle is closed while the connect uses its stack state");
    closed |= h == &pipe_handle ? 1 : h == &event ? 2 : h == &process ? 4 : h == &thread ? 8 : 0;
}
static int read_lines(HANDLE h, HANDLE ev, void (*line)(void *, const char *, size_t), void *ctx) {
    (void)h; (void)ev; (void)line; (void)ctx;
    read_count++;
    return 0;
}
/* The old source doesn't reference this fake. */
static int __attribute__((unused)) TerminateProcess(HANDLE h, UINT code) {
    CHECK(h == &process && !pending && (closed & 1) && code == ERROR_TIMEOUT,
          "terminate only the launched worker after draining and closing its pipe");
    terminated++;
    return 0;   /* closing the pipe must still work when the handle cannot terminate the worker */
}

#include "admin_elevate.inc"

static void release(void *ctx) { (void)ctx; released++; }

int main(void) {
    for (int race = 0; race <= 1; race++) {
        pending = cancelled = read_count = closed = terminated = released = worker_waits = 0;
        connected_at_deadline = race;
        admin_win aw = {0};
        int code;
        int r = admin_win_elevate(&aw, "worker", "args", "pipe", NULL, NULL, release, NULL, &code);
        CHECK(wait_ms == 60000, "connection has a 60 s deadline (got %lu ms)", wait_ms);
        CHECK(closed == 15 && cancelled == 1 && released == 1 && !pending, "the connect is drained and every handle closed");
        if (race) {
            CHECK(r == 0 && code == 0 && aw.last_err == 0 && read_count == 1 && worker_waits == 1 && terminated == 0,
                  "a connection that beats cancellation completes normally");
        } else {
            CHECK(r == -1 && code == -1 && aw.last_err == ERROR_TIMEOUT && read_count == 0 && worker_waits == 0 && terminated == 1,
                  "a worker that never connects fails startup without entering the running session (r=%d code=%d error=%lu)", r, code, aw.last_err);
        }
    }
    printf("admin connect: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
