/* The view model's app section (M4, mac-core §3.4): Controller.swift's refresh() (the lights, the
 * buttons and their titles, the status line between actions), busy() and prepareChanged(quiet:),
 * the location's onChange, and the model's sha256 memo (Rekordbox.swift currentModelSHA). Every
 * decision is today's, ported line by line; the words come from M5's pure functions (pure.h).
 *
 * The facts are the host's cheap facts (host->facts, facts.def), read on main on every refresh,
 * never cached. Only the model's sha256 is memoised (by path, size and mtime), worked out by a
 * slow fact on a worker as today's is in the background. */
#include "internal.h"
#include <stdio.h>

/* ---------------------------------------------------------------- facts */

static int vf_flag(const core_facts *f, core_fact id) { return f->set[id] && f->i[id] != 0; }
static int64_t vf_i64(const core_facts *f, core_fact id, int64_t none) { return f->set[id] ? f->i[id] : none; }
static const char *vf_str(const core_facts *f, core_fact id) { return f->set[id] && f->s[id] ? f->s[id] : ""; }
static const char *vf_str_or_null(const core_facts *f, core_fact id) { return f->set[id] && f->s[id] ? f->s[id] : NULL; }

static const core_fact loc_ids[LOC_FACTS] = {
    FACT_LOC_MEASURED, FACT_LOC_KIND, FACT_LOC_PROBE, FACT_LOC_STATUS_DRIVE, FACT_LOC_DRIVE, FACT_LOC_BRIDGE_STATE,
    FACT_LOC_BRIDGE_REASON, FACT_LOC_BRIDGE_DRIVE, FACT_REKORDBOX_RUNNING, FACT_LOC_PAUSED_FLOOR, FACT_LOC_IN_USE};

void view_read_facts(core *c, core_facts *f, uint32_t parts) {
    facts_init(f, &c->scratch);
    f->parts = parts;
    CALLBACK(c, c->life->host.facts(c->life->host.ctx, f));
    view_loc_overlay(c, f);
}

void view_loc_overlay(core *c, core_facts *f) {
    const view_loc_memo *m = &c->v.loc;
    if (!m->known) return;
    for (int k = 0; k < LOC_FACTS; k++) {   /* the location as the core's memo has it, copied into the sheet's arena */
        core_fact id = loc_ids[k];
        size_t n = m->s[k] ? strlen(m->s[k]) : 0;
        const char *v = m->s[k] && f->a ? arena_strdup(f->a, m->s[k], n) : NULL;
        f->set[id] = m->set[k] && (!m->s[k] || v);
        f->i[id] = m->i[k];
        f->s[id] = v;
        f->len[id] = v ? n : 0;
    }
#ifdef CORE_WIN_POLICY
    if (m->known == VIEW_LOC_STALE) core_fact_flag(f, FACT_LOC_MEASURED, 0);
#endif
}

/* a fact holding one item a line ("" lines dropped), as an array in the scratch */
int32_t view_split_lines(arena *a, const char *s, const char ***out) {
    *out = NULL;
    if (!s) return 0;
    int32_t n = 0;
    for (const char *p = s; *p;) {
        const char *e = strchr(p, '\n');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        n += k > 0;
        p += k + (e != NULL);
    }
    if (!n) return 0;
    const char **v = arena_alloc(a, (size_t)n * sizeof *v);
    if (!v) return 0;
    int32_t i = 0;
    for (const char *p = s; *p && i < n;) {
        const char *e = strchr(p, '\n');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k > 0 && !(v[i++] = arena_strdup(a, p, k))) return 0;
        p += k + (e != NULL);
    }
    *out = v;
    return n;
}

/* a text built into the scratch; "" when it couldn't be (a column not written yet, out of memory) */
#define TEXT_CAP 4096
static char *scratch_buf(core *c) {
    char *b = arena_alloc(&c->scratch, TEXT_CAP);
    if (b) b[0] = 0;
    return b;
}

static const char *built(tbuf *b) { return b->err ? "" : b->p; }

/* LocationSnapshot.lightTip (LocationMonitor.swift:38): locationLight of the snapshot's status. */
const char *view_location_tip(core *c, const core_facts *f) {
#ifdef CORE_WIN_POLICY
    if ((c->os == CORE_OS_WIN && c->v.loc.known == VIEW_LOC_STALE) || win_location_unchecked(c->os, f))
        return core_text_template(c->os, T_CHECKING_WHERE);
#endif
    if (!vf_flag(f, FACT_LOC_MEASURED)) return NULL;
    loc_status st = {(int32_t)vf_i64(f, FACT_LOC_KIND, LOC_THIS_MAC), (int32_t)vf_i64(f, FACT_LOC_PROBE, PROBE_AVAILABLE),
                     vf_str_or_null(f, FACT_LOC_STATUS_DRIVE)};
    loc_bridge br = {vf_str_or_null(f, FACT_LOC_BRIDGE_STATE), vf_str_or_null(f, FACT_LOC_BRIDGE_REASON),
                     vf_str_or_null(f, FACT_LOC_BRIDGE_DRIVE)};
    char *buf = scratch_buf(c);
    if (!buf) return "";
    tbuf b;
    tb_init(&b, c->os, buf, TEXT_CAP);
    if (!location_light(&b, &st, br.state ? &br : NULL, vf_flag(f, FACT_REKORDBOX_RUNNING))) return NULL;
    return built(&b);
}

/* cacheLight (Controller.swift:402) with refresh's and prepareFacts' inputs. waiter: who is told
 * when the model's sha256 asked here becomes known (SHA_WAIT_NONE: nobody) */
int32_t view_cache_light(core *c, const core_facts *f, int waiter, const char **tip) {
    int plus = vf_flag(f, FACT_STEMS_PLUS_PRESENT), cache = vf_flag(f, FACT_STEMS_CACHE_PRESENT);
    int own = cache && !plus;   /* with rekordbox's own model: whether the bridge saves its stems */
    cache_light_in in;
    memset(&in, 0, sizeof in);
    in.installed = cache;
    in.plus_on = plus;
    in.chosen = vf_flag(f, FACT_CHOSEN_CACHE);
    in.rekordbox_model = vf_flag(f, FACT_CONFIG_RB_MODEL);
    if (own) {
        const char *models = vf_str_or_null(f, FACT_BRIDGE_MODELS);
        const char **list;
        in.has_bridge_models = models != NULL;
        in.n_bridge_models = view_split_lines(&c->scratch, models, &list);
        in.bridge_models = list;
        in.model = view_model_sha(c, f, waiter);
    }
    in.engine = vf_str(f, FACT_ENGINE);
    int64_t paused = vf_i64(f, FACT_LOC_PAUSED_FLOOR, -1);
    if (cache && vf_flag(f, FACT_CONFIG_ENABLED) && paused >= 0) {   /* only while it's on in config.ini */
        in.has_paused_floor = 1;
        in.paused_floor = paused;
    }
    in.location = view_location_tip(c, f);
    in.drive = vf_str_or_null(f, FACT_LOC_DRIVE);
#ifdef CORE_WIN_POLICY
    in.win_r1 = c->os == CORE_OS_WIN ? c->v.win_r1 : R1_OK;   /* R1 first (C19): refresh's and prepareFacts' light */
#endif
    char *buf = scratch_buf(c);
    tbuf b;
    tb_init(&b, c->os, buf ? buf : (char[1]){0}, buf ? TEXT_CAP : 1);
    int32_t look;
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN && c->v.win_r1_bridge_repair.state == WIN_ROW_REPAIR) {
        w_win_row_reason(&b, &c->v.win_r1_bridge_repair);
        look = LOOK_LIMITED;
    } else
#endif
        look = light_cache(&b, &in);
    *tip = buf ? built(&b) : "";
    return look;
}

/* ---------------------------------------------------------------- the model's sha256 (currentModelSHA) */

typedef struct { int64_t size, mtime; int ok; char sha[72]; char path[]; } sha_result;
typedef struct { sha_result *r; core_job *done; } sha_work;   /* done: r's job, reserved (E2) */

static void sha_result_job(core *c, void *payload);

/* a worker: the slow fact, then its result on main */
static void sha_work_run(core_life *life, void *payload) {
    sha_work *w = payload;
    sha_result *r = w->r;
    size_t n = strlen(r->path);
    if (life->host.slow_fact && !(life->native & NATIVE_SLOW_FACT)) {
        arena a = {0};
        core_facts f;
        facts_init(&f, &a);
        if (life->host.slow_fact(life->host.ctx, CORE_SLOW_MODEL_SHA, r->path, n, &f) == 0 && f.set[FACT_SHA256] && f.s[FACT_SHA256] &&
            f.len[FACT_SHA256] > 0 && f.len[FACT_SHA256] < sizeof r->sha) {
            memcpy(r->sha, f.s[FACT_SHA256], f.len[FACT_SHA256]);
            r->ok = 1;
        }
        arena_free(&a);
    }
    job_post(life, w->done);   /* reserved: main always hears, so `working` always ends */
}
static void sha_work_drop(void *payload) { core_job_drop(((sha_work *)payload)->done); }

