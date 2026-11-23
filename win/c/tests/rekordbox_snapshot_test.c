#include <strings.h>

#include "check.h"
#include "plat.h"
#include "core_native_steps.h"
#include "helper_proc.h"
#include "host_facts.h"
#include "native_steps.h"
#include "update.h"
#include "update_shell.h"

int checks, failures;

/* Re-enter the real worker code from a fake read to control which result finishes first. */
typedef int SRWLOCK;
typedef int64_t LONG64;
#define NS_PATH_MAX 1024
#define NS_KEYS_MAX 4
#define SRWLOCK_INIT 0
#define _stricmp strcasecmp
static void (*after_ticket)(void), (*after_saved_read)(void);

static void AcquireSRWLockExclusive(SRWLOCK *lock) { if ((*lock)++) abort(); }
static void ReleaseSRWLockExclusive(SRWLOCK *lock) { if (--(*lock)) abort(); }
#define AcquireSRWLockShared AcquireSRWLockExclusive
#define ReleaseSRWLockShared ReleaseSRWLockExclusive

static LONG64 InterlockedIncrement64(LONG64 *gen) {
    LONG64 ticket = ++*gen;
    void (*next)(void) = after_ticket;
    after_ticket = NULL;
    if (next) next();
    return ticket;
}

static const char product_version[] = "7.2.19.0342";
static const char newer_version[] = "7.2.20.0354";
static const char *const folders[] = {"C:\\rekordbox\\previous", "C:\\rekordbox\\current"};
static int current, saved = -1, installed = 1, next_saved, next_installed;
static int saved_reads, installed_reads, stats, covered_count, newest_only;
static int64_t older_mtime;

static int read_payload(payload_manifest *out, int count) {
    if (count < 0) return PAYLOAD_NONE;
    out->has_windows = 1;
    out->n_resampler_proved = count;
    strcpy(out->resampler_proved[0].product_version, newest_only ? newer_version : product_version);
    memset(out->resampler_proved[0].rekordbox_sha256, newest_only ? '8' : '7', 64);
    out->resampler_proved[0].rekordbox_sha256[64] = 0;
    if (count > 1) {
        strcpy(out->resampler_proved[1].product_version, newer_version);
        memset(out->resampler_proved[1].rekordbox_sha256, '8', 64);
        out->resampler_proved[1].rekordbox_sha256[64] = 0;
    }
    return PAYLOAD_OK;
}

void update_store_init(const update_env *e, update_store *out) { (void)e; (void)out; }

int payload_saved(const payload_files *f, payload_manifest *out) {
    (void)f;
    saved_reads++;
    int r = read_payload(out, saved);
    void (*next)(void) = after_saved_read;
    after_saved_read = NULL;
    if (next) next();
    return r;
}

int update_installed(const update_env *e, payload_manifest *out) {
    (void)e;
    installed_reads++;
    return read_payload(out, installed);
}

static compat_rekordbox candidate(int which) {
    compat_rekordbox rb = {0};
    strcpy(rb.folder, folders[which]);
    strcpy(rb.folder_version, which ? "7.2.20" : "7.2.19");
    strcpy(rb.exe.pe.product_version, which ? newer_version : product_version);
    rb.exe.mtime = which ? 0 : older_mtime;
    memset(rb.exe.sha256, '7' + which, 64);
    rb.exe.id.file[0] = which + 1;
    return rb;
}

const compat_os *compat_os_windows(void) { return NULL; }

static int fake_find_rekordbox(const compat_os *os, const char *root, compat_rekordbox *out, int max, int *cur, int *state) {
    (void)os; (void)root; (void)state;
    if (current < 0) return 0;
    if (max < 2) abort();
    *cur = current;
    out[0] = candidate(0);
    out[1] = candidate(1);
    return 2;
}

static int fake_stat_path(const char *path, int follow, plat_stat *st) {
    if (follow) abort();
    stats++;
    *st = (plat_stat){.kind = PLAT_FILE};
    st->id.file[0] = strstr(path, "\\current\\") ? 2 : 1;
    st->mtime = st->id.file[0] == 1 ? older_mtime : 0;
    return 0;
}

int host_facts_read_core(core *c, host_facts *out) {
    (void)c;
    memset(out, 0, sizeof *out);
    out->cache_answered = 1;
    out->cache_manifest = calloc(1, sizeof *out->cache_manifest);
    if (!out->cache_manifest) abort();
    if (payload_saved(NULL, out->cache_manifest) != PAYLOAD_OK
        && update_installed(NULL, out->cache_manifest) != PAYLOAD_OK) {
        free(out->cache_manifest);
        out->cache_manifest = NULL;
    }
    out->ncovered = covered_count ? covered_count : current >= 0;
    for (int i = 0; i < out->ncovered; i++) {
        int which = out->ncovered == 2 ? 1 - i : current;
        compat_rekordbox rb = candidate(which);
        host_covered_exe *exe = &out->covered[i];
        strcpy(exe->folder, rb.folder);
        strcpy(exe->product_version, rb.exe.pe.product_version);
        strcpy(exe->sha256, rb.exe.sha256);
        exe->id = rb.exe.id;
        exe->size = rb.exe.size;
        exe->mtime = rb.exe.mtime;
    }
    return 0;
}

