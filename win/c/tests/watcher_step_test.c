/* The Windows watcher adapters with fake platform calls and the shared startup flow. */
#include "check.h"
#include "../lifecycle.h"
#include "../native_steps.h"
#include "../update.h"

#define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h supplies the platform types */
#include "internal.h"
#include "native.h"

#include <pthread.h>

typedef struct {
    core *c;
    const char *apps, *own;
    update_env ue;
} nw_steps;

static struct {
    int moved, read_error, off, ours, removed;
    int reads, writes, startup_checks;
} watcher;

int life_run_read(const char *stub, life_run *out) {
    watcher.reads++;
    if (watcher.read_error) return EACCES;
    *out = (life_run){.present = !watcher.removed, .ours = stub[0] && watcher.ours, .off = watcher.off};
    return 0;
}

int life_run_write(const char *stub) {
    CHECK(strstr(stub, "\\RB Stems Plus.exe") != NULL, "repair names the stub");
    watcher.writes++;
    watcher.ours = 1;
    return 0;
}

int life_run_remove(void) {
    watcher.removed = 1;
    watcher.off = 0;
    return 0;
}

int update_in_place(const update_env *e) {
    (void)e;
    return watcher.moved;
}

#include "watcher_steps.inc"

static void check_flag(nw_steps *s, core_step kind, int expected, const char *message) {
    core_facts f = {0};
    int rc = watcher_step(s, kind, &f);
    CHECK(rc == 0 && f.set[FACT_STEP_FLAG] && f.i[FACT_STEP_FLAG] == expected, "%s", message);
}

static void test_steps(core *c) {
    nw_steps s = {.c = c, .apps = "C:\\Apps", .own = "1.4.0"};
    watcher.moved = watcher.off = 1;
    check_flag(&s, CORE_STEP_WATCHER_CHECK, 1, "skipped check still reports reminders off");
    check_flag(&s, CORE_STEP_LAUNCH_AGENT_LOAD, 1, "skipped load still reports reminders off");
    CHECK(watcher.reads == 2 && watcher.writes == 0, "skipped repairs only read the registration");

    nw_steps no_paths = {.c = c};
    check_flag(&no_paths, CORE_STEP_WATCHER_CHECK, 1, "a check without install paths still measures the registration");
    check_flag(&no_paths, CORE_STEP_WATCHER_STATE, 1, "activation without install paths measures the same registration");

    watcher.moved = 0;
    check_flag(&s, CORE_STEP_WATCHER_CHECK, 1, "normal check reports the measured off state");
    CHECK(watcher.writes == 1, "normal check repairs the old registration");
    watcher.off = 0;
    check_flag(&s, CORE_STEP_WATCHER_CHECK, 0, "normal check reports the measured on state");

    core_facts f = {0};
    int rc = watcher_unload(&no_paths, &f);
    CHECK(rc == 0 && f.set[FACT_STEP_FLAG] && !f.i[FACT_STEP_FLAG], "unload without install paths reports the removed registration");

    watcher.read_error = 1;
    memset(&f, 0, sizeof f);
    rc = watcher_step(&s, CORE_STEP_LAUNCH_AGENT_LOAD, &f);
    CHECK(rc == 0 && !f.set[FACT_STEP_FLAG], "load still succeeds with no flag when the state is unreadable");
    rc = watcher_unload(&no_paths, &f);
    CHECK(rc == 0 && !f.set[FACT_STEP_FLAG], "unload still succeeds with no flag when the state is unreadable");
}

/* Run queued jobs after the host callbacks return, in the order they were posted. */
typedef struct {
    core_job *jobs[64];
    size_t head, tail;
} test_queue;

typedef struct {
    nw_steps steps;
    test_queue main, work;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    int working, done;
} test_host;

static void post(test_host *h, test_queue *q, core_job *job) {
    pthread_mutex_lock(&h->mutex);
    if (q->tail == sizeof q->jobs / sizeof *q->jobs) abort();
    q->jobs[q->tail++] = job;
    pthread_cond_broadcast(&h->ready);
    pthread_mutex_unlock(&h->mutex);
}

static void post_main(void *ctx, core_job *job) {
    test_host *h = ctx;
    post(h, &h->main, job);
}
static void post_work(void *ctx, core_work kind, core_job *job) {
    test_host *h = ctx;
    (void)kind;
    post(h, &h->work, job);
}

