/* admin.h's Windows part (app.md §5.6, W4): the worker started with ShellExecuteExW verb runas on
 * a short-lived STA thread (COM wants one for the shell's verbs; the flow thread isn't one), the
 * local pipe server for its lines, the wait for its exit; the pipe's random name. In the core
 * DLL: SHELL32 and ole32 are on its allowlist (app.md §7). Unverified until the VM run (W13a). */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include "admin_win.h"

#include <stdlib.h>
#include <string.h>

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        w = NULL;
    }
    return w;
}

typedef struct {
    const wchar_t *exe, *params, *dir;
    HWND owner;
    HANDLE process;
    DWORD err;
} runas_job;

static DWORD WINAPI runas_thread(LPVOID p) {
    runas_job *j = p;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_UNICODE;
    sei.hwnd = j->owner;   /* the UAC prompt in front of the app's window */
    sei.lpVerb = L"runas";
    sei.lpFile = j->exe;
    sei.lpParameters = j->params;
    sei.lpDirectory = j->dir;   /* System32, never a user folder */
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei) && sei.hProcess) j->process = sei.hProcess;
    else j->err = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

static int read_lines(HANDLE pipe, HANDLE ev, void (*line)(void *, const char *, size_t), void *lctx) {
    admin_lines l;
    admin_lines_init(&l);
    char buf[4096];
    for (;;) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD got = 0;
        if (!ReadFile(pipe, buf, sizeof buf, &got, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                if (!GetOverlappedResult(pipe, &ov, &got, TRUE)) e = GetLastError();
                else e = 0;
            }
            if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED) break;
            if (e) return -1;
        }
        if (got) admin_lines_feed(&l, buf, got, 0, line, lctx);
    }
    admin_lines_feed(&l, "", 0, 1, line, lctx);
    return 0;
}

int admin_win_elevate(void *ctx, const char *exe, const char *params, const char *pipe, void (*line)(void *lctx, const char *s, size_t n), void *lctx,
                      void (*started)(void *sctx), void *sctx, int *code) {
    admin_win *aw = ctx;
    int told = 0;   /* started called: once, right after ShellExecuteExW, or on the way out */
    *code = -1;
    aw->last_err = 0;
    wchar_t *we = wide(exe), *wp = wide(params), *wn = wide(pipe), dir[MAX_PATH + 1];
    HANDLE srv = INVALID_HANDLE_VALUE, ev = NULL, th = NULL;
    runas_job j;
    memset(&j, 0, sizeof j);
    int r = -1;
    UINT dl = GetSystemDirectoryW(dir, MAX_PATH + 1);
    if (!we || !wp || !wn || !dl || dl > MAX_PATH) goto out;
    /* the server first: one instance, local clients only, never a pipe someone made before us */
    srv = CreateNamedPipeW(wn, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 64 * 1024, 0, NULL);
    if (srv == INVALID_HANDLE_VALUE || !(ev = CreateEventW(NULL, TRUE, FALSE, NULL))) goto out;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = ev;
    int connected = 0;
    if (!ConnectNamedPipe(srv, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED) connected = 1;
        else if (e != ERROR_IO_PENDING) goto out;
    }
    /* runas on its own short-lived STA thread */
    j.exe = we;
    j.params = wp;
    j.dir = dir;
    j.owner = (HWND)aw->owner;
    if (!(th = CreateThread(NULL, 0, runas_thread, &j, 0, NULL))) goto out;
    WaitForSingleObject(th, INFINITE);
    /* ShellExecuteExW has returned: the caller's hold on the exe goes now, before the session
     * (the worker's renames and deletes under its folders); the started process's image section
     * keeps its exe from being changed */
    told = 1;
    if (started) started(sctx);
    if (!j.process) {
        aw->last_err = j.err;
        CancelIo(srv);
        r = j.err == ERROR_CANCELLED ? ADMIN_EXIT_USER_STOPPED : -1;   /* UAC declined: the user stopped */
        goto out;
    }
    if (!connected) {
        HANDLE hs[2] = {ev, j.process};
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
            /* it ended without connecting (it refused before the pipe, or never got there) */
            CancelIo(srv);
            DWORD x = 0;
            if (GetExitCodeProcess(j.process, &x)) *code = (int)x, r = 0;
            goto out;
        }
        DWORD dummy;
        if (!GetOverlappedResult(srv, &ov, &dummy, FALSE) && GetLastError() != ERROR_PIPE_CONNECTED) goto out;
    }
    /* the client must be the process we started */
    ULONG client = 0;
    if (!GetNamedPipeClientProcessId(srv, &client) || client != GetProcessId(j.process)) {
        DisconnectNamedPipe(srv);
        WaitForSingleObject(j.process, INFINITE);
        goto out;
    }
    read_lines(srv, ev, line, lctx);
    WaitForSingleObject(j.process, INFINITE);
    DWORD x = 0;
    if (GetExitCodeProcess(j.process, &x)) *code = (int)x, r = 0;
out:
    if (!told && started) started(sctx);
    if (j.process) CloseHandle(j.process);
    if (th) CloseHandle(th);
    if (ev) CloseHandle(ev);
    if (srv != INVALID_HANDLE_VALUE) CloseHandle(srv);
    free(we);
    free(wp);
    free(wn);
    return r;
}

BOOLEAN WINAPI SystemFunction036(PVOID buffer, ULONG length);   /* RtlGenRandom, ADVAPI32 */

void admin_win_random(void *ctx, unsigned char out[16]) {
    (void)ctx;
    if (!SystemFunction036(out, 16)) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        for (int i = 0; i < 16; i++) out[i] = (unsigned char)((uint64_t)t.QuadPart >> (8 * (i & 7))) ^ (unsigned char)(GetCurrentProcessId() >> i);
    }
}
