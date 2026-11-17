/* The Status tab (M4, PrepareView.swift's StatusTab): the prepared playlists (the kept sets), each
 * one's tracks from the library and its check, the run's progress in its row, Prepare Again.
 * Each function is the Swift one of the same name, ported line by line.
 *
 * T_STATUS_SETS's rows are the sets as keptSets() lists them (by name); their cells are what the
 * table's data source returns now, so a check's result, a count or the run's progress rewrites
 * the cells that change. A new list of sets is a new list_gen. The checks are M4's hrun (core.c):
 * a check whose gen moved (StatusTab.gen) is dropped, and the checks under way are stopped when
 * it moves (HelperJob.cancel). */
#include "internal.h"
#include <stdio.h>

/* ---------------------------------------------------------------- a few names (linear) */

static char *dup0(const char *s) {
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static const char *sd_get(const status_dict *d, const char *k) {
    for (int32_t i = 0; i < d->n; i++)
        if (!strcmp(d->a[i].k, k)) return d->a[i].v;
    return NULL;
}

static void sd_del(status_dict *d, const char *k) {
    for (int32_t i = 0; i < d->n; i++)
        if (!strcmp(d->a[i].k, k)) {
            free(d->a[i].k);
            free(d->a[i].v);
            d->a[i] = d->a[--d->n];
            return;
        }
}

static void sd_set(status_dict *d, const char *k, const char *v) {
    for (int32_t i = 0; i < d->n; i++)
        if (!strcmp(d->a[i].k, k)) {
            char *n = dup0(v);
            if (!n) return;
            free(d->a[i].v);
            d->a[i].v = n;
            return;
        }
    if (d->n == d->cap) {
        int32_t cap = d->cap ? d->cap * 2 : 8;
        status_kv *a = realloc(d->a, (size_t)cap * sizeof *a);
        if (!a) return;
        d->a = a;
        d->cap = cap;
    }
    char *nk = dup0(k), *nv = dup0(v);
    if (!nk || !nv) {
        free(nk);
        free(nv);
        return;
    }
    d->a[d->n].k = nk;
    d->a[d->n].v = nv;
    d->n++;
}

static void sd_clear(status_dict *d) {
    for (int32_t i = 0; i < d->n; i++) {
        free(d->a[i].k);
        free(d->a[i].v);
    }
    d->n = 0;
}

/* ---------------------------------------------------------------- the sets */

static void sets_free(status_set *s, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        free(s[i].name);
        free(s[i].until);
    }
    free(s);
}

