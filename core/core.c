/* The core handle: life cycle, jobs, the callback rules (re-entry, deferral, the thread check),
 * the commit turn, events, close and quit, helper runs, menus' live state, fact sheets, and the
 * test hooks.
 *
 * SCAFFOLD (M3): the plumbing is real; the decisions behind it (refresh, both tabs, flows, the
 * helper parser) are M4-M6's and marked as such. */
#include "internal.h"
#include <stdio.h>
#ifdef CORE_WIN_POLICY
#include "native.h"
#endif

uint32_t core_api_version(void) { return CORE_API_VERSION; }

#ifdef CORE_DEBUG
atomic_long core_debug_live;
/* a marker only debug builds carry: the release gate (core/build.sh lib) checks it is absent */
static const char debug_marker[] = "RBSP-CORE-DEBUG-BUILD";
_Noreturn void core_fail_(const char *what, const char *func) {
    /* no format string built from data: fixed pieces only */
    fputs(debug_marker, stderr);
    fputs(": check failed in ", stderr);
    fputs(func, stderr);
    fputs(": ", stderr);
    fputs(what, stderr);
    fputs("\n", stderr);
    abort();
}
long core_debug_live_objects(void) { return atomic_load(&core_debug_live); }
#else
long core_debug_live_objects(void) { return -1; }
#endif

int core_on_main(const core *c) { return c && plat_thread_equal(plat_thread_self(), c->main_thread); }
int life_on_main(const core_life *l) { return plat_thread_equal(plat_thread_self(), l->main_thread); }

/* ---------------------------------------------------------------- life and jobs */

core_life *life_retain(core_life *l) {
    atomic_fetch_add_explicit(&l->refs, 1, memory_order_relaxed);
    return l;
}

void life_release(core_life *l) {
    if (l && atomic_fetch_sub_explicit(&l->refs, 1, memory_order_acq_rel) == 1) {
#ifdef CORE_WIN_POLICY
        native_release(l);
#endif
        LIVE_DEC();
        free(l);
    }
}

int life_alive(const core_life *l) { return atomic_load_explicit(&l->alive, memory_order_acquire); }

/* host->log, counted as a callback when it is called on main */
void core_log(core_life *life, const char *line) {
    if (!life->host.log) return;
    core *c = life_on_main(life) && life_alive(life) ? &life->c : NULL;
    if (c) c->cb_depth++;
    life->host.log(life->host.ctx, line, strlen(line));
    if (c) c->cb_depth--;
}

static core_job *job_new(core_life *life, void *payload, void (*drop_fn)(void *)) {
#ifdef CORE_TEST_HOOKS
    int f = atomic_load(&life->fail_allocs);
    while (f > 0 && !atomic_compare_exchange_weak(&life->fail_allocs, &f, f - 1)) {}
    if (f > 0) return NULL;
    int a = atomic_load(&life->fail_after);
    while (a >= 0 && !atomic_compare_exchange_weak(&life->fail_after, &a, a - 1)) {}
    if (a == 0) {   /* this is the one; the rest of its count fail after it */
        atomic_fetch_add(&life->fail_allocs, atomic_exchange(&life->fail_after_more, 0));
        return NULL;
    }
#endif
    core_job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    LIVE_INC();
    j->life = life_retain(life);
    j->payload = payload;
    j->drop_fn = drop_fn;
    return j;
}

static void job_free(core_job *j) {
    life_release(j->life);
    free(j->payload);
    free(j);
    LIVE_DEC();
}

/* a job that can't run: its cleanup, then free */
static void job_drop(core_job *j) {
    if (j->drop_fn) j->drop_fn(j->payload);
    job_free(j);
}

static void host_post_main(core_life *life, core_job *j) {
    core *c = life_on_main(life) && life_alive(life) ? &life->c : NULL;
    if (c) c->cb_depth++;
    life->host.post_main(life->host.ctx, j);
    if (c) c->cb_depth--;
}

core_job *job_reserve(core_life *life, job_fn fn, void *payload, void (*drop_fn)(void *)) {
    core_job *j = job_new(life, payload, drop_fn);
    if (!j) return NULL;
    j->on_main = 1;
    j->main_fn = fn;
    return j;
}

void job_post(core_life *life, core_job *j) { host_post_main(life, j); }

int core_post_main(core_life *life, job_fn fn, void *payload, void (*drop_fn)(void *), int domain, uint64_t gen) {
    core_job *j = job_new(life, payload, drop_fn);
    if (!j) {   /* out of memory: the job is lost, and whoever waits on it is told so */
        if (drop_fn) drop_fn(payload);
        free(payload);
        return CORE_E_NOMEM;
    }
    j->on_main = 1;
    j->main_fn = fn;
    j->domain = domain;
    j->gen = gen;
    host_post_main(life, j);
    return CORE_OK;
}

int core_post_work(core_life *life, core_work kind, work_fn fn, void *payload, void (*drop_fn)(void *)) {
    if (!life->host.post_work || (life->native & NATIVE_POST_WORK)) {
        /* "native" post_work is the DLL's Windows code (W-core): not here yet */
        if (drop_fn) drop_fn(payload);
        free(payload);
        return CORE_E_STATE;
    }
    core_job *j = job_new(life, payload, drop_fn);
    if (!j) {
        if (drop_fn) drop_fn(payload);
        free(payload);
        return CORE_E_NOMEM;
    }
    j->work_fn = fn;
    core *c = life_on_main(life) && life_alive(life) ? &life->c : NULL;
    if (c) c->cb_depth++;
    life->host.post_work(life->host.ctx, kind, j);
    if (c) c->cb_depth--;
    return CORE_OK;
}

/* ---------------------------------------------------------------- core calls on main (D2, D11) */

static void free_now(core *c);
static void run_main_job(core *c, core_job *j);

/* the outermost core call on main is returning: deferred jobs, the scratch, a deferred free */
static void leave(core *c) {
    if (c->entry > 1) {
        c->entry--;
        return;
    }
    while (c->deferred) {   /* nested core_runs from a pump inside a callback, in order */
        core_job *j = c->deferred;
        c->deferred = j->next;
        if (!c->deferred) c->deferred_tail = NULL;
        if (c->free_pending) job_drop(j);
        else run_main_job(c, j);
    }
    arena_reset(&c->scratch);
    c->entry = 0;
    if (c->free_pending) free_now(c);
}

void core_leave(core *c) { leave(c); }

/* main only */
static void warn_nested(core *c, const char *line) {
    if (c->warned_nested) return;
    c->warned_nested = 1;
    core_log(c->life, line);
}

/* any thread: the off-main paths race each other (R8) */
static void warn_thread(core *c, const char *line) {
    if (atomic_exchange_explicit(&c->warned_thread, 1, memory_order_relaxed)) return;
    core_log(c->life, line);
}

/* a main-only call: 0 to go on, else what to return. In CORE_DEBUG a violation aborts. */
static int main_entry(core *c, int allowed_in_callback) {
    if (!core_on_main(c)) {
        CORE_VIOLATION("a main-only core call off main");
        warn_thread(c, "core: a main-only call came from another thread and was refused");
        return CORE_E_THREAD;
    }
    if (c->cb_depth > 0 && !allowed_in_callback) {
        CORE_VIOLATION("a core call from inside a host callback");
        warn_nested(c, "core: a call from inside a host callback was refused");
        return CORE_E_STATE;
    }
    return CORE_OK;
}

static void run_main_job(core *c, core_job *j) {
    if (!j->gen || j->gen == c->gen[j->domain]) j->main_fn(c, j->payload);
    else if (j->drop_fn) j->drop_fn(j->payload);   /* a stale result: dropped here, in one place */
    job_free(j);
}