static int same_key(const char *p, int64_t s, int64_t m, const char *q, int64_t s2, int64_t m2) {
    return p && q && s == s2 && m == m2 && !strcmp(p, q);
}

const char *view_model_sha(core *c, const core_facts *f, int waiter) {
    view_sha_memo *m = &c->v.sha;
    const char *path = c->paths[P_MODEL];
    if (!path || !f->set[FACT_MODEL_SIZE]) return NULL;   /* no such file: nil */
    int64_t size = f->i[FACT_MODEL_SIZE], mtime = vf_i64(f, FACT_MODEL_MTIME, 0);
    if (m->known && same_key(m->path, m->size, m->mtime, path, size, mtime)) return m->sha;
    if (m->working && same_key(m->work_path, m->work_size, m->work_mtime, path, size, mtime)) return NULL;
    /* not known, not under way: worked out in the background (a newer key replaces an older
     * work's: its result is kept only for its own key) */
    size_t n = strlen(path);
    sha_work *w = malloc(sizeof *w);
    char *wp = strdup(path);
    sha_result *r = calloc(1, sizeof *r + n + 1);
    core_job *done = r ? job_reserve(c->life, sha_result_job, r, NULL) : NULL;
    if (!w || !wp || !done) {
        free(w);
        free(wp);
        if (done) core_job_drop(done); else free(r);
        return NULL;
    }
    r->size = size;
    r->mtime = mtime;
    memcpy(r->path, path, n + 1);
    w->r = r;
    w->done = done;
    free(m->work_path);
    m->work_path = wp;
    m->work_size = size;
    m->work_mtime = mtime;
    m->working = 1;
    m->waiter = waiter;
    if (core_post_work(c->life, CORE_WORK_FACT, sha_work_run, w, sha_work_drop)) m->working = 0;   /* not posted: asked again next time */
    return NULL;
}

