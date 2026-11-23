/* core_native.c: the core DLL's own Windows code for the start config's "native" host entries
 * (core/native.h's backend, API-V1-NOTES §11, app.md §4.1 and §5.6). Only in the core DLL
 * (core/build.sh dll, CORE_NATIVE_WIN). Unverified until the VM run (W13a).
 *
 *  - post_work: CORE_WORK_FLOW and CORE_WORK_QUIT each on their own serial queue (plat_queue: one
 *    thread, FIFO), CORE_WORK_WALK on a maintenance queue (below-normal priority, low I/O),
 *    CORE_WORK_FACT and the rest on the Windows thread pool (TrySubmitThreadpoolCallback), the
 *    native fast path as GCD is on the Mac. A job that can't be queued fails fast, as core.h asks
 *    of a host. The queues live as long as the process (plat_queue is never freed).
 *  - helper_spawn and helper_signal: helper_proc_win.c (a Job object, the clean environment,
 *    Pause/Resume by suspending, Stop by stdin's EOF for a run, by terminating for any other
 *    call). A run is a call with "--keep" and without "--estimate" (Prepare's run: the helper's
 *    stdin watcher is on its run path only, helper.md). Its lines and its end go to
 *    core_helper_line and core_helper_ended from its reader thread; its handles are closed on the
 *    thread pool after that (helper_proc_close waits for the reader). stderr is its own pipe,
 *    drained by its own thread: the end's err is its last 4 KiB (core.h's err, the Mac's). Its
 *    paths under Program Files are the DLL's own, whatever the core's argv says (review PF3:
 *    helper_program_args): --ort is <Program Files>\RB Stems Plus\ort\onnxruntime.dll from
 *    native_core_programs, --rekordbox the folder a worker last verified as current, checked with
 *    one stat at the spawn (PF1); a run without one isn't started, an estimate gets "", from which
 *    it loads nothing (PF2). A run (helper_is_run): "--keep" without "--estimate" or "--quick".
 *  - slow_fact: the model's and the bridge's sha256 (FACT_SHA256, read in 1 MiB chunks through
 *    plat.h), memoised (sha_memo.c: by path and the open handle's size, 100 ns last-write and
 *    change times and 128-bit file id, stored only when they didn't change while it was
 *    hashed), so every caller's ask of an unchanged file is a stamp read. The stamp is a cache
 *    key, not proof of content: a same-size rewrite that keeps or restores it hits (core.h's
 *    contract, as on the Mac). DRIVE, SIZE_WALK and ALPHATHETA: native_facts.c (the volume's
 *    free bytes for this account, entriesSize, the AlphaTheta pin). R1: core_native_admin.c's
 *    nw_admin_r1 (native_facts.c's rule with the app's verified payload lists). LOCATION:
 *    native_location.c's measure with this PC's answers (review HF5): the live bridge from
 *    host_facts.c's read, the measure on its own queue through native_location_slow_fact
 *    (never on the UI thread, one at a time, waited for NL_WAIT_NS), enabled by
 *    native_location.c's NATIVE_LOCATION_LIVE.
 *  - admin: core_native_admin.c (the worker through admin.c's client, one elevation per call);
 *    its check of the bridge copy hashes uncached (sha_file, never the memo).
 *  - step: core_native_steps.c (every step the shared flows ask on Windows, native-steps-reach.py;
 *    any other kind is CORE_E_STATE, as a step entry that isn't provided). */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "plat.h"
#include "plat_win.h"

#include "native.h"
#include "core_win.h"   /* core_test_win_fake_helper (test-hooks DLLs) */

#include "core_native_admin.h"
#include "core_native_steps.h"
#include "helper_proc.h"
#include "helper_proc_win.h"
#include "host_facts.h"
#include "native_choice_win.h"   /* nl_rekordbox_running, nl_process_start */
#include "native_facts.h"
#include "native_location.h"
#include "native_steps.h"
#include "sha_memo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "win_rules.h"

typedef struct nw nw;

