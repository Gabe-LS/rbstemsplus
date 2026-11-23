/* The Prepare tab (M4, PrepareView.swift's PrepareTab): the playlists from rekordbox's library as
 * an outline, the chosen playlist's tracks and estimate, the quiet re-checks, the held click, the
 * run. Each function is the Swift one of the same name, ported line by line; where the core does
 * something AppKit did (a table's reloadData, a selection, a scroll), the comment says so.
 *
 * Rows and deltas (mac-core §3.4): the outline is T_PREP_OUTLINE, every playlist of the tree in
 * pre-order (its parent's row, its depth, whether a folder is shown open); a new list of
 * playlists is a new list_gen. The tracks are T_PREP_TRACKS; their cells are what the table's
 * data source returns now (the name, the Result column, the Note column), so a state that changes
 * every row's text ("Checking…" against "Not checked", the row being prepared) rewrites the cells
 * that change, and one track's line changes its own row. A new set of rows is a new list_gen;
 * rows that stay, and rows appended, keep it.
 *
 * The helper's runs are M4's hrun (core.c): parsed on the reader thread, their results on main
 * here. A run whose gen moved is dropped (today's `guard g == self.gen`), and the estimate under
 * way is stopped when the gen moves (HelperJob.cancel). */
#include "internal.h"
#include <stdio.h>
#include <time.h>

/* ---------------------------------------------------------------- small things */