static void sha_result_job(core *c, void *payload) {
    sha_result *r = payload;
    view_sha_memo *m = &c->v.sha;
    int waiter = SHA_WAIT_NONE;
    if (m->working && same_key(m->work_path, m->work_size, m->work_mtime, r->path, r->size, r->mtime)) {
        waiter = m->waiter;
        m->working = 0;
    }
    if (!r->ok) return;   /* unreadable: nil, nobody told (today's) */
    char *p = strdup(r->path);
    if (!p) return;
    free(m->path);
    m->path = p;
    m->size = r->size;
    m->mtime = r->mtime;
    memcpy(m->sha, r->sha, sizeof m->sha);
    m->known = 1;
    switch (waiter) {
    case SHA_WAIT_REFRESH: view_refresh(c); break;
    case SHA_WAIT_PREP_UPDATE: prep_sha_known(c, 0); break;
    case SHA_WAIT_PREP_CHECK: prep_sha_known(c, 1); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- R1 (Windows, review WR1) */

#ifdef CORE_WIN_POLICY
/* R1's check (app.md §5.2): CORE_SLOW_R1 on a worker, its key "" (the host's own discovery of the
 * current version folder), at start, at activation, on the watcher's notifications and when an
 * action ends. Each ask moves r1_gen; an answer for an older one is dropped. No answer (no
 * folder, unreadable, an unknown value) is R1_OK: nothing to put back. A changed state refreshes
 * once, and the Prepare tab checks again quietly when it exists (CW2); the same state does
 * nothing, except the first answer after an action, which refreshes once to give Put Back back
 * (CW3: until then it stays off, so a restore that worked can't be clicked again on the old
 * line), and an answer whose FACT_R1_DISMISSED moved, which refreshes once for Don't Ask Again's
 * button (N10). A refresh never asks, so asks and refreshes never feed each other. */
typedef struct {
    uint64_t gen;
    int answered, dismissed;
    int64_t state;
    int rows_set, nrows;
    win_row rows[WIN_ROWS_MAX];
} r1_result;
typedef struct { r1_result *r; core_job *done; } r1_work;

static void r1_result_job(core *c, void *payload);
static void view_refresh_r1(core *c, const r1_result *r);

static void r1_work_run(core_life *life, void *payload) {
    r1_work *w = payload;
    if (life->host.slow_fact && !(life->native & NATIVE_SLOW_FACT)) {
        arena a = {0};
        core_facts f;
        facts_init(&f, &a);
        if (life->host.slow_fact(life->host.ctx, CORE_SLOW_R1, "", 0, &f) == 0 && f.set[FACT_R1_STATE]) {
            w->r->state = f.i[FACT_R1_STATE];
            w->r->dismissed = f.set[FACT_R1_DISMISSED] && f.i[FACT_R1_DISMISSED];   /* N10 */
            w->r->answered = 1;
            if (f.set[FACT_WIN_RB_ROWS]) {
                w->r->rows_set = 1;
                w->r->nrows = win_rows_parse(f.s[FACT_WIN_RB_ROWS], f.len[FACT_WIN_RB_ROWS], w->r->rows, WIN_ROWS_MAX);
            }
        }
        arena_free(&a);
    }
    job_post(life, w->done);   /* reserved: main always hears */
}
static void r1_work_drop(void *payload) { core_job_drop(((r1_work *)payload)->done); }

void view_r1_check(core *c) {
    if (c->os != CORE_OS_WIN || c->role != CORE_ROLE_APP) return;
    uint64_t gen = ++c->v.r1_gen;   /* first: an older ask's answer can't release CW3's hold */
    r1_work *w = malloc(sizeof *w);
    r1_result *r = calloc(1, sizeof *r);
    core_job *done = r ? job_reserve(c->life, r1_result_job, r, NULL) : NULL;
    if (!w || !done) {   /* no answer will come: Put Back stays off (CW3) until the next ask answers */
        free(w);
        if (done) core_job_drop(done); else free(r);
        return;
    }
    r->gen = gen;
    w->r = r;
    w->done = done;
    core_post_work(c->life, CORE_WORK_FACT, r1_work_run, w, r1_work_drop);   /* refused: the same, till the next ask */
}

static void r1_result_job(core *c, void *payload) {
    const r1_result *r = payload;
    if (r->gen != c->v.r1_gen) return;   /* a newer ask is under way (it clears r1_after_action) */
    int64_t s = r->answered && r->state >= R1_OK && r->state <= R1_ORT_MISSING ? r->state : R1_OK;
    int d = s != R1_OK && r->dismissed;   /* N10: Don't Ask Again's button goes once this version is dismissed */
    int changed = s != c->v.win_r1, pending = c->v.r1_after_action || d != c->v.win_r1_dismissed;
    c->v.r1_after_action = 0;
    if (!changed && !pending && !r->rows_set) return;
    c->v.win_r1 = s;
    c->v.win_r1_dismissed = d;
    view_refresh_r1(c, r);
    if (changed && !view_tabs_hidden(c)) view_prepare_changed(c, 1);   /* Prepare's problems (CW2) */
}
#endif

/* ---------------------------------------------------------------- refresh */

static void set_light(core *c, core_field look_f, core_field tip_f, core_field ax_f, int32_t look, const char *tip) {
    model_set_i64(&c->m, look_f, look);
    model_set_str(&c->m, tip_f, tip, CORE_NTS);
    char ax[256];
    tbuf b;
    tb_init(&b, c->os, ax, sizeof ax);
    TB_TEXT0(&b, light_ax_text(look));
    model_set_str(&c->m, ax_f, built(&b), CORE_NTS);
}

static void show_buttons(core *c, const view_buttons *v) {
    model *md = &c->m;
    c->v.shown = *v;
    model_set_i64(md, F_INSTALL_PLUS_ENABLED, v->install_plus);
    model_set_i64(md, F_INSTALL_CACHE_ENABLED, v->install_cache);
    model_set_i64(md, F_REINSTALL_ENABLED, v->reinstall);
    model_set_i64(md, F_REINSTALL_PLUS_ENABLED, v->reinstall_plus);
    model_set_i64(md, F_REINSTALL_CACHE_ENABLED, v->reinstall_cache);
    model_set_i64(md, F_SETTINGS_ENABLED, v->settings);
    model_set_i64(md, F_UNINSTALL_PLUS_ENABLED, v->uninstall_plus);
    model_set_i64(md, F_UNINSTALL_CACHE_ENABLED, v->uninstall_cache);
    model_set_i64(md, F_UNINSTALL_ALL_ENABLED, v->uninstall_all);
}

/* M6's confirmation guard (API-V1-NOTES §8, F8): the status gen moves only when what a
 * confirmation describes changed: the installed features, chosen(), the location config or its
 * availability, rekordbox's version. Not on activation alone, a size or the clock. */
static void status_gen_track(core *c, const core_facts *f) {
    char key[2048];
    tbuf b;
    tb_init(&b, c->os, key, sizeof key);
    const core_fact flags[] = {FACT_STEMS_PLUS_PRESENT, FACT_STEMS_CACHE_PRESENT, FACT_CHOSEN_MODEL, FACT_CHOSEN_CACHE, FACT_LOC_MEASURED};
    for (size_t i = 0; i < sizeof flags / sizeof *flags; i++) tb_puts(&b, vf_flag(f, flags[i]) ? "1" : "0");
    const core_fact nums[] = {FACT_CONFIG_LOCATION, FACT_LOC_KIND, FACT_LOC_PROBE};
    for (size_t i = 0; i < sizeof nums / sizeof *nums; i++) {
        tb_puts(&b, " ");
        tb_i64(&b, vf_i64(f, nums[i], -1));
    }
    const core_fact strs[] = {FACT_REKORDBOX_VERSION, FACT_CONFIG_DIR, FACT_LOC_IN_USE, FACT_CONFIG_UNREADABLE};
    for (size_t i = 0; i < sizeof strs / sizeof *strs; i++) {
        tb_puts(&b, "\n");
        tb_puts(&b, vf_str(f, strs[i]));
    }
    if (b.err) return;
    if (c->v.status_key && !strcmp(c->v.status_key, key)) return;
    char *k = strdup(key);
    if (!k) return;
    if (c->v.status_key) c->gen[DOM_STATUS]++;   /* the first look sets the key */
    free(c->v.status_key);
    c->v.status_key = k;
}

#ifdef CORE_WIN_POLICY
static void win_rows_fields(core *c, const win_row *rows, int n, const char *summary, const char *location,
                            int admin) {
    model *m = &c->m;
    int shown = n >= 2;
    int count = shown ? n : 0;
    table_obj *table = m->tab[T_WIN_RB_VERSIONS];
    int reset = table->n != count;
    int version_col = core_column_meta[C_WIN_RB_VERSION].index;
    for (int i = 0; i < count && !reset; i++) {
        const cell *v = &table->rows[i]->cells[version_col];
        reset = !v->len || strcmp(v->v.s, rows[i].version);
    }
    if (reset && model_table_reset(m, T_WIN_RB_VERSIONS, count)) return;
    for (int i = 0; i < count; i++) {
        const win_row *r = &rows[i];
        char text[192], why[512];
        tbuf b;
        tb_init(&b, CORE_OS_WIN, text, sizeof text);
        w_win_row_text(&b, r, 0);
        model_cell_str(m, C_WIN_RB_TEXT, i, built(&b), CORE_NTS);
        tb_init(&b, CORE_OS_WIN, why, sizeof why);
        if (r->state == WIN_ROW_ON && location) tb_puts(&b, location);
        else if (r->state == WIN_ROW_READY && !admin) TB_TEXT0(&b, T_NEEDS_ADMIN);
        else w_win_row_reason(&b, r);
        model_cell_str(m, C_WIN_RB_TIP, i, built(&b), CORE_NTS);
        int look = r->state == WIN_ROW_ON ? (location ? LOOK_LIMITED : LOOK_ON)
                 : r->state == WIN_ROW_REPAIR ? LOOK_LIMITED : LOOK_OFF;
        model_cell_i64(m, C_WIN_RB_LOOK, i, look);
        model_cell_str(m, C_WIN_RB_VERSION, i, r->version, CORE_NTS);
    }
    model_set_str(m, F_WIN_CACHE_SUMMARY, summary, CORE_NTS);
    model_set_i64(m, F_WIN_RB_ROWS_SHOWN, shown);
}

typedef struct {
    int count, on, ready, repair, unchecked, dismiss;
    const win_row *first_problem, *first_repair;
    char summary[128], problem[TEXT_CAP];
    char dismiss_versions[256];   /* F_WIN_R1_DISMISS_VERSIONS: the versions Don't Ask Again covers */
} win_coverage;

/* a version added to Don't Ask Again's list (comma-separated): 0, or -1 when it doesn't fit */
static int win_dismiss_add(win_coverage *w, const char *version, size_t n) {
    size_t at = strlen(w->dismiss_versions);
    if (!n || at + n + 2 > sizeof w->dismiss_versions) return -1;
    if (at) w->dismiss_versions[at++] = ',';
    memcpy(w->dismiss_versions + at, version, n);
    w->dismiss_versions[at + n] = 0;
    w->dismiss++;
    return 0;
}

static win_coverage win_coverage_read(const core_facts *f, const win_row *rows, int n) {
    win_coverage w = {.count = n, .unchecked = n < 0 || vf_flag(f, FACT_LIB_UNCHECKED)};
    for (int i = 0; i < n; i++) {
        const win_row *r = &rows[i];
        w.on += r->state == WIN_ROW_ON;
        w.ready += r->state == WIN_ROW_READY;
        w.unchecked |= r->why == WIN_WHY_NO_ANSWER;
        if (r->state == WIN_ROW_REPAIR) {
            w.repair++;
            if (!w.first_repair) w.first_repair = r;
        }
        /* Don't Ask Again covers every version R1 asks about and isn't dismissed in: with two or
         * more rows the repair rows shown, with one the row R1's line is about. WIN_R1_DISMISS
         * writes exactly these versions, as the click sends them back */
        if (w.dismiss >= 0 && r->r1 != R1_OK && !r->r1_dismissed && (n < 2 || r->state == WIN_ROW_REPAIR)
            && win_dismiss_add(&w, r->version, strlen(r->version)))
            w.dismiss_versions[0] = 0, w.dismiss = -1;   /* more than the list holds: no button */
        if (!w.first_problem && r->state != WIN_ROW_ON) w.first_problem = r;
    }
    if (n <= 0 && w.dismiss == 0) {   /* no rows: R1's line is the current version folder's */
        const char *current = vf_str(f, FACT_REKORDBOX_VERSION);
        size_t len = 0;
        for (int dots = 0; current[len] && (current[len] != '.' || ++dots < 3); len++) {}
        win_dismiss_add(&w, current, len);
    }
    tbuf b;
    if (n >= 2) {
        tb_init(&b, CORE_OS_WIN, w.summary, sizeof w.summary);
        w_win_cache_summary(&b, w.on, n);
    }
    tb_init(&b, CORE_OS_WIN, w.problem, sizeof w.problem);
    if (w.unchecked && (n < 0 || n >= 2)) {
        if (w.first_problem && w.first_problem->why == WIN_WHY_NO_ANSWER) w_win_row_reason(&b, w.first_problem);
        else TB_TEXT0(&b, T_WIN_CACHE_NOT_CHECKED);
    } else if (n >= 2 && w.first_problem) {
        if (w.first_problem->state != WIN_ROW_READY) w_win_row_reason(&b, w.first_problem);
        else if (!vf_flag(f, FACT_USER_IS_ADMIN)) TB_TEXT0(&b, T_NEEDS_ADMIN);
        else {
            const char *ready[WIN_ROWS_MAX];
            int count = 0;
            for (int i = 0; i < n; i++)
                if (rows[i].state == WIN_ROW_READY) ready[count++] = rows[i].version;
            char versions[WIN_ROWS_MAX * (WIN_VERSION_CAP + sizeof " and " - 1)];
            tbuf list;
            tb_init(&list, CORE_OS_WIN, versions, sizeof versions);
            w_win_version_list(&list, ready, count);
            core_slot version = SLOT_S(rekordbox, versions);
            tb_text(&b, T_WIN_ROW_READY_TIP, &version, 1);
        }
    }
    return w;
}

static void win_coverage_r1(core *c, const win_coverage *w) {
    int busy = c->action_running || c->v.r1_after_action;
    const win_row *repair = w->first_repair;
    /* These inventory reasons identify our bridge even when R1 reports a foreign runtime. */
    int our_bridge = repair && (repair->why == WIN_WHY_NOT_RECORDED || repair->why == WIN_WHY_DELISTED_BRIDGE);
    c->v.win_r1_bridge_repair = our_bridge ? *repair : (win_row){0};
    if (w->count >= 2 || (repair && (repair->r1 == R1_OK || our_bridge))) {
        char line[512];
        tbuf b;
        tb_init(&b, CORE_OS_WIN, line, sizeof line);
        if (repair) w_win_row_reason(&b, repair);
        model_set_str(&c->m, F_WIN_R1_LINE, built(&b), CORE_NTS);
        model_set_i64(&c->m, F_WIN_RESTORE_VISIBLE, w->repair > 0);
        model_set_i64(&c->m, F_WIN_RESTORE_ENABLED, w->repair > 0 && !busy);
        model_set_i64(&c->m, F_WIN_R1_DISMISS_VISIBLE, w->dismiss > 0);
        model_set_i64(&c->m, F_WIN_R1_DISMISS_ENABLED, w->dismiss > 0 && !busy);
        snprintf(c->v.win_r1_dismiss_versions, sizeof c->v.win_r1_dismiss_versions, "%s", w->dismiss > 0 ? w->dismiss_versions : "");
    } else {
        win_r1_fields(&c->m, c->v.win_r1, c->v.win_r1_dismissed, busy);
        /* the compact layout: its button names the one version it asks about, or is hidden */
        int ask = model_get_i64(&c->m, F_WIN_R1_DISMISS_VISIBLE) && w->dismiss > 0;
        if (!ask) {
            model_set_i64(&c->m, F_WIN_R1_DISMISS_VISIBLE, 0);
            model_set_i64(&c->m, F_WIN_R1_DISMISS_ENABLED, 0);
        }
        snprintf(c->v.win_r1_dismiss_versions, sizeof c->v.win_r1_dismiss_versions, "%s", ask ? w->dismiss_versions : "");
    }
    model_set_str(&c->m, F_WIN_R1_DISMISS_VERSIONS, c->v.win_r1_dismiss_versions, CORE_NTS);
}

static int32_t win_coverage_light(core *c, const win_coverage *w, const win_row *rows, int32_t basic,
                                  const char *location, const char **tip) {
    if (w->count < 2) {
        if (w->count < 0) *tip = w->problem;
        return w->count < 0 && basic == LOOK_ON ? LOOK_LIMITED : basic;
    }
    char *buf = scratch_buf(c);
    if (buf) {
        tbuf b;
        tb_init(&b, CORE_OS_WIN, buf, TEXT_CAP);
        tb_puts(&b, w->summary);
        if (*w->problem) {
            tb_puts(&b, "\n");
            tb_puts(&b, w->problem);
        }
        for (int i = 0; i < w->count; i++) {
            tb_puts(&b, "\n");
            w_win_row_text(&b, &rows[i], 1);
        }
        const char *detail = location ? location : basic == LOOK_LIMITED && !w->repair ? *tip : "";
        if (*detail) {
            tb_puts(&b, "\n");
            tb_puts(&b, detail);
        }
        if (!b.err) *tip = buf;
    }
    if (!w->on) return LOOK_OFF;
    return w->on < w->count || w->unchecked || location || basic == LOOK_LIMITED ? LOOK_LIMITED : LOOK_ON;
}
#endif

/* Controller.refresh() (Controller.swift:222): every button's final state and label in one go */
#ifdef CORE_WIN_POLICY
static void view_refresh_r1(core *c, const r1_result *r) {
#else
void view_refresh(core *c) {
#endif
    if (c->role != CORE_ROLE_APP) return;
    core_facts f;
    view_read_facts(c, &f, CORE_FACTS_REFRESH);
    int plus = vf_flag(&f, FACT_STEMS_PLUS_PRESENT), cache = vf_flag(&f, FACT_STEMS_CACHE_PRESENT);
    const char *ctip;
#ifdef CORE_WIN_POLICY
    win_row rows[WIN_ROWS_MAX];
    const win_row *current = rows;
    const char *location = NULL;
    win_coverage coverage = {0};
    if (c->os == CORE_OS_WIN) {
        int n = win_rows_parse(vf_str(&f, FACT_WIN_RB_ROWS), f.len[FACT_WIN_RB_ROWS], rows, WIN_ROWS_MAX);
        /* A slow answer is used when the cheap sheet did not carry this fact. Otherwise the
         * cheap sheet is read after the answer, and may already describe a newer inventory. */
        if (!f.set[FACT_WIN_RB_ROWS] && r && r->rows_set) { n = r->nrows; current = r->rows; }
        coverage = win_coverage_read(&f, current, n);
        if (n >= 2 && coverage.on) location = view_location_tip(c, &f);
        win_rows_fields(c, current, n, coverage.summary, location, vf_flag(&f, FACT_USER_IS_ADMIN));
        win_coverage_r1(c, &coverage);
    }
#endif
    int32_t clook = view_cache_light(c, &f, SHA_WAIT_REFRESH, &ctip);
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN) clook = win_coverage_light(c, &coverage, current, clook, location, &ctip);
#endif
    char pbuf[256];
    tbuf pb;
    tb_init(&pb, c->os, pbuf, sizeof pbuf);
    int32_t plook = light_plus(&pb, plus);
    set_light(c, F_LIGHT_PLUS_LOOK, F_LIGHT_PLUS_TIP, F_LIGHT_PLUS_AX, plook, built(&pb));
    set_light(c, F_LIGHT_CACHE_LOOK, F_LIGHT_CACHE_TIP, F_LIGHT_CACHE_AX, clook, ctip);
    core_mark_changed(c);
    status_gen_track(c, &f);
    if (c->action_running) return;   /* the buttons stay off until the action ends */
    plus_facts pf = {vf_flag(&f, FACT_REKORDBOX_INSTALLED), vf_str(&f, FACT_ENGINE), vf_flag(&f, FACT_ENGINE_SUPPORTED)};
    cache_facts kf = {vf_flag(&f, FACT_USER_IS_ADMIN), vf_flag(&f, FACT_REKORDBOX_INSTALLED), vf_str(&f, FACT_REKORDBOX_VERSION),
                      vf_flag(&f, FACT_REKORDBOX_SUPPORTED), cache, vf_flag(&f, FACT_ROOT_ORT_EXISTS), vf_flag(&f, FACT_PROTOTYPE_EXISTS),
                      vf_flag(&f, FACT_LIB_EXISTS), vf_flag(&f, FACT_LIB_SIGNED), vf_str_or_null(&f, FACT_ORT_VERSION),
                      vf_str(&f, FACT_ORT_PREFIX)};
    gate_in in;
    memset(&in, 0, sizeof in);
    in.plus = plus;
    in.cache = cache;
    in.chosen_model = vf_flag(&f, FACT_CHOSEN_MODEL);
    in.chosen_cache = vf_flag(&f, FACT_CHOSEN_CACHE);
    in.plus_why = plus_blocked(&pf);
#ifdef CORE_WIN_POLICY
    in.cache_why = c->os == CORE_OS_WIN ? cache_blocked_win(&kf, vf_flag(&f, FACT_ORT_LISTED), vf_flag(&f, FACT_LIB_UNCHECKED)) : cache_blocked(&kf);
    in.win_cache_problem = coverage.problem;
#else
    in.cache_why = cache_blocked(&kf);
#endif
    in.engine = pf.engine;
    in.rekordbox_version = kf.rekordbox_version;
    in.ort_version = kf.ort_version;
    in.cache_traces = vf_flag(&f, FACT_CACHE_TRACES);
    in.rekordbox_installed = pf.rekordbox_installed;
    in.user_is_admin = kf.user_is_admin;
    in.own_bundle = vf_flag(&f, FACT_OWN_BUNDLE);
    in.cache_is_link = vf_flag(&f, FACT_CACHE_IS_LINK);
    in.watcher_off = c->v.watcher_off;
    in.location_notice = NULL;   /* Controller.locationNotice: never set in 1.3.0 */
    char mbuf[512], nbuf[512];
    if (c->fl.model_notice) {
        tbuf b;
        tb_init(&b, c->os, mbuf, sizeof mbuf);
        TB_TEXT0(&b, T_NOTICE_NO_SAVED_MODEL);
        in.model_notice = built(&b);
    }
    if (c->fl.update_version) {   /* updateFound's notice (Update.swift updateNotice) */
        tbuf b;
        tb_init(&b, c->os, nbuf, sizeof nbuf);
        core_slot sl = SLOT_S(version, c->fl.update_version);
        tb_text(&b, T_UPDATE_NOTICE, &sl, 1);
        in.notice = built(&b);
    }
    gate_out g;
    gate_buttons(&in, &g);
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN && coverage.count == 1 && coverage.first_repair &&
        (coverage.first_repair->why == WIN_WHY_NOT_RECORDED || coverage.first_repair->why == WIN_WHY_DELISTED_BRIDGE)) {
        g.install_cache = g.reinstall_cache = 0;   /* D13: no swap over this bridge, Put Back only */
        g.reinstall = in.chosen_model && in.plus_why == FEAT_OK;
        g.reinstall_features = T_FEATURE_PLUS;
    }
    if (c->os == CORE_OS_WIN && (coverage.count >= 2 || coverage.count < 0)) {
        int may_change = in.user_is_admin && !coverage.unchecked;
        int eligible = coverage.ready || coverage.on || kf.rekordbox_supported;
        int opt_in = !coverage.unchecked && !in.chosen_cache && coverage.on && vf_flag(&f, FACT_CACHE_RECORD_OK);
        g.install_cache = (may_change && coverage.ready) || opt_in;
        g.reinstall_cache = may_change && in.chosen_cache && eligible;
        g.reinstall = g.reinstall_cache || (in.chosen_model && in.plus_why == FEAT_OK);
    }
#endif
    view_buttons v = {g.install_plus, g.install_cache, g.reinstall, g.reinstall_plus, g.reinstall_cache, g.settings,
                      g.uninstall_plus, g.uninstall_cache, g.uninstall_all};
    show_buttons(c, &v);
    char *buf = scratch_buf(c);
    if (buf) {
        tbuf b;
        tb_init(&b, c->os, buf, TEXT_CAP);
        w_reinstall_title(&b, &g);
        model_set_str(&c->m, F_REINSTALL_TITLE, built(&b), CORE_NTS);
        tb_init(&b, c->os, buf, TEXT_CAP);
        w_status_lines(&b, &in);   /* between actions: why a feature can't be installed, then any notice */
        model_set_str(&c->m, F_STATUS_LINE, built(&b), CORE_NTS);
    }
}

#ifdef CORE_WIN_POLICY
void view_refresh(core *c) { view_refresh_r1(c, NULL); }
#endif

/* ---------------------------------------------------------------- busy and prepareChanged */

/* busy(msg): `buttons.forEach { $0.isEnabled = false }`: the window's seven (the combined
 * Reinstall and Uninstall Completely aren't among them: the menus check actionRunning) */
void view_busy_on(core *c) {
    view_buttons v = c->v.shown;
    v.install_plus = v.reinstall_plus = v.uninstall_plus = 0;
    v.install_cache = v.reinstall_cache = v.settings = v.uninstall_cache = 0;
    show_buttons(c, &v);
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN) {   /* Put Back Pioneer's File and Don't Ask Again too: no second action under this one */
        model_set_i64(&c->m, F_WIN_RESTORE_ENABLED, 0);
        model_set_i64(&c->m, F_WIN_R1_DISMISS_ENABLED, 0);
        core_mark_changed(c);
    }
#endif
}