/* keptSets() as the host read it (FACT_KEPT_SETS): "name\tuntil\tfiles" a line */
static int32_t sets_now(core *c, status_set **out) {
    core_facts f;
    view_read_facts(c, &f, CORE_FACTS_KEPT);   /* keptSets() alone, as StatusView.reload reads it */
    *out = NULL;
    const char *s = f.set[FACT_KEPT_SETS] && f.s[FACT_KEPT_SETS] ? f.s[FACT_KEPT_SETS] : "";
    const char **lines;
    int32_t n = view_split_lines(&c->scratch, s, &lines);
    if (!n) return 0;
    status_set *v = calloc((size_t)n, sizeof *v);
    if (!v) return 0;
    int32_t k = 0;
    for (int32_t i = 0; i < n; i++) {
        const char *t1 = strchr(lines[i], '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2 || t1 == lines[i]) continue;
        size_t nl = (size_t)(t1 - lines[i]), ul = (size_t)(t2 - t1 - 1);
        v[k].name = malloc(nl + 1);
        v[k].until = malloc(ul + 1);
        if (!v[k].name || !v[k].until) {
            free(v[k].name);
            free(v[k].until);
            break;
        }
        memcpy(v[k].name, lines[i], nl);
        v[k].name[nl] = 0;
        memcpy(v[k].until, t1 + 1, ul);
        v[k].until[ul] = 0;
        int64_t files = 0;
        for (const char *p = t2 + 1; *p >= '0' && *p <= '9' && files < (INT64_MAX - 9) / 10; p++) files = files * 10 + (*p - '0');
        v[k].files = files;
        k++;
    }
    *out = v;
    return k;
}

/* tableView(_:viewFor:row:): the set's four cells as the data source gives them now */
static void cells_of(core *c, int32_t row) {
    status_state *st = &c->st;
    const status_set *s = &st->sets[row];
    char buf[2048];
    tbuf b;
    tb_init(&b, c->os, buf, sizeof buf);
    prep_playlist_name(c, s->name, &b);
    model_cell_str(&c->m, C_SET_NAME, row, b.err ? "" : buf, CORE_NTS);
    const char *counted = sd_get(&st->counts, s->name);   /* counts[name] ?? s.files */
    int64_t n = s->files;
    if (counted) {
        n = 0;
        for (const char *p = counted; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
    }
    tb_init(&b, c->os, buf, sizeof buf);
    if (n > 0) tb_i64(&b, n);
    else TB_TEXT0(&b, T_DASH);
    model_cell_str(&c->m, C_SET_TRACKS, row, b.err ? "" : buf, CORE_NTS);
    tb_init(&b, c->os, buf, sizeof buf);
    if (*s->until) tb_puts(&b, s->until);
    else TB_TEXT0(&b, T_DASH);
    model_cell_str(&c->m, C_SET_UNTIL, row, b.err ? "" : buf, CORE_NTS);
    /* a state already known stays while it is checked again: it changes only if the check says otherwise */
    tb_init(&b, c->os, buf, sizeof buf);
    const char *running = prep_running_set(c), *state = sd_get(&st->states, s->name);
    if (running && !strcmp(running, s->name)) prep_run_summary(c, &b);
    else if (state) tb_puts(&b, state);
    else TB_TEXT0(&b, sd_get(&st->checking, s->name) ? T_CHECKING : T_NOT_CHECKED);
    model_cell_str(&c->m, C_SET_STATE, row, b.err ? "" : buf, CORE_NTS);
}

static void cells_all(core *c) {
    for (int32_t i = 0; i < c->st.nsets; i++) cells_of(c, i);
    core_mark_changed(c);
}

static void sets_replace(core *c, status_set *v, int32_t n) {
    status_state *st = &c->st;
    int same = n == st->nsets;
    for (int32_t i = 0; same && i < n; i++) same = !strcmp(v[i].name, st->sets[i].name);
    sets_free(st->sets, st->nsets);
    st->sets = v;
    st->nsets = n;
    if (!same) {
        model_table_reset(&c->m, T_STATUS_SETS, n);
        if (model_get_i64(&c->m, F_STATUS_SELECTED_ROW) >= n) model_set_i64(&c->m, F_STATUS_SELECTED_ROW, -1);
    }
    cells_all(c);
}

/* ---------------------------------------------------------------- the checks */

static void gen_bump(core *c) {   /* gen += 1: the checks under way are stopped (HelperJob.cancel) */
    status_state *st = &c->st;
    c->gen[DOM_SETS]++;
    for (int32_t i = 0; i < st->njobs; i++)
        if (c->life->host.helper_signal && !(c->life->native & NATIVE_HELPER))
            CALLBACK(c, c->life->host.helper_signal(c->life->host.ctx, st->jobs[i], CORE_HELPER_STOP));
    st->njobs = 0;
}

static void job_add(core *c, uint64_t run) {
    status_state *st = &c->st;
    if (!run) return;
    if (st->njobs == st->capjobs) {
        int32_t cap = st->capjobs ? st->capjobs * 2 : 8;
        uint64_t *v = realloc(st->jobs, (size_t)cap * sizeof *v);
        if (!v) return;
        st->jobs = v;
        st->capjobs = cap;
    }
    st->jobs[st->njobs++] = run;
}

static void job_done(core *c, uint64_t run) {
    status_state *st = &c->st;
    for (int32_t i = 0; i < st->njobs; i++)
        if (st->jobs[i] == run) {
            st->jobs[i] = st->jobs[--st->njobs];
            return;
        }
}

typedef struct { uint64_t g, run; char *name; int32_t argc; char **argv; int second; } set_ctx;
static void set_ctx_free(void *p) {
    set_ctx *s = p;
    if (!s) return;
    free(s->name);
    for (int32_t i = 0; i < s->argc; i++) free(s->argv[i]);
    free(s->argv);
    free(s);
}

static set_ctx *ctx_new(uint64_t g, const char *name, const char *const *argv, int32_t argc) {
    set_ctx *s = calloc(1, sizeof *s);
    if (!s || !(s->name = dup0(name)) || (argc && !(s->argv = calloc((size_t)argc, sizeof *s->argv)))) {
        set_ctx_free(s);
        return NULL;
    }
    s->g = g;
    for (int32_t i = 0; i < argc; i++, s->argc++)
        if (!(s->argv[i] = dup0(argv[i]))) {
            set_ctx_free(s);
            return NULL;
        }
    return s;
}

/* the set's --tracks: the number of tracks is the library's, known whatever can't be checked now */
static void count_end(core *c, hrun *h) {
    set_ctx *s = h->ctx;
    if (s->g != c->gen[DOM_SETS]) return;
    if (h->status == 0 && !h->oom) {
        char n[24];
        snprintf(n, sizeof n, "%d", (int)h->nfiles);
        sd_set(&c->st.counts, s->name, n);
    } else sd_del(&c->st.counts, s->name);   /* deleted: what the set holds */
    cells_all(c);
}

static void check_start(core *c, set_ctx *s, int with_playlist);

/* checkSet (Prepare.swift:397): the playlist as it is now, else the set's own files */
static void check_end(core *c, hrun *h) {
    set_ctx *s = h->ctx;
    job_done(c, s->run);
    if (s->g != c->gen[DOM_SETS]) return;
    if (!s->second && !prep_estimate_ok(&h->e)) {   /* the playlist can't be read: the set's own files */
        set_ctx *n = ctx_new(s->g, s->name, (const char *const *)s->argv, s->argc);
        if (n) {
            n->second = 1;
            check_start(c, n, 0);
        }
        return;
    }
    status_state *st = &c->st;
    sd_del(&st->checking, s->name);
    char buf[2048];
    tbuf b;
    tb_init(&b, c->os, buf, sizeof buf);
    w_set_state(&b, !h->oom && prep_estimate_ok(&h->e) ? &h->e : NULL);   /* setState(parseEstimate(r.out)) */
    sd_set(&st->states, s->name, b.err ? "" : buf);
    cells_all(c);
}

static void check_start(core *c, set_ctx *s, int with_playlist) {
    hrun *h = hrun_new(HK_ESTIMATE);
    if (!h) {
        set_ctx_free(s);
        return;
    }
    h->deadline_ms = 120000;   /* checkSet's runPrepareHelper(timeout: 120) */
    h->ctx = s;
    h->ctx_free = set_ctx_free;
    h->on_end = check_end;   /* no track lines: the table shows the set, not its tracks */
    const char *argv[32];
    int32_t k = 0;
    for (int32_t i = 0; i < s->argc && k < 30; i++) argv[k++] = s->argv[i];
    if (with_playlist) {
        argv[k++] = "--playlist";
        argv[k++] = s->name;
    }
    uint64_t run = helper_start_run(c, h, argv, k);   /* 0: h, and s with it, may be freed already */
    if (run) {
        s->run = run;
        job_add(c, run);
    }
}

/* reload() (PrepareView.swift:166): the sets again, their names, and each one's check */
static void status_reload(core *c) {
    status_state *st = &c->st;
    gen_bump(c);
    uint64_t g = c->gen[DOM_SETS];
    sd_clear(&st->checking);
    status_set *v;
    int32_t n = sets_now(c, &v);
    sets_replace(c, v, n);   /* "Checking…" only where no state is known yet (the table's reload at the end) */
    status_update_buttons(c);
    if (n) prep_load_playlists(c);   /* the sets' names, as they are now */
    for (int32_t i = 0; i < n; i++) {   /* the number of tracks: the library's */
        set_ctx *s = ctx_new(g, st->sets[i].name, NULL, 0);
        hrun *h = s ? hrun_new(HK_TRACKS) : NULL;
        if (!h) {
            set_ctx_free(s);
            continue;
        }
        h->ctx = s;
        h->ctx_free = set_ctx_free;
        h->on_end = count_end;
        const char *argv[2] = {"--tracks", st->sets[i].name};
        helper_start_run(c, h, argv, 2);
    }
    const char *base[16];
    int32_t problems;
    int32_t k = prep_base_args_now(c, base, &problems);
    if (problems) {
        char buf[256];
        tbuf b;
        tb_init(&b, c->os, buf, sizeof buf);
        TB_TEXT0(&b, T_SET_CANT_CHECK_NOW);
        for (int32_t i = 0; i < n; i++) sd_set(&st->states, st->sets[i].name, b.err ? "" : buf);
        cells_all(c);
        return;
    }
    char cant[256], couldnt[256];
    {
        tbuf b;
        tb_init(&b, c->os, cant, sizeof cant);
        TB_TEXT0(&b, T_SET_CANT_CHECK_NOW);
        tb_init(&b, c->os, couldnt, sizeof couldnt);
        TB_TEXT0(&b, T_SET_UNCHECKED_ERR);
    }
    const char *running = prep_running_set(c);
    for (int32_t i = 0; i < n; i++) {
        const char *name = st->sets[i].name;
        if (running && !strcmp(running, name)) continue;
        /* a "can't be checked" isn't a state of the set: "Checking…" while it is checked now */
        const char *was = sd_get(&st->states, name);
        if (was && (!strcmp(was, cant) || !strcmp(was, couldnt))) sd_del(&st->states, name);
        sd_set(&st->checking, name, "");
        const char *argv[24];
        int32_t a = 0;
        for (int32_t j = 0; j < k; j++) argv[a++] = base[j];
        argv[a++] = "--estimate";
        argv[a++] = "--keep";
        argv[a++] = name;
        argv[a++] = "--wait-read";
        argv[a++] = "1";
        set_ctx *s = ctx_new(g, name, argv, a);
        if (s) check_start(c, s, 1);
    }
    cells_all(c);
}

/* ---------------------------------------------------------------- the entries */

void status_forget(core *c) {   /* what the checks said isn't so now: they stop, and the next reload checks again */
    gen_bump(c);
    sd_clear(&c->st.checking);
    sd_clear(&c->st.states);
}

void status_reload_if_showing(core *c) {
    if (view_in_front(c, CORE_TAB_STATUS) && !c->action_running) status_reload(c);
}

void status_will_appear(core *c) { status_reload(c); }

/* updateButtons (PrepareView.swift:214) */
void status_update_buttons(core *c) {
    if (c->role != CORE_ROLE_APP) return;
    status_state *st = &c->st;
    int64_t row = model_get_i64(&c->m, F_STATUS_SELECTED_ROW);
    int valid = row >= 0 && row < st->nsets;
    const char *running = prep_running_set(c);
    model_set_i64(&c->m, F_STATUS_AGAIN_ENABLED, valid && !prep_running(c) && !c->action_running);
    /* off during an action (the busy state: an install, an uninstall…), whose flow holds the flow
     * thread so the confirmation can't be asked: a change from 1.3.0, where it stayed on (owner,
     * 2026-11-12) */
    model_set_i64(&c->m, F_STATUS_STOP_ENABLED, valid && !c->action_running && !(running && !strcmp(running, st->sets[row].name)));
    char buf[1024];
    tbuf b;
    tb_init(&b, c->os, buf, sizeof buf);
    prep_run_summary(c, &b);
    model_set_str(&c->m, F_STATUS_RUN_LINE, b.err ? "" : buf, CORE_NTS);
    cells_all(c);   /* the row of the set being prepared says what this line says */
}

void status_names_changed(core *c) { cells_all(c); }

void status_set_forget(core *c, const char *id) { sd_del(&c->st.states, id); }

/* prepareAgain (PrepareView.swift:249): the Prepare tab, the set's playlist chosen */
static void prepare_again(core *c) {
    int64_t row = model_get_i64(&c->m, F_STATUS_SELECTED_ROW);
    if (row < 0 || row >= c->st.nsets) return;
    char *name = dup0(c->st.sets[row].name);
    if (!name) return;
    view_show_tab(c, CORE_TAB_PREPARE);
    prep_select_id(c, name);
    free(name);
}

/* stopKeepingSet (PrepareView.swift:256): its confirmation is a flow's (flows_stop_keeping) */
static void stop_keeping_set(core *c) {
    int64_t row = model_get_i64(&c->m, F_STATUS_SELECTED_ROW);
    if (row < 0 || row >= c->st.nsets) return;
    char buf[1024];
    tbuf b;
    tb_init(&b, c->os, buf, sizeof buf);
    prep_playlist_name(c, c->st.sets[row].name, &b);
    if (b.err || flows_stop_keeping(c, c->st.sets[row].name, buf))
        core_log(c->life, "core: a click while an action runs is ignored");
}

/* the set confirmed and removed (the flow logged it): what was known of it goes, and the sets
 * again (states[s.name] = nil; counts[s.name] = nil; reload()) */
void status_stopped_keeping(core *c, const char *set) {
    sd_del(&c->st.states, set);
    sd_del(&c->st.counts, set);
    status_reload(c);
}

int status_event(core *c, const char *id, size_t n, int menu) {
    (void)menu;
    if (n == 13 && !memcmp(id, "prepare_again", 13)) {   /* the button and the row's double click */
        prepare_again(c);
        return 1;
    }
    if (n == 12 && !memcmp(id, "stop_keeping", 12)) {   /* Stop Keeping…: only when it is on */
        if (model_get_i64(&c->m, F_STATUS_STOP_ENABLED)) stop_keeping_set(c);
        return 1;
    }
    return 0;
}

void status_init(core *c) { memset(&c->st, 0, sizeof c->st); }

void status_free(core *c) {
    status_state *st = &c->st;
    sets_free(st->sets, st->nsets);
    sd_clear(&st->states);
    sd_clear(&st->checking);
    sd_clear(&st->counts);
    free(st->states.a);
    free(st->checking.a);
    free(st->counts.a);
    free(st->jobs);
    memset(st, 0, sizeof *st);
}
