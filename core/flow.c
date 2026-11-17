/* Flows (mac-core §3.7): one action at a time on the flow thread (host->post_work CORE_WORK_FLOW),
 * and the quit flow beside it (CORE_WORK_QUIT). A flow reads facts through the host, posts
 * progress to main, asks dialogs through host->dialog (which blocks its own thread only) and,
 * after every dialog, sends revalidate and waits for main's answer. A flow never touches the
 * core's state: only the shared life (the host table) and jobs posted to main. Each flow holds a
 * life reference until it returns (its job's), so it may outlive core_free.
 *
 * SCAFFOLD (M3): the plumbing, the quit flow's shape and two test flows; the action flows are M6's. */
#include "internal.h"
#include <stdio.h>

typedef struct {
    core_life *life;
    core_work kind;
    flow_fn fn;
    void *arg;
    void (*arg_free)(void *);   /* NULL: free() */
    core_job *done;             /* the flow's end, reserved at its start (E2) */
} flow_msg;

static void flow_done_job(core *c, void *payload) {
    core_work kind = *(core_work *)payload;
    if (c->role == CORE_ROLE_WATCHER) {   /* the watcher's rounds are over: the host exits (core.h Life cycle) */
        c->flow_running = 0;
        CALLBACK(c, c->life->host.close_reply(c->life->host.ctx, CORE_CLOSE_WATCHER_DONE, 1));
        return;
    }
    if (kind == CORE_WORK_FLOW) {   /* only the normal flow's end clears the normal flow's busy state */
        flows_flow_end_busy(c);   /* first: a busy(nil) the flow lost is run here (CF1, CF2) */
        c->flow_running = c->action_running = 0;
        status_update_buttons(c);   /* Stop Keeping… back on */
        core_mark_changed(c);
    }
    flows_flow_ended(c);   /* the delayed clicks, a pending cache offer (M6) */
}

static void free_arg(flow_msg *m) {
    if (m->arg_free) m->arg_free(m->arg);
    else free(m->arg);
}

static void flow_work(core_life *life, void *payload) {
    flow_msg *m = payload;
    m->fn(life, m->arg);
    job_post(life, m->done);   /* first the end, then the count (R13) */
    atomic_fetch_sub(&life->flows_out, 1);
}

/* a flow job dropped before it ran: after shutdown, or after core_free (E1) */
static void flow_dropped(void *payload) {
    flow_msg *m = payload;
    core_job_drop(m->done);
    atomic_fetch_sub(&m->life->flows_out, 1);
    free_arg(m);
}

int flow_start(core *c, core_work kind, flow_fn fn, void *arg, void (*arg_free)(void *)) {
    return flow_start_ex(c, kind, fn, arg, arg_free, 1);
}

int flow_start_ex(core *c, core_work kind, flow_fn fn, void *arg, void (*arg_free)(void *), int busy) {
    CORE_MAIN(c);
    flow_msg tmp = {c->life, kind, fn, arg, arg_free, NULL};
    if (c->flow_running && kind == CORE_WORK_FLOW) {
        free_arg(&tmp);
        return CORE_E_STATE;   /* one flow at a time (the buttons are off while one runs) */
    }
    flow_msg *m = malloc(sizeof *m);
    core_work *k = malloc(sizeof *k);
    if (k) *k = kind;
    core_job *done = m && k ? job_reserve(c->life, flow_done_job, k, NULL) : NULL;
    if (!done) {   /* the end can't be reserved: the flow doesn't start */
        free(k);
        free(m);
        free_arg(&tmp);
        return CORE_E_NOMEM;
    }
    tmp.done = done;
    *m = tmp;
    int was_running = c->action_running;
    if (kind == CORE_WORK_FLOW) {
        atomic_store(&c->life->asked, 0);   /* the new flow hasn't asked anything yet (F4) */
        c->flow_running = 1;
        if (busy) c->action_running = 1;
    }
    atomic_fetch_add(&c->life->flows_out, 1);
    int r = core_post_work(c->life, kind, flow_work, m, flow_dropped);
    if (r) {   /* not accepted: flow_dropped ran; roll the busy state back */
        if (kind == CORE_WORK_FLOW) {
            c->flow_running = 0;
            c->action_running = was_running;
        }
        return r;
    }
    if (kind == CORE_WORK_FLOW) status_update_buttons(c);   /* Stop Keeping… off when it turned busy on */
    core_mark_changed(c);
    return CORE_OK;
}

/* ---------------------------------------------------------------- waiting on main */

typedef struct {
    plat_mutex mu;
    plat_cond cv;
    int done, answer;
} core_wait;

typedef struct { core_wait *w; int (*q)(core *, void *); void *arg; int if_gone; } query_msg;