void core_run(core_job *j) {
    if (!j) return;
    core_life *l = j->life;
    if (!j->on_main) {
        if (!life_alive(l)) {   /* queued before core_free, not started: dropped */
            job_drop(j);
            return;
        }
        j->work_fn(l, j->payload);
        job_free(j);
        return;
    }
    if (!life_on_main(l)) {   /* a main job off main: back to main */
        CORE_VIOLATION("core_run of a main job off main");
        l->host.post_main(l->host.ctx, j);
        return;
    }
    if (!life_alive(l)) {
        job_drop(j);
        return;
    }
    core *c = &l->c;
    if (c->cb_depth > 0) {   /* a nested pump inside a callback: run when the outer call returns */
        CORE_VIOLATION("core_run inside a host callback");
        j->next = NULL;
        if (c->deferred_tail) c->deferred_tail->next = j; else c->deferred = j;
        c->deferred_tail = j;
        return;
    }
    life_retain(l);   /* a deferred free inside leave mustn't take the life away under us */
    c->entry++;
    run_main_job(c, j);
    leave(c);
    life_release(l);
}

void core_job_drop(core_job *j) {
    if (j) job_drop(j);
}

/* ---------------------------------------------------------------- the commit turn (D1) */

static void do_commit(core *c) {
    if (c->role != CORE_ROLE_APP || !model_needs_render(&c->m)) return;   /* nothing moved: no render */
    core_snap *s = model_freeze(&c->m, 1);
    if (!s) {
        core_log(c->life, "core: out of memory at a commit");
        return;
    }
    CALLBACK(c, c->life->host.render(c->life->host.ctx, s, &s->delta));   /* the shell owns this reference */
}

/* On the Mac the commit job renders and ends the turn. On Windows it only wakes the drain, whose
 * core_commit renders once (E4). */
static void commit_job(core *c, void *payload) {
    (void)payload;
    c->commit_posted = 0;
    if (c->os != CORE_OS_WIN) do_commit(c);
}

static void commit_dropped(void *payload) { (void)payload; }

void core_mark_changed(core *c) {
    if (c->role != CORE_ROLE_APP || c->commit_posted) return;
    c->commit_posted = 1;
    if (core_post_main(c->life, commit_job, NULL, commit_dropped, 0, 0)) c->commit_posted = 0;
}

void core_commit(core *c) {
    if (!c || main_entry(c, 0)) return;
    c->entry++;
    do_commit(c);
    leave(c);
}

core_snap *core_snap_current(core *c) {
    if (!c || main_entry(c, 0) || c->role != CORE_ROLE_APP) return NULL;
    return model_freeze(&c->m, 0);
}

/* ---------------------------------------------------------------- core_new, core_free */

/* in P_* order (internal.h). M4 adds model (rekordbox's model file, modelDir + "/hdemucs.onnx"),
 * root_ort (rootOrt), rekordbox (rbApp) and kept (keptDir): the prepare helper's arguments */
static const char *const path_keys[] = {"home", "support", "app", "helper", "cache_default", "log", "tmp", "watcher",
                                        "model", "root_ort", "rekordbox", "kept"};
_Static_assert(sizeof path_keys / sizeof *path_keys == sizeof ((core *)0)->paths / sizeof (char *), "a key per path");

#ifdef CORE_WIN_POLICY
static const struct { const char *name; uint32_t bit; } native_names[] = {
    {"post_work", NATIVE_POST_WORK}, {"slow_fact", NATIVE_SLOW_FACT}, {"step", NATIVE_STEP},
    {"admin", NATIVE_ADMIN}, {"helper", NATIVE_HELPER},
};
#endif

static int read_config(core *c, const char *p, size_t n) {
    arena a = {0};
    jval root;
    int r = json_parse(&a, p, n, &root), has_os = 0;
#ifdef CORE_WIN_POLICY
    int has_tabs = 0;
#endif
    if (!r && root.type != J_OBJ) r = CORE_E_PARSE;
    for (size_t i = 0; !r && i < root.n; i++) {
        const char *k = root.keys[i];
        const jval *v = &root.items[i];
        if (!strcmp(k, "version")) {
            if (v->type != J_STR || !(c->version = strdup(v->s))) r = CORE_E_UNKNOWN;
        } else if (!strcmp(k, "os")) {
            has_os = 1;
            if (v->type == J_STR && !strcmp(v->s, "mac")) c->os = CORE_OS_MAC;
#ifdef CORE_WIN_POLICY
            else if (v->type == J_STR && !strcmp(v->s, "win")) c->os = CORE_OS_WIN;
#endif
            else r = CORE_E_UNKNOWN;
        } else if (!strcmp(k, "paths") && v->type == J_OBJ) {
            for (size_t j = 0; !r && j < v->n; j++) {
                size_t which = 0;
                while (which < sizeof path_keys / sizeof *path_keys && strcmp(path_keys[which], v->keys[j])) which++;
                if (which == sizeof path_keys / sizeof *path_keys || v->items[j].type != J_STR || c->paths[which]) r = CORE_E_UNKNOWN;
                else if (!(c->paths[which] = strdup(v->items[j].s))) r = CORE_E_NOMEM;
            }
#ifdef CORE_WIN_POLICY
        } else if (!strcmp(k, "native") && v->type == J_ARR) {
            for (size_t j = 0; !r && j < v->n; j++) {
                size_t w = 0;
                while (w < sizeof native_names / sizeof *native_names && (v->items[j].type != J_STR || strcmp(native_names[w].name, v->items[j].s))) w++;
                if (w == sizeof native_names / sizeof *native_names || (c->native & native_names[w].bit)) r = CORE_E_UNKNOWN;
                else c->native |= native_names[w].bit;
            }
        } else if (!strcmp(k, "prepare_tabs") && v->type == J_BOOL) {
            has_tabs = 1;
            c->prepare_tabs = v->b;
#endif
        } else r = CORE_E_UNKNOWN;
    }
    if (!r && !has_os) r = CORE_E_UNKNOWN;   /* "os" is required (D7) */
#ifdef CORE_WIN_POLICY
    if (!r && has_tabs && c->os != CORE_OS_WIN) r = CORE_E_UNKNOWN;   /* Windows' key: the Mac always shows the tabs */
#endif
    arena_free(&a);
    return r;
}

/* each entry the role needs is in the table, unless the config hands it to the DLL ("native"),
 * and then it must be NULL there */
static int host_ok(const core_host *h, core_role role, uint32_t native) {
    struct { const void *fn; uint32_t native_bit; int app, watcher; } e[] = {
        {(const void *)h->render, 0, 1, 0},       {(const void *)h->front_facts, 0, 1, 0},
        {(const void *)h->modal_open, 0, 1, 0},   {(const void *)h->facts, 0, 1, 1},
        {(const void *)h->post_main, 0, 1, 1},    {(const void *)h->post_work, NATIVE_POST_WORK, 1, 1},
        {(const void *)h->dialog, 0, 1, 1},       {(const void *)h->close_reply, 0, 1, 1},
        {(const void *)h->log, 0, 1, 1},          {(const void *)h->step, NATIVE_STEP, 0, 1},
        {(const void *)h->slow_fact, NATIVE_SLOW_FACT, 0, 0}, {(const void *)h->admin, NATIVE_ADMIN, 0, 0},
        {(const void *)h->helper_spawn, NATIVE_HELPER, 0, 0}, {(const void *)h->helper_signal, NATIVE_HELPER, 0, 0},
    };
    for (size_t i = 0; i < sizeof e / sizeof *e; i++) {
        int needed = role == CORE_ROLE_APP ? e[i].app : e[i].watcher;
        if (e[i].native_bit & native) {
            if (e[i].fn) return 0;   /* listed as native, yet given */
        } else if (needed && !e[i].fn) return 0;
    }
    return 1;
}