/* one helper call, from its spawn until its handles are closed */
typedef struct hcall {
    struct hcall *next;    /* nw's list, while it may be signalled */
    nw *n;                 /* NULL once the call has left the list (ended) */
    core *c;
    uint64_t run;
    helper_proc *p;        /* set once helper_proc_spawn returned */
    PTP_WORK work;         /* close_call, made before the spawn: submitting it can't fail */
    int ended;             /* the reader's ended came */
} hcall;

struct nw {
    core *c;
    nw_admin *admin;
    nw_steps *steps;
    char *helper;          /* the helper's exe (paths.helper), NULL: none */
    char *support, *cache_default;   /* paths.support and paths.cache_default (CORE_SLOW_LOCATION), NULL: none */
    plat_thread_id ui;     /* the UI thread: core_new's, which opens the backend */
    char ort[512];         /* the helper's --ort: <Program Files>\RB Stems Plus\ort\onnxruntime.dll (PF3) */
    plat_queue *flow, *quit, *walk;
    SRWLOCK lock;          /* calls, and each call's p and ended */
    hcall *calls;
    sha_memo shas;         /* slow_fact's sha256 memo, its own lock */
};

/* ---------------------------------------------------------------- post_work */

static void run_job(void *job) { core_run(job); }
static VOID NTAPI pool_job(PTP_CALLBACK_INSTANCE inst, PVOID job) {
    (void)inst;
    core_run(job);
}

static void nw_post_work(void *st, core_work kind, core_job *job) {
    nw *n = st;
    int e;
    switch (kind) {
    case CORE_WORK_FLOW: e = plat_async(n->flow, run_job, job); break;
    case CORE_WORK_QUIT: e = plat_async(n->quit, run_job, job); break;
    case CORE_WORK_WALK: e = plat_async(n->walk, run_job, job); break;
    default: e = TrySubmitThreadpoolCallback(pool_job, job, NULL) ? 0 : ENOMEM; break;
    }
    if (e) {   /* out of memory: core.h's host never drops a job while the core lives */
        native_core_log(n->c, "core: a worker job couldn't be queued: stopping");
        RaiseFailFastException(NULL, NULL, 0);
    }
}

/* ---------------------------------------------------------------- the helper */

static VOID NTAPI close_call(PTP_CALLBACK_INSTANCE inst, PVOID v, PTP_WORK work) {
    (void)inst;
    hcall *h = v;
    helper_proc_close(h->p);   /* waits for its reader, which has returned from ended */
    CloseThreadpoolWork(work);  /* allowed inside its own callback */
    free(h);
}

/* after both the spawn's return and the reader's end: the handles closed off the reader thread */
static void call_done(hcall *h) { SubmitThreadpoolWork(h->work); }

/* removes h from n's list (under n->lock) */
static void unlink_call(nw *n, hcall *h) {
    for (hcall **pp = &n->calls; *pp; pp = &(*pp)->next)
        if (*pp == h) {
            *pp = h->next;
            break;
        }
    h->n = NULL;
}

static void on_line(void *ctx, const char *s, size_t len) {
    hcall *h = ctx;
    core_helper_line(h->c, h->run, s, len);
}

/* a line past HELPER_LINE_MAX was dropped (the run still holds the core) */
static void on_log(void *ctx, const char *line) {
    hcall *h = ctx;
    native_core_log(h->c, line);
}

static void on_ended(void *ctx, int code, const char *err, size_t err_len) {
    hcall *h = ctx;
    nw *n = h->n;
    int spawned;
    /* off the list first: once core_helper_ended is in, the core (and n) may go */
    AcquireSRWLockExclusive(&n->lock);
    unlink_call(n, h);
    h->ended = 1;
    spawned = h->p != NULL;
    ReleaseSRWLockExclusive(&n->lock);
    core_helper_ended(h->c, h->run, (int32_t)code, err, err_len);
    if (spawned) call_done(h);   /* else the spawner does, once it has p */
}

#ifdef CORE_TEST_HOOKS
/* the harness's fake helper (core_win.h's core_test_win_fake_helper): test-hooks DLLs only, so a
 * release DLL has no way to start anything but paths.helper */
#define FAKE_HELPER_MAX 1024   /* the path's bytes with its NUL: the spawn's copy holds no more */
static SRWLOCK fake_lock = SRWLOCK_INIT;
static char *fake_helper;