void view_busy_off(core *c, int marked) {
#ifdef CORE_TEST_HOOKS
    /* the probe's P3 trigger, where Controller.busy(nil) marks it: before the refresh it starts */
    if (c->test_mark) CALLBACK(c, c->test_mark(c->test_mark_ctx, "trigger.actionEnd"));
#endif
    /* not a short wait (inBackground), unless something changed during it */
    int ended = marked || c->v.changed_during_action;
    int quietly = !ended && c->v.quiet_during_action;
    c->v.changed_during_action = c->v.quiet_during_action = 0;
    view_location_measure(c);   /* an action may have moved the location (Settings, Clear, an uninstall) */
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN && c->role == CORE_ROLE_APP) c->v.r1_after_action = 1;   /* Put Back off until R1 answers (CW3) */
#endif
    view_refresh(c);
#ifdef CORE_WIN_POLICY
    view_r1_check(c);   /* what the action did to rekordbox's files (a restore, an install) */
#endif
    if (ended) view_prepare_changed(c, 0);
    else if (quietly) view_prepare_changed(c, 1);
    else prep_update(c);
}

void view_prepare_changed(core *c, int quiet) {
    if (!quiet) {
        prep_set_stale(c);
        status_forget(c);
    }
    /* not during an action: a password prompt makes the app inactive and active again, and its
     * end checks (quietly if only coming back to the app came meanwhile) */
    if (c->action_running) {
        if (quiet) c->v.quiet_during_action = 1;
        else c->v.changed_during_action = 1;
    }
    if (view_in_front(c, CORE_TAB_PREPARE) && !c->action_running) prep_reload_from_change(c, quiet);
    else prep_update(c);
    status_reload_if_showing(c);
    core_mark_changed(c);
}

