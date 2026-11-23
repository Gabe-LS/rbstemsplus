/* The Windows helper's Stop on stdin (helper/stdin_stop.h; helper.md §1, §5, T15).
 *
 *   win-stop-test                the line rule (both OSes); on Windows also the watcher in a
 *                                child process (itself, --child), driven as the app drives a run:
 *                                stdin closed, a "stop" line, lines that aren't "stop", Pause then
 *                                Stop (suspended, stdin closed, resumed once), Pause and Resume
 *                                alone, and no stdin at all
 *
 * helper/tests/win-helper.sh runs the Mac's (the line rule); helper/build-win.sh builds
 * rbsp-win-stop-test.exe, which the VM runs (T16/T17). Exits 1 on a failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdin_stop.h"
#ifdef _WIN32
#include <windows.h>
#include "plat_helper.h"
#endif

static int fails;
#define CHECK(c, ...)                      \
    do {                                   \
        if (c) printf("ok   ");            \
        else { printf("FAIL "); fails++; } \
        printf(__VA_ARGS__);               \
        printf("\n");                      \
        fflush(stdout);                    \
    } while (0)

static int feed(const char *s) {
    stop_scan sc = {0};
    return stop_scan_feed(&sc, s, strlen(s));
}

static void t_rule(void) {
    CHECK(feed("stop\n") && feed("stop\r\n") && feed("hello\nstop\n") && feed("x\r\n\nstop\nmore"), "a \"stop\" line stops");
    CHECK(!feed("stop") && !feed("stopx\n") && !feed("xstop\n") && !feed("STOP\n") && !feed(" stop\n") && !feed("stop \n")
              && !feed("stop\r\r\n") && !feed("stopstopstop\n") && !feed("\n\n") && !feed(""),
          "anything else doesn't (an unended line, other words, case, spaces, a long line)");
    /* across reads, byte by byte */
    stop_scan sc = {0};
    const char *s = "abc\nst";
    int hit = stop_scan_feed(&sc, s, strlen(s));
    hit |= stop_scan_feed(&sc, "o", 1);
    hit |= stop_scan_feed(&sc, "p\r", 2);
    int before = hit;
    hit |= stop_scan_feed(&sc, "\n", 1);
    CHECK(!before && hit, "a line split across reads");
    stop_scan l = {0};
    char big[300];
    memset(big, 'x', sizeof big);
    int r = stop_scan_feed(&l, big, sizeof big);
    r |= stop_scan_feed(&l, "stop\n", 5);
    CHECK(!r && feed("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\nstop\n"), "a long line ends as one line");
}

#ifdef _WIN32
typedef LONG(NTAPI *nt_proc_fn)(HANDLE);
static nt_proc_fn nt_suspend, nt_resume;

static volatile LONG stopped;
static void on_stop(void) { InterlockedExchange(&stopped, 1); }

/* the child: the watcher, then "ready" ("no-watcher" without a stdin handle), then its final line
 * once stopped, or after ms */
static int child(int ms) {
    if (stop_watch_start(on_stop)) printf("no-watcher\n");
    else printf("ready\n");
    fflush(stdout);
    ULONGLONG until = GetTickCount64() + ms;
    while (!stopped && GetTickCount64() < until) Sleep(10);
    printf("%s\n", stopped ? "stopped" : "running");   /* the final line, as the helper's after its set */
    fflush(stdout);
    return 0;
}

typedef struct { HANDLE proc, in, out; } kid;

static int spawn(kid *k, int with_stdin, int ms) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    memset(k, 0, sizeof *k);
    if (with_stdin && !CreatePipe(&in_r, &in_w, &sa, 0)) return -1;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) return -1;
    if (in_w) SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);   /* the app's ends aren't inherited */
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    wchar_t exe[MAX_PATH + 1], cmd[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH + 1);
    if (!n || n > MAX_PATH) return -1;
    _snwprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" --child %d", exe, ms);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;                                            /* NULL: no stdin */
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (in_r) CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) { if (in_w) CloseHandle(in_w); CloseHandle(out_r); return -1; }
    CloseHandle(pi.hThread);
    k->proc = pi.hProcess;
    k->in = in_w;
    k->out = out_r;
    return 0;
}

/* the child's next line within ms ("" when none came) */
static void next_line(kid *k, char *buf, size_t n, DWORD ms) {
    size_t len = 0;
    ULONGLONG until = GetTickCount64() + ms;
    buf[0] = 0;
    while (GetTickCount64() < until) {
        DWORD avail = 0;
        /* the child gone: what it wrote is still there for ReadFile (which then fails at the end) */
        if (!PeekNamedPipe(k->out, NULL, 0, NULL, &avail, NULL)) avail = 1;
        if (!avail) { Sleep(5); continue; }
        char c;
        DWORD got = 0;
        if (!ReadFile(k->out, &c, 1, &got, NULL) || !got) break;
        if (c == '\n') { buf[len] = 0; return; }
        if (c != '\r' && len + 1 < n) buf[len++] = c;
    }
    buf[0] = 0;
}