void host_facts_free(host_facts *f) { free(f->cache_manifest); }

#define compat_find_rekordbox fake_find_rekordbox
#define plat_stat_path fake_stat_path
#include "rekordbox_snapshot.inc"
#undef plat_stat_path
#undef compat_find_rekordbox

static nw_steps steps = {.support = "support", .own = "1.0.0"};

typedef struct {
    nw_steps *steps;
    const char *ort;
    core *c;
} nw;

#include "prepare_args.inc"

static int prepare_passes(core *c, int which) {
    nw n = {.steps = &steps, .ort = "C:\\ort\\onnxruntime.dll", .c = c};
    const char *const argv[] = {"--rekordbox", "ignored", "track.wav"};
    const char *out[4];
    char rb[COMPAT_PATH_MAX] = "";
    int argc = nw_prepare_args(&n, argv, 3, 1, rb, out);
    int opt = argc - 3;
    return (opt == 0 || opt == 1) && !strcmp(out[opt], "--rekordbox") && !strcmp(out[opt + 1], folders[which]);
}

int native_step_payload(const native_steps_env *e, native_flow *f, const char *arg, native_payload_facts *out) {
    (void)e; (void)f; (void)arg;
    saved = next_saved;
    *out = (native_payload_facts){.found = saved >= 0};
    return 0;
}

static int refresh_compat_payload(nw_steps *s, const native_steps_env *e, native_payload_facts *out) {
    return native_step_payload(e, &s->flow, "fetch", out);
}

static int run_payload(nw_steps *s, const char *const *args, int nargs, core_facts *out) {
    native_steps_env e = {0};
    switch ((core_step)CORE_STEP_PAYLOAD) {
#include "rekordbox_payload_step.inc"
    default: return 1;
    }
}

static void fetch_payload(int count) {
    const char *args[] = {"fetch"};
    arena a = {0};
    core_facts out;
    facts_init(&out, &a);
    next_saved = count;
    if (run_payload(&steps, args, 1, &out)) abort();
    arena_free(&a);
}

typedef struct { update_env e; } shell_env;
static int shell_env_init(shell_env *s, const char *support, const char *app, const char *own, core_win_log_fn log, void *ctx) {
    (void)s; (void)support; (void)app; (void)own; (void)log; (void)ctx;
    return 0;
}

int update_after_render(const update_env *e) { (void)e; installed = next_installed; return 0; }

#include "rekordbox_after_render.inc"

static int known(int which, int proved) {
    char folder[COMPAT_PATH_MAX];
    int bit;
    return nw_steps_rekordbox_known(&steps, folder, sizeof folder, &bit) && bit == proved && !strcmp(folder, folders[which]);
}

static void discover(void) {
    char folder[COMPAT_PATH_MAX], v[COMPAT_VERSION_MAX];
    if (!current_folder(&steps, folder, v)) abort();
}

typedef struct {
    core_job *work[4], *main[4];
    int work_count, main_count, flow_count, request_reads;
} notification_jobs;

int64_t native_support_age(const char *support, const char *name, int64_t now) {
    (void)now;
    if (strcmp(support, steps.support) || strcmp(name, "request")) abort();
    return -1;
}

int native_support_remove(const char *support, const char *name) { (void)support; (void)name; abort(); }
static void log_failed(nw_steps *s, const char *what, int e) { (void)s; (void)what; (void)e; abort(); }

static int32_t notification_step(void *ctx, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    notification_jobs *jobs = ctx;
    nw_steps *s = &steps;
    if (kind == CORE_STEP_REQUEST) jobs->request_reads++;
    switch (kind) {
#include "rekordbox_request_step.inc"
    default: return CORE_E_STATE;
    }
}

static void post_main(void *ctx, core_job *job) {
    notification_jobs *jobs = ctx;
    if (jobs->main_count == 4) abort();
    jobs->main[jobs->main_count++] = job;
}

static void post_work(void *ctx, core_work kind, core_job *job) {
    notification_jobs *jobs = ctx;
    if (jobs->work_count == 4) abort();
    jobs->work[jobs->work_count++] = job;
    if (kind == CORE_WORK_FLOW) jobs->flow_count++;
}