/* Controller.swift:143-155 */
void view_location_changed(core *c) {
    view_refresh(c);
    core_facts f;
    view_read_facts(c, &f, CORE_FACTS_REFRESH);
    const char *tip = view_location_tip(c, &f);
    const char *in_use = vf_str_or_null(&f, FACT_LOC_IN_USE);
    char floorb[24], *seen;
    {
        tbuf b;
        tb_init(&b, c->os, floorb, sizeof floorb);
        tb_i64(&b, vf_i64(&f, FACT_LOC_PAUSED_FLOOR, -1));
    }
    /* "\(s.status == nil) \(s.pausedFloor ?? -1) \(folderInUse ?? "") \(s.lightTip ?? "")" */
    const char *measured = vf_flag(&f, FACT_LOC_MEASURED) ? "false" : "true";
    size_t n = strlen(measured) + strlen(floorb) + (in_use ? strlen(in_use) : 0) + (tip ? strlen(tip) : 0) + 4;
    if (!(seen = malloc(n))) return;
    seen[0] = 0;
    strcat(seen, measured);
    strcat(seen, " ");
    strcat(seen, floorb);
    strcat(seen, " ");
    if (in_use) strcat(seen, in_use);
    strcat(seen, " ");
    if (tip) strcat(seen, tip);
    if (c->v.seen_location && !strcmp(seen, c->v.seen_location)) {
        free(seen);
        return;
    }
    free(c->v.seen_location);
    c->v.seen_location = seen;
    view_prepare_changed(c, 1);
}

void view_watcher_off(core *c, int off) {
    if ((off != 0) == c->v.watcher_off) return;
    c->v.watcher_off = off != 0;
#ifdef CORE_WIN_POLICY
    if (off) core_log(c->life, "watcher: installed but not running (turned off in Startup apps, or not started): reinstall reminders are off");
#else
    if (off) core_log(c->life, "watcher: installed but not running (turned off in Login Items, or not loaded): reinstall reminders are off");
#endif
    view_refresh(c);
}

/* ---------------------------------------------------------------- the location's memo (LocationMonitor) */

static void loc_memo_clear(view_loc_memo *m) {
    for (int k = 0; k < LOC_FACTS; k++) {
#if defined(CORE_WIN_POLICY) && defined(CORE_DEBUG)
        if (m->s[k]) LIVE_DEC();
#endif
        free(m->s[k]);
    }
    memset(m->set, 0, sizeof m->set);
    memset(m->i, 0, sizeof m->i);
    memset(m->s, 0, sizeof m->s);
}

enum { LK_KIND = 1, LK_PROBE = 2, LK_DRIVE = 4, LK_FLOOR = 9, LK_IN_USE = 10 };   /* loc_ids' places */
#define SIZE_EVERY_MS 600000   /* LocationMonitor: the size again every 10 minutes */

typedef struct {
    int ok, size, declined;
    view_loc_memo r;
#ifdef CORE_WIN_POLICY
    int not_now;
    uint64_t size_gen;
#endif
} loc_result;
typedef struct { loc_result *r; core_job *done; } loc_work;

static void loc_work_run(core_life *life, void *payload) {   /* a worker: measureCheapState */
    loc_work *w = payload;
    if (life->host.slow_fact && !(life->native & NATIVE_SLOW_FACT)) {
        arena a = {0};
        core_facts f;
        facts_init(&f, &a);
        int32_t rc = life->host.slow_fact(life->host.ctx, CORE_SLOW_LOCATION, "", 0, &f);
        int ok = rc == 0;
#ifdef CORE_WIN_POLICY
        w->r->not_now = rc == CORE_SLOW_NOT_NOW;
        w->r->declined = rc != 0 && !w->r->not_now;
#else
        w->r->declined = !ok;
#endif
        for (int k = 0; ok && k < LOC_FACTS; k++) {
            core_fact id = loc_ids[k];
            w->r->r.set[k] = f.set[id];
            w->r->r.i[k] = f.i[id];
            if (f.set[id] && f.s[id] && !(w->r->r.s[k] = strdup(f.s[id]))) ok = 0;
#if defined(CORE_WIN_POLICY) && defined(CORE_DEBUG)
            if (w->r->r.s[k]) LIVE_INC();
#endif
        }
        w->r->ok = ok;
        arena_free(&a);
    }
    job_post(life, w->done);   /* reserved: main always hears, so `working` always ends */
}
static void loc_work_drop(void *payload) { core_job_drop(((loc_work *)payload)->done); }
static void loc_result_drop(void *payload) { loc_memo_clear(&((loc_result *)payload)->r); }

static int loc_same(const view_loc_memo *a, const view_loc_memo *b) {
    for (int k = 0; k < LOC_FACTS; k++)
        if (a->set[k] != b->set[k] || (a->set[k] && (a->i[k] != b->i[k] || (a->s[k] ? !b->s[k] || strcmp(a->s[k], b->s[k]) : b->s[k] != NULL))))
            return 0;
    return 1;
}

static int str_same(const char *a, const char *b) { return a ? b && !strcmp(a, b) : !b; }
static uint64_t mono_now(core *c) {
#ifdef CORE_TEST_HOOKS
    if (c->size_test_now) return c->size_test_now;
#endif
    (void)c;
    return plat_now_mono_ns();
}

/* the size walk (LocationMonitor.walk, its own serial queue): entriesSize of the folder in use */
typedef struct { uint64_t gen; int ok; int64_t bytes; char folder[]; } walk_result;
typedef struct { walk_result *r; core_job *done; } walk_work;

