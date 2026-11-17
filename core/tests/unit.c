/* The core's unit tests (mac-core §7), white-box (internal.h), over a fake host with real threads:
 * a main queue pumped by the test's main thread, a thread per worker job (or a held list), and a
 * dialog presented "on main" while the flow thread waits, as the Mac's presenter does.
 * Built by core/build.sh test: plain and asanrel (release behaviour: no CORE_DEBUG), asan and tsan
 * (CORE_DEBUG: violations abort, checked in a forked child); for mac (as shipped) and win.
 * In CORE_DEBUG builds every test ends with the leak check: no counted object left alive.
 *
 *   core-test-...      prints one line per failed check and "<n> passed, <m> failed"; exits 1 on a failure
 */
#include "../internal.h"
#include "../ids_hash.h"
#include "../native.h"
#include "dfuzz.h"
#include "flows_test.h"
#include "win_texts.h"
#include "view_test.h"
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define UNDER_TSAN 1
#  endif
#  if __has_feature(address_sanitizer)
#    define UNDER_ASAN 1
#  endif
#endif

static int passed, failed;
#define CHECK(cond) do { if (cond) passed++; else { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
/* the leak check: every counted object released (CORE_DEBUG builds; -1 otherwise) */
#ifdef CORE_DEBUG
#  define LEAKCHECK() do { long live_ = core_debug_live_objects(); CHECK(live_ == 0); } while (0)
#else
#  define LEAKCHECK() do { long live_ = core_debug_live_objects(); CHECK(live_ == -1); } while (0)
#endif

/* ---------------------------------------------------------------- the fake host */

typedef struct item { struct item *next; core_job *job; void (*fn)(void *); void *arg; } item;
typedef struct fake fake;

struct fake {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    item *head, *tail;
    pthread_t threads[64];
    int nthreads;
    int hold_work;                  /* post_work keeps jobs in held[] instead of starting threads */
    core_job *held[16];
    int nheld;
    int closed;                     /* post_main drops synchronously (the loop has stopped) */
    int pump_in_post_main;          /* post_main runs a nested pump (a violation: E3's test) */
    int free_in_post_main;          /* ... and calls core_free there */
    /* recorded */
    int renders, facts_calls, dialogs, steps, spawns;
    core_snap *held_snap;           /* the snapshot of the last render (the shell's) */
    uint32_t last_sections;
    int last_reload[NTAB], last_old_rows[NTAB], last_nchanged[NTAB];
    int32_t last_changed0[NTAB];
    int nclose;
    int close_what[8], close_proceed[8], close_reask[8];
    char logs[64][128];
    int nlogs;
    /* scripted */
    uint32_t front;
    int modal;                      /* read on main only */
    int answer;                     /* the dialog's button */
    int reask;                      /* close_reply asks core_close_asked(what) again, from inside */
    char dlg_title[2048];           /* whole: SO12's long names */
    int dlg_buttons, dlg_default, dlg_cancel;
    int step_block, step_released;  /* the step callback blocks until released */
    int dialogs_asked;              /* dialog calls made (under mu) */
    int spawn_refuse;
    int signals;                    /* helper_signal calls: the last run and signal */
    uint64_t signal_run;
    int signal_sig;
    core *c;                        /* for the callbacks that call back in (re-entry tests) */
    void (*on_render)(fake *f);
    void (*on_facts)(fake *f);
};

static void push(fake *f, core_job *job, void (*fn)(void *), void *arg) {
    item *it = calloc(1, sizeof *it);
    it->job = job;
    it->fn = fn;
    it->arg = arg;
    pthread_mutex_lock(&f->mu);
    if (f->tail) f->tail->next = it; else f->head = it;
    f->tail = it;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->mu);
}

/* runs what is queued for main now (one "run loop turn"), one item at a time from the queue, so a
 * nested pump (inside a callback) reaches the items still waiting; returns how many ran */
static int pump(fake *f) {
    pthread_mutex_lock(&f->mu);
    int todo = 0;
    for (item *i = f->head; i; i = i->next) todo++;
    pthread_mutex_unlock(&f->mu);
    int n = 0;
    while (n < todo) {
        pthread_mutex_lock(&f->mu);
        item *it = f->head;
        if (it) {
            f->head = it->next;
            if (!f->head) f->tail = NULL;
        }
        pthread_mutex_unlock(&f->mu);
        if (!it) break;
        if (it->job) core_run(it->job); else it->fn(it->arg);
        free(it);
        n++;
    }
    return n;
}

/* pumps until cond holds, waiting for other threads' posts (at most ~5 s) */
#define PUMP_UNTIL(f, cond) do { \
    for (int tries_ = 0; tries_ < 5000 && !(cond); tries_++) { \
        if (!pump(f)) { struct timespec ts_ = {0, 1000000}; nanosleep(&ts_, NULL); } \
    } } while (0)

static void drain_drop(fake *f) {
    pthread_mutex_lock(&f->mu);
    item *it = f->head;
    f->head = f->tail = NULL;
    pthread_mutex_unlock(&f->mu);
    while (it) {
        item *next = it->next;
        if (it->job) core_job_drop(it->job);
        free(it);
        it = next;
    }
}

static void h_render(void *ctx, core_snap *snap, const core_delta *d) {
    fake *f = ctx;
    f->renders++;
    f->last_sections = core_delta_sections(d);
    for (int t = 0; t < CORE_TABLE_COUNT; t++) {
        int32_t n;
        const int32_t *ch = core_delta_changed(d, (core_table)t, &n);
        f->last_reload[t] = core_delta_reload(d, (core_table)t);
        f->last_old_rows[t] = core_delta_old_rows(d, (core_table)t);
        f->last_nchanged[t] = n;
        f->last_changed0[t] = n ? ch[0] : -1;
        for (int32_t i = 1; i < n; i++) if (ch[i] <= ch[i - 1]) f->last_nchanged[t] = -1;   /* must be ascending */
    }
    core_snap_release(f->held_snap);   /* the previous one, after the new one is "applied" */
    f->held_snap = snap;
    if (f->on_render) f->on_render(f);
}

static uint32_t h_front(void *ctx) { return ((fake *)ctx)->front; }
static int32_t h_modal_open(void *ctx) { return ((fake *)ctx)->modal; }

static void h_facts(void *ctx, core_facts *out) {
    fake *f = ctx;
    f->facts_calls++;
    core_fact_flag(out, FACT_STEMS_PLUS_PRESENT, 1);
    core_fact_str(out, FACT_CHOSEN_LOCATION, "/Volumes/GLS", CORE_NTS);
    if (f->on_facts) f->on_facts(f);
}

static int pump(fake *f);
static void h_post_main(void *ctx, core_job *job) {
    fake *f = ctx;
    if (f->closed) {   /* the loop has stopped: dropped synchronously */
        core_job_drop(job);
        return;
    }
    push(f, job, NULL, NULL);
    if (f->pump_in_post_main) pump(f);
    if (f->free_in_post_main) core_free(f->c);
}

static void *worker(void *p) {
    core_run(p);
    return NULL;
}

static void h_post_work(void *ctx, core_work kind, core_job *job) {
    fake *f = ctx;
    (void)kind;
    pthread_mutex_lock(&f->mu);
    if (f->closed) {
        pthread_mutex_unlock(&f->mu);
        core_job_drop(job);
        return;
    }
    if (f->hold_work) {
        f->held[f->nheld++] = job;
        pthread_mutex_unlock(&f->mu);
        return;
    }
    int i = f->nthreads++;
    pthread_create(&f->threads[i], NULL, worker, job);
    pthread_mutex_unlock(&f->mu);
}

/* the dialog: presented "on main" (a closure in the main queue) while the flow thread waits */
typedef struct { fake *f; const core_dialog *d; core_reply *r; pthread_mutex_t mu; pthread_cond_t cv; int done; } present;

static void present_on_main(void *p) {
    present *pr = p;
    fake *f = pr->f;
    f->dialogs++;
    snprintf(f->dlg_title, sizeof f->dlg_title, "%s", core_dlg_text(pr->d, CORE_DLG_TITLE, 0, 0, NULL));
    f->dlg_buttons = core_dlg_int(pr->d, CORE_DLG_N_BUTTONS, 0, 0);
    f->dlg_default = core_dlg_int(pr->d, CORE_DLG_DEFAULT, 0, 0);
    f->dlg_cancel = core_dlg_int(pr->d, CORE_DLG_CANCEL, 0, 0);
    core_reply_button(pr->r, f->answer);
    pthread_mutex_lock(&pr->mu);
    pr->done = 1;
    pthread_cond_signal(&pr->cv);
    pthread_mutex_unlock(&pr->mu);
}