static void host_log(const core_host *host, const char *line) {
    if (host->log) host->log(host->ctx, line, strlen(line));
}

core *core_new(const core_host *host, core_role role, const char *config, size_t config_len) {
    if (!host || host->size < sizeof(core_host)) return NULL;
    if (host->reserved) {
        host_log(host, "core: the host table's reserved field isn't 0");
        return NULL;
    }
    if ((int)role != CORE_ROLE_APP && (int)role != CORE_ROLE_WATCHER) return NULL;
    if (!config) {
        host_log(host, "core: no start config");
        return NULL;
    }
    core_life *life = calloc(1, sizeof *life);
    if (!life) return NULL;
    LIVE_INC();
    atomic_init(&life->refs, 1);   /* the core's own reference, released by core_free */
    atomic_init(&life->alive, 1);
    atomic_init(&life->flows_out, 0);
    atomic_init(&life->flow_quits, 0);
    atomic_init(&life->flow_marked, 0);
#ifdef CORE_TEST_HOOKS
    atomic_init(&life->fail_after, -1);   /* core_test_fail_alloc_after: none */
#endif
    life->host = *host;
    life->main_thread = plat_thread_self();
    core *c = &life->c;
    c->life = life;
    c->main_thread = life->main_thread;
    atomic_init(&c->warned_thread, 0);
    c->role = role;
    c->os = CORE_OS_MAC;
    for (int d = 0; d < DOM_COUNT; d++) c->gen[d] = 1;
    if (config_len == CORE_NTS) config_len = strlen(config);
    int r = read_config(c, config, config_len);
    if (r) host_log(host, "core: the start config is refused");
    if (!r && !host_ok(host, role, c->native)) {
        host_log(host, "core: the host table misses an entry the role needs, or gives one the config calls native");
        r = CORE_E_ARG;
    }
    life->os = c->os;
    life->native = c->native;
#if defined(_WIN32) && defined(CORE_NATIVE_WIN)
    /* Windows' own Program Files, before the backend opens (it builds the admin root from it):
     * unreadable or refused, the DLL doesn't start (fails closed) */
    if (!r && native_win_programs(c->win_programs, sizeof c->win_programs)) {
        host_log(host, "core: Windows' Program Files folder can't be read");
        r = CORE_E_STATE;
    }
#endif
#ifdef CORE_WIN_POLICY
    if (!r && c->native && native_install(life, c->native)) {
        host_log(host, "core: the native entries couldn't be set up");
        r = CORE_E_NOMEM;
    }
#endif
    flows_init(c);
    view_init(c);
    prep_init(c);
    status_init(c);
    if (!r && role == CORE_ROLE_APP) {
        if (model_init(&c->m)) r = CORE_E_NOMEM;
        /* nothing selected, nothing running, until the shell or M4 says otherwise */
        else if (model_set_i64(&c->m, F_STATUS_SELECTED_ROW, -1) || model_set_i64(&c->m, F_PREP_SELECTED_ROW, -1) ||
                 model_set_i64(&c->m, F_PREP_RUN_ROW, -1) || model_set_i64(&c->m, F_PREP_SCROLL_ROW, -1))
            r = CORE_E_NOMEM;
    }
    if (r) {
        free_now(c);
        return NULL;
    }
    return c;
}

static void free_now(core *c) {
    core_life *l = c->life;
    atomic_store_explicit(&l->alive, 0, memory_order_release);
    while (c->deferred) {
        core_job *j = c->deferred;
        c->deferred = j->next;
        job_drop(j);
    }
    c->deferred_tail = NULL;
    if (c->role == CORE_ROLE_APP) model_free(&c->m);
    status_free(c);
    prep_free(c);
    view_free(c);
    flows_free(c);
    arena_free(&c->scratch);
    free(c->version);
    c->version = NULL;
    for (size_t i = 0; i < sizeof c->paths / sizeof *c->paths; i++) {
        free(c->paths[i]);
        c->paths[i] = NULL;
    }
    life_release(l);   /* the header lives on inside the life while jobs, flows and runs hold it */
}

void core_free(core *c) {
    if (!c) return;
    if (!core_on_main(c)) {
        CORE_VIOLATION("core_free off main");
        warn_thread(c, "core: core_free came from another thread and was ignored");
        return;
    }
    if (!life_alive(c->life) || c->free_pending) return;
    if (c->cb_depth > 0 || c->entry > 0) {   /* inside a callback: at the outermost return */
        if (c->cb_depth > 0) CORE_VIOLATION("core_free inside a host callback");
        c->free_pending = 1;
        return;
    }
    free_now(c);
}

/* ---------------------------------------------------------------- events */

const event_meta core_event_meta[CORE_EVENT_COUNT] = {
#define EVENT(id, name, os, keys) CORE_KEEP_##os([id] = {name, keys},)
#include "events.def"
};

int core_event_lookup(const char *name, size_t n) {
    for (int i = 0; i < CORE_EVENT_COUNT; i++)
        if (strlen(core_event_meta[i].name) == n && !memcmp(core_event_meta[i].name, name, n)) return i;
    return -1;
}

/* key is one of the space-separated names in keys */
static int key_listed(const char *keys, const char *key, size_t n) {
    for (const char *p = keys; *p;) {
        const char *e = strchr(p, ' ');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == n && !memcmp(p, key, n)) return 1;
        p += k + (e != NULL);
    }
    return 0;
}

static const jval *member(const jval *ev, const char *key, int type) {
    const jval *v = json_get(ev, key);
    return v && v->type == type && (type != J_NUM || v->is_int) ? v : NULL;
}

/* a table an event may name by row */
static int row_table(const char *name) {
    if (!strcmp(name, "T_STATUS_SETS")) return T_STATUS_SETS;
    if (!strcmp(name, "T_PREP_OUTLINE")) return T_PREP_OUTLINE;
    if (!strcmp(name, "T_PREP_TRACKS")) return T_PREP_TRACKS;
    return -1;
}

/* the row event's "gen" names the table's current list; row is in it (or -1 when allowed). 1: go
 * on; 0: stale, ignored quietly; < 0: a bad member */
static int row_event_ok(core *c, int table, const jval *row, const jval *gen, int allow_none) {
    if (!row || !gen || gen->i < 0) return CORE_E_ARG;
    if ((uint64_t)gen->i != c->m.tab[table]->list_gen) {
        core_log(c->life, "core: an event for a list no longer shown is ignored");
        return 0;
    }
    if (row->i < (allow_none ? -1 : 0) || row->i >= c->m.tab[table]->n) return CORE_E_ARG;
    return 1;
}

#ifdef CORE_TEST_HOOKS
static void stub_select(core *c);
#endif