CORE_API int32_t CORE_CALL core_test_win_fake_helper(const char *path) {
    if (path && strlen(path) >= FAKE_HELPER_MAX) return CORE_E_ARG;
    char *p = path && path[0] ? _strdup(path) : NULL;
    if (path && path[0] && !p) return CORE_E_NOMEM;
    AcquireSRWLockExclusive(&fake_lock);
    char *old = fake_helper;
    fake_helper = p;
    ReleaseSRWLockExclusive(&fake_lock);
    free(old);
    return CORE_OK;
}
#endif

/* Build the command line with the DLL's paths and the proof for that exact rekordbox.exe.
 * av has argc + 1 slots and rb stays alive through the spawn. */
static int nw_prepare_args(nw *n, const char *const *argv, int argc, int is_run, char rb[COMPAT_PATH_MAX], const char **av) {
    int has_rb;
    if (helper_program_args(argv, argc, n->ort, "", av, &has_rb)) return -1;
    int proved;
    if (has_rb && nw_steps_rekordbox_known(n->steps, rb, COMPAT_PATH_MAX, &proved)) {
        /* The helper stops parsing options at its first file operand. */
        if (proved) av[0] = "--resample-proved";
        if (helper_program_args(argv, argc, n->ort, rb, av + proved, NULL)) return -1;
        return argc + proved;
    }
    if (helper_run_refused(has_rb, rb, is_run)) {
        native_core_log(n->c, "helper: no verified current rekordbox, the run isn't started");
        return -1;
    }
    return argc;
}

static int32_t nw_helper_spawn(void *st, uint64_t run, const char *const *argv, int32_t argc) {
    nw *n = st;
    const char *exe = n->helper;
#ifdef CORE_TEST_HOOKS
    char fake[FAKE_HELPER_MAX] = "";
    AcquireSRWLockShared(&fake_lock);
    if (fake_helper) snprintf(fake, sizeof fake, "%s", fake_helper);
    ReleaseSRWLockShared(&fake_lock);
    if (fake[0]) exe = fake;
#endif
    if (!exe) return -1;
    int is_run = helper_is_run(argv, argc);
    if (is_run && !nw_steps_cache_covers_prepare(n->steps)) {
        native_core_log(n->c, "helper: Stems Cache covers no rekordbox version");
        return -1;
    }
    hcall *h = calloc(1, sizeof *h);
    if (!h) return -1;
    if (!(h->work = CreateThreadpoolWork(close_call, h, NULL))) {
        free(h);
        return -1;
    }
    h->n = n;
    h->c = n->c;
    h->run = run;
    AcquireSRWLockExclusive(&n->lock);
    h->next = n->calls;
    n->calls = h;
    ReleaseSRWLockExclusive(&n->lock);
    /* the paths under Program Files are the DLL's own (PF3): --ort ours; --rekordbox, when the call
     * names it, the newest covered folder still holding its verified EXE. Spawn checks the
     * published file stamps. An estimate can use the current folder when Cache covers none. */
    char rb[COMPAT_PATH_MAX] = "";
    const char **av = malloc(((size_t)argc + 1) * sizeof *av);
    helper_proc *p = NULL;
    int next_argc = av ? nw_prepare_args(n, argv, argc, is_run, rb, av) : -1;
    if (next_argc < 0 || helper_proc_spawn(exe, av, next_argc, is_run, on_line, on_ended, on_log, h, &p)) {
        free(av);
        AcquireSRWLockExclusive(&n->lock);   /* not started: no ended came, none will */
        unlink_call(n, h);
        ReleaseSRWLockExclusive(&n->lock);
        CloseThreadpoolWork(h->work);
        free(h);
        return -1;
    }
    free(av);   /* the command line is built: argv isn't kept */
    AcquireSRWLockExclusive(&n->lock);
    h->p = p;
    int ended = h->ended;
    ReleaseSRWLockExclusive(&n->lock);
    if (ended) call_done(h);   /* it ended before spawn returned: n is no longer touched */
    return 0;
}