static void *worker(void *ctx) {
    test_host *h = ctx;
    pthread_mutex_lock(&h->mutex);
    while (!h->done) {
        if (h->work.head == h->work.tail) {
            pthread_cond_wait(&h->ready, &h->mutex);
            continue;
        }
        core_job *job = h->work.jobs[h->work.head++];
        h->working = 1;
        pthread_mutex_unlock(&h->mutex);
        core_run(job);
        pthread_mutex_lock(&h->mutex);
        h->working = 0;
        pthread_cond_broadcast(&h->ready);
    }
    pthread_mutex_unlock(&h->mutex);
    return NULL;
}

static void drain(test_host *h) {
    core_commit(h->steps.c);
    pthread_t thread;
    h->done = 0;
    if (pthread_create(&thread, NULL, worker, h)) abort();
    pthread_mutex_lock(&h->mutex);
    while (h->main.head < h->main.tail || h->working || h->work.head < h->work.tail) {
        if (h->main.head == h->main.tail) {
            pthread_cond_wait(&h->ready, &h->mutex);
            continue;
        }
        core_job *job = h->main.jobs[h->main.head++];
        pthread_mutex_unlock(&h->mutex);
        core_run(job);
        core_commit(h->steps.c);
        pthread_mutex_lock(&h->mutex);
    }
    h->done = 1;
    pthread_cond_broadcast(&h->ready);
    pthread_mutex_unlock(&h->mutex);
    pthread_join(thread, NULL);
}

static uint32_t front(void *ctx) { (void)ctx; return 0; }
static int32_t modal(void *ctx) { (void)ctx; return 0; }
static void facts(void *ctx, core_facts *f) { (void)ctx; (void)f; }
static void log_line(void *ctx, const char *line, size_t len) { (void)ctx; (void)line; (void)len; }
static void close_reply(void *ctx, core_close what, int32_t proceed) { (void)ctx; (void)what; (void)proceed; }
static int32_t dialog(void *ctx, const core_dialog *d, core_reply *r) { (void)ctx; (void)d; (void)r; return 1; }
static void render(void *ctx, core_snap *snap, const core_delta *delta) {
    (void)ctx;
    (void)delta;
    core_snap_release(snap);
}

static int32_t step(void *ctx, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    test_host *h = ctx;
    (void)args;
    (void)nargs;
    if (kind == CORE_STEP_WATCHER_CHECK) {
        watcher.startup_checks++;
        return watcher_step(&h->steps, kind, out);
    }
    if (kind == CORE_STEP_WELCOME) {
        core_fact_flag(out, FACT_STEP_FLAG, 1);
        return 0;
    }
    return CORE_E_STATE;
}

static int reminders_off(core *c) {
    core_snap *snap = core_snap_current(c);
    int off = snap && strstr(core_str(snap, F_STATUS_LINE, NULL), "Reinstall reminders are off");
    core_snap_release(snap);
    return off;
}

int checks, failures;

int main(void) {
    test_host h = {.steps = {.apps = "C:\\Apps", .own = "1.4.0"},
                   .mutex = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};
    core_host host = {.size = sizeof host, .ctx = &h, .render = render, .front_facts = front,
                      .modal_open = modal, .facts = facts, .post_main = post_main, .post_work = post_work,
                      .dialog = dialog, .step = step, .close_reply = close_reply, .log = log_line};
    core *c = core_new(&host, CORE_ROLE_APP, "{\"os\":\"win\",\"version\":\"1.4.0\"}", CORE_NTS);
    CHECK(c, "Windows core starts");
    if (!c) return 1;
    h.steps.c = c;
    CHECK(core_event(c, "{\"ev\":\"start\"}", CORE_NTS) == CORE_OK, "start event accepted");
    view_watcher_off(c, 1);
    drain(&h);
    CHECK(reminders_off(c), "the prior off measurement is visible");

    watcher.read_error = 1;
    CHECK(core_event(c, "{\"ev\":\"shown\"}", CORE_NTS) == CORE_OK, "startup flow starts");
    drain(&h);
    CHECK(watcher.startup_checks == 1 && reminders_off(c), "startup read failure preserves the reminders-off line");
    memset(&watcher, 0, sizeof watcher);
    test_steps(c);
    core_free(c);
    while (h.main.head < h.main.tail) core_job_drop(h.main.jobs[h.main.head++]);
    while (h.work.head < h.work.tail) core_job_drop(h.work.jobs[h.work.head++]);
    pthread_cond_destroy(&h.ready);
    pthread_mutex_destroy(&h.mutex);
    printf("watcher steps: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