static int handle_app_event(core *c, int id, const jval *ev) {
    switch (id) {
    case EV_START:
        view_start(c);   /* Controller.init: the first tab, the buttons before the window appears */
        view_location_update(c, 1);   /* LocationMonitor.startObserving's first update(size: true) */
        return CORE_OK;
    case EV_ACTIVATED:   /* applicationDidBecomeActive (main.swift:165) */
        view_location_measure(c);   /* LocationMonitor.shared.update(), in the background */
        view_prepare_changed(c, 1);
        view_refresh(c);
#ifdef CORE_WIN_POLICY
        view_r1_check(c);   /* R1 at activation (app.md §5.2) */
        flows_win_inventory_changed(c);   /* the shell can coalesce activation with discovery */
#endif
        flows_check_request(c);   /* Q12: the watcher's request, with EV_REOPENED's guards */
        view_check_watcher_on(c);   /* checkWatcherOn: the reminders line follows Login Items (M72) */
        return CORE_OK;
    case EV_DEMINIATURIZED:   /* windowDidDeminiaturize: a tab now in front, nothing else says so */
        view_prepare_changed(c, 1);
        return CORE_OK;
    case EV_TAB: {
        const jval *t = member(ev, "tab", J_NUM);
        if (!t || t->i < CORE_TAB_STATUS || t->i > CORE_TAB_MANAGE) return CORE_E_ARG;
#ifdef CORE_WIN_POLICY
        if (view_tabs_hidden(c) && t->i != CORE_TAB_MANAGE) {
            core_log(c->life, "core: a hidden tab is ignored");
            return CORE_OK;
        }
#endif
        if (model_get_i64(&c->m, F_TAB) == t->i) return CORE_OK;
        if (model_set_i64(&c->m, F_TAB, t->i)) return CORE_E_NOMEM;
        core_mark_changed(c);
        view_tab_shown(c, (int)t->i);   /* its viewWillAppear */
        return CORE_OK;
    }
    case EV_SELECT: {
        const jval *t = member(ev, "table", J_STR);
        int table = t ? row_table(t->s) : -1;
        if (table != T_STATUS_SETS && table != T_PREP_OUTLINE) return CORE_E_ARG;
        int ok = row_event_ok(c, table, member(ev, "row", J_NUM), member(ev, "gen", J_NUM), 1);
        if (ok <= 0) return ok;
        int32_t row = (int32_t)member(ev, "row", J_NUM)->i;
#ifdef CORE_TEST_HOOKS
        if (table == T_PREP_OUTLINE) stub_select(c);   /* the G1 spike's stub: the estimate shown ends */
#endif
        if (model_set_i64(&c->m, table == T_STATUS_SETS ? F_STATUS_SELECTED_ROW : F_PREP_SELECTED_ROW, row)) return CORE_E_NOMEM;
        core_mark_changed(c);
        if (table == T_PREP_OUTLINE) prep_user_select(c, row);   /* outlineViewSelectionDidChange */
        else status_update_buttons(c);                            /* tableViewSelectionDidChange */
        return CORE_OK;
    }
    case EV_FOLDER: {
        const jval *open = member(ev, "open", J_BOOL);
        if (!open) return CORE_E_ARG;
        int ok = row_event_ok(c, T_PREP_OUTLINE, member(ev, "row", J_NUM), member(ev, "gen", J_NUM), 0);
        if (ok > 0) prep_user_folder(c, (int32_t)member(ev, "row", J_NUM)->i, open->b);   /* the closed folders */
        return ok < 0 ? ok : CORE_OK;
    }
    case EV_CLICK:
    case EV_MENU: {
        const jval *idv = member(ev, "id", J_STR);
        if (!idv) return CORE_E_ARG;
#ifdef CORE_WIN_POLICY
        /* R1's Don't Ask Again names the versions its button showed: {"ev":"r1_dismiss"} only */
        if (c->os == CORE_OS_WIN && idv->len == sizeof "r1_dismiss" - 1 && !memcmp(idv->s, "r1_dismiss", idv->len)) return CORE_E_ARG;
#endif
        if (!view_click_enabled(c, idv->s, idv->len, id == EV_MENU))   /* a control that is off sends nothing */
            core_log(c->life, id == EV_MENU ? "core: a menu item that is off is ignored" : "core: a button that is off is ignored");
        else if (!flows_event(c, idv->s, idv->len))               /* the action flows (M6) */
            view_event(c, idv->s, idv->len, id == EV_MENU);       /* the tabs and views (M4) */
        return CORE_OK;
    }
    case EV_SHOWN:
        if (!c->fl.shown_done) view_tab_shown(c, (int)model_get_i64(&c->m, F_TAB));   /* the first tab appears */
        flows_shown(c);
        return CORE_OK;
    case EV_REOPENED:
    case EV_CHECK_REQUEST:
#ifdef CORE_WIN_POLICY
        if (id == EV_CHECK_REQUEST) view_r1_check(c);   /* the watcher's notification: R1 again */
#endif
        flows_check_request(c);
        return CORE_OK;
    case EV_LOCATION:
        view_location_changed(c);
        return CORE_OK;
    case EV_MOUNT:   /* LocationMonitor's volume observers and its bridge-state watch: update() */
    case EV_UNMOUNT:
        if (!member(ev, "path", J_STR)) return CORE_E_ARG;
        view_location_measure(c);
        return CORE_OK;
    case EV_RENAME:
        if (!member(ev, "path", J_STR) || !member(ev, "name", J_STR)) return CORE_E_ARG;
        view_location_measure(c);
        return CORE_OK;
    case EV_BRIDGE_STATE:
        view_location_measure(c);
        return CORE_OK;
#ifdef CORE_WIN_POLICY
    case EV_REKORDBOX_CHANGED:
        view_refresh(c);
        view_r1_check(c);
        flows_win_inventory_changed(c);
        return CORE_OK;
    case EV_FACTS_WARM:   /* the cheap facts read again, now warm: no re-check, no request */
        view_refresh(c);
        return CORE_OK;
    case EV_R1_DISMISS: {
        const jval *v = member(ev, "versions", J_STR);
        if (!v) return CORE_E_ARG;
        if (!model_get_i64(&c->m, F_WIN_R1_DISMISS_ENABLED)) core_log(c->life, "core: a button that is off is ignored");
        else flows_win_r1_dismiss(c, v->s, v->len);
        return CORE_OK;
    }
#endif
    case EV_SETTINGS:
        return member(ev, "key", J_STR) && member(ev, "value", J_STR) ? CORE_OK : CORE_E_ARG;
    case EV_UNTIL: {
        const jval *d = member(ev, "date", J_STR);
        return d ? prep_until(c, d->s) : CORE_E_ARG;
    }
    default:   /* deactivated, watch: nothing in the app today */
        return CORE_OK;
    }
}

int32_t core_event(core *c, const char *json, size_t len) {
    if (!c || !json) return CORE_E_ARG;
    int e = main_entry(c, 0);
    if (e) return e;
    if (len == CORE_NTS) len = strlen(json);
    c->entry++;
    jval ev;
    int r = json_parse(&c->scratch, json, len, &ev);
    const jval *name = r ? NULL : json_get(&ev, "ev");
    if (!r && (ev.type != J_OBJ || !name || name->type != J_STR)) r = CORE_E_PARSE;
    int id = r ? -1 : core_event_lookup(name->s, name->len);
    if (!r && id < 0) r = CORE_E_UNKNOWN;
    for (size_t i = 0; !r && i < ev.n; i++)
        if (strcmp(ev.keys[i], "ev") && !key_listed(core_event_meta[id].keys, ev.keys[i], ev.key_lens[i])) r = CORE_E_UNKNOWN;
    if (!r) {
        if (c->role == CORE_ROLE_APP) r = handle_app_event(c, id, &ev);
        /* the watcher's start: one start of the watcher, its rounds as a flow (Watcher.swift's
         * runWatcher); {"ev":"watch"} (a resident watcher's notification) isn't the Mac's */
        else if (id == EV_START) r = flows_watch_start(c);
    }
    if (r) core_log(c->life, r == CORE_E_PARSE ? "core: a malformed event is ignored" :
                             r == CORE_E_UNKNOWN ? "core: an unknown event is ignored" : "core: an event with a bad member is ignored");
    leave(c);
    return r;
}

/* ---------------------------------------------------------------- close and quit (D2) */