static void nw_helper_signal(void *st, uint64_t run, core_helper_sig sig) {
    nw *n = st;
    /* under the lock: a call on the list hasn't been handed to close_call */
    AcquireSRWLockExclusive(&n->lock);
    for (hcall *h = n->calls; h; h = h->next)
        if (h->run == run && h->p) helper_proc_signal(h->p, (int)sig);
    ReleaseSRWLockExclusive(&n->lock);
}

/* ---------------------------------------------------------------- slow facts */

/* the admin's check of the bridge copy before an elevation: uncached, always (sha_file, never
 * sha_memo_file: it reads the very bytes about to be staged) */
int nw_file_sha256(const char *path, char out[65]) { return sha_file(path, out); }

_Static_assert(NF_SLOW_DRIVE == CORE_SLOW_DRIVE && NF_SLOW_SIZE_WALK == CORE_SLOW_SIZE_WALK && NF_SLOW_ALPHATHETA == CORE_SLOW_ALPHATHETA,
               "native_facts.h's kinds are core.h's");

/* CORE_SLOW_LOCATION (review HF5): native_location.c's bounded measure with this PC's answers.
 * CORE_SLOW_NOT_NOW keeps the memo, its size and its timer (review NW1). A late answer can serve
 * another caller still waiting on the same paths (N3R18). */
/* the measure's queue, one measure at a time for the process: a measure may outlive the core that
 * asked (its wait ran out), so neither it nor the ops below hold the core's state */
static native_location_runner nl_runner = NATIVE_LOCATION_RUNNER_INIT;

/* Check the live onnxruntime.dll in every verified rekordbox folder through compat.c's memo.
 * Read Windows' Program Files again because the measurement may outlive the core. */
static int nl_live_bridge(void *ctx, int *present, int *location) {
    (void)ctx;
    char rbs[1100];
    *present = *location = 0;
    if (plat_program_files(WR_PF_REKORDBOX, rbs, sizeof rbs)) return -1;
    return host_facts_live_bridge(compat_os_windows(), rbs, present, location);
}
/* the volume holding dir, named: plat_volume_name (its label and drive letter, through the same root
 * lookup as plat_volume_info: review NL1, NL5); native_location_drive_name picks and remembers */
static int nl_volume_name(void *ctx, const char *dir, char *label, size_t ln, char *letter, size_t lt) {
    (void)ctx;
    return plat_volume_name(dir, label, ln, letter, lt);
}
static int nl_remote(void *ctx, const char *dir) {
    (void)ctx;
    return plat_win_drive_remote(dir);
}
/* rekordboxOpen() and the process's creation time: native_choice_win.c's, which winc-location-wintest
 * runs on the VM with a stand-in rekordbox.exe (review N3R5) */
const native_location_ops nw_location_ops = {NULL, nl_live_bridge, nl_rekordbox_running, nl_process_start, nl_volume_name, nl_remote, NULL, NULL,
                                            native_pc_cache_probe};

/* the measure's rows (ShellFacts.location's, row for row) */
int nw_location_write(const native_location *l, core_facts *out) {
    int checked = native_location_checked(l);
    int local_problem = l->kind == LOC_THIS_MAC && l->probe == PROBE_MARKER_MISSING_OR_WRONG;
    core_fact_flag(out, FACT_LOC_MEASURED, checked || local_problem);
    core_fact_flag(out, FACT_REKORDBOX_RUNNING, l->rekordbox_running);
    core_fact_i64(out, FACT_LOC_PAUSED_FLOOR, checked ? l->paused_floor : -1);
    if (l->has_status_drive) core_fact_str(out, FACT_LOC_DRIVE, l->status_drive, strlen(l->status_drive));   /* the snapshot's drive: config.ini's chosen folder's */
    core_fact_i64(out, FACT_LOC_KIND, l->kind);
    if (l->kind == LOC_CHOSEN || local_problem) core_fact_i64(out, FACT_LOC_PROBE, l->probe);
    if (l->has_status_drive && (l->kind == LOC_CHOSEN || l->kind == LOC_BRIDGE_TOO_OLD))
        core_fact_str(out, FACT_LOC_STATUS_DRIVE, l->status_drive, strlen(l->status_drive));
    if (l->has_in_use) core_fact_str(out, FACT_LOC_IN_USE, l->in_use, strlen(l->in_use));
    if (l->has_bridge) {
        core_fact_str(out, FACT_LOC_BRIDGE_STATE, l->bridge_state, strlen(l->bridge_state));
        core_fact_str(out, FACT_LOC_BRIDGE_REASON, l->bridge_reason, strlen(l->bridge_reason));
        if (l->has_bridge_drive) core_fact_str(out, FACT_LOC_BRIDGE_DRIVE, l->bridge_drive, strlen(l->bridge_drive));
    }
    return 0;
}

