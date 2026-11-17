/* The Windows bridge's entry points (bridge.md §1 "Exports", §3, task T4; the private Windows plan's R1, R3):
 * what rekordbox.exe and Demucs3.dll call by ordinal, the loader-safe two-phase start, and the
 * refusal stub. It sits in the bridge's one translation unit after common/plat_win.c and
 * bridge/win/ort_win.h.
 *
 * Ordinals (onnxruntime.def): 1 OrtGetApiBase, 2 OrtSessionOptionsAppendExecutionProviderEx_DML,
 * 3 ..._CPU, 4 ..._DML, 5 rbstems_marker (DATA). 2-4 pass through to the real library.
 *
 * The start (§3 "Loader lock"):
 *  - DllMain does nothing but DisableThreadLibraryCalls; nothing at DLL_PROCESS_DETACH.
 *  - Phase 1, in the first OrtGetApiBase (INIT_ONCE): the real library's path under Windows' own
 *    Program Files (plat_program_files: HKLM's ProgramFilesDir through ADVAPI32, no shell call),
 *    the admin-only and no-reparse check of the real library and its two folders (plat_lib_open_checked, owner decided, review B1: the
 *    folders opened top-down, each relative to the one above, then the file, sharing READ only,
 *    the descriptor read from each handle with READ_CONTROL through NtQuerySecurityObject: no
 *    on-demand DLL load, r4:I48, J8), its LoadLibraryExW while those handles are held, the "is it
 *    the bridge itself" check, OrtGetApiBase and the version.
 *    Phase 1's log lines are kept in a static buffer. No sha256 (bcrypt) and no thread here.
 *  - At the end of the first OrtGetApiBase the bridge reads whether this thread owns the PEB's
 *    LoaderLock. If not (rekordbox's own start, measured: phase0-probes.md §2.1), phase 2 runs at
 *    once. If it does, phase 2 waits, in INIT_ONCE, for a thread created then (it starts once the
 *    lock is free) or the first later call made outside the lock, whichever comes first.
 *    That check keeps phase 2 safe. Phase 1 relies on the measured start order (the first call
 *    from rekordbox.exe's main thread, outside the lock): the architecture gate of bridge.md §3.
 *    If it ever runs under the lock, it says so in bridge.log (review B3).
 *  - Phase 2: the core's paths (LocalAppData through the shell), the buffered lines into its log,
 *    then its start (config, bridge-state, validate).
 *
 * Never NULL (R1, r4:I46): on any refusal (Program Files unreadable, the real library missing, refused by the owner or ACL
 * check, a reparse point, LoadLibraryExW failing, no OrtGetApiBase, the bridge itself, and R1's
 * version mismatch: no OrtApi of version ORT_API_VERSION (18) from the real GetApi) OrtGetApiBase
 * returns the static stub OrtApiBase, whose GetApi returns the complete static stub OrtApi of
 * stub_api.inc (every slot typed from the vendored 1.18 header, J3), and ordinals 2-4 return the
 * stub's static status: the real library is never used. A real library that gives version 18 but
 * NULL for another version asked later gets the stub OrtApi for that one. A real library that
 * isn't 1.18 but gives version 18 is passed through untouched, as on the Mac (hooks off). Nothing
 * in the stub allocates: it may run in Demucs3.dll's DllMain.
 *
 * What the core provides (rbstems_bridge.c, once it builds for Windows, T1b2), as static functions
 * defined before this file is included:
 *   bridge_core_paths()      phase 2's first step: home, log and state paths (set_paths)
 *   bridge_core_log(line)    one line into bridge.log (logf_'s writer), "\n" added
 *   bridge_core_start()      phase 2's rest: config, write_state, validate enqueued (load_real's
 *                            second half)
 *   bridge_core_hook(api)    replaces the hooked slots of api, a copy of R (bridge_GetApi); each
 *                            hook calls ew_phase2_now() first (a deferred phase 2 runs at the
 *                            first hook outside the loader lock; one running on another thread
 *                            is waited for) and passes the call through to the real library
 *                            until ew_phase2_done: under the loader lock no hook touches the
 *                            core before bridge_core_start() has returned (review BW2)
 * ew_phase2_done is set only once bridge_core_start() has returned; ew_log_direct, set before the
 * buffered lines are written, sends later lines straight to the log. Nothing phase 2 runs may call
 * ew_phase2_now() on its own thread (InitOnceExecuteOnce would wait for itself).
 * Until then RBSP_WIN_ENTRY_ONLY builds a stand-in (end of this file): a bridge that caches nothing,
 * logs its start to %LOCALAPPDATA%\rbstemsplus\Logs\bridge.log, and, to show the wide model path
 * and the CPU-tensor check in a real rekordbox, logs each Demucs session and its first Run's
 * tensors. bridge/win/build.sh builds it.
 *
 * Heaps (§3): the bridge never frees what ONNX Runtime returned; statuses go through the real
 * ReleaseStatus. */