int32_t core_close_asked(core *c, core_close what) {
    if (!c || (what != CORE_CLOSE_WINDOW && what != CORE_CLOSE_QUIT)) return CORE_CLOSE_REFUSE;
    if (main_entry(c, 1)) return CORE_CLOSE_REFUSE;
    if (c->close_approved[what]) {   /* the re-ask after close_reply(what, 1): allowed anywhere */
        c->close_approved[what] = 0;
        return CORE_CLOSE_NOW;
    }
    if (c->cb_depth > 0) {
        CORE_VIOLATION("core_close_asked inside a host callback");
        warn_nested(c, "core: a call from inside a host callback was refused");
        return CORE_CLOSE_REFUSE;
    }
    if (c->close_pending[what]) return CORE_CLOSE_LATER;   /* the one reply already coming */
    /* windowShouldClose: not while an action runs (beside a pending quit, the quit decides, D2) */
    if (what == CORE_CLOSE_WINDOW && c->action_running && !c->close_pending[CORE_CLOSE_QUIT]) return CORE_CLOSE_REFUSE;
    if (!c->preparing && !c->quit_flow_running) return CORE_CLOSE_NOW;
    c->entry++;
    int r = CORE_CLOSE_LATER;
    c->close_pending[what] = 1;   /* QUIT beside a pending WINDOW: one decision answers both */
    if (!c->quit_flow_running && quit_flow_start(c)) {
        c->close_pending[what] = 0;
        r = CORE_CLOSE_REFUSE;   /* couldn't ask: stay, as a Cancel would */
    }
    leave(c);
    return r;
}

/* ---------------------------------------------------------------- helper runs (D3) */

typedef struct deadline_ctx deadline_ctx;
typedef struct {
    uint64_t run; int ended; int32_t status;
    hrun *h;              /* the end job of an M4 run: its accumulator, freed when the job is dropped */
    plat_timer *dl;       /* ... and its deadline, cancelled at its end (dl_ctx: the timer's) */
    deadline_ctx *dl_ctx;
#ifdef CORE_TEST_HOOKS
    int32_t stub_op;      /* the G1 spike's stub parse (core_test_helper_stub): STUB_* */
    uint32_t stub_len;
    char stub_text[];     /* NUL-terminated */
#endif
} helper_msg;

#ifdef CORE_TEST_HOOKS
static void stub_apply(core *c, int32_t purpose, const helper_msg *m);
#endif

/* A run id carries its slot in the low 5 bits (E2), so core_helper_ended reaches the slot's
 * reserved end job without scanning slots main writes. Ids stay opaque to the host. */
_Static_assert(RUNS_MAX == 32, "a run id keeps its slot in 5 bits");
#define RUN_SLOT(run) ((int)((run) & 31u))

static run_slot *run_of(core *c, uint64_t run) {
    run_slot *s = &c->runs[RUN_SLOT(run)];
    return run && s->id == run ? s : NULL;
}

/* ---- M4's runs: the accumulator and its handlers */

hrun *hrun_new(int kind) {
    hrun *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->kind = kind;
    h->status = -1;
    /* runPrepareHelper's timeouts (PrepareView.swift): --playlists and --tracks 60 s, an estimate
     * 300 s (the Status tab's checks set their 120 s); a run (PrepareRun) has none */
    h->deadline_ms = kind == HK_PLAYLISTS || kind == HK_TRACKS ? 60000 : kind == HK_ESTIMATE ? 300000 : 0;
    prep_estimate_init(&h->e);
    return h;
}

void hrun_free(hrun *h) {
    if (!h) return;
    arena_free(&h->a);
    arena_free(&h->la);
    free(h->pls);
    free(h->files);
    free(h->err);
    if (h->ctx_free) h->ctx_free(h->ctx);
    else free(h->ctx);
    free(h);
}

/* today's per-operation deadlines (runPrepareHelper's `g.wait(timeout:)`, then p.terminate()): a
 * plat.h timer posts to main, which stops the run's helper if it still goes. The timer holds a life
 * reference until it has fired or been cancelled; it is cancelled with the run's end job. */
struct deadline_ctx { core_life *life; uint64_t run; };

static void deadline_job(core *c, void *payload) {
    uint64_t run = *(uint64_t *)payload;
    core_life *l = c->life;
    if (run_of(c, run) && l->host.helper_signal && !(l->native & NATIVE_HELPER))
        CALLBACK(c, l->host.helper_signal(l->host.ctx, run, CORE_HELPER_STOP));
}

static void deadline_fired(void *ctx) {   /* the timer's thread (plat.h: its maintenance queue) */
    deadline_ctx *d = ctx;
    uint64_t *run = malloc(sizeof *run);
    if (run) {
        *run = d->run;
        core_post_main(d->life, deadline_job, run, NULL, 0, 0);   /* after core_free: dropped */
    }
    life_release(d->life);
    free(d);
}

static void deadline_cancel(helper_msg *m) {   /* any thread */
    if (m->dl && plat_timer_cancel(m->dl)) {   /* it never fires: its context is ours to free */
        life_release(m->dl_ctx->life);
        free(m->dl_ctx);
    }
    m->dl = NULL;
    m->dl_ctx = NULL;
}

static void deadline_arm(core *c, helper_msg *m, uint64_t run, int32_t ms) {
#ifdef CORE_TEST_HOOKS
    if (c->ndeadline_seen < 64) {
        c->deadline_seen[c->ndeadline_seen].run = run;
        c->deadline_seen[c->ndeadline_seen++].ms = ms;
    }
    if (ms > 0 && c->deadline_test_ms > 0) ms = c->deadline_test_ms;
#endif
    deadline_ctx *d = ms > 0 ? malloc(sizeof *d) : NULL;
    if (!d) return;   /* out of memory: no deadline, as a run without one */
    d->life = life_retain(c->life);
    d->run = run;
    if (!(m->dl = plat_timer_new(NULL, (uint64_t)ms * 1000000u, deadline_fired, d))) {
        life_release(d->life);
        free(d);
        return;
    }
    m->dl_ctx = d;
}

static void end_msg_drop(void *payload) {   /* the core is gone: the run's accumulator goes with its end job */
    helper_msg *m = payload;
    deadline_cancel(m);
    hrun_free(m->h);
    m->h = NULL;
}

/* a parsed line for main: one block, its strings after it */
enum { HL_TRACK = 1, HL_RUN = 2 };
typedef struct { uint64_t run; int kind; prep_run_line l; char strs[]; } hline_msg;

static size_t pstr_size(pstr p) { return p.n + 1; }
static pstr pstr_put(char **w, pstr p) {
    pstr o = {*w, p.n};
    if (p.n) memcpy(*w, p.s, p.n);
    (*w)[p.n] = 0;
    *w += p.n + 1;
    return o;
}

size_t view_track_size(const prep_track *t) {
    return pstr_size(t->file) + pstr_size(t->result) + pstr_size(t->why) + pstr_size(t->access) + pstr_size(t->folder);
}

void view_track_put(prep_track *dst, const prep_track *src, char *w) {
    *dst = *src;
    dst->file = pstr_put(&w, src->file);
    dst->result = pstr_put(&w, src->result);
    dst->why = pstr_put(&w, src->why);
    dst->access = pstr_put(&w, src->access);
    dst->folder = pstr_put(&w, src->folder);
}

static hline_msg *hline_new(uint64_t run, int kind, const prep_run_line *l) {
    size_t n = view_track_size(&l->track) + pstr_size(l->stopped) + pstr_size(l->error);
    hline_msg *m = malloc(sizeof *m + n);
    if (!m) return NULL;
    m->run = run;
    m->kind = kind;
    m->l = *l;
    char *w = m->strs;
    view_track_put(&m->l.track, &l->track, w);
    w += view_track_size(&l->track);
    m->l.stopped = pstr_put(&w, l->stopped);
    m->l.error = pstr_put(&w, l->error);
    return m;
}