static void test_notification(const char *event) {
    notification_jobs jobs = {0};
    core_life life = {.refs = 1, .alive = 1, .os = CORE_OS_WIN, .main_thread = plat_thread_self(),
                      .host = {.ctx = &jobs, .post_main = post_main, .post_work = post_work, .step = notification_step}};
    core *c = &life.c;
    c->life = &life;
    c->role = CORE_ROLE_APP;
    c->os = life.os;
    c->main_thread = life.main_thread;
    c->fl.startup_done = 1;
    if (model_init(&c->m)) abort();

    current = 0;
    discover();
    current = 1;
    LONG64 gen = steps.rb_gen;
    int accepted = core_event(c, event, CORE_NTS) == CORE_OK;
    int queued = jobs.flow_count == 1 && steps.rb_gen == gen && prepare_passes(c, 0);
    for (int i = 0; i < jobs.work_count; i++) core_run(jobs.work[i]);
    int newer = prepare_passes(c, 1);
    CHECK(accepted && queued && jobs.request_reads == 1 && steps.rb_gen == gen + 1 && newer,
          "%s queues one publishing refresh and Prepare passes the newer folder without a reinstall request", event);
    for (int i = 0; i < jobs.main_count; i++) core_job_drop(jobs.main[i]);
    arena_free(&c->scratch);
    model_free(&c->m);
}

static void fetch_empty(void) { fetch_payload(0); }

static LONG64 pending_discovery;

static void finish_discovery(void) {
    current = 0;
    compat_rekordbox rb = candidate(0);
    if (!publish_rb_folder(&steps, pending_discovery, &rb)) abort();
}

int main(void) {
    (void)e_saved;
    (void)read_installed_list;
    discover();
    CHECK(known(0, 1) && stats == 1 && saved_reads == 1 && installed_reads == 1,
          "installed fallback is proved on discovery and spawn checks one stat");

    int before = installed_reads;
    fetch_payload(0);
    CHECK(known(0, 0) && installed_reads == before, "an empty saved list supersedes a matching installed list on the payload step");
    discover();
    installed = 0;
    fetch_payload(1);
    CHECK(known(0, 1) && installed_reads == before, "a matching saved list supersedes an empty installed list on the payload step");

    char folder[COMPAT_PATH_MAX], v[COMPAT_VERSION_MAX];
    LONG64 applied = steps.rb_applied, gen = steps.rb_gen;
    before = saved_reads;
    current = 1;
    CHECK(nw_steps_rekordbox_current(&steps, folder, sizeof folder, v, sizeof v) && !strcmp(folder, folders[1]) &&
          known(0, 1) && steps.rb_applied == applied && steps.rb_gen == gen && saved_reads == before,
          "R1 returns the newly found folder and leaves the Prepare snapshot alone");

    compat_rekordbox previous = candidate(0);
    LONG64 older = InterlockedIncrement64(&steps.rb_gen);
    discover();
    CHECK(!publish_rb_folder(&steps, older, &previous) && known(1, 0), "a stale discovery cannot replace a newer folder or proof");

    current = 0;
    discover();
    gen = steps.rb_gen;
    after_ticket = fetch_empty;
    refresh_rb_proved(&steps);
    CHECK(known(0, 0) && steps.rb_applied == gen + 2 && steps.rb_gen == gen + 2,
          "an older payload worker discards its result when a newer one has published");

    fetch_payload(1);
    current = 1;
    after_saved_read = fetch_empty;
    discover();
    CHECK(known(1, 0), "discovery retries when a payload refresh publishes while discovery reads");

    fetch_payload(1);
    pending_discovery = InterlockedIncrement64(&steps.rb_gen);
    after_saved_read = finish_discovery;
    refresh_rb_proved(&steps);
    CHECK(known(0, 1), "a payload refresh retries when discovery replaces the exe it copied");

    installed = 1;
    fetch_payload(-1);
    next_installed = 0;
    steps_list = &steps;
    CHECK(core_win_update_after_render(steps.support, "app", steps.own, NULL, NULL) == 0 && known(0, 0),
          "the update handoff recomputes the bit after writing installed-release");

    installed = 1;
    fetch_payload(-1);
    installed = -1;
    fetch_payload(-1);
    CHECK(known(0, 0), "an unreadable payload and installed record clear the proof");

    test_notification("{\"ev\":\"check_request\"}");
    test_notification("{\"ev\":\"reopened\"}");

    current = 1;
    covered_count = 2;
    fetch_payload(2);
    discover();
    CHECK(known(1, 1), "the newest covered folder is used when both EXEs have exact proofs");
    newest_only = 1;
    fetch_payload(1);
    CHECK(known(1, 0), "a proof for only the newest covered EXE is insufficient");
    newest_only = 0;
    fetch_payload(2);
    CHECK(known(1, 1), "proof returns when both EXEs are proved again");
    older_mtime++;
    CHECK(known(1, 0), "changing the older covered EXE disables the flag for the newer EXE");
    discover();
    CHECK(known(1, 1), "a fresh publication picks up the older EXE's new stamp");

    printf("Rekordbox snapshot: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