static void wait_answer(core_wait *w, int answer) {
    plat_mutex_lock(&w->mu);
    w->answer = answer;
    w->done = 1;
    plat_cond_signal(&w->cv);
    plat_mutex_unlock(&w->mu);
}

static void query_job(core *c, void *payload) {
    query_msg *m = payload;
    wait_answer(m->w, m->q(c, m->arg));
}

static void query_dropped(void *payload) {
    query_msg *m = payload;
    wait_answer(m->w, m->if_gone);
}

int flow_query(core_life *life, int (*q)(core *, void *), void *arg, int if_gone) {
    if (!life_alive(life)) return if_gone;
    core_wait w = {.done = 0};
    plat_mutex_init(&w.mu);
    plat_cond_init(&w.cv);
    query_msg *m = malloc(sizeof *m);
    if (!m) return if_gone;
    *m = (query_msg){&w, q, arg, if_gone};
    core_post_main(life, query_job, m, query_dropped, 0, 0);   /* on failure query_dropped has answered */
    plat_mutex_lock(&w.mu);
    while (!w.done) plat_cond_wait(&w.cv, &w.mu);
    int answer = w.answer;
    plat_mutex_unlock(&w.mu);
    plat_mutex_destroy(&w.mu);
    plat_cond_destroy(&w.cv);
    return answer;
}

typedef struct { int domain; uint64_t gen; } reval_arg;

static int reval_q(core *c, void *arg) {
    reval_arg *a = arg;
    /* M4/M6: the state the dialog was about (today's `estimate == e` guard) */
    return c->gen[a->domain] == a->gen;
}

int flow_revalidate(core_life *life, int domain, uint64_t gen) {
    reval_arg a = {domain, gen};
    return flow_query(life, reval_q, &a, 0);
}

/* ---------------------------------------------------------------- the Prepare run, from a flow (M4) */

static int run_name_q(core *c, void *arg) {
    const char *name = prep_run_name(c);
    *(char **)arg = strdup(name ? name : "");
    return 1;
}
static int stop_q(core *c, void *arg) {
    (void)arg;
    prep_stop(c);
    return 1;
}
static int running_q(core *c, void *arg) {
    (void)arg;
    return prep_running(c);
}

char *flow_run_name(core_life *life) {
    char *name = NULL;
    flow_query(life, run_name_q, &name, 0);
    return name;
}

/* stopAndWait (PrepareView.swift:878) on the flow's own thread, never by spinning main: the run is
 * stopped and its set written; 1 once no run goes, 0 when it still runs after 30 s (or the core
 * is gone). Polled until plat.h's timer replaces the poll. */
int flow_stop_and_wait(core_life *life) {
    /* a query that couldn't be asked (out of memory, or the core gone): no, as confirm_stop_preparing
     * refuses when its own stop can't be asked; the caller's decision still comes (E2) */
    if (flow_query(life, stop_q, NULL, -1) != 1) return 0;
    uint64_t end = plat_now_mono_ns() + 30ull * 1000000000ull;
    for (;;) {
        int r = flow_query(life, running_q, NULL, -1);
        if (r < 0) return 0;
        if (r != 1) return 1;
        if (plat_now_mono_ns() >= end) return 0;
        plat_sleep_ms(100);
    }
}

/* ---------------------------------------------------------------- the quit flow (D2) */

static int modal_q(core *c, void *arg) {
    (void)arg;
    int open;
    CALLBACK(c, open = c->life->host.modal_open(c->life->host.ctx));
    return open != 0;
}

/* main, from its own job: the decision answers every pending what (QUIT supersedes WINDOW) */
static void quit_decided_job(core *c, void *payload) {
    int proceed = *(int *)payload;
    c->quit_flow_running = 0;
    if (proceed) c->preparing = c->paused = 0;   /* the run was stopped and waited for on the quit flow's thread (a test hook's run: no run) */
    for (int what = CORE_CLOSE_WINDOW; what <= CORE_CLOSE_QUIT; what++) {
        if (!c->close_pending[what]) continue;
        c->close_pending[what] = 0;
        c->close_approved[what] = proceed;
        CALLBACK(c, c->life->host.close_reply(c->life->host.ctx, (core_close)what, proceed));
    }
    core_mark_changed(c);
}

/* the quit flow's arg: its decision job, reserved before the flow starts (E2) */
static void drop_decision(void *arg) { core_job_drop(arg); }