static void hline_job(core *c, void *payload) {
    hline_msg *m = payload;
    run_slot *s = run_of(c, m->run);
    if (!s || !s->h) return;
    if (m->kind == HL_TRACK) {
        if (s->h->on_track) s->h->on_track(c, s->h, &m->l.track);
    } else if (s->h->on_line) s->h->on_line(c, s->h, &m->l);
}

/* the reader thread, for run `run` of h: the line parsed by M5's parser, kept or posted to main */
static void hrun_line(core *c, uint64_t run, hrun *h, const char *line, size_t len) {
    arena_reset(&h->la);
    prep_run_line l;
    int r;
    switch (h->kind) {
    case HK_PLAYLISTS: {   /* parsePlaylists */
        prep_playlist p;
        if ((r = prep_playlist_parse(&h->la, line, len, &p)) <= 0) break;
        if (h->npl == h->cappl) {
            int32_t cap = h->cappl ? h->cappl * 2 : 64;
            prep_playlist *v = cap <= (1 << 24) ? realloc(h->pls, (size_t)cap * sizeof *v) : NULL;
            if (!v) { r = -1; break; }
            h->pls = v;
            h->cappl = cap;
        }
        prep_playlist *d = &h->pls[h->npl];
        d->tracks = p.tracks;
        if (!(d->id.s = arena_strdup(&h->a, p.id.s, p.id.n)) || !(d->parent.s = arena_strdup(&h->a, p.parent.s, p.parent.n)) ||
            !(d->name.s = arena_strdup(&h->a, p.name.s, p.name.n)) || !(d->kind.s = arena_strdup(&h->a, p.kind.s, p.kind.n))) {
            r = -1;
            break;
        }
        d->id.n = p.id.n;
        d->parent.n = p.parent.n;
        d->name.n = p.name.n;
        d->kind.n = p.kind.n;
        h->npl++;
        break;
    }
    case HK_TRACKS: {   /* parseTrackList */
        pstr f;
        if ((r = prep_track_list_parse(&h->la, line, len, &f)) <= 0) break;
        if (h->nfiles == h->capfiles) {
            int32_t cap = h->capfiles ? h->capfiles * 2 : 256;
            pstr *v = cap <= (1 << 24) ? realloc(h->files, (size_t)cap * sizeof *v) : NULL;
            if (!v) { r = -1; break; }
            h->files = v;
            h->capfiles = cap;
        }
        if (!(h->files[h->nfiles].s = arena_strdup(&h->a, f.s, f.n))) { r = -1; break; }
        h->files[h->nfiles++].n = f.n;
        break;
    }
    case HK_ESTIMATE:   /* parseEstimate, and each track's line as it comes (runPrepareHelper's track:) */
        if (prep_estimate_line(&h->a, &h->e, line, len) < 0) h->oom = 1;
        if ((r = prep_run_line_parse(&h->la, line, len, &l)) > 0 && l.kind == RUN_TRACK) {
            hline_msg *m = hline_new(run, HL_TRACK, &l);
            if (m) core_post_main(c->life, hline_job, m, NULL, h->domain, h->gen);
            else r = -1;
        }
        break;
    case HK_RUN:   /* PrepareRun: each line to main as it comes */
        if ((r = prep_run_line_parse(&h->la, line, len, &l)) > 0) {
            hline_msg *m = hline_new(run, HL_RUN, &l);
            if (m) core_post_main(c->life, hline_job, m, NULL, h->domain, h->gen);
            else r = -1;
        }
        break;
    default: r = 0; break;
    }
    if (r < 0) h->oom = 1;   /* a line lost to memory: an update lost, never a state */
}

