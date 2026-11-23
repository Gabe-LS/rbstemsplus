/* The prepare helper's process on Windows (helper_proc.h, app.md §5.6, I63): CreateProcessW
 * suspended into a Job object that kills it when the Job closes, a clean environment (SystemRoot,
 * TEMP and TMP: LocalAppData\Temp), stdin, stdout and stderr pipes passed through an explicit
 * handle list (nothing else inherited), a reader thread for its lines and its exit, and a thread
 * that drains stderr into its last 4 KiB (core_helper_ended's err); Pause/Resume with
 * NtSuspendProcess/NtResumeProcess, Stop of a run by stdin's EOF (then one resume when paused),
 * any other call terminated with its Job. In the core DLL (ntdll, SHELL32 and ole32 are on its
 * allowlist, app.md §7). Unverified until the VM run (W13a). */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>

#include "plat.h"

#include "helper_proc_win.h"

#include "admin.h"

#include <stdlib.h>
#include <string.h>

NTSTATUS NTAPI NtSuspendProcess(HANDLE process);
NTSTATUS NTAPI NtResumeProcess(HANDLE process);

struct helper_proc {
    HANDLE process, job, in_w, out_r, err_r, reader, err_reader;
    HANDLE keeping;
    helper_tail tail;   /* the err thread's alone until the reader has waited for it */
    SRWLOCK lock;
    helper_state st;
    helper_line_fn line;
    helper_ended_fn ended;
    helper_log_fn log;
    void *ctx;
};

/* One named event per kept set, held by its Prepare runs in this logon session. The event's
 * lifetime follows those runs (Windows closes the handles on process exit as well). */
static int keeping_name(const char *set, wchar_t name[128]) {
    const wchar_t prefix[] = L"Local\\io.github.rbstemsplus.prepare.";
    size_t n = strlen(set), at = sizeof prefix / sizeof *prefix - 1;
    if (!n || n > 64) return 0;
    memcpy(name, prefix, sizeof prefix);
    for (size_t i = 0; i < n; i++) {
        char c = set[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return 0;
        name[at++] = (wchar_t)c;
    }
    name[at] = 0;
    return 1;
}

int helper_proc_keeping(const char *set) {
    wchar_t name[128];
    if (!keeping_name(set, name)) return 0;
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}

static HANDLE keeping_event(const char *const *argv, int argc) {
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--keep")) continue;
        wchar_t name[128];
        if (keeping_name(argv[i + 1], name)) return CreateEventW(NULL, TRUE, FALSE, name);
        break;
    }
    return NULL;
}

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

static int utf8(const wchar_t *w, char *out, size_t cap) {
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, (int)cap, NULL, NULL) > 0 ? 0 : -1;
}

/* the clean environment: SystemRoot from the system (never this process's environment), TEMP
 * and TMP the account's LocalAppData\Temp (plat_home: the known folder, from the registry) */
static uint16_t *env_block(void) {
    wchar_t sr[MAX_PATH + 1];
    char srs[3 * MAX_PATH + 1], temp[3 * MAX_PATH + 16];
    UINT n = GetSystemWindowsDirectoryW(sr, MAX_PATH + 1);
    if (!n || n > MAX_PATH || utf8(sr, srs, sizeof srs)) return NULL;
    /* plat_home: SHGetKnownFolderPath(FOLDERID_LocalAppData), strictly UTF-8, absolute */
    if (plat_home(temp, sizeof temp - 8)) return NULL;
    strcat(temp, "\\Temp");
    uint16_t *b = malloc(sizeof(uint16_t) * 4096);
    if (b && helper_env_block(srs, temp, b, 4096) < 0) {
        free(b);
        b = NULL;
    }
    return b;
}

static void on_line(void *ctx, const char *s, size_t n) {
    helper_proc *p = ctx;
    if (p->line) p->line(p->ctx, s, n);
}

static void on_too_long(void *ctx) {
    helper_proc *p = ctx;
    if (p->log) p->log(p->ctx, "helper: a line longer than 4 MiB was dropped");
}

/* stderr, read to its end (the helper's exit, or the Job's) into the tail */
static DWORD WINAPI err_main(LPVOID v) {
    helper_proc *p = v;
    char buf[4096];
    DWORD got;
    while (ReadFile(p->err_r, buf, sizeof buf, &got, NULL))   /* FALSE (ERROR_BROKEN_PIPE) is the end */
        if (got) helper_tail_feed(&p->tail, buf, got);           /* 0 bytes: a 0-byte write, not EOF */
    return 0;
}

static DWORD WINAPI reader_main(LPVOID v) {
    helper_proc *p = v;
    helper_lines l;
    helper_lines_init(&l, 0);
    char buf[4096];
    DWORD got;
    while (ReadFile(p->out_r, buf, sizeof buf, &got, NULL))
        if (got) helper_lines_feed(&l, buf, got, 0, on_line, on_too_long, p);
    helper_lines_feed(&l, NULL, 0, 1, on_line, on_too_long, p);
    helper_lines_free(&l);
    /* its last lines are in; then stderr's end, then its exit */
    WaitForSingleObject(p->err_reader, INFINITE);
    WaitForSingleObject(p->process, INFINITE);
    if (p->keeping) CloseHandle(p->keeping);
    DWORD code = 1;
    GetExitCodeProcess(p->process, &code);
    AcquireSRWLockExclusive(&p->lock);
    p->st.ended = 1;
    ReleaseSRWLockExclusive(&p->lock);
    size_t tn = 0;
    const char *tail = helper_tail_get(&p->tail, &tn);
    if (p->ended) p->ended(p->ctx, (int)code, tail, tn);
    return 0;
}