static void walk_work_run(core_life *life, void *payload) {
    walk_work *w = payload;
    if (life->host.slow_fact && !(life->native & NATIVE_SLOW_FACT)) {
        arena a = {0};
        core_facts f;
        facts_init(&f, &a);
        if (life->host.slow_fact(life->host.ctx, CORE_SLOW_SIZE_WALK, w->r->folder, strlen(w->r->folder), &f) == 0 &&
            f.set[FACT_SIZE_BYTES]) {
            w->r->ok = 1;
            w->r->bytes = f.i[FACT_SIZE_BYTES];
        }
        arena_free(&a);
    }
    job_post(life, w->done);
}
static void walk_work_drop(void *payload) { core_job_drop(((walk_work *)payload)->done); }

static int walk_start(core *c, const char *folder);

static void walk_result_job(core *c, void *payload) {
    walk_result *r = payload;
    view_loc_memo *m = &c->v.loc;
    m->walk_busy = 0;
    /* published when no Clear came meanwhile (REMEASURE) and its folder is still the one in use */
    int moved = 0;
    if (r->gen == m->size_gen && str_same(m->set[LK_IN_USE] ? m->s[LK_IN_USE] : NULL, r->folder)) {
        char *f = strdup(r->folder);
        if (f) {
            free(m->size_folder);
            m->size_folder = f;
            m->size_kind = r->ok ? 1 : 2;
            m->size_bytes = r->ok ? r->bytes : 0;
            m->size_time = mono_now(c);
            moved = 1;
        }
    }
    if (m->walk_again) {   /* asked during this walk: the folder in use now */
        m->walk_again = 0;
        const char *folder = m->set[LK_IN_USE] ? m->s[LK_IN_USE] : NULL;
        if (folder && !(m->walk_busy = walk_start(c, folder))) m->size_time = 0;   /* couldn't start: the next measure walks */
    }
    if (moved) view_location_changed(c);   /* onChange */
}

/* 1: the walk is under way (its result job will run) */
static int walk_start(core *c, const char *folder) {
    size_t n = strlen(folder);
    walk_work *w = malloc(sizeof *w);
    walk_result *r = calloc(1, sizeof *r + n + 1);
    core_job *done = w && r ? job_reserve(c->life, walk_result_job, r, NULL) : NULL;
    if (!done) {
        free(w);
        free(r);
        return 0;
    }
    r->gen = c->v.loc.size_gen;
    memcpy(r->folder, folder, n + 1);
    w->r = r;
    w->done = done;
    return core_post_work(c->life, CORE_WORK_WALK, walk_work_run, w, walk_work_drop) == 0;
}

/* LocationMonitor.measure's size part, on the new snapshot: the old size stands while it is the
 * size of the folder in use; otherwise "measuring…" (or, with no folder in use, not connected or
 * failed); a walk when asked, when the folder changed, or when the last is 10 minutes old.
 * 1: the size shown changed */
static int size_policy(core *c, int size) {
    view_loc_memo *m = &c->v.loc;
    const char *folder = m->set[LK_IN_USE] ? m->s[LK_IN_USE] : NULL;
    int same = str_same(folder, m->size_folder);
    uint64_t now = mono_now(c);
    int need = size || !same || !m->size_time || now - m->size_time > (uint64_t)SIZE_EVERY_MS * 1000000u;
    int64_t kind = m->size_kind, bytes = m->size_bytes;
    if (!(same && !size)) {
        int64_t k = m->i[LK_KIND];
        kind = folder ? 0 : k == LOC_CHOSEN || k == LOC_BRIDGE_TOO_OLD ? 3 : 2;   /* status.dir != nil */
        bytes = 0;
    }
    if (!folder) {
        free(m->size_folder);
        m->size_folder = NULL;
        m->size_time = now;
    }
    int moved = kind != m->size_kind || bytes != m->size_bytes;
    m->size_kind = kind;
    m->size_bytes = bytes;
    if (need && folder) {   /* one at a time: a walk under way is followed by one more, not voided */
        if (m->walk_busy) m->walk_again = 1;
        else m->walk_busy = walk_start(c, folder);
    }
    return moved;
}

/* the 10-minute timer: update(size: true) on main, then armed again. Its job is reserved when it's
 * armed, so a fire with no memory left can't lose it and leave the timer stopped for the session */
typedef struct { core_life *life; core_job *job; } every_ctx;
static void every_arm(core *c);
static void every_job(core *c, void *payload) {
    view_loc_memo *m = &c->v.loc;
#ifdef CORE_WIN_POLICY
    if (*(uintptr_t *)payload != m->every_gen) return;
#else
    (void)payload;
#endif
    if (!m->every) return;   /* cancelled meanwhile */
    plat_timer_cancel(m->every);   /* fired: its context went with it */
    m->every = NULL;
    m->every_ctx = NULL;
    every_arm(c);
    view_location_update(c, 1);
}
static void every_fired(void *ctx) {
    every_ctx *e = ctx;
    job_post(e->life, e->job);
    life_release(e->life);
    free(e);
}
static void every_arm(core *c) {
    view_loc_memo *m = &c->v.loc;
    int32_t ms = SIZE_EVERY_MS;
#ifdef CORE_TEST_HOOKS
    if (c->size_every_test_ms > 0) ms = c->size_every_test_ms;
#endif
    every_ctx *e = m->every ? NULL : malloc(sizeof *e);
    if (!e) return;   /* out of memory: armed at the next measure (loc_result_job) */
    void *payload = NULL;
#ifdef CORE_WIN_POLICY
    uintptr_t *gen = malloc(sizeof *gen);
    if (!gen) { free(e); return; }
    *gen = m->every_gen + 1;
    payload = gen;
#endif
    if (!(e->job = job_reserve(c->life, every_job, payload, NULL))) {
        free(payload);
        free(e);
        return;
    }
    e->life = life_retain(c->life);
    if (!(m->every = plat_timer_new(NULL, (uint64_t)ms * 1000000u, every_fired, e))) {
        core_job_drop(e->job);
        life_release(e->life);
        free(e);
        return;
    }
    m->every_ctx = e;
#ifdef CORE_WIN_POLICY
    m->every_gen++;
#endif
}
static void every_cancel(view_loc_memo *m) {
    if (m->every && plat_timer_cancel(m->every)) {
        every_ctx *e = m->every_ctx;
        core_job_drop(e->job);
        life_release(e->life);
        free(e);
    }
    m->every = NULL;
    m->every_ctx = NULL;
}

#ifdef CORE_WIN_POLICY
/* review LN1: the DLL drops a measure's late answer, so after a not now the core asks again itself:
 * a one-shot timer, separate from the 10-minute one, 30 s doubling to 10 minutes; reset by a good
 * answer and by a new target (view_location_remeasure), cancelled by a plain no and at the end.
 * Its job is reserved when armed, as every_arm's; it carries the retry's generation, so the job of
 * a retry that fired and was cancelled before its job ran leaves a newer retry alone */
#define LOC_RETRY_MS 30000
#define LOC_RETRY_MAX_MS 600000
static void retry_job(core *c, void *payload) {
    view_loc_memo *m = &c->v.loc;
    if (!m->retry || *(uintptr_t *)payload != m->retry_gen) return;   /* cancelled meanwhile */
    plat_timer_cancel(m->retry);   /* fired: its context went with it */
    m->retry = NULL;
    m->retry_ctx = NULL;
    view_location_measure(c);   /* a measure under way: `again` covers it */
}
static void retry_arm(core *c) {   /* at retry_ms, which then doubles (bounded) */
    view_loc_memo *m = &c->v.loc;
    int32_t ms = m->retry_ms;
#ifdef CORE_TEST_HOOKS
    if (c->size_retry_test_ms > 0) ms = c->size_retry_test_ms;
#endif
    every_ctx *e = m->retry ? NULL : malloc(sizeof *e);
    uintptr_t *gen = e ? malloc(sizeof *gen) : NULL;   /* the job's payload: freed with it */
    if (gen) *gen = m->retry_gen + 1;
    if (!gen || !(e->job = job_reserve(c->life, retry_job, gen, NULL))) {   /* out of memory: armed at the next not now */
        free(gen);
        free(e);
        return;
    }
    e->life = life_retain(c->life);
    if (!(m->retry = plat_timer_new(NULL, (uint64_t)ms * 1000000u, every_fired, e))) {
        core_job_drop(e->job);
        life_release(e->life);
        free(e);
        return;
    }
    m->retry_ctx = e;
    m->retry_gen++;
    m->retry_armed_ms = m->retry_ms;
    m->retry_ms = m->retry_ms > LOC_RETRY_MAX_MS / 2 ? LOC_RETRY_MAX_MS : m->retry_ms * 2;
}
static void retry_cancel(view_loc_memo *m) {
    if (m->retry && plat_timer_cancel(m->retry)) {
        every_ctx *e = m->retry_ctx;
        core_job_drop(e->job);
        life_release(e->life);
        free(e);
    }
    m->retry = NULL;
    m->retry_ctx = NULL;
}
#endif