/* the measure as CORE_SLOW_LOCATION's facts, on a core worker (view.c's loc_work_run,
 * CORE_WORK_FACT: the thread pool): native_location_slow_fact, which maps no answer this time to
 * CORE_SLOW_NOT_NOW (review NW1, NW4) */
typedef struct { const nw *n; core_facts *out; } nl_ctx;
static int nl_write(void *ctx, const native_location *l) { return nw_location_write(l, ((nl_ctx *)ctx)->out); }
static void nl_log(void *ctx, const char *line) { native_core_log(((nl_ctx *)ctx)->n->c, line); }
static int32_t nl_location_facts(const nw *n, core_facts *out) {
    nl_ctx x = {n, out};
    native_location_env e = {n->support, n->cache_default, &nw_location_ops};
    return native_location_slow_fact(&nl_runner, &e, &n->ui, NL_WAIT_NS, nl_write, nl_log, &x);
}

int nw_location_measure_now(const char *support, const char *cache_default, uint64_t wait_ns, native_location *out) {
    if (!support || !cache_default) return -1;
    native_location_env e = {support, cache_default, &nw_location_ops};
    return native_location_measure_bounded(&nl_runner, &e, NULL, wait_ns, out) ? -1 : 0;
}

/* MODEL_SHA and BRIDGE_SHA use the file memo. DRIVE, SIZE_WALK and ALPHATHETA
 * use native_facts. R1 reads the inventory and returns the worst repair state
 * and the rows for an empty key. An explicit folder is looked up in that inventory. */
static int32_t nw_slow_fact(void *st, core_slow kind, const char *key, size_t key_len, core_facts *out) {
    nw *n = st;
    if (kind == CORE_SLOW_LOCATION) return nl_location_facts(n, out);   /* key ""; -1 while the gate is off */
    if (kind != CORE_SLOW_MODEL_SHA && kind != CORE_SLOW_BRIDGE_SHA && kind != CORE_SLOW_DRIVE && kind != CORE_SLOW_SIZE_WALK
        && kind != CORE_SLOW_ALPHATHETA && kind != CORE_SLOW_R1)
        return -1;
    if (!key) return -1;
    if (key_len == CORE_NTS) key_len = strlen(key);
    if (kind == CORE_SLOW_R1 && (key_len == 0 || !memchr(key, 0, key_len))) {
        host_facts *h = malloc(sizeof *h);
        if (!h) return -1;
        int rc = host_facts_read_core(n->c, h), state = -1;
        if (!rc && !key_len) {
            core_fact_str(out, FACT_WIN_RB_ROWS, h->win_rows, CORE_NTS);
            state = h->r1_state;
            if (state > 0) core_fact_flag(out, FACT_R1_DISMISSED, h->r1_dismissed);
        } else if (!rc && key_len < COMPAT_PATH_MAX) {
            char folder[COMPAT_PATH_MAX];
            memcpy(folder, key, key_len);
            folder[key_len] = 0;
            state = native_r1_assessed(h->inventory, h->assessment, folder);
        }
        if (state >= 0) core_fact_i64(out, FACT_R1_STATE, state);
        host_facts_free(h);
        free(h);
        return state >= 0 || (!key_len && !rc) ? 0 : -1;
    }
    if (kind == CORE_SLOW_R1) return -1;
    char *path = malloc(key_len + 1);
    if (!path) return -1;
    memcpy(path, key, key_len);
    path[key_len] = 0;
    if (strlen(path) != key_len) {   /* a NUL inside the key: refused */
        free(path);
        return -1;
    }
    if (kind != CORE_SLOW_MODEL_SHA && kind != CORE_SLOW_BRIDGE_SHA) {
        /* TODO(NF2): ALPHATHETA keys with '/' separators: rewrite '/' to '\\' in path here once the core sends them */
        native_fact f;
        int e = native_fact_read(compat_os_windows(), (int)kind, path, &f);
        free(path);
        if (e) return -1;
        if (f.has_free) core_fact_i64(out, FACT_FREE_BYTES, f.free_bytes);
        if (f.has_size) core_fact_i64(out, FACT_SIZE_BYTES, f.size_bytes);
        if (f.has_signature) core_fact_flag(out, FACT_SIGNATURE_OK, f.signature_ok);
        return 0;
    }
    char hex[65];
    int e = sha_memo_file(&n->shas, path, hex);
    free(path);
    if (e) return -1;
    core_fact_str(out, FACT_SHA256, hex, 64);
    return 0;
}