static char *dupn(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static void set_owned(char **slot, const char *s) {
    char *n = s ? dupn(s, strlen(s)) : NULL;
    if (s && !n) return;   /* out of memory: the old text stays */
    free(*slot);
    *slot = n;
}

static int pstr_eq(pstr a, pstr b) { return a.n == b.n && (!a.n || !memcmp(a.s, b.s, a.n)); }
static int pstr_is_s(pstr a, const char *s) { return a.n == strlen(s) && !memcmp(a.s, s, a.n); }
static int str_eq0(const char *a, const char *b) { return a && b && !strcmp(a, b); }

/* a text into the scratch; "" when it couldn't be built */
static const char *tx(core *c, void (*fill)(tbuf *, const void *), const void *arg) {
    char *buf = arena_alloc(&c->scratch, 8192);
    if (!buf) return "";
    tbuf b;
    tb_init(&b, c->os, buf, 8192);
    fill(&b, arg);
    return b.err ? "" : buf;
}
typedef struct { core_text_id id; const core_slot *sl; int32_t n; } tx_args;
static void fill_text(tbuf *b, const void *a) {
    const tx_args *t = a;
    tb_text(b, t->id, t->sl, t->n);
}
static const char *text0(core *c, core_text_id id) { return tx(c, fill_text, &(tx_args){id, NULL, 0}); }

/* String(decoding:as:): the helper's stderr with invalid UTF-8 repaired, in the scratch */
static const char *repaired(core *c, const char *s) {
    size_t n;
    return utf8_repair(&c->scratch, s, strlen(s), &n);
}

static int64_t now_s(core *c) { return c->p.test_now ? c->p.test_now : (int64_t)time(NULL); }

static int local_tm(int64_t t, struct tm *out) {
    time_t tt = (time_t)t;
#ifdef _WIN32
    return localtime_s(out, &tt) == 0;
#else
    return localtime_r(&tt, out) != NULL;
#endif
}

/* ---------------------------------------------------------------- the state's pieces */

static void pl_clear(prep_pl *p) {
    free(p->id);
    free(p->parent);
    free(p->name);
    free(p->kind);
    memset(p, 0, sizeof *p);
}

static int pl_copy(prep_pl *d, const prep_pl *s) {
    memset(d, 0, sizeof *d);
    d->tracks = s->tracks;
    if (!(d->id = dupn(s->id, strlen(s->id))) || !(d->parent = dupn(s->parent, strlen(s->parent))) ||
        !(d->name = dupn(s->name, strlen(s->name))) || !(d->kind = dupn(s->kind, strlen(s->kind)))) {
        pl_clear(d);
        return -1;
    }
    return 0;
}

static int pl_eq(const prep_pl *a, const prep_pl *b) {
    return !strcmp(a->id, b->id) && !strcmp(a->parent, b->parent) && !strcmp(a->name, b->name) && !strcmp(a->kind, b->kind) &&
           a->tracks == b->tracks;
}

static int can_prepare(const prep_pl *p) { return !strcmp(p->kind, "playlist"); }   /* Playlist.canPrepare */

static void est_free(prep_est *e) {
    if (!e) return;
    arena_free(&e->a);
    free(e);
}

static int track_eq(const prep_track *a, const prep_track *b);

/* Estimate's ==: every field the helper said, the tracks and the folders */
static int est_eq(const prep_estimate *a, const prep_estimate *b) {
    if (a->has_refused != b->has_refused || !pstr_eq(a->refused, b->refused) || a->ready != b->ready || a->prepare != b->prepare ||
        a->skipped != b->skipped || a->unchecked != b->unchecked || a->asking != b->asking || a->fit_files != b->fit_files ||
        a->time_s != b->time_s || a->need_gb != b->need_gb || a->kept_gb != b->kept_gb || a->share_gb != b->share_gb ||
        a->max_gb != b->max_gb || a->max_gb_needed != b->max_gb_needed || a->fits != b->fits || a->disk_ok != b->disk_ok ||
        a->ntracks != b->ntracks || a->nfolders != b->nfolders)
        return 0;
    for (int32_t i = 0; i < a->ntracks; i++)
        if (!track_eq(&a->tracks[i], &b->tracks[i])) return 0;
    for (int32_t i = 0; i < a->nfolders; i++)
        if (!pstr_eq(a->folders[i], b->folders[i])) return 0;
    return 1;
}

static void set_est(core *c, prep_est *e) {
    /* `estimate == e` (Prepare.swift:244) by value: a dialog's answer is about this estimate only,
     * and the same estimate coming back (an activation, a re-check) is still the one asked about.
     * nil in between counts as a change: stricter than 1.3.0, so the click checks again. */
    if (!(e && c->p.est && est_eq(&e->e, &c->p.est->e))) c->p.est_seq++;
    est_free(c->p.est);
    c->p.est = e;
}

static prep_track *track_dup(const prep_track *t) {
    size_t n = view_track_size(t);
    prep_track *d = malloc(sizeof *d + n);
    if (!d) return NULL;
    view_track_put(d, t, (char *)(d + 1));
    return d;
}

/* TrackLine(file:): a track with nothing said about it */
static prep_track *track_bare(pstr file) {
    prep_track t;
    memset(&t, 0, sizeof t);
    t.file = file;
    t.result = t.why = t.access = t.folder = (pstr){"", 0};
    return track_dup(&t);
}

static int track_eq(const prep_track *a, const prep_track *b) {   /* TrackLine's == */
    return pstr_eq(a->file, b->file) && pstr_eq(a->result, b->result) && pstr_eq(a->why, b->why) && pstr_eq(a->access, b->access) &&
           pstr_eq(a->folder, b->folder) && a->entries == b->entries && a->written == b->written && a->already == b->already &&
           a->missing == b->missing && a->refused == b->refused && a->seconds == b->seconds && a->gb == b->gb;
}

static void lines_free(prep_track **v, int32_t n) {
    for (int32_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* ---------------------------------------------------------------- the facts (prepareFacts, prepareProblems) */

typedef struct {
    prep_facts pf;
    const char *cache_folder;   /* NULL: none usable */
    int chosen_drive;
#ifdef CORE_WIN_POLICY
    int location_unchecked;
#endif
    const char *max_gb_text;    /* configNumber(maxGB) */
    double max_gb;
} prep_now;

static double number_of(core *c, const char *s, double none) {
    jval v;
    if (!s || !*s || json_parse(&c->scratch, s, strlen(s), &v) || v.type != J_NUM) return none;
    return v.num;
}

/* prepareFacts (Prepare.swift:123) for the app (measured); waiter: who hears of the model's sha256 */
/* prepareFacts from a facts sheet. measured 0: the watcher's (it doesn't measure where the stems are
 * saved: config.ini's chosen folder is taken as it is, prepareFacts(measured: false)) */
static void prep_facts_of(core *c, const core_facts *fp, int waiter, int measured, prep_now *n) {
    const core_facts f = *fp;
    memset(n, 0, sizeof *n);
#define FLAG(id) (f.set[id] && f.i[id] != 0)
#define STR(id) (f.set[id] && f.s[id] ? f.s[id] : "")
#define STR0(id) (f.set[id] && f.s[id] ? f.s[id] : NULL)
    n->pf.apple_silicon = FLAG(FACT_APPLE_SILICON);
    n->pf.helper_there = FLAG(FACT_HELPER_THERE);
    n->pf.rekordbox_installed = FLAG(FACT_REKORDBOX_INSTALLED);
    n->pf.rekordbox_version = STR(FACT_REKORDBOX_VERSION);
    n->pf.rekordbox_ok = FLAG(FACT_REKORDBOX_SUPPORTED);
    n->pf.cache_installed = FLAG(FACT_STEMS_CACHE_PRESENT);
    n->pf.bridge_keeps = FLAG(FACT_BRIDGE_KEEPS);
    n->pf.ort_there = FLAG(FACT_ROOT_ORT_EXISTS);
    n->pf.config_unreadable = STR0(FACT_CONFIG_UNREADABLE);
    n->pf.enabled = FLAG(FACT_CONFIG_ENABLED);
    n->max_gb_text = STR(FACT_CONFIG_MAX_GB);
    n->max_gb = number_of(c, n->max_gb_text, 20.0);
    n->pf.plus_on = FLAG(FACT_STEMS_PLUS_PRESENT);
    n->pf.model_sha = view_model_sha(c, &f, waiter);
    const char *tip;
    n->pf.cache_look = view_cache_light(c, &f, SHA_WAIT_NONE, &tip);
    n->pf.cache_tip = tip;
#ifdef CORE_WIN_POLICY
    n->location_unchecked = measured && win_location_unchecked(c->os, &f);
#endif
    n->pf.quality = (int32_t)(f.set[FACT_STEMS_QUALITY] ? f.i[FACT_STEMS_QUALITY] : -1);
    const char **ours;
    n->pf.n_our_models = view_split_lines(&c->scratch, STR0(FACT_OUR_MODELS), &ours);
    n->pf.our_models = ours;
    switch (f.set[FACT_CONFIG_LOCATION] ? f.i[FACT_CONFIG_LOCATION] : 0) {
    case 0: n->cache_folder = c->paths[P_CACHE_DEFAULT] ? c->paths[P_CACHE_DEFAULT] : ""; break;   /* this Mac */
    case 1: {   /* a chosen folder: used once the measurement says it's the folder in use */
        const char *dir = STR(FACT_CONFIG_DIR), *in_use = STR0(FACT_LOC_IN_USE);
#ifdef CORE_WIN_POLICY
        if (n->location_unchecked) in_use = NULL;
#endif
        n->cache_folder = !measured || str_eq0(in_use, dir) ? dir : NULL;
        n->chosen_drive = 1;
        break;
    }
    default: n->cache_folder = NULL; break;   /* malformed */
    }
    n->pf.cache_folder = n->cache_folder != NULL;
    n->pf.programs = CORE_PROGRAMS(c);
#undef FLAG
#undef STR
#undef STR0
}

static void prep_facts_now(core *c, int waiter, prep_now *n) {
    core_facts f;
    view_read_facts(c, &f, CORE_FACTS_REFRESH | CORE_FACTS_PREPARE);   /* prepareFacts: refresh's rows and its own */
    prep_facts_of(c, &f, waiter, 1, n);
}

/* problems = prepareProblems(facts) */
static void set_problems(core *c, const prep_now *n) {
    char *buf = arena_alloc(&c->scratch, 8192);
    if (!buf) return;
    tbuf b;
    tb_init(&b, c->os, buf, 8192);
    int32_t k = w_prepare_problems(&b, &n->pf);
    set_owned(&c->p.problems, k && !b.err ? buf : NULL);
    c->p.nproblems = c->p.problems ? k : 0;
}

static void problems_append(core *c, const char *line) {
    prep_state *p = &c->p;
    size_t a = p->problems ? strlen(p->problems) : 0, k = strlen(line);
    char *n = malloc(a + k + 2);
    if (!n) return;
    if (a) {
        memcpy(n, p->problems, a);
        n[a++] = '\n';
    }
    memcpy(n + a, line, k + 1);
    free(p->problems);
    p->problems = n;
    p->nproblems++;
}

static void problems_set1(core *c, const char *line) {
    set_owned(&c->p.problems, line);
    c->p.nproblems = c->p.problems ? 1 : 0;
}

/* prepareBaseArgs (Prepare.swift:157) into argv (at least 14 slots) */
static int32_t base_args(core *c, const prep_now *n, const char **argv) {
    const char *def = c->paths[P_CACHE_DEFAULT] ? c->paths[P_CACHE_DEFAULT] : "";
    int32_t k = 0;
    argv[k++] = "--model";
    argv[k++] = c->paths[P_MODEL] ? c->paths[P_MODEL] : "";
    argv[k++] = "--cache";
    argv[k++] = n->cache_folder ? n->cache_folder : n->chosen_drive ? "" : def;
    argv[k++] = "--max-gb";
    argv[k++] = n->max_gb_text;
    argv[k++] = "--kept-dir";
    argv[k++] = c->paths[P_KEPT] ? c->paths[P_KEPT] : "";
    argv[k++] = "--ort";
    argv[k++] = c->paths[P_ROOT_ORT] ? c->paths[P_ROOT_ORT] : "";
    argv[k++] = "--rekordbox";
    argv[k++] = c->paths[P_REKORDBOX] ? c->paths[P_REKORDBOX] : "";
    if (n->chosen_drive) argv[k++] = "--chosen";
    return k;
}

int32_t prep_base_args_now(core *c, const char **argv, int32_t *problems) {
    prep_now n;
    prep_facts_now(c, SHA_WAIT_NONE, &n);
    char *buf = arena_alloc(&c->scratch, 8192);
    *problems = 1;
    if (buf) {
        tbuf b;
        tb_init(&b, c->os, buf, 8192);
        *problems = w_prepare_problems(&b, &n.pf);
    }
    return base_args(c, &n, argv);
}

/* The watcher's prepared-playlists check (main, its flow's query): prepareFacts(measured: false)
 * from the WATCH_KEPT step's facts, prepareProblems into problems (one a line, "" none; cap bytes)
 * and prepareBaseArgs into argv (at least 14 slots, pointing into f and the core's paths) */
int32_t prep_watch_args(core *c, const core_facts *f, const char **argv, char *problems, size_t cap) {
    prep_now n;
    prep_facts_of(c, f, SHA_WAIT_NONE, 0, &n);
    tbuf b;
    tb_init(&b, c->os, problems, cap);
    int32_t k = w_prepare_problems(&b, &n.pf);
    if (b.err) snprintf(problems, cap, "%s", k ? "?" : "");
    else if (!k) problems[0] = 0;
    return base_args(c, &n, argv);
}

/* argv copied into the scratch (a run's arguments outlive the caller's locals) */
static const char **argv_keep(core *c, const char *const *v, int32_t n) {
    const char **a = arena_alloc(&c->scratch, (size_t)(n ? n : 1) * sizeof *a);
    if (!a) return NULL;
    for (int32_t i = 0; i < n; i++)
        if (!(a[i] = arena_strdup(&c->scratch, v[i], strlen(v[i])))) return NULL;
    return a;
}

/* ---------------------------------------------------------------- the until date */

static void until_text_ask(core *c);
static void until_default(core *c) {   /* the date picker's first value: now + keepDaysDefault (10) days */
    struct tm tm;
    if (!local_tm(now_s(c) + 10 * 86400, &tm)) return;
    snprintf(c->p.until, sizeof c->p.until, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    until_text_ask(c);
}

static int parse_date(const char *s, int *y, int *m, int *d) {
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return 0;
    for (int i = 0; i < 10; i++)
        if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return 0;
    *y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    *m = (s[5] - '0') * 10 + (s[6] - '0');
    *d = (s[8] - '0') * 10 + (s[9] - '0');
    return *y >= 1970 && *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31;
}

/* untilArg (PrepareView.swift:700): the end of the day chosen, at least an hour from now */
static void until_arg(core *c, char out[24]) {
    if (!c->p.until[0]) until_default(c);
    int y, m, d;
    int64_t now = now_s(c), end = now;
    if (parse_date(c->p.until, &y, &m, &d)) {
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        tm.tm_year = y - 1900;
        tm.tm_mon = m - 1;
        tm.tm_mday = d;
        tm.tm_hour = 23;
        tm.tm_min = 59;
        tm.tm_isdst = -1;
        time_t t = mktime(&tm);
        if (t != (time_t)-1) end = (int64_t)t;
    }
    if (end < now + 3600) end = now + 3600;
    snprintf(out, 24, "%lld", (long long)end);
}

/* the date picker's date as the log writes it: the host's medium date style, asked on a worker
 * (CORE_STEP_DATE_TEXT) whenever the date is set and kept for that date; until it has come (or
 * without it), YYYY-MM-DD. Main never calls the step. */
typedef struct { char date[16]; char text[200]; } date_result;
typedef struct { date_result *r; core_job *done; } date_work;

static void date_work_run(core_life *life, void *payload) {
    date_work *w = payload;
    if (life->host.step && !(life->native & NATIVE_STEP)) {
        arena a = {0};
        core_facts f;
        facts_init(&f, &a);
        const char *args[1] = {w->r->date};
        if (life->host.step(life->host.ctx, CORE_STEP_DATE_TEXT, args, 1, &f) == 0 && f.set[FACT_STEP_TEXT] && f.s[FACT_STEP_TEXT] &&
            f.len[FACT_STEP_TEXT] > 0 && f.len[FACT_STEP_TEXT] < sizeof w->r->text)
            memcpy(w->r->text, f.s[FACT_STEP_TEXT], f.len[FACT_STEP_TEXT] + 1);
        arena_free(&a);
    }
    job_post(life, w->done);
}
static void date_work_drop(void *payload) { core_job_drop(((date_work *)payload)->done); }

static void date_result_job(core *c, void *payload) {
    date_result *r = payload;
    if (!r->text[0] || strcmp(r->date, c->p.until)) return;   /* no text, or the date moved meanwhile */
    memcpy(c->p.until_text, r->text, sizeof c->p.until_text);
    memcpy(c->p.until_text_for, r->date, sizeof c->p.until_text_for);
}

static void until_text_ask(core *c) {
    date_work *w = malloc(sizeof *w);
    date_result *r = calloc(1, sizeof *r);
    core_job *done = w && r ? job_reserve(c->life, date_result_job, r, NULL) : NULL;
    if (!done) {
        free(w);
        free(r);
        return;
    }
    snprintf(r->date, sizeof r->date, "%s", c->p.until);
    w->r = r;
    w->done = done;
    core_post_work(c->life, CORE_WORK_FACT, date_work_run, w, date_work_drop);
}

static const char *until_text(core *c) {
    if (!c->p.until[0]) until_default(c);
    return !strcmp(c->p.until_text_for, c->p.until) && c->p.until_text[0] ? c->p.until_text : c->p.until;
}

static int day_key(int64_t t) {   /* the local date of t as yyyymmdd, 0 when it can't be read */
    struct tm tm;
    return local_tm(t, &tm) ? (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday : 0;
}

/* {"ev":"until"}: a real date in the date picker's range (PrepareView.swift:411-412: from
 * tomorrow to a year from now, by day); anything else is refused, and the picker drawn again
 * from F_PREP_UNTIL (SO3: a tab left open past midnight shows a day that is now today) */
static int until_set(core *c, const char *date);
int prep_until(core *c, const char *date) {
    int r = until_set(c, date);
    if (r != CORE_OK) {
        model_touch(&c->m, F_PREP_UNTIL);
        core_mark_changed(c);
    }
    return r;
}

static int until_set(core *c, const char *date) {
    int y, m, d;
    if (!parse_date(date, &y, &m, &d)) return CORE_E_ARG;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    if (mktime(&tm) == (time_t)-1 || tm.tm_year != y - 1900 || tm.tm_mon != m - 1 || tm.tm_mday != d) return CORE_E_ARG;   /* 02-31 */
    int key = y * 10000 + m * 100 + d;
    int64_t t = now_s(c);
    if (key < day_key(t + 86400) || key > day_key(t + 365 * 86400)) return CORE_E_ARG;
    snprintf(c->p.until, sizeof c->p.until, "%s", date);
    until_text_ask(c);
    model_set_str(&c->m, F_PREP_UNTIL, c->p.until, CORE_NTS);
    core_mark_changed(c);
    return CORE_OK;
}

/* ---------------------------------------------------------------- the tracks' rows */

static int checking_now(core *c) {   /* resultText's checking: (estimating || refreshing && estimate?.refused == nil) && !isRunning */
    prep_state *p = &c->p;
    return (p->estimating || (p->refreshing && !(p->est && p->est->e.has_refused))) && !p->running;
}

/* tableView(_:viewFor:row:) for the tracks: the row's three cells as the data source gives them now */
static void cells_of(core *c, int32_t row) {
    prep_state *p = &c->p;
    const prep_track *t = p->lines[row];
    pstr name = prep_last_path_component(t->file);
    model_cell_str(&c->m, C_TRK_NAME, row, name.s, name.n);
    char buf[2048];
    tbuf b;
    tb_init(&b, c->os, buf, sizeof buf);
    if (p->running && row == p->run_row && pstr_is_s(t->result, "prepare")) TB_TEXT0(&b, T_RES_PREPARING);
    else w_result_text(&b, t, p->cells_checking);
    model_cell_str(&c->m, C_TRK_RESULT, row, b.err ? "" : buf, CORE_NTS);
    tb_init(&b, c->os, buf, sizeof buf);
    w_track_note(&b, t);
    model_cell_str(&c->m, C_TRK_WHY, row, b.err ? "" : buf, CORE_NTS);
}

/* the cells follow the state they show: rewritten where it moved (the model keeps a cell that
 * doesn't change as it is) */
static void cells_state(core *c) {
    prep_state *p = &c->p;
    int checking = checking_now(c), running = p->running;
    int32_t old_row = p->cells_run_row;
    if (checking != p->cells_checking || running != p->cells_running) {
        p->cells_checking = checking;
        p->cells_running = running;
        p->cells_run_row = p->run_row;
        for (int32_t i = 0; i < p->nlines; i++) cells_of(c, i);
    } else if (p->run_row != old_row) {
        p->cells_run_row = p->run_row;
        if (old_row >= 0 && old_row < p->nlines) cells_of(c, old_row);
        if (p->run_row >= 0 && p->run_row < p->nlines) cells_of(c, p->run_row);
    }
    core_mark_changed(c);
}

/* tracksTable.reloadData() after `lines` changed as a whole: the same rows (by file, in order) are
 * rewritten in place; rows only added at the end are appended; anything else is a new list */
static void lines_replace(core *c, prep_track **v, int32_t n) {
    prep_state *p = &c->p;
    int32_t same = 0, keep = p->nlines <= n;
    while (same < p->nlines && same < n && pstr_eq(p->lines[same]->file, v[same]->file)) same++;
    keep = keep && same == p->nlines;
    int32_t was = p->nlines;
    lines_free(p->lines, p->nlines);
    p->lines = v;
    p->nlines = p->caplines = n;
    if (!keep) model_table_reset(&c->m, T_PREP_TRACKS, n);
    else if (n > was) model_table_append(&c->m, T_PREP_TRACKS, n - was);
    p->cells_checking = checking_now(c);
    p->cells_running = p->running;
    p->cells_run_row = p->run_row;
    for (int32_t i = 0; i < n; i++) cells_of(c, i);
    core_mark_changed(c);
}

/* lines = notChecked(lines): the same tracks, nothing said about them */
static void lines_not_checked(core *c) {
    prep_state *p = &c->p;
    prep_track **v = p->nlines ? malloc((size_t)p->nlines * sizeof *v) : NULL;
    if (p->nlines && !v) return;
    for (int32_t i = 0; i < p->nlines; i++)
        if (!(v[i] = track_bare(p->lines[i]->file))) {
            lines_free(v, i);
            return;
        }
    lines_replace(c, v, p->nlines);
}

static void lines_empty(core *c) { lines_replace(c, NULL, 0); }

/* place (Prepare.swift:228): t in its track's row, or a new row at the end; its row */
static int32_t place(core *c, const prep_track *t, int32_t hint, int *changed) {
    prep_state *p = &c->p;
    int32_t row = -1;
    if (hint >= 0 && hint < p->nlines && pstr_eq(p->lines[hint]->file, t->file)) row = hint;
    for (int32_t i = 0; row < 0 && i < p->nlines; i++)
        if (pstr_eq(p->lines[i]->file, t->file)) row = i;
    *changed = 1;
    if (row >= 0) {
        if (track_eq(p->lines[row], t)) {
            *changed = 0;
            return row;
        }
        prep_track *d = track_dup(t);
        if (!d) return row;
        free(p->lines[row]);
        p->lines[row] = d;
        cells_of(c, row);   /* reloadData(forRowIndexes: [row]) */
        return row;
    }
    if (p->nlines == p->caplines) {
        int32_t cap = p->caplines ? p->caplines * 2 : 64;
        prep_track **v = realloc(p->lines, (size_t)cap * sizeof *v);
        if (!v) return p->nlines - 1;
        p->lines = v;
        p->caplines = cap;
    }
    prep_track *d = track_dup(t);
    if (!d || model_table_append(&c->m, T_PREP_TRACKS, 1)) {
        free(d);
        return p->nlines - 1;
    }
    p->lines[p->nlines++] = d;
    cells_of(c, p->nlines - 1);
    return p->nlines - 1;
}

static void scroll_to(core *c, int32_t row) {   /* tracksTable.scrollRowToVisible(row) */
    model_set_i64(&c->m, F_PREP_SCROLL_ROW, row);
    model_set_i64(&c->m, F_PREP_SCROLL_SEQ, model_get_i64(&c->m, F_PREP_SCROLL_SEQ) + 1);
}

/* ---------------------------------------------------------------- the outline */

typedef struct { const char **k; int32_t *v; uint32_t cap; } smap;
static uint64_t fnv(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ULL;
    return h;
}
static int smap_init(core *c, smap *m, int32_t n) {
    m->cap = 16;
    while (m->cap < (uint32_t)n * 2 + 1) m->cap <<= 1;
    m->k = arena_alloc(&c->scratch, m->cap * sizeof *m->k);
    m->v = arena_alloc(&c->scratch, m->cap * sizeof *m->v);
    if (!m->k || !m->v) return -1;
    memset(m->k, 0, m->cap * sizeof *m->k);
    return 0;
}
static uint32_t smap_slot(const smap *m, const char *k) {
    uint32_t i = (uint32_t)fnv(k) & (m->cap - 1);
    while (m->k[i] && strcmp(m->k[i], k)) i = (i + 1) & (m->cap - 1);
    return i;
}
static void smap_put(smap *m, const char *k, int32_t v) {
    uint32_t i = smap_slot(m, k);
    m->k[i] = k;
    m->v[i] = v;
}
static int32_t smap_get(const smap *m, const char *k) {
    uint32_t i = smap_slot(m, k);
    return m->k[i] ? m->v[i] : -1;
}

static int closed_has(core *c, const char *id) {
    for (int32_t i = 0; i < c->p.nclosed; i++)
        if (!strcmp(c->p.closed[i], id)) return 1;
    return 0;
}
static void closed_remove(core *c, const char *id) {
    prep_state *p = &c->p;
    for (int32_t i = 0; i < p->nclosed; i++)
        if (!strcmp(p->closed[i], id)) {
            free(p->closed[i]);
            p->closed[i] = p->closed[--p->nclosed];
            return;
        }
}
static void closed_insert(core *c, const char *id) {
    prep_state *p = &c->p;
    if (closed_has(c, id)) return;
    if (p->nclosed == p->capclosed) {
        int32_t cap = p->capclosed ? p->capclosed * 2 : 16;
        char **v = realloc(p->closed, (size_t)cap * sizeof *v);
        if (!v) return;
        p->closed = v;
        p->capclosed = cap;
    }
    char *d = dupn(id, strlen(id));
    if (d) p->closed[p->nclosed++] = d;
}

enum { OUT_FOLDER = 0, OUT_PLAYLIST = 1, OUT_INTELLIGENT = 2, OUT_OTHER = 3 };
static int32_t out_kind(const prep_pl *p) {
    return !strcmp(p->kind, "folder") ? OUT_FOLDER : !strcmp(p->kind, "playlist") ? OUT_PLAYLIST
         : !strcmp(p->kind, "intelligent") ? OUT_INTELLIGENT : OUT_OTHER;
}

/* the playlists' tree (playlistTree: a parent comes before its children; the latest playlist
 * with an id is its holder) and its rows in pre-order. -1 when out of memory. */
static int tree_build(core *c, int32_t **first_child, int32_t **next_sib) {
    prep_state *p = &c->p;
    int32_t n = p->npl;
    free(p->pl_parent);
    free(p->pl_row);
    free(p->row_pl);
    p->pl_parent = p->pl_row = p->row_pl = NULL;
    if (!n) return 0;
    smap m;
    int32_t *last = arena_alloc(&c->scratch, (size_t)n * sizeof *last);
    *first_child = arena_alloc(&c->scratch, (size_t)n * sizeof **first_child);
    *next_sib = arena_alloc(&c->scratch, (size_t)n * sizeof **next_sib);
    p->pl_parent = malloc((size_t)n * sizeof *p->pl_parent);
    p->pl_row = malloc((size_t)n * sizeof *p->pl_row);
    p->row_pl = malloc((size_t)n * sizeof *p->row_pl);
    if (smap_init(c, &m, n) || !last || !*first_child || !*next_sib || !p->pl_parent || !p->pl_row || !p->row_pl) return -1;
    int32_t root_first = -1, root_last = -1;
    for (int32_t i = 0; i < n; i++) {
        (*first_child)[i] = last[i] = (*next_sib)[i] = -1;
        smap_put(&m, p->pls[i].id, i);   /* byID[p.id] = n, then the parent looked up */
        int32_t par = smap_get(&m, p->pls[i].parent);
        if (par == i) par = -1;          /* its own parent: a root here (Swift would loop) */
        p->pl_parent[i] = par;
        if (par < 0) {
            if (root_last < 0) root_first = i; else (*next_sib)[root_last] = i;
            root_last = i;
        } else {
            if (last[par] < 0) (*first_child)[par] = i; else (*next_sib)[last[par]] = i;
            last[par] = i;
        }
    }
    /* pre-order: a node, then its children's subtrees in order */
    int32_t *stack = arena_alloc(&c->scratch, (size_t)n * sizeof *stack), sp = 0, row = 0;
    if (!stack) return -1;
    for (int32_t r = root_first; r >= 0; r = (*next_sib)[r]) {
        stack[sp++] = r;
        while (sp) {
            int32_t i = stack[--sp];
            p->pl_row[i] = row;
            p->row_pl[row++] = i;
            /* children pushed last-first, so the first comes out first */
            int32_t k = 0;
            for (int32_t ch = (*first_child)[i]; ch >= 0; ch = (*next_sib)[ch]) k++;
            int32_t base = sp;
            sp += k;
            for (int32_t ch = (*first_child)[i], j = 0; ch >= 0; ch = (*next_sib)[ch], j++) stack[base + k - 1 - j] = ch;
        }
    }
    return 0;
}

static void outline_row_open(core *c, int32_t i, int open) { model_cell_i64(&c->m, C_OUT_OPEN, c->p.pl_row[i], open); }

/* closeFolders(in: roots) (PrepareView.swift:516), in its order (each node after its children):
 * the folders the user closed, closed again, except one the selected playlist is in, whose row
 * stays visible (it is no longer counted as closed) */
static void close_folders(core *c, const int32_t *first_child, const int32_t *next_sib) {
    prep_state *p = &c->p;
    int32_t n = p->npl;
    int32_t *holds = arena_alloc(&c->scratch, (size_t)n * sizeof *holds), *cur = arena_alloc(&c->scratch, (size_t)n * sizeof *cur);
    int32_t *stack = arena_alloc(&c->scratch, (size_t)n * sizeof *stack);
    if (!holds || !cur || !stack) return;
    const char *sel = p->has_sel ? p->sel.id : NULL;
    for (int32_t r = 0; r < n; r++) {
        int32_t root = p->row_pl[r];
        if (p->pl_parent[root] >= 0) continue;
        int32_t sp = 0;
        stack[sp++] = root;
        cur[root] = first_child[root];
        holds[root] = 0;
        while (sp) {
            int32_t i = stack[sp - 1];
            if (cur[i] >= 0) {   /* the next child first */
                int32_t ch = cur[i];
                cur[i] = next_sib[ch];
                cur[ch] = first_child[ch];
                holds[ch] = 0;
                stack[sp++] = ch;
                continue;
            }
            sp--;
            int inside = 0;
            for (int32_t ch = first_child[i]; ch >= 0; ch = next_sib[ch]) inside |= holds[ch];
            if (inside) closed_remove(c, p->pls[i].id);
            else if (closed_has(c, p->pls[i].id)) outline_row_open(c, i, 0);
            holds[i] = inside || str_eq0(p->pls[i].id, sel);
        }
    }
}

/* drawOutline (PrepareView.swift:505): the list from the tree, every folder open, except those
 * the user closed */
static void draw_outline(core *c) {
    prep_state *p = &c->p;
    int32_t *first_child = NULL, *next_sib = NULL;
    if (tree_build(c, &first_child, &next_sib) || model_table_reset(&c->m, T_PREP_OUTLINE, p->npl)) {
        core_log(c->life, "core: out of memory drawing the playlists");
        return;
    }
    for (int32_t r = 0; r < p->npl; r++) {
        int32_t i = p->row_pl[r], par = p->pl_parent[i], depth = 0;
        for (int32_t a = par; a >= 0; a = p->pl_parent[a]) depth++;
        const prep_pl *pl = &p->pls[i];
        model_cell_str(&c->m, C_OUT_ID, r, pl->id, CORE_NTS);
        model_cell_i64(&c->m, C_OUT_PARENT, r, par >= 0 ? p->pl_row[par] : -1);
        model_cell_i64(&c->m, C_OUT_DEPTH, r, depth);
        model_cell_i64(&c->m, C_OUT_KIND, r, out_kind(pl));
        int prep = can_prepare(pl);
        if (prep) {   /* "\(name)  (\(tracks))" */
            char num[24];
            snprintf(num, sizeof num, "%lld", (long long)pl->tracks);
            size_t k = strlen(pl->name) + strlen(num) + 5;
            char *t = arena_alloc(&c->scratch, k);
            if (t) {
                t[0] = 0;
                strcat(t, pl->name);
                strcat(t, "  (");
                strcat(t, num);
                strcat(t, ")");
                model_cell_str(&c->m, C_OUT_TITLE, r, t, CORE_NTS);
            }
        } else model_cell_str(&c->m, C_OUT_TITLE, r, pl->name, CORE_NTS);
        model_cell_i64(&c->m, C_OUT_SECONDARY, r, !prep);
        model_cell_i64(&c->m, C_OUT_OPEN, r, first_child[i] >= 0);   /* expandItem(nil, expandChildren: true) */
    }
    close_folders(c, first_child, next_sib);
    model_set_i64(&c->m, F_PREP_SELECTED_ROW, -1);   /* a reload drops the selection; selectRow sets it again */
    core_mark_changed(c);
}

static int row_open(core *c, int32_t i) {   /* outline.isItemExpanded: the row's C_OUT_OPEN now */
    const table_obj *t = c->m.tab[T_PREP_OUTLINE];
    int32_t r = c->p.pl_row[i];
    return r < t->n && t->rows[r]->cells[core_column_meta[C_OUT_OPEN].index].v.i != 0;
}

/* selectRow(id, open:) (PrepareView.swift:540): its row selected, if it is shown, and scrolled to */
static void select_row(core *c, const char *id, int open) {
    prep_state *p = &c->p;
    if (!id || !p->pl_row) return;
    int32_t node = -1;
    for (int32_t r = 0; r < p->npl && node < 0; r++)
        if (!strcmp(p->pls[p->row_pl[r]].id, id)) node = p->row_pl[r];
    if (node < 0) return;
    int visible = 1;
    /* the folders it is in, from the top: opened with `open` */
    int32_t chain[256], k = 0;
    for (int32_t a = p->pl_parent[node]; a >= 0 && k < 256; a = p->pl_parent[a]) chain[k++] = a;
    for (int32_t j = k - 1; j >= 0; j--) {
        int32_t f = chain[j];
        if (open && !row_open(c, f)) {
            outline_row_open(c, f, 1);
            closed_remove(c, p->pls[f].id);
        }
        visible = visible && row_open(c, f);
    }
    int32_t row = visible ? p->pl_row[node] : -1;   /* outline.row(forItem:): -1 inside a closed folder */
    if (row >= 0 && row != model_get_i64(&c->m, F_PREP_SELECTED_ROW)) {
        model_set_i64(&c->m, F_PREP_SELECTED_ROW, row);
        model_set_i64(&c->m, F_PREP_OUTLINE_SCROLL_SEQ, model_get_i64(&c->m, F_PREP_OUTLINE_SCROLL_SEQ) + 1);
    }
    core_mark_changed(c);
}

/* outlineViewItemDidCollapse / DidExpand: the user's own (the shell guards its reloads) */
void prep_user_folder(core *c, int32_t row, int open) {
    prep_state *p = &c->p;
    if (!p->row_pl || row < 0 || row >= p->npl) return;
    int32_t i = p->row_pl[row];
    if (open) closed_remove(c, p->pls[i].id);
    else closed_insert(c, p->pls[i].id);
    outline_row_open(c, i, open);
    core_mark_changed(c);
}

/* ---------------------------------------------------------------- update() */

static void fill_summary(tbuf *b, const void *a) {
    core *c = (core *)a;
    w_run_summary(b, (pstr){c->p.run_pl.name, strlen(c->p.run_pl.name)}, c->p.paused, c->p.run_fraction);
}

void prep_run_summary(core *c, tbuf *b) {   /* runSummary */
    if (!c->p.running || !c->p.has_run_pl) return;
    fill_summary(b, c);
}

static void fill_est_summary(tbuf *b, const void *a) { w_estimate_summary(b, a); }

/* update() (PrepareView.swift:708): every label and button as things are now */
void prep_update(core *c) {
    if (c->role != CORE_ROLE_APP) return;
    prep_state *p = &c->p;
    model *md = &c->m;
    int running = p->running;
    if (!p->until[0]) until_default(c);
    model_set_str(md, F_PREP_UNTIL, p->until, CORE_NTS);
    const char *name = running && p->has_run_pl ? p->run_pl.name : p->has_sel ? p->sel.name
                     : text0(c, p->load_error ? T_PREP_RB_PLAYLISTS : T_PREP_CHOOSE);
    model_set_str(md, F_PREP_NAME, name, CORE_NTS);
    const char *text = "", *sub = "";
    if (p->load_error) text = p->load_error;
    else if (p->nproblems) text = p->problems;
    else if (running) {
        const char *sum = tx(c, fill_summary, c), *phase = p->run_phase ? p->run_phase : "";
        size_t k = strlen(sum) + strlen(phase) + 2;
        char *t = arena_alloc(&c->scratch, k);
        if (t) {
            t[0] = 0;
            strcat(t, sum);
            strcat(t, "\n");
            strcat(t, phase);
            text = t;
        }
    } else if (p->has_sel && !can_prepare(&p->sel)) {
        text = text0(c, !strcmp(p->sel.kind, "folder") ? T_PREP_FOLDER : !strcmp(p->sel.kind, "intelligent") ? T_PREP_INTELLIGENT : T_PREP_CANT_LIST);
    } else if (p->has_sel && p->estimating) text = text0(c, T_PREP_CHECKING_PL);
    else if (p->est) {
        const prep_estimate *e = &p->est->e;
        if (e->has_refused) text = arena_strdup(&c->scratch, e->refused.s, e->refused.n);
        else {
            const char *s = tx(c, fill_est_summary, e);
            if (!e->disk_ok) {
                const char *more = text0(c, T_PREP_NO_DISK_ROOM);
                size_t k = strlen(s) + strlen(more) + 1;
                char *t = arena_alloc(&c->scratch, k);
                if (t) {
                    t[0] = 0;
                    strcat(t, s);
                    strcat(t, more);
                    s = t;
                }
            }
            text = s;
            if (e->asking > 0) sub = text0(c, T_PREP_ASKING);
            else if (p->last_run && *p->last_run) sub = p->last_run;
        }
    } else if (!p->has_sel) text = text0(c, T_PREP_INTRO);
    else if (p->run_phase && *p->run_phase) text = p->run_phase;
    model_set_str(md, F_PREP_INFO, text ? text : "", CORE_NTS);
    model_set_str(md, F_PREP_INFO_SUB, sub, CORE_NTS);
    int ready = !running && !c->action_running && !c->fl.prep_reserved && !p->nproblems && !p->load_error && p->has_sel && can_prepare(&p->sel) && !p->estimating;
    const prep_estimate *e = p->est ? &p->est->e : NULL;
    int enabled = ready && e && !e->has_refused && e->prepare + e->ready > 0;
    model_set_i64(md, F_PREP_PREPARE_ENABLED, enabled);
    int keep = (e ? e->prepare : 1) == 0 && (e ? e->ready : 0) > 0;
    model_set_str(md, F_PREP_PREPARE_TITLE, text0(c, keep ? T_KEEP : T_BTN_PREPARE), CORE_NTS);
    model_set_i64(md, F_PREP_KEEP_ENABLED, enabled && keep);
    model_set_i64(md, F_PREP_CHECK_ENABLED, !running && p->has_sel);
    model_set_i64(md, F_PREP_UNTIL_ENABLED, !running);
    model_set_i64(md, F_PREP_RUNNING, running);
    model_set_str(md, F_PREP_PAUSE_TITLE, text0(c, p->paused ? T_BTN_RESUME : T_BTN_PAUSE), CORE_NTS);
    model_set_f64(md, F_PREP_PROGRESS, p->run_fraction);
    model_set_i64(md, F_PREP_OUTLINE_ENABLED, !running);
    model_set_i64(md, F_PREP_RUN_ROW, running ? p->run_row : -1);
    cells_state(c);
    status_update_buttons(c);
    core_mark_changed(c);
}

/* ---------------------------------------------------------------- the checks */

static void prep_gen_bump(core *c) {   /* gen += 1: the estimate under way is stopped (HelperJob.cancel) */
    prep_state *p = &c->p;
    c->gen[DOM_PREPARE]++;
    if (p->est_run && c->life->host.helper_signal && !(c->life->native & NATIVE_HELPER))
        CALLBACK(c, c->life->host.helper_signal(c->life->host.ctx, p->est_run, CORE_HELPER_STOP));
    p->est_run = 0;
}

static void prep_check(core *c, int results, int quiet);
static void prepare_action(core *c);

/* runEstimate (PrepareView.swift:639): its tracks' rows as they come, then all of it */
static void est_track(core *c, hrun *h, const prep_track *t) {
    (void)h;
    prep_state *p = &c->p;
    int changed;
    int32_t row = place(c, t, p->next_row, &changed);
    p->next_row = row + 1;
    if (changed) core_mark_changed(c);
}

static void est_end(core *c, hrun *h) {
    prep_state *p = &c->p;
    if (h->gen != c->gen[DOM_PREPARE]) return;   /* moved on */
    p->est_run = 0;
    int was_checking = p->estimating || p->refreshing;   /* rows saying "Checking…" */
    p->estimating = 0;
    p->refreshing = 0;
    prep_est *e = NULL;
    if (!h->oom && prep_estimate_ok(&h->e) && (e = calloc(1, sizeof *e))) {   /* parseEstimate's result */
        e->a = h->a;   /* the estimate's strings move with it */
        memset(&h->a, 0, sizeof h->a);
        e->e = h->e;
    }
    set_est(c, e);
    /* rows = e.flatMap { refused == nil ? tracks : nil } ?? notChecked(lines) */
    prep_track **rows = NULL;
    int32_t n = 0, ok = 1;
    if (e && !e->e.has_refused) {
        n = e->e.ntracks;
        if (n && !(rows = malloc((size_t)n * sizeof *rows))) ok = 0;
        for (int32_t i = 0; ok && i < n; i++)
            if (!(rows[i] = track_dup(&e->e.tracks[i]))) {
                lines_free(rows, i);
                rows = NULL;
                ok = 0;
            }
    } else {
        n = p->nlines;
        if (n && !(rows = malloc((size_t)n * sizeof *rows))) ok = 0;
        for (int32_t i = 0; ok && i < n; i++)
            if (!(rows[i] = track_bare(p->lines[i]->file))) {
                lines_free(rows, i);
                rows = NULL;
                ok = 0;
            }
    }
    if (ok) {
        int same = n == p->nlines;
        for (int32_t i = 0; same && i < n; i++) same = track_eq(rows[i], p->lines[i]);
        if (!same || was_checking) lines_replace(c, rows, n);
        else lines_free(rows, n);
    }
    if (!e) {
        const char *err = h->err && *h->err ? repaired(c, h->err) : NULL;
        problems_set1(c, err ? err : text0(c, T_PREP_PL_UNCHECKABLE));
    }
    prep_update(c);
    /* a Prepare clicked meanwhile: only where the user still is, and only if the new estimate
     * leaves the button on */
    if (p->held) {
        p->held = 0;
        if (model_get_i64(&c->m, F_PREP_PREPARE_ENABLED) && view_in_front_held(c, CORE_TAB_PREPARE)) prepare_action(c);
    }
}

static void run_estimate(core *c, const char *const *argv, int32_t argc, uint64_t g) {
    hrun *h = hrun_new(HK_ESTIMATE);
    if (!h) return;
    h->domain = DOM_PREPARE;
    h->gen = g;
    h->on_track = est_track;
    h->on_end = est_end;
    c->p.est_run = helper_start_run(c, h, argv, argc);
}

/* check's --tracks run: what it captured */
typedef struct { uint64_t g; int silent, checks; int32_t argc; char **argv; } tracks_ctx;
static void tracks_ctx_free(void *p) {
    tracks_ctx *t = p;
    if (!t) return;
    for (int32_t i = 0; i < t->argc; i++) free(t->argv[i]);
    free(t->argv);
    free(t);
}

static void tracks_end(core *c, hrun *h) {
    prep_state *p = &c->p;
    tracks_ctx *t = h->ctx;
    if (t->g != c->gen[DOM_PREPARE]) return;   /* moved on: no check of this one */
    if (h->status == 0 && !h->oom) {
        int32_t n = h->nfiles;
        prep_track **rows = n ? malloc((size_t)n * sizeof *rows) : NULL;
        int ok = !n || rows;
        if (ok && t->silent) {   /* the rows shown stay for tracks still there (the first of a file) */
            smap m;
            ok = !smap_init(c, &m, p->nlines);
            for (int32_t i = p->nlines - 1; ok && i >= 0; i--) {
                const char *k = arena_strdup(&c->scratch, p->lines[i]->file.s, p->lines[i]->file.n);
                if (!k) ok = 0;
                else smap_put(&m, k, i);
            }
            for (int32_t i = 0; ok && i < n; i++) {
                int32_t at = smap_get(&m, h->files[i].s);
                if (at >= 0 && !pstr_eq(p->lines[at]->file, h->files[i])) at = -1;   /* a NUL inside: bytes decide */
                if (!(rows[i] = at >= 0 ? track_dup(p->lines[at]) : track_bare(h->files[i]))) {
                    lines_free(rows, i);
                    rows = NULL;
                    ok = 0;
                }
            }
            if (ok) {
                int same = n == p->nlines;
                for (int32_t i = 0; same && i < n; i++) same = track_eq(rows[i], p->lines[i]);
                if (!same) lines_replace(c, rows, n);
                else lines_free(rows, n);
            }
        } else if (ok) {
            for (int32_t i = 0; ok && i < n; i++)
                if (!(rows[i] = track_bare(h->files[i]))) {
                    lines_free(rows, i);
                    rows = NULL;
                    ok = 0;
                }
            if (ok) lines_replace(c, rows, n);
        }
    } else if (!t->checks) {   /* an estimate would say why itself */
        lines_empty(c);
        const char *err = h->err && *h->err ? repaired(c, h->err) : NULL;
        problems_append(c, err ? err : text0(c, T_PREP_LIB_UNREADABLE));
        prep_update(c);
    }
    if (!t->checks) return;
    if (view_in_front(c, CORE_TAB_PREPARE)) run_estimate(c, (const char *const *)t->argv, t->argc, t->g);
    else if (t->silent) {   /* checked on coming back */
        p->refreshing = p->held = p->estimating = 0;
        prep_update(c);
    } else {   /* left the tab or the app meanwhile: no read now */
        p->estimating = 0;
        set_est(c, NULL);
        prep_update(c);
    }
}

/* check(results:quiet:) (PrepareView.swift:573): the checks again, the selected playlist's
 * tracks from the library, then, with `results` and nothing in the way, what each one needs */
static void prep_check(core *c, int results, int quiet) {
    prep_state *p = &c->p;
    prep_now n;
    prep_facts_now(c, SHA_WAIT_PREP_UPDATE, &n);
    set_problems(c, &n);
    prep_gen_bump(c);
    uint64_t g = c->gen[DOM_PREPARE];
    if (p->running) {
        p->estimating = p->refreshing = p->held = 0;
        prep_update(c);
        return;
    }
    if (!p->has_sel || !can_prepare(&p->sel)) {
        p->estimating = 0;
        set_est(c, NULL);
        lines_empty(c);
        prep_update(c);
        return;
    }
    int checks = results && !p->nproblems;
    if (quiet && !results && !p->nproblems && p->est) {
        /* left the tab or the app before the playlists came: what is shown stays, and coming back
         * checks again */
        p->refreshing = p->held = p->estimating = 0;
        prep_update(c);
        return;
    }
    /* quiet: the playlist shown is checked again in the background; what it shows changes only
     * where the check says something else */
    int silent = quiet && checks && p->est;
    p->refreshing = silent;
    if (!silent) {
        p->held = 0;
        p->estimating = checks;
        if (!checks) set_est(c, NULL);
        lines_not_checked(c);
    }
    p->next_row = 0;
    prep_update(c);
    /* the estimate's arguments: base + --estimate --playlist <id> --until <untilArg> */
    const char *argv[24];
    int32_t k = base_args(c, &n, argv);
    char until[24];
    until_arg(c, until);
    argv[k++] = "--estimate";
    argv[k++] = "--playlist";
    argv[k++] = p->sel.id;
    argv[k++] = "--until";
    argv[k++] = until;
    tracks_ctx *t = calloc(1, sizeof *t);
    hrun *h = hrun_new(HK_TRACKS);
    if (t && (t->argv = calloc((size_t)k, sizeof *t->argv))) {
        t->argc = k;
        for (int32_t i = 0; i < k; i++)
            if (!(t->argv[i] = dupn(argv[i], strlen(argv[i])))) {
                t->argc = i;
                tracks_ctx_free(t);
                t = NULL;
                break;
            }
    } else {
        free(t);
        t = NULL;
    }
    if (!t || !h) {
        tracks_ctx_free(t);
        hrun_free(h);
        core_log(c->life, "core: out of memory starting a check");
        return;
    }
    t->g = g;
    t->silent = silent;
    t->checks = checks;
    h->ctx = t;
    h->ctx_free = tracks_ctx_free;
    h->on_end = tracks_end;
    const char *targv[2] = {"--tracks", p->sel.id};
    helper_start_run(c, h, targv, 2);
}

/* ---------------------------------------------------------------- the playlists */

enum { THEN_NONE = 0, THEN_CHECK = 1, THEN_SELECT = 2 };
typedef struct { uint64_t g; int then; int silent; char *shown; char *id; } list_ctx;
static void list_ctx_free(void *p) {
    list_ctx *l = p;
    if (!l) return;
    free(l->shown);
    free(l->id);
    free(l);
}

static void choose(core *c, const prep_pl *pl);

/* p?.id != selected?.id, with Swift's optionals */
static int ids_differ(const prep_pl *pl, const prep_state *p) {
    const char *a = pl ? pl->id : NULL, *b = p->has_sel ? p->sel.id : NULL;
    return (a == NULL) != (b == NULL) || (a && strcmp(a, b));
}

/* show(r) (PrepareView.swift:485): what --playlists said. The list on the left is redrawn only
 * when it changed */
static void show(core *c, hrun *h) {
    prep_state *p = &c->p;
    int ok = h->status == 0 && !h->oom;
    if (ok) set_owned(&p->load_error, NULL);
    else {
        const char *err = h->err && *h->err ? repaired(c, h->err) : NULL;
        set_owned(&p->load_error, err ? err : text0(c, T_PREP_LIB_UNREADABLE));
    }
    if (ok) {
        int same = h->npl == p->npl;
        prep_pl *v = h->npl ? calloc((size_t)h->npl, sizeof *v) : NULL;
        if (h->npl && !v) return;
        for (int32_t i = 0; i < h->npl; i++) {
            const prep_playlist *s = &h->pls[i];
            prep_pl tmp = {(char *)s->id.s, (char *)s->parent.s, (char *)s->name.s, (char *)s->kind.s, s->tracks};
            if (pl_copy(&v[i], &tmp)) {
                for (int32_t j = 0; j < i; j++) pl_clear(&v[j]);
                free(v);
                return;
            }
            same = same && pl_eq(&v[i], &p->pls[i]);
        }
        if (!same) {
            for (int32_t i = 0; i < p->npl; i++) pl_clear(&p->pls[i]);
            free(p->pls);
            p->pls = v;
            p->npl = h->npl;
            draw_outline(c);
        } else {
            for (int32_t i = 0; i < h->npl; i++) pl_clear(&v[i]);
            free(v);
        }
    }
    if (p->running && p->has_run_pl) {   /* the run's playlist as it is now (renamed) */
        for (int32_t i = 0; i < p->npl; i++)
            if (!strcmp(p->pls[i].id, p->run_pl.id)) {
                prep_pl n;
                if (!pl_copy(&n, &p->pls[i])) {
                    pl_clear(&p->run_pl);
                    p->run_pl = n;
                }
                break;
            }
    }
    if (p->has_sel) {   /* renamed, or deleted */
        int32_t at = -1;
        for (int32_t i = 0; i < p->npl && at < 0; i++)
            if (!strcmp(p->pls[i].id, p->sel.id)) at = i;
        if (at >= 0) {
            prep_pl n;
            if (!pl_copy(&n, &p->pls[at])) {
                pl_clear(&p->sel);
                p->sel = n;
            }
        } else choose(c, NULL);
    }
    select_row(c, p->has_sel ? p->sel.id : NULL, 0);   /* reloadingList: no choose from it */
    status_names_changed(c);
    prep_update(c);
}

static void list_end(core *c, hrun *h) {
    list_ctx *l = h->ctx;
    if (l->g == c->gen[DOM_LIST]) show(c, h);   /* only the last read says what the library holds now */
    switch (l->then) {   /* then?(): whatever the read */
    case THEN_CHECK:
        prep_check(c, view_in_front(c, CORE_TAB_PREPARE), l->silent && c->p.has_sel && str_eq0(c->p.sel.id, l->shown));
        break;
    case THEN_SELECT: {   /* select(id:)'s go */
        prep_state *p = &c->p;
        for (int32_t i = 0; i < p->npl; i++)
            if (!strcmp(p->pls[i].id, l->id)) {
                prep_pl pl;
                if (pl_copy(&pl, &p->pls[i])) break;
                select_row(c, l->id, 1);
                choose(c, &pl);
                pl_clear(&pl);
                break;
            }
        break;
    }
    default: break;
    }
}

/* loadPlaylists(then:) (PrepareView.swift:466): rekordbox's playlists, read by the helper */
static void load_playlists(core *c, int then, int silent, const char *shown, const char *id) {
    prep_state *p = &c->p;
    prep_now n;
    prep_facts_now(c, SHA_WAIT_PREP_CHECK, &n);
    set_problems(c, &n);
    if (!n.pf.helper_there || !n.pf.apple_silicon) {
        p->estimating = p->refreshing = p->held = 0;
        cells_state(c);   /* tracksTable.reloadData() */
        prep_update(c);
        return;
    }
#ifdef CORE_WIN_POLICY
    if (n.location_unchecked) prep_update(c);
#endif
    c->gen[DOM_LIST]++;
    list_ctx *l = calloc(1, sizeof *l);
    hrun *h = hrun_new(HK_PLAYLISTS);
    if (!l || !h || (shown && !(l->shown = dupn(shown, strlen(shown)))) || (id && !(l->id = dupn(id, strlen(id))))) {
        list_ctx_free(l);
        hrun_free(h);
        core_log(c->life, "core: out of memory reading the playlists");
        return;
    }
    l->g = c->gen[DOM_LIST];
    l->then = then;
    l->silent = silent;
    h->ctx = l;
    h->ctx_free = list_ctx_free;
    h->on_end = list_end;
    const char *argv[1] = {"--playlists"};
    helper_start_run(c, h, argv, 1);
}

void prep_load_playlists(core *c) { load_playlists(c, THEN_NONE, 0, NULL, NULL); }

/* reloadList(quiet:) (PrepareView.swift:681): the playlists again, then the check; "Checking"
 * meanwhile, so Prepare doesn't act on the last estimate. quiet: a playlist already checked keeps
 * what it shows until the new check says otherwise */
static void reload_list(core *c, int quiet) {
    prep_state *p = &c->p;
    if (p->running) {
        prep_load_playlists(c);
        prep_check(c, 1, 0);   /* recheck() */
        return;
    }
    const char *shown = p->has_sel ? p->sel.id : NULL;
    char *shown_copy = shown ? arena_strdup(&c->scratch, shown, strlen(shown)) : NULL;
    int silent = quiet && !p->estimating && p->est && shown && !p->nproblems;
    prep_gen_bump(c);   /* a check under way is of what was true before */
    if (silent) {
        p->refreshing = 1;
        cells_state(c);
    } else {
        p->held = 0;
        p->estimating = p->has_sel && can_prepare(&p->sel) && !p->nproblems;
        lines_not_checked(c);
        prep_update(c);
    }
    load_playlists(c, THEN_CHECK, silent, shown_copy, NULL);
}

/* choose (PrepareView.swift:559) */
static void choose(core *c, const prep_pl *pl) {
    prep_state *p = &c->p;
    if (p->running) return;
    if (ids_differ(pl, p)) {   /* p?.id != selected?.id */
        set_owned(&p->last_run, "");
        lines_empty(c);
    }
    prep_pl n;
    if (pl && pl_copy(&n, pl)) return;
    if (p->has_sel) pl_clear(&p->sel);
    p->has_sel = pl != NULL;
    if (pl) p->sel = n;
    set_est(c, NULL);
    prep_check(c, 1, 0);   /* recheck() */
}

/* outlineViewSelectionDidChange: the user chose a row (or none) */
void prep_user_select(core *c, int32_t row) {
    prep_state *p = &c->p;
    const prep_pl *pl = row >= 0 && p->row_pl && row < p->npl ? &p->pls[p->row_pl[row]] : NULL;
    if (ids_differ(pl, p)) choose(c, pl);
}

/* select(id:) (PrepareView.swift:528): Status' Prepare Again */
void prep_select_id(core *c, const char *id) {
    prep_state *p = &c->p;
    if (p->running) return;
    if (!p->npl) {
        load_playlists(c, THEN_SELECT, 0, NULL, id);
        return;
    }
    for (int32_t i = 0; i < p->npl; i++)
        if (!strcmp(p->pls[i].id, id)) {
            prep_pl pl;
            if (pl_copy(&pl, &p->pls[i])) return;
            select_row(c, id, 1);
            choose(c, &pl);
            pl_clear(&pl);
            return;
        }
}

void prep_will_appear(core *c) {   /* viewWillAppear: the list read before the tab was first shown is drawn already */
    prep_state *p = &c->p;
    reload_list(c, !p->stale);
    p->stale = 0;
}

void prep_set_stale(core *c) { c->p.stale = 1; }

void prep_reload_from_change(core *c, int quiet) {
    prep_state *p = &c->p;
    reload_list(c, quiet && !p->stale);
    p->stale = 0;
}

void prep_sha_known(core *c, int check) {
    if (check) prep_check(c, view_in_front(c, CORE_TAB_PREPARE), 0);
    else prep_update(c);
}

int prep_has_selection(core *c) { return c->p.has_sel; }
int prep_running(core *c) { return c->p.running; }
const char *prep_running_set(core *c) { return c->p.running && c->p.has_run_pl ? c->p.run_pl.id : NULL; }
const char *prep_run_name(core *c) { return c->p.has_run_pl ? c->p.run_pl.name : ""; }

void prep_playlist_name(core *c, const char *id, tbuf *b) {   /* playlistName(id) */
    for (int32_t i = 0; i < c->p.npl; i++)
        if (!strcmp(c->p.pls[i].id, id)) {
            tb_puts(b, c->p.pls[i].name);
            return;
        }
    core_slot sl = SLOT_S(name, id);
    tb_text(b, T_PREP_PLAYLIST_ID, &sl, 1);
}

/* ---------------------------------------------------------------- the run */

static void run_flags(core *c) {   /* the core's busy facts for the menus and quit */
    c->preparing = c->p.running;
    c->paused = c->p.paused;
}

static void log_cat(core *c, const char *a, const char *b) {
    size_t n = strlen(a) + strlen(b) + 1;
    char *line = arena_alloc(&c->scratch, n);
    if (!line) return;
    line[0] = 0;
    strcat(line, a);
    strcat(line, b);
    core_log(c->life, line);
}

typedef struct { core *c; const char *s; } np_arg;
static void fill_no_paths(tbuf *b, const void *a) { w_no_paths(b, ((const np_arg *)a)->s); }
static const char *nopaths(core *c, const char *s) { return tx(c, fill_no_paths, &(np_arg){c, s}); }

static void run_line(core *c, hrun *h, const prep_run_line *l);
static void run_end(core *c, hrun *h);

/* startRun (PrepareView.swift:785) */
static void start_run(core *c, const prep_pl *pl, const char *const *argv, int32_t argc, int partial) {
    prep_state *p = &c->p;
    if (c->action_running || c->fl.prep_reserved || p->running) return;   /* an install or uninstall changes what it reads */
    p->run_got_done = 0;
    set_owned(&p->run_stopped, NULL);
    set_owned(&p->run_error, NULL);
    p->run_partial = partial;
    prep_gen_bump(c);   /* a check under way isn't this run's list */
    p->estimating = p->refreshing = p->held = 0;
    p->next_row = 0;
    prep_pl n;
    if (pl_copy(&n, pl)) return;
    if (p->has_run_pl) pl_clear(&p->run_pl);
    p->run_pl = n;
    p->has_run_pl = 1;
    p->run_done = p->run_skipped = 0;
    p->run_files = p->est && p->est->e.ntracks > 1 ? p->est->e.ntracks : 1;
    p->run_fraction = 0;
    set_owned(&p->run_phase, text0(c, T_RUN_STARTING));
    p->run_row = -1;
    cells_state(c);
    char line[512];   /* PrepareView.swift:803: "prepare: playlist <id>, kept until <dateOnly of the date picker's date>" */
    snprintf(line, sizeof line, "prepare: playlist %s, kept until %s", pl->id, until_text(c));
    core_log(c->life, line);
    hrun *h = hrun_new(HK_RUN);
    uint64_t run = 0;
    if (h) {
        h->on_line = run_line;
        h->on_end = run_end;
        run = helper_start_run(c, h, argv, argc);
    }
    if (run) {
        p->run = run;
        p->running = 1;
        p->paused = 0;
    } else {
        p->run = 0;
        set_owned(&p->run_phase, text0(c, T_RUN_NO_HELPER));
        core_log(c->life, "prepare: the helper couldn't start");
    }
    run_flags(c);
    prep_update(c);
    status_reload_if_showing(c);
}

static void fill_last_run(tbuf *b, const void *a) {
    const core *c = a;
    const prep_state *p = &c->p;
    pstr st = {p->run_stopped ? p->run_stopped : "", p->run_stopped ? strlen(p->run_stopped) : 0};
    w_last_run_line(b, p->run_done, p->run_skipped, p->run_stopped ? &st : NULL, p->run_error != NULL);
}

typedef struct { const prep_run_line *l; const prep_state *p; } done_arg;
static void fill_done(tbuf *b, const void *a) {
    const done_arg *d = a;
    w_run_done_line(b, d->p->run_done, d->p->run_skipped, d->l->kept_gb, d->l->has_stopped ? &d->l->stopped : NULL,
                    d->l->has_error ? &d->l->error : NULL);
}

/* runLine (PrepareView.swift:810) */
static void run_line(core *c, hrun *h, const prep_run_line *l) {
    (void)h;
    prep_state *p = &c->p;
    switch (l->kind) {
    case RUN_PROGRESS: {
        p->run_files = l->files > 1 ? l->files : 1;
        p->run_fraction = ((double)l->at + (double)l->chunk / (double)(l->chunks > 1 ? l->chunks : 1)) / (double)p->run_files;
        char a[24], b[24];
        snprintf(a, sizeof a, "%lld", (long long)(l->at + 1));
        snprintf(b, sizeof b, "%lld", (long long)l->files);
        core_slot sl[2] = {SLOT_S(n, a), SLOT_S(m, b)};
        set_owned(&p->run_phase, tx(c, fill_text, &(tx_args){T_RUN_TRACK_OF, sl, 2}));   /* how far: the bar */
        /* the rows are the run's tracks in the same order, unless the list changed under it */
        int32_t row = (p->run_partial || l->files == p->nlines) && l->at < p->nlines && l->at >= 0 ? (int32_t)l->at : -1;
        if (row != p->run_row) {
            p->run_row = row;
            cells_state(c);
            if (row >= 0) scroll_to(c, row);
        }
        break;
    }
    case RUN_TRACK: {
        int changed;
        int32_t row = place(c, &l->track, p->next_row, &changed);
        p->next_row = row + 1;
        if (pstr_is_s(l->track.result, "skipped")) {
            p->run_skipped++;
            char *why = arena_strdup(&c->scratch, l->track.why.s, l->track.why.n);
            log_cat(c, "prepare: a track skipped: ", nopaths(c, why ? why : ""));
        } else if (pstr_is_s(l->track.result, "prepared")) p->run_done++;   /* a track cut short isn't done */
        if (row >= 0) scroll_to(c, row);
        break;
    }
    case RUN_ROOM: {
        char n[24], gb[64], line[160];
        snprintf(n, sizeof n, "%lld", (long long)l->removed);
        fmt_fixed(l->cache_gb, 2, gb);
        line[0] = 0;
        strcat(line, "prepare: made room: ");
        strcat(line, n);
        strcat(line, " stems removed, the cache now ");
        strcat(line, gb);
        strcat(line, " GB");
        core_log(c->life, line);
        break;
    }
    case RUN_DONE: {
        p->run_got_done = 1;
        set_owned(&p->run_stopped, NULL);
        if (l->has_stopped && !(p->run_stopped = dupn(l->stopped.s, l->stopped.n))) return;
        set_owned(&p->run_error, NULL);
        if (l->has_error && !(p->run_error = dupn(l->error.s, l->error.n))) return;
        set_owned(&p->run_short, tx(c, fill_last_run, c));
        const char *s = tx(c, fill_done, &(done_arg){l, p});
        set_owned(&p->run_phase, s);
        const char *id = p->has_run_pl ? p->run_pl.id : "";
        size_t k = strlen(id) + 3;
        char *head = arena_alloc(&c->scratch, strlen("prepare: playlist ") + k);
        if (head) {
            head[0] = 0;
            strcat(head, "prepare: playlist ");
            strcat(head, id);
            strcat(head, ": ");
            log_cat(c, head, nopaths(c, s));
        }
        break;
    }
    default: break;
    }
    prep_update(c);
}

typedef struct { char *title, *body; } notify_msg;
static void notify_drop(void *p) {
    notify_msg *m = p;
    free(m->title);
    free(m->body);
}
static void notify_work(core_life *life, void *payload) {   /* notify(title:body:): never a dialog */
    notify_msg *m = payload;
    if (life->host.step && !(life->native & NATIVE_STEP)) {
        arena a = {0};
        core_facts f;
        facts_init(&f, &a);
        const char *args[2] = {m->title, m->body};
        life->host.step(life->host.ctx, CORE_STEP_NOTIFY, args, 2, &f);
        arena_free(&a);
    }
    notify_drop(m);
}

typedef struct { const prep_state *p; int32_t status; } title_arg;
static void fill_title(tbuf *b, const void *a) {
    const title_arg *t = a;
    const prep_state *p = t->p;
    pstr st = {p->run_stopped ? p->run_stopped : "", p->run_stopped ? strlen(p->run_stopped) : 0};
    w_run_end_title(b, (pstr){p->run_pl.name, strlen(p->run_pl.name)}, t->status, p->run_got_done, p->run_stopped ? &st : NULL,
                    p->run_done, p->run_skipped, p->run_error != NULL, p->run_partial);
}

/* runEnded (PrepareView.swift:848) */
static void run_end(core *c, hrun *h) {
    prep_state *p = &c->p;
    const char *err = h->err ? repaired(c, h->err) : "";
    if (!err) err = "";
    if (h->status != 0 && *err) {
        set_owned(&p->run_phase, err);
        log_cat(c, "prepare: the helper stopped: ", nopaths(c, err));
    } else if (!p->run_got_done) {
        set_owned(&p->run_phase, text0(c, T_RUN_UNEXPECTED));
        char line[96];
        snprintf(line, sizeof line, "prepare: the helper ended without its last line (status %d)", (int)h->status);
        core_log(c->life, line);
    }
    /* the notification's title, made before the run's playlist goes */
    const char *title = p->has_run_pl ? tx(c, fill_title, &(title_arg){p, h->status}) : NULL;
    p->run = 0;
    p->running = p->paused = 0;
    p->run_fraction = 0;
    p->run_row = -1;
    int had = p->has_run_pl;
    prep_pl pl = p->run_pl;
    memset(&p->run_pl, 0, sizeof p->run_pl);
    p->has_run_pl = 0;
    run_flags(c);
    set_owned(&p->last_run, p->run_got_done ? p->run_short : p->run_phase);
    if (had) status_set_forget(c, pl.id);   /* what it said before the run isn't so now */
    /* what is ready now; the run's summary stays below it. Not over rekordbox: a new track may
     * bring up a privacy prompt. Coming back to the app checks. */
    if (view_in_front(c, CORE_TAB_PREPARE)) reload_list(c, 0);
    else {
        set_est(c, NULL);
        cells_state(c);
        prep_update(c);
    }
    status_reload_if_showing(c);
    uint32_t ff;
    CALLBACK(c, ff = c->life->host.front_facts(c->life->host.ctx));
    if (!(ff & CORE_FRONT_ACTIVE) && had && title) {
        notify_msg *m = calloc(1, sizeof *m);
        if (m && (m->title = dupn(title, strlen(title))) && (m->body = dupn(p->run_phase ? p->run_phase : "", p->run_phase ? strlen(p->run_phase) : 0)))
            core_post_work(c->life, CORE_WORK_FACT, notify_work, m, notify_drop);
        else if (m) {
            notify_drop(m);
            free(m);
        }
    }
    pl_clear(&pl);
    /* the quit flow or an action waiting for the run's end (stopAndWait) reads prep_running */
}

static void run_signal(core *c, core_helper_sig sig) {
    if (c->life->host.helper_signal && !(c->life->native & NATIVE_HELPER))
        CALLBACK(c, c->life->host.helper_signal(c->life->host.ctx, c->p.run, sig));
}

static void pause_action(core *c) {   /* pauseAction */
    prep_state *p = &c->p;
    if (!p->running) return;
    if (p->paused) {
        run_signal(c, CORE_HELPER_RESUME);
        p->paused = 0;
        core_log(c->life, "prepare: resumed");
    } else {
        run_signal(c, CORE_HELPER_PAUSE);
        p->paused = 1;
        core_log(c->life, "prepare: paused");
    }
    run_flags(c);
    prep_update(c);
}

void prep_stop(core *c) {   /* PrepareRun.stop: SIGTERM, then SIGCONT when it was paused */
    prep_state *p = &c->p;
    if (!p->running) return;
    run_signal(c, CORE_HELPER_STOP);
    if (p->paused) {
        run_signal(c, CORE_HELPER_RESUME);
        p->paused = 0;
        run_flags(c);
    }
}

static void stop_action(core *c) {   /* stopAction */
    prep_stop(c);
    set_owned(&c->p.run_phase, text0(c, T_RUN_STOPPING));
    core_log(c->life, "prepare: stop asked");
    prep_update(c);
}

/* startRun(p, prepareBaseArgs(facts) + runArgs(p.id, until: untilArg(), first: first)) on the
 * selected playlist; first < 0: nil */
static void run_selected(core *c, const prep_now *n, int64_t first) {
    prep_state *p = &c->p;
    const char *argv[32];
    int32_t k = base_args(c, n, argv);
    char until[24], nfirst[24];
    until_arg(c, until);
    argv[k++] = "--until";
    argv[k++] = until;
    argv[k++] = "--keep";
    argv[k++] = p->sel.id;
    argv[k++] = "--playlist";
    argv[k++] = p->sel.id;
    if (first >= 0) {
        snprintf(nfirst, sizeof nfirst, "%lld", (long long)first);
        argv[k++] = "--first";
        argv[k++] = nfirst;
    }
    const char **kept = argv_keep(c, argv, k);
    prep_pl pl;
    if (!kept || pl_copy(&pl, &p->sel)) return;
    start_run(c, &pl, kept, k, first >= 0);
    pl_clear(&pl);
}

/* a dialog of prepareAction's couldn't be asked (out of memory, or another flow holds the flow
 * thread): the click does nothing, as one the modal session dropped */
static void ask(core *c, int kind, const prep_estimate *e, const char *folder) {
    prep_state *p = &c->p;
    if (flows_prepare_ask(c, kind, p->sel.id, p->sel.name, e, folder, p->est_seq))
        core_log(c->life, "core: a click while an action runs is ignored");
}

/* prepareFacts() now (into n), and prepareAction's guard on them: no problem, a folder to save in,
 * and the limit the estimate was made with (`max_gb`). 0: the limit or the location may have
 * changed since the estimate: never a run on what it said */
static int facts_still_hold(core *c, prep_now *n, double max_gb) {
    prep_facts_now(c, SHA_WAIT_NONE, n);
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN && !n->pf.rekordbox_installed)
        core_log(c->life, "prepare: no verified current rekordbox, the run isn't started");
#endif
    char *buf = arena_alloc(&c->scratch, 8192);
    int32_t problems = 1;
    if (buf) {
        tbuf b;
        tb_init(&b, c->os, buf, 8192);
        problems = w_prepare_problems(&b, &n->pf);
    }
    double d = n->max_gb - max_gb;
    return !problems && n->cache_folder && (d < 0 ? -d : d) <= 0.001 * n->max_gb;
}

/* prepareAction (PrepareView.swift:750). Its dialogs (a refused playlist, "doesn't fit", "Not
 * enough free space") are a flow's (flows.c, flows_prepare_ask); the run starts from main once
 * the flow's answer comes (prep_ask_go). */
static void prepare_action(core *c) {
    prep_state *p = &c->p;
    if (!p->has_sel || !p->est || p->running || c->action_running || c->fl.prep_reserved) return;   /* SO1: a flow's preflight holds it */
    const prep_estimate *e = &p->est->e;
    if (p->load_error || !(e->has_refused || e->prepare + e->ready > 0)) return;
    if (p->refreshing) {   /* the check under way says what to do: shown */
        p->held = 1;
        p->estimating = 1;
        cells_state(c);
        prep_update(c);
        return;
    }
    if (e->has_refused) {
        ask(c, PREP_ASK_REFUSED, e, NULL);
        return;
    }
    prep_now n;
    if (!facts_still_hold(c, &n, e->max_gb)) {
        prep_check(c, 1, 0);
        return;
    }
    if (!e->fits) {
        ask(c, PREP_ASK_FIT, e, n.cache_folder);
        return;
    }
    if (!e->disk_ok) {
        ask(c, PREP_ASK_SPACE, e, NULL);
        return;
    }
    if (p->estimating || p->refreshing) {
        prep_check(c, 1, 0);
        return;
    }
    run_selected(c, &n, -1);
}

/* prepareAction after its dialog, on main: Prepare (first < 0), Prepare the First N Tracks, or
 * the limit raised (raised > 0: the limit written). The app may have been left during a sheet, and
 * come back to a new check: then the playlist is checked again, never a run on what the dialog
 * said. The facts are read again and prepareAction's guard repeated (VM4): 1.3.0 runs on the facts
 * it checked before the sheet (read again after Raise), so config.ini or the location changed
 * under the sheet is checked again here rather than run on. */
void prep_ask_go(core *c, uint64_t seq, const char *id, int64_t first, double raised) {
    prep_state *p = &c->p;
    prep_now n;
    if (p->estimating || p->refreshing || !p->est || p->est_seq != seq || !p->has_sel || strcmp(p->sel.id, id) ||
        !facts_still_hold(c, &n, raised > 0 ? raised : p->est->e.max_gb)) {
        prep_check(c, 1, 0);   /* recheck() */
        return;
    }
    run_selected(c, &n, first);
}

/* the Prepare tab's buttons and menu items (PrepareView.swift, main.swift) */
static int is_id(const char *id, size_t n, const char *name) { return strlen(name) == n && !memcmp(id, name, n); }

int prep_event(core *c, const char *id, size_t n, int menu) {
    prep_state *p = &c->p;
    if (is_id(id, n, "prepare")) {   /* the button: only when it is on (AppKit doesn't send a disabled button's click) */
        if (model_get_i64(&c->m, F_PREP_PREPARE_ENABLED)) prepare_action(c);
    } else if (is_id(id, n, "check_again")) {
        if (menu) {   /* checkAgainMenu: the tab checks as it comes to the front */
            int shown = model_get_i64(&c->m, F_TAB) == CORE_TAB_PREPARE;
            view_show_tab(c, CORE_TAB_PREPARE);
            if (shown) reload_list(c, 0);
        } else if (!p->running && p->has_sel) reload_list(c, 0);   /* checkAgain() */
    } else if (is_id(id, n, "pause")) pause_action(c);
    else if (is_id(id, n, "stop")) {
        if (p->running) stop_action(c);
    } else if (is_id(id, n, "prepare_playlist")) {   /* preparePlaylistMenu: the tab, the outline focused */
        view_show_tab(c, CORE_TAB_PREPARE);
        model_set_i64(&c->m, F_PREP_FOCUS_SEQ, model_get_i64(&c->m, F_PREP_FOCUS_SEQ) + 1);
        core_mark_changed(c);
    } else return 0;
    return 1;
}

/* ---------------------------------------------------------------- life */

void prep_init(core *c) {
    memset(&c->p, 0, sizeof c->p);
    c->p.run_row = c->p.cells_run_row = -1;
}

void prep_free(core *c) {
    prep_state *p = &c->p;
    for (int32_t i = 0; i < p->npl; i++) pl_clear(&p->pls[i]);
    free(p->pls);
    free(p->row_pl);
    free(p->pl_row);
    free(p->pl_parent);
    free(p->load_error);
    for (int32_t i = 0; i < p->nclosed; i++) free(p->closed[i]);
    free(p->closed);
    if (p->has_sel) pl_clear(&p->sel);
    est_free(p->est);
    lines_free(p->lines, p->nlines);
    free(p->problems);
    if (p->has_run_pl) pl_clear(&p->run_pl);
    free(p->run_phase);
    free(p->last_run);
    free(p->run_short);
    free(p->run_error);
    free(p->run_stopped);
    memset(p, 0, sizeof *p);
}