#include "onnxruntime_c_api.h"
#include "ort_win.h"

#include <stdarg.h>

#ifndef _WIN32
#error "entry_win.c is the Windows bridge's"
#endif

/* The real library: admin-only, outside rekordbox's folder (the installer copies Pioneer's file
 * there): <Program Files>\RB Stems Plus\ort\onnxruntime.dll under Windows' own Program Files, on
 * whichever drive (owner decided 2026-11-12). Only this path: Program Files from HKLM's
 * ProgramFilesDir, which only an administrator can change (plat_program_files, the rule the
 * worker, the core DLL and the watcher read, so the bridge loads what the worker installed),
 * never the environment or a per-user setting; unreadable, rekordbox gets the stub.
 * RBSTEMS_TEST_REAL_PATH exists for the tests (build.sh --win --test); a release build refuses
 * it. A test build takes the rule's own override instead when RBSP_TEST_PROGRAMS is set
 * (PLAT_TEST_PROGRAMS, as every component's test build). */
#ifdef RBSTEMS_TEST_REAL_PATH
#define EW_BUILD_NOTE ", TEST BUILD"
#define EW_ACCEPT_SELF 1
#else
#define EW_BUILD_NOTE ""
#define EW_ACCEPT_SELF 0
#endif

static char ew_real_path[WR_PF_MAX + sizeof WR_PF_REAL_ORT + 1];   /* phase 1 sets it once; "" when unread */
#ifdef RBSTEMS_TEST_REAL_PATH
_Static_assert(sizeof RBSTEMS_TEST_REAL_PATH <= sizeof ew_real_path, "--test REAL_PATH too long");
#endif

/* phase 1's first step: the real library's path; 0, or -1 (Program Files unreadable or refused) */
static int ew_resolve_real_path(void) {
#ifdef RBSTEMS_TEST_REAL_PATH
    if (!GetEnvironmentVariableW(L"RBSP_TEST_PROGRAMS", NULL, 0)) {
        snprintf(ew_real_path, sizeof ew_real_path, "%s", RBSTEMS_TEST_REAL_PATH);
        return 0;
    }
#endif
    return plat_program_files(WR_PF_REAL_ORT, ew_real_path, sizeof ew_real_path);
}

typedef const OrtApiBase *(ORT_API_CALL *ew_get_base)(void);

static HMODULE ew_self;                 /* this DLL (DllMain's handle) */
static plat_lib *ew_real;               /* the real library, or NULL: refused */
static const OrtApiBase *ew_real_base;
static int ew_hooks_ok;                 /* the real library is 1.18: its OrtApi gets the hooks */
static const char *ew_refused;          /* why the stub is returned ("" when it isn't) */
static INIT_ONCE ew_once1 = INIT_ONCE_STATIC_INIT, ew_once2 = INIT_ONCE_STATIC_INIT;
static volatile LONG ew_phase2_done;   /* phase 2 has returned: the core is started */
static volatile LONG ew_log_direct;    /* the core's log is open: ew_log writes, no more buffering */
/* ntdll's RtlDllShutdownInProgress, resolved in phase 1 (NULL: not found): true once ExitProcess
 * has begun, its other threads killed, so hook_ReleaseSession takes no lock (BX1) */
typedef BOOLEAN(NTAPI *ew_shutdown_fn)(void);
static ew_shutdown_fn ew_shutdown;

/* ---------------------------------------------------------------- the stub (R1, J3) */

struct OrtStatus { OrtErrorCode code; const char *msg; };
static OrtStatus stub_status = {ORT_FAIL, "rbstems bridge: ONNX Runtime unavailable (Stems Cache refused the real library)"};