static void quit_flow(core_life *life, void *arg) {
    core_job *decision = arg;
    /* while a dialog is up, wait until it ends before asking (Quit stays disabled under an
     * app-modal dialog, as today). M3 polls; plat.h's timer replaces the poll. */
    while (flow_query(life, modal_q, NULL, -1) == 1) plat_sleep_ms(20);
    int proceed = 0;
    if (life_alive(life) && life->host.dialog) {
        char msg[256], stop[64], cancel[64];
        char *name = flow_run_name(life);   /* "“x” is being prepared": the whole name (SO12) */
        core_slot pl = {CORE_SLOT_playlist, 0, name ? name : "", name ? strlen(name) : 0};
        size_t n = 0;
        core_text(life->os, T_STOP_PREPARING_TITLE, &pl, 1, NULL, 0, &n);   /* the title's length */
        char *title = malloc(n + 1);
        if (title) core_text(life->os, T_STOP_PREPARING_TITLE, &pl, 1, title, n + 1, &n);
        free(name);
        core_text(life->os, T_STOP_PREPARING_MSG, NULL, 0, msg, sizeof msg, &n);
        core_text(life->os, T_STOP_AND_QUIT, NULL, 0, stop, sizeof stop, &n);
        core_text(life->os, T_CANCEL, NULL, 0, cancel, sizeof cancel, &n);
        core_dialog d;
        memset(&d, 0, sizeof d);
        d.id = T_STOP_PREPARING_TITLE;
        d.title = title ? title : "";
        d.message = msg;
        d.nbuttons = 2;
        d.buttons[0] = stop;
        d.buttons[1] = cancel;
        d.def = 0;
        d.cancel = 1;
        arena a = {0};
        core_reply r;
        reply_init(&r, &a);
        if (life->host.dialog(life->host.ctx, &d, &r) == 0 && r.button == 0) {
            core_log(life, "prepare: stopped for quit");
            proceed = flow_stop_and_wait(life);   /* confirmStopPreparing: true once nothing is being prepared */
        }
        arena_free(&a);
        free(title);
    }
    *(int *)decision->payload = proceed;
    job_post(life, decision);   /* reserved: it can't be lost (after core_free, core_run drops it) */
}

int quit_flow_start(core *c) {
    int *p = calloc(1, sizeof *p);
    core_job *decision = p ? job_reserve(c->life, quit_decided_job, p, NULL) : NULL;
    if (!decision) {   /* core_close_asked then answers REFUSE */
        free(p);
        return CORE_E_NOMEM;
    }
    c->quit_flow_running = 1;
    int r = flow_start(c, CORE_WORK_QUIT, quit_flow, decision, drop_decision);
    if (r) c->quit_flow_running = 0;
    return r;
}

/* ---------------------------------------------------------------- the test flows */

#ifdef CORE_TEST_HOOKS
typedef struct { int mode; uint64_t gen; } test_flow_arg;

static void test_flow(core_life *life, void *arg) {
    test_flow_arg *a = arg;
    char line[96];
    if (a->mode == CORE_TEST_FLOW_STEP) {
        int32_t rc = CORE_E_STATE;   /* no step entry: fails cleanly */
        if (life->host.step && !(life->native & NATIVE_STEP)) {
            static const char *const args[] = {"https://example.invalid/"};
            arena fa = {0};
            core_facts out;
            facts_init(&out, &fa);
            rc = life->host.step(life->host.ctx, CORE_STEP_OPEN_URL, args, 1, &out);
            arena_free(&fa);
        }
        snprintf(line, sizeof line, "test flow: step %d", (int)rc);   /* fixed format, numbers only */
        core_log(life, line);
        free(a);
        return;
    }
    core_dialog d;
    memset(&d, 0, sizeof d);
    d.id = T_APP_NAME;
    d.title = core_text_template(life->os, T_APP_NAME);
    d.message = "";
    d.nbuttons = 2;
    d.buttons[0] = core_text_template(life->os, T_OK);
    d.buttons[1] = core_text_template(life->os, T_CANCEL);
    d.def = 0;
    d.cancel = 1;
    arena a2 = {0};
    core_reply r;
    reply_init(&r, &a2);
    if (!life->host.dialog || life->host.dialog(life->host.ctx, &d, &r)) r.button = d.cancel;
    int still = flow_revalidate(life, DOM_PREPARE, a->gen);
    snprintf(line, sizeof line, "test flow: button %d, revalidate %d", (int)r.button, still);
    core_log(life, line);
    arena_free(&a2);
    free(a);
}

int32_t core_test_flow(core *c, int32_t mode) {
    if (!c || !core_on_main(c) || c->cb_depth > 0 || c->role != CORE_ROLE_APP) return CORE_E_STATE;
    test_flow_arg *a = malloc(sizeof *a);
    if (!a) return CORE_E_NOMEM;
    a->mode = mode;
    a->gen = c->gen[DOM_PREPARE];
    core_life *l = life_retain(c->life);   /* inside an entry frame (E3) */
    c->entry++;
    int32_t r = flow_start(c, CORE_WORK_FLOW, test_flow, a, NULL);
    core_leave(c);
    life_release(l);
    return r;
}
#endif