int helper_proc_spawn(const char *exe, const char *const *argv, int argc, int run, helper_line_fn line, helper_ended_fn ended, helper_log_fn log,
                      void *ctx, helper_proc **out) {
    *out = NULL;
    helper_proc *p = calloc(1, sizeof *p);
    if (!p) return -1;
    InitializeSRWLock(&p->lock);
    helper_state_init(&p->st, run);
    helper_tail_init(&p->tail);
    p->line = line;
    p->ended = ended;
    p->log = log;
    p->ctx = ctx;
    /* the command line: the program, then the core's argv, each quoted */
    const char **all = malloc(sizeof(char *) * (size_t)(argc + 1));
    char *cmd = malloc(32768);
    wchar_t *wexe = wide(exe), *wcmd = NULL;
    uint16_t *env = env_block();
    HANDLE in_r = NULL, out_w = NULL, err_w = NULL;
    int r = -1;
    if (!all || !cmd || !wexe || !env) goto fail;
    all[0] = exe;
    for (int i = 0; i < argc; i++) all[i + 1] = argv[i];
    if (admin_quote_argv(all, argc + 1, cmd, 32768) < 0 || !(wcmd = wide(cmd))) goto fail;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    if (!CreatePipe(&in_r, &p->in_w, &sa, 0) || !CreatePipe(&p->out_r, &out_w, &sa, 0) || !CreatePipe(&p->err_r, &err_w, &sa, 0)) goto fail;
    SetHandleInformation(p->in_w, HANDLE_FLAG_INHERIT, 0);   /* our ends stay ours */
    SetHandleInformation(p->out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(p->err_r, HANDLE_FLAG_INHERIT, 0);
    /* the Job: the helper dies with it (the app's exit or crash included) */
    if (!(p->job = CreateJobObjectW(NULL, NULL))) goto fail;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof li);
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &li, sizeof li)) goto fail;
    /* only these three handles are inherited */
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = malloc(asz);
    HANDLE inherit[3] = {in_r, out_w, err_w};
    if (!al || !InitializeProcThreadAttributeList(al, 1, 0, &asz)) {
        free(al);
        goto fail;
    }
    if (!UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL)) {
        DeleteProcThreadAttributeList(al);
        free(al);
        goto fail;
    }
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    si.lpAttributeList = al;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(wexe, wcmd, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                             env, NULL, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(al);
    free(al);
    if (!ok) goto fail;
    if (!AssignProcessToJobObject(p->job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        goto fail;
    }
    p->process = pi.hProcess;
    if (run) p->keeping = keeping_event(argv, argc);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(in_r), in_r = NULL;
    CloseHandle(out_w), out_w = NULL;
    CloseHandle(err_w), err_w = NULL;   /* the helper's copy is its only writer now */
    if (!(p->err_reader = CreateThread(NULL, 0, err_main, p, 0, NULL))) {
        TerminateJobObject(p->job, 1);
        goto fail;
    }
    if (!(p->reader = CreateThread(NULL, 0, reader_main, p, 0, NULL))) {
        TerminateJobObject(p->job, 1);   /* stderr ends with it: its thread returns */
        WaitForSingleObject(p->err_reader, INFINITE);
        goto fail;
    }
    *out = p;
    r = 0;
    goto done;
fail:
    if (p->keeping) CloseHandle(p->keeping);
    if (p->process) CloseHandle(p->process);
    if (p->job) CloseHandle(p->job);
    if (p->in_w) CloseHandle(p->in_w);
    if (p->out_r) CloseHandle(p->out_r);
    if (p->err_reader) CloseHandle(p->err_reader);
    if (p->err_r) CloseHandle(p->err_r);
    free(p);
done:
    if (in_r) CloseHandle(in_r);
    if (out_w) CloseHandle(out_w);
    if (err_w) CloseHandle(err_w);
    free(all);
    free(cmd);
    free(wexe);
    free(wcmd);
    free(env);
    return r;
}

void helper_proc_signal(helper_proc *p, int sig) {
    AcquireSRWLockExclusive(&p->lock);
    unsigned ops = helper_ops(&p->st, sig);
    if (ops & HELPER_OP_SUSPEND) NtSuspendProcess(p->process);
    if (ops & HELPER_OP_CLOSE_STDIN) {
        CloseHandle(p->in_w);   /* EOF: the helper's stop */
        p->in_w = NULL;
    }
    if (ops & HELPER_OP_RESUME) NtResumeProcess(p->process);
    if (ops & HELPER_OP_TERMINATE) TerminateJobObject(p->job, 1);
    ReleaseSRWLockExclusive(&p->lock);
}

void helper_proc_close(helper_proc *p) {
    if (!p) return;
    /* giving up on a helper that hasn't ended (paused, ignoring its EOF, never stopped): its Job
     * ends it now, so the wait below can't hang the app's quit or update. The graceful stop is
     * helper_proc_signal(STOP), then close after ended. */
    AcquireSRWLockExclusive(&p->lock);
    int ended = p->st.ended;
    ReleaseSRWLockExclusive(&p->lock);
    if (!ended) TerminateJobObject(p->job, 1);
    WaitForSingleObject(p->reader, INFINITE);   /* its last lines and its exit (ended has fired) */
    CloseHandle(p->reader);
    CloseHandle(p->err_reader);                  /* the reader waited for it */
    CloseHandle(p->err_r);
    if (p->in_w) CloseHandle(p->in_w);
    CloseHandle(p->out_r);
    CloseHandle(p->process);
    CloseHandle(p->job);   /* kill on close: nothing of it stays */
    free(p);
}