static int32_t h_dialog(void *ctx, const core_dialog *d, core_reply *r) {
    fake *f = ctx;
    present pr = {ctx, d, r, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
    pthread_mutex_lock(&f->mu);
    f->dialogs_asked++;
    pthread_mutex_unlock(&f->mu);
    push(ctx, NULL, present_on_main, &pr);
    pthread_mutex_lock(&pr.mu);
    while (!pr.done) pthread_cond_wait(&pr.cv, &pr.mu);
    pthread_mutex_unlock(&pr.mu);
    return 0;
}

static int32_t h_step(void *ctx, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    fake *f = ctx;
    (void)kind; (void)args; (void)nargs; (void)out;
    pthread_mutex_lock(&f->mu);
    f->steps++;
    while (f->step_block && !f->step_released) pthread_cond_wait(&f->cv, &f->mu);
    pthread_mutex_unlock(&f->mu);
    return 0;
}

static int32_t h_spawn(void *ctx, uint64_t run, const char *const *argv, int32_t argc) {
    fake *f = ctx;
    (void)run; (void)argv; (void)argc;
    f->spawns++;
    return f->spawn_refuse;
}

static void h_signal(void *ctx, uint64_t run, core_helper_sig sig) {
    fake *f = ctx;
    f->signals++;
    f->signal_run = run;
    f->signal_sig = (int)sig;
}

static void h_close_reply(void *ctx, core_close what, int32_t proceed) {
    fake *f = ctx;
    if (f->nclose < 8) {
        f->close_what[f->nclose] = what;
        f->close_proceed[f->nclose] = proceed;
        /* WPF: Window.Close raises Closing again, which asks again; posted on the Mac, but the
         * core must answer NOW to it even from here */
        f->close_reask[f->nclose] = f->reask ? core_close_asked(f->c, what) : -1;
        f->nclose++;
    }
}

static void h_log(void *ctx, const char *line, size_t len) {
    fake *f = ctx;
    pthread_mutex_lock(&f->mu);
    if (f->nlogs < 64) {
        snprintf(f->logs[f->nlogs], sizeof f->logs[0], "%.*s", (int)len, line);
        f->nlogs++;
    }
    pthread_mutex_unlock(&f->mu);
}

static int steps_of(fake *f) {
    pthread_mutex_lock(&f->mu);
    int n = f->steps;
    pthread_mutex_unlock(&f->mu);
    return n;
}

static int logged(fake *f, const char *line) {
    pthread_mutex_lock(&f->mu);
    int hit = 0;
    for (int i = 0; i < f->nlogs; i++) hit |= !strcmp(f->logs[i], line);
    pthread_mutex_unlock(&f->mu);
    return hit;
}

static core_host host_for(fake *f) {
    core_host h;
    memset(&h, 0, sizeof h);
    h.size = sizeof h;
    h.ctx = f;
    h.render = h_render;
    h.front_facts = h_front;
    h.modal_open = h_modal_open;
    h.facts = h_facts;
    h.post_main = h_post_main;
    h.post_work = h_post_work;
    h.dialog = h_dialog;
    h.step = h_step;
    h.helper_spawn = h_spawn;
    h.helper_signal = h_signal;
    h.close_reply = h_close_reply;
    h.log = h_log;
    return h;
}

static void fake_init(fake *f) {
    memset(f, 0, sizeof *f);
    pthread_mutex_init(&f->mu, NULL);
    pthread_cond_init(&f->cv, NULL);
    f->front = CORE_FRONT_ACTIVE | CORE_FRONT_WINDOW_LOADED | (CORE_TAB_PREPARE << CORE_FRONT_TAB_SHIFT);
}

static void join_workers(fake *f) {
    for (int i = 0; i < f->nthreads; i++) pthread_join(f->threads[i], NULL);
    f->nthreads = 0;
}

static void fake_end(fake *f) {
    join_workers(f);
    for (int i = 0; i < f->nheld; i++) core_job_drop(f->held[i]);
    f->nheld = 0;
    drain_drop(f);
    core_snap_release(f->held_snap);
    f->held_snap = NULL;
    pthread_mutex_destroy(&f->mu);
    pthread_cond_destroy(&f->cv);
}

static const char *CONFIG = "{\"version\":\"1.4.0\",\"os\":\"mac\",\"paths\":{\"home\":\"/Users/x\",\"support\":\"/Users/x/Library/Application Support/rbstemsplus\"}}";

static core *new_core(fake *f) {
    core_host h = host_for(f);
    core *c = core_new(&h, CORE_ROLE_APP, CONFIG, CORE_NTS);
    f->c = c;
    return c;
}

/* CORE_DEBUG: fn(arg) in a forked child must abort (a violation). Not under TSan (no fork). */
static __attribute__((unused)) int aborts(void (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        (void)!freopen("/dev/null", "w", stderr);
        fn();
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}

/* ---------------------------------------------------------------- tests */

static void t_selftest(void) {
    CHECK(core_selftest() == 0);
    CHECK(core_api_version() == CORE_API_VERSION);
    CHECK(core_ids_hash() == ids_hash_compute() && core_ids_hash() != 0 && core_ids_hash() == core_ids_hash());
    LEAKCHECK();
}

static void t_new_refusals(void) {
    fake f;
    fake_init(&f);
    core_host h = host_for(&f);
    CHECK(core_new(NULL, CORE_ROLE_APP, CONFIG, CORE_NTS) == NULL);
    core_host small = h;
    small.size = sizeof h - 8;
    CHECK(core_new(&small, CORE_ROLE_APP, CONFIG, CORE_NTS) == NULL);
    core_host norender = h;
    norender.render = NULL;
    CHECK(core_new(&norender, CORE_ROLE_APP, CONFIG, CORE_NTS) == NULL);
    CHECK(core_new(&h, (core_role)7, CONFIG, CORE_NTS) == NULL);
    CHECK(core_new(&h, CORE_ROLE_APP, NULL, 0) == NULL);                                  /* a config is required */
    CHECK(core_new(&h, CORE_ROLE_APP, "{\"version\":\"1.4.0\"}", CORE_NTS) == NULL);     /* and "os" in it */
    CHECK(core_new(&h, CORE_ROLE_WATCHER, "{}", CORE_NTS) == NULL);
    core *w = core_new(&norender, CORE_ROLE_WATCHER, "{\"os\":\"mac\"}", CORE_NTS);   /* the watcher needs no render */
    CHECK(w != NULL);
    CHECK(core_snap_current(w) == NULL);
    core_free(w);
    core_host nostep = norender;
    nostep.step = NULL;
    CHECK(core_new(&nostep, CORE_ROLE_WATCHER, "{\"os\":\"mac\"}", CORE_NTS) == NULL);  /* but it needs step */
    core_host nodialog = norender;
    nodialog.dialog = NULL;
    CHECK(core_new(&nodialog, CORE_ROLE_WATCHER, "{\"os\":\"mac\"}", CORE_NTS) == NULL);
    CHECK(core_new(&h, CORE_ROLE_APP, "{\"os\":\"mac\",\"bogus\":1}", CORE_NTS) == NULL);
    CHECK(core_new(&h, CORE_ROLE_APP, "{\"os\":\"mac\",\"paths\":{\"nope\":\"/\"}}", CORE_NTS) == NULL);
    CHECK(core_new(&h, CORE_ROLE_APP, "{\"version\":", CORE_NTS) == NULL);
#ifdef CORE_WIN_POLICY
    core *cw = core_new(&h, CORE_ROLE_APP, "{\"os\":\"win\"}", CORE_NTS);
    CHECK(cw != NULL && cw->os == CORE_OS_WIN);
    core_free(cw);
    /* "native": a listed entry must be NULL in the table, an unlisted required one present */
    CHECK(core_new(&h, CORE_ROLE_APP, "{\"os\":\"win\",\"native\":[\"post_work\"]}", CORE_NTS) == NULL);
    core_host nat = h;
    nat.post_work = NULL;
    nat.step = NULL;
    nat.helper_spawn = NULL;
    nat.helper_signal = NULL;   /* "helper" names both helper entries */
    core *cn = core_new(&nat, CORE_ROLE_APP, "{\"os\":\"win\",\"native\":[\"post_work\",\"step\",\"helper\"]}", CORE_NTS);
    CHECK(cn != NULL && cn->native == (NATIVE_POST_WORK | NATIVE_STEP | NATIVE_HELPER));
    CHECK(core_new(&nat, CORE_ROLE_APP, "{\"os\":\"win\"}", CORE_NTS) == NULL);   /* post_work NULL, not native */
    CHECK(core_new(&nat, CORE_ROLE_APP, "{\"os\":\"win\",\"native\":[\"post_work\",\"post_work\"]}", CORE_NTS) == NULL);
    core_free(cn);
#else
    CHECK(core_new(&h, CORE_ROLE_APP, "{\"os\":\"win\"}", CORE_NTS) == NULL);                         /* no Windows policy in a Mac build */
    CHECK(core_new(&h, CORE_ROLE_APP, "{\"os\":\"mac\",\"native\":[\"post_work\"]}", CORE_NTS) == NULL); /* "native" is unknown here */
    CHECK(core_new(&h, CORE_ROLE_APP, "{\"os\":\"mac\",\"prepare_tabs\":true}", CORE_NTS) == NULL);   /* so is "prepare_tabs" */
#endif
    core *c = new_core(&f);
    CHECK(c != NULL && c->paths[1] && !strcmp(c->paths[1], "/Users/x/Library/Application Support/rbstemsplus"));
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

static void t_events(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    CHECK(core_event(c, "{\"ev\":\"start\"}", CORE_NTS) == CORE_OK && f.facts_calls >= 1);
    int calls = f.facts_calls;
    CHECK(core_event(c, "{\"ev\":\"activated\"}", CORE_NTS) == CORE_OK && f.facts_calls > calls);
    /* today's mapping (C39): deminiaturize is a quiet re-check (M4: the tab in front checks), reopen
     * is checkRequest: no refresh */
    CHECK(core_event(c, "{\"ev\":\"deminiaturized\"}", CORE_NTS) == CORE_OK);
    calls = f.facts_calls;
    CHECK(core_event(c, "{\"ev\":\"reopened\"}", CORE_NTS) == CORE_OK);
    CHECK(f.facts_calls == calls);
    CHECK(core_event(c, "{\"ev\":\"tab\",\"tab\":2}", CORE_NTS) == CORE_OK && model_get_i64(&c->m, F_TAB) == CORE_TAB_MANAGE);
    CHECK(core_event(c, "{\"ev\":\"tab\",\"tab\":3}", CORE_NTS) == CORE_E_ARG);
    CHECK(core_event(c, "{\"ev\":\"tab\",\"tab\":\"1\"}", CORE_NTS) == CORE_E_ARG);
    CHECK(core_event(c, "{\"ev\":\"tab\",\"bogus\":1}", CORE_NTS) == CORE_E_UNKNOWN);
    CHECK(core_event(c, "{\"ev\":\"nope\"}", CORE_NTS) == CORE_E_UNKNOWN);
    CHECK(core_event(c, "{\"ev\":1}", CORE_NTS) == CORE_E_PARSE);
    CHECK(core_event(c, "{\"id\":\"x\"}", CORE_NTS) == CORE_E_PARSE);
    CHECK(core_event(c, "[]", CORE_NTS) == CORE_E_PARSE);
    CHECK(core_event(c, "{\"ev\":\"activated\"", CORE_NTS) == CORE_E_PARSE);
    const char exact[] = "{\"ev\":\"activated\"}garbage";
    calls = f.facts_calls;
    CHECK(core_event(c, exact, 18) == CORE_OK && f.facts_calls > calls);   /* by length, not NUL */
    /* a Windows path, escaped as the shell's escaper must (C37) */
    CHECK(core_event(c, "{\"ev\":\"mount\",\"path\":\"D:\\\\Music\\\\\"}", CORE_NTS) == CORE_OK);
    CHECK(core_event(c, "{\"ev\":\"mount\",\"path\":\"D:\\Music\"}", CORE_NTS) == CORE_E_PARSE);   /* unescaped */
    CHECK(core_event(c, "{\"ev\":\"mount\"}", CORE_NTS) == CORE_E_ARG);
    CHECK(core_event(c, "{\"ev\":\"rename\",\"path\":\"/Volumes/GLS\",\"name\":\"GLS 2\"}", CORE_NTS) == CORE_OK);
    CHECK(core_event(c, "{\"ev\":\"until\",\"date\":\"2026-12-01\"}", CORE_NTS) == CORE_OK);
    CHECK(core_event(c, "{\"ev\":\"watch\"}", CORE_NTS) == CORE_OK && core_event(c, "{\"ev\":\"shown\"}", CORE_NTS) == CORE_OK);
    /* row events carry the list_gen the shell rendered (D9) */
    core_test_table_reset(c, T_PREP_OUTLINE, 5);
    char ev[128];
    uint64_t gen = c->m.tab[T_PREP_OUTLINE]->list_gen;
    snprintf(ev, sizeof ev, "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":3,\"gen\":%llu}", (unsigned long long)gen);
    CHECK(core_event(c, ev, CORE_NTS) == CORE_OK && model_get_i64(&c->m, F_PREP_SELECTED_ROW) == 3);
    core_test_table_reset(c, T_PREP_OUTLINE, 5);   /* a new list arrives before the click is handled */
    snprintf(ev, sizeof ev, "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":1,\"gen\":%llu}", (unsigned long long)gen);
    CHECK(core_event(c, ev, CORE_NTS) == CORE_OK && model_get_i64(&c->m, F_PREP_SELECTED_ROW) == 3);   /* stale: ignored */
    CHECK(logged(&f, "core: an event for a list no longer shown is ignored"));
    gen = c->m.tab[T_PREP_OUTLINE]->list_gen;
    snprintf(ev, sizeof ev, "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":5,\"gen\":%llu}", (unsigned long long)gen);
    CHECK(core_event(c, ev, CORE_NTS) == CORE_E_ARG);   /* out of range */
    snprintf(ev, sizeof ev, "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":-1,\"gen\":%llu}", (unsigned long long)gen);
    CHECK(core_event(c, ev, CORE_NTS) == CORE_OK && model_get_i64(&c->m, F_PREP_SELECTED_ROW) == -1);
    CHECK(core_event(c, "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":1}", CORE_NTS) == CORE_E_ARG);   /* no gen */
    CHECK(core_event(c, "{\"ev\":\"select\",\"table\":\"nope\",\"row\":1,\"gen\":1}", CORE_NTS) == CORE_E_ARG);
    snprintf(ev, sizeof ev, "{\"ev\":\"folder\",\"row\":2,\"open\":true,\"gen\":%llu}", (unsigned long long)gen);
    CHECK(core_event(c, ev, CORE_NTS) == CORE_OK);
    snprintf(ev, sizeof ev, "{\"ev\":\"folder\",\"row\":2,\"open\":1,\"gen\":%llu}", (unsigned long long)gen);
    CHECK(core_event(c, ev, CORE_NTS) == CORE_E_ARG);
    CHECK(logged(&f, "core: an unknown event is ignored") && logged(&f, "core: a malformed event is ignored"));
    CHECK(c->scratch.used_total == 0);   /* the per-event scratch is reset */
    PUMP_UNTIL(&f, !c->flow_running);      /* "shown" started the start-up flow (M6): its dialogs are shown on main */
    join_workers(&f);
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

static void t_commit_turn(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    core_snap *first = core_snap_current(c);
    for (int i = 0; i < 100; i++) {
        char b[32];
        snprintf(b, sizeof b, "line %d", i);
        CHECK(core_test_set_str(c, F_STATUS_LINE, b, CORE_NTS) == CORE_OK);
    }
    CHECK(f.renders == 0);   /* nothing renders inside the turn */
    pump(&f);
    CHECK(f.renders == 1);   /* one render for the turn */
    CHECK(!strcmp(core_str(f.held_snap, F_STATUS_LINE, NULL), "line 99"));
    CHECK(f.last_sections == (1u << CORE_SECTION_COUNT) - 1);   /* the first render: everything */
    CHECK(f.last_reload[T_PREP_TRACKS] == 1);
    uint64_t app = core_snap_rev(f.held_snap, S_APP), status = core_snap_rev(f.held_snap, S_STATUS);
    CHECK(app != core_snap_rev(first, S_APP) && status == core_snap_rev(first, S_STATUS));
    /* the same value again: nothing moves, and nothing renders (C34) */
    CHECK(core_test_set_str(c, F_STATUS_LINE, "line 99", CORE_NTS) == CORE_OK);
    core_mark_changed(c);
    pump(&f);
    CHECK(f.renders == 1 && core_snap_rev(f.held_snap, S_APP) == app);
    CHECK(core_test_set_i64(c, F_SPINNER, 5) == CORE_OK && core_test_set_f64(c, F_PREP_PROGRESS, 0.25) == CORE_OK);
    CHECK(core_test_set_i64(c, F_LIGHT_PLUS_LOOK, -3) == CORE_OK);
    pump(&f);
    CHECK(f.renders == 2);
    CHECK(core_flag(f.held_snap, F_SPINNER) == 1 && core_f64(f.held_snap, F_PREP_PROGRESS) == 0.25 && core_i64(f.held_snap, F_LIGHT_PLUS_LOOK) == -3);
    CHECK(f.last_sections == ((1u << S_APP) | (1u << S_PREPARE)));
    CHECK(core_snap_rev(f.held_snap, S_APP) > app && core_snap_rev(f.held_snap, S_STATUS) == status);
    /* core_commit at a drain boundary (D1): renders now; the queued commit job is then a no-op */
    CHECK(core_test_set_str(c, F_NOTICE, "drained", CORE_NTS) == CORE_OK);
    core_commit(c);
    CHECK(f.renders == 3 && !strcmp(core_str(f.held_snap, F_NOTICE, NULL), "drained"));
    pump(&f);
    CHECK(f.renders == 3);
    core_commit(c);   /* nothing changed: nothing */
    CHECK(f.renders == 3);
    /* a snapshot for the probe doesn't consume the render's changes */
    CHECK(core_test_set_str(c, F_NOTICE, "probe", CORE_NTS) == CORE_OK);
    core_snap *probe = core_snap_current(c);
    core_commit(c);
    CHECK(f.renders == 4 && !strcmp(core_str(f.held_snap, F_NOTICE, NULL), "probe") && (f.last_sections & (1u << S_APP)));
    core_snap_release(probe);
    /* the first snapshot still reads its own values */
    CHECK(!strcmp(core_str(first, F_STATUS_LINE, NULL), "") && core_flag(first, F_SPINNER) == 0);
    core_snap_release(first);
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

/* the G1 API spike's shape, in C: 5,000 rows drawn, 5,000 row deltas taken */
static void t_rows_and_deltas(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    enum { N = 5000, PER_TURN = 37 };
    CHECK(core_test_table_reset(c, T_PREP_TRACKS, N) == CORE_OK);
    for (int i = 0; i < N; i++) {
        char b[32];
        snprintf(b, sizeof b, "Track %d", i);
        core_test_cell_str(c, C_TRK_NAME, i, b, CORE_NTS);
    }
    pump(&f);
    CHECK(f.renders == 1 && core_rows(f.held_snap, T_PREP_TRACKS) == N && f.last_reload[T_PREP_TRACKS] == 1);
    uint64_t gen = core_list_gen(f.held_snap, T_PREP_TRACKS);
    core_snap *before = f.held_snap;
    core_snap_retain(before);
    int total = 0, renders0 = f.renders, ok_rows = 1;
    for (int i = 0; i < N; i++) {
        core_test_cell_str(c, C_TRK_RESULT, i, "Ready", CORE_NTS);
        core_test_cell_str(c, C_TRK_RESULT, i, "To prepare", CORE_NTS);   /* twice in a turn: one delta entry */
        if (i % PER_TURN == PER_TURN - 1 || i == N - 1) {
            pump(&f);
            int32_t n;
            const int32_t *ch = core_delta_changed(&f.held_snap->delta, T_PREP_TRACKS, &n);
            total += n;
            if (!ch || ch[n - 1] != i || f.last_reload[T_PREP_TRACKS] || f.last_nchanged[T_PREP_TRACKS] < 0) ok_rows = 0;
            if (core_list_gen(f.held_snap, T_PREP_TRACKS) != gen || core_rows(f.held_snap, T_PREP_TRACKS) != N) ok_rows = 0;
        }
    }
    CHECK(ok_rows);
    CHECK(total == N);
    CHECK(f.renders - renders0 == (N + PER_TURN - 1) / PER_TURN);
    CHECK(!strcmp(core_cell_str(before, C_TRK_RESULT, 4999, NULL), "") && !strcmp(core_cell_str(f.held_snap, C_TRK_RESULT, 4999, NULL), "To prepare"));
    CHECK(!strcmp(core_cell_str(f.held_snap, C_TRK_NAME, 1234, NULL), "Track 1234"));
    CHECK(core_row_rev(before, T_PREP_TRACKS, 7) != core_row_rev(f.held_snap, T_PREP_TRACKS, 7));
    core_snap_release(before);
    CHECK(core_test_table_append(c, T_PREP_TRACKS, 10) == CORE_OK);
    CHECK(core_test_cell_str(c, C_TRK_NAME, N + 3, "new", CORE_NTS) == CORE_OK);
    CHECK(core_test_cell_str(c, C_TRK_NAME, 2, "changed", CORE_NTS) == CORE_OK);
    pump(&f);
    CHECK(core_rows(f.held_snap, T_PREP_TRACKS) == N + 10 && f.last_old_rows[T_PREP_TRACKS] == N);
    CHECK(f.last_nchanged[T_PREP_TRACKS] == 1 && f.last_changed0[T_PREP_TRACKS] == 2 && !f.last_reload[T_PREP_TRACKS]);
    CHECK(!strcmp(core_cell_str(f.held_snap, C_TRK_NAME, N + 3, NULL), "new"));
    CHECK(core_test_cell_str(c, C_TRK_NAME, N + 10, "x", CORE_NTS) == CORE_E_ARG);
    CHECK(core_test_cell_str(c, C_TRK_NAME, -1, "x", CORE_NTS) == CORE_E_ARG);
    CHECK(core_test_table_reset(c, T_PREP_TRACKS, 2) == CORE_OK);
    CHECK(core_test_cell_str(c, C_TRK_NAME, 1, "after", CORE_NTS) == CORE_OK);
    pump(&f);
    CHECK(f.last_reload[T_PREP_TRACKS] == 1 && f.last_nchanged[T_PREP_TRACKS] == 0 && core_list_gen(f.held_snap, T_PREP_TRACKS) != gen);
    CHECK(core_rows(f.held_snap, T_PREP_TRACKS) == 2 && f.last_reload[T_STATUS_SETS] == 0);
    CHECK(core_test_table_reset(c, T_PREP_OUTLINE, 3) == CORE_OK);
    CHECK(core_test_cell_i64(c, C_OUT_DEPTH, 2, 4) == CORE_OK && core_test_cell_i64(c, C_OUT_OPEN, 1, 9) == CORE_OK);
    pump(&f);
    CHECK(core_cell_i64(f.held_snap, C_OUT_DEPTH, 2) == 4 && core_cell_flag(f.held_snap, C_OUT_OPEN, 1) == 1 && core_cell_flag(f.held_snap, C_OUT_OPEN, 0) == 0);
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

/* ASan + P10's scenario (§3.10): three 5,000-row estimates, their snapshots released out of order */
static void t_release_order(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    core_snap *s[3];
    for (int k = 0; k < 3; k++) {
        core_test_table_reset(c, T_PREP_TRACKS, 5000);
        for (int i = 0; i < 5000; i += 1 + k) {
            char b[24];
            snprintf(b, sizeof b, "e%d r%d", k, i);
            core_test_cell_str(c, C_TRK_WHY, i, b, CORE_NTS);
        }
        pump(&f);
        s[k] = f.held_snap;
        core_snap_retain(s[k]);
    }
    core_snap_release(s[1]);
    CHECK(!strcmp(core_cell_str(s[0], C_TRK_WHY, 4998, NULL), "e0 r4998"));
    core_free(c);   /* the core goes before the snapshots the shell still holds */
    core_snap_release(s[2]);
    CHECK(!strcmp(core_cell_str(s[0], C_TRK_WHY, 10, NULL), "e0 r10"));
    core_snap_release(s[0]);
    fake_end(&f);
    LEAKCHECK();
}

/* snapshots released on another thread while main writes and commits (a C# finalizer, a Swift
 * deinit): TSan's case (C29) */
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; core_snap *q[64]; int n, done; } relq;
static void *releaser(void *p) {
    relq *r = p;
    for (;;) {
        pthread_mutex_lock(&r->mu);
        while (!r->n && !r->done) pthread_cond_wait(&r->cv, &r->mu);
        if (!r->n && r->done) { pthread_mutex_unlock(&r->mu); return NULL; }
        core_snap *s = r->q[--r->n];
        pthread_mutex_unlock(&r->mu);
        (void)core_rows(s, T_PREP_TRACKS);
        (void)core_cell_str(s, C_TRK_NAME, 0, NULL);
        core_snap_release(s);
    }
}
static void t_release_off_main(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    relq r = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {0}, 0, 0};
    pthread_t t;
    pthread_create(&t, NULL, releaser, &r);
    core_test_table_reset(c, T_PREP_TRACKS, 500);
    for (int i = 0; i < 2000; i++) {
        core_test_cell_str(c, C_TRK_NAME, i % 500, i & 1 ? "odd" : "even", CORE_NTS);
        core_test_set_i64(c, F_LIGHT_CACHE_LOOK, i);
        core_commit(c);
        core_snap *s = f.held_snap;   /* hand the render's snapshot to the other thread */
        f.held_snap = NULL;
        pthread_mutex_lock(&r.mu);
        if (s && r.n < 64) { r.q[r.n++] = s; s = NULL; }
        pthread_cond_signal(&r.cv);
        pthread_mutex_unlock(&r.mu);
        core_snap_release(s);
    }
    pthread_mutex_lock(&r.mu);
    r.done = 1;
    pthread_cond_signal(&r.cv);
    pthread_mutex_unlock(&r.mu);
    pthread_join(t, NULL);
    CHECK(f.renders == 2000);
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

/* C13: a field set from the live section's own string, while the section compacts */
static void t_self_alias(void) {
    model m;
    CHECK(model_init(&m) == CORE_OK);
    /* No snapshot held, a different 3,999-byte value each pass, and F_NOTICE set from the live
     * section's own copy of it: past 64 KiB of garbage the section compacts (a clone) inside the
     * aliasing write itself (R6). Reverting C13 makes ASan fail here. */
    char big[4000];
    big[sizeof big - 1] = 0;
    int ok = 1, compacted_while_aliased = 0;
    const field_meta *fl = &core_field_meta[F_STATUS_LINE], *fn = &core_field_meta[F_NOTICE];
    for (int i = 0; i < 100; i++) {
        memset(big, 'a' + i % 26, sizeof big - 1);
        ok &= model_set_str(&m, F_STATUS_LINE, big, CORE_NTS) == CORE_OK;
        const section_obj *before = m.sec[fl->section];
        const cell *x = &before->cells[fl->index];
        ok &= model_set_str(&m, F_NOTICE, x->v.s, x->len) == CORE_OK;
        compacted_while_aliased += m.sec[fn->section] != before;
        const cell *y = &m.sec[fn->section]->cells[fn->index];
        ok &= y->len == sizeof big - 1 && y->v.s[0] == 'a' + i % 26 && y->v.s[sizeof big - 2] == 'a' + i % 26;
    }
    CHECK(ok);
    CHECK(compacted_while_aliased > 0);
    model_free(&m);
    LEAKCHECK();
}

static void t_dump(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    core_test_set_str(c, F_NOTICE, "a \"quote\"\n\x01", CORE_NTS);
    core_test_set_f64(c, F_PREP_PROGRESS, 0.5);
    core_test_table_reset(c, T_STATUS_SETS, 2);
    core_test_cell_str(c, C_SET_NAME, 1, "second", CORE_NTS);
    core_snap *s = core_snap_current(c);
    size_t len = 0;
    CHECK(core_snap_dump(s, NULL, 0, &len) == CORE_E_SPACE && len > 0);
    char *b = malloc(len + 1);
    CHECK(core_snap_dump(s, b, len + 1, &len) == CORE_OK && strlen(b) == len);
    arena a = {0};
    jval v;
    CHECK(json_parse(&a, b, len, &v) == CORE_OK);
    const jval *notice = json_get(json_get(json_get(&v, "sections"), "S_APP"), "F_NOTICE");
    CHECK(notice && notice->type == J_STR && !strcmp(notice->s, "a \"quote\"\n\x01"));
    const jval *rows = json_get(json_get(json_get(&v, "tables"), "T_STATUS_SETS"), "rows");
    CHECK(rows && rows->type == J_ARR && rows->n == 2 && !strcmp(json_get(&rows->items[1], "C_SET_NAME")->s, "second"));
    CHECK(strstr(b, "\"F_PREP_PROGRESS\":0.5") != NULL);
    CHECK(json_get(json_get(&v, "sections"), "S_LOG") == NULL);   /* the log is the shell's (D10) */
    char small[8];
    CHECK(core_snap_dump(s, small, sizeof small, &len) == CORE_E_SPACE && strlen(small) == 7);
    arena_free(&a);
    free(b);
    core_snap_release(s);
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

/* every text formats with exactly its own slots; slot values are never templates */
static void t_texts(void) {
    CHECK(core_text_check_all() == 0);
    int all_ok = 1;
    for (int id = 0; id < CORE_TEXT_COUNT; id++) {
        const char *t = core_text_template(CORE_OS_MAC, id);
        if (!*t) continue;   /* a Windows-only row (CORE_WIN_POLICY): win_texts.c formats those */
        core_slot s[CORE_SLOT_COUNT];
        int n = 0;
        for (const char *p = t; (p = strchr(p, '{')); p++) {
            const char *e = strchr(p, '}');
            for (int k = 0; k < CORE_SLOT_COUNT; k++)
                if (strlen(core_slot_names[k]) == (size_t)(e - p - 1) && !memcmp(core_slot_names[k], p + 1, (size_t)(e - p - 1))) {
                    int dup = 0;
                    for (int j = 0; j < n; j++) dup |= s[j].id == k;
                    if (!dup) s[n++] = (core_slot){k, 0, "{version}%s%n", CORE_NTS};
                }
        }
        char b[2048];
        size_t len;
        if (core_text(CORE_OS_MAC, (core_text_id)id, s, n, b, sizeof b, &len) != CORE_OK || strlen(b) != len) all_ok = 0;
        if (n && !strstr(b, "{version}%s%n")) all_ok = 0;
    }
    CHECK(all_ok);
    char b[64];
    size_t len;
    core_slot two[2] = {{CORE_SLOT_drive, 0, "GLS", 3}, {CORE_SLOT_gb, 0, "20", CORE_NTS}};
    char big[256];
    CHECK(core_text(CORE_OS_MAC, T_CACHE_PAUSED, two, 2, big, sizeof big, &len) == CORE_OK);
    CHECK(!strcmp(big, "Stems Cache stopped saving stems: \"GLS\" has less than 20 GB free."));
    /* errors leave "" in the buffer, after pieces were copied too (C15) */
    memset(b, 'X', sizeof b);
    core_slot unused = {CORE_SLOT_drive, 0, "a", 1};
    CHECK(core_text(CORE_OS_MAC, T_OK, &unused, 1, b, 8, &len) == CORE_E_ARG && strlen(b) == 0 && len == 0);
    memset(b, 'X', sizeof b);
    CHECK(core_text(CORE_OS_MAC, T_CACHE_PAUSED, two, 1, b, sizeof b, &len) == CORE_E_ARG && strlen(b) == 0);   /* {gb} missing */
    memset(b, 'X', sizeof b);
    core_slot dup[2] = {{CORE_SLOT_drive, 0, "a", 1}, {CORE_SLOT_drive, 0, "b", 1}};
    CHECK(core_text(CORE_OS_MAC, T_CACHE_PAUSED, dup, 2, b, sizeof b, &len) == CORE_E_ARG && strlen(b) == 0);
    core_slot bad = {CORE_SLOT_COUNT, 0, "a", 1};
    CHECK(core_text(CORE_OS_MAC, T_OK, &bad, 1, b, sizeof b, &len) == CORE_E_ARG && strlen(b) == 0);
    CHECK(core_text(CORE_OS_MAC, (core_text_id)CORE_TEXT_COUNT, NULL, 0, b, sizeof b, &len) == CORE_E_ARG);
    CHECK(core_text(CORE_OS_MAC, T_OK, NULL, 0, NULL, 0, &len) == CORE_E_SPACE && len == 2);
    CHECK(core_text(CORE_OS_MAC, T_PICK_FOLDER_CHOOSE, NULL, 0, b, sizeof b, &len) == CORE_OK && !strcmp(b, "Choose"));
#ifdef CORE_WIN_POLICY
    CHECK(core_text(CORE_OS_WIN, T_APP_NAME, NULL, 0, b, sizeof b, &len) == CORE_OK && !strcmp(b, "RB Stems Plus"));
    CHECK(core_text(CORE_OS_WIN, T_CHECKING, NULL, 0, b, sizeof b, &len) == CORE_OK && !strcmp(b, "Checking…"));
    CHECK(core_text(CORE_OS_WIN, T_PW_TITLE, NULL, 0, b, sizeof b, &len) == CORE_E_NOTYET);   /* Mac-only (win_texts.c) */
#else
    CHECK(core_text(CORE_OS_WIN, T_APP_NAME, NULL, 0, b, sizeof b, &len) == CORE_E_ARG);   /* no Windows column here */
#endif
    core_mask_ctx mc = {sizeof mc, 0, "/Users/x", NULL, 0, 0, NULL};
    CHECK(core_mask(&mc, "x", 1, b, sizeof b, &len) == CORE_OK && !strcmp(b, "x") && len == 1);
    CHECK(core_mask(NULL, "x", 1, b, sizeof b, &len) == CORE_E_ARG);
    CHECK(sizeof(core_mask_ctx) == 40 && sizeof(core_slot) == 24 && sizeof(core_menu_item) == 40 && sizeof(core_host) == 136);
    LEAKCHECK();
}

/* C14: numbers parse in bounded time, with sane values */
static double ms_since(const struct timespec *a) {
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (double)(b.tv_sec - a->tv_sec) * 1e3 + (double)(b.tv_nsec - a->tv_nsec) / 1e6;
}
static void t_json_numbers(void) {
    arena a = {0};
    jval v;
    char one[512];
    one[0] = '1';
    memset(one + 1, '0', 400);
    memcpy(one + 401, "e-400", 6);
    CHECK(json_parse(&a, one, strlen(one), &v) == CORE_OK && v.type == J_NUM && !v.is_int && v.num == 1.0);
    CHECK(json_parse(&a, "0e999999", 8, &v) == CORE_OK && v.num == 0);
    CHECK(json_parse(&a, "-0.000e-99999", 13, &v) == CORE_OK && v.num == 0);
    CHECK(json_parse(&a, "1e400", 5, &v) == CORE_OK && v.num > 1e308);
    CHECK(json_parse(&a, "1e-400", 6, &v) == CORE_OK && v.num == 0);
    CHECK(json_parse(&a, "123.456e2", 9, &v) == CORE_OK && v.num > 12345.59 && v.num < 12345.61);
    CHECK(json_parse(&a, "-0.25", 5, &v) == CORE_OK && v.num == -0.25);
    CHECK(json_parse(&a, "9223372036854775807", 19, &v) == CORE_OK && v.is_int && v.i == INT64_MAX);
    CHECK(json_parse(&a, "9223372036854775808", 19, &v) == CORE_OK && !v.is_int && v.num > 9.2e18);
    arena_reset(&a);
    /* the worst cases at the 64 KiB limit: zero mantissas and long mantissas, each at most a few ms */
    static char buf[65536];
    size_t n = 0;
    buf[n++] = '[';
    while (n + 20 < sizeof buf) { memcpy(buf + n, "0e99999,", 8); n += 8; }
    memcpy(buf + n, "0]", 2);
    n += 2;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = json_parse(&a, buf, n, &v);
    double ms1 = ms_since(&t0);
    CHECK(r == CORE_E_PARSE || r == CORE_OK);   /* past 256 members: refused, quickly */
    arena_reset(&a);
    n = 0;
    buf[n++] = '[';
    for (int k = 0; k < 150 && n + 420 < sizeof buf; k++) {
        memset(buf + n, '9', 400);
        n += 400;
        memcpy(buf + n, "e-99999,", 8);
        n += 8;
    }
    memcpy(buf + n, "0]", 2);
    n += 2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r = json_parse(&a, buf, n, &v);
    double ms2 = ms_since(&t0);
    CHECK(r == CORE_OK && v.n == 151 && v.items[0].num == 0);
    CHECK(ms1 < 50 && ms2 < 50);   /* a backstop only: the step bound below is the check (R7) */
    /* every number scales in a bounded number of steps, whatever its exponent */
    static const char *const extremes[] = {"1e9999999", "1e-9999999", "0e9999999"};
    for (int k = 0; k < 3; k++) {
        arena_reset(&a);
        CHECK(json_parse(&a, extremes[k], strlen(extremes[k]), &v) == CORE_OK && v.scale_steps <= 40);
    }
    /* 16 arrays deep, 255 such numbers at every level */
    n = 0;
    for (int depth = 0; depth < 16; depth++) {
        buf[n++] = '[';
        for (int k = 0; k < 255; k++) {
            size_t l = strlen(extremes[k % 3]);
            memcpy(buf + n, extremes[k % 3], l);
            n += l;
            if (k < 254 || depth < 15) buf[n++] = ',';
        }
    }
    for (int depth = 0; depth < 16; depth++) buf[n++] = ']';
    arena_reset(&a);
    CHECK(n < sizeof buf && json_parse(&a, buf, n, &v) == CORE_OK);
    int most = 0, numbers = 0;
    for (const jval *level = &v; level && level->type == J_ARR;) {
        const jval *next = NULL;
        for (size_t k = 0; k < level->n; k++) {
            const jval *it = &level->items[k];
            if (it->type == J_ARR) next = it;
            else {
                numbers++;
                if (it->scale_steps > most) most = it->scale_steps;
            }
        }
        level = next;
    }
    CHECK(numbers == 16 * 255 && most <= 40);
    arena_free(&a);
    LEAKCHECK();
}

static void t_menus(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    int n = core_menu_count(CORE_OS_MAC);
    CHECK(n == CORE_MENU_COUNT);
    core_menu_item it;
    /* today's app-menu order: About, a separator, Update, Settings, Uninstall (C36) */
    CHECK(core_menu_at(CORE_OS_MAC, 1, &it) == CORE_OK && it.id == M_APP_ABOUT && it.kind == CORE_MENU_NATIVE);
    CHECK(core_menu_at(CORE_OS_MAC, 3, &it) == CORE_OK && it.id == M_APP_UPDATE);
    CHECK(core_menu_at(CORE_OS_MAC, 4, &it) == CORE_OK && it.id == M_APP_SETTINGS && it.menu == M_APP && !strcmp(it.key, ",") && !strcmp(it.event, "settings"));
    CHECK(core_menu_at(CORE_OS_MAC, n, &it) == CORE_E_ARG);
    CHECK(core_menu_at(CORE_OS_MAC, 9, &it) == CORE_OK && it.id == M_APP_QUIT && it.kind == CORE_MENU_NATIVE && !strcmp(it.key, "q"));
    /* nested: Manage › Demucs v4 › Install (C21) */
    CHECK(core_menu_at(CORE_OS_MAC, M_MANAGE_PLUS_INSTALL, &it) == CORE_OK && it.menu == M_MANAGE_PLUS && core_menu_rows[M_MANAGE_PLUS].parent == M_MANAGE);
    CHECK(core_menu_at(CORE_OS_MAC, M_HELP_HOME, &it) == CORE_OK && it.mods == CORE_MOD_CMD_OR_CTRL && !strcmp(it.key, "?"));
#ifdef CORE_WIN_POLICY
    /* the Mac's own rows: About, Hide, Quit, two separators, Edit, Window */
    CHECK(core_menu_count(CORE_OS_WIN) == n - 7);
    CHECK(core_menu_at(CORE_OS_WIN, M_HELP_HOME - 7, &it) == CORE_OK && it.id == M_HELP_HOME && !strcmp(it.key, "F1") && it.mods == 0);
#else
    CHECK(core_menu_count(CORE_OS_WIN) == 0);
#endif
    const char *title;
    size_t tl;
    uint32_t st = core_menu_state(c, M_PREP_PAUSE, &title, &tl);
    CHECK(!(st & CORE_MENU_ENABLED) && !strcmp(title, "Pause") && tl == 5);
    c->preparing = c->paused = 1;
    st = core_menu_state(c, M_PREP_PAUSE, &title, &tl);
    CHECK((st & CORE_MENU_ENABLED) && !strcmp(title, "Resume"));
    c->preparing = c->paused = 0;
    CHECK(core_close_asked(c, CORE_CLOSE_WINDOW) == CORE_CLOSE_NOW);
    CHECK(core_menu_state(c, M_APP_SETTINGS, NULL, NULL) & CORE_MENU_ENABLED);
    CHECK(core_menu_state(c, M_APP_UPDATE, NULL, NULL) & CORE_MENU_HIDDEN);
    c->action_running = 1;
    CHECK(!(core_menu_state(c, M_APP_SETTINGS, NULL, NULL) & CORE_MENU_ENABLED));
    CHECK(core_menu_state(c, M_HELP_HOME, NULL, NULL) & CORE_MENU_ENABLED);
    CHECK(core_menu_state(c, M_APP_QUIT, NULL, NULL) & CORE_MENU_ENABLED);
    c->action_running = 0;
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

/* D5: enabled states and placement, and Settings' rules (C4, C31) */
static void t_dialogs(void) {
    core_dialog d;
    memset(&d, 0, sizeof d);
    d.nbuttons = 3;
    d.buttons[0] = "Save";
    d.buttons[1] = "Clear Saved Stems";
    d.buttons[2] = "Cancel";
    d.nrows = 1;
    d.row_label[0] = "Stems Cache location: GLS";
    d.row_nbuttons[0] = 2;
    d.row_buttons[0][0] = "Change…";
    d.row_buttons[0][1] = "Use This Mac";
    /* a zeroed dialog is fully enabled, nothing on the left */
    CHECK(core_dlg_int(&d, CORE_DLG_BUTTON_ENABLED, 0, 0) && core_dlg_int(&d, CORE_DLG_BUTTON_ENABLED, 2, 0));
    CHECK(core_dlg_int(&d, CORE_DLG_ROW_BUTTON_ENABLED, 0, 1) && !core_dlg_int(&d, CORE_DLG_BUTTON_LEFT, 0, 0));
    CHECK(!core_dlg_int(&d, CORE_DLG_BUTTON_ENABLED, 3, 0) && !core_dlg_int(&d, CORE_DLG_ROW_BUTTON_ENABLED, 0, 2) && !core_dlg_int(&d, CORE_DLG_ROW_BUTTON_ENABLED, 1, 0));
    CHECK(!strcmp(core_dlg_text(&d, CORE_DLG_ROW_BUTTON, 0, 1, NULL), "Use This Mac") && core_dlg_int(&d, CORE_DLG_N_ROW_BUTTONS, 0, 0) == 2);
#define EN(i) core_dlg_int(&d, CORE_DLG_BUTTON_ENABLED, i, 0)
#define REN(j) core_dlg_int(&d, CORE_DLG_ROW_BUTTON_ENABLED, 0, j)
    dlg_settings_enable(&d, 0, 1, 1, 0);   /* config.ini unreadable: nothing can be changed */
    CHECK(!EN(0) && !EN(1) && EN(2) && !REN(0) && !REN(1));
    dlg_settings_enable(&d, 1, 0, 1, 0);   /* nothing to clear */
    CHECK(EN(0) && !EN(1) && REN(0) && REN(1));
    dlg_settings_enable(&d, 1, 1, 0, 0);   /* a location that can't be chosen (an old bridge) */
    CHECK(EN(0) && EN(1) && !REN(0) && REN(1));
    dlg_settings_enable(&d, 1, 1, 1, 1);   /* already This Mac */
    CHECK(EN(0) && EN(1) && REN(0) && !REN(1));
    d.inline_ = 1;
    d.left_buttons = 1u << 1;   /* "Don't Ask Again…" */
    CHECK(core_dlg_int(&d, CORE_DLG_BUTTON_LEFT, 1, 0) && !core_dlg_int(&d, CORE_DLG_BUTTON_LEFT, 0, 0) && core_dlg_int(&d, CORE_DLG_INLINE, 0, 0));
    LEAKCHECK();
}

/* a flow on its own thread: the dialog is presented on main while the flow waits, main keeps
 * committing under it, then revalidate */
static void t_flow(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    f.answer = 0;
    CHECK(core_test_flow(c, CORE_TEST_FLOW_DIALOG) == CORE_OK);
    CHECK(core_test_flow(c, CORE_TEST_FLOW_DIALOG) == CORE_E_STATE);   /* one flow at a time */
    PUMP_UNTIL(&f, f.dialogs == 1);
    CHECK(f.dialogs == 1 && !strcmp(f.dlg_title, "RB Stems Plus") && f.dlg_buttons == 2 && f.dlg_default == 0 && f.dlg_cancel == 1);
    PUMP_UNTIL(&f, logged(&f, "test flow: button 0, revalidate 1") && !c->flow_running);
    CHECK(logged(&f, "test flow: button 0, revalidate 1"));
    CHECK(!c->flow_running && !c->action_running && atomic_load(&c->life->flows_out) == 0);
    f.answer = 1;
    CHECK(core_test_flow(c, CORE_TEST_FLOW_DIALOG) == CORE_OK);
    int r0 = f.renders;
    core_test_set_str(c, F_STATUS_LINE, "under the dialog", CORE_NTS);
    PUMP_UNTIL(&f, logged(&f, "test flow: button 1, revalidate 1") && !c->flow_running);
    CHECK(logged(&f, "test flow: button 1, revalidate 1"));
    CHECK(f.renders > r0 && !strcmp(core_str(f.held_snap, F_STATUS_LINE, NULL), "under the dialog"));
    /* revalidate after the domain's state moved: the flow is told */
    f.answer = 0;
    CHECK(core_test_flow(c, CORE_TEST_FLOW_DIALOG) == CORE_OK);
    PUMP_UNTIL(&f, f.dialogs == 3);
    c->gen[DOM_PREPARE]++;
    PUMP_UNTIL(&f, logged(&f, "test flow: button 0, revalidate 0") && !c->flow_running);
    CHECK(logged(&f, "test flow: button 0, revalidate 0"));
    join_workers(&f);
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

/* C10: the quit flow ends beside a normal flow blocked in a step; the normal flow's busy state
 * stays; quit's decision answers QUIT and the WINDOW asked meanwhile, each re-ask answers NOW */
static void t_quit_beside_flow(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    f.step_block = 1;
    CHECK(core_test_flow(c, CORE_TEST_FLOW_STEP) == CORE_OK);
    PUMP_UNTIL(&f, steps_of(&f) == 1);
    CHECK(c->flow_running && c->action_running);
    CHECK(core_test_set_preparing(c, 1) == CORE_OK);
    f.modal = 1;   /* a dialog is up: the quit flow waits for it to end before asking */
    CHECK(core_close_asked(c, CORE_CLOSE_QUIT) == CORE_CLOSE_LATER);
    CHECK(core_close_asked(c, CORE_CLOSE_QUIT) == CORE_CLOSE_LATER);     /* coalesced */
    CHECK(core_close_asked(c, CORE_CLOSE_WINDOW) == CORE_CLOSE_LATER);   /* answered with QUIT's decision */
    for (int i = 0; i < 30; i++) { pump(&f); plat_sleep_ms(2); }
    CHECK(f.dialogs == 0 && f.nclose == 0);
    f.modal = 0;
    f.answer = 0;    /* "Stop and Quit" */
    f.reask = 1;
    PUMP_UNTIL(&f, f.nclose == 2);
    CHECK(f.dialogs == 1 && !strcmp(f.dlg_title, "“” is being prepared"));
    CHECK(f.nclose == 2 && f.close_what[0] == CORE_CLOSE_WINDOW && f.close_what[1] == CORE_CLOSE_QUIT);
    CHECK(f.close_proceed[0] == 1 && f.close_proceed[1] == 1);
    CHECK(f.close_reask[0] == CORE_CLOSE_NOW && f.close_reask[1] == CORE_CLOSE_NOW);   /* the latch, from inside close_reply */
    PUMP_UNTIL(&f, atomic_load(&c->life->flows_out) == 1);
    while (pump(&f)) {}   /* drain main: the quit flow's done job, posted before its count dropped (R13) */
    CHECK(c->flow_running && c->action_running && !c->quit_flow_running);   /* the normal flow is still busy */
    CHECK(core_test_flow(c, CORE_TEST_FLOW_DIALOG) == CORE_E_STATE);
    pthread_mutex_lock(&f.mu);
    f.step_released = 1;
    pthread_cond_broadcast(&f.cv);
    pthread_mutex_unlock(&f.mu);
    PUMP_UNTIL(&f, !c->flow_running);
    CHECK(!c->flow_running && !c->action_running && logged(&f, "test flow: step 0"));
    /* Cancel: stay */
    f.reask = 0;
    core_test_set_preparing(c, 1);
    f.answer = 1;
    CHECK(core_close_asked(c, CORE_CLOSE_QUIT) == CORE_CLOSE_LATER);
    PUMP_UNTIL(&f, f.nclose == 3);
    CHECK(f.close_what[2] == CORE_CLOSE_QUIT && f.close_proceed[2] == 0 && c->preparing);
    CHECK(core_close_asked(c, CORE_CLOSE_QUIT) == CORE_CLOSE_LATER);   /* asks again */
    PUMP_UNTIL(&f, f.nclose == 4);
    join_workers(&f);
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

/* SO12: the quit flow's "“x” is being prepared" names the whole playlist, a 600-byte multibyte
 * name too (no fixed buffer cuts it, mid-character or not) */
static void t_quit_long_name(void) {
    /* 600 bytes (past the old 512-byte name) and 900 (past the old 768-byte title) */
    for (int chars = 200; chars <= 300; chars += 100) {
        fake f;
        fake_init(&f);
        core *c = new_core(&f);
        char name[1000] = "", want[1100];
        for (int i = 0; i < chars; i++) strcat(name, "\xe6\x97\xa5");   /* 日: 3 bytes each */
        CHECK(strlen(name) == (size_t)chars * 3);
        snprintf(want, sizeof want, "\xe2\x80\x9c%s\xe2\x80\x9d is being prepared", name);
        CHECK(core_test_set_preparing(c, 1) == CORE_OK);
        c->p.run_pl.name = strdup(name);   /* the run's playlist (prep_free clears it) */
        c->p.has_run_pl = 1;
        f.answer = 1;   /* Cancel: stay */
        CHECK(core_close_asked(c, CORE_CLOSE_QUIT) == CORE_CLOSE_LATER);
        PUMP_UNTIL(&f, f.nclose == 1);
        CHECK(f.dialogs == 1 && !strcmp(f.dlg_title, want));
        CHECK(f.close_what[0] == CORE_CLOSE_QUIT && f.close_proceed[0] == 0 && c->preparing);
        join_workers(&f);
        core_free(c);
        fake_end(&f);
        LEAKCHECK();
    }
}

/* D3: core_free while a flow is blocked in its dialog; the flow carries on, revalidate answers 0 */
static void t_free_during_flow(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    core_life *life = c->life;
    CHECK(core_test_flow(c, CORE_TEST_FLOW_DIALOG) == CORE_OK);
    pthread_mutex_lock(&f.mu);   /* wait until the flow is blocked in its dialog */
    while (!f.dialogs_asked) {
        pthread_mutex_unlock(&f.mu);
        plat_sleep_ms(1);
        pthread_mutex_lock(&f.mu);
    }
    pthread_mutex_unlock(&f.mu);
    core_free(c);
    CHECK(!life_alive(life));
    f.answer = 0;
    PUMP_UNTIL(&f, logged(&f, "test flow: button 0, revalidate 0"));
    CHECK(logged(&f, "test flow: button 0, revalidate 0"));
    join_workers(&f);
    pump(&f);   /* the flow's done job: dropped */
    fake_end(&f);
    LEAKCHECK();   /* the life went with the flow */
}

/* C3: a worker job queued before core_free and run after it is dropped, not run */
static void t_worker_after_free(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    f.hold_work = 1;
    CHECK(core_test_flow(c, CORE_TEST_FLOW_STEP) == CORE_OK && f.nheld == 1);
    core_free(c);
    core_run(f.held[0]);   /* the host's worker gets to it late */
    f.nheld = 0;
    CHECK(f.steps == 0);
    fake_end(&f);
    LEAKCHECK();
}

/* C9: allocation failure and a closed host queue leave nothing stuck */
static void t_alloc_failure(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    core_test_fail_allocs(c, 1);
    CHECK(core_test_set_str(c, F_NOTICE, "a", CORE_NTS) == CORE_OK);   /* its commit job can't be made */
    CHECK(c->commit_posted == 0);
    CHECK(core_test_set_str(c, F_NOTICE, "b", CORE_NTS) == CORE_OK);
    pump(&f);
    CHECK(f.renders == 1 && !strcmp(core_str(f.held_snap, F_NOTICE, NULL), "b"));
    core_test_fail_allocs(c, 1);
    CHECK(core_test_flow(c, CORE_TEST_FLOW_STEP) == CORE_E_NOMEM);
    CHECK(!c->flow_running && !c->action_running && atomic_load(&c->life->flows_out) == 0);
    CHECK(core_test_flow(c, CORE_TEST_FLOW_STEP) == CORE_OK);
    PUMP_UNTIL(&f, !c->flow_running);
    CHECK(logged(&f, "test flow: step 0"));
    join_workers(&f);
    /* the host's loop has stopped: jobs are dropped synchronously inside post_main/post_work */
    f.closed = 1;
    CHECK(core_test_set_str(c, F_NOTICE, "c", CORE_NTS) == CORE_OK);   /* its commit is dropped: shutting down */
    CHECK(core_test_flow(c, CORE_TEST_FLOW_STEP) == CORE_OK);   /* posted, then dropped by the closed host */
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

typedef struct { core *c; uint64_t run; int n; } liner;
static void *reader_thread(void *p) {
    liner *l = p;
    for (int i = 0; i < l->n; i++) core_helper_line(l->c, l->run, "{\"track\":1}", CORE_NTS);
    core_flow_progress(l->c, CORE_PROGRESS_DOWNLOAD, 1, 2);   /* as from a flow: off main */
    core_helper_ended(l->c, l->run, 0, "", 0);
    return NULL;
}

/* D3: helper runs per purpose; overlapping runs; a refused start; a run outliving core_free */
static void t_helper_runs(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    uint64_t a = core_test_helper_start(c, 0), b = core_test_helper_start(c, 1);
    CHECK(a && b && a != b && f.spawns == 2);
    liner la = {c, a, 300}, lb = {c, b, 200};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, reader_thread, &la);
    pthread_create(&tb, NULL, reader_thread, &lb);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    PUMP_UNTIL(&f, core_test_helper_lines(c, 0) == 300 && core_test_helper_lines(c, 1) == 200);
    CHECK(core_test_helper_lines(c, 0) == 300 && core_test_helper_lines(c, 1) == 200);   /* overlapping runs both count */
    pump(&f);
    /* a newer run of the same purpose: the older one's lines are dropped */
    uint64_t a1 = core_test_helper_start(c, 0), a2 = core_test_helper_start(c, 0);
    core_helper_line(c, a1, "x", 1);
    core_helper_line(c, a2, "y", 1);
    core_helper_line(c, a2, "\xff", 1);   /* not UTF-8: never posted */
    pump(&f);
    CHECK(core_test_helper_lines(c, 0) == 301);
    core_helper_ended(c, a1, 0, "", 0);
    core_helper_ended(c, a2, 0, "", 0);
    pump(&f);
    f.spawn_refuse = 1;
    CHECK(core_test_helper_start(c, 2) == 0);   /* not started: no core_helper_ended follows */
    f.spawn_refuse = 0;
    uint64_t late = core_test_helper_start(c, 3);
    core_free(c);   /* the run holds the life: its late calls are legal and do nothing */
    core_helper_line(c, late, "z", 1);
    core_flow_progress(c, CORE_PROGRESS_DOWNLOAD, 1, 2);
    core_helper_ended(c, late, 0, "", 0);
    pump(&f);
    fake_end(&f);
    LEAKCHECK();
}

/* the G1 spike's stub parse (core_test_helper_stub): a reader thread sends the fake helper's lines */
typedef struct { core *c; uint64_t run; int n; int estimate; } stub_liner;
static void *stub_reader(void *p) {
    stub_liner *l = p;
    char line[256];
    for (int i = 0; i < l->n; i++) {
        if (l->estimate)
            snprintf(line, sizeof line, "{\"file\":\"/Users/dj/Music/Harness/big-%d.mp3\",\"result\":\"%s\",\"seconds\":240}", i + 1, i % 2 ? "ready" : "prepare");
        else
            snprintf(line, sizeof line, "{\"file\":\"/Users/dj/Music/Harness/big-%d.mp3\"}", i + 1);
        core_helper_line(l->c, l->run, line, strlen(line));
    }
    if (l->estimate) core_helper_line(l->c, l->run, "{\"estimate\":3000,\"ready\":1500}", CORE_NTS);
    core_helper_ended(l->c, l->run, 0, "", 0);
    return NULL;
}

static int cell_is(core_snap *s, core_column col, int32_t row, const char *want) {
    size_t n;
    const char *v = core_cell_str(s, col, row, &n);
    return v && n == strlen(want) && !memcmp(v, want, n);
}

static void t_helper_stub(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    CHECK(core_test_helper_stub(NULL, 1) == CORE_E_ARG);
    CHECK(core_test_helper_stub(c, 1) == CORE_OK);
    CHECK(core_test_table_reset(c, T_PREP_TRACKS, 0) == CORE_OK);
    pump(&f);
    /* the list: one row appended per line, rendered once per turn, the previous snapshot held */
    uint64_t tracks = core_test_helper_start(c, 1);
    stub_liner lt = {c, tracks, 3000, 0};
    pthread_t t;
    pthread_create(&t, NULL, stub_reader, &lt);
    pthread_join(t, NULL);
    PUMP_UNTIL(&f, f.held_snap && core_rows(f.held_snap, T_PREP_TRACKS) == 3000 && !c->runs[tracks & 31u].id);
    CHECK(core_rows(f.held_snap, T_PREP_TRACKS) == 3000 && core_test_helper_lines(c, 1) == 3000);
    CHECK(cell_is(f.held_snap, C_TRK_NAME, 0, "big-1") && cell_is(f.held_snap, C_TRK_NAME, 2999, "big-3000"));
    CHECK(cell_is(f.held_snap, C_TRK_RESULT, 1234, "Checking\xe2\x80\xa6"));
    /* the estimate: each line sets the next row's result; the summary line is only counted */
    uint64_t est = core_test_helper_start(c, 2);
    stub_liner le = {c, est, 3000, 1};
    int before = f.renders;
    pthread_create(&t, NULL, stub_reader, &le);
    pthread_join(t, NULL);
    PUMP_UNTIL(&f, core_test_helper_lines(c, 2) == 3001 && !c->runs[est & 31u].id);
    pump(&f);
    CHECK(core_test_helper_lines(c, 2) == 3001 && f.renders > before);
    CHECK(cell_is(f.held_snap, C_TRK_RESULT, 0, "To prepare") && cell_is(f.held_snap, C_TRK_RESULT, 2999, "Ready"));
    CHECK(cell_is(f.held_snap, C_TRK_NAME, 2999, "big-3000") && core_rows(f.held_snap, T_PREP_TRACKS) == 3000);
    /* the estimate's rows count again from 0 for the next run; lines past the table's end, odd
     * lines and a stale run's lines change nothing */
    uint64_t old = core_test_helper_start(c, 2), cur = core_test_helper_start(c, 2);
    const char *ready0 = "{\"file\":\"/a/b.mp3\",\"result\":\"ready\"}";
    core_helper_line(c, old, ready0, CORE_NTS);                          /* stale: dropped */
    core_helper_line(c, cur, "{\"file\":\"/a/b.mp3\",\"result\":\"skipped\"}", CORE_NTS);
    core_helper_line(c, cur, "{\"file\":\"/a/unterminated", CORE_NTS);   /* counted only */
    core_helper_line(c, cur, "{\"file\":\"/a/b.mp3\",\"result\":\"x", CORE_NTS);
    core_helper_line(c, cur, "not json", CORE_NTS);
    pump(&f);
    pump(&f);
    CHECK(cell_is(f.held_snap, C_TRK_RESULT, 0, "skipped") && cell_is(f.held_snap, C_TRK_RESULT, 1, "Ready"));
    CHECK(f.last_reload[T_PREP_TRACKS] == 0 && f.last_nchanged[T_PREP_TRACKS] == 1 && f.last_changed0[T_PREP_TRACKS] == 0);
    core_helper_ended(c, old, 0, "", 0);
    core_helper_ended(c, cur, 0, "", 0);
    pump(&f);
    /* review Q32: an outline select ends the estimate shown: its helper is told to stop and its
     * later lines no longer land in the rows; a select with no estimate running signals nothing */
    uint64_t shown = core_test_helper_start(c, 2);
    core_helper_line(c, shown, "{\"file\":\"/a/b.mp3\",\"result\":\"kept\"}", CORE_NTS);
    pump(&f);
    pump(&f);
    CHECK(cell_is(f.held_snap, C_TRK_RESULT, 0, "kept"));
    char sel[128];
    snprintf(sel, sizeof sel, "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":-1,\"gen\":%llu}",
             (unsigned long long)c->m.tab[T_PREP_OUTLINE]->list_gen);
    int sig0 = f.signals;
    CHECK(core_event(c, sel, CORE_NTS) == CORE_OK);
    CHECK(f.signals == sig0 + 1 && f.signal_run == shown && f.signal_sig == CORE_HELPER_STOP);
    core_helper_line(c, shown, "{\"file\":\"/a/c.mp3\",\"result\":\"late\"}", CORE_NTS);
    pump(&f);
    pump(&f);
    CHECK(cell_is(f.held_snap, C_TRK_RESULT, 1, "Ready"));   /* the stopped run's line was dropped */
    CHECK(core_event(c, sel, CORE_NTS) == CORE_OK && f.signals == sig0 + 1);
    core_helper_ended(c, shown, 143, "", 0);
    pump(&f);
    /* off again: lines are only counted, and a select stops nothing */
    CHECK(core_test_helper_stub(c, 0) == CORE_OK);
    uint64_t plain = core_test_helper_start(c, 2);
    core_helper_line(c, plain, ready0, CORE_NTS);
    CHECK(core_event(c, sel, CORE_NTS) == CORE_OK && f.signals == sig0 + 1);
    core_helper_ended(c, plain, 0, "", 0);
    pump(&f);
    pump(&f);
    CHECK(cell_is(f.held_snap, C_TRK_RESULT, 0, "kept"));
    core_free(c);
    pump(&f);
    fake_end(&f);
    LEAKCHECK();
}

/* D2: callbacks that call back in (release builds: refused or deferred; CORE_DEBUG: abort) */
static int reentry_event_rc, reentry_lines_inside;
static __attribute__((unused)) void render_sends_event(fake *f) { reentry_event_rc = core_event(f->c, "{\"ev\":\"activated\"}", CORE_NTS); }
static __attribute__((unused)) void render_frees(fake *f) { core_free(f->c); }
static __attribute__((unused)) void render_pumps(fake *f) {
    pump(f);   /* a nested message pump inside render */
    reentry_lines_inside = (int)f->c->run_lines[0];
}
static __attribute__((unused)) void facts_sends_event(fake *f) { reentry_event_rc = core_event(f->c, "{\"ev\":\"tab\",\"tab\":1}", CORE_NTS); }

static __attribute__((unused)) void child_render_frees(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    f.on_render = render_frees;
    core_test_set_str(c, F_NOTICE, "x", CORE_NTS);
    pump(&f);
}
static __attribute__((unused)) void child_render_pumps(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    uint64_t run = core_test_helper_start(c, 0);
    core_test_set_str(c, F_NOTICE, "x", CORE_NTS);
    core_helper_line(c, run, "l", 1);
    f.on_render = render_pumps;
    pump(&f);
}
static __attribute__((unused)) void child_off_main_event(void);

static void t_reentry(void) {
#ifdef CORE_DEBUG
#  ifndef UNDER_TSAN
    CHECK(aborts(child_render_frees));
    CHECK(aborts(child_render_pumps));
    CHECK(aborts(child_off_main_event));
#  endif
#else
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    /* a nested core_event: refused */
    f.on_render = render_sends_event;
    core_test_set_str(c, F_NOTICE, "1", CORE_NTS);
    pump(&f);
    CHECK(reentry_event_rc == CORE_E_STATE && f.facts_calls == 0);
    CHECK(logged(&f, "core: a call from inside a host callback was refused"));
    f.on_render = NULL;
    f.on_facts = facts_sends_event;
    CHECK(core_event(c, "{\"ev\":\"activated\"}", CORE_NTS) == CORE_OK && reentry_event_rc == CORE_E_STATE);
    f.on_facts = NULL;
    /* a nested pump: the jobs it reaches run after the outer call, in order */
    uint64_t run = core_test_helper_start(c, 0);
    core_test_set_str(c, F_NOTICE, "2", CORE_NTS);   /* the commit job, first in the queue */
    core_helper_line(c, run, "l1", 2);
    core_helper_line(c, run, "l2", 2);
    core_helper_line(c, run, "l3", 2);
    f.on_render = render_pumps;
    pump(&f);
    CHECK(reentry_lines_inside == 0 && core_test_helper_lines(c, 0) == 3);
    f.on_render = NULL;
    core_helper_ended(c, run, 0, "", 0);
    pump(&f);
    /* core_free inside render: deferred to the outermost return, no use after free */
    f.on_render = render_frees;
    core_test_set_str(c, F_NOTICE, "3", CORE_NTS);
    core_life *life = c->life;
    life_retain(life);
    pump(&f);
    CHECK(!life_alive(life));
    life_release(life);
    pump(&f);
    fake_end(&f);
#endif
    LEAKCHECK();
}

/* CORE_DEBUG: a main-only call from another thread aborts; release: CORE_E_THREAD */
typedef struct { int rc[2]; core *c; } offarg;
static __attribute__((unused)) void *off_main(void *p) {
    offarg *a = p;
    a->rc[0] = core_event(a->c, "{\"ev\":\"activated\"}", CORE_NTS);
    a->rc[1] = core_close_asked(a->c, CORE_CLOSE_QUIT);
    return NULL;
}
static __attribute__((unused)) void child_off_main_event(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    offarg a = {{0, 0}, c};
    pthread_t t;
    pthread_create(&t, NULL, off_main, &a);
    pthread_join(t, NULL);
}
static void t_thread_check(void) {
#ifndef CORE_DEBUG
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    offarg a = {{0, 0}, c}, b = {{0, 0}, c};
    pthread_t t, u;   /* two threads at once: the logged-once flag is atomic (R8; TSan in tsanrel) */
    pthread_create(&t, NULL, off_main, &a);
    pthread_create(&u, NULL, off_main, &b);
    pthread_join(t, NULL);
    pthread_join(u, NULL);
    CHECK(a.rc[0] == CORE_E_THREAD && a.rc[1] == CORE_CLOSE_REFUSE && f.facts_calls == 0);
    CHECK(b.rc[0] == CORE_E_THREAD && b.rc[1] == CORE_CLOSE_REFUSE);
    core_free(c);
    fake_end(&f);
#endif
    LEAKCHECK();
}

static void t_facts_sheet(void) {
    arena a = {0};
    core_facts fs;
    facts_init(&fs, &a);
    char path[] = "/Volumes/GLS";
    core_fact_str(&fs, FACT_CHOSEN_LOCATION, path, CORE_NTS);
    path[1] = 'X';   /* copied */
    core_fact_i64(&fs, FACT_FREE_BYTES, 1234);
    core_fact_flag(&fs, FACT_USER_IS_ADMIN, 7);
    CHECK(fs.set[FACT_CHOSEN_LOCATION] && !strcmp(fs.s[FACT_CHOSEN_LOCATION], "/Volumes/GLS") && fs.len[FACT_CHOSEN_LOCATION] == 12);
    CHECK(fs.i[FACT_FREE_BYTES] == 1234 && fs.i[FACT_USER_IS_ADMIN] == 1 && !fs.set[FACT_SHA256]);
    arena_free(&a);
    core_reply r;
    arena b = {0};
    reply_init(&r, &b);
    CHECK(r.button == -1);
    core_reply_field(&r, 0, "/Volumes/X", CORE_NTS);
    core_reply_row_button(&r, 1, 2);
    CHECK(!strcmp(r.fields[0], "/Volumes/X") && r.row == 1 && r.row_button == 2 && r.button == -1);
    arena_free(&b);
    LEAKCHECK();
}

/* E2: the jobs that end a state are reserved when it begins: with every later allocation failing,
 * the flow's end, the quit decision and the helper run's end still arrive (R1, R2, R3) */
static void t_reserved_terminals(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    /* the flow's end */
    f.step_block = 1;
    CHECK(core_test_flow(c, CORE_TEST_FLOW_STEP) == CORE_OK);
    PUMP_UNTIL(&f, steps_of(&f) == 1);
    core_test_fail_allocs(c, 1000000);
    pthread_mutex_lock(&f.mu);
    f.step_released = 1;
    pthread_cond_broadcast(&f.cv);
    pthread_mutex_unlock(&f.mu);
    PUMP_UNTIL(&f, !c->flow_running);
    CHECK(!c->flow_running && !c->action_running);
    join_workers(&f);
    core_test_fail_allocs(c, 0);
    /* the quit decision */
    core_test_set_preparing(c, 1);
    f.answer = 0;
    CHECK(core_close_asked(c, CORE_CLOSE_QUIT) == CORE_CLOSE_LATER);
    core_test_fail_allocs(c, 1000000);
    PUMP_UNTIL(&f, f.nclose == 1);
    /* the stop couldn't be asked: the decision comes, a refusal, never an approval */
    CHECK(f.nclose == 1 && f.close_what[0] == CORE_CLOSE_QUIT && f.close_proceed[0] == 0 && !c->quit_flow_running);
    PUMP_UNTIL(&f, atomic_load(&c->life->flows_out) == 0);
    join_workers(&f);
    core_test_fail_allocs(c, 0);
    CHECK(core_close_asked(c, CORE_CLOSE_QUIT) == CORE_CLOSE_LATER);   /* asked again, memory back */
    PUMP_UNTIL(&f, f.nclose == 2);
    CHECK(f.close_proceed[1] == 1 && core_close_asked(c, CORE_CLOSE_QUIT) == CORE_CLOSE_NOW);
    PUMP_UNTIL(&f, atomic_load(&c->life->flows_out) == 0);
    join_workers(&f);
    /* the helper run's end: the slot retires */
    uint64_t run = core_test_helper_start(c, 1);
    CHECK(run && c->runs[run & 31].id == run && c->runs[run & 31].end_job);
    core_test_fail_allocs(c, 1000000);
    core_helper_ended(c, run, 0, "", 0);
    pump(&f);
    CHECK(c->runs[run & 31].id == 0 && c->runs[run & 31].end_job == NULL && c->current_run[1] == 0);
    core_test_fail_allocs(c, 0);
    /* a reservation that fails: nothing starts, nothing is stuck */
    core_test_fail_allocs(c, 1);
    CHECK(core_test_helper_start(c, 2) == 0 && f.spawns == 1);
    core_test_fail_allocs(c, 1);
    core_test_set_preparing(c, 1);
    CHECK(core_close_asked(c, CORE_CLOSE_QUIT) == CORE_CLOSE_REFUSE && !c->quit_flow_running && !c->close_pending[CORE_CLOSE_QUIT]);
    core_test_set_preparing(c, 0);
    core_free(c);
    fake_end(&f);
    LEAKCHECK();
}

/* E3: a helper entry on main runs inside an entry frame. A host post_main that pumps (a violation):
 * the line is applied before the call returns, nothing stays deferred, and a core_free from there
 * takes effect before the call returns. CORE_DEBUG: the nested run aborts. */
static __attribute__((unused)) void child_pump_in_post_main(void) {
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    uint64_t run = core_test_helper_start(c, 0);
    f.pump_in_post_main = 1;
    core_helper_line(c, run, "l", 1);
}
static void t_pump_in_post_main(void) {
#ifdef CORE_DEBUG
#  ifndef UNDER_TSAN
    CHECK(aborts(child_pump_in_post_main));
#  endif
#else
    fake f;
    fake_init(&f);
    core *c = new_core(&f);
    uint64_t run = core_test_helper_start(c, 0);
    f.pump_in_post_main = 1;
    core_helper_line(c, run, "l", 1);
    CHECK(core_test_helper_lines(c, 0) == 1 && c->deferred == NULL && c->entry == 0);
    core_life *life = life_retain(c->life);
    f.free_in_post_main = 1;
    core_helper_line(c, run, "m", 1);
    CHECK(!life_alive(life));
    f.pump_in_post_main = f.free_in_post_main = 0;
    core_helper_ended(c, run, 0, "", 0);   /* legal after core_free: the run held the life */
    pump(&f);
    life_release(life);
    fake_end(&f);
#endif
    LEAKCHECK();
}

/* R14: the documented zeros are enforced, so a later version can give them a meaning */
static void t_reserved_zero(void) {
    fake f;
    fake_init(&f);
    core_host h = host_for(&f);
    h.reserved = 1;
    CHECK(core_new(&h, CORE_ROLE_APP, CONFIG, CORE_NTS) == NULL);
    CHECK(logged(&f, "core: the host table's reserved field isn't 0"));
    char b[256];
    size_t len;
    core_slot s = {CORE_SLOT_engine, 1, "3", 1};
    CHECK(core_text(CORE_OS_MAC, T_CACHE_ENGINE, &s, 1, b, sizeof b, &len) == CORE_E_ARG && strlen(b) == 0);
    s.reserved = 0;
    CHECK(core_text(CORE_OS_MAC, T_CACHE_ENGINE, &s, 1, b, sizeof b, &len) == CORE_OK);
    core_mask_ctx mc = {sizeof mc, 1, NULL, NULL, 0, 0, NULL};
    CHECK(core_mask(&mc, "x", 1, b, sizeof b, &len) == CORE_E_ARG);
    fake_end(&f);
    LEAKCHECK();
}

/* E4: with "os":"win" the commit job only wakes the drain; the drain's core_commit renders once */
static void t_win_drain(void) {
#ifdef CORE_WIN_POLICY
    fake f;
    fake_init(&f);
    core_host h = host_for(&f);
    core *c = core_new(&h, CORE_ROLE_APP, "{\"os\":\"win\"}", CORE_NTS);
    f.c = c;
    core_test_table_reset(c, T_PREP_TRACKS, 10);
    pump(&f);
    CHECK(f.renders == 0);   /* the commit job rendered nothing */
    core_commit(c);
    CHECK(f.renders == 1);   /* the drain's boundary did */
    const int rows[3] = {2, 5, 7};
    for (int k = 0; k < 3; k++) {   /* three results, each with its commit job run before the next */
        core_test_cell_str(c, C_TRK_RESULT, rows[k], "Ready", CORE_NTS);
        pump(&f);
    }
    CHECK(f.renders == 1);
    core_commit(c);
    CHECK(f.renders == 2 && f.last_nchanged[T_PREP_TRACKS] == 3 && f.last_changed0[T_PREP_TRACKS] == 2);
    CHECK(!strcmp(core_cell_str(f.held_snap, C_TRK_RESULT, 7, NULL), "Ready"));
    core_free(c);
    fake_end(&f);
#endif
    LEAKCHECK();
}

/* the "native" dispatch (native.c): the listed entries reach the DLL's backend with its own state,
 * every other entry the shell's with the shell's ctx; a listed entry the backend lacks, or a
 * backend whose state can't be made, fails cleanly; the state is closed once, after the last
 * reference. A Mac test build has no backend: a listed entry is "not provided". */
#ifdef CORE_WIN_POLICY
typedef struct {
    pthread_mutex_t mu;
    int opened, closed, works, steps, slows, admins, spawns, signals;
    int last_work, last_step, last_slow, last_admin, last_sig;
    uint32_t front_in_admin;   /* front_facts as the core saw it during the admin call */
    uint64_t last_run;
    core *c;          /* what open was given */
    void *closed_st;
    pthread_t closed_on;   /* the thread that dropped the core's last reference */
} nfake;
static nfake g_nf = {.mu = PTHREAD_MUTEX_INITIALIZER};
static pthread_t nf_threads[16];
static int nf_nthreads;

static void *nf_open(core *c) {
    g_nf.opened++;
    g_nf.c = c;
    return &g_nf;
}
static void *nf_open_fails(core *c) {
    (void)c;
    return NULL;
}
static void nf_close(void *st) {
    nfake *n = st;
    n->closed++;
    n->closed_st = st;
    n->closed_on = pthread_self();
}

/* the backend's reader thread: the run's end, after core_free */
typedef struct { core *c; uint64_t run; } nf_end_arg;
static void *nf_end_thread(void *p) {
    nf_end_arg *a = p;
    core_helper_ended(a->c, a->run, 0, "", 0);
    return NULL;
}
static void nf_post_work(void *st, core_work kind, core_job *job) {
    nfake *n = st;
    pthread_mutex_lock(&n->mu);
    n->works++;
    n->last_work = (int)kind;
    pthread_create(&nf_threads[nf_nthreads++], NULL, worker, job);
    pthread_mutex_unlock(&n->mu);
}
static int32_t nf_slow(void *st, core_slow kind, const char *key, size_t key_len, core_facts *out) {
    nfake *n = st;
    (void)key; (void)key_len;
    n->slows++;
    n->last_slow = (int)kind;
    core_fact_str(out, FACT_SHA256, "ab", CORE_NTS);
    return 0;
}
static int32_t nf_spawn(void *st, uint64_t run, const char *const *argv, int32_t argc) {
    nfake *n = st;
    (void)argv; (void)argc;
    n->spawns++;
    n->last_run = run;
    return 0;
}
static void nf_signal(void *st, uint64_t run, core_helper_sig sig) {
    nfake *n = st;
    n->signals++;
    n->last_run = run;
    n->last_sig = (int)sig;
}
static int32_t nf_admin(void *st, int32_t kind, const char *version, const char *bundle_id, const char *bridge_sha, const char *ort_sha, int32_t force) {
    nfake *n = st;
    (void)version; (void)bundle_id; (void)bridge_sha; (void)ort_sha; (void)force;
    n->admins++;
    n->last_admin = kind;
    n->front_in_admin = n->c->life->host.front_facts(n->c->life->host.ctx);
    return 7;
}
static int32_t nf_step(void *st, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    nfake *n = st;
    (void)args; (void)nargs; (void)out;
    pthread_mutex_lock(&n->mu);
    n->steps++;
    n->last_step = (int)kind;
    pthread_mutex_unlock(&n->mu);
    return 0;
}
static int nf_steps(void) {
    pthread_mutex_lock(&g_nf.mu);
    int k = g_nf.steps;
    pthread_mutex_unlock(&g_nf.mu);
    return k;
}
static void nf_join(void) {
    for (int i = 0; i < nf_nthreads; i++) pthread_join(nf_threads[i], NULL);
    nf_nthreads = 0;
}
static const native_backend nf_all = {nf_open, nf_close, nf_post_work, nf_slow, nf_spawn, nf_signal, nf_admin, nf_step};
static const native_backend nf_no_admin = {nf_open, nf_close, nf_post_work, nf_slow, nf_spawn, nf_signal, NULL, nf_step};
static const native_backend nf_failing = {nf_open_fails, nf_close, nf_post_work, nf_slow, nf_spawn, nf_signal, nf_admin, nf_step};
#endif

static __attribute__((noinline)) void t_native(void) {   /* its fakes off main()'s frame (ASan) */
#ifdef CORE_WIN_POLICY
    static const char *const all = "{\"os\":\"win\",\"native\":[\"post_work\",\"slow_fact\",\"step\",\"admin\",\"helper\"]}";
    fake f;
    fake_init(&f);
    core_host h = host_for(&f);
    h.post_work = NULL;
    h.step = NULL;
    h.helper_spawn = NULL;
    h.helper_signal = NULL;
    /* no backend here: listed entries are "not provided"; the shell's still reach the shell */
    core *c = core_new(&h, CORE_ROLE_APP, all, CORE_NTS);
    f.c = c;
    CHECK(c != NULL && c->native == (NATIVE_POST_WORK | NATIVE_SLOW_FACT | NATIVE_STEP | NATIVE_ADMIN | NATIVE_HELPER));
    CHECK(c->life->native == 0 && c->life->host.ctx == c->life && c->life->shell.ctx == &f);
    CHECK(!c->life->host.post_work && !c->life->host.step && !c->life->host.slow_fact && !c->life->host.admin && !c->life->host.helper_spawn &&
          !c->life->host.helper_signal);
    CHECK(core_test_flow(c, CORE_TEST_FLOW_STEP) == CORE_E_STATE);   /* no worker: the flow can't start */
    CHECK(core_test_helper_start(c, 0) == 0);                        /* not started */
    CHECK(core_event(c, "{\"ev\":\"start\"}", CORE_NTS) == CORE_OK);
    core_commit(c);
    CHECK(f.renders == 1 && f.facts_calls >= 1);                      /* the shell's, with its ctx */
    core_free(c);
    fake_end(&f);

    /* the backend: every listed entry, with its state; the others still the shell's */
    fake_init(&f);
    memset(&g_nf.opened, 0, sizeof g_nf - offsetof(nfake, opened));
    c = core_new(&h, CORE_ROLE_APP, all, CORE_NTS);
    f.c = c;
    CHECK(core_test_native(c, &nf_all) == CORE_OK && g_nf.opened == 1 && g_nf.c == c);
    CHECK(core_test_flow(c, CORE_TEST_FLOW_STEP) == CORE_OK);
    PUMP_UNTIL(&f, logged(&f, "test flow: step 0") && !c->flow_running);
    CHECK(g_nf.works == 1 && g_nf.last_work == CORE_WORK_FLOW && nf_steps() == 1 && g_nf.last_step == CORE_STEP_OPEN_URL);
    CHECK(logged(&f, "test flow: step 0"));                          /* the core's log, through the shell's */
    arena fa = {0};
    core_facts out;
    facts_init(&out, &fa);
    CHECK(c->life->host.slow_fact(c->life->host.ctx, CORE_SLOW_R1, "7.2.19", CORE_NTS, &out) == 0 && g_nf.last_slow == CORE_SLOW_R1);
    CHECK(out.set[FACT_SHA256] && !strcmp(out.s[FACT_SHA256], "ab"));
    arena_free(&fa);
    CHECK(c->life->host.admin(c->life->host.ctx, CORE_ADMIN_RESTORE, "7.2.19", NULL, NULL, NULL, 0) == 7 && g_nf.last_admin == CORE_ADMIN_RESTORE);
    /* the shell can't see a native elevation: front_facts has ELEVATING during the call only */
    CHECK(g_nf.front_in_admin == (f.front | CORE_FRONT_ELEVATING));
    CHECK(c->life->host.front_facts(c->life->host.ctx) == f.front);
    CHECK(win_in_front(g_nf.front_in_admin, 0, CORE_TAB_PREPARE, 0) == 0 && win_in_front(f.front, 0, CORE_TAB_PREPARE, 0) == 1);
    uint64_t run = core_test_helper_start(c, 0);
    CHECK(run && g_nf.spawns == 1 && g_nf.last_run == run);
    c->life->host.helper_signal(c->life->host.ctx, run, CORE_HELPER_PAUSE);
    CHECK(g_nf.signals == 1 && g_nf.last_sig == CORE_HELPER_PAUSE && g_nf.last_run == run);
    nf_join();
    PUMP_UNTIL(&f, !pump(&f));            /* nothing queued holds the core but the run */
    core_free(c);
    CHECK(g_nf.closed == 0);              /* the run still holds the core */
    /* the run ends on the backend's reader thread after core_free, and the shell's loop has
     * stopped (its post_main drops the end job there): the last reference, and the close, are
     * that thread's */
    f.closed = 1;
    nf_end_arg ea = {c, run};
    pthread_t et;
    pthread_create(&et, NULL, nf_end_thread, &ea);
    pthread_join(et, NULL);
    CHECK(g_nf.closed == 1 && g_nf.closed_st == &g_nf && pthread_equal(g_nf.closed_on, et));
    fake_end(&f);
    CHECK(g_nf.closed == 1);

    /* a good open, then a core_new that fails: the state is closed once, with the core */
    fake_init(&f);
    memset(&g_nf.opened, 0, sizeof g_nf - offsetof(nfake, opened));
    core_test_native_default(&nf_all, 1);
    CHECK(core_new(&h, CORE_ROLE_APP, all, CORE_NTS) == NULL);
    core_test_native_default(NULL, 0);
    CHECK(g_nf.opened == 1 && g_nf.closed == 1 && g_nf.closed_st == &g_nf);
    CHECK(logged(&f, "core: the native entries couldn't be set up"));
    /* the default backend opened by core_new itself */
    core_test_native_default(&nf_all, 0);
    c = core_new(&h, CORE_ROLE_APP, all, CORE_NTS);
    core_test_native_default(NULL, 0);
    f.c = c;
    CHECK(c && g_nf.opened == 2 && g_nf.c == c && c->life->host.step);
    core_free(c);
    fake_end(&f);
    CHECK(g_nf.closed == 2);

    /* a listed entry the backend lacks fails cleanly; one not listed is the shell's */
    fake_init(&f);
    memset(&g_nf.opened, 0, sizeof g_nf - offsetof(nfake, opened));
    core_host ha = host_for(&f);
    ha.post_work = NULL;
    c = core_new(&ha, CORE_ROLE_APP, "{\"os\":\"win\",\"native\":[\"post_work\",\"admin\"]}", CORE_NTS);
    f.c = c;
    CHECK(core_test_native(c, &nf_no_admin) == CORE_OK);
    CHECK(!c->life->host.admin && c->life->host.post_work && c->life->host.step && c->life->host.step != h_step);
    CHECK(core_test_flow(c, CORE_TEST_FLOW_STEP) == CORE_OK);
    PUMP_UNTIL(&f, logged(&f, "test flow: step 0") && !c->flow_running);
    CHECK(steps_of(&f) == 1 && nf_steps() == 0 && g_nf.works == 1);   /* the shell's step, the backend's worker */
    /* a backend whose state can't be made: nothing listed is provided */
    CHECK(core_test_native(c, &nf_failing) == CORE_E_NOMEM && g_nf.closed == 1);
    CHECK(!c->life->host.post_work && !c->life->host.admin && c->life->host.step);
    core_free(c);
    nf_join();
    fake_end(&f);
    CHECK(g_nf.closed == 1);              /* not again: the failed open has nothing to close */

    /* what a backend's open reads: the config's paths by name; its log line reaches the shell's */
    fake_init(&f);
    core_host hs = host_for(&f);
    hs.step = NULL;
    c = core_new(&hs, CORE_ROLE_APP, "{\"os\":\"win\",\"version\":\"1.4.0\",\"native\":[\"step\"],\"paths\":{\"helper\":\"/h/rbsp-prepare.exe\"}}", CORE_NTS);
    f.c = c;
    CHECK(c && native_core_path(c, "helper") && !strcmp(native_core_path(c, "helper"), "/h/rbsp-prepare.exe"));
    CHECK(c && !native_core_path(c, "support") && !native_core_path(c, "nope"));
    CHECK(c && native_core_version(c) && !strcmp(native_core_version(c), "1.4.0"));
    native_core_log(c, "native: a line");
    CHECK(logged(&f, "native: a line"));
    core_free(c);
    fake_end(&f);

    /* no "native" in the config: the host table is the shell's own */
    fake_init(&f);
    c = new_core(&f);
    CHECK(c->life->host.ctx == &f && c->life->host.step == h_step && core_test_native(c, &nf_all) == CORE_E_STATE);
    core_free(c);
    fake_end(&f);
#endif
    LEAKCHECK();
}

/* the stateful model and delta fuzz, deterministic here (C29) */
void swift_vector_tests(int *passed, int *failed);
void pure_tests(int *passed, int *failed);

static void t_dfuzz(void) {
    static uint8_t ops[3000];
    uint64_t x = 0x9E3779B97F4A7C15ULL;
    int bad = 0;
    for (int run = 0; run < 100 && !bad; run++) {
        for (size_t i = 0; i < sizeof ops; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            ops[i] = (uint8_t)(x >> 24);
        }
        bad = dfuzz_run(ops, sizeof ops);
    }
    CHECK(bad == 0);
    LEAKCHECK();
}

int main(void) {
    t_selftest();
    t_new_refusals();
    t_events();
    t_commit_turn();
    t_rows_and_deltas();
    t_release_order();
    t_release_off_main();
    t_self_alias();
    t_dump();
    t_texts();
    t_json_numbers();
    t_menus();
    t_dialogs();
    t_flow();
    t_quit_beside_flow();
    t_quit_long_name();
    t_free_during_flow();
    t_worker_after_free();
    t_alloc_failure();
    t_helper_runs();
    t_helper_stub();
    t_reentry();
    t_thread_check();
    t_facts_sheet();
    t_reserved_terminals();
    t_pump_in_post_main();
    t_reserved_zero();
    t_win_drain();
    t_native();
    t_dfuzz();
    swift_vector_tests(&passed, &failed);   /* the pure functions against today's Swift (vectors.c) */
    pure_tests(&passed, &failed);           /* ... and against the Swift that can't run alone (pure.c) */
    flows_tests(&passed, &failed);
    win_texts_tests(&passed, &failed);      /* the Windows column (W-core) */
    win_front_tests(&passed, &failed);      /* the Windows in-front policy (W-core) */
    view_tests(&passed, &failed);
    printf("%d passed, %d failed (%s, %s)\n", passed, failed,
#ifdef CORE_WIN_POLICY
           "CORE_WIN_POLICY",
#else
           "mac",
#endif
#if defined(UNDER_TSAN) && defined(CORE_DEBUG)
           "tsan, CORE_DEBUG"
#elif defined(UNDER_TSAN)
           "tsan, release checks"
#elif defined(UNDER_ASAN) && defined(CORE_DEBUG)
           "asan+ubsan, CORE_DEBUG"
#elif defined(UNDER_ASAN)
           "asan+ubsan, release checks"
#else
           "plain, release checks"
#endif
    );
    return failed ? 1 : 0;
}