#include "stub_api.inc"

_Static_assert(sizeof(OrtApi) == STUB_SLOTS * sizeof(void (*)(void)), "a slot of OrtApi isn't in the stub");

static const OrtApi *ORT_API_CALL stub_GetApi(uint32_t version) {
    (void)version;
    return &stub_api;
}
static const char *ORT_API_CALL stub_GetVersionString(void) { return "1.18.0"; }   /* what Phase 0 ran rekordbox with */
static const OrtApiBase stub_base = {stub_GetApi, stub_GetVersionString};

/* ---------------------------------------------------------------- phase 1's buffered log */

static SRWLOCK ew_log_lock = SRWLOCK_INIT;
static char ew_notes[4096];             /* phase 1's lines, "\n"-separated: no allocation, no file */
static size_t ew_notes_n;
static void bridge_core_log(const char *line);

static void ew_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ew_log(const char *fmt, ...) {
    char line[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    if (ew_log_direct) { bridge_core_log(line); return; }
    AcquireSRWLockExclusive(&ew_log_lock);
    if (!ew_log_direct) {
        size_t l = strlen(line);
        if (ew_notes_n + l + 1 < sizeof ew_notes) {
            memcpy(ew_notes + ew_notes_n, line, l);
            ew_notes[ew_notes_n + l] = '\n';
            ew_notes_n += l + 1;
        }
        ReleaseSRWLockExclusive(&ew_log_lock);
        return;
    }
    ReleaseSRWLockExclusive(&ew_log_lock);
    bridge_core_log(line);
}

/* ---------------------------------------------------------------- the loader lock */

/* Whether this thread owns the loader lock: the PEB's LoaderLock (PEB + 0x110 on 64-bit Windows,
 * an RTL_CRITICAL_SECTION *) is owned by this thread's id. The TEB holds the PEB at 0x60. The
 * public 64-bit layouts; validated in the VM (phase0-probes.md: every DllMain read 1). Unsure
 * counts as owned: phase 2 then only waits for a thread. */
static int ew_owns_loader_lock(void) {
    unsigned char *teb = (unsigned char *)NtCurrentTeb();
    unsigned char *peb = teb ? *(unsigned char **)(teb + 0x60) : NULL;
    RTL_CRITICAL_SECTION *cs = peb ? *(RTL_CRITICAL_SECTION **)(peb + 0x110) : NULL;
    if (!cs) return 1;
    return (DWORD)(ULONG_PTR)cs->OwningThread == GetCurrentThreadId();
}

/* ---------------------------------------------------------------- phase 1 */

static BOOL CALLBACK ew_phase1(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    char bad[1024];
    const char *err = NULL;
    ew_refused = "";
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");            /* always loaded; never freed */
    if (nt) {
        FARPROC f = GetProcAddress(nt, "RtlDllShutdownInProgress");
        memcpy(&ew_shutdown, &f, sizeof f);
    }
    if (ew_owns_loader_lock())                              /* the measured order changed (bridge.md §3) */
        ew_log("WARNING phase 1 ran under the loader lock: rekordbox's start order isn't the one measured (bridge.md §3)");
    if (ew_resolve_real_path()) {
        ew_refused = "programs";
        ew_log("ERROR real library: Windows' Program Files folder can't be read (ProgramFilesDir): rekordbox gets the stub (no stems)");
        return TRUE;
    }
    plat_lib *l = NULL;
    int owner = plat_lib_open_checked(ew_real_path, 2, EW_ACCEPT_SELF, bad, sizeof bad, &l, &err);   /* checked and held (B1) */
    if (owner == PLAT_OWNER_MISSING) {
        ew_refused = "missing";
        ew_log("ERROR real library: %s is missing: rekordbox gets the stub (no stems); reinstall or uninstall", ew_real_path);
        return TRUE;
    }
    if (owner != PLAT_OWNER_OK) {
        ew_refused = owner == PLAT_OWNER_REFUSED ? "path" : "owner";
        ew_log("ERROR real library: %s is a reparse point, held open for writing, or others than administrators can change it: not loaded",
               bad[0] ? bad : ew_real_path);
        return TRUE;
    }
    if (!l) {
        ew_refused = "load";
        ew_log("real library: cannot load %s: %s", ew_real_path, err ? err : "?");
        return TRUE;
    }
    if ((HMODULE)(void *)l == ew_self || plat_lib_sym(l, "rbstems_marker")) {   /* the loader handed back a bridge */
        ew_refused = "self";
        ew_log("ERROR real library: %s resolved to the bridge itself", ew_real_path);
        return TRUE;
    }
    ew_get_base get;
    void *sym = plat_lib_sym(l, "OrtGetApiBase");
    memcpy(&get, &sym, sizeof get);
    const OrtApiBase *base = get ? get() : NULL;
    if (!base) {
        ew_refused = "no-api";
        ew_log("ERROR real library: %s has no OrtGetApiBase", ew_real_path);
        return TRUE;
    }
    if (!base->GetApi || !base->GetApi(ORT_API_VERSION)) {  /* R1's version mismatch: refused everywhere */
        ew_refused = "version";
        ew_log("ERROR real library: %s has no OrtApi version %u: rekordbox gets the stub (no stems)", ew_real_path, (unsigned)ORT_API_VERSION);
        return TRUE;
    }
    ew_real = l;
    ew_real_base = base;
    ew_hooks_ok = ortw_version_ok(base->GetVersionString ? base->GetVersionString() : NULL);
    return TRUE;
}

/* ---------------------------------------------------------------- phase 2 */

static void bridge_core_paths(void);
static void bridge_core_start(void);

static BOOL CALLBACK ew_phase2(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    bridge_core_paths();
    AcquireSRWLockExclusive(&ew_log_lock);
    InterlockedExchange(&ew_log_direct, 1);                 /* later lines go straight to the log */
    ReleaseSRWLockExclusive(&ew_log_lock);
    for (char *s = ew_notes, *e; s < ew_notes + ew_notes_n; s = e + 1) {   /* phase 1's lines, in order */
        e = memchr(s, '\n', (size_t)(ew_notes + ew_notes_n - s));
        *e = 0;
        bridge_core_log(s);
    }
    bridge_core_start();
    InterlockedExchange(&ew_phase2_done, 1);                /* only now may a hook use the core (BW2) */
    return TRUE;
}

/* phase 2 now, unless this thread holds the loader lock (then a later call or the thread runs it) */
static void ew_phase2_now(void) {
    if (!ew_phase2_done && !ew_owns_loader_lock()) InitOnceExecuteOnce(&ew_once2, ew_phase2, NULL, NULL);
}

static DWORD WINAPI ew_phase2_thread(LPVOID p) {
    (void)p;
    InitOnceExecuteOnce(&ew_once2, ew_phase2, NULL, NULL);   /* starts once the loader lock is free */
    return 0;
}

static void ew_start(void) {
    InitOnceExecuteOnce(&ew_once1, ew_phase1, NULL, NULL);
    if (ew_phase2_done) return;
    if (!ew_owns_loader_lock()) {
        InitOnceExecuteOnce(&ew_once2, ew_phase2, NULL, NULL);
        return;
    }
    static volatile LONG spawned;
    if (InterlockedExchange(&spawned, 1) == 0) {            /* never waited on */
        HANDLE t = CreateThread(NULL, 0, ew_phase2_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
}

/* ---------------------------------------------------------------- the entry points */

static OrtApi ew_api;                   /* what rekordbox gets: the real table with the hooks */
static const OrtApi *ew_R;              /* the real library's table the hooks were made from */
static void bridge_core_hook(OrtApi *a);

static const OrtApi *ORT_API_CALL bridge_GetApi(uint32_t version) {
    ew_phase2_now();
    const OrtApi *real = ew_real_base->GetApi(version);
    if (!real) {                                            /* never NULL to rekordbox (r4:I46) */
        static volatile LONG said;
        if (InterlockedExchange(&said, 1) == 0) ew_log("ERROR real library: no OrtApi version %u: rekordbox gets the stub (no stems)", (unsigned)version);
        return &stub_api;
    }
    if (!ew_hooks_ok) return real;
    static SRWLOCK m = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&m);
    if (!ew_R) {
        memcpy(&ew_api, real, sizeof ew_api);
        bridge_core_hook(&ew_api);
        ew_R = real;
    }
    ReleaseSRWLockExclusive(&m);
    return real == ew_R ? &ew_api : real;
}

static const char *ORT_API_CALL bridge_GetVersionString(void) { return ew_real_base->GetVersionString(); }

static const OrtApiBase bridge_base = {bridge_GetApi, bridge_GetVersionString};

/* ordinal 1 */
const OrtApiBase *ORT_API_CALL OrtGetApiBase(void) {
    ew_start();
    return ew_real_base ? &bridge_base : &stub_base;
}

/* A failed status the caller can release with the table it was given: the real library's own
 * when it gave one, else the stub's static status (whose ReleaseStatus is a no-op). */
static OrtStatus *ew_fail(const char *msg) {
    const OrtApi *r = ew_real_base ? ew_real_base->GetApi(ORT_API_VERSION) : NULL;
    return r ? r->CreateStatus(ORT_FAIL, msg) : &stub_status;
}

#define EW_PROC(dst, name)                                    \
    do {                                                      \
        void *s_ = ew_real ? plat_lib_sym(ew_real, name) : NULL; \
        memcpy(&(dst), &s_, sizeof s_);                       \
    } while (0)

typedef OrtStatus *(ORT_API_CALL *ew_ep_ex)(OrtSessionOptions *, void *, void *);
typedef OrtStatus *(ORT_API_CALL *ew_ep_int)(OrtSessionOptions *, int);

/* ordinal 2: DirectML with rekordbox's own device and queue, passed through */
OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProviderEx_DML(OrtSessionOptions *o, void *dml, void *queue) {
    ew_start();
    ew_phase2_now();
    ew_ep_ex f;
    EW_PROC(f, "OrtSessionOptionsAppendExecutionProviderEx_DML");
    return f ? f(o, dml, queue) : ew_fail("rbstems bridge: real DirectML provider missing");
}

/* ordinal 3 */
OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_CPU(OrtSessionOptions *o, int use_arena) {
    ew_start();
    ew_phase2_now();
    ew_ep_int f;
    EW_PROC(f, "OrtSessionOptionsAppendExecutionProvider_CPU");
    return f ? f(o, use_arena) : ew_fail("rbstems bridge: real CPU provider missing");
}

/* ordinal 4 */
OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(OrtSessionOptions *o, int device) {
    ew_start();
    ew_phase2_now();
    ew_ep_int f;
    EW_PROC(f, "OrtSessionOptionsAppendExecutionProvider_DML");
    return f ? f(o, device) : ew_fail("rbstems bridge: real DirectML provider missing");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)r;
    if (why == DLL_PROCESS_ATTACH) {                        /* nothing else here (R3) */
        ew_self = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;                                            /* nothing at DLL_PROCESS_DETACH */
}

#ifdef RBSP_WIN_ENTRY_ONLY
/* ---------------------------------------------------------------- the stand-in core
 * Until rbstems_bridge.c builds for Windows (T1b2): no cache. A start line and phase 1's lines in
 * %LOCALAPPDATA%\rbstemsplus\Logs\bridge.log (LF only, r4:I50), and, on the real library's 1.18
 * table, two diagnostic hooks: each Demucs session's model path (ortw_is_demucs16 on the wide
 * path), and the device of each Demucs session's first Run's inputs and pre-allocated outputs
 * (ortw_on_cpu). Everything else passes through. */

const char rbstems_marker[] = "rbstems-bridge";   /* ordinal 5, DATA */
/* the app reads what a bridge can cache from this line: this one caches nothing */
__attribute__((used)) static const char rbstems_caps[] = "rbstems-cache: entry-only;";

static char eo_log[1100];               /* "": no log */

static void bridge_core_paths(void) {
    char home[1024], dir[1100];
#ifdef RBSTEMS_TEST_REAL_PATH
    wchar_t w[1024];
    DWORD k = GetEnvironmentVariableW(L"RBSTEMS_TEST_HOME", w, 1024);
    if (k > 0 && k < 1024) {
        if (wr_utf16_to_8((const uint16_t *)w, k, home, sizeof home) < 0) return;
    } else
#endif
    if (plat_home(home, sizeof home)) return;
    if (snprintf(dir, sizeof dir, "%s\\rbstemsplus\\Logs", home) >= (int)sizeof dir) return;
    plat_mkdirs(dir);
    if (snprintf(eo_log, sizeof eo_log, "%s\\bridge.log", dir) >= (int)sizeof eo_log) eo_log[0] = 0;
}

static void bridge_core_log(const char *line) {
    if (!eo_log[0]) return;
    static SRWLOCK m = SRWLOCK_INIT;
    char t[32];
    struct tm tm;
    plat_localtime(plat_now_wall_s(), &tm);
    strftime(t, sizeof t, "%Y-%m-%d %H:%M:%S", &tm);
    AcquireSRWLockExclusive(&m);
    FILE *f = plat_fopen_w(eo_log, 1);                      /* opened and closed per line, as logf_ */
    if (f) {
        fprintf(f, "%s [%d] %s\n", t, plat_pid(), line);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&m);
}

static void bridge_core_start(void) {
    ew_log("bridge loaded, ENTRY ONLY: nothing is cached (real library %s from %s%s%s)",
           ew_real_base ? ew_real_base->GetVersionString() : "REFUSED", ew_real_path,
           ew_real_base && !ew_hooks_ok ? ", NOT 1.18.x: passing everything through" : "", EW_BUILD_NOTE);
    if (ew_refused[0]) ew_log("rekordbox gets the stub OrtApi (%s): its stems fail cleanly", ew_refused);
}

#define EO_SESSIONS 16
static SRWLOCK eo_lock = SRWLOCK_INIT;
static struct { OrtSession *s; int ran; } eo_sessions[EO_SESSIONS];

static OrtStatus *ORT_API_CALL eo_CreateSession(const OrtEnv *env, const ORTCHAR_T *path, const OrtSessionOptions *o, OrtSession **out) {
    ew_phase2_now();
    OrtStatus *st = ew_R->CreateSession(env, path, o, out);
    if (st || !ortw_is_demucs16((const uint16_t *)path)) return st;
    char p8[1024];
    if (wr_utf16_to_8((const uint16_t *)path, wcslen(path), p8, sizeof p8) < 0) snprintf(p8, sizeof p8, "(not valid Unicode)");
    ew_log("Demucs session opened: %s", p8);
    AcquireSRWLockExclusive(&eo_lock);
    for (int i = 0; i < EO_SESSIONS; i++)
        if (!eo_sessions[i].s) { eo_sessions[i].s = *out; eo_sessions[i].ran = 0; break; }
    ReleaseSRWLockExclusive(&eo_lock);
    return NULL;
}

static void ORT_API_CALL eo_ReleaseSession(OrtSession *s) {
    AcquireSRWLockExclusive(&eo_lock);
    for (int i = 0; i < EO_SESSIONS; i++)
        if (eo_sessions[i].s == s) eo_sessions[i].s = NULL;
    ReleaseSRWLockExclusive(&eo_lock);
    ew_R->ReleaseSession(s);
}

static OrtStatus *ORT_API_CALL eo_Run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names, const OrtValue *const *in,
                                      size_t n_in, const char *const *out_names, size_t n_out, OrtValue **out) {
    ew_phase2_now();
    int first = 0;
    AcquireSRWLockExclusive(&eo_lock);
    for (int i = 0; i < EO_SESSIONS; i++)
        if (eo_sessions[i].s == s && !eo_sessions[i].ran) { eo_sessions[i].ran = 1; first = 1; }
    ReleaseSRWLockExclusive(&eo_lock);
    if (first) {
        size_t pre = 0;
        for (size_t i = 0; out && i < n_out; i++) pre += out[i] != NULL;
        ew_log("Demucs Run: %zu inputs %s CPU memory, %zu of %zu outputs pre-allocated%s", n_in,
               ortw_all_on_cpu(ew_R, in, n_in) ? "all in" : "NOT all in", pre, n_out,
               pre ? ortw_all_on_cpu(ew_R, (const OrtValue *const *)out, n_out) ? " (CPU)" : " (NOT all CPU)" : "");
    }
    return ew_R->Run(s, ro, in_names, in, n_in, out_names, n_out, out);
}

static void bridge_core_hook(OrtApi *a) {
    a->CreateSession = eo_CreateSession;
    a->ReleaseSession = eo_ReleaseSession;
    a->Run = eo_Run;
}
#endif