/* String.trimmingCharacters(in: .whitespacesAndNewlines), for ASCII whitespace */
static char *trimmed_dup(const char *s, size_t n) {
    if (!s) n = 0;
    while (n && strchr(" \t\n\r\v\f", s[0]) && s[0]) { s++; n--; }
    while (n && strchr(" \t\n\r\v\f", s[n - 1]) && s[n - 1]) n--;
    char *d = malloc(n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static void helper_job(core *c, void *payload) {
    helper_msg *m = payload;
    if (m->ended) deadline_cancel(m);
    run_slot *s = run_of(c, m->run);
    CORE_CHECK(s != NULL);
    if (!s) return;
    if (m->ended) {   /* the run's reserved end job: the slot retires */
        if (s->purpose >= 0 && c->current_run[s->purpose] == s->id) c->current_run[s->purpose] = 0;
        s->id = 0;
        s->end_job = NULL;
        s->h = NULL;
        hrun *h = m->h;
        m->h = NULL;
        if (h) {   /* an M4 run: its handler, whatever its gen (it decides), then its accumulator goes */
            h->status = m->status;
            if (h->on_end) h->on_end(c, h);
            hrun_free(h);
        }
        return;
    }
    if (s->purpose < 0 || c->current_run[s->purpose] != s->id) return;   /* a run of this purpose that is no longer current */
    c->run_lines[s->purpose]++;
#ifdef CORE_TEST_HOOKS
    if (m->stub_op) stub_apply(c, s->purpose, m);
#endif
}

/* a run that couldn't start: its end, later on main (as runPrepareHelper's (-1, "", err) comes back) */
typedef struct { hrun *h; } hrun_ref;
static void not_started_job(core *c, void *payload) {
    hrun_ref *r = payload;
    if (r->h->on_end) r->h->on_end(c, r->h);
    hrun_free(r->h);
    r->h = NULL;
}
static void hrun_ref_drop(void *payload) {
    hrun_ref *r = payload;
    hrun_free(r->h);
    r->h = NULL;
}

/* main (the host's helper_spawn may be called from any thread). The end job is reserved before
 * the spawn, so the run's end can't be lost to an allocation failure (E2). h NULL: a run of
 * `purpose` (the test hooks, the G1 spike); else an M4 run (purpose -1). */
static uint64_t start_run(core *c, int32_t purpose, hrun *h, const char *const *argv, int32_t argc) {
    core_life *l = c->life;
    if ((!h && (purpose < 0 || purpose >= PURPOSES)) || !l->host.helper_spawn || (l->native & NATIVE_HELPER)) return 0;
    int idx = 0;
    while (idx < RUNS_MAX && c->runs[idx].id) idx++;
    if (idx == RUNS_MAX) return 0;
    uint64_t run = (++c->next_run << 5) | (uint64_t)idx;
    helper_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->run = run;
    m->ended = 1;
    m->h = h;
    core_job *end = job_reserve(l, helper_job, m, end_msg_drop);
    if (!end) {
        free(m);
        return 0;
    }
    run_slot *s = &c->runs[idx];
    s->id = run;
    s->purpose = h ? -1 : purpose;
    s->h = h;
    s->end_job = end;   /* written before the spawn starts the reader; main leaves it alone until the end job runs */
    if (!h) c->current_run[purpose] = run;
    if (h) deadline_arm(c, m, run, h->deadline_ms);   /* before the spawn: the reader never sees m change */
    life_retain(l);     /* held until the run's core_helper_ended */
    int32_t r;
    CALLBACK(c, r = l->host.helper_spawn(l->host.ctx, run, argv, argc));
    if (r) {   /* not started: no core_helper_ended follows */
        s->id = 0;
        s->end_job = NULL;
        s->h = NULL;
        if (!h) c->current_run[purpose] = 0;
        m->h = NULL;   /* the caller's h: not freed with the end job */
        core_job_drop(end);
        life_release(l);
        return 0;
    }
    return run;
}

uint64_t helper_start(core *c, int32_t purpose, const char *const *argv, int32_t argc) {
    return start_run(c, purpose, NULL, argv, argc);
}

uint64_t helper_start_run(core *c, hrun *h, const char *const *argv, int32_t argc) {
    uint64_t run = start_run(c, -1, h, argv, argc);
    if (run) return run;
    /* not started: the end comes later on main with status -1 (HK_RUN: PrepareRun.start's false is
     * the caller's, so no end) */
    h->status = -1;
    hrun_ref *r = h->kind == HK_RUN ? NULL : malloc(sizeof *r);
    if (!r || !(h->err = calloc(1, 1))) {
        free(r);
        hrun_free(h);
        return 0;
    }
    r->h = h;
    core_post_main(c->life, not_started_job, r, hrun_ref_drop, 0, 0);   /* a failed post drops it: h freed */
    return 0;
}

#ifdef CORE_TEST_HOOKS
/* The G1 API spike's stub parse (API-V1-NOTES open point 12: "a stub parse that sets a cell"; the
 * real parser is M5's, and P15 times it apart). On the reader thread, pure: a helper line becomes
 * one typed op. The fake helper's lines (tests/harness/fake-prepare.c), local files only:
 *   {"file":"<path>"}                    a track of the list: STUB_APPEND, the file's name
 *   {"file":"<path>","result":"<r>",...} an estimate line: STUB_SET, the next row's result
 *   anything else                        counted only, as without the stub
 * It is not M5's parser and never becomes a release path (the review's api_additions ruling):
 * once M5's helper-line parser reads helper output, the stub calls it, or goes after G1.7. */
enum { STUB_NONE = 0, STUB_APPEND = 1, STUB_SET = 2 };
enum { STUB_ESTIMATE = 2 };   /* the purpose of the spike's estimate runs (app/Spike) */

/* main, on a Prepare outline select (EV_SELECT) while the stub is on (review Q32): the estimate
 * run shown ends there. It is no longer current, so its later result lines are dropped instead of
 * landing in the next list's rows, and its helper is told to stop. */
static void stub_select(core *c) {
    core_life *l = c->life;
    uint64_t run = c->current_run[STUB_ESTIMATE];
    if (!atomic_load_explicit(&l->stub_parse, memory_order_relaxed) || !run) return;
    c->current_run[STUB_ESTIMATE] = 0;
    if (l->host.helper_signal) CALLBACK(c, l->host.helper_signal(l->host.ctx, run, CORE_HELPER_STOP));
}

static helper_msg *stub_msg(int32_t op, const char *text, size_t n) {
    helper_msg *m = calloc(1, sizeof *m + n + 1);
    if (!m) return NULL;
    m->stub_op = op;
    m->stub_len = (uint32_t)n;
    if (n) memcpy(m->stub_text, text, n);
    m->stub_text[n] = 0;
    return m;
}

static const char *stub_find(const char *p, size_t n, const char *what) {
    size_t w = strlen(what);
    for (size_t i = 0; i + w <= n; i++)
        if (!memcmp(p + i, what, w)) return p + i + w;
    return NULL;
}

static helper_msg *stub_parse(const char *line, size_t len) {
    static const char file_key[] = "{\"file\":\"";
    size_t k = sizeof file_key - 1;
    if (len < k || memcmp(line, file_key, k)) return stub_msg(STUB_NONE, NULL, 0);
    const char *path = line + k, *end = memchr(path, '"', len - k);
    if (!end) return stub_msg(STUB_NONE, NULL, 0);
    const char *rest = end + 1, *r = stub_find(rest, (size_t)(line + len - rest), "\"result\":\"");
    if (!r) {   /* a track: its name, the path's last part without the extension */
        const char *base = path;
        for (const char *q = path; q < end; q++)
            if (*q == '/') base = q + 1;
        const char *dot = end;
        for (const char *q = base; q < end; q++)
            if (*q == '.') dot = q;
        return stub_msg(STUB_APPEND, base, (size_t)(dot - base));
    }
    const char *re = memchr(r, '"', (size_t)(line + len - r));
    if (!re) return stub_msg(STUB_NONE, NULL, 0);
    size_t rn = (size_t)(re - r);
    const char *label = rn == 5 && !memcmp(r, "ready", 5) ? "Ready" : rn == 7 && !memcmp(r, "prepare", 7) ? "To prepare" : NULL;
    return label ? stub_msg(STUB_SET, label, strlen(label)) : stub_msg(STUB_SET, r, rn);
}

/* main, inside the run's line job, the run current for its purpose */
static void stub_apply(core *c, int32_t purpose, const helper_msg *m) {
    const table_obj *t = c->m.tab[T_PREP_TRACKS];
    if (m->stub_op == STUB_APPEND) {
        int32_t row = t->n;
        if (model_table_append(&c->m, T_PREP_TRACKS, 1)) return;
        model_cell_str(&c->m, C_TRK_NAME, row, m->stub_text, m->stub_len);
        model_cell_str(&c->m, C_TRK_RESULT, row, "Checking\xe2\x80\xa6", CORE_NTS);
    } else if (m->stub_op == STUB_SET) {
        int32_t row = c->stub_row[purpose]++;
        if (row >= t->n) return;
        model_cell_str(&c->m, C_TRK_RESULT, row, m->stub_text, m->stub_len);
    } else {
        return;
    }
    core_mark_changed(c);
}
#endif

void core_helper_line(core *c, uint64_t run, const char *line, size_t len) {
    if (!c || (!line && len) || !life_alive(c->life)) return;   /* after core_free: nothing */
    if (len == CORE_NTS) len = strlen(line);
    /* an M4 run: parsed here, on the reader thread, by M5's parser (untrusted: the helper runs
     * Pioneer's code; invalid UTF-8 is repaired as String(decoding:) repairs it). The slot was
     * written on main before the spawn and is left alone until the run's end job. */
    const run_slot *rs = &c->runs[RUN_SLOT(run)];
    hrun *h = run && rs->id == run ? rs->h : NULL;
    if (h) {
        core_life *l = c->life;
        int framed = core_on_main(c);   /* on main, inside an entry frame (E3) */
        if (framed) {
            life_retain(l);
            c->entry++;
        }
        hrun_line(c, run, h, line, len);
        if (framed) {
            core_leave(c);
            life_release(l);
        }
        return;
    }
    if (!utf8_valid(line, len)) return;
    helper_msg *m;
#ifdef CORE_TEST_HOOKS
    if (atomic_load_explicit(&c->life->stub_parse, memory_order_relaxed)) m = stub_parse(line, len);
    else
#endif
    m = calloc(1, sizeof *m);
    if (!m) return;   /* a lost line loses an update, not a state: not reserved */
    m->run = run;
    core_life *l = c->life;
    int framed = core_on_main(c);   /* on main, inside an entry frame (E3) */
    if (framed) {
        life_retain(l);
        c->entry++;
    }
    core_post_main(l, helper_job, m, NULL, 0, 0);
    if (framed) {
        core_leave(c);
        life_release(l);
    }
}

void core_helper_ended(core *c, uint64_t run, int32_t status, const char *err, size_t err_len) {
    if (!c) return;
    core_life *l = c->life;
    core_job *end = c->runs[RUN_SLOT(run)].end_job;
    CORE_CHECK(end != NULL);
    int framed = core_on_main(c) && life_alive(l);
    if (framed) {
        life_retain(l);
        c->entry++;
    }
    if (end) {   /* posted even after core_free: core_run then drops it */
        helper_msg *em = end->payload;
        em->status = status;
        if (em->h) {   /* an M4 run: stderr's tail, trimmed (runPrepareHelper's err; NULL: "" when lost) */
            if (err_len == CORE_NTS) err_len = err ? strlen(err) : 0;
            em->h->err = trimmed_dup(err, err_len);
        }
        job_post(l, end);
    }
    if (framed) core_leave(c);
    life_release(l);   /* the run's reference, after the frame; c may be gone after this */
    if (framed) life_release(l);
}

typedef struct { int32_t stage; int64_t done, total; } progress_msg;

static void progress_job(core *c, void *payload) {
    progress_msg *m = payload;
    flows_progress(c, m->stage, m->done, m->total);   /* the stage and its numbers become the status line and the spinner */
}

void core_flow_progress(core *c, core_progress stage, int64_t done, int64_t total) {
    if (!c || !life_alive(c->life)) return;
    if (core_on_main(c)) {   /* worker-only: on main it posts nothing (E3) */
        CORE_VIOLATION("core_flow_progress on main");
        return;
    }
    progress_msg *m = malloc(sizeof *m);
    if (!m) return;
    *m = (progress_msg){(int32_t)stage, done, total};
    core_post_main(c->life, progress_job, m, NULL, 0, 0);
}

/* ---------------------------------------------------------------- menus: live state */

uint32_t core_menu_state(core *c, core_menu_id id, const char **title, size_t *title_len) {
    if (title) *title = "";
    if (title_len) *title_len = 0;
    if (!c || main_entry(c, 1) || (unsigned)id >= (unsigned)CORE_MENU_COUNT) return 0;
    /* main.swift:71-91's rules (menu_rule) over the view model's state now */
    menu_facts mf;
    view_menu_facts(c, &mf);
    int32_t t = -1;
    uint32_t st = menu_rule(id, &mf, &t);
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN && id == M_APP && t == T_APP_NAME) t = T_WIN_MENU_APP;   /* core_menu_at's title, pulled too (review C15) */
#endif
    if (title && t >= 0) {
        size_t n = 0;
        if (core_text(c->os, (core_text_id)t, NULL, 0, c->menu_title, sizeof c->menu_title, &n) >= CORE_OK) {
            *title = c->menu_title;
            if (title_len) *title_len = n;
        }
    }
    return st;
}

/* ---------------------------------------------------------------- fact sheets */

void facts_init(core_facts *f, arena *a) {
    memset(f, 0, sizeof *f);
    f->a = a;
}

uint32_t core_facts_parts(const core_facts *f) { return f ? f->parts : 0; }

static int fact_ok(core_facts *out, core_fact f, int kind) {
    int ok = out && (unsigned)f < (unsigned)CORE_FACT_COUNT && core_fact_meta[f].kind == kind;
    CORE_CHECK(ok);
    return ok;
}

void core_fact_flag(core_facts *out, core_fact f, int32_t v) {
    if (!fact_ok(out, f, K_FLAG)) return;
    out->i[f] = v != 0;
    out->set[f] = 1;
}

void core_fact_i64(core_facts *out, core_fact f, int64_t v) {
    if (!fact_ok(out, f, K_I64)) return;
    out->i[f] = v;
    out->set[f] = 1;
}

void core_fact_str(core_facts *out, core_fact f, const char *v, size_t len) {
    if (!fact_ok(out, f, K_STR) || (!v && len)) return;
    if (len == CORE_NTS) len = strlen(v);
    const char *p = arena_strdup(out->a, v ? v : "", v ? len : 0);
    if (!p) return;
    out->s[f] = p;
    out->len[f] = len;
    out->set[f] = 1;
}

/* pure: masking (D8) is mask.c's */

/* ---------------------------------------------------------------- test hooks */

#ifdef CORE_TEST_HOOKS
static int hook_entry(core *c) {
    if (!c) return CORE_E_ARG;
    int e = main_entry(c, 0);
    if (e) return e;
    return c->role == CORE_ROLE_APP ? CORE_OK : CORE_E_STATE;
}

static int hooked(core *c, int r) {
    if (r == CORE_OK) core_mark_changed(c);
    return r;
}

/* every hook runs inside an entry frame, as a real core call does (E3) */
#define HOOK(c, call) do { \
    int e_ = hook_entry(c); \
    if (e_) return e_; \
    core_life *l_ = life_retain((c)->life); \
    (c)->entry++; \
    int r_ = hooked(c, call); \
    core_leave(c); \
    life_release(l_); \
    return r_; \
} while (0)