static void loc_result_job(core *c, void *payload) {
    loc_result *r = payload;
    view_loc_memo *m = &c->v.loc;
    m->working = 0;
    int moved = 0;
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN && r->size_gen != m->size_gen) {
        loc_memo_clear(&r->r);
        goto rerun;
    }
#endif
    if (r->ok) {   /* the snapshot replaced; onChange when it differs (`s != old`) */
        moved = !m->known || !loc_same(m, &r->r);
#ifdef CORE_WIN_POLICY
        moved |= m->known == VIEW_LOC_STALE;
#endif
        if (!m->every) every_arm(c);   /* startObserving's timer, once the host measures (again after an arm out of memory) */
        loc_memo_clear(m);
        memcpy(m->set, r->r.set, sizeof m->set);
        memcpy(m->i, r->r.i, sizeof m->i);
        memcpy(m->s, r->r.s, sizeof m->s);
        memset(r->r.s, 0, sizeof r->r.s);
        m->known = 1;
        moved |= size_policy(c, r->size);
#ifdef CORE_WIN_POLICY
        retry_cancel(m);   /* a good answer: the retry ends and starts over at 30 s (review LN1) */
        m->retry_ms = LOC_RETRY_MS;
#endif
    } else {   /* not measured */
        loc_memo_clear(&r->r);
        if (r->declined && m->known) {   /* the host answers nonzero now: its own snapshot is used again */
            loc_memo_clear(m);
            m->known = 0;
            moved = 1;
            every_cancel(m);   /* no 10-minute measure for a memo no longer kept (armed again if one comes) */
            free(m->size_folder);   /* the size goes with it: "measuring…", a walk under way not published */
            m->size_folder = NULL;
            m->size_kind = m->size_bytes = 0;
            m->size_time = 0;
            m->size_gen++;
            m->walk_again = 0;
        }   /* out of memory here: the memo stands */
#ifdef CORE_WIN_POLICY
        if (r->declined) {
            every_cancel(m);   /* a plain no drops the timer a first not now armed (review LN2) */
            retry_cancel(m);   /* and the retry (review LN1) */
        } else if (c->life->host.slow_fact && !(c->life->native & NATIVE_SLOW_FACT)) {
            if (c->os == CORE_OS_WIN && r->not_now && m->known && m->known != VIEW_LOC_STALE) {
                m->known = VIEW_LOC_STALE;
                moved = 1;
            }
            if (!m->every) every_arm(c);   /* not now (review NW1): the timer asks again, even before a first answer; never for a host that doesn't measure (LN3) */
            if (!m->retry) retry_arm(c);   /* and sooner, backing off (review LN1) */
        }
#endif
    }
#ifdef CORE_WIN_POLICY
rerun:
#endif
    if (m->again) {   /* asked meanwhile: measured again (`rerun`) */
        int sz = m->again_size;
        m->again = m->again_size = 0;
        view_location_update(c, sz);
    }
    if (moved) view_location_changed(c);
}

void view_location_measure(core *c) { view_location_update(c, 0); }

void view_location_remeasure(core *c) {
    view_loc_memo *m = &c->v.loc;
#ifdef CORE_WIN_POLICY
    int stale = c->os == CORE_OS_WIN && m->known && m->known != VIEW_LOC_STALE;
    if (c->os == CORE_OS_WIN) {
        if (stale) m->known = VIEW_LOC_STALE;
        m->walk_again = 0;
    } else
#endif
    {
        m->size_kind = 0;   /* invalidateSize: "measuring…" until the next walk */
        m->size_bytes = 0;
    }
    m->size_time = 0;
    m->size_gen++;   /* a walk from before (Clear, a new location) isn't published */
#ifdef CORE_WIN_POLICY
    m->retry_ms = LOC_RETRY_MS;   /* a new target: the retry starts over at 30 s (review LN1) */
    if (m->retry && m->retry_armed_ms > LOC_RETRY_MS) {
        retry_cancel(m);
        retry_arm(c);
    }
#endif
    view_location_update(c, 1);
#ifdef CORE_WIN_POLICY
    if (stale) view_location_changed(c);
#endif
}

/* a memo string, whole, in the flow's arena: "" when the memo has none, NULL out of memory */
static const char *loc_copy(arena *a, const view_loc_memo *m, int k) {
    const char *s = m->set[k] && m->s[k] ? m->s[k] : "";
    return arena_strdup(a, s, strlen(s));
}

int view_loc_query(core *c, void *arg) {
    const view_loc_q *q = arg;
    view_loc_now *n = q->n;
    view_loc_memo *m = &c->v.loc;
    memset(n, 0, sizeof *n);
    n->known = m->known;
    n->floor = m->set[LK_FLOOR] ? m->i[LK_FLOOR] : -1;
    n->size_kind = m->size_kind;
    n->size_bytes = m->size_bytes;
    n->drive = loc_copy(q->a, m, LK_DRIVE);
    n->in_use = loc_copy(q->a, m, LK_IN_USE);
#ifdef CORE_WIN_POLICY
    n->drive_set = m->set[LK_DRIVE];
    n->kind = m->set[LK_KIND] ? m->i[LK_KIND] : -1;
    n->probe = m->set[LK_PROBE] ? m->i[LK_PROBE] : -1;
#endif
#ifdef CORE_TEST_HOOKS
    if (m->test_fail_copies > 0) {   /* the copies fail, as out of memory (t_loc_oom) */
        m->test_fail_copies--;
        n->drive = NULL;
    }
#endif
    if (!n->drive || !n->in_use) {   /* out of memory: the query fails (the flow stops), never a cut snapshot */
        n->known = 0;
        n->drive = n->in_use = "";
        return 0;
    }
    return 1;
}

void view_location_update(core *c, int size) {
    view_loc_memo *m = &c->v.loc;
    if (c->role != CORE_ROLE_APP) return;
    if (m->working) {
        m->again = 1;
        m->again_size |= size;
        return;
    }
    loc_work *w = malloc(sizeof *w);
    loc_result *r = calloc(1, sizeof *r);
    core_job *done = w && r ? job_reserve(c->life, loc_result_job, r, loc_result_drop) : NULL;
    if (!done) {
        free(w);
        free(r);
        return;
    }
    r->size = size;
#ifdef CORE_WIN_POLICY
    r->size_gen = m->size_gen;
#endif
    w->r = r;
    w->done = done;
    m->working = 1;
    if (core_post_work(c->life, CORE_WORK_FACT, loc_work_run, w, loc_work_drop)) m->working = 0;
}

/* checkWatcherOn (Actions.swift:199): updateWatcherState's measurement on the flow queue
 * (`work.async`), so never while an install or uninstall replaces the LaunchAgent; its main half
 * is view_watcher_off. CORE_STEP_WATCHER_STATE measures only (WATCHER_CHECK would repair the
 * LaunchAgent, the M7 review's M72). The result is reserved, so main hears back once the worker is
 * posted. If a job can't be allocated, the measurement is lost and the next activation measures
 * again. A step that fails changes nothing. */
typedef struct { core_job *done; int32_t *off; } watcher_work;

static void watcher_result_job(core *c, void *payload) {
    int32_t off = *(int32_t *)payload;
    if (off >= 0) view_watcher_off(c, off);
}

static void watcher_work_run(core_life *life, void *payload) {
    watcher_work *w = payload;
    *w->off = -1;
    if (life->host.step && !(life->native & NATIVE_STEP)) {
        arena a = {0};
        core_facts f;
        facts_init(&f, &a);
        if (life->host.step(life->host.ctx, CORE_STEP_WATCHER_STATE, NULL, 0, &f) == 0 && f.set[FACT_STEP_FLAG])
            *w->off = f.i[FACT_STEP_FLAG] != 0;
        arena_free(&a);
    }
    job_post(life, w->done);
}
static void watcher_work_drop(void *payload) { core_job_drop(((watcher_work *)payload)->done); }

void view_check_watcher_on(core *c) {
    if (c->role != CORE_ROLE_APP) return;
    watcher_work *w = malloc(sizeof *w);
    int32_t *off = malloc(sizeof *off);
    core_job *done = w && off ? job_reserve(c->life, watcher_result_job, off, NULL) : NULL;
    if (!done) {
        free(w);
        free(off);
        return;
    }
    *off = -1;
    w->done = done;
    w->off = off;
    core_post_work(c->life, CORE_WORK_FLOW, watcher_work_run, w, watcher_work_drop);
}

/* ---------------------------------------------------------------- in front (PrepareView.swift:293) */

/* inFront(tab): the tab's view is loaded and in the window, the window isn't minimized, and the app
 * is active. Read live, at the decision (mac-core §3.6). */
static int in_front(core *c, int tab, int held) {
    uint32_t ff;
    int32_t open = 0;
    CALLBACK(c, ff = c->life->host.front_facts(c->life->host.ctx));
    if (held) CALLBACK(c, open = c->life->host.modal_open(c->life->host.ctx));
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN) return win_in_front(ff, open, tab, held);   /* W-core's policy (win.c) */
#endif
    return (ff & CORE_FRONT_ACTIVE) && (ff & CORE_FRONT_WINDOW_LOADED) && !(ff & CORE_FRONT_MINIATURIZED) &&
           (int)((ff & CORE_FRONT_TAB_MASK) >> CORE_FRONT_TAB_SHIFT) == tab && !(held && open);
}