/* ---------------------------------------------------------------- open, close */

static char *dup_or_null(const char *s) { return s && s[0] ? _strdup(s) : NULL; }

static void *nw_open(core *c) {
    nw *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->c = c;
    n->ui = plat_thread_self();   /* core_new's thread (native_install): the UI thread */
    InitializeSRWLock(&n->lock);
    sha_memo_init(&n->shas);
    n->shas.stat = sha_memo_win_stamp;
    const char *helper = native_core_path(c, "helper");
    n->helper = dup_or_null(helper);
    n->support = dup_or_null(native_core_path(c, "support"));   /* NULL: CORE_SLOW_LOCATION has no answer */
    n->cache_default = dup_or_null(native_core_path(c, "cache_default"));
    n->flow = plat_queue_new("rbsp.core.flow", PLAT_WORK_DEFAULT);
    n->quit = plat_queue_new("rbsp.core.quit", PLAT_WORK_DEFAULT);
    n->walk = plat_queue_new("rbsp.core.walk", PLAT_WORK_MAINT);
    n->admin = nw_admin_open(c);
    n->steps = n->admin ? nw_steps_open(c, n->admin, &n->shas) : NULL;
    const char *pf = native_core_programs(c);
    if ((helper && helper[0] && !n->helper) || !n->admin || !n->steps || !n->flow || !n->quit || !n->walk || !pf
        || wr_program_path(pf, WR_PF_REAL_ORT, n->ort, sizeof n->ort) < 0) {
        nw_steps_close(n->steps);
        nw_admin_close(n->admin);
        free(n->helper);
        free(n->support);
        free(n->cache_default);
        sha_memo_free(&n->shas);
        free(n);   /* a queue made stays the process's (never freed) */
        return NULL;
    }
    return n;
}

int native_win_programs(char *out, size_t n) { return plat_program_files(NULL, out, n); }

static void nw_close(void *st) {
    nw *n = st;
    /* every run has ended (each held the core): the list is empty; no flow runs an admin call and no
     * worker asks a slow fact */
    nw_steps_close(n->steps);
    nw_admin_close(n->admin);
    sha_memo_free(&n->shas);
    free(n->helper);
    free(n->support);
    free(n->cache_default);
    free(n);
}

/* ---------------------------------------------------------------- admin */

static int32_t nw_admin_entry(void *st, int32_t kind, const char *version, const char *bundle_id, const char *bridge_sha, const char *ort_sha, int32_t force) {
    nw *n = st;
    (void)bundle_id;   /* NULL on Windows */
    (void)ort_sha;     /* the worker reads and checks rekordbox's own files itself */
    return nw_admin_run(n->admin, kind, version, bridge_sha, force);
}

/* ---------------------------------------------------------------- step */

static int32_t nw_step(void *st, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    nw *n = st;
    return nw_steps_run(n->steps, kind, args, nargs, out);
}

const native_backend native_win_backend = {
    .open = nw_open,
    .close = nw_close,
    .post_work = nw_post_work,
    .slow_fact = nw_slow_fact,
    .helper_spawn = nw_helper_spawn,
    .helper_signal = nw_helper_signal,
    .admin = nw_admin_entry,
    .step = nw_step,
};