int32_t core_test_set_str(core *c, core_field f, const char *v, size_t len) { HOOK(c, model_set_str(&c->m, f, v, len)); }
int32_t core_test_set_i64(core *c, core_field f, int64_t v) { HOOK(c, model_set_i64(&c->m, f, v)); }
int32_t core_test_set_f64(core *c, core_field f, double v) { HOOK(c, model_set_f64(&c->m, f, v)); }
int32_t core_test_table_reset(core *c, core_table t, int32_t rows) { HOOK(c, model_table_reset(&c->m, t, rows)); }
int32_t core_test_table_append(core *c, core_table t, int32_t rows) { HOOK(c, model_table_append(&c->m, t, rows)); }
int32_t core_test_cell_str(core *c, core_column col, int32_t row, const char *v, size_t len) { HOOK(c, model_cell_str(&c->m, col, row, v, len)); }
int32_t core_test_cell_i64(core *c, core_column col, int32_t row, int64_t v) { HOOK(c, model_cell_i64(&c->m, col, row, v)); }

int32_t core_test_set_preparing(core *c, int32_t preparing) {
    int e = hook_entry(c);
    if (e) return e;
    c->entry++;
    c->preparing = preparing != 0;
    core_leave(c);
    return CORE_OK;
}

int32_t core_test_fail_allocs(core *c, int32_t n) {
    if (!c) return CORE_E_ARG;
    atomic_store(&c->life->fail_allocs, n);
    return CORE_OK;
}

int32_t core_test_fail_alloc_after(core *c, int32_t skip, int32_t count) {
    if (!c) return CORE_E_ARG;
    atomic_store(&c->life->fail_after_more, count > 1 ? count - 1 : 0);
    atomic_store(&c->life->fail_after, skip < 0 ? -1 : skip);
    return CORE_OK;
}

uint64_t core_test_helper_start(core *c, int32_t purpose) {
    if (hook_entry(c)) return 0;
    static const char *const argv[] = {"--test"};
    core_life *l = life_retain(c->life);
    c->entry++;
    if (purpose >= 0 && purpose < PURPOSES) c->stub_row[purpose] = 0;   /* the stub's estimate rows count again */
    uint64_t run = helper_start(c, purpose, argv, 1);
    core_leave(c);
    life_release(l);
    return run;
}

int32_t core_test_set_now(core *c, int64_t unix_seconds) {
    int e = hook_entry(c);
    if (e) return e;
    c->p.test_now = unix_seconds;
    return CORE_OK;
}

int32_t core_test_set_mark(core *c, core_test_mark_fn mark, void *ctx) {
    int e = hook_entry(c);
    if (e) return e;
    c->test_mark = mark;
    c->test_mark_ctx = ctx;
    return CORE_OK;
}

int32_t core_test_helper_stub(core *c, int32_t on) {
    if (!c) return CORE_E_ARG;
    atomic_store(&c->life->stub_parse, on != 0);
    return CORE_OK;
}

int64_t core_test_helper_lines(core *c, int32_t purpose) {
    if (hook_entry(c) || purpose < 0 || purpose >= PURPOSES) return -1;
    return c->run_lines[purpose];
}
#endif