static int quiet_for(kid *k, DWORD ms) {   /* no output and still running for ms */
    if (WaitForSingleObject(k->proc, ms) != WAIT_TIMEOUT) return 0;
    DWORD avail = 1;
    return PeekNamedPipe(k->out, NULL, 0, NULL, &avail, NULL) && avail == 0;
}

static void close_in(kid *k) {
    if (k->in) CloseHandle(k->in);
    k->in = NULL;
}

static int ended(kid *k, DWORD ms) { return WaitForSingleObject(k->proc, ms) == WAIT_OBJECT_0; }

static void done(kid *k) {
    if (WaitForSingleObject(k->proc, 0) != WAIT_OBJECT_0) TerminateProcess(k->proc, 9);
    close_in(k);
    CloseHandle(k->out);
    CloseHandle(k->proc);
}

static int put(kid *k, const char *s) {
    DWORD w = 0;
    return WriteFile(k->in, s, (DWORD)strlen(s), &w, NULL) && w == strlen(s);
}

static void t_processes(void) {
    char l[64];
    kid k;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    FARPROC a = nt ? GetProcAddress(nt, "NtSuspendProcess") : NULL, b = nt ? GetProcAddress(nt, "NtResumeProcess") : NULL;
    memcpy(&nt_suspend, &a, sizeof a);
    memcpy(&nt_resume, &b, sizeof b);
    CHECK(nt_suspend && nt_resume, "NtSuspendProcess and NtResumeProcess found");

    /* Stop: stdin closed */
    CHECK(!spawn(&k, 1, 10000), "a child with a stdin pipe");
    next_line(&k, l, sizeof l, 5000);
    CHECK(!strcmp(l, "ready"), "its watcher started (%s)", l);
    close_in(&k);
    next_line(&k, l, sizeof l, 3000);
    CHECK(!strcmp(l, "stopped") && ended(&k, 3000), "stdin's EOF stops it, its final line, then its exit (%s)", l);
    done(&k);

    /* Stop: a "stop" line, the pipe still open */
    spawn(&k, 1, 10000);
    next_line(&k, l, sizeof l, 5000);
    put(&k, "hello\r\nstop\r\n");
    next_line(&k, l, sizeof l, 3000);
    CHECK(!strcmp(l, "stopped") && ended(&k, 3000), "a \"stop\" line stops it (%s)", l);
    done(&k);

    /* other lines don't */
    spawn(&k, 1, 10000);
    next_line(&k, l, sizeof l, 5000);
    put(&k, "stopx\nxstop\nSTOP\n stop\n");
    CHECK(quiet_for(&k, 700), "other lines don't stop it");
    close_in(&k);
    next_line(&k, l, sizeof l, 3000);
    CHECK(!strcmp(l, "stopped"), "then the EOF does (%s)", l);
    done(&k);

    /* Pause, then Stop: suspended, stdin closed (it can't see it), resumed once: stopped */
    spawn(&k, 1, 10000);
    next_line(&k, l, sizeof l, 5000);
    CHECK(nt_suspend(k.proc) == 0, "paused (NtSuspendProcess)");
    close_in(&k);
    CHECK(quiet_for(&k, 700), "paused: the EOF isn't seen, no line, no exit");
    CHECK(nt_resume(k.proc) == 0, "resumed once (NtResumeProcess)");
    next_line(&k, l, sizeof l, 3000);
    CHECK(!strcmp(l, "stopped") && ended(&k, 3000), "Pause then Stop: its final line, then its exit (%s)", l);
    done(&k);

    /* Pause and Resume alone stop nothing */
    spawn(&k, 1, 10000);
    next_line(&k, l, sizeof l, 5000);
    nt_suspend(k.proc);
    Sleep(300);
    nt_resume(k.proc);
    CHECK(quiet_for(&k, 500), "Pause then Resume: it goes on");
    close_in(&k);
    next_line(&k, l, sizeof l, 3000);
    CHECK(!strcmp(l, "stopped"), "and stops at the EOF after (%s)", l);
    done(&k);

    /* started with no stdin pipe: Windows may give a console program its console's input instead
     * (VM round 1: it did), which never ends; either way nothing stops it but its end */
    CHECK(!spawn(&k, 0, 1500), "a child with no stdin pipe");
    next_line(&k, l, sizeof l, 5000);
    CHECK(!strcmp(l, "no-watcher") || !strcmp(l, "ready"), "no stdin pipe: %s", l);
    /* it runs its 1.5 s and ends on its own: its last line read after its end */
    DWORD end = WaitForSingleObject(k.proc, 8000);
    next_line(&k, l, sizeof l, 1000);
    CHECK(end == WAIT_OBJECT_0 && !strcmp(l, "running"), "and it isn't stopped: it ends on its own, not stopped (%s, \"%s\")",
          end == WAIT_OBJECT_0 ? "ended" : "still running after 8 s", l);
    done(&k);
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    if (plat_exe_start()) { fprintf(stderr, "plat_exe_start failed\n"); return 1; }
    if (argc == 3 && !strcmp(argv[1], "--child")) return child(atoi(argv[2]));
#endif
    (void)argc;
    (void)argv;
    t_rule();
#ifdef _WIN32
    t_processes();
#endif
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