int view_in_front(core *c, int tab) { return in_front(c, tab, 0); }

/* the held click's check (PrepareView.swift:670): in front, and no dialog or sheet up */
int view_in_front_held(core *c, int tab) { return in_front(c, tab, 1); }

/* ---------------------------------------------------------------- start, tabs, menus */

/* Controller.init: Status, or Manage the first time (nothing installed yet); then the buttons in
 * their final state before the window appears */
void view_start(core *c) {
    core_facts f;
    view_read_facts(c, &f, CORE_FACTS_REFRESH);
    int first = vf_flag(&f, FACT_STEMS_PLUS_PRESENT) || vf_flag(&f, FACT_STEMS_CACHE_PRESENT) ? CORE_TAB_STATUS : CORE_TAB_MANAGE;
#ifdef CORE_WIN_POLICY
    if (view_tabs_hidden(c)) first = CORE_TAB_MANAGE;   /* Manage-only host */
#endif
    model_set_i64(&c->m, F_TAB, first);
    view_refresh(c);
#ifdef CORE_WIN_POLICY
    view_r1_check(c);
#endif
}

/* a tab comes to the front (viewWillAppear): the user's click or the core's own switch */
void view_tab_shown(core *c, int tab) {
    if (tab == CORE_TAB_PREPARE) prep_will_appear(c);
    else if (tab == CORE_TAB_STATUS) status_will_appear(c);
}

void view_show_tab(core *c, int tab) {
    if (model_get_i64(&c->m, F_TAB) == tab) return;
    model_set_i64(&c->m, F_TAB, tab);
    core_mark_changed(c);
    view_tab_shown(c, tab);
}

#ifdef CORE_WIN_POLICY
int view_tabs_hidden(const core *c) { return c->os == CORE_OS_WIN && !c->prepare_tabs; }
#endif

void view_menu_facts(core *c, void *out) {
    menu_facts *mf = out;
    const view_buttons *v = &c->v.shown;
    memset(mf, 0, sizeof *mf);
    mf->action_running = c->action_running;
    mf->preparing = c->preparing;
    mf->paused = c->paused;
#ifdef CORE_WIN_POLICY
    mf->tabs_hidden = view_tabs_hidden(c);
#endif
    if (c->role != CORE_ROLE_APP) return;
    mf->prep_selected = prep_has_selection(c);
    mf->update_visible = (int32_t)model_get_i64(&c->m, F_UPDATE_VISIBLE);
    mf->buttons.install_plus = v->install_plus;
    mf->buttons.install_cache = v->install_cache;
    mf->buttons.reinstall = v->reinstall;
    mf->buttons.reinstall_plus = v->reinstall_plus;
    mf->buttons.reinstall_cache = v->reinstall_cache;
    mf->buttons.settings = v->settings;
    mf->buttons.uninstall_plus = v->uninstall_plus;
    mf->buttons.uninstall_cache = v->uninstall_cache;
    mf->buttons.uninstall_all = v->uninstall_all;
}

/* a menu item's enabled state now (validateMenuItem), by the event it sends; an id no item sends: 1 */
static int menu_enabled(core *c, const char *id, size_t n) {
    for (int32_t i = 0; i < core_menu_rows_n; i++) {
        const menu_row *r = &core_menu_rows[i];
        if (!r->event || strlen(r->event) != n || memcmp(r->event, id, n)) continue;
        menu_facts mf;
        view_menu_facts(c, &mf);
        return (menu_rule(i, &mf, NULL) & CORE_MENU_ENABLED) != 0;
    }
    return 1;
}

static int is_id(const char *id, size_t n, const char *name) { return strlen(name) == n && !memcmp(id, name, n); }

/* the Help menu's pages (Paths.swift): the project's home page, its troubleshooting page, its issues page */
#define HOME_URL "https://github.com/Gabe-LS/rbstemsplus"
static const struct { const char *id, *url; } help_urls[] = {
    {"help", HOME_URL},                                                /* homeURL (openHome) */
    {"troubleshooting", HOME_URL "/blob/main/docs/troubleshooting.md"}, /* troubleshootingURL (openTroubleshooting) */
    {"issues", HOME_URL "/issues"},                                    /* issuesPageURL (openIssues) */
};

static void open_url_work(core_life *life, void *payload) {   /* NSWorkspace.open: a short worker job (D4) */
    if (life->host.step && !(life->native & NATIVE_STEP)) {
        arena a = {0};
        core_facts f;
        facts_init(&f, &a);
        const char *args[1] = {(const char *)payload};
        life->host.step(life->host.ctx, CORE_STEP_OPEN_URL, args, 1, &f);
        arena_free(&a);
    }
}

/* an action's click or menu choice comes only from a control that is on (a disabled NSButton or
 * menu item sends nothing): a button by its state as last shown (refresh, busy), a menu item by
 * validateMenuItem now. An id with no such state (Windows' restore_pioneer, the Update and Create
 * Report clicks) is left to the flows' own busy rule. 1: it may start */
int view_click_enabled(core *c, const char *id, size_t n, int menu) {
#ifdef CORE_TEST_HOOKS
    if (c->click_gate_off) return 1;
#endif
    if (menu) return menu_enabled(c, id, n);
#ifdef CORE_WIN_POLICY
    /* the hidden tabs' clicks: their buttons don't exist in the first release */
    if (view_tabs_hidden(c) && (is_id(id, n, "tab_status") || is_id(id, n, "tab_prepare") || is_id(id, n, "prepare_playlist")
                                || is_id(id, n, "check_again") || is_id(id, n, "pause") || is_id(id, n, "stop")))
        return 0;
#endif
    const view_buttons *v = &c->v.shown;
    const struct { const char *id; int32_t on; } b[] = {
        {"install_plus", v->install_plus},       {"install_cache", v->install_cache},     {"reinstall", v->reinstall},
        {"reinstall_plus", v->reinstall_plus},   {"reinstall_cache", v->reinstall_cache}, {"settings", v->settings},
        {"uninstall_plus", v->uninstall_plus},   {"uninstall_cache", v->uninstall_cache}, {"uninstall_all", v->uninstall_all},
    };
    for (size_t i = 0; i < sizeof b / sizeof *b; i++)
        if (is_id(id, n, b[i].id)) return b[i].on != 0;
    return 1;
}

/* the clicks and menu choices that aren't actions (main.swift's menus, PrepareView.swift's
 * buttons). A menu item that is off now (validateMenuItem) does nothing, as AppKit's wouldn't. */
void view_event(core *c, const char *id, size_t n, int menu) {
    if (menu && !menu_enabled(c, id, n)) {
        core_log(c->life, "core: a menu item that is off is ignored");
        return;
    }
    for (size_t i = 0; i < sizeof help_urls / sizeof *help_urls; i++)
        if (is_id(id, n, help_urls[i].id)) {   /* the app delegate's: on during an action too */
            char *url = strdup(help_urls[i].url);   /* the job's payload: freed with it */
            if (url) core_post_work(c->life, CORE_WORK_FACT, open_url_work, url, NULL);
            return;
        }
    if (is_id(id, n, "tab_status")) view_show_tab(c, CORE_TAB_STATUS);
    else if (is_id(id, n, "tab_prepare")) view_show_tab(c, CORE_TAB_PREPARE);
    else if (is_id(id, n, "tab_manage")) view_show_tab(c, CORE_TAB_MANAGE);
    else if (is_id(id, n, "show_log")) {   /* showLogMenu: Manage, the log scrolled to its end */
        view_show_tab(c, CORE_TAB_MANAGE);
        model_set_i64(&c->m, F_LOG_SHOW_SEQ, model_get_i64(&c->m, F_LOG_SHOW_SEQ) + 1);
        core_mark_changed(c);
    } else if (!prep_event(c, id, n, menu) && !status_event(c, id, n, menu))
        core_log(c->life, "core: an unknown click or menu choice is ignored");
}

/* ---------------------------------------------------------------- life */

void view_init(core *c) {
    memset(&c->v, 0, sizeof c->v);
    c->v.loc.size_kind = 0;   /* StemsSize.measuring */
#ifdef CORE_WIN_POLICY
    c->v.loc.retry_ms = LOC_RETRY_MS;
#endif
}

void view_free(core *c) {
    every_cancel(&c->v.loc);
#ifdef CORE_WIN_POLICY
    retry_cancel(&c->v.loc);
#endif
    loc_memo_clear(&c->v.loc);
    free(c->v.loc.size_folder);
    free(c->v.seen_location);
    free(c->v.status_key);
    free(c->v.sha.path);
    free(c->v.sha.work_path);
    memset(&c->v, 0, sizeof c->v);
}
