/* The view model's scenario tests (M4, mac-core §7): a deterministic fake host on the test's main
 * thread (the main queue pumped by the test, slow facts and short worker jobs run in that pump,
 * the helper's lines delivered by the test as a reader thread would deliver them), scripted
 * cheap facts and front facts. Each scenario names the 1.3.0 commit whose behaviour it pins
 * (tests/harness/scenarios.tsv); the expected words are 1.3.0's Swift literals, so the oracle is
 * today's app, not the core's own tables. The frame-level oracle is G0's harness traces (G2):
 * these tests assert the same properties on the snapshot the shell would render. */
#include "../internal.h"
#include "view_test.h"
#include <stdio.h>
#include <time.h>

static int *g_passed, *g_failed;
#define CHECK(cond) do { if (cond) (*g_passed)++; else { (*g_failed)++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKS(cond, what) do { if (cond) (*g_passed)++; else { (*g_failed)++; printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond, (what)); } } while (0)
#ifdef CORE_DEBUG
#  define LEAKCHECK() CHECK(core_debug_live_objects() == 0)
#else
#  define LEAKCHECK() ((void)0)
#endif

/* ---------------------------------------------------------------- the fake host */

typedef struct vitem { struct vitem *next; core_job *job; } vitem;

typedef struct { uint64_t id; char argv[1024]; int ended; } vrun;

typedef struct vt {
    pthread_mutex_t mu;
    vitem *head, *tail;      /* main and short worker jobs, run by pump() on the test's thread */
    pthread_t threads[16];
    int nthreads;
    int running;   /* flow threads not yet ended (under mu) */
    core *c;
    core_snap *snap;         /* the shell's: the last render */
    int renders;
    int trk_reload, trk_old_rows, trk_nchanged;
    int32_t trk_changed[8];
    uint32_t front;
    int modal;
    int spawn_refuse;
    vrun runs[128];
    int nruns;
    struct { uint64_t run; int sig; } sigs[64];
    int nsigs;
    char logs[128][200];
    int nlogs;
    int notifies;
    char notify_title[256], notify_body[512];
    int slow_calls;
    int refresh_reads, prepare_reads, kept_reads;   /* host->facts by part asked (M71) */
    int watcher_off, watcher_steps;                 /* the reminders' state the watcher steps measure (M72) */
    const char *model_sha;   /* CORE_SLOW_MODEL_SHA's answer */
    /* the cheap facts */
    int plus, cache, chosen_model, chosen_cache, rb_installed, rb_supported, engine_supported, admin, own_bundle;
    int root_ort, lib_exists, lib_signed, enabled, rb_model, measured, bridge_keeps, apple, helper_there, traces;
    int64_t model_size, model_mtime, quality, paused_floor, cfg_location, loc_kind, loc_probe;
    const char *loc_drive;
    const char *engine, *rb_version, *ort_version, *ort_prefix, *max_gb, *bridge_models, *our_models, *kept_sets;
    const char *max_days;    /* FACT_CONFIG_MAX_DAYS, NULL not set */
    const char *br_state, *br_reason, *br_drive;   /* the live bridge's state (FACT_LOC_BRIDGE_*), NULL none */
    int rb_running;
    int ort_listed;          /* Windows: FACT_ORT_LISTED */
    int lib_unchecked;       /* Windows: FACT_LIB_UNCHECKED */
    const char *win_rows;
    int64_t r1_state;        /* Windows: CORE_SLOW_R1's FACT_R1_STATE (review CW2) */
    const char *config;      /* the start config, NULL: CONFIG */
    /* the dialogs, as the flow thread asked them (under mu), and the buttons pressed, in order (0 by default) */
    struct { char title[256], msg[1024], buttons[512]; } dlg[8];
    int ndlg;
    int answer[8];
    int hold, held, release;  /* hold: the next dialog waits (held 1) until release, at most 5 s */
    pthread_cond_t cv;
    /* the steps: FREE_SPACE's bytes (-1: it fails), CONFIG_WRITE and STOP_KEEPING failing; each step
     * asked, as "NAME arg arg" */
    int64_t free_bytes;
    int config_fail, stop_fail;
    int slow_location, loc_measures;   /* CORE_SLOW_LOCATION answered from the location facts below; how often */
    int32_t slow_decline;      /* CORE_SLOW_LOCATION's answer while slow_location is 0 (0: 1) */
    int no_loc_sheet;          /* the cheap sheet carries no FACT_LOC_* rows (Windows keeps no snapshot) */
    int hold_timer_post, timer_post_held, release_timer_post;
    int no_slow_fact;          /* the host leaves slow_fact out (review LN3) */
    const char *in_use;        /* FACT_LOC_IN_USE (CORE_SLOW_LOCATION), NULL not set */
    const char *slow_drive;    /* FACT_LOC_DRIVE (CORE_SLOW_LOCATION), NULL not set */
    char locnow_arg[4096];     /* the last LOCATION_NOW's folder in use ("" asked with none) */
    arena qa;                  /* loc_now's copies */
    int64_t walk_bytes;        /* CORE_SLOW_SIZE_WALK's answer, -1 it fails */
    int walks;
    int hold_step, step_held, step_release;   /* the step of kind hold_step - 1 waits (held 1) until let go, 5 s at most */
    int follow_tab;            /* front_facts' tab is the one the core asks for (the window follows F_TAB) */
    char step_log[2048];       /* every step kind asked, as numbers */
    int64_t locnow_kind;       /* LOCATION_NOW's FACT_LOC_SIZE_KIND (this Mac, no gate) */
    int hold_walks;            /* CORE_WORK_WALK jobs wait in held_walk until let go (walk_go) */
    core_job *held_walk[32];
    int nheld_walk;
    int64_t set_size_kind;     /* SETTINGS' FACT_SET_SIZE_KIND (4: the core's), FACT_SET_SIZE 7 GiB */
    int64_t set_clear, set_location;   /* SETTINGS' FACT_SET_CLEAR (3: the drive "B") and FACT_SET_LOCATION */
#ifdef CORE_WIN_POLICY
    const char *settings_in_use;
#endif
    int date_asks, date_on_main;   /* DATE_TEXT: asked; asked inside a callback on main */
    /* SQ1: the queued actions' steps. REQUEST's age + 1 (0: none); the next REKORDBOX checks that
     * fail, find rekordbox open, still closing (Windows) or updating, each used up as answered;
     * LEFTOVER's action; BRIDGE_SHA's installed bridge */
    int64_t req_age1;
    int rb_fail_n, rb_open_n, rb_closing_n, rb_upd_n;
    const char *leftover_id, *leftover_label, *bridge_installed;
    char max_gb_written[32];   /* a CONFIG_WRITE's max_gb, which max_gb then points to */
    char steps[16][256];
    int nsteps;
} vt;

static void vpush(vt *t, core_job *j) {
    vitem *it = calloc(1, sizeof *it);
    it->job = j;
    pthread_mutex_lock(&t->mu);
    if (t->tail) t->tail->next = it; else t->head = it;
    t->tail = it;
    pthread_mutex_unlock(&t->mu);
}

static vitem *vpop(vt *t) {
    pthread_mutex_lock(&t->mu);
    vitem *it = t->head;
    if (it) {
        t->head = it->next;
        if (!t->head) t->tail = NULL;
    }
    pthread_mutex_unlock(&t->mu);
    return it;
}

/* every queued job, until none is left (jobs post jobs); returns how many ran */
static int pump(vt *t) {
    int n = 0;
    for (;;) {
        vitem *it = vpop(t);
        if (!it) {
            if (!n) return 0;
            /* a flow on its own thread may still post: give it a moment, once */
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&t->mu);
            int more = t->head != NULL;
            pthread_mutex_unlock(&t->mu);
            if (!more) return n;
            continue;
        }
        core_run(it->job);
        free(it);
        n++;
    }
}

static void h_render(void *ctx, core_snap *s, const core_delta *d) {
    vt *t = ctx;
    t->renders++;
    t->trk_reload = core_delta_reload(d, T_PREP_TRACKS);
    t->trk_old_rows = core_delta_old_rows(d, T_PREP_TRACKS);
    int32_t n;
    const int32_t *ch = core_delta_changed(d, T_PREP_TRACKS, &n);
    t->trk_nchanged = n;
    for (int32_t i = 0; i < n && i < 8; i++) t->trk_changed[i] = ch[i];
    core_snap_release(t->snap);
    t->snap = s;
}
static uint32_t h_front(void *ctx) {
    vt *t = ctx;
    if (!t->follow_tab) return t->front;
    return (t->front & ~(uint32_t)CORE_FRONT_TAB_MASK) | ((uint32_t)model_get_i64(&t->c->m, F_TAB) << CORE_FRONT_TAB_SHIFT);
}
static int32_t h_modal(void *ctx) { return ((vt *)ctx)->modal; }

/* host->facts writes only the parts asked for (core_facts_parts, M71), so a read that asks for
 * too little sees its rows missing; each part's reads are counted */
static void h_facts(void *ctx, core_facts *o) {
    vt *t = ctx;
    uint32_t parts = core_facts_parts(o);
    if (parts & CORE_FACTS_REFRESH) t->refresh_reads++;
    if (parts & CORE_FACTS_PREPARE) {
        t->prepare_reads++;
        core_fact_flag(o, FACT_BRIDGE_KEEPS, t->bridge_keeps);
        core_fact_i64(o, FACT_STEMS_QUALITY, t->quality);
        core_fact_str(o, FACT_OUR_MODELS, t->our_models, CORE_NTS);
    }
    if (parts & CORE_FACTS_KEPT) {
        t->kept_reads++;
        if (t->kept_sets) core_fact_str(o, FACT_KEPT_SETS, t->kept_sets, CORE_NTS);
    }
    if (!(parts & CORE_FACTS_REFRESH)) return;
    core_fact_flag(o, FACT_STEMS_PLUS_PRESENT, t->plus);
    core_fact_flag(o, FACT_STEMS_CACHE_PRESENT, t->cache);
    core_fact_flag(o, FACT_CHOSEN_MODEL, t->chosen_model);
    core_fact_flag(o, FACT_CHOSEN_CACHE, t->chosen_cache);
    core_fact_flag(o, FACT_REKORDBOX_INSTALLED, t->rb_installed);
    core_fact_str(o, FACT_REKORDBOX_VERSION, t->rb_version, CORE_NTS);
    core_fact_flag(o, FACT_REKORDBOX_SUPPORTED, t->rb_supported);
    core_fact_str(o, FACT_ENGINE, t->engine, CORE_NTS);
    core_fact_flag(o, FACT_ENGINE_SUPPORTED, t->engine_supported);
    core_fact_flag(o, FACT_USER_IS_ADMIN, t->admin);
    core_fact_flag(o, FACT_OWN_BUNDLE, t->own_bundle);
    core_fact_flag(o, FACT_CACHE_TRACES, t->traces);
    core_fact_flag(o, FACT_CACHE_IS_LINK, 0);
    core_fact_flag(o, FACT_ROOT_ORT_EXISTS, t->root_ort);
    core_fact_flag(o, FACT_PROTOTYPE_EXISTS, 0);
    core_fact_flag(o, FACT_LIB_EXISTS, t->lib_exists);
    core_fact_flag(o, FACT_LIB_SIGNED, t->lib_signed);
    if (t->ort_version) core_fact_str(o, FACT_ORT_VERSION, t->ort_version, CORE_NTS);
    core_fact_str(o, FACT_ORT_PREFIX, t->ort_prefix, CORE_NTS);
#ifdef CORE_WIN_POLICY
    core_fact_flag(o, FACT_ORT_LISTED, t->ort_listed);
    if (t->lib_unchecked) core_fact_flag(o, FACT_LIB_UNCHECKED, 1);
    if (t->win_rows) core_fact_str(o, FACT_WIN_RB_ROWS, t->win_rows, CORE_NTS);
#endif
    core_fact_flag(o, FACT_CONFIG_ENABLED, t->enabled);
    core_fact_flag(o, FACT_CONFIG_RB_MODEL, t->rb_model);
    pthread_mutex_lock(&t->mu);   /* a flow's CONFIG_WRITE may change it */
    core_fact_str(o, FACT_CONFIG_MAX_GB, t->max_gb, CORE_NTS);
    pthread_mutex_unlock(&t->mu);
    core_fact_i64(o, FACT_CONFIG_LOCATION, t->cfg_location);
    if (t->cfg_location == 1) core_fact_str(o, FACT_CONFIG_DIR, "E:\\rbstemsplus", CORE_NTS);
    if (!t->no_loc_sheet) {
        core_fact_flag(o, FACT_LOC_MEASURED, t->measured);
        core_fact_i64(o, FACT_LOC_KIND, t->loc_kind);
        if (t->loc_drive) core_fact_str(o, FACT_LOC_STATUS_DRIVE, t->loc_drive, CORE_NTS);
        core_fact_i64(o, FACT_LOC_PROBE, t->loc_probe);
        core_fact_i64(o, FACT_LOC_PAUSED_FLOOR, t->paused_floor);
    }
    core_fact_flag(o, FACT_REKORDBOX_RUNNING, t->rb_running);
    if (!t->no_loc_sheet) {
        if (t->in_use) core_fact_str(o, FACT_LOC_IN_USE, t->in_use, CORE_NTS);
        if (t->br_state) core_fact_str(o, FACT_LOC_BRIDGE_STATE, t->br_state, CORE_NTS);
        if (t->br_reason) core_fact_str(o, FACT_LOC_BRIDGE_REASON, t->br_reason, CORE_NTS);
        if (t->br_drive) core_fact_str(o, FACT_LOC_BRIDGE_DRIVE, t->br_drive, CORE_NTS);
    }
    if (t->bridge_models) core_fact_str(o, FACT_BRIDGE_MODELS, t->bridge_models, CORE_NTS);
    if (t->model_size >= 0) {
        core_fact_i64(o, FACT_MODEL_SIZE, t->model_size);
        core_fact_i64(o, FACT_MODEL_MTIME, t->model_mtime);
    }
    core_fact_flag(o, FACT_APPLE_SILICON, t->apple);
    core_fact_flag(o, FACT_HELPER_THERE, t->helper_there);
    if (t->max_days) core_fact_str(o, FACT_CONFIG_MAX_DAYS, t->max_days, CORE_NTS);
}

static void h_post_main(void *ctx, core_job *j) {
    vt *t = ctx;
    pthread_mutex_lock(&t->mu);
    if (t->hold_timer_post && !core_on_main(t->c)) {
        t->hold_timer_post = 0;
        t->timer_post_held = 1;
        pthread_cond_broadcast(&t->cv);
        while (!t->release_timer_post) pthread_cond_wait(&t->cv, &t->mu);
    }
    pthread_mutex_unlock(&t->mu);
    vpush(ctx, j);
}

typedef struct { vt *t; core_job *j; } vwork;
static void *worker(void *a) {
    vwork w = *(vwork *)a;
    free(a);
    core_run(w.j);
    pthread_mutex_lock(&w.t->mu);
    w.t->running--;
    pthread_mutex_unlock(&w.t->mu);
    return NULL;
}
static void h_post_work(void *ctx, core_work kind, core_job *j) {
    vt *t = ctx;
    if (kind == CORE_WORK_FLOW || kind == CORE_WORK_QUIT) {   /* a flow blocks on main: its own thread */
        vwork *w = malloc(sizeof *w);
        w->t = t;
        w->j = j;
        pthread_mutex_lock(&t->mu);
        t->running++;
        pthread_create(&t->threads[t->nthreads++], NULL, worker, w);
        pthread_mutex_unlock(&t->mu);
        return;
    }
    if (kind == CORE_WORK_WALK) {
        pthread_mutex_lock(&t->mu);
        int hold = t->hold_walks && t->nheld_walk < 32;
        if (hold) t->held_walk[t->nheld_walk++] = j;
        pthread_mutex_unlock(&t->mu);
        if (hold) return;
    }
    vpush(t, j);   /* a slow fact or a short step: run in the pump, in order */
}

static int32_t h_dialog(void *ctx, const core_dialog *d, core_reply *r) {
    vt *t = ctx;
    pthread_mutex_lock(&t->mu);
    int i = t->ndlg < 8 ? t->ndlg++ : 7;
    snprintf(t->dlg[i].title, sizeof t->dlg[i].title, "%s", d->title);
    snprintf(t->dlg[i].msg, sizeof t->dlg[i].msg, "%s", d->message);
    t->dlg[i].buttons[0] = 0;
    for (int32_t b = 0; b < d->nbuttons; b++) {
        if (b) strncat(t->dlg[i].buttons, " | ", sizeof t->dlg[i].buttons - strlen(t->dlg[i].buttons) - 1);
        strncat(t->dlg[i].buttons, d->buttons[b], sizeof t->dlg[i].buttons - strlen(t->dlg[i].buttons) - 1);
    }
    if (t->hold) {   /* the dialog stays up until the test lets it go (5 s at most) */
        t->hold = 0;
        t->held = 1;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 5;
        while (!t->release && pthread_cond_timedwait(&t->cv, &t->mu, &until) == 0) {}
        t->held = 0;
        t->release = 0;
    }
    int a = t->answer[i];
    pthread_mutex_unlock(&t->mu);
    if (a >= 100) core_reply_row_button(r, 0, a - 100);   /* the Settings sheet's row: Change…, Use This Mac */
    else core_reply_button(r, a);
    return 0;
}

static int32_t h_slow(void *ctx, core_slow kind, const char *key, size_t n, core_facts *out) {
    vt *t = ctx;
    (void)key; (void)n;
    if (kind == CORE_SLOW_LOCATION) {   /* measureCheapState, when the fake measures (else its cheap facts carry it) */
        pthread_mutex_lock(&t->mu);
        int on = t->slow_location;
        if (on) {
            t->loc_measures++;
            core_fact_flag(out, FACT_LOC_MEASURED, t->measured);
            core_fact_i64(out, FACT_LOC_KIND, t->loc_kind);
            core_fact_i64(out, FACT_LOC_PROBE, t->loc_probe);
            if (t->loc_drive) core_fact_str(out, FACT_LOC_STATUS_DRIVE, t->loc_drive, CORE_NTS);
            core_fact_i64(out, FACT_LOC_PAUSED_FLOOR, t->paused_floor);
            core_fact_flag(out, FACT_REKORDBOX_RUNNING, 0);
            if (t->in_use) core_fact_str(out, FACT_LOC_IN_USE, t->in_use, CORE_NTS);
            if (t->slow_drive) core_fact_str(out, FACT_LOC_DRIVE, t->slow_drive, CORE_NTS);
        }
        int32_t no = t->slow_decline ? t->slow_decline : 1;
        pthread_mutex_unlock(&t->mu);
        return on ? 0 : no;
    }
    if (kind == CORE_SLOW_SIZE_WALK) {   /* entriesSize, on the walk's queue */
        pthread_mutex_lock(&t->mu);
        t->walks++;
        int64_t b = t->walk_bytes;
        pthread_mutex_unlock(&t->mu);
        if (b < 0) return 1;
        core_fact_i64(out, FACT_SIZE_BYTES, b);
        return 0;
    }
    t->slow_calls++;
#ifdef CORE_WIN_POLICY
    if (kind == CORE_SLOW_R1) {
        core_fact_i64(out, FACT_R1_STATE, t->r1_state);
        return 0;
    }
#endif
    if (kind != CORE_SLOW_MODEL_SHA || !t->model_sha) return 1;
    core_fact_str(out, FACT_SHA256, t->model_sha, CORE_NTS);
    return 0;
}

static int32_t h_spawn(void *ctx, uint64_t run, const char *const *argv, int32_t argc) {
    vt *t = ctx;
    if (t->spawn_refuse) return 1;
    vrun *r = &t->runs[t->nruns++ % 128];
    memset(r, 0, sizeof *r);
    r->id = run;
    for (int32_t i = 0; i < argc; i++) {
        strncat(r->argv, argv[i], sizeof r->argv - strlen(r->argv) - 2);
        strncat(r->argv, " ", sizeof r->argv - strlen(r->argv) - 1);
    }
    return 0;
}

static void h_signal(void *ctx, uint64_t run, core_helper_sig sig) {
    vt *t = ctx;
    if (t->nsigs < 64) {
        t->sigs[t->nsigs].run = run;
        t->sigs[t->nsigs].sig = (int)sig;
        t->nsigs++;
    }
}

static int32_t h_step(void *ctx, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    vt *t = ctx;
    if (kind == CORE_STEP_NOTIFY && nargs == 2) {
        t->notifies++;
        snprintf(t->notify_title, sizeof t->notify_title, "%s", args[0]);
        snprintf(t->notify_body, sizeof t->notify_body, "%s", args[1]);
        return 0;
    }
    if (kind == CORE_STEP_DATE_TEXT) {   /* a medium style of the fake's own: "medium <date>" */
        t->date_asks++;   /* the worker jobs run in the pump: on main, a step would be inside a callback */
        if (t->c && t->c->cb_depth > 0) t->date_on_main++;
        if (nargs != 1) return 1;
        char m[64];
        snprintf(m, sizeof m, "medium %s", args[0]);
        core_fact_str(out, FACT_STEP_TEXT, m, CORE_NTS);
        return 0;
    }
    pthread_mutex_lock(&t->mu);
    {
        char k[16];
        snprintf(k, sizeof k, "%d ", (int)kind);
        strncat(t->step_log, k, sizeof t->step_log - strlen(t->step_log) - 1);
    }
    if (t->hold_step == (int)kind + 1) {
        t->hold_step = 0;
        t->step_held = 1;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 5;
        while (!t->step_release && pthread_cond_timedwait(&t->cv, &t->mu, &until) == 0) {}
        t->step_held = 0;
        t->step_release = 0;
    }
    pthread_mutex_unlock(&t->mu);
    if (kind == CORE_STEP_REQUEST) {   /* the watcher's request, until it is taken */
        pthread_mutex_lock(&t->mu);
        if (nargs && !strcmp(args[0], "take")) t->req_age1 = 0;
        else if (t->req_age1) core_fact_i64(out, FACT_STEP_CODE, t->req_age1 - 1);
        pthread_mutex_unlock(&t->mu);
        return 0;
    }
    if (kind == CORE_STEP_REKORDBOX) {
        pthread_mutex_lock(&t->mu);
        int fail = t->rb_fail_n > 0, open = !fail && t->rb_open_n > 0, closing = !fail && !open && t->rb_closing_n > 0;
        int upd = !fail && !open && !closing && t->rb_upd_n > 0;
        if (fail) t->rb_fail_n--;
        if (open) t->rb_open_n--;
        if (closing) t->rb_closing_n--;
        if (upd) t->rb_upd_n--;
        pthread_mutex_unlock(&t->mu);
        if (fail) return 1;
        if (open || closing) core_fact_flag(out, FACT_REKORDBOX_OPEN, 1);
#ifdef CORE_WIN_POLICY
        if (closing) core_fact_flag(out, FACT_REKORDBOX_CLOSING, 1);
#endif
        if (upd) core_fact_flag(out, FACT_PIONEER_UPDATING, 1);
        return 0;
    }
    if (kind == CORE_STEP_LEFTOVER) {
        if (t->leftover_id) {
            core_fact_str(out, FACT_STEP_TEXT, t->leftover_id, CORE_NTS);
            core_fact_str(out, FACT_STEP_TEXT2, t->leftover_label, CORE_NTS);
        }
        return 0;
    }
    if (kind == CORE_STEP_BRIDGE_SHA) {
        if (t->bridge_installed) core_fact_str(out, FACT_STEP_TEXT, t->bridge_installed, CORE_NTS);
        return 0;
    }
    if (kind == CORE_STEP_PAYLOAD) {   /* a verified payload.json: Demucs v4's install goes through */
        core_fact_i64(out, FACT_PAYLOAD_FETCH, nargs && !strcmp(args[0], "latest") ? 0 : 2);
        core_fact_flag(out, FACT_PAYLOAD_FOUND, 1);
        core_fact_str(out, FACT_PAYLOAD_VERSION, "1.4.0", CORE_NTS);
        core_fact_str(out, FACT_PAYLOAD_APP_VERSION, "1.4.0", CORE_NTS);
        core_fact_str(out, FACT_PAYLOAD_SIGNED_BY, "everyday", CORE_NTS);
        core_fact_str(out, FACT_PAYLOAD_MODEL_SHA, "1111111111111111111111111111111111111111111111111111111111111111", CORE_NTS);
        core_fact_str(out, FACT_PAYLOAD_BRIDGE_SHA, "2222222222222222222222222222222222222222222222222222222222222222", CORE_NTS);
        core_fact_str(out, FACT_STEP_PATH, "/RB/hdemucs.onnx", CORE_NTS);
        return 0;
    }
#ifdef CORE_WIN_POLICY
    if (kind == CORE_STEP_WIN_SCOPE) {
        core_fact_i64(out, FACT_STEP_CODE, 0);
        core_fact_str(out, FACT_WIN_SCOPE, "7.2.10:1111111111111111", CORE_NTS);
        core_fact_str(out, FACT_WIN_SCOPE_VERSIONS, "7.2.10\n", CORE_NTS);
        return 0;
    }
#endif
    if (kind == CORE_STEP_BLOCKED) {   /* nothing blocks */
        core_fact_str(out, FACT_ENGINE, "1.0.0", CORE_NTS);
        core_fact_flag(out, FACT_ENGINE_SUPPORTED, 1);
        core_fact_flag(out, FACT_REKORDBOX_SUPPORTED, 1);
        core_fact_flag(out, FACT_ROOT_ORT_EXISTS, 1);
        core_fact_flag(out, FACT_LIB_EXISTS, 1);
        core_fact_flag(out, FACT_LIB_SIGNED, 1);
        core_fact_str(out, FACT_ORT_VERSION, "1.17.3", CORE_NTS);
        core_fact_str(out, FACT_ORT_PREFIX, "1.17", CORE_NTS);
        return 0;
    }
    if (kind == CORE_STEP_LOCATION_NOW) {   /* LocationMonitor.current(): this Mac, the size as the test says */
        pthread_mutex_lock(&t->mu);
        int64_t k = t->locnow_kind;
        snprintf(t->locnow_arg, sizeof t->locnow_arg, "%s", nargs >= 1 ? args[0] : "");
        pthread_mutex_unlock(&t->mu);
        core_fact_str(out, FACT_LOC_DIR, "", CORE_NTS);
        core_fact_str(out, FACT_LOC_GATE_ID, "", CORE_NTS);
        core_fact_i64(out, FACT_LOC_SIZE_KIND, k);
        core_fact_i64(out, FACT_LOC_SIZE, 9ll << 30);
        return 0;
    }
    if (kind == CORE_STEP_SETTINGS) {   /* the Settings sheet's facts: This Mac, its size as the test says */
        core_fact_str(out, FACT_SET_GB, "20", CORE_NTS);
        core_fact_str(out, FACT_SET_DAYS, "30", CORE_NTS);
        core_fact_flag(out, FACT_SET_CAN_CHOOSE, 1);
        core_fact_i64(out, FACT_SET_SIZE_KIND, t->set_size_kind);
        core_fact_i64(out, FACT_SET_SIZE, 7ll << 30);
        core_fact_i64(out, FACT_SET_CLEAR, t->set_clear);
        core_fact_i64(out, FACT_SET_LOCATION, t->set_location);
#ifdef CORE_WIN_POLICY
        if (t->settings_in_use) core_fact_str(out, FACT_LOC_IN_USE, t->settings_in_use, CORE_NTS);
#endif
        if (t->set_clear == 3) {
            core_fact_str(out, FACT_LOC_DIR, "/Volumes/B/rbstemsplus", CORE_NTS);
            core_fact_str(out, FACT_LOC_GATE_ID, "id1", CORE_NTS);
            core_fact_str(out, FACT_LOC_DRIVE, "B", CORE_NTS);
            core_fact_str(out, FACT_STEP_PATH, "/Volumes/B/rbstemsplus", CORE_NTS);
        }
        return 0;
    }
    if (kind == CORE_STEP_LOCATION_PROBE) {   /* a drive that may be used */
        core_fact_i64(out, FACT_STEP_CODE, 0);
        core_fact_str(out, FACT_STEP_TEXT, "/Volumes/B", CORE_NTS);
        core_fact_str(out, FACT_STEP_TEXT2, "/Volumes/B", CORE_NTS);
        core_fact_str(out, FACT_LOC_DRIVE, "B", CORE_NTS);
        core_fact_str(out, FACT_STEP_PATH, "/Volumes/B", CORE_NTS);
        return 0;
    }
    if (kind == CORE_STEP_LOCATION_DISCOVER) {   /* our folder, the location in use renamed: the same id */
        core_fact_i64(out, FACT_STEP_CODE, 2);
        core_fact_str(out, FACT_LOC_DIR, "/Volumes/B/rbstemsplus", CORE_NTS);
        core_fact_str(out, FACT_LOC_GATE_ID, "id1", CORE_NTS);
        core_fact_str(out, FACT_STEP_TEXT, "/Volumes/A/rbstemsplus", CORE_NTS);
        core_fact_str(out, FACT_STEP_TEXT2, "id1", CORE_NTS);
        core_fact_str(out, FACT_LOC_DRIVE, "A", CORE_NTS);
        core_fact_str(out, FACT_STEP_PATH, "/Volumes/B/rbstemsplus", CORE_NTS);
        return 0;
    }
    if (kind == CORE_STEP_WATCHER_STATE || kind == CORE_STEP_LAUNCH_AGENT_LOAD || kind == CORE_STEP_LAUNCH_AGENT_UNLOAD) {
        pthread_mutex_lock(&t->mu);
        t->watcher_steps++;
        int off = t->watcher_off;
        pthread_mutex_unlock(&t->mu);
        core_fact_flag(out, FACT_STEP_FLAG, off);   /* updateWatcherState's measurement (M72) */
        return 0;
    }
    if (kind != CORE_STEP_FREE_SPACE && kind != CORE_STEP_CONFIG_WRITE && kind != CORE_STEP_STOP_KEEPING && kind != CORE_STEP_OPEN_URL) return 0;
    pthread_mutex_lock(&t->mu);
    if (t->nsteps < 16) {
        char *l = t->steps[t->nsteps++];
        snprintf(l, 256, "%s", kind == CORE_STEP_FREE_SPACE ? "FREE_SPACE" : kind == CORE_STEP_CONFIG_WRITE ? "CONFIG_WRITE" : kind == CORE_STEP_OPEN_URL ? "OPEN_URL" : "STOP_KEEPING");
        for (int32_t i = 0; i < nargs; i++) {
            strncat(l, " ", 255 - strlen(l));
            strncat(l, args[i], 255 - strlen(l));
        }
    }
    pthread_mutex_unlock(&t->mu);
    if (kind == CORE_STEP_FREE_SPACE) {
        if (t->free_bytes < 0) return 1;
        core_fact_i64(out, FACT_STEP_CODE, t->free_bytes);
        return 0;
    }
    if (kind == CORE_STEP_CONFIG_WRITE) {
        if (t->config_fail) return 1;
        pthread_mutex_lock(&t->mu);   /* config.ini as written: the next facts read it */
        for (int32_t i = 0; i + 1 < nargs; i += 2)
            if (!strcmp(args[i], "max_gb")) {
                snprintf(t->max_gb_written, sizeof t->max_gb_written, "%s", args[i + 1]);
                t->max_gb = t->max_gb_written;
            }
        pthread_mutex_unlock(&t->mu);
        return 0;
    }
    return kind == CORE_STEP_STOP_KEEPING ? t->stop_fail : 0;
}

/* the dialog the flow holds up has been asked (the test pumps main meanwhile) */
static int wait_held(vt *t) {
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&t->mu);
        int h = t->held;
        pthread_mutex_unlock(&t->mu);
        if (h) return 1;
        if (!pump(t)) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
    }
    return 0;
}
static void release(vt *t) {
    pthread_mutex_lock(&t->mu);
    t->release = 1;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mu);
}
static int stepped(vt *t, const char *step) {
    for (int i = 0; i < t->nsteps; i++)
        if (!strcmp(t->steps[i], step)) return 1;
    return 0;
}

static int32_t h_pick(void *ctx, const char *start, size_t n, core_reply *r) {   /* the folder picker: drive B */
    (void)ctx; (void)start; (void)n;
    core_reply_field(r, 0, "/Volumes/B", CORE_NTS);
    return 0;
}

static void h_close_reply(void *ctx, core_close what, int32_t proceed) { (void)ctx; (void)what; (void)proceed; }

static void h_log(void *ctx, const char *line, size_t len) {
    vt *t = ctx;
    pthread_mutex_lock(&t->mu);
    if (t->nlogs < 128) snprintf(t->logs[t->nlogs++], sizeof t->logs[0], "%.*s", (int)len, line);
    pthread_mutex_unlock(&t->mu);
}

static int logged(vt *t, const char *line) {
    for (int i = 0; i < t->nlogs; i++)
        if (!strcmp(t->logs[i], line)) return 1;
    return 0;
}

#define FRONT(tab) (CORE_FRONT_ACTIVE | CORE_FRONT_WINDOW_LOADED | ((uint32_t)(tab) << CORE_FRONT_TAB_SHIFT))

/* a Mac ready to prepare playlists: Stems Cache on this Mac with rekordbox's own model */
static void vt_init(vt *t) {
    memset(t, 0, sizeof *t);
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->max_days = "30";
    t->set_clear = 1;
    t->free_bytes = 100ll << 30;
    t->front = FRONT(CORE_TAB_STATUS);
    t->cache = t->chosen_cache = t->rb_installed = t->rb_supported = t->engine_supported = t->admin = t->own_bundle = 1;
    t->root_ort = t->lib_exists = t->lib_signed = t->enabled = t->rb_model = t->measured = t->bridge_keeps = 1;
    t->apple = t->helper_there = t->traces = 1;
    t->model_size = 300;
    t->model_mtime = 7;
    t->paused_floor = -1;
    t->engine = "1.0.0";
    t->rb_version = "7.2.4.0001";
    t->ort_version = "1.17.3";
    t->ort_prefix = "1.17";
    t->max_gb = "20";
    t->bridge_models = "aaaa\nbbbb";
    t->our_models = "cccc";
    t->model_sha = "aaaa";
}

static const char *CONFIG =
    "{\"version\":\"1.4.0\",\"os\":\"mac\",\"paths\":{\"home\":\"/Users/x\",\"support\":\"/Users/x/S\","
    "\"cache_default\":\"/Users/x/S/cache\",\"model\":\"/RB/hdemucs.onnx\",\"root_ort\":\"/L/ort.dylib\","
    "\"rekordbox\":\"/Applications/rekordbox 7/rekordbox.app\",\"kept\":\"/Users/x/S/kept\"}}";

static core *vt_core(vt *t) {
    core_host h;
    memset(&h, 0, sizeof h);
    h.size = sizeof h;
    h.ctx = t;
    h.render = h_render;
    h.front_facts = h_front;
    h.modal_open = h_modal;
    h.facts = h_facts;
    h.post_main = h_post_main;
    h.post_work = h_post_work;
    h.dialog = h_dialog;
    h.slow_fact = t->no_slow_fact ? NULL : h_slow;
    h.helper_spawn = h_spawn;
    h.helper_signal = h_signal;
    h.step = h_step;
    h.pick_folder = h_pick;
    h.close_reply = h_close_reply;
    h.log = h_log;
    t->c = core_new(&h, CORE_ROLE_APP, t->config ? t->config : CONFIG, CORE_NTS);
    return t->c;
}

/* pumps main until every flow thread has ended and the queue is empty, then joins them */
static void vt_settle(vt *t) {
    for (int i = 0; i < 10000; i++) {
        int n = pump(t);
        pthread_mutex_lock(&t->mu);
        int busy = t->running || t->head;
        pthread_mutex_unlock(&t->mu);
        if (!n && !busy) break;
        if (!n) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
    }
    for (int i = 0; i < t->nthreads; i++) pthread_join(t->threads[i], NULL);
    t->nthreads = 0;
}

static void vt_end(vt *t) {
    vt_settle(t);
    core_free(t->c);
    /* every run started ends, as the host's reader says once each: after the free, dropped */
    for (int i = 0; i < t->nruns && i < 128; i++)
        if (!t->runs[i].ended) {
            t->runs[i].ended = 1;
            core_helper_ended(t->c, t->runs[i].id, 0, "", 0);
        }
    pump(t);   /* dropped now */
    core_snap_release(t->snap);
    t->snap = NULL;
    arena_free(&t->qa);
    pthread_cond_destroy(&t->cv);
    pthread_mutex_destroy(&t->mu);
}

static int ev(vt *t, const char *json) {
    int r = core_event(t->c, json, CORE_NTS);
    pump(t);
    return r;
}

/* the field now, as the next render would show it */
static char sbuf[8][4096];
static int sbuf_i;
static const char *S(vt *t, core_field f) {
    core_snap *s = core_snap_current(t->c);
    char *b = sbuf[sbuf_i++ & 7];
    snprintf(b, 4096, "%s", core_str(s, f, NULL));
    core_snap_release(s);
    return b;
}
static int64_t I(vt *t, core_field f) {
    core_snap *s = core_snap_current(t->c);
    int64_t v = core_i64(s, f);
    core_snap_release(s);
    return v;
}
static int64_t FL(vt *t, core_field f) {
    core_snap *s = core_snap_current(t->c);
    int64_t v = core_flag(s, f);
    core_snap_release(s);
    return v;
}
static const char *CELL(vt *t, core_column col, int32_t row) {
    core_snap *s = core_snap_current(t->c);
    char *b = sbuf[sbuf_i++ & 7];
    snprintf(b, 4096, "%s", core_cell_str(s, col, row, NULL));
    core_snap_release(s);
    return b;
}
static int64_t CELLI(vt *t, core_column col, int32_t row) {
    core_snap *s = core_snap_current(t->c);
    int64_t v = core_cell_i64(s, col, row);
    core_snap_release(s);
    return v;
}
static int32_t ROWS(vt *t, core_table tb) {
    core_snap *s = core_snap_current(t->c);
    int32_t n = core_rows(s, tb);
    core_snap_release(s);
    return n;
}
static uint64_t GEN(vt *t, core_table tb) {
    core_snap *s = core_snap_current(t->c);
    uint64_t g = core_list_gen(s, tb);
    core_snap_release(s);
    return g;
}

/* the latest run whose arguments contain `what` */
static vrun *run_with(vt *t, const char *what) {
    for (int i = t->nruns - 1; i >= 0 && i >= t->nruns - 128; i--)
        if (strstr(t->runs[i % 128].argv, what)) return &t->runs[i % 128];
    return NULL;
}
static int runs_with(vt *t, const char *what) {
    int n = 0;
    for (int i = 0; i < t->nruns && i < 128; i++) n += strstr(t->runs[i].argv, what) != NULL;
    return n;
}
static void line(vt *t, vrun *r, const char *json) {
    if (!r) return;
    core_helper_line(t->c, r->id, json, CORE_NTS);
    pump(t);
}
static void end_run(vt *t, vrun *r, int status, const char *err) {
    if (!r || r->ended) return;
    r->ended = 1;
    core_helper_ended(t->c, r->id, status, err, CORE_NTS);
    pump(t);
}

static void select_outline(vt *t, int32_t row) {
    char e[160];
    snprintf(e, sizeof e, "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":%d,\"gen\":%llu}", row,
             (unsigned long long)GEN(t, T_PREP_OUTLINE));
    ev(t, e);
}

/* the library: a folder with Friday inside, an intelligent list, Saturday */
static const char *const LIB[] = {
    "{\"playlist\":\"1\",\"parent\":\"0\",\"name\":\"Sets\",\"kind\":\"folder\",\"tracks\":0}",
    "{\"playlist\":\"2\",\"parent\":\"1\",\"name\":\"Friday\",\"kind\":\"playlist\",\"tracks\":3}",
    "{\"playlist\":\"3\",\"parent\":\"0\",\"name\":\"Smart\",\"kind\":\"intelligent\",\"tracks\":9}",
    "{\"playlist\":\"4\",\"parent\":\"0\",\"name\":\"Saturday\",\"kind\":\"playlist\",\"tracks\":2}",
};
static void give_library(vt *t, int n) {
    vrun *r = run_with(t, "--playlists");
    CHECK(r && !r->ended);
    for (int i = 0; i < n; i++) line(t, r, LIB[i]);
    end_run(t, r, 0, "");
}
static void give_tracks(vt *t, int n) {
    vrun *r = run_with(t, "--tracks");
    CHECK(r && !r->ended);
    static const char *const f[] = {"{\"file\":\"/Music/a.mp3\"}", "{\"file\":\"/Music/b.mp3\"}", "{\"file\":\"/Music/c.mp3\"}"};
    for (int i = 0; i < n; i++) line(t, r, f[i]);
    end_run(t, r, 0, "");
}
static const char *const EST_A = "{\"file\":\"/Music/a.mp3\",\"result\":\"ready\",\"seconds\":200}";
static const char *const EST_B = "{\"file\":\"/Music/b.mp3\",\"result\":\"prepare\",\"seconds\":240}";
static const char *const EST_C = "{\"file\":\"/Music/c.mp3\",\"result\":\"prepare\",\"seconds\":180}";
static const char *const EST_END =
    "{\"estimate\":1,\"ready\":1,\"prepare\":2,\"skipped\":0,\"unchecked\":0,\"asking\":0,\"fit_files\":3,\"time_s\":300,"
    "\"need_gb\":0.5,\"kept_gb\":1.0,\"share_gb\":15,\"max_gb\":20,\"max_gb_needed\":0,\"fits\":true,\"disk_ok\":true,\"folders\":[]}";
static void give_estimate(vt *t) {
    vrun *r = run_with(t, "--estimate");
    CHECK(r && !r->ended);
    line(t, r, EST_A);
    line(t, r, EST_B);
    line(t, r, EST_C);
    line(t, r, EST_END);
    end_run(t, r, 0, "");
}

/* the app started on the Status tab, then the Prepare tab shown with Friday chosen and estimated */
static void to_friday_estimated(vt *t) {
    ev(t, "{\"ev\":\"start\"}");
    core_commit(t->c);
    ev(t, "{\"ev\":\"shown\"}");
    t->front = FRONT(CORE_TAB_PREPARE);
    ev(t, "{\"ev\":\"tab\",\"tab\":1}");
    give_library(t, 4);
    select_outline(t, 1);   /* Friday */
    give_tracks(t, 3);
    give_estimate(t);
}

/* ---------------------------------------------------------------- refresh (Controller.refresh) */

/* light-states, activation-refresh: the lights, the buttons, the status line, the model's
 * sha256 worked out in the background and the light judged once it is known */
static void t_refresh(void) {
    vt t;
    vt_init(&t);
    t.model_sha = "ffff";   /* a model the installed bridge doesn't list */
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    CHECK(I(&t, F_TAB) == CORE_TAB_STATUS);   /* something is installed: Status first */
    CHECK(I(&t, F_LIGHT_PLUS_LOOK) == LOOK_OFF && !strcmp(S(&t, F_LIGHT_PLUS_TIP), "Demucs v4 is off."));
    CHECK(!strcmp(S(&t, F_LIGHT_PLUS_AX), "off"));
    /* the model's sha256 isn't known at the first look: not judged, and asked in the background */
    CHECK(t.slow_calls == 1);
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED);   /* the refresh it started ran when it came */
    CHECK(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache doesn't support STEMS Engine 1.0.0 yet. When an RB Stems Plus update supports it, click Reinstall Stems Cache."));
    CHECK(!strcmp(S(&t, F_LIGHT_CACHE_AX), "installed, not working right now"));
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(t.slow_calls == 1);   /* memoised by path, size and mtime */
    t.model_mtime = 8;           /* the file changed: worked out again */
    t.model_sha = "bbbb";
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(t.slow_calls == 2);
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON && !strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache is on."));
    /* the buttons between actions (Controller.swift:257-269) */
    CHECK(FL(&t, F_INSTALL_PLUS_ENABLED) == 1 && FL(&t, F_INSTALL_CACHE_ENABLED) == 0);
    CHECK(FL(&t, F_REINSTALL_CACHE_ENABLED) == 1 && FL(&t, F_REINSTALL_PLUS_ENABLED) == 0);
    CHECK(FL(&t, F_UNINSTALL_CACHE_ENABLED) == 1 && FL(&t, F_UNINSTALL_PLUS_ENABLED) == 0 && FL(&t, F_UNINSTALL_ALL_ENABLED) == 1);
    CHECK(FL(&t, F_SETTINGS_ENABLED) == 1 && FL(&t, F_REINSTALL_ENABLED) == 1);
    CHECK(!strcmp(S(&t, F_REINSTALL_TITLE), "Reinstall Stems Cache"));
    CHECK(!strcmp(S(&t, F_STATUS_LINE), ""));
    /* a standard account: Stems Cache's buttons off, and why */
    t.admin = 0;
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(FL(&t, F_UNINSTALL_CACHE_ENABLED) == 0 && FL(&t, F_UNINSTALL_ALL_ENABLED) == 0 && FL(&t, F_REINSTALL_CACHE_ENABLED) == 0);
    CHECK(!strcmp(S(&t, F_STATUS_LINE), "Reinstalling or removing Stems Cache needs an administrator account."));
    /* the menus read the buttons as shown (validateMenuItem) */
    CHECK(!(core_menu_state(t.c, M_MANAGE_CACHE_UNINSTALL, NULL, NULL) & CORE_MENU_ENABLED));
    CHECK(core_menu_state(t.c, M_MANAGE_PLUS_INSTALL, NULL, NULL) & CORE_MENU_ENABLED);
    t.admin = 1;
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(FL(&t, F_UNINSTALL_ALL_ENABLED) == 1);
    /* an action runs: every window button off and the status line the action's; a refresh then
     * changes only the lights (Controller.swift:249) */
    t.c->entry++;
    t.c->action_running = 1;
    model_set_str(&t.c->m, F_STATUS_LINE, "Installing…", CORE_NTS);
    view_busy_on(t.c);
    core_leave(t.c);
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(FL(&t, F_INSTALL_PLUS_ENABLED) == 0 && FL(&t, F_SETTINGS_ENABLED) == 0 && FL(&t, F_REINSTALL_CACHE_ENABLED) == 0);
    CHECK(FL(&t, F_UNINSTALL_ALL_ENABLED) == 1);   /* not one of the window's buttons */
    CHECK(!strcmp(S(&t, F_STATUS_LINE), "Installing…"));
    t.plus = 1;
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(I(&t, F_LIGHT_PLUS_LOOK) == LOOK_ON);   /* the lights still follow */
    /* the action ends: the buttons back */
    t.c->entry++;
    t.c->action_running = 0;
    view_busy_off(t.c, 1);
    core_leave(t.c);
    pump(&t);
    CHECK(FL(&t, F_UNINSTALL_PLUS_ENABLED) == 1 && FL(&t, F_SETTINGS_ENABLED) == 1);
    vt_end(&t);
    LEAKCHECK();
}

/* the model's sha256 under a failed allocation: nothing stays "under way", and the next look asks
 * again and judges the light (E2: its result job is reserved before the work is) */
static void t_sha_alloc_fail(void) {
    vt t;
    vt_init(&t);
    t.model_sha = "ffff";
    vt_core(&t);
    core_test_fail_allocs(t.c, 1);   /* the first allocation: the hash's reserved result job */
    ev(&t, "{\"ev\":\"start\"}");
    core_test_fail_allocs(t.c, 0);
    CHECK(t.slow_calls == 0 && !t.c->v.sha.working && I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON);   /* not judged yet */
    /* coming back asks again; the Status tab's prepareFacts() asks first, with nobody to tell (as
     * today: only the call that starts the work hears), so the light is judged at the next look */
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(t.slow_calls == 1 && t.c->v.sha.known && !t.c->v.sha.working);
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(t.slow_calls == 1 && I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED);
    vt_end(&t);
    LEAKCHECK();
}

/* the location's onChange: a quiet check only when what the tabs show about the location moved;
 * settings-sheet (e6d8c06): the tabs check again on the first measurement and when a drive goes */
static void t_location_changed(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    int playlists = runs_with(&t, "--playlists");
    ev(&t, "{\"ev\":\"location\"}");   /* the first measurement: a check */
    CHECK(runs_with(&t, "--playlists") == playlists + 1);
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    ev(&t, "{\"ev\":\"location\"}");   /* nothing moved: no check */
    CHECK(runs_with(&t, "--playlists") == playlists + 1);
    /* only the light's text moves (a drive not connected; the floor as it was): a check, and the
     * tab says why it can't check */
    t.loc_kind = LOC_CHOSEN;
    t.loc_probe = PROBE_NOT_CONNECTED;
    t.loc_drive = "GLS";
    ev(&t, "{\"ev\":\"location\"}");
    CHECK(runs_with(&t, "--playlists") == playlists + 2);
    give_library(&t, 4);   /* shown once the playlists come, as today */
    CHECK(*S(&t, F_LIGHT_CACHE_TIP) && strstr(S(&t, F_LIGHT_CACHE_TIP), "GLS") && !strcmp(S(&t, F_PREP_INFO), S(&t, F_LIGHT_CACHE_TIP)));
    t.paused_floor = 5LL << 30;        /* under its floor: a check */
    ev(&t, "{\"ev\":\"location\"}");
    CHECK(runs_with(&t, "--playlists") == playlists + 3);
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED);
    vt_end(&t);
    LEAKCHECK();
}

/* runPrepareHelper's timeouts: a helper that goes past its deadline is stopped (p.terminate()),
 * from a plat.h timer through main; one that ended in time, and a Prepare run, are never */
static int stops_of(vt *t, uint64_t run) {
    int n = 0;
    for (int i = 0; i < t->nsigs; i++) n += t->sigs[i].run == run && t->sigs[i].sig == CORE_HELPER_STOP;
    return n;
}
static void wait_ms(vt *t, int ms) {   /* real time passes, main pumped */
    for (int i = 0; i < ms; i++) {
        pump(t);
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    pump(t);
}
/* the deadline a run was given (before the tests' override), -1 none recorded */
static int32_t deadline_of(vt *t, const vrun *r) {
    for (int32_t i = 0; r && i < t->c->ndeadline_seen; i++)
        if (t->c->deadline_seen[i].run == r->id) return t->c->deadline_seen[i].ms;
    return -1;
}
/* after a core_free: the timers' threads let their life references go (at most 2 s) */
static void live_settled(void) {
#ifdef CORE_DEBUG
    for (int i = 0; i < 400 && core_debug_live_objects() != 0; i++) {
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
    }
#endif
}
static void t_deadlines(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    t.c->deadline_test_ms = 30;
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    vrun *r = run_with(&t, "--playlists");
    CHECK(r && !r->ended);
    for (int i = 0; i < 2000 && r && !stops_of(&t, r->id); i++) wait_ms(&t, 1);
    CHECK(r && stops_of(&t, r->id) == 1);   /* the library's read stopped, once */
    CHECK(deadline_of(&t, r) == 60000);
    end_run(&t, r, 15, "");
    vt_end(&t);
    live_settled();
    LEAKCHECK();
    vt_init(&t);   /* in time: the library, the tracks, the estimate, then the run */
    vt_core(&t);
    t.c->deadline_test_ms = 300;
    to_friday_estimated(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    vrun *run = run_with(&t, "--keep 2");
    CHECK(run && !run->ended);
    int before = t.nsigs;
    wait_ms(&t, 450);
    CHECK(t.nsigs == before);   /* nothing stopped: the ended ones' timers are cancelled, a run has none */
    CHECK(deadline_of(&t, run_with(&t, "--playlists")) == 60000 && deadline_of(&t, run_with(&t, "--tracks")) == 60000);
    CHECK(deadline_of(&t, run_with(&t, "--estimate")) == 300000 && deadline_of(&t, run) == 0);
    ev(&t, "{\"ev\":\"click\",\"id\":\"stop\"}");
    end_run(&t, run, 0, "");
    /* the Status tab's checks: their own deadline, stopped the same way */
    t.c->deadline_test_ms = 30;
    t.kept_sets = "2\t\t3";
    t.front = FRONT(CORE_TAB_STATUS);
    ev(&t, "{\"ev\":\"tab\",\"tab\":0}");
    vrun *tr = run_with(&t, "--tracks 2");
    for (int i = 0; i < 2000 && tr && !stops_of(&t, tr->id); i++) wait_ms(&t, 1);
    CHECK(tr && stops_of(&t, tr->id) == 1 && deadline_of(&t, tr) == 60000);
    give_library(&t, 4);
    vrun *ck = run_with(&t, "--keep 2 --wait-read 1 --playlist 2");
    CHECK(deadline_of(&t, ck) == 120000);   /* checkSet's attempt, then its fallback: 120 s each */
    end_run(&t, ck, 1, "");
    vrun *own = run_with(&t, "--keep 2 --wait-read 1 ");
    CHECK(own && own != ck && deadline_of(&t, own) == 120000);
    vt_end(&t);
    live_settled();
    LEAKCHECK();
}

/* a button or menu item that is off sends nothing (a disabled NSButton or NSMenuItem): an action's
 * click starts only from the state last shown, a menu choice only when validateMenuItem allows it */
static void t_click_gate(void) {
    vt t;
    vt_init(&t);
    t.admin = 0;   /* Stems Cache's buttons off: a standard account */
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    CHECK(FL(&t, F_UNINSTALL_CACHE_ENABLED) == 0 && FL(&t, F_UNINSTALL_ALL_ENABLED) == 0);
    t.admin = 1;   /* the facts moved, no refresh yet: the button is still off */
    ev(&t, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    ev(&t, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    vt_settle(&t);
    CHECK(t.ndlg == 0 && !t.c->flow_running);
    CHECK(logged(&t, "core: a button that is off is ignored") && logged(&t, "core: a menu item that is off is ignored"));
    ev(&t, "{\"ev\":\"activated\"}");   /* the refresh turns it on: the click starts its flow */
    vt_settle(&t);
    t.answer[0] = 1;
    ev(&t, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    vt_settle(&t);
    CHECKS(t.ndlg >= 1 && strstr(t.dlg[0].title, "Stems Cache"), t.dlg[0].title);
    vt_end(&t);
    LEAKCHECK();
}

/* LocationMonitor's memo in the core (mac-core §3.5): measured off main at start, on activation
 * and on a volume or bridge-state event, one at a time; its facts win over the cheap sheet's.
 * The plan's scenario: the folder's free space goes under its floor while the app is inactive,
 * with no volume, bridge-state or cache-root event, and activation shows it */
static void t_location_memo(void) {
    vt t;
    vt_init(&t);
    t.slow_location = 1;
    vt_core(&t);
    to_friday_estimated(&t);
    CHECK(t.loc_measures == 1 && t.c->v.loc.known && I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON);
    int playlists = runs_with(&t, "--playlists");
    pthread_mutex_lock(&t.mu);
    t.paused_floor = 5LL << 30;   /* under its floor while away */
    pthread_mutex_unlock(&t.mu);
    ev(&t, "{\"ev\":\"location\"}");   /* a refresh alone: the memo's floor, not the sheet's */
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON && runs_with(&t, "--playlists") == playlists);
    ev(&t, "{\"ev\":\"activated\"}");   /* measured again: it moved, so the tabs check (quietly) */
    vt_settle(&t);
    CHECK(t.loc_measures == 2 && I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED);
    CHECK(runs_with(&t, "--playlists") > playlists);
    give_library(&t, 4);
    give_tracks(&t, 3);   /* under its floor: no estimate */
    /* a drive unplugged: the unmount measures; two asks in one turn measure once more, not twice at once */
    pthread_mutex_lock(&t.mu);
    t.loc_kind = LOC_CHOSEN;
    t.loc_probe = PROBE_NOT_CONNECTED;
    t.loc_drive = "GLS";
    pthread_mutex_unlock(&t.mu);
    CHECK(core_event(t.c, "{\"ev\":\"unmount\",\"path\":\"/Volumes/GLS\"}", CORE_NTS) == CORE_OK);
    CHECK(core_event(t.c, "{\"ev\":\"bridge_state\"}", CORE_NTS) == CORE_OK && t.c->v.loc.again);
    vt_settle(&t);
    CHECK(t.loc_measures == 4 && !t.c->v.loc.working && !t.c->v.loc.again);
    CHECK(strstr(S(&t, F_LIGHT_CACHE_TIP), "GLS") != NULL);
    vt_end(&t);
    LEAKCHECK();
    /* the host declines to measure (nonzero): the memo goes, the cheap facts' snapshot is used
     * again, with no {"ev":"location"} */
    vt_init(&t);
    t.slow_location = 1;
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(t.c->v.loc.known && I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON);
    pthread_mutex_lock(&t.mu);
    t.slow_location = 0;
    t.paused_floor = 5LL << 30;
    pthread_mutex_unlock(&t.mu);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(!t.c->v.loc.known && I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED);
    CHECK(!t.c->v.loc.every && t.c->v.loc.size_kind == 0 && !t.c->v.loc.size_folder);   /* its timer and size go too */
    vt_end(&t);
    LEAKCHECK();
}

/* the stems' size walk in the core (LocationMonitor.measure/walk, mac-core §3.5): walked once the
 * folder in use is known, kept while it stays the folder in use and is under 10 minutes old, walked
 * again past that (the plan's scenario: the folder's size changes with no event, and the new value
 * shows), on the 10-minute timer, and after LOCATION_REMEASURE; the flows' steps read it */
static view_loc_now loc_now(vt *t) {
    view_loc_now n;
    view_loc_q q = {&n, &t->qa};
    view_loc_query(t->c, &q);
    return n;
}
/* the first held walk goes now (run in the pump); 0: none was held */
static int walk_go(vt *t) {
    pthread_mutex_lock(&t->mu);
    core_job *j = t->nheld_walk ? t->held_walk[0] : NULL;
    if (j) memmove(t->held_walk, t->held_walk + 1, (size_t)--t->nheld_walk * sizeof *t->held_walk);
    pthread_mutex_unlock(&t->mu);
    if (j) vpush(t, j);
    pump(t);
    return j != NULL;
}

/* review MR1: one walk at a time, as LocationMonitor's serial size queue: twenty measurements
 * during a slow walk give one more walk, not twenty, and the first one publishes; a Clear
 * (LOCATION_REMEASURE) during a walk drops that walk's result, and the next one publishes */
static int step_wait(vt *t) {   /* the flow's held step is asked (main pumped meanwhile) */
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&t->mu);
        int h = t->step_held;
        pthread_mutex_unlock(&t->mu);
        if (h) return 1;
        wait_ms(t, 1);
    }
    return 0;
}
static void step_go(vt *t) {
    pthread_mutex_lock(&t->mu);
    t->step_release = 1;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
}
static const char *size_words(char *buf, size_t n, int64_t bytes) {
    tbuf b;
    tb_init(&b, CORE_OS_MAC, buf, n);
    w_stems_size(&b, 1, bytes);
    return buf;
}

/* review MR2: LOCATION_NOW's kind 4 is the size of the memo snapshot it was asked with, even when
 * the memo moved to another folder (and published its size) while the step ran */
static void t_locnow_snapshot(void) {
    vt t;
    vt_init(&t);
    t.slow_location = 1;
    t.in_use = "/Users/x/S/cache";
    t.walk_bytes = 3ll << 30;
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(loc_now(&t).size_bytes == (3ll << 30));
    t.locnow_kind = 4;
    t.hold_step = CORE_STEP_LOCATION_NOW + 1;
    t.answer[0] = 1;   /* Cancel the complete uninstall's confirmation */
    ev(&t, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    CHECK(step_wait(&t));
    pthread_mutex_lock(&t.mu);
    t.in_use = "/Volumes/B/rbstemsplus";   /* meanwhile: another folder, walked and published */
    t.walk_bytes = 5ll << 30;
    pthread_mutex_unlock(&t.mu);
    ev(&t, "{\"ev\":\"activated\"}");
    wait_ms(&t, 30);
    CHECK(loc_now(&t).size_bytes == (5ll << 30));
    step_go(&t);
    vt_settle(&t);
    char a[128], b[128];
    size_words(a, sizeof a, 3ll << 30);
    size_words(b, sizeof b, 5ll << 30);
    CHECKS(t.ndlg >= 1 && strstr(t.dlg[t.ndlg - 1].msg, a) && !strstr(t.dlg[t.ndlg - 1].msg, b), t.dlg[t.ndlg - 1].msg);
    vt_end(&t);
    LEAKCHECK();
}

/* review MR11: the memo's drive and folder in use reach a flow whole, however long (a Windows path
 * or drive name): past the old 256- and 1024-byte buffers, in the query, in LOCATION_NOW's
 * argument and in the confirmation that names the drive */
static void t_loc_long(void) {
    static char drive[301], dir[1601];
    memset(drive, 'D', 300);
    drive[300] = 0;
    memcpy(dir, "/Volumes/", 9);
    for (int i = 9; i < 1600; i++) dir[i] = i % 100 == 0 ? '/' : (char)('a' + i % 26);
    dir[1599] = 'z';   /* the end, which a cut copy loses */
    dir[1600] = 0;
    vt t;
    vt_init(&t);
    t.slow_location = 1;
    t.slow_drive = drive;
    t.in_use = dir;
    t.paused_floor = 5LL << 30;   /* Stems Cache paused: the confirmation names the drive */
    t.walk_bytes = 1ll << 30;
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    view_loc_now n = loc_now(&t);
    CHECK(n.known && strlen(n.drive) == 300 && !strcmp(n.drive, drive));
    CHECK(strlen(n.in_use) == 1600 && !strcmp(n.in_use, dir));
    t.answer[0] = 1;   /* Cancel the complete uninstall's confirmation */
    ev(&t, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    vt_settle(&t);
    CHECKS(!strcmp(t.locnow_arg, dir), t.locnow_arg);
    t.ndlg = 0;
    t.answer[0] = 1;   /* Cancel the reinstall's confirmation */
    ev(&t, "{\"ev\":\"click\",\"id\":\"reinstall_cache\"}");
    vt_settle(&t);
    CHECKS(t.ndlg >= 1 && strstr(t.dlg[t.ndlg - 1].msg, drive), t.ndlg ? t.dlg[t.ndlg - 1].msg : "");
    vt_end(&t);
    LEAKCHECK();
}

/* review MR1 (MR11's): the memo's copies fail (out of memory): the flow stops, with no confirmation
 * and no LOCATION_NOW, never the host's path (a memo host keeps no snapshot); the next flow, with
 * the copies whole, names the drive again (the stopped flow ended and busy is off) */
static void t_loc_oom(void) {
    vt t;
    vt_init(&t);
    t.slow_location = 1;
    t.slow_drive = "DRIVEZQ";
    t.in_use = "/Volumes/DRIVEZQ/cache";
    t.paused_floor = 5LL << 30;
    t.walk_bytes = 1ll << 30;
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(loc_now(&t).known);
    t.answer[0] = 1;   /* Cancel */
    ev(&t, "{\"ev\":\"click\",\"id\":\"reinstall_cache\"}");
    vt_settle(&t);
    CHECKS(t.ndlg == 1 && strstr(t.dlg[0].msg, "DRIVEZQ"), t.ndlg ? t.dlg[0].msg : "");   /* the control */
    t.ndlg = 0;
    t.c->v.loc.test_fail_copies = 1;
    ev(&t, "{\"ev\":\"click\",\"id\":\"reinstall_cache\"}");
    vt_settle(&t);
    CHECKS(t.ndlg == 0 && t.c->v.loc.test_fail_copies == 0, t.ndlg ? t.dlg[0].msg : "");
    snprintf(t.locnow_arg, sizeof t.locnow_arg, "%s", "not asked");
    t.c->v.loc.test_fail_copies = 1;
    ev(&t, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    vt_settle(&t);
    CHECKS(t.ndlg == 0 && !strcmp(t.locnow_arg, "not asked"), t.locnow_arg);
    ev(&t, "{\"ev\":\"click\",\"id\":\"reinstall_cache\"}");
    vt_settle(&t);
    CHECKS(t.ndlg == 1 && strstr(t.dlg[0].msg, "DRIVEZQ"), t.ndlg ? t.dlg[0].msg : "");
    vt_end(&t);
    LEAKCHECK();
}

/* review MR3: the first measurement is update(size: true): with no folder in use, a chosen drive
 * that isn't connected says "not connected" (3) and an unreadable config.ini "failed" (2) at once */
static void t_first_size(void) {
    for (int k = 0; k < 2; k++) {
        vt t;
        vt_init(&t);
        t.slow_location = 1;
        t.loc_kind = k ? LOC_CONFIG_UNREADABLE : LOC_CHOSEN;
        t.loc_probe = PROBE_NOT_CONNECTED;
        vt_core(&t);
        ev(&t, "{\"ev\":\"start\"}");
        vt_settle(&t);
        CHECK(loc_now(&t).known && loc_now(&t).size_kind == (k ? 2 : 3) && t.walks == 0);
        vt_end(&t);
        LEAKCHECK();
    }
}

static void t_walk_one_at_a_time(void) {
    vt t;
    vt_init(&t);
    t.slow_location = 1;
    t.in_use = "/Users/x/S/cache";
    t.walk_bytes = 3ll << 30;
    t.hold_walks = 1;
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(t.nheld_walk == 1);
    for (int i = 0; i < 20; i++) {   /* each activation's watcher measure (M72) is a flow thread: joined each time */
        ev(&t, "{\"ev\":\"activated\"}");
        vt_settle(&t);
    }
    CHECK(t.nheld_walk == 1 && t.c->v.loc.walk_busy && t.c->v.loc.walk_again);
    CHECK(walk_go(&t) && loc_now(&t).size_kind == 1 && loc_now(&t).size_bytes == (3ll << 30));   /* the first publishes */
    CHECK(t.nheld_walk == 1 && t.walks == 1);   /* and one more follows */
    pthread_mutex_lock(&t.mu);
    t.walk_bytes = 4ll << 30;
    pthread_mutex_unlock(&t.mu);
    CHECK(walk_go(&t) && t.walks == 2 && loc_now(&t).size_bytes == (4ll << 30) && t.nheld_walk == 0);
    /* a Clear during a walk: that walk's result is dropped, the next one publishes */
    t.c->entry++;
    view_location_remeasure(t.c);
    core_leave(t.c);
    vt_settle(&t);
    CHECK(t.nheld_walk == 1);
    t.c->entry++;
    view_location_remeasure(t.c);   /* Cleared again while that walk goes */
    core_leave(t.c);
    vt_settle(&t);
    pthread_mutex_lock(&t.mu);
    t.walk_bytes = 1ll << 30;
    pthread_mutex_unlock(&t.mu);
    CHECK(walk_go(&t) && loc_now(&t).size_kind == 0 && t.nheld_walk == 1);   /* from before the Clear: not published */
    CHECK(walk_go(&t) && loc_now(&t).size_kind == 1 && loc_now(&t).size_bytes == (1ll << 30) && t.walks == 4);
    t.hold_walks = 0;
    vt_end(&t);
    LEAKCHECK();
}

/* review MS1, MS5, MS2: the folder in use changes during a walk (A's size is never shown as B's,
 * B is walked next, a measurement during B's walk starts no second one); the 10-minute tick
 * during a long walk doesn't void it (it publishes, and one more walk follows) */
static void t_walk_folder_and_tick(void) {
    vt t;
    vt_init(&t);
    t.slow_location = 1;
    t.in_use = "/Volumes/A/cache";
    t.walk_bytes = 3ll << 30;
    t.hold_walks = 1;
    vt_core(&t);
    t.c->size_test_now = 1000000000ull;
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(t.nheld_walk == 1);
    pthread_mutex_lock(&t.mu);
    t.in_use = "/Volumes/B/cache";
    pthread_mutex_unlock(&t.mu);
    ev(&t, "{\"ev\":\"activated\"}");   /* the folder in use moved to B during A's walk */
    vt_settle(&t);
    CHECK(t.nheld_walk == 1 && loc_now(&t).size_kind == 0);
    CHECK(walk_go(&t) && loc_now(&t).size_kind == 0 && t.nheld_walk == 1);   /* A's size isn't B's; B is walked next */
    ev(&t, "{\"ev\":\"activated\"}");   /* B not measured yet: this measure needs a size, during B's walk */
    vt_settle(&t);
    CHECK(t.nheld_walk == 1 && t.c->v.loc.walk_busy);   /* still one walk at a time */
    pthread_mutex_lock(&t.mu);
    t.walk_bytes = 5ll << 30;
    pthread_mutex_unlock(&t.mu);
    CHECK(walk_go(&t) && loc_now(&t).size_kind == 1 && loc_now(&t).size_bytes == (5ll << 30));
    CHECK(t.c->v.loc.size_folder && !strcmp(t.c->v.loc.size_folder, "/Volumes/B/cache"));
    t.hold_walks = 0;
    while (walk_go(&t)) {}
    vt_end(&t);
    LEAKCHECK();
    /* the 10-minute tick (update(size: true)) during a long walk */
    vt_init(&t);
    t.slow_location = 1;
    t.in_use = "/Volumes/A/cache";
    t.walk_bytes = 3ll << 30;
    t.hold_walks = 1;
    vt_core(&t);
    t.c->size_every_test_ms = 20;
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    for (int i = 0; i < 2000 && t.loc_measures < 4; i++) wait_ms(&t, 1);
    t.c->size_every_test_ms = 600000;
    wait_ms(&t, 60);
    CHECK(t.loc_measures >= 4 && t.nheld_walk == 1);
    CHECK(walk_go(&t) && loc_now(&t).size_kind == 1 && loc_now(&t).size_bytes == (3ll << 30));
    CHECK(t.nheld_walk == 1);   /* asked meanwhile: exactly one more */
    t.hold_walks = 0;
    while (walk_go(&t)) {}
    vt_end(&t);
    LEAKCHECK();
}

/* review MS4: the follow-up walk that can't start (out of memory) leaves the size due, so the next
 * measurement walks */
static void t_walk_again_fail(void) {
    vt t;
    vt_init(&t);
    t.slow_location = 1;
    t.in_use = "/Volumes/A/cache";
    t.walk_bytes = 3ll << 30;
    t.hold_walks = 1;
    vt_core(&t);
    t.c->size_test_now = 1000000000ull;
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(t.nheld_walk == 1);
    t.c->entry++;
    view_location_update(t.c, 1);   /* the tick, during A's walk */
    core_leave(t.c);
    vt_settle(&t);
    CHECK(t.c->v.loc.walk_again);
    core_test_fail_allocs(t.c, 1);
    CHECK(walk_go(&t) && loc_now(&t).size_kind == 1 && loc_now(&t).size_bytes == (3ll << 30));
    core_test_fail_allocs(t.c, 0);
    CHECK(t.nheld_walk == 0 && !t.c->v.loc.walk_busy);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(t.nheld_walk == 1 && loc_now(&t).size_kind == 1);   /* asked again; the size shown stays */
    t.hold_walks = 0;
    while (walk_go(&t)) {}
    vt_end(&t);
    LEAKCHECK();
}

static void t_size_walk(void) {
    vt t;
    vt_init(&t);
    t.slow_location = 1;
    t.in_use = "/Users/x/S/cache";
    t.walk_bytes = 3ll << 30;
    vt_core(&t);
    t.c->size_test_now = 1000000000ull;
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    view_loc_now n = loc_now(&t);
    CHECK(t.walks == 1 && n.known && n.size_kind == 1 && n.size_bytes == (3ll << 30));
    pthread_mutex_lock(&t.mu);
    t.walk_bytes = 5ll << 30;   /* the folder grew: no event says so */
    pthread_mutex_unlock(&t.mu);
    ev(&t, "{\"ev\":\"activated\"}");   /* under 10 minutes: the size stands */
    vt_settle(&t);
    CHECK(t.walks == 1 && loc_now(&t).size_bytes == (3ll << 30));
    t.c->size_test_now += 601ull * 1000000000u;
    ev(&t, "{\"ev\":\"activated\"}");   /* older than 10 minutes: walked again, the new value shows */
    vt_settle(&t);
    CHECK(t.walks == 2 && loc_now(&t).size_bytes == (5ll << 30));
    /* Settings reads the core's size (kind 4), as it reads the host's (kind 1) */
    t.set_size_kind = 1;
    t.answer[0] = 2;   /* Cancel */
    ev(&t, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    vt_settle(&t);
    CHECKS(t.ndlg == 1 && strstr(t.dlg[0].buttons, "Clear Saved Stems"), t.dlg[0].buttons);
    char host[512];
    snprintf(host, sizeof host, "%s", t.dlg[0].buttons);
    t.ndlg = 0;
    pthread_mutex_lock(&t.mu);
    t.walk_bytes = 7ll << 30;
    pthread_mutex_unlock(&t.mu);
    t.c->entry++;
    view_location_remeasure(t.c);   /* LOCATION_REMEASURE's effect: "measuring…", then walked */
    CHECK(loc_now(&t).size_kind == 0);
    core_leave(t.c);
    vt_settle(&t);
    CHECK(t.walks == 3 && loc_now(&t).size_bytes == (7ll << 30));
    t.set_size_kind = 4;
    ev(&t, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    vt_settle(&t);
    CHECKS(t.ndlg == 1 && !strcmp(t.dlg[0].buttons, host), t.dlg[0].buttons);
    /* the folder gone (a drive not connected): no walk, "not connected" */
    pthread_mutex_lock(&t.mu);
    t.in_use = NULL;
    t.loc_kind = LOC_CHOSEN;
    t.loc_probe = PROBE_NOT_CONNECTED;
    pthread_mutex_unlock(&t.mu);
    ev(&t, "{\"ev\":\"unmount\",\"path\":\"/Volumes/GLS\"}");
    vt_settle(&t);
    CHECK(t.walks == 3 && loc_now(&t).size_kind == 3);
    vt_end(&t);
    LEAKCHECK();
    /* the 10-minute timer: walked again with no event */
    vt_init(&t);
    t.slow_location = 1;
    t.in_use = "/Users/x/S/cache";
    t.walk_bytes = 1;
    vt_core(&t);
    t.c->size_every_test_ms = 30;
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(t.walks == 1);
    for (int i = 0; i < 2000 && t.walks < 3; i++) wait_ms(&t, 1);
    CHECK(t.walks >= 3 && t.loc_measures >= 3);
    t.c->size_every_test_ms = 600000;   /* the next arm waits: the free then cancels it before it fires */
    wait_ms(&t, 80);
    vt_end(&t);
    LEAKCHECK();
}

/* the OOM sweep's view.c site: the 10-minute timer fires while every job allocation fails. Its job
 * was reserved when it was armed, so it runs; the arm after it fails, and the next measure arms it
 * again, so the size keeps being measured with no event (a lost fire stopped it for the session) */
static void t_every_oom(void) {
    vt t;
    vt_init(&t);
    t.slow_location = 1;
    t.in_use = "/Users/x/S/cache";
    t.walk_bytes = 1;
    vt_core(&t);
    t.c->size_every_test_ms = 30;
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(t.walks == 1 && t.c->v.loc.every);
    CHECK(core_test_fail_allocs(t.c, 1 << 20) == CORE_OK);
    wait_ms(&t, 120);   /* a fire or more */
    core_test_fail_allocs(t.c, 0);
    ev(&t, "{\"ev\":\"activated\"}");   /* a measure: the timer armed again */
    vt_settle(&t);
    int w = t.walks;
    for (int i = 0; i < 2000 && t.walks < w + 2; i++) wait_ms(&t, 1);
    CHECK(t.walks >= w + 2 && t.c->v.loc.every);
    t.c->size_every_test_ms = 600000;
    wait_ms(&t, 80);
    vt_end(&t);
    LEAKCHECK();
}

/* review MR13 (e6d8c06): only the folder in use moves (two available folders, the same light and
 * floor): the location's key moves, so the Prepare tab checks again and a new estimate comes */
static void t_folder_moved(void) {
    vt t;
    vt_init(&t);
    t.in_use = "/Volumes/A/rbstemsplus";
    vt_core(&t);
    to_friday_estimated(&t);
    ev(&t, "{\"ev\":\"location\"}");   /* the first measurement */
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    int est = runs_with(&t, "--estimate"), lists = runs_with(&t, "--playlists");
    const char *tip = S(&t, F_LIGHT_CACHE_TIP);
    char was[512];
    snprintf(was, sizeof was, "%s", tip);
    ev(&t, "{\"ev\":\"location\"}");   /* nothing moved: no check */
    CHECK(runs_with(&t, "--estimate") == est && runs_with(&t, "--playlists") == lists);
    t.in_use = "/Volumes/B/rbstemsplus";
    ev(&t, "{\"ev\":\"location\"}");
    give_library(&t, 4);
    give_tracks(&t, 3);
    CHECK(!strcmp(S(&t, F_LIGHT_CACHE_TIP), was) && runs_with(&t, "--estimate") == est + 1);
    vt_end(&t);
    LEAKCHECK();
}

/* ---------------------------------------------------------------- the Prepare tab */

/* prepare-choose-estimate (b15cc8e, 236a1c0, 23585dd), prepare-folders' outline */
static void t_choose_estimate(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    core_commit(t.c);
    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    CHECK(!strcmp(S(&t, F_PREP_NAME), "Choose a playlist"));
    give_library(&t, 4);
    /* the outline: the tree in pre-order, folders open, titles with their track counts */
    CHECK(ROWS(&t, T_PREP_OUTLINE) == 4);
    CHECK(!strcmp(CELL(&t, C_OUT_TITLE, 0), "Sets") && CELLI(&t, C_OUT_OPEN, 0) == 1 && CELLI(&t, C_OUT_KIND, 0) == 0);
    CHECK(!strcmp(CELL(&t, C_OUT_TITLE, 1), "Friday  (3)") && CELLI(&t, C_OUT_PARENT, 1) == 0 && CELLI(&t, C_OUT_DEPTH, 1) == 1);
    CHECK(!strcmp(CELL(&t, C_OUT_TITLE, 2), "Smart") && CELLI(&t, C_OUT_SECONDARY, 2) == 1);
    CHECK(!strcmp(CELL(&t, C_OUT_TITLE, 3), "Saturday  (2)") && CELLI(&t, C_OUT_PARENT, 3) == -1);
    CHECK(!strcmp(S(&t, F_PREP_INFO), "Choose a playlist on the left to see what preparing it takes. Its stems are kept until the date below, then Stems Cache treats them like any others."));
    /* a folder: no estimate, no old summary (b15cc8e) */
    select_outline(&t, 0);
    CHECK(!strcmp(S(&t, F_PREP_INFO), "A folder: choose one of its playlists."));
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 0 && FL(&t, F_PREP_CHECK_ENABLED) == 1);
    select_outline(&t, 2);
    CHECK(!strcmp(S(&t, F_PREP_INFO), "Intelligent playlists can't be prepared: their tracks change as you add music."));
    /* a playlist: its tracks always listed, "Checking…" while it checks (23585dd) */
    select_outline(&t, 1);
    CHECK(!strcmp(S(&t, F_PREP_NAME), "Friday") && !strcmp(S(&t, F_PREP_INFO), "Checking the playlist…"));
    vrun *tr = run_with(&t, "--tracks");
    CHECK(tr && strstr(tr->argv, "--tracks 2 ") != NULL);
    give_tracks(&t, 3);
    CHECK(ROWS(&t, T_PREP_TRACKS) == 3 && !strcmp(CELL(&t, C_TRK_NAME, 1), "b.mp3"));
    CHECK(!strcmp(CELL(&t, C_TRK_RESULT, 0), "Checking…") && !strcmp(CELL(&t, C_TRK_RESULT, 2), "Checking…"));
    vrun *er = run_with(&t, "--estimate");
    CHECK(er && strstr(er->argv, "--estimate --playlist 2 --until ") && strstr(er->argv, "--cache /Users/x/S/cache --max-gb 20 "));
    /* results fill the rows as they come (236a1c0): one row changes, the rest stay */
    core_commit(t.c);
    uint64_t gen0 = GEN(&t, T_PREP_TRACKS);
    line(&t, er, EST_A);
    core_commit(t.c);
    CHECK(!strcmp(CELL(&t, C_TRK_RESULT, 0), "Ready") && !strcmp(CELL(&t, C_TRK_RESULT, 1), "Checking…"));
    CHECK(GEN(&t, T_PREP_TRACKS) == gen0 && t.trk_reload == 0 && t.trk_nchanged == 1 && t.trk_changed[0] == 0);
    line(&t, er, EST_B);
    line(&t, er, EST_C);
    CHECK(!strcmp(CELL(&t, C_TRK_RESULT, 1), "To prepare") && !strcmp(CELL(&t, C_TRK_WHY, 1), "4 min"));
    line(&t, er, EST_END);
    end_run(&t, er, 0, "");
    CHECK(!strcmp(S(&t, F_PREP_INFO), "1 track ready, 2 to prepare (7 min of music). About 512 MB and 5 min. Prepared playlists will use 1.5 GB of the 15.0 GB they may use (75% of Stems Cache's 20 GB limit)."));
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1 && !strcmp(S(&t, F_PREP_PREPARE_TITLE), "Prepare"));
    CHECK(core_menu_state(t.c, M_PREP_CHECK, NULL, NULL) & CORE_MENU_ENABLED);
    vt_end(&t);
    LEAKCHECK();
}

/* prepare-come-back-quiet (61a5393, 178c9ed): coming back checks again without clearing what is
 * shown, and never shows "Checking…" where a state was known */
static void t_come_back_quiet(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    const char *summary = S(&t, F_PREP_INFO);
    char before[4096];
    snprintf(before, sizeof before, "%s", summary);
    t.front = CORE_FRONT_WINDOW_LOADED | (CORE_TAB_PREPARE << CORE_FRONT_TAB_SHIFT);   /* another app in front */
    ev(&t, "{\"ev\":\"deactivated\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"activated\"}");
#define QUIET() do { CHECK(!strcmp(S(&t, F_PREP_INFO), before)); \
    CHECK(!strcmp(CELL(&t, C_TRK_RESULT, 0), "Ready") && !strcmp(CELL(&t, C_TRK_RESULT, 1), "To prepare")); \
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1); } while (0)
    QUIET();
    give_library(&t, 4);
    QUIET();
    give_tracks(&t, 3);
    QUIET();
    give_estimate(&t);
    QUIET();
#undef QUIET
    /* a refused playlist: no "Checking…" flash on coming back (178c9ed) */
    select_outline(&t, 3);   /* Saturday */
    vrun *tr = run_with(&t, "--tracks");
    line(&t, tr, "{\"file\":\"/Music/s.mp3\"}");
    end_run(&t, tr, 0, "");
    vrun *er = run_with(&t, "--estimate");
    line(&t, er, "{\"refused\":\"The library is busy.\"}");
    end_run(&t, er, 0, "");
    CHECK(!strcmp(S(&t, F_PREP_INFO), "The library is busy.") && !strcmp(CELL(&t, C_TRK_RESULT, 0), "Not checked"));
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(!strcmp(CELL(&t, C_TRK_RESULT, 0), "Not checked") && !strcmp(S(&t, F_PREP_INFO), "The library is busy."));
    vt_end(&t);
    LEAKCHECK();
}

/* prepare-held-click (5764bd4, 61a5393): Prepare clicked during the quiet re-check waits for it,
 * says "Checking…", and fires once the new estimate leaves the button on, in front, no sheet */
static void t_held_click(void) {
    for (int in_front = 0; in_front < 2; in_front++) {
        vt t;
        vt_init(&t);
        vt_core(&t);
        to_friday_estimated(&t);
        ev(&t, "{\"ev\":\"activated\"}");   /* the quiet re-check */
        CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1);
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
        CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 0 && !strcmp(S(&t, F_PREP_INFO), "Checking the playlist…"));
        CHECK(runs_with(&t, "--keep 2") == 0);   /* no run on the old estimate */
        give_library(&t, 4);
        if (!in_front) t.front = CORE_FRONT_WINDOW_LOADED | (CORE_TAB_PREPARE << CORE_FRONT_TAB_SHIFT);
        give_tracks(&t, 3);
        vrun *er = run_with(&t, "--estimate");
        if (!in_front) {   /* left before the estimate started: none now, and no run */
            CHECK(er && er->ended);
            CHECK(runs_with(&t, "--keep 2") == 0 && FL(&t, F_PREP_RUNNING) == 0);
            CHECK(strcmp(S(&t, F_PREP_INFO), "Checking the playlist…") != 0);   /* the held click's "Checking" ends */
            for (int32_t r = 0; r < ROWS(&t, T_PREP_TRACKS); r++) CHECK(strcmp(CELL(&t, C_TRK_RESULT, r), "Checking…") != 0);
        } else {
            give_estimate(&t);
            CHECK(runs_with(&t, "--keep 2") == 1 && FL(&t, F_PREP_RUNNING) == 1);
            vrun *rr = run_with(&t, "--keep 2");
            CHECK(rr && strstr(rr->argv, "--until ") && strstr(rr->argv, "--playlist 2 "));
        }
        vt_end(&t);
        LEAKCHECK();
    }
}

/* a check that moved on: the estimate under way is stopped and its late lines change nothing */
static void t_stale_estimate(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    give_library(&t, 4);
    select_outline(&t, 1);
    give_tracks(&t, 3);
    vrun *old = run_with(&t, "--estimate");
    line(&t, old, EST_A);
    select_outline(&t, 3);   /* Saturday: gen moves */
    CHECK(t.nsigs == 1 && t.sigs[0].run == old->id && t.sigs[0].sig == CORE_HELPER_STOP);
    line(&t, old, EST_B);    /* late: dropped */
    end_run(&t, old, 0, "");
    CHECK(ROWS(&t, T_PREP_TRACKS) == 0 && !strcmp(S(&t, F_PREP_NAME), "Saturday"));
    CHECK(!strcmp(S(&t, F_PREP_INFO), "Checking the playlist…"));
    vt_end(&t);
    LEAKCHECK();
}

/* prepare-folders (d8d5e64, a310fc2, b606a39): the folders the user closed stay closed across a
 * reload, except one holding the selection; Status' Prepare Again opens its playlist's folders */
static void t_folders(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    give_library(&t, 4);
    char e[160];
    snprintf(e, sizeof e, "{\"ev\":\"folder\",\"row\":0,\"open\":false,\"gen\":%llu}", (unsigned long long)GEN(&t, T_PREP_OUTLINE));
    ev(&t, e);
    CHECK(CELLI(&t, C_OUT_OPEN, 0) == 0);
    /* a new list (a playlist added): the folder stays closed */
    ev(&t, "{\"ev\":\"click\",\"id\":\"check_again\"}");   /* no selection: Check Again is off, nothing */
    ev(&t, "{\"ev\":\"menu\",\"id\":\"check_again\"}");    /* the menu item is off too */
    t.c->entry++;
    prep_load_playlists(t.c);
    core_leave(t.c);
    pump(&t);
    vrun *r = run_with(&t, "--playlists");
    for (int i = 0; i < 4; i++) line(&t, r, LIB[i]);
    line(&t, r, "{\"playlist\":\"5\",\"parent\":\"1\",\"name\":\"Sunday\",\"kind\":\"playlist\",\"tracks\":1}");
    end_run(&t, r, 0, "");
    CHECK(ROWS(&t, T_PREP_OUTLINE) == 5 && CELLI(&t, C_OUT_OPEN, 0) == 0);
    CHECK(!strcmp(CELL(&t, C_OUT_TITLE, 2), "Sunday  (1)"));   /* pre-order: the folder's children first */
    /* Prepare Again on Friday, inside the closed folder: the folder opens and Friday is selected */
    t.c->entry++;
    prep_select_id(t.c, "2");
    core_leave(t.c);
    pump(&t);
    CHECK(CELLI(&t, C_OUT_OPEN, 0) == 1 && I(&t, F_PREP_SELECTED_ROW) == 1 && !strcmp(S(&t, F_PREP_NAME), "Friday"));
    CHECK(I(&t, F_PREP_OUTLINE_SCROLL_SEQ) >= 1);
    vt_end(&t);
    LEAKCHECK();
}

/* prepare-run (5db560f, 64325ad, e47d1aa: the last run under the next estimate in one line),
 * status-sets (6c7380f: the run's line says the track's percentage, not its parts), prepare-stop:
 * the run's rows, line, bar and end; Pause, Stop; the notification when the app is in the background */
static void t_run(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    vrun *rr = run_with(&t, "--keep 2");
    CHECK(rr && FL(&t, F_PREP_RUNNING) == 1 && FL(&t, F_PREP_OUTLINE_ENABLED) == 0 && FL(&t, F_PREP_UNTIL_ENABLED) == 0);
    CHECK(!strcmp(S(&t, F_PREP_INFO), "Preparing “Friday”: 0%\nStarting…"));
    CHECK(core_menu_state(t.c, M_PREP_STOP, NULL, NULL) & CORE_MENU_ENABLED);
    line(&t, rr, "{\"at\":1,\"files\":3,\"chunk\":1,\"chunks\":2}");
    CHECK(!strcmp(S(&t, F_PREP_INFO), "Preparing “Friday”: 50%\nTrack 2 of 3."));   /* the line names the track; the bar how far */
    CHECK(I(&t, F_PREP_RUN_ROW) == 1 && !strcmp(CELL(&t, C_TRK_RESULT, 1), "Preparing…"));
    CHECK(!strcmp(CELL(&t, C_TRK_RESULT, 0), "Ready"));   /* the rows stay during a run */
    line(&t, rr, "{\"file\":\"/Music/b.mp3\",\"result\":\"prepared\",\"written\":4,\"already\":0}");
    CHECK(!strcmp(CELL(&t, C_TRK_RESULT, 1), "Prepared") && !strcmp(CELL(&t, C_TRK_WHY, 1), "separated now"));
    /* Pause and Resume; the menu's title follows */
    ev(&t, "{\"ev\":\"click\",\"id\":\"pause\"}");
    const char *title = NULL;
    core_menu_state(t.c, M_PREP_PAUSE, &title, NULL);
    CHECK(t.nsigs >= 1 && t.sigs[t.nsigs - 1].sig == CORE_HELPER_PAUSE && !strcmp(S(&t, F_PREP_PAUSE_TITLE), "Resume"));
    CHECK(title && !strcmp(title, "Resume"));
    CHECK(!strcmp(S(&t, F_PREP_INFO), "Paused “Friday”: 50%\nTrack 2 of 3."));
    /* Stop while paused: stopped, then let go on so it can stop */
    int n = t.nsigs;
    ev(&t, "{\"ev\":\"menu\",\"id\":\"stop\"}");
    CHECK(t.nsigs == n + 2 && t.sigs[n].sig == CORE_HELPER_STOP && t.sigs[n + 1].sig == CORE_HELPER_RESUME);
    CHECK(logged(&t, "prepare: stop asked") && strstr(S(&t, F_PREP_INFO), "Stopping…"));
    line(&t, rr, "{\"done\":1,\"kept_gb\":0.5,\"stopped\":\"stopped\"}");
    CHECK(!strcmp(S(&t, F_PREP_INFO), "Preparing “Friday”: 50%\n1 track done. Prepared playlists use 512 MB. Stopped: the tracks not done yet aren't prepared."));
    t.front = CORE_FRONT_WINDOW_LOADED | (CORE_TAB_PREPARE << CORE_FRONT_TAB_SHIFT);   /* in the background */
    end_run(&t, rr, 0, "");
    CHECK(FL(&t, F_PREP_RUNNING) == 0 && FL(&t, F_PREP_OUTLINE_ENABLED) == 1);
    CHECK(t.notifies == 1 && !strcmp(t.notify_title, "“Friday” isn't fully prepared"));
    CHECK(!strcmp(S(&t, F_PREP_INFO_SUB), ""));   /* the estimate dropped out of sight: the next check shows it */
    CHECK(!strcmp(S(&t, F_PREP_INFO), "1 track done. Prepared playlists use 512 MB. Stopped: the tracks not done yet aren't prepared."));
    /* restoring checks again; the last run's line under the new estimate */
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"activated\"}");
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    CHECK(!strcmp(S(&t, F_PREP_INFO_SUB), "Last time: 1 track done, stopped before the end."));
    vt_end(&t);
    LEAKCHECK();
}

/* stopAndWait from a flow's thread (the quit flow, preflight, Update): a stop that can't be asked
 * (out of memory) is a refusal, the run untouched; asked, it stops the run and waits for its end */
typedef struct { core_life *life; atomic_int done; int r; } stop_arg;
static void *stop_thread(void *p) {
    stop_arg *a = p;
    a->r = flow_stop_and_wait(a->life);
    atomic_store(&a->done, 1);
    return NULL;
}

static void t_stop_and_wait(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    vrun *rr = run_with(&t, "--keep 2");
    CHECK(rr && prep_running(t.c));
    /* the stop's query can't be made */
    stop_arg a = {t.c->life, 0, -1};
    pthread_t th;
    core_test_fail_allocs(t.c, 1);
    pthread_create(&th, NULL, stop_thread, &a);
    for (int i = 0; i < 5000 && !atomic_load(&a.done); i++) {
        if (!pump(&t)) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    }
    pthread_join(th, NULL);
    core_test_fail_allocs(t.c, 0);
    CHECK(atomic_load(&a.done) && a.r == 0);   /* a refusal, never a premature yes */
    CHECK(t.nsigs == 0 && prep_running(t.c) && FL(&t, F_PREP_RUNNING) == 1);   /* the run goes on */
    /* asked: STOP, then the run's end is waited for */
    stop_arg b = {t.c->life, 0, -1};
    pthread_create(&th, NULL, stop_thread, &b);
    for (int i = 0; i < 5000 && !t.nsigs; i++) {
        if (!pump(&t)) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    }
    CHECK(t.nsigs == 1 && t.sigs[0].run == rr->id && t.sigs[0].sig == CORE_HELPER_STOP);
    CHECK(!atomic_load(&b.done));   /* still waiting for the set to be written */
    line(&t, rr, "{\"done\":0,\"kept_gb\":0,\"stopped\":\"stopped\"}");
    end_run(&t, rr, 0, "");
    for (int i = 0; i < 5000 && !atomic_load(&b.done); i++) {
        if (!pump(&t)) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    }
    pthread_join(th, NULL);
    CHECK(atomic_load(&b.done) && b.r == 1 && !prep_running(t.c));
    vt_end(&t);
    LEAKCHECK();
}

/* prepare-check-cant-run (cfe858c, 99ad35d): what's in the way, the tracks still listed, no estimate */
static void t_cant_check(void) {
    vt t;
    vt_init(&t);
    t.enabled = 0;   /* Stems Cache turned off in config.ini */
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    give_library(&t, 4);
    select_outline(&t, 1);
    CHECK(!strcmp(S(&t, F_PREP_INFO), "Stems Cache is turned off in config.ini (enabled=0)."));
    give_tracks(&t, 3);
    CHECK(ROWS(&t, T_PREP_TRACKS) == 3 && !strcmp(CELL(&t, C_TRK_RESULT, 0), "Not checked"));
    CHECK(run_with(&t, "--estimate") == NULL && FL(&t, F_PREP_PREPARE_ENABLED) == 0);
    /* the library can't be read: the rows go, and why */
    ev(&t, "{\"ev\":\"click\",\"id\":\"check_again\"}");
    give_library(&t, 4);
    vrun *tr = run_with(&t, "--tracks");
    end_run(&t, tr, 1, "  the library is locked\n");
    CHECK(ROWS(&t, T_PREP_TRACKS) == 0);
    CHECK(!strcmp(S(&t, F_PREP_INFO), "Stems Cache is turned off in config.ini (enabled=0).\nthe library is locked"));
    vt_end(&t);
    LEAKCHECK();
}

/* the until date and its argument, on the fake clock; prepare-run (0da5b7a): the run's log line
 * names the playlist by its ID */
static void t_until(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    int64_t now = 1800000000;   /* 2027-01-15 */
    CHECK(core_test_set_now(t.c, now) == CORE_OK);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    struct tm tm;
    time_t later = (time_t)(now + 10 * 86400);
    localtime_r(&later, &tm);
    char want[16];
    snprintf(want, sizeof want, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    CHECK(!strcmp(S(&t, F_PREP_UNTIL), want));
    tm.tm_hour = 23;
    tm.tm_min = 59;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    char arg[48];
    snprintf(arg, sizeof arg, "--until %lld", (long long)mktime(&tm));
    give_library(&t, 4);
    select_outline(&t, 1);
    give_tracks(&t, 3);
    CHECK(run_with(&t, "--estimate") && strstr(run_with(&t, "--estimate")->argv, arg));
    /* only a real date in the picker's range: tomorrow to a year from now, by day */
    char day[48];
    CHECK(ev(&t, "{\"ev\":\"until\",\"date\":\"2027-1-5\"}") == CORE_E_ARG);
    CHECK(ev(&t, "{\"ev\":\"until\",\"date\":\"2020-01-01\"}") == CORE_E_ARG);   /* past */
    CHECK(ev(&t, "{\"ev\":\"until\",\"date\":\"2027-02-31\"}") == CORE_E_ARG);   /* no such day */
    struct tm d;
    time_t at = (time_t)(now + 366 * 86400);
    localtime_r(&at, &d);
    snprintf(arg, sizeof arg, "{\"ev\":\"until\",\"date\":\"%04d-%02d-%02d\"}", d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);
    CHECK(ev(&t, arg) == CORE_E_ARG);   /* a year and a day */
    at = (time_t)(now + 365 * 86400);
    localtime_r(&at, &d);
    snprintf(arg, sizeof arg, "{\"ev\":\"until\",\"date\":\"%04d-%02d-%02d\"}", d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);
    CHECK(ev(&t, arg) == CORE_OK);
    at = (time_t)(now + 86400);
    localtime_r(&at, &d);
    snprintf(day, sizeof day, "%04d-%02d-%02d", d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);
    snprintf(arg, sizeof arg, "{\"ev\":\"until\",\"date\":\"%s\"}", day);
    CHECK(ev(&t, arg) == CORE_OK && !strcmp(S(&t, F_PREP_UNTIL), day));   /* tomorrow */
    ev(&t, "{\"ev\":\"click\",\"id\":\"check_again\"}");
    give_library(&t, 4);
    give_tracks(&t, 3);
    d.tm_hour = 23;
    d.tm_min = 59;
    d.tm_sec = 0;
    d.tm_isdst = -1;
    snprintf(arg, sizeof arg, "--until %lld", (long long)mktime(&d));   /* the end of that day */
    CHECK(run_with(&t, "--estimate") && strstr(run_with(&t, "--estimate")->argv, arg));
    /* the run's line names the date as the host writes it (dateOnly, medium style), asked on a
     * worker when the date was set (the default, then this one), never on main; a run before the
     * answer came writes YYYY-MM-DD */
    give_estimate(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    char logl[96];
    snprintf(logl, sizeof logl, "prepare: playlist 2, kept until medium %s", day);
    CHECKS(logged(&t, logl), logl);
    CHECK(t.date_asks == 3 && t.date_on_main == 0);   /* the default, a year from now, tomorrow */
    ev(&t, "{\"ev\":\"click\",\"id\":\"stop\"}");
    end_run(&t, run_with(&t, "--keep 2"), 0, "");
    ev(&t, "{\"ev\":\"click\",\"id\":\"check_again\"}");
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    at = (time_t)(now + 2 * 86400);
    localtime_r(&at, &d);
    snprintf(day, sizeof day, "%04d-%02d-%02d", d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);
    snprintf(arg, sizeof arg, "{\"ev\":\"until\",\"date\":\"%s\"}", day);
    CHECK(core_event(t.c, arg, CORE_NTS) == CORE_OK);   /* its answer not run yet */
    CHECK(core_event(t.c, "{\"ev\":\"click\",\"id\":\"prepare\"}", CORE_NTS) == CORE_OK);
    pump(&t);
    snprintf(logl, sizeof logl, "prepare: playlist 2, kept until %s", day);
    CHECKS(logged(&t, logl), logl);
    CHECK(t.date_asks == 4 && t.date_on_main == 0);
    vt_end(&t);
    LEAKCHECK();
}

/* SO3: the tab left open past midnight: the picker's earliest day is now today, which the core
 * refuses; the Prepare section is drawn again so the picker shows the date the core keeps (the
 * one the run uses), never the refused one */
static void t_until_past_midnight(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    int64_t now = 1800000000;   /* 2027-01-15 */
    CHECK(core_test_set_now(t.c, now) == CORE_OK);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    struct tm d;
    time_t at = (time_t)(now + 86400);
    localtime_r(&at, &d);
    char day[16], arg[64];
    snprintf(day, sizeof day, "%04d-%02d-%02d", d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);
    snprintf(arg, sizeof arg, "{\"ev\":\"until\",\"date\":\"%s\"}", day);
    CHECK(ev(&t, arg) == CORE_OK);   /* tomorrow */
    vt_settle(&t);
    core_commit(t.c);
    CHECK(t.snap && !strcmp(core_str(t.snap, F_PREP_UNTIL, NULL), day));
    char later[16];   /* a later day, still in range after midnight */
    at = (time_t)(now + 5 * 86400);
    localtime_r(&at, &d);
    snprintf(later, sizeof later, "%04d-%02d-%02d", d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);
    snprintf(arg, sizeof arg, "{\"ev\":\"until\",\"date\":\"%s\"}", later);
    CHECK(ev(&t, arg) == CORE_OK);
    vt_settle(&t);
    core_commit(t.c);
    CHECK(core_test_set_now(t.c, now + 86400) == CORE_OK);   /* past midnight: "tomorrow" is today */
    uint64_t rev = core_snap_rev(t.snap, S_PREPARE);
    int renders = t.renders;
    snprintf(arg, sizeof arg, "{\"ev\":\"until\",\"date\":\"%s\"}", day);
    CHECK(ev(&t, arg) == CORE_E_ARG);
    core_commit(t.c);
    CHECK(t.renders > renders && core_snap_rev(t.snap, S_PREPARE) != rev);   /* drawn again ... */
    CHECK(!strcmp(core_str(t.snap, F_PREP_UNTIL, NULL), later));          /* ... from the date kept */
    vt_end(&t);
    LEAKCHECK();
}

/* the helper can't start: the library's read says so (today's err names the OS's reason) */
static void t_helper_refused(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    t.spawn_refuse = 1;
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    CHECK(!strcmp(S(&t, F_PREP_INFO), "rekordbox's library can't be read.") && !strcmp(S(&t, F_PREP_NAME), "rekordbox's playlists"));
    vt_end(&t);
    LEAKCHECK();
}

/* the core freed with runs under way: their accumulators go with their end jobs */
static void t_free_with_runs(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    vrun *r = run_with(&t, "--playlists");
    line(&t, r, LIB[0]);
    vt_settle(&t);
    core_free(t.c);
    core_helper_line(t.c, r->id, LIB[1], CORE_NTS);   /* after core_free: nothing */
    core_helper_ended(t.c, r->id, 0, "", 0);
    pump(&t);
    core_snap_release(t.snap);
    t.snap = NULL;
    pthread_cond_destroy(&t.cv);
    pthread_mutex_destroy(&t.mu);
    LEAKCHECK();
}

/* prepare-stop (9e34800): the end's notification says prepared only with nothing skipped and the
 * set saved; the last run counts the finished tracks and says when the set wasn't saved */
static void t_run_not_saved(void) {
    for (int saved = 0; saved < 2; saved++) {
        vt t;
        vt_init(&t);
        vt_core(&t);
        to_friday_estimated(&t);
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
        vrun *rr = run_with(&t, "--keep 2");
        line(&t, rr, "{\"file\":\"/Music/b.mp3\",\"result\":\"prepared\",\"written\":4,\"already\":0}");
        line(&t, rr, "{\"file\":\"/Music/c.mp3\",\"result\":\"prepared\",\"written\":4,\"already\":0}");
        line(&t, rr, saved ? "{\"done\":2,\"kept_gb\":0.5}" : "{\"done\":2,\"kept_gb\":0.5,\"kept_error\":\"no space\"}");
        t.front = CORE_FRONT_WINDOW_LOADED | (CORE_TAB_PREPARE << CORE_FRONT_TAB_SHIFT);
        end_run(&t, rr, 0, "");
        CHECKS(t.notifies == 1 && !strcmp(t.notify_title, saved ? "“Friday” is prepared" : "“Friday” isn't fully prepared"), t.notify_title);
        t.front = FRONT(CORE_TAB_PREPARE);
        ev(&t, "{\"ev\":\"activated\"}");
        give_library(&t, 4);
        give_tracks(&t, 3);
        give_estimate(&t);
        CHECKS(!strcmp(S(&t, F_PREP_INFO_SUB), saved ? "Last time: 2 tracks done." :
                       "Last time: 2 tracks done, the set couldn't be saved (see the log)."), S(&t, F_PREP_INFO_SUB));
        vt_end(&t);
        LEAKCHECK();
    }
}

static void dlg_reset(vt *t);

/* no-check-during-action (61b7375, 78ae5d9, d934b51), with the real flows (review MR9): the
 * complete uninstall cancelled at its location check (a short wait) checks nothing at its end; an
 * install brings the Manage tab to the front; the Prepare tab brought back during it checks as it
 * appears (1.3.0, recorded) but starts no run, and the install's end checks it exactly once more;
 * a tab not in front checks nothing until it appears */
static void t_action_checks(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.follow_tab = 1;   /* the window shows the tab the core asks for */
    dlg_reset(&t);
    int lists = runs_with(&t, "--playlists");
    t.answer[0] = 1;   /* Cancel, after "where the stems are" was checked */
    ev(&t, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    vt_settle(&t);
    CHECK(t.ndlg == 1 && !strcmp(t.dlg[0].title, "Uninstall RB Stems Plus?") && strstr(t.step_log, "24 "));
    CHECK(runs_with(&t, "--playlists") == lists && I(&t, F_TAB) == CORE_TAB_PREPARE);
    dlg_reset(&t);
    t.hold_step = CORE_STEP_DOWNLOAD + 1;   /* the install waits in its download */
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    CHECK(step_wait(&t) && I(&t, F_TAB) == CORE_TAB_MANAGE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");   /* the user brings the Prepare tab back meanwhile: it checks as it appears (1.3.0) */
    lists = runs_with(&t, "--playlists");
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    CHECK(!run_with(&t, "--keep 2") && FL(&t, F_PREP_RUNNING) == 0);   /* no run during an install */
    step_go(&t);
    vt_settle(&t);
    CHECK(t.ndlg == 1 && runs_with(&t, "--playlists") == lists + 1);   /* the install's end: checked again, once */
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    /* the Manage tab in front: a change checks nothing there; the Prepare tab checks as it appears */
    ev(&t, "{\"ev\":\"tab\",\"tab\":2}");
    t.c->entry++;
    view_prepare_changed(t.c, 0);
    core_leave(t.c);
    pump(&t);
    CHECK(runs_with(&t, "--playlists") == lists + 1);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    CHECK(runs_with(&t, "--playlists") == lists + 2);
    vt_end(&t);
    LEAKCHECK();
}

/* review MR10 (d934b51): Prepare's button and its menu item are off while an action runs (a real
 * install, held in its download) and on again once it has ended */
static void t_prepare_off_during_action(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.follow_tab = 1;
    dlg_reset(&t);
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1 && (core_menu_state(t.c, M_PREP_PLAYLIST, NULL, NULL) & CORE_MENU_ENABLED));
    t.hold_step = CORE_STEP_DOWNLOAD + 1;
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    CHECK(step_wait(&t));
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 0 && !(core_menu_state(t.c, M_PREP_PLAYLIST, NULL, NULL) & CORE_MENU_ENABLED));
    step_go(&t);
    vt_settle(&t);
    CHECK(!t.c->action_running && FL(&t, F_PREP_PREPARE_ENABLED) == 1 && (core_menu_state(t.c, M_PREP_PLAYLIST, NULL, NULL) & CORE_MENU_ENABLED));
    vt_end(&t);
    LEAKCHECK();
}

/* SO1: preflight's stop check and the action are one step, as 1.3.0's on main: from the check (no run,
 * or the run stopped) until the flow ends, no Prepare starts. (a) Prepare clicked while preflight's
 * REKORDBOX check runs (busy not on yet); (b) a held Prepare whose estimate ends then; (c) a run
 * stopped by Stop and Continue, Prepare clicked during REKORDBOX, then started again once the flow
 * ended. */
static void wait_sig(vt *t, int n) {
    for (int i = 0; i < 5000 && t->nsigs < n; i++) {
        if (!pump(t)) wait_ms(t, 1);
    }
}
static void t_prepare_reserved(void) {
    /* (a) */
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.follow_tab = 1;
    dlg_reset(&t);
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1);
    t.hold_step = CORE_STEP_REKORDBOX + 1;
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    CHECK(step_wait(&t));
    CHECK(!t.c->action_running && t.c->flow_running);   /* preflight: the busy state isn't on yet */
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 0);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    CHECK(runs_with(&t, "--keep 2") == 0 && !prep_running(t.c));
    step_go(&t);
    vt_settle(&t);
    CHECK(runs_with(&t, "--keep 2") == 0);   /* never beside the install */
    CHECK(!t.c->flow_running && !t.c->fl.prep_reserved && FL(&t, F_PREP_PREPARE_ENABLED) == 1);
    vt_end(&t);
    LEAKCHECK();
    /* (b) */
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    dlg_reset(&t);
    ev(&t, "{\"ev\":\"activated\"}");   /* the quiet re-check */
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");   /* held until the estimate ends */
    CHECK(t.c->p.held && runs_with(&t, "--keep 2") == 0);
    t.hold_step = CORE_STEP_REKORDBOX + 1;
    t.answer[0] = 1;   /* Cancel the install's confirmation */
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    CHECK(step_wait(&t));
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    CHECK(!t.c->p.held && runs_with(&t, "--keep 2") == 0 && !prep_running(t.c));   /* the held click dropped */
    step_go(&t);
    vt_settle(&t);
    CHECK(runs_with(&t, "--keep 2") == 0 && !t.c->fl.prep_reserved && FL(&t, F_PREP_PREPARE_ENABLED) == 1);
    vt_end(&t);
    LEAKCHECK();
    /* (c) */
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    vrun *rr = run_with(&t, "--keep 2");
    CHECK(rr && prep_running(t.c));
    dlg_reset(&t);
    t.answer[0] = 0;   /* Stop and Continue */
    t.answer[1] = 1;   /* Cancel the install's confirmation */
    t.hold_step = CORE_STEP_REKORDBOX + 1;
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    wait_sig(&t, 1);
    CHECK(t.nsigs == 1 && t.sigs[0].sig == CORE_HELPER_STOP);
    line(&t, rr, "{\"done\":0,\"kept_gb\":0,\"stopped\":\"stopped\"}");
    end_run(&t, rr, 0, "");
    for (int i = 0; i < 5000 && !t.step_held; i++) {
        if (!pump(&t)) wait_ms(&t, 1);
    }
    CHECK(step_wait(&t) && !prep_running(t.c) && t.c->fl.prep_reserved && FL(&t, F_PREP_PREPARE_ENABLED) == 0);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    CHECK(runs_with(&t, "--keep 2") == 1);
    step_go(&t);
    vt_settle(&t);
    CHECK(runs_with(&t, "--keep 2") == 1 && !t.c->fl.prep_reserved);
    ev(&t, "{\"ev\":\"activated\"}");   /* the stopped run's playlist checked again (t_run) */
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");   /* Prepare starts again once the flow ended */
    CHECK(runs_with(&t, "--keep 2") == 2 && prep_running(t.c));
    vt_end(&t);
    LEAKCHECK();
}

/* SQ1 (SO1's re-review): the queued actions (the watcher's Reinstall Now, the Stems Cache update
 * offer, "Action interrupted" > Continue) reserve Prepare in readyForQueuedAction's first query,
 * as 1.3.0 ran it and start() back to back on main; a run already going is left alone. */
static int step_asked(vt *t, core_step k) {
    char want[16], log[2100];
    snprintf(want, sizeof want, " %d ", (int)k);
    pthread_mutex_lock(&t->mu);
    snprintf(log, sizeof log, " %s", t->step_log);
    pthread_mutex_unlock(&t->mu);
    return strstr(log, want) != NULL;
}
static void step_log_clear(vt *t) {
    pthread_mutex_lock(&t->mu);
    t->step_log[0] = 0;
    pthread_mutex_unlock(&t->mu);
}
/* the queued action's REKORDBOX check is held: Prepare is off and a click starts nothing; then the
 * action runs to `step` with no run beside it, and the reservation ends with the flow */
static void sq1_during_check(vt *t, const char *ctx, core_step step) {
    CHECKS(step_wait(t), ctx);
    CHECKS(t->c->flow_running && !t->c->action_running && t->c->fl.prep_reserved == 1, ctx);
    CHECKS(FL(t, F_PREP_PREPARE_ENABLED) == 0, ctx);
    ev(t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    CHECKS(runs_with(t, "--keep 2") == 0 && !prep_running(t->c), ctx);
    step_go(t);
    vt_settle(t);
    CHECKS(step_asked(t, step) && runs_with(t, "--keep 2") == 0, ctx);
    CHECKS(!t->c->flow_running && t->c->fl.prep_reserved == 0, ctx);
}
/* the start-up's queued action (resume, then the cache offer) held at its REKORDBOX check while
 * the Prepare tab is shown, Friday chosen and estimated */
static void sq1_startup_held(vt *t) {
    ev(t, "{\"ev\":\"start\"}");
    core_commit(t->c);
    t->follow_tab = 0;
    t->hold_step = CORE_STEP_REKORDBOX + 1;
    ev(t, "{\"ev\":\"shown\"}");
    CHECK(step_wait(t));
    t->front = FRONT(CORE_TAB_PREPARE);
    ev(t, "{\"ev\":\"tab\",\"tab\":1}");
    give_library(t, 4);
    select_outline(t, 1);   /* Friday */
    give_tracks(t, 3);
    give_estimate(t);
}
static void t_prepare_reserved_queued(void) {
    vt t;
    /* 1. the watcher's request (F_REQUEST): Demucs v4 chosen and missing, reinstalled */
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.follow_tab = 1;
    dlg_reset(&t);
    step_log_clear(&t);
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1);
    t.chosen_model = 1;
    t.plus = 0;
    t.req_age1 = 11;
    t.hold_step = CORE_STEP_REKORDBOX + 1;
    ev(&t, "{\"ev\":\"check_request\"}");
    sq1_during_check(&t, "request", CORE_STEP_MODEL_COPY);
    CHECK(logged(&t, "watcher asked to reinstall") && t.req_age1 == 0);
    vt_end(&t);
    LEAKCHECK();
    /* 2. the Stems Cache update offer (F_CACHE_OFFER at start-up), answered Update */
    vt_init(&t);
    vt_core(&t);
    t.bridge_installed = "3333333333333333333333333333333333333333333333333333333333333333";
    sq1_startup_held(&t);
    sq1_during_check(&t, "cache offer", CORE_STEP_CACHE_PREPARE);
    CHECKS(t.ndlg >= 2 && !strcmp(t.dlg[1].title, "Stems Cache update"), t.dlg[1].title);   /* after the welcome */
    vt_end(&t);
    LEAKCHECK();
    /* 3. "Action interrupted" > Continue (check_resume) on a reinstall-plus */
    vt_init(&t);
    vt_core(&t);
    t.leftover_id = "reinstall-plus";
    t.leftover_label = "Reinstall Demucs v4";
    sq1_startup_held(&t);
    sq1_during_check(&t, "resume", CORE_STEP_MODEL_COPY);
    CHECKS(t.ndlg >= 2 && !strcmp(t.dlg[1].title, "Action interrupted"), t.dlg[1].title);   /* after the welcome */
    vt_end(&t);
    LEAKCHECK();
    /* 4. a held Prepare whose estimate ends during the held check starts nothing */
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    dlg_reset(&t);
    ev(&t, "{\"ev\":\"activated\"}");   /* the quiet re-check */
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");   /* held until the estimate ends */
    CHECK(t.c->p.held && runs_with(&t, "--keep 2") == 0);
    t.chosen_model = 1;
    t.plus = 0;
    t.req_age1 = 11;
    t.hold_step = CORE_STEP_REKORDBOX + 1;
    ev(&t, "{\"ev\":\"check_request\"}");
    CHECK(step_wait(&t));
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    CHECK(!t.c->p.held && runs_with(&t, "--keep 2") == 0 && !prep_running(t.c));   /* the held click dropped */
    step_go(&t);
    vt_settle(&t);
    CHECK(runs_with(&t, "--keep 2") == 0 && t.c->fl.prep_reserved == 0);
    vt_end(&t);
    LEAKCHECK();
    /* 5. every way the check says "not now" releases it: Cancel at "rekordbox open", the
     * updating refusal, a REKORDBOX step that fails */
    for (int way = 0; way < 3; way++) {
        static const char *const ctx[] = {"rekordbox open, Cancel", "rekordbox updating", "the check failed"};
        vt_init(&t);
        vt_core(&t);
        to_friday_estimated(&t);
        t.follow_tab = 1;
        dlg_reset(&t);
        t.chosen_model = 1;
        t.plus = 0;
        t.req_age1 = 11;
        if (way == 0) {
            t.rb_open_n = 2;   /* the updating check and the loop's */
            t.answer[0] = 1;   /* Cancel */
        } else if (way == 1) t.rb_upd_n = 1;
        else t.rb_fail_n = 1;
        ev(&t, "{\"ev\":\"check_request\"}");
        vt_settle(&t);
        CHECKS(!t.c->flow_running && t.c->fl.prep_reserved == 0 && FL(&t, F_PREP_PREPARE_ENABLED) == 1, ctx[way]);
        CHECKS(!step_asked(&t, CORE_STEP_MODEL_COPY), ctx[way]);
        CHECKS(way == 2 ? !logged(&t, "  rekordbox is updating: not started")
                        : (way == 0 ? logged(&t, "  rekordbox is open, the user cancelled: not started")
                                    : logged(&t, "  rekordbox is updating: not started")), ctx[way]);
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");   /* Prepare starts again */
        CHECKS(runs_with(&t, "--keep 2") == 1, ctx[way]);
        vt_end(&t);
        LEAKCHECK();
    }
#ifdef CORE_WIN_POLICY
    /* 6. Windows: rekordbox still closing (the check waits with no dialog): a Prepare click during
     * the wait starts nothing (the Prepare release: its tabs shown) */
    vt_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"prepare_tabs\":true,\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
    vt_core(&t);
    CHECK(t.c->os == CORE_OS_WIN);
    to_friday_estimated(&t);
    t.follow_tab = 1;
    dlg_reset(&t);
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1);
    t.c->life->win_closing_max_ms = 1500;
    t.chosen_model = 1;
    t.plus = 0;
    t.req_age1 = 11;
    t.rb_closing_n = 100000;   /* still closing past the deadline: then it counts as open */
    t.answer[0] = 1;           /* "rekordbox open": Cancel */
    ev(&t, "{\"ev\":\"check_request\"}");
    for (int i = 0; i < 1000 && !logged(&t, "  rekordbox is still closing: waiting"); i++) wait_ms(&t, 1);
    CHECK(logged(&t, "  rekordbox is still closing: waiting") && t.c->fl.prep_reserved == 1 && FL(&t, F_PREP_PREPARE_ENABLED) == 0);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    CHECK(runs_with(&t, "--keep 2") == 0 && !prep_running(t.c));
    vt_settle(&t);
    CHECK(runs_with(&t, "--keep 2") == 0 && !t.c->flow_running && t.c->fl.prep_reserved == 0);
    vt_end(&t);
    LEAKCHECK();
#endif
    /* 7. a Prepare already running when the request comes is left alone (1.3.0): no stop, and no
     * second run starts during the reservation */
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.follow_tab = 1;
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    vrun *rr = run_with(&t, "--keep 2");
    CHECK(rr && prep_running(t.c));
    dlg_reset(&t);
    int sigs = t.nsigs;
    t.chosen_model = 1;
    t.plus = 0;
    t.req_age1 = 11;
    t.hold_step = CORE_STEP_REKORDBOX + 1;
    ev(&t, "{\"ev\":\"check_request\"}");
    CHECK(step_wait(&t));
    CHECK(t.c->fl.prep_reserved == 1 && prep_running(t.c) && t.nsigs == sigs);   /* reserved, the run untouched */
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    CHECK(runs_with(&t, "--keep 2") == 1);
    step_go(&t);
    vt_settle(&t);
    CHECK(runs_with(&t, "--keep 2") == 1 && t.nsigs == sigs && t.c->fl.prep_reserved == 0);
    vt_end(&t);
    LEAKCHECK();
}

/* the probe's trigger.actionEnd (core_test_set_mark): once per action end, on main, before the
 * refresh it starts; nothing for a flow that never turned busy on (a cancelled confirmation) */
typedef struct { int n, off_main, outside_cb; char name[64]; vt *t; pthread_t main; } mark_rec;
static void rec_mark(void *ctx, const char *name) {
    mark_rec *m = ctx;
    m->n++;
    if (!pthread_equal(pthread_self(), m->main)) m->off_main++;
    if (m->t->c->cb_depth < 1) m->outside_cb++;   /* a host callback: no core entry from inside it */
    snprintf(m->name, sizeof m->name, "%s", name);
}
static void t_action_end_mark(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.follow_tab = 1;
    dlg_reset(&t);
    mark_rec m = {0, 0, 0, "", &t, pthread_self()};
    CHECK(core_test_set_mark(t.c, rec_mark, &m) == CORE_OK);
    t.answer[0] = 1;   /* Cancel the install's confirmation: no busy state, no mark */
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    vt_settle(&t);
    CHECK(m.n == 0);
    dlg_reset(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");   /* Install */
    vt_settle(&t);
    CHECK(m.n == 1 && !strcmp(m.name, "trigger.actionEnd") && m.off_main == 0 && m.outside_cb == 0 && !t.c->action_running);
    dlg_reset(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");   /* another action: one more */
    vt_settle(&t);
    CHECK(m.n == 2);
    CHECK(core_test_set_mark(t.c, NULL, NULL) == CORE_OK);   /* none again */
    dlg_reset(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    vt_settle(&t);
    CHECK(m.n == 2);
    vt_end(&t);
    LEAKCHECK();
}

/* AE3: busy(nil)'s EF_IDLE is reserved when busy turns on, so an allocation that fails at the
 * action's end can't leave the spinner, the status line and the buttons busy */
static void t_idle_reserved(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.follow_tab = 1;
    dlg_reset(&t);
    int64_t install_on = FL(&t, F_INSTALL_PLUS_ENABLED);
    mark_rec m = {0, 0, 0, "", &t, pthread_self()};
    CHECK(core_test_set_mark(t.c, rec_mark, &m) == CORE_OK);
    t.hold_step = CORE_STEP_REPORT_ZIP + 1;
    t.answer[0] = 1;   /* "Report saved": Done */
    ev(&t, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    CHECK(step_wait(&t));
    CHECK(t.c->action_running && FL(&t, F_SPINNER) == 1 && !strcmp(S(&t, F_STATUS_LINE), "Creating a report…"));
    CHECK(core_test_fail_allocs(t.c, 1) == CORE_OK);   /* the next job's allocation fails: busy(nil)'s, without the reservation */
    t.hold = 1;   /* "Report saved" stays up: the flow hasn't ended, so its end can't stand in for busy(nil) */
    step_go(&t);
    CHECK(wait_held(&t));
    wait_ms(&t, 20);
    core_commit(t.c);
    CHECK(!t.c->action_running && FL(&t, F_SPINNER) == 0 && strcmp(S(&t, F_STATUS_LINE), "Creating a report…") != 0);
    CHECK(FL(&t, F_INSTALL_PLUS_ENABLED) == install_on);   /* the buttons back */
    CHECK(m.n == 1);   /* busy(nil) ran: its probe mark too */
    release(&t);
    vt_settle(&t);
    core_test_fail_allocs(t.c, 0);
    core_commit(t.c);
    CHECK(!t.c->action_running && FL(&t, F_SPINNER) == 0 && m.n == 1);
    vt_end(&t);
    LEAKCHECK();
}

/* CF1: a flow stopped while busy (the facts query after Demucs v4's restore lost to an allocation)
 * never runs busy(nil): the flow's end does (flows_flow_end_busy), with its probe mark */
static void t_flow_end_busy(void) {
    vt t;
    vt_init(&t);
    t.plus = t.chosen_model = 1;   /* Demucs v4 in use: Uninstall Demucs v4 is on */
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    core_commit(t.c);
    ev(&t, "{\"ev\":\"shown\"}");
    vt_settle(&t);
    core_commit(t.c);
    t.follow_tab = 1;
    dlg_reset(&t);
    int64_t settings_on = FL(&t, F_SETTINGS_ENABLED);
    CHECK(settings_on == 1 && FL(&t, F_UNINSTALL_PLUS_ENABLED) == 1);
    mark_rec m = {0, 0, 0, "", &t, pthread_self()};
    CHECK(core_test_set_mark(t.c, rec_mark, &m) == CORE_OK);
    t.hold_step = CORE_STEP_MODEL_RESTORE + 1;
    ev(&t, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}");
    CHECK(step_wait(&t));
    char busy_line[128];
    snprintf(busy_line, sizeof busy_line, "%s", S(&t, F_STATUS_LINE));
    CHECK(t.c->action_running && FL(&t, F_SPINNER) == 1 && busy_line[0] && FL(&t, F_SETTINGS_ENABLED) == 0);
    CHECK(core_test_fail_allocs(t.c, 1) == CORE_OK);   /* the next job's: the facts query after the restore */
    step_go(&t);
    vt_settle(&t);
    core_test_fail_allocs(t.c, 0);
    core_commit(t.c);
    CHECK(!t.c->flow_running && !t.c->action_running && FL(&t, F_SPINNER) == 0 && strcmp(S(&t, F_STATUS_LINE), busy_line) != 0);
    CHECK(FL(&t, F_SETTINGS_ENABLED) == settings_on);   /* the buttons back */
    CHECK(m.n == 1 && !strcmp(m.name, "trigger.actionEnd"));
    vt_end(&t);
    LEAKCHECK();
}

/* CF2: busy(nil)'s reservation fails while EF_BUSY goes through, and busy(nil)'s own post fails at
 * the end. The flow's end restores the rendered view through the main queue. */
static void t_busy_unreserved(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.follow_tab = 1;
    dlg_reset(&t);
    mark_rec m = {0, 0, 0, "", &t, pthread_self()};
    CHECK(core_test_set_mark(t.c, rec_mark, &m) == CORE_OK);
    t.hold_step = CORE_STEP_BUSY_MARKER + 1;
    CHECK(t.snap && core_flag(t.snap, F_SETTINGS_ENABLED) == 1);
    t.answer[0] = 1;   /* "Report saved": Done */
    ev(&t, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    CHECK(step_wait(&t));
    CHECK(core_test_fail_alloc_after(t.c, 0, 1) == CORE_OK);   /* the next: fx_busy's reservation; EF_BUSY's goes through */
    pthread_mutex_lock(&t.mu);
    t.hold_step = CORE_STEP_REPORT_ZIP + 1;
    pthread_mutex_unlock(&t.mu);
    step_go(&t);
    int at_zip = 0;   /* the busy marker let go, the report's facts asked, then the zip held */
    for (int i = 0; i < 5000 && !at_zip; i++) {
        pthread_mutex_lock(&t.mu);
        at_zip = t.step_held && strstr(t.step_log, " 21 61 6 ") != NULL;
        pthread_mutex_unlock(&t.mu);
        if (!at_zip) wait_ms(&t, 1);
    }
    CHECK(at_zip);
    wait_ms(&t, 20);
    CHECK(atomic_load(&t.c->life->fail_after) < 0);   /* the failure was used */
    CHECK(t.c->action_running && t.snap && core_flag(t.snap, F_SPINNER) == 1 &&
          !strcmp(core_str(t.snap, F_STATUS_LINE, NULL), "Creating a report…") &&
          core_flag(t.snap, F_SETTINGS_ENABLED) == 0);
    CHECK(core_test_fail_allocs(t.c, 1) == CORE_OK);   /* busy(nil)'s own post, with no reservation */
    step_go(&t);
    vt_settle(&t);
    CHECK(atomic_load(&t.c->life->fail_allocs) == 0);
    CHECK(!t.c->flow_running && !t.c->action_running);
    CHECK(t.snap && core_flag(t.snap, F_SPINNER) == 0 &&
          strcmp(core_str(t.snap, F_STATUS_LINE, NULL), "Creating a report…") != 0 &&
          core_flag(t.snap, F_SETTINGS_ENABLED) == 1);
    CHECK(m.n == 1);
    vt_end(&t);
    LEAKCHECK();
}

/* CF4: the install's mark lost (fx_start's EF_ACTION_MARK, out of memory): busy(nil) carries it,
 * so the install's end still checks the Prepare tab again (as t_action_checks) */
static void t_mark_lost(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.follow_tab = 1;
    dlg_reset(&t);
    t.hold = 1;   /* "Install Demucs v4?" stays up until let go */
    t.hold_step = CORE_STEP_DOWNLOAD + 1;   /* then the install waits in its download */
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    CHECK(wait_held(&t));
    CHECK(core_test_fail_alloc_after(t.c, 1, 1) == CORE_OK);   /* revalidate's query goes through, the mark's post fails */
    release(&t);
    CHECK(step_wait(&t) && I(&t, F_TAB) == CORE_TAB_MANAGE);
    CHECK(atomic_load(&t.c->life->fail_after) < 0 && t.c->action_running && !t.c->fl.action_marked);   /* busy on, no mark */
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");   /* the Prepare tab in front: it checks as it appears */
    int lists = runs_with(&t, "--playlists");
    step_go(&t);
    vt_settle(&t);
    CHECK(!t.c->action_running && runs_with(&t, "--playlists") == lists + 1);   /* the install's end: checked again */
    vt_end(&t);
    LEAKCHECK();
}

/* OM3: Uninstall Demucs v4's mark lost (CF4 carries it on the reserved busy(nil)), then the flow
 * stops while busy (the facts query after the restore lost), so that busy(nil) is dropped: the
 * flow's end still checks the Prepare tab again */
static void t_mark_lost_abort(void) {
    vt t;
    vt_init(&t);
    t.plus = t.chosen_model = 1;   /* Demucs v4 in use: Uninstall Demucs v4 is on */
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    core_commit(t.c);
    ev(&t, "{\"ev\":\"shown\"}");
    vt_settle(&t);
    core_commit(t.c);
    t.follow_tab = 1;
    dlg_reset(&t);
    CHECK(FL(&t, F_UNINSTALL_PLUS_ENABLED) == 1);
    t.hold = 1;   /* "Uninstall Demucs v4?" stays up until let go */
    t.hold_step = CORE_STEP_MODEL_RESTORE + 1;   /* then the uninstall waits in its restore */
    ev(&t, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}");
    CHECK(wait_held(&t));
    CHECK(core_test_fail_alloc_after(t.c, 1, 1) == CORE_OK);   /* revalidate's query goes through, the mark's post fails */
    release(&t);
    CHECK(step_wait(&t));
    CHECK(atomic_load(&t.c->life->fail_after) < 0 && t.c->action_running && !t.c->fl.action_marked);   /* busy on, no mark */
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");   /* the Prepare tab in front: it checks as it appears */
    give_library(&t, 4);
    int lists = runs_with(&t, "--playlists");
    CHECK(core_test_fail_allocs(t.c, 1) == CORE_OK);   /* the facts query after the restore: the flow stops */
    step_go(&t);
    vt_settle(&t);
    core_test_fail_allocs(t.c, 0);
    core_commit(t.c);
    CHECK(!t.c->flow_running && !t.c->action_running && FL(&t, F_SPINNER) == 0);
    CHECK(runs_with(&t, "--playlists") == lists + 1);   /* the uninstall's end: checked again */
    vt_end(&t);
    LEAKCHECK();
}

/* OOM sweep (the AE3 follow-up): each scenario runs once per N with the Nth job allocation after
 * its start failing (core_test_fail_alloc_after), until a run leaves the failure unused. However
 * a flow then goes, it ends, and nothing is left stuck: no flow, no busy state, no spinner, the
 * start-up done, no waiting request, the Prepare tab not left checking with no run to end it. */
enum { OOM_INSTALL, OOM_REPORT, OOM_REQUEST, OOM_STARTUP, OOM_PREPARE, OOM_SCENARIOS };
static const char *const oom_names[] = {"install", "report", "request", "startup", "prepare check"};

static int oom_open_runs(vt *t) {
    int n = 0;
    for (int i = 0; i < t->nruns && i < 128; i++) n += !t->runs[i].ended;
    return n;
}
/* the helper's runs answered as the fake would, until none is left open (a few rounds) */
static void oom_feed_runs(vt *t) {
    for (int round = 0; round < 8 && oom_open_runs(t); round++) {
        for (int i = 0; i < t->nruns && i < 128; i++) {
            vrun *r = &t->runs[i];
            if (r->ended) continue;
            if (strstr(r->argv, "--playlists")) { for (int k = 0; k < 4; k++) line(t, r, LIB[k]); }
            else if (strstr(r->argv, "--tracks")) line(t, r, "{\"file\":\"/Music/a.mp3\"}");
            else if (strstr(r->argv, "--estimate")) { line(t, r, EST_A); line(t, r, EST_END); }
            end_run(t, r, 0, "");
        }
        vt_settle(t);
    }
}

static int req_left(vt *t) {   /* the watcher's request still waits (not taken) */
    pthread_mutex_lock(&t->mu);
    int r = t->req_age1 != 0;
    pthread_mutex_unlock(&t->mu);
    return r;
}

static int oom_run(int scenario, int n, int count, char *why, size_t whyn) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    why[0] = 0;
    if (scenario == OOM_STARTUP) {
        t.chosen_model = 1;
        t.plus = 0;
        t.req_age1 = 11;   /* a watcher's request waits: the start-up asks it */
        ev(&t, "{\"ev\":\"start\"}");
        core_commit(t.c);
        core_test_fail_alloc_after(t.c, n, count);
        ev(&t, "{\"ev\":\"shown\"}");
    } else {
        to_friday_estimated(&t);
        t.follow_tab = 1;
        dlg_reset(&t);
        if (scenario == OOM_REQUEST) {
            t.chosen_model = 1;
            t.plus = 0;
            t.req_age1 = 11;
        }
        core_test_fail_alloc_after(t.c, n, count);
        if (scenario == OOM_INSTALL) ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
        else if (scenario == OOM_REPORT) ev(&t, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
        else if (scenario == OOM_REQUEST) ev(&t, "{\"ev\":\"check_request\"}");
        else ev(&t, "{\"ev\":\"activated\"}");   /* OOM_PREPARE: the quiet re-check */
    }
    vt_settle(&t);
    oom_feed_runs(&t);
    int used = atomic_load(&t.c->life->fail_after) < 0;
    core_test_fail_alloc_after(t.c, -1, 0);
    core_test_fail_allocs(t.c, 0);
    ev(&t, "{\"ev\":\"activated\"}");   /* the user comes back: what is asked again then may run */
    vt_settle(&t);
    oom_feed_runs(&t);
    core_commit(t.c);
    const prep_state *p = &t.c->p;
    if (t.c->flow_running) snprintf(why, whyn, "a flow still runs");
    else if (t.c->action_running) snprintf(why, whyn, "the busy state is still on");
    else if (FL(&t, F_SPINNER)) snprintf(why, whyn, "the spinner still turns");
    else if (atomic_load(&t.c->life->flows_out)) snprintf(why, whyn, "a flow job is still out");
    else if (!t.c->fl.startup_done) snprintf(why, whyn, "the start-up never finished");
    else if (t.c->fl.request_waiting) snprintf(why, whyn, "a request still waits");
    else if (t.c->fl.prep_reserved) snprintf(why, whyn, "Prepare is still reserved");
    else if (scenario == OOM_STARTUP && req_left(&t)) snprintf(why, whyn, "the watcher's request wasn't taken");
    else if ((p->refreshing || p->estimating) && !oom_open_runs(&t)) snprintf(why, whyn, "the Prepare tab checks with no run");
    vt_end(&t);
    LEAKCHECK();
    return used;
}

static void oom_sweep(int s, int count) {
    {
        int n = 0;
        for (; n < 400; n++) {
            char why[96];
            int used = oom_run(s, n, count, why, sizeof why);
            if (why[0]) {
                char ctx[160];
                snprintf(ctx, sizeof ctx, "%s, allocation %d (x%d) fails: %s", oom_names[s], n, count, why);
                CHECKS(0, ctx);
            }
            if (!used) break;   /* the scenario made fewer allocations than n: every one was tried */
        }
        char ctx[96];
        snprintf(ctx, sizeof ctx, "%s (x%d): %d allocations swept", oom_names[s], count, n);
        CHECKS(n > 0 && n < 400, ctx);
    }
}
static void t_oom_sweep(void) {
    for (int count = 1; count <= 2; count++)   /* one allocation failing, and two in a row (a failure path that allocates) */
        for (int s = 0; s < OOM_SCENARIOS; s++) oom_sweep(s, count);
    /* OM1: the start-up with 3 to 6 in a row: its end's reservation, its fallback post and what
     * comes between; the flow's end still marks it done */
    for (int count = 3; count <= 6; count++) oom_sweep(OOM_STARTUP, count);
}

/* review MR12 (1ab82da): no check during an action: coming back to the app while the complete
 * uninstall checks where the stems are (its short wait) doesn't reload the Status tab until it ends */
static void t_status_quiet_during_action(void) {
    vt t;
    vt_init(&t);
    t.kept_sets = "2\t\t3";
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    give_library(&t, 4);
    dlg_reset(&t);
    int checks = runs_with(&t, "--wait-read"), lists = runs_with(&t, "--playlists");
    CHECK(checks >= 1);
    t.answer[0] = 1;   /* Cancel */
    t.hold_step = CORE_STEP_LOCATION_NOW + 1;
    ev(&t, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    CHECK(step_wait(&t) && t.c->action_running);
    ev(&t, "{\"ev\":\"activated\"}");
    wait_ms(&t, 20);
    CHECK(runs_with(&t, "--wait-read") == checks && runs_with(&t, "--playlists") == lists);
    step_go(&t);
    vt_settle(&t);
    CHECK(runs_with(&t, "--playlists") > lists);   /* its end: the quiet check coming back asked */
    vt_end(&t);
    LEAKCHECK();
}

/* settings-clear (664bfcd, 6c231bd): clearing saved stems checks the tab in front again (the
 * Prepare tab, the Status tab's sets); a hidden Status tab checks nothing */
static void t_clear_checks(void) {
    vt t;
    vt_init(&t);
    t.kept_sets = "2\tNov 22, 2026\t3\n";
    vt_core(&t);
    to_friday_estimated(&t);
    int lists = runs_with(&t, "--playlists"), checks = runs_with(&t, "--wait-read");
    t.c->entry++;
    view_prepare_changed(t.c, 0);   /* Clear Saved Stems' end (prepareChanged) */
    core_leave(t.c);
    pump(&t);
    CHECK(runs_with(&t, "--playlists") == lists + 1 && runs_with(&t, "--wait-read") == checks);
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    t.front = FRONT(CORE_TAB_STATUS);
    ev(&t, "{\"ev\":\"tab\",\"tab\":0}");
    give_library(&t, 4);
    checks = runs_with(&t, "--wait-read");
    CHECK(checks >= 1);
    t.c->entry++;
    view_prepare_changed(t.c, 0);
    core_leave(t.c);
    pump(&t);
    CHECK(runs_with(&t, "--wait-read") > checks && !strcmp(CELL(&t, C_SET_STATE, 0), "Checking…"));
    vt_end(&t);
    LEAKCHECK();
}

/* drive-replug (c674d1f): the chosen drive unplugged while rekordbox runs and its bridge hasn't
 * noticed (still "available"): no restart asked; back, the bridge not yet using it: "in a few
 * seconds" */
static void t_drive_light(void) {
    vt t;
    vt_init(&t);
    t.loc_kind = LOC_CHOSEN;
    t.loc_probe = PROBE_NOT_CONNECTED;
    t.loc_drive = "GLS";
    t.rb_running = 1;
    t.br_state = "available";
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache's drive \"GLS\" isn't connected: rekordbox separates tracks as usual. "
                   "Connect it: Stems Cache uses it again by itself. Renamed the drive, or does it show as \"GLS 1\"? Choose it "
                   "again in Settings: the stems on it are kept."), S(&t, F_LIGHT_CACHE_TIP));
    t.br_state = NULL;   /* no live bridge: rekordbox restarted to use it */
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(strstr(S(&t, F_LIGHT_CACHE_TIP), "Connect it, then restart rekordbox.") != NULL);
    t.loc_probe = PROBE_AVAILABLE;   /* plugged in again; the bridge's next check uses it */
    t.br_state = "unavailable";
    t.br_reason = "not-connected";
    t.br_drive = "GLS";
    ev(&t, "{\"ev\":\"activated\"}");
    CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache will use \"GLS\" again in a few seconds."), S(&t, F_LIGHT_CACHE_TIP));
    vt_end(&t);
    LEAKCHECK();
}

/* status-sets (dc50c5f, c5cb4bc): "No tracks in the playlist" by the check's counts, so an emptied
 * playlist is empty, not ready from the tracks it had; the Prepare tab offers nothing to prepare */
static void t_empty_playlist(void) {
    static const char *const EMPTY = "{\"estimate\":1,\"ready\":0,\"prepare\":0,\"skipped\":0,\"unchecked\":0,\"asking\":0}";
    vt t;
    vt_init(&t);
    t.kept_sets = "2\tNov 22, 2026\t3\n";
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    give_library(&t, 4);
    vrun *ck = run_with(&t, "--keep 2 --wait-read 1 --playlist 2");
    line(&t, ck, EMPTY);
    end_run(&t, ck, 0, "");
    CHECKS(!strcmp(CELL(&t, C_SET_STATE, 0), "No tracks in the playlist"), CELL(&t, C_SET_STATE, 0));
    vt_end(&t);
    LEAKCHECK();
}

/* install-offline-fails (0a63096): long work brings the Manage tab to the front, where its
 * status line and spinner are */
static void t_work_in_front(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    t.hold = 1;   /* "Install Demucs v4?" stays up until let go */
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    CHECK(wait_held(&t) && I(&t, F_TAB) == CORE_TAB_PREPARE);   /* the question first, on the tab shown */
    release(&t);
    for (int i = 0; i < 2000 && I(&t, F_TAB) != CORE_TAB_MANAGE; i++) wait_ms(&t, 1);
    CHECK(I(&t, F_TAB) == CORE_TAB_MANAGE);
    vt_settle(&t);
    vt_end(&t);
    LEAKCHECK();
}

/* ---------------------------------------------------------------- the Status tab */

/* status-sets (b606a39, a310fc2, 178c9ed, 6c7380f: Checking… while it checks, d76b06f: each check
 * reads the playlist as it is now, --playlist): the prepared playlists, their library counts and
 * checks; a known state stays while it is checked again; what the app changes stops the checks;
 * Prepare Again opens the playlist on the Prepare tab */
static void t_status_sets(void) {
    vt t;
    vt_init(&t);
    t.kept_sets = "2\tNov 22, 2026\t3\n4\t\t0\n";
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");   /* the Status tab appears: reload() */
    CHECK(ROWS(&t, T_STATUS_SETS) == 2);
    CHECK(!strcmp(CELL(&t, C_SET_NAME, 0), "Playlist 2") && !strcmp(CELL(&t, C_SET_TRACKS, 0), "3"));
    CHECK(!strcmp(CELL(&t, C_SET_UNTIL, 0), "Nov 22, 2026") && !strcmp(CELL(&t, C_SET_UNTIL, 1), "–"));
    CHECK(!strcmp(CELL(&t, C_SET_TRACKS, 1), "–") && !strcmp(CELL(&t, C_SET_STATE, 0), "Checking…"));
    /* the names, as the library has them now */
    give_library(&t, 4);
    CHECK(!strcmp(CELL(&t, C_SET_NAME, 0), "Friday") && !strcmp(CELL(&t, C_SET_NAME, 1), "Saturday"));
    /* the library's counts: --tracks per set */
    CHECK(runs_with(&t, "--tracks 2 ") == 1 && runs_with(&t, "--tracks 4 ") == 1);
    vrun *tr = run_with(&t, "--tracks 4 ");
    line(&t, tr, "{\"file\":\"/Music/s.mp3\"}");
    end_run(&t, tr, 0, "");
    CHECK(!strcmp(CELL(&t, C_SET_TRACKS, 1), "1"));
    /* each set's check: the playlist as it is now */
    vrun *ck = run_with(&t, "--keep 2 --wait-read 1 --playlist 2");
    CHECK(ck != NULL);
    line(&t, ck, EST_END);
    end_run(&t, ck, 0, "");
    CHECK(!strcmp(CELL(&t, C_SET_STATE, 0), "2 tracks to prepare"));
    /* the playlist can't be read: the set's own files */
    vrun *ck4 = run_with(&t, "--keep 4 --wait-read 1 --playlist 4");
    end_run(&t, ck4, 1, "");
    vrun *own = run_with(&t, "--keep 4 --wait-read 1 ");
    CHECK(own && own != ck4 && !strstr(own->argv, "--playlist"));
    line(&t, own, "{\"estimate\":1,\"ready\":1,\"prepare\":0,\"skipped\":0,\"unchecked\":0}");
    end_run(&t, own, 0, "");
    CHECK(!strcmp(CELL(&t, C_SET_STATE, 1), "Ready"));
    /* coming back: a known state stays while it is checked again */
    t.front = CORE_FRONT_WINDOW_LOADED | (CORE_TAB_STATUS << CORE_FRONT_TAB_SHIFT);
    ev(&t, "{\"ev\":\"deactivated\"}");
    t.front = FRONT(CORE_TAB_STATUS);
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(!strcmp(CELL(&t, C_SET_STATE, 0), "2 tracks to prepare") && !strcmp(CELL(&t, C_SET_STATE, 1), "Ready"));
    /* the app changed something: the checks under way stop, the states go */
    vrun *again = run_with(&t, "--keep 2 --wait-read 1 --playlist 2");
    int n = t.nsigs;
    t.c->entry++;
    view_prepare_changed(t.c, 0);
    core_leave(t.c);
    pump(&t);
    CHECK(t.nsigs > n && again && t.sigs[n].run == again->id && t.sigs[n].sig == CORE_HELPER_STOP);
    end_run(&t, again, 0, "");   /* late: dropped */
    CHECK(!strcmp(CELL(&t, C_SET_STATE, 0), "Checking…"));   /* shown, it checks again at once */
    /* the buttons follow the selection */
    char e[160];
    snprintf(e, sizeof e, "{\"ev\":\"select\",\"table\":\"T_STATUS_SETS\",\"row\":0,\"gen\":%llu}", (unsigned long long)GEN(&t, T_STATUS_SETS));
    ev(&t, e);
    CHECK(FL(&t, F_STATUS_AGAIN_ENABLED) == 1 && FL(&t, F_STATUS_STOP_ENABLED) == 1);
    /* Prepare Again: the Prepare tab, the playlist chosen and checked */
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare_again\"}");
    CHECK(I(&t, F_TAB) == CORE_TAB_PREPARE && !strcmp(S(&t, F_PREP_NAME), "Friday") && I(&t, F_PREP_SELECTED_ROW) == 1);
    vt_end(&t);
    LEAKCHECK();
}

/* the run's progress in its set's row and the Status tab's run line */
static void t_status_run(void) {
    vt t;
    vt_init(&t);
    t.kept_sets = "2\tNov 22, 2026\t3\n";
    vt_core(&t);
    to_friday_estimated(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    vrun *rr = run_with(&t, "--keep 2 --playlist 2");
    CHECK(rr != NULL);
    line(&t, rr, "{\"at\":0,\"files\":3,\"chunk\":1,\"chunks\":2}");
    CHECK(!strcmp(S(&t, F_STATUS_RUN_LINE), "Preparing “Friday”: 16%"));
    CHECK(ROWS(&t, T_STATUS_SETS) == 1 && !strcmp(CELL(&t, C_SET_STATE, 0), "Preparing “Friday”: 16%"));
    vt_end(&t);
    LEAKCHECK();
}


/* ---------------------------------------------------------------- prepareAction's dialogs (flows) */

/* the dialogs asked from here on: the start-up flow's are done and forgotten */
static void dlg_reset(vt *t) {
    vt_settle(t);
    pthread_mutex_lock(&t->mu);
    t->ndlg = 0;
    memset(t->answer, 0, sizeof t->answer);
    t->nsteps = 0;
    pthread_mutex_unlock(&t->mu);
}

/* Friday's estimate with `end` as its estimate line */
static void give_estimate_end(vt *t, const char *end) {
    vrun *r = run_with(t, "--estimate");
    CHECK(r && !r->ended);
    line(t, r, EST_A);
    line(t, r, EST_B);
    line(t, r, EST_C);
    line(t, r, end);
    end_run(t, r, 0, "");
}

static void to_friday_with(vt *t, const char *end) {
    ev(t, "{\"ev\":\"start\"}");
    core_commit(t->c);
    ev(t, "{\"ev\":\"shown\"}");
    t->front = FRONT(CORE_TAB_PREPARE);
    ev(t, "{\"ev\":\"tab\",\"tab\":1}");
    give_library(t, 4);
    select_outline(t, 1);   /* Friday */
    give_tracks(t, 3);
    give_estimate_end(t, end);
    dlg_reset(t);
}

static const char *const EST_NO_FIT =
    "{\"estimate\":1,\"ready\":1,\"prepare\":2,\"skipped\":0,\"unchecked\":0,\"asking\":0,\"fit_files\":2,\"time_s\":300,"
    "\"need_gb\":6,\"kept_gb\":10,\"share_gb\":15,\"max_gb\":20,\"max_gb_needed\":25.25,\"fits\":false,\"disk_ok\":true,\"folders\":[]}";
static const char *const EST_NO_FIT_0 =   /* no track fits: no "Prepare the First N Tracks" */
    "{\"estimate\":1,\"ready\":1,\"prepare\":2,\"skipped\":0,\"unchecked\":0,\"asking\":0,\"fit_files\":0,\"time_s\":300,"
    "\"need_gb\":6,\"kept_gb\":10,\"share_gb\":15,\"max_gb\":20,\"max_gb_needed\":25.25,\"fits\":false,\"disk_ok\":true,\"folders\":[]}";
static const char *const EST_NO_FIT_1 =   /* EST_NO_FIT with one track fitting: a different estimate */
    "{\"estimate\":1,\"ready\":1,\"prepare\":2,\"skipped\":0,\"unchecked\":0,\"asking\":0,\"fit_files\":1,\"time_s\":300,"
    "\"need_gb\":6,\"kept_gb\":10,\"share_gb\":15,\"max_gb\":20,\"max_gb_needed\":25.25,\"fits\":false,\"disk_ok\":true,\"folders\":[]}";
static const char *const FIT_MSG =
    "Prepared playlists may use 75% of Stems Cache's 20 GB limit (15.0 GB), the rest stays for the tracks rekordbox "
    "separates during a set. This playlist would take them to 16.0 GB.";

/* prepare-first-n (38a4cbd, 9e52915, 2a53f9a, 692e695: the First N run keeps the whole playlist,
 * skipped ones included, with --first N): "doesn't fit" (PrepareView.swift:765): the first
 * tracks that fit, with the whole playlist and --first N; the limit raised (writeConfig, its log
 * line), then the whole playlist; Cancel; a write that fails; no room for a higher limit */
static void t_doesnt_fit(void) {
    for (int answer = 0; answer < 4; answer++) {   /* First N, Raise, Cancel, Raise with the write failing */
        vt t;
        vt_init(&t);
        t.config_fail = answer == 3;
        vt_core(&t);
        to_friday_with(&t, EST_NO_FIT);
        CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1);
        t.answer[0] = answer == 3 ? 1 : answer;
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
        vt_settle(&t);
        CHECK(t.ndlg >= 1 && !strcmp(t.dlg[0].title, "“Friday” doesn't fit"));
        CHECKS(!strcmp(t.dlg[0].msg, FIT_MSG), t.dlg[0].msg);
        CHECKS(!strcmp(t.dlg[0].buttons, "Prepare the First 2 Tracks | Raise the Limit to 25.3 GB | Cancel"), t.dlg[0].buttons);
        CHECK(stepped(&t, "FREE_SPACE /Users/x/S/cache"));   /* this Mac's Stems Cache folder */
        vrun *rr = run_with(&t, "--keep 2");
        if (answer == 0) {
            CHECK(rr && strstr(rr->argv, "--keep 2 --playlist 2 --first 2 ") && FL(&t, F_PREP_RUNNING) == 1);
            CHECK(t.ndlg == 1 && !stepped(&t, "CONFIG_WRITE max_gb 25.3 max_days 30"));
            /* 2a53f9a: Prepare the First N isn't the whole playlist: "isn't fully prepared" */
            line(&t, rr, "{\"file\":\"/Music/a.mp3\",\"result\":\"prepared\",\"written\":4,\"already\":0}");
            line(&t, rr, "{\"file\":\"/Music/b.mp3\",\"result\":\"prepared\",\"written\":4,\"already\":0}");
            line(&t, rr, "{\"done\":2,\"kept_gb\":0.5}");
            t.front = CORE_FRONT_WINDOW_LOADED | (CORE_TAB_PREPARE << CORE_FRONT_TAB_SHIFT);
            end_run(&t, rr, 0, "");
            CHECKS(t.notifies == 1 && !strcmp(t.notify_title, "“Friday” isn't fully prepared"), t.notify_title);
        } else if (answer == 1) {
            CHECK(stepped(&t, "CONFIG_WRITE max_gb 25.3 max_days 30"));
            CHECK(logged(&t, "settings: max_gb=25.3 (raised to prepare playlist 2)"));
            CHECK(rr && strstr(rr->argv, "--keep 2 --playlist 2 ") && !strstr(rr->argv, "--first"));
            CHECKS(rr && strstr(rr->argv, "--max-gb 25.3 "), rr ? rr->argv : "");   /* the run reads the raised limit */
        } else if (answer == 2) {
            CHECK(!rr && t.ndlg == 1 && t.nsteps == 1 && FL(&t, F_PREP_RUNNING) == 0);
        } else {   /* the settings file couldn't be written */
            CHECK(!rr && t.ndlg == 2 && !strcmp(t.dlg[1].title, "Limit not raised"));
            CHECK(!strcmp(t.dlg[1].msg, "RB Stems Plus couldn't write its settings file. See the log.") && !strcmp(t.dlg[1].buttons, "OK"));
            CHECK(!logged(&t, "settings: max_gb=25.3 (raised to prepare playlist 2)"));
        }
        vt_end(&t);
        LEAKCHECK();
    }
    /* 2 GiB free past the 1 GiB spare, 5.3 GB short: no Raise; free space unknown counts 0 */
    for (int unknown = 0; unknown < 2; unknown++) {
        vt t;
        vt_init(&t);
        t.free_bytes = unknown ? -1 : 3ll << 30;
        vt_core(&t);
        to_friday_with(&t, EST_NO_FIT);
        t.answer[0] = 1;   /* Cancel */
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
        vt_settle(&t);
        char want[512];
        snprintf(want, sizeof want, "%s The disk doesn't have room for a higher limit.", FIT_MSG);
        CHECKS(t.ndlg == 1 && !strcmp(t.dlg[0].msg, want), t.dlg[0].msg);
        CHECK(!strcmp(t.dlg[0].buttons, "Prepare the First 2 Tracks | Cancel") && !run_with(&t, "--keep 2"));
        vt_end(&t);
        LEAKCHECK();
    }
    /* no track fits (fit_files 0): Raise and Cancel only, and Raise (button 0) raises and runs */
    {
        vt t;
        vt_init(&t);
        vt_core(&t);
        to_friday_with(&t, EST_NO_FIT_0);
        t.answer[0] = 0;
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
        vt_settle(&t);
        CHECK(t.ndlg == 1 && !strcmp(t.dlg[0].title, "“Friday” doesn't fit"));
        CHECKS(!strcmp(t.dlg[0].buttons, "Raise the Limit to 25.3 GB | Cancel"), t.dlg[0].buttons);
        CHECK(stepped(&t, "CONFIG_WRITE max_gb 25.3 max_days 30"));
        CHECK(logged(&t, "settings: max_gb=25.3 (raised to prepare playlist 2)"));
        vrun *rr = run_with(&t, "--keep 2");
        CHECKS(rr && strstr(rr->argv, "--keep 2 --playlist 2 ") && !strstr(rr->argv, "--first") && strstr(rr->argv, "--max-gb 25.3 "),
               rr ? rr->argv : "");
        vt_end(&t);
        LEAKCHECK();
    }
    /* config.ini has no max_days (readConfig().maxDays nil): "Limit not raised", nothing written */
    {
        vt t;
        vt_init(&t);
        t.max_days = NULL;
        vt_core(&t);
        to_friday_with(&t, EST_NO_FIT);
        t.answer[0] = 1;   /* Raise the Limit */
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
        vt_settle(&t);
        CHECK(t.ndlg == 2 && !strcmp(t.dlg[1].title, "Limit not raised"));
        CHECK(t.nsteps == 1 && stepped(&t, "FREE_SPACE /Users/x/S/cache"));   /* no CONFIG_WRITE */
        CHECK(!logged(&t, "settings: max_gb=25.3 (raised to prepare playlist 2)"));
        CHECK(!run_with(&t, "--keep 2") && FL(&t, F_PREP_RUNNING) == 0);
        vt_end(&t);
        LEAKCHECK();
    }
}

/* "Not enough free space" (PrepareView.swift:777): Prepare runs anyway, Cancel doesn't */
static void t_no_space(void) {
    static const char *const end =
        "{\"estimate\":1,\"ready\":1,\"prepare\":2,\"skipped\":0,\"unchecked\":0,\"asking\":0,\"fit_files\":3,\"time_s\":300,"
        "\"need_gb\":0.5,\"kept_gb\":1.0,\"share_gb\":15,\"max_gb\":20,\"max_gb_needed\":0,\"fits\":true,\"disk_ok\":false,\"folders\":[]}";
    for (int answer = 0; answer < 2; answer++) {
        vt t;
        vt_init(&t);
        vt_core(&t);
        to_friday_with(&t, end);
        t.answer[0] = answer;
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
        vt_settle(&t);
        CHECK(t.ndlg == 1 && !strcmp(t.dlg[0].title, "Not enough free space"));
        CHECKS(!strcmp(t.dlg[0].msg, "The disk Stems Cache saves on doesn't have room for 512 MB more. Prepare anyway? "
                                     "It stops when the disk gets low."), t.dlg[0].msg);
        CHECK(!strcmp(t.dlg[0].buttons, "Prepare | Cancel") && t.nsteps == 0);
        vrun *rr = run_with(&t, "--keep 2");
        if (answer == 0) CHECK(rr && strstr(rr->argv, "--keep 2 --playlist 2 ") && !strstr(rr->argv, "--first"));
        else CHECK(!rr && FL(&t, F_PREP_RUNNING) == 0);
        vt_end(&t);
        LEAKCHECK();
    }
}

/* "Can't prepare “x”" with the refusal [OK] (PrepareView.swift:760): the button is off for a
 * refused playlist, so the flow is asked directly */
static void t_refused(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    dlg_reset(&t);
    prep_estimate e;
    prep_estimate_init(&e);
    e.has_refused = 1;
    e.refused = (pstr){"The playlist is too long.", 25};
    t.c->entry++;
    CHECK(flows_prepare_ask(t.c, PREP_ASK_REFUSED, "2", "Friday", &e, NULL, t.c->p.est_seq) == CORE_OK);
    core_leave(t.c);
    vt_settle(&t);
    CHECK(t.ndlg == 1 && !strcmp(t.dlg[0].title, "Can't prepare “Friday”"));
    CHECK(!strcmp(t.dlg[0].msg, "The playlist is too long.") && !strcmp(t.dlg[0].buttons, "OK"));
    CHECK(!run_with(&t, "--keep 2"));
    vt_end(&t);
    LEAKCHECK();
}

/* the app left during the sheet and back to a new check (`estimate == e`): no run on what the
 * dialog said, the playlist checked again; and the core freed under the dialog: nothing after it */
static void t_dialog_moved(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_with(&t, EST_NO_FIT);
    t.hold = 1;   /* Prepare the First 2 Tracks, once the test lets it go */
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    CHECK(wait_held(&t));
    int checks = runs_with(&t, "--estimate");
    ev(&t, "{\"ev\":\"click\",\"id\":\"check_again\"}");   /* a new check under the sheet */
    release(&t);
    vt_settle(&t);
    CHECK(!run_with(&t, "--keep 2") && FL(&t, F_PREP_RUNNING) == 0);
    give_library(&t, 4);
    give_tracks(&t, 3);
    CHECK(runs_with(&t, "--estimate") > checks);
    vt_end(&t);
    LEAKCHECK();

    vt_init(&t);
    vt_core(&t);
    to_friday_with(&t, EST_NO_FIT);
    t.hold = 1;
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    CHECK(wait_held(&t));
    core_free(t.c);
    release(&t);
    vt_settle(&t);
    CHECK(!run_with(&t, "--keep 2") && t.nsteps == 1);   /* FREE_SPACE only */
    for (int i = 0; i < t.nruns && i < 128; i++)
        if (!t.runs[i].ended) {
            t.runs[i].ended = 1;
            core_helper_ended(t.c, t.runs[i].id, 0, "", 0);
        }
    pump(&t);
    core_snap_release(t.snap);
    t.snap = NULL;
    pthread_cond_destroy(&t.cv);
    pthread_mutex_destroy(&t.mu);
    LEAKCHECK();
}

/* VM4: config.ini or the location changed under a Prepare dialog, with no event: the answer
 * reads the facts again and repeats prepareAction's guard, so no run on what the dialog was
 * about, and the playlist is checked again (1.3.0 never runs on facts it didn't check). Raise's
 * own new limit is the one expected. */
static void t_dialog_facts(void) {
    for (int change = 0; change < 3; change++) {   /* the limit edited, the location gone, nothing */
        vt t;
        vt_init(&t);
        vt_core(&t);
        to_friday_with(&t, EST_NO_FIT);
        t.hold = 1;   /* Prepare the First 2 Tracks, once the test lets it go */
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
        CHECK(wait_held(&t));
        int checks = runs_with(&t, "--estimate");
        pthread_mutex_lock(&t.mu);
        if (change == 0) t.max_gb = "30";
        if (change == 1) t.cfg_location = 2;   /* config.ini's location set wrongly */
        pthread_mutex_unlock(&t.mu);
        release(&t);
        vt_settle(&t);
        vrun *rr = run_with(&t, "--keep 2");
        if (change == 0) {   /* checked again with the new limit */
            CHECK(!rr && FL(&t, F_PREP_RUNNING) == 0);
            give_tracks(&t, 3);
            CHECK(runs_with(&t, "--estimate") == checks + 1 && strstr(run_with(&t, "--estimate")->argv, "--max-gb 30 "));
        } else if (change == 1) {   /* no folder to save in: the tab says why, no estimate */
            give_tracks(&t, 3);
            CHECK(!rr && FL(&t, F_PREP_RUNNING) == 0 && runs_with(&t, "--estimate") == checks && FL(&t, F_PREP_PREPARE_ENABLED) == 0);
            CHECK(!strcmp(S(&t, F_PREP_INFO), "Where Stems Cache saves stems can't be used now."));
        } else CHECK(rr && strstr(rr->argv, "--first 2 ") && FL(&t, F_PREP_RUNNING) == 1);
        vt_end(&t);
        LEAKCHECK();
    }
}

/* the app left during "doesn't fit" and back (`estimate == e` by value): the quiet re-check
 * on activation comes back with the same estimate, so the answer runs (Prepare the First 2
 * Tracks, Raise the Limit); with a different estimate (fit_files 1), no run and a new check */
static void t_dialog_came_back(void) {
    for (int k = 0; k < 3; k++) {   /* First N, Raise, First N with a different estimate */
        vt t;
        vt_init(&t);
        vt_core(&t);
        to_friday_with(&t, EST_NO_FIT);
        uint64_t seq = t.c->p.est_seq;
        t.answer[0] = k == 1;
        t.hold = 1;
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
        CHECK(wait_held(&t));
        t.front = CORE_FRONT_WINDOW_LOADED | (CORE_TAB_PREPARE << CORE_FRONT_TAB_SHIFT);   /* another app in front */
        ev(&t, "{\"ev\":\"deactivated\"}");
        t.front = FRONT(CORE_TAB_PREPARE);
        ev(&t, "{\"ev\":\"activated\"}");
        give_library(&t, 4);
        give_tracks(&t, 3);
        give_estimate_end(&t, k == 2 ? EST_NO_FIT_1 : EST_NO_FIT);
        CHECK(k == 2 ? t.c->p.est_seq != seq : t.c->p.est_seq == seq);
        int checks = runs_with(&t, "--estimate");
        release(&t);
        vt_settle(&t);
        vrun *rr = run_with(&t, "--keep 2");
        if (k == 0) {
            CHECKS(rr && strstr(rr->argv, "--keep 2 --playlist 2 --first 2 ") && FL(&t, F_PREP_RUNNING) == 1, rr ? rr->argv : "");
        } else if (k == 1) {
            CHECK(stepped(&t, "CONFIG_WRITE max_gb 25.3 max_days 30"));
            CHECKS(rr && strstr(rr->argv, "--keep 2 --playlist 2 ") && !strstr(rr->argv, "--first") && strstr(rr->argv, "--max-gb 25.3 "),
                   rr ? rr->argv : "");
            CHECK(FL(&t, F_PREP_RUNNING) == 1);
        } else {
            CHECK(!rr && FL(&t, F_PREP_RUNNING) == 0);
            give_tracks(&t, 3);   /* recheck(): the playlist's tracks, then its estimate */
            CHECK(runs_with(&t, "--estimate") > checks);
        }
        vt_end(&t);
        LEAKCHECK();
    }
}

/* status-sets (38a4cbd), prepare-run-under-dialog: Stop Keeping…'s confirmation
 * (PrepareView.swift:256): confirmed, the set removed and logged, its state forgotten and the
 * sets read again; Cancel changes nothing; a removal that fails says FAILED; off without a selection */
static void t_stop_keeping(void) {
    for (int k = 0; k < 3; k++) {   /* confirmed, Cancel, confirmed but the removal fails */
        vt t;
        vt_init(&t);
        t.kept_sets = "2\tNov 22, 2026\t3\n";
        t.stop_fail = k == 2;
        vt_core(&t);
        ev(&t, "{\"ev\":\"start\"}");
        ev(&t, "{\"ev\":\"shown\"}");
        give_library(&t, 4);
        dlg_reset(&t);
        ev(&t, "{\"ev\":\"click\",\"id\":\"stop_keeping\"}");   /* nothing selected: the button is off */
        vt_settle(&t);
        CHECK(t.ndlg == 0);
        char e[160];
        snprintf(e, sizeof e, "{\"ev\":\"select\",\"table\":\"T_STATUS_SETS\",\"row\":0,\"gen\":%llu}", (unsigned long long)GEN(&t, T_STATUS_SETS));
        ev(&t, e);
        CHECK(FL(&t, F_STATUS_STOP_ENABLED) == 1);
        /* the set's state and count known before the click */
        vrun *tr = run_with(&t, "--tracks 2 ");
        CHECK(tr != NULL);
        line(&t, tr, "{\"file\":\"/Music/a.mp3\"}");
        end_run(&t, tr, 0, "");
        vrun *ck = run_with(&t, "--keep 2 --wait-read 1 --playlist 2");
        CHECK(ck != NULL);
        line(&t, ck, EST_END);
        end_run(&t, ck, 0, "");
        CHECK(!strcmp(CELL(&t, C_SET_STATE, 0), "2 tracks to prepare") && !strcmp(CELL(&t, C_SET_TRACKS, 0), "1"));
        CHECK(t.c->st.states.n == 1 && t.c->st.counts.n == 1);
        int lists = runs_with(&t, "--playlists"), checks = runs_with(&t, "--keep 2 --wait-read 1");
        t.answer[0] = k == 1;
        if (k == 0) t.kept_sets = "";   /* removed: gone from the kept folder; a failed removal keeps it */
        ev(&t, "{\"ev\":\"click\",\"id\":\"stop_keeping\"}");
        vt_settle(&t);
        CHECK(t.ndlg == 1 && !strcmp(t.dlg[0].title, "Stop keeping “Friday”?"));
        CHECK(!strcmp(t.dlg[0].msg, "Its stems stay in Stems Cache like any others, and may be removed when Stems Cache needs room."));
        CHECK(!strcmp(t.dlg[0].buttons, "Stop Keeping | Cancel"));
        if (k == 1) {
            CHECK(t.nsteps == 0 && ROWS(&t, T_STATUS_SETS) == 1 && runs_with(&t, "--playlists") == lists);
            CHECK(!strcmp(CELL(&t, C_SET_STATE, 0), "2 tracks to prepare") && runs_with(&t, "--keep 2 --wait-read 1") == checks);
        } else {
            CHECK(stepped(&t, "STOP_KEEPING 2"));
            CHECK(logged(&t, k == 0 ? "prepare: stopped keeping playlist 2: ok" : "prepare: stopped keeping playlist 2: FAILED"));
            if (k == 0) {   /* reload(): the sets as the kept folder has them now; the state and count gone */
                CHECK(ROWS(&t, T_STATUS_SETS) == 0);
                CHECK(t.c->st.states.n == 0 && t.c->st.counts.n == 0);
            } else {   /* the set stays, its state and count forgotten: checked again */
                CHECK(ROWS(&t, T_STATUS_SETS) == 1);
                CHECKS(!strcmp(CELL(&t, C_SET_STATE, 0), "Checking…"), CELL(&t, C_SET_STATE, 0));
                CHECK(runs_with(&t, "--keep 2 --wait-read 1") == checks + 1);
                vrun *again = run_with(&t, "--keep 2 --wait-read 1");
                CHECKS(again && strstr(again->argv, "--estimate --keep 2 "), again ? again->argv : "");
                CHECK(t.c->st.states.n == 0 && t.c->st.counts.n == 0 && !strcmp(CELL(&t, C_SET_TRACKS, 0), "3"));
            }
        }
        vt_end(&t);
        LEAKCHECK();
    }
}

/* an action's flow that holds the flow thread busy until the test lets it go (5 s at most) */
static atomic_int busy_go;
static void busy_flow(core_life *life, void *arg) {
    (void)life;
    (void)arg;
    for (int i = 0; i < 5000 && !atomic_load(&busy_go); i++) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
}
static void busy_arg_free(void *p) { (void)p; }

/* Stop Keeping… during an install (owner, 2026-11-12, a change from 1.3.0): off while the
 * action's flow runs, so a click asks nothing; back on when the flow ends */
static void t_stop_keeping_busy(void) {
    vt t;
    vt_init(&t);
    t.kept_sets = "2\tNov 22, 2026\t3\n";
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    give_library(&t, 4);
    dlg_reset(&t);
    char e[160];
    snprintf(e, sizeof e, "{\"ev\":\"select\",\"table\":\"T_STATUS_SETS\",\"row\":0,\"gen\":%llu}", (unsigned long long)GEN(&t, T_STATUS_SETS));
    ev(&t, e);
    CHECK(FL(&t, F_STATUS_STOP_ENABLED) == 1);
    atomic_store(&busy_go, 0);
    t.c->entry++;
    CHECK(flow_start(t.c, CORE_WORK_FLOW, busy_flow, NULL, busy_arg_free) == CORE_OK);   /* busy on, as an install's */
    core_leave(t.c);
    pump(&t);
    CHECK(t.c->flow_running && t.c->action_running);
    CHECK(FL(&t, F_STATUS_STOP_ENABLED) == 0);
    ev(&t, "{\"ev\":\"click\",\"id\":\"stop_keeping\"}");
    CHECK(t.ndlg == 0 && t.nsteps == 0);
    atomic_store(&busy_go, 1);
    vt_settle(&t);
    CHECK(!t.c->flow_running && FL(&t, F_STATUS_STOP_ENABLED) == 1);
    CHECK(t.ndlg == 0);   /* the click during the install was dropped, not queued */
    vt_end(&t);
    LEAKCHECK();
}

/* the Help menu (main.swift helpMenuItems, Paths.swift): each item opens its page, during an
 * action too (the app delegate's items stay on) */
static void t_help_urls(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    ev(&t, "{\"ev\":\"shown\"}");
    dlg_reset(&t);
    ev(&t, "{\"ev\":\"menu\",\"id\":\"help\"}");
    ev(&t, "{\"ev\":\"menu\",\"id\":\"troubleshooting\"}");
    t.c->action_running = 1;
    ev(&t, "{\"ev\":\"menu\",\"id\":\"issues\"}");
    t.c->action_running = 0;
    CHECK(t.nsteps == 3);
    CHECK(!strcmp(t.steps[0], "OPEN_URL https://github.com/Gabe-LS/rbstemsplus"));
    CHECK(!strcmp(t.steps[1], "OPEN_URL https://github.com/Gabe-LS/rbstemsplus/blob/main/docs/troubleshooting.md"));
    CHECK(!strcmp(t.steps[2], "OPEN_URL https://github.com/Gabe-LS/rbstemsplus/issues"));
    CHECK(!logged(&t, "core: an unknown click or menu choice is ignored"));
    vt_end(&t);
    LEAKCHECK();
}

#ifdef CORE_WIN_POLICY
/* Windows' cheap sheet reaches cache_blocked_win: Pioneer's ORT by FACT_ORT_LISTED, whatever its
 * version prefix (the Mac's prefix wouldn't match here) */
static void t_win_ort_listed(void) {
    vt t;
    vt_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
    t.cache = t.chosen_cache = t.traces = 0;
    t.ort_version = "1.18.0";
    t.ort_prefix = "9.99";
    t.ort_listed = 1;
    CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
    ev(&t, "{\"ev\":\"start\"}");
    CHECK(FL(&t, F_INSTALL_CACHE_ENABLED) == 1);   /* listed: eligible */
    CHECK(!strstr(S(&t, F_STATUS_LINE), "ONNX Runtime"));
    t.ort_listed = 0;
    ev(&t, "{\"ev\":\"activated\"}");
    CHECK(FL(&t, F_INSTALL_CACHE_ENABLED) == 0);   /* unlisted: "checks again" */
    CHECK(!strcmp(S(&t, F_STATUS_LINE), "Stems Cache doesn't support ONNX Runtime 1.18.0 yet. RB Stems Plus checks again each time you open it."));
    vt_end(&t);
}

static void t_win_version_rows(void) {
    vt t;
    vt_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\"}";
    t.plus = t.cache = t.chosen_cache = t.traces = t.ort_listed = 1;
    t.win_rows = "7.2.20\t2\t1\t0\t0\t0\t7.2.20.0354\n"
                 "7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n";
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    CHECK(FL(&t, F_WIN_RB_ROWS_SHOWN) && ROWS(&t, T_WIN_RB_VERSIONS) == 2);
    CHECK(!strcmp(S(&t, F_WIN_CACHE_SUMMARY), "Stems Cache on in 1 of 2 versions"));
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED);
    CHECK(!strcmp(CELL(&t, C_WIN_RB_TEXT, 0), "7.2.20 · Unsupported"));
    CHECK(!strcmp(CELL(&t, C_WIN_RB_TEXT, 1), "7.2.19 · On"));
    CHECK(CELLI(&t, C_WIN_RB_LOOK, 0) == LOOK_OFF && CELLI(&t, C_WIN_RB_LOOK, 1) == LOOK_ON);
    CHECK(!FL(&t, F_INSTALL_CACHE_ENABLED) && FL(&t, F_UNINSTALL_CACHE_ENABLED));
    CHECK(strstr(S(&t, F_STATUS_LINE), "Stems Cache doesn't support rekordbox 7.2.20 yet.") != NULL);
    CHECK(strstr(S(&t, F_LIGHT_CACHE_TIP), "7.2.20.0354 · Unsupported\n7.2.19.0342 · On") != NULL);
    uint64_t gen = GEN(&t, T_WIN_RB_VERSIONS);
    t.win_rows = "7.2.20\t2\t1\t0\t0\t0\t7.2.20.0354\n"
                 "7.2.19\t0\t0\t1\t0\t0\t7.2.19.0342\n";
    ev(&t, "{\"ev\":\"facts_warm\"}");
    CHECK(GEN(&t, T_WIN_RB_VERSIONS) == gen);
    CHECK(!strcmp(CELL(&t, C_WIN_RB_TEXT, 1), "7.2.19 · On · Running"));
    t.win_rows = "7.2.21\t1\t0\t0\t0\t0\t7.2.21.1\n"
                 "7.2.20\t3\t5\t0\t1\t0\t7.2.20.0354\n"
                 "7.2.19\t0\t0\t1\t0\t0\t7.2.19.0342\n";
    ev(&t, "{\"ev\":\"facts_warm\"}");
    CHECK(ROWS(&t, T_WIN_RB_VERSIONS) == 3 && I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED);
    CHECK(!strcmp(CELL(&t, C_WIN_RB_TEXT, 2), "7.2.19 · On · Running"));
    CHECK(FL(&t, F_WIN_RESTORE_VISIBLE) && FL(&t, F_INSTALL_CACHE_ENABLED));
    t.lib_unchecked = 1;
    ev(&t, "{\"ev\":\"facts_warm\"}");
    CHECK(ROWS(&t, T_WIN_RB_VERSIONS) == 3 && !FL(&t, F_INSTALL_CACHE_ENABLED));
    CHECK(strstr(S(&t, F_STATUS_LINE), "couldn't check") != NULL);
    t.lib_unchecked = 0;
    t.win_rows = "7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n";
    ev(&t, "{\"ev\":\"facts_warm\"}");
    CHECK(!FL(&t, F_WIN_RB_ROWS_SHOWN) && !ROWS(&t, T_WIN_RB_VERSIONS) && !*S(&t, F_WIN_CACHE_SUMMARY));
    vt_end(&t);
}

static void t_win_install_disabled_for_only_repair(void) {
    static const char unrecorded[] = "7.2.10\t3\t11\t0\t2\t0\t7.2.10.0342\n";
    static const char delisted[] = "7.2.10\t3\t8\t0\t2\t0\t7.2.10.0342\n";
    static const struct {
        const char *name, *row;
        int chosen_cache, chosen_model;
    } cases[] = {
        {"unrecorded", unrecorded, 0, 0},
        {"unrecorded, cache chosen", unrecorded, 1, 0},
        {"unrecorded, cache and Plus chosen", unrecorded, 1, 1},
        {"delisted", delisted, 0, 0},
        {"delisted, cache chosen", delisted, 1, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        vt t;
        vt_init(&t);
        t.config = "{\"version\":\"1.4.0\",\"os\":\"win\"}";
        t.cache = t.traces = t.ort_listed = 1;
        t.chosen_cache = cases[i].chosen_cache;
        t.plus = t.chosen_model = cases[i].chosen_model;
        t.r1_state = R1_LIVE_FOREIGN;
        t.win_rows = cases[i].row;
        CHECK(vt_core(&t) != NULL);
        ev(&t, "{\"ev\":\"start\"}");
        CHECKS(!FL(&t, F_INSTALL_CACHE_ENABLED), cases[i].name);
        CHECKS(!FL(&t, F_REINSTALL_CACHE_ENABLED), cases[i].name);
        CHECKS(FL(&t, F_REINSTALL_ENABLED) == cases[i].chosen_model, cases[i].name);
        CHECKS(FL(&t, F_WIN_RESTORE_ENABLED), cases[i].name);
        if (t.chosen_model)
            CHECKS(!strcmp(S(&t, F_REINSTALL_TITLE), "Reinstall Demucs v4"), S(&t, F_REINSTALL_TITLE));
        vt_end(&t);
    }
}

static void t_win_repair_notice_uses_bridge_identity(void) {
    static const struct { const char *row, *notice; } cases[] = {
        {"7.2.10\t3\t11\t0\t2\t0\t7.2.10.0342\n",
         "Stems Cache wasn't fully installed in rekordbox 7.2.10. Click Put Back Pioneer's File."},
        {"7.2.10\t3\t8\t0\t2\t0\t7.2.10.0342\n",
         "Stems Cache doesn't support rekordbox 7.2.10 any more. Click Put Back Pioneer's File."},
        {"7.2.10\t3\t6\t0\t2\t0\t7.2.10.0342\n",
         "rekordbox's onnxruntime.dll isn't Pioneer's or RB Stems Plus's, so rekordbox may not separate stems. Click Put Back Pioneer's File."},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        vt t;
        vt_init(&t);
        t.config = "{\"version\":\"1.4.0\",\"os\":\"win\"}";
        t.cache = t.traces = 1;
        t.r1_state = R1_LIVE_FOREIGN;
        t.win_rows = cases[i].row;
        CHECK(vt_core(&t) != NULL);
        ev(&t, "{\"ev\":\"start\"}");
        CHECKS(!strcmp(S(&t, F_WIN_R1_LINE), cases[i].notice), S(&t, F_WIN_R1_LINE));
        CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), cases[i].notice), S(&t, F_LIGHT_CACHE_TIP));
        CHECK(FL(&t, F_WIN_RESTORE_ENABLED));
        vt_end(&t);
    }
}

static void win_ready_versions(vt *t, int admin) {
    vt_init(t);
    t->config = "{\"version\":\"1.4.0\",\"os\":\"win\"}";
    t->admin = admin;
    t->cache = t->chosen_cache = t->traces = 0;
    t->ort_listed = 1;
    t->win_rows = "7.2.20\t1\t0\t0\t0\t0\t7.2.20.0354\n"
                  "7.2.19\t1\t0\t0\t0\t0\t7.2.19.0342\n";
    CHECK(vt_core(t) != NULL);
    ev(t, "{\"ev\":\"start\"}");
    CHECK(!strcmp(S(t, F_WIN_CACHE_SUMMARY), "Stems Cache on in 0 of 2 versions"));
}

static void t_win_ready_versions_standard(void) {
    vt t;
    win_ready_versions(&t, 0);
    CHECK(!FL(&t, F_INSTALL_CACHE_ENABLED));
    const char *status = S(&t, F_STATUS_LINE);
    CHECKS(!strstr(status, "Click Install Stems Cache"), status);
    CHECKS(strstr(status, "Stems Cache needs an administrator account.") != NULL, status);
    CHECK(!strcmp(CELL(&t, C_WIN_RB_TIP, 0), "Stems Cache needs an administrator account."));
    CHECK(!strcmp(CELL(&t, C_WIN_RB_TIP, 1), "Stems Cache needs an administrator account."));
    vt_end(&t);
}

static void t_win_ready_versions_admin(void) {
    vt t;
    win_ready_versions(&t, 1);
    CHECK(FL(&t, F_INSTALL_CACHE_ENABLED));
    const char *status = S(&t, F_STATUS_LINE);
    CHECKS(!strcmp(status, "Click Install Stems Cache to add it to rekordbox 7.2.20 and 7.2.19."), status);
    vt_end(&t);
}

/* HF7: Windows' cheap sheet says Pioneer's ORT couldn't be checked: the status line says so (not
 * "can't find it, reinstall rekordbox", not "checks again") and Stems Cache can't be installed */
static void t_win_lib_unchecked(void) {
    for (int unchecked = 0; unchecked < 2; unchecked++) {
        vt t;
        vt_init(&t);
        t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
        t.cache = t.chosen_cache = t.traces = 0;
        t.lib_exists = t.lib_signed = 0;
        t.ort_listed = 0;
        t.lib_unchecked = unchecked;
        CHECK(vt_core(&t) != NULL);
        ev(&t, "{\"ev\":\"start\"}");
        const char *line = S(&t, F_STATUS_LINE);
        CHECKS(FL(&t, F_INSTALL_CACHE_ENABLED) == 0, unchecked ? "unchecked: no install" : "no ORT: no install");
        int says_unchecked = strstr(line, "Stems Cache couldn't check rekordbox just now.") != NULL;
        int says_missing = strstr(line, "Stems Cache can't find rekordbox's ONNX Runtime") != NULL;
        CHECKS(says_unchecked == unchecked && says_missing == !unchecked, line);
        CHECKS(!strstr(line, "checks again"), line);
        vt_end(&t);
    }
}

/* HG3: "facts_warm" (the shell's warm on activation ended) is one refresh: the line the unchecked
 * sheet left goes once the sheet answers; the Mac's core has no such event */
static void t_win_facts_warm(void) {
    vt t;
    vt_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
    t.cache = t.chosen_cache = t.traces = 0;
    t.lib_exists = t.lib_signed = 0;
    t.ort_listed = 0;
    t.lib_unchecked = 1;
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    CHECK(strstr(S(&t, F_STATUS_LINE), "Stems Cache couldn't check rekordbox just now.") != NULL);
    t.lib_unchecked = 0;   /* the warm's pin answered: the lib is there, signed and listed */
    t.lib_exists = t.lib_signed = t.ort_listed = 1;
    CHECK(ev(&t, "{\"ev\":\"facts_warm\"}") == CORE_OK);
    CHECKS(strstr(S(&t, F_STATUS_LINE), "couldn't check") == NULL, S(&t, F_STATUS_LINE));
    CHECK(ev(&t, "{\"ev\":\"facts_warm\",\"x\":1}") == CORE_E_UNKNOWN);   /* no members */
    vt_end(&t);
}

/* review NR2: Windows' drive with no name ("": unplugged, no label remembered) gives each light
 * and the paused tip their nameless forms, never `drive ""` */
static void t_win_nameless_light(void) {
    vt t;
    vt_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
    t.ort_listed = 1;
    t.loc_kind = LOC_CHOSEN;
    t.loc_probe = PROBE_NOT_CONNECTED;
    t.loc_drive = "";
    CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
    ev(&t, "{\"ev\":\"start\"}");
    CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache's drive isn't connected: rekordbox separates tracks as usual. "
                   "Connect it, then open rekordbox."), S(&t, F_LIGHT_CACHE_TIP));
    t.br_state = "pending";
    t.br_drive = "";
    ev(&t, "{\"ev\":\"activated\"}");
    CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache is still checking its drive."), S(&t, F_LIGHT_CACHE_TIP));
    t.br_state = "unavailable";
    t.br_reason = "not-connected";
    ev(&t, "{\"ev\":\"activated\"}");
    CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache isn't using its drive right now: the drive isn't connected. Connect it: Stems Cache uses it again by itself."), S(&t, F_LIGHT_CACHE_TIP));
    t.loc_probe = PROBE_AVAILABLE;   /* back; the bridge's next check uses it */
    ev(&t, "{\"ev\":\"activated\"}");
    CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache will use its drive again in a few seconds."), S(&t, F_LIGHT_CACHE_TIP));
    t.br_state = NULL;
    t.br_reason = NULL;
    t.br_drive = NULL;
    t.loc_probe = PROBE_READ_ONLY;
    ev(&t, "{\"ev\":\"activated\"}");
    CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache's drive is read-only; Windows may need to repair it (the drive's Properties › Tools › Check in File Explorer)."), S(&t, F_LIGHT_CACHE_TIP));
    t.loc_kind = LOC_BRIDGE_TOO_OLD;
    ev(&t, "{\"ev\":\"activated\"}");
    CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache needs an update to save stems on its drive: it's saving them on this PC for now. Click Reinstall Stems Cache."), S(&t, F_LIGHT_CACHE_TIP));
    CHECK(!strstr(S(&t, F_LIGHT_CACHE_TIP), "\"\""));
    vt_end(&t);
    /* the paused tip: the slow location's drive is "" */
    vt_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
    t.ort_listed = 1;
    t.slow_location = 1;
    t.slow_drive = "";
    t.in_use = "E:\\rbstemsplus";
    t.paused_floor = 5LL << 30;
    CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    ev(&t, "{\"ev\":\"activated\"}");
    CHECKS(!strcmp(S(&t, F_LIGHT_CACHE_TIP), "Stems Cache stopped saving stems: its drive has less than 5 GB free."), S(&t, F_LIGHT_CACHE_TIP));
    vt_end(&t);
    LEAKCHECK();
}

/* review NT3, NT5: the install note's paused line names the drive only when its folder is the one
 * in use (a chosen drive, available), "" then in its nameless form; this PC's floor (Stems Cache
 * too old for the drive) says "your disk", even with a named drive */
static void t_win_install_note_drive(void) {
    static const struct { int64_t kind, probe; const char *drive, *want; } cases[] = {
        {LOC_CHOSEN, PROBE_AVAILABLE, "", "Stems Cache will stop saving stems while its drive has less than 5 GB free."},
        {LOC_CHOSEN, PROBE_AVAILABLE, "GLS", "Stems Cache will stop saving stems while \"GLS\" has less than 5 GB free."},
        {LOC_BRIDGE_TOO_OLD, PROBE_AVAILABLE, "GLS", "Stems Cache will stop saving stems while your disk has less than 5 GB free."},
        {LOC_BRIDGE_TOO_OLD, PROBE_AVAILABLE, "", "Stems Cache will stop saving stems while your disk has less than 5 GB free."},
        {LOC_THIS_MAC, PROBE_AVAILABLE, NULL, "Stems Cache will stop saving stems while your disk has less than 5 GB free."},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        vt t;
        vt_init(&t);
        t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
        t.ort_listed = 1;
        t.slow_location = 1;
        t.loc_kind = cases[i].kind;
        t.loc_probe = cases[i].probe;
        t.slow_drive = cases[i].drive;
        t.in_use = "E:\\rbstemsplus";
        t.paused_floor = 5LL << 30;
        CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
        ev(&t, "{\"ev\":\"start\"}");
        vt_settle(&t);
        CHECK(loc_now(&t).known);
        t.answer[0] = 1;   /* Cancel */
        ev(&t, "{\"ev\":\"click\",\"id\":\"reinstall_cache\"}");
        vt_settle(&t);
        CHECKS(t.ndlg == 1 && strstr(t.dlg[0].msg, cases[i].want) && !strstr(t.dlg[0].msg, "\"\""), t.ndlg ? t.dlg[0].msg : "no dialog");
        vt_end(&t);
    }
    LEAKCHECK();
}

/* review NW1/NW4: Windows keeps no location snapshot of its own (no FACT_LOC_* rows in the sheet),
 * so CORE_SLOW_NOT_NOW (a measure that timed out, was busy or had no answer) keeps the core's memo,
 * its size and its 10-minute timer; a first such answer arms the timer, which asks again and
 * publishes a good answer; a plain nonzero answer still drops the memo */
static void t_win_loc_init(vt *t) {
    vt_init(t);
    t->config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
    t->ort_listed = 1;
    t->no_loc_sheet = 1;
    t->slow_location = 1;
    t->slow_drive = "GLS";
    t->in_use = "E:\\rbstemsplus";
    t->paused_floor = 5LL << 30;   /* under its floor: the light and tip only the memo gives */
    t->walk_bytes = 3ll << 30;
}
static void t_win_loc_decline(vt *t, int32_t rc) {
    pthread_mutex_lock(&t->mu);
    t->slow_location = 0;
    t->slow_decline = rc;
    pthread_mutex_unlock(&t->mu);
}
static const char paused_tip[] = "Stems Cache stopped saving stems: \"GLS\" has less than 5 GB free.";
static void t_win_loc_first_timeout(void) {
    vt t;
    t_win_loc_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"prepare_tabs\":true,\"paths\":{\"home\":\"C:\\\\Users\\\\x\","
               "\"support\":\"/Users/x/S\",\"cache_default\":\"/Users/x/S/cache\",\"kept\":\"/Users/x/S/kept\"}}";
    t.cfg_location = 1;
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(!t.c->v.loc.known && t.c->v.loc.retry);
    CHECKS(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED &&
           !strcmp(S(&t, F_LIGHT_CACHE_TIP), "Checking where stems are saved…"), S(&t, F_LIGHT_CACHE_TIP));

    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    give_library(&t, 4);
    select_outline(&t, 1);
    give_tracks(&t, 3);
    CHECKS(!strcmp(S(&t, F_PREP_INFO), "Checking where stems are saved…"), S(&t, F_PREP_INFO));
    CHECK(!run_with(&t, "--estimate") && !FL(&t, F_PREP_PREPARE_ENABLED));
    vt_end(&t);
    LEAKCHECK();
}

static void t_win_settings_fresh_size(void) {
    vt t;
    t_win_loc_init(&t);
    t.set_size_kind = 4;
    t.set_clear = 1;
    CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(loc_now(&t).size_kind == 1 && loc_now(&t).size_bytes == (3ll << 30));

    t.settings_in_use = "F:\\other";
    t.answer[0] = 2;
    ev(&t, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    vt_settle(&t);
    CHECKS(t.ndlg == 1 && strstr(t.dlg[0].buttons, "(measuring"), t.dlg[0].buttons);

    t.ndlg = 0;
    t.settings_in_use = t.in_use;
    ev(&t, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    vt_settle(&t);
    CHECKS(t.ndlg == 1 && strstr(t.dlg[0].buttons, "about 3.0 GB"), t.dlg[0].buttons);

    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(loc_now(&t).known == VIEW_LOC_STALE && loc_now(&t).size_bytes == (3ll << 30));
    t.ndlg = 0;
    ev(&t, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    vt_settle(&t);
    CHECKS(t.ndlg == 1 && strstr(t.dlg[0].buttons, "(measuring"), t.dlg[0].buttons);
    vt_end(&t);
    LEAKCHECK();
}

static void t_win_loc_not_now(void) {
    /* D2 (Lane C): keep the drive and size through a timeout, then show a fresh answer */
    vt t;
    t_win_loc_init(&t);
    t.cfg_location = 1;
    t.loc_kind = LOC_CHOSEN;
    t.paused_floor = -1;
    CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    view_loc_memo *m = &t.c->v.loc;
    int measures = t.loc_measures;
    CHECK(m->known && m->every && I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON);
    CHECK(loc_now(&t).size_kind == 1 && loc_now(&t).size_bytes == (3ll << 30));
    plat_timer *every = m->every;
    uint64_t size_gen = m->size_gen, status_gen = t.c->gen[DOM_STATUS];
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(t.loc_measures == measures && !m->working);
    CHECKS(m->known && I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED &&
           !strcmp(S(&t, F_LIGHT_CACHE_TIP), "Checking where stems are saved…"), S(&t, F_LIGHT_CACHE_TIP));
    CHECK(loc_now(&t).size_kind == 1 && loc_now(&t).size_bytes == (3ll << 30) &&
          !strcmp(loc_now(&t).drive, "GLS"));
    CHECK(m->every == every && m->size_gen == size_gen && t.c->gen[DOM_STATUS] > status_gen);
    t.slow_location = 1;
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON && !m->retry);
    vt_end(&t);
    LEAKCHECK();

    /* a first-ever measure not now: no memo yet, the timer armed */
    t_win_loc_init(&t);
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(!t.c->v.loc.known && t.c->v.loc.every && t.loc_measures == 0);
    vt_end(&t);
    LEAKCHECK();

    /* the timer asks again; a good answer publishes */
    t_win_loc_init(&t);
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    CHECK(vt_core(&t) != NULL);
    t.c->size_every_test_ms = 20;
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(!t.c->v.loc.known && t.c->v.loc.every);
    pthread_mutex_lock(&t.mu);
    t.slow_location = 1;   /* the drive answers again */
    pthread_mutex_unlock(&t.mu);
    for (int i = 0; i < 2000 && !(t.c->v.loc.known && loc_now(&t).size_kind == 1); i++) wait_ms(&t, 1);
    t.c->size_every_test_ms = 600000;
    wait_ms(&t, 60);
    CHECK(t.loc_measures >= 1 && t.c->v.loc.known && t.c->v.loc.every);
    CHECKS(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED && !strcmp(S(&t, F_LIGHT_CACHE_TIP), paused_tip), S(&t, F_LIGHT_CACHE_TIP));
    vt_end(&t);
    LEAKCHECK();

    /* a plain nonzero answer still drops the memo, its size and its timer */
    t_win_loc_init(&t);
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(t.c->v.loc.known && t.c->v.loc.every);
    t_win_loc_decline(&t, 1);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(!t.c->v.loc.known && !t.c->v.loc.every && t.c->v.loc.size_kind == 0 && !t.c->v.loc.size_folder);
    CHECKS(strcmp(S(&t, F_LIGHT_CACHE_TIP), paused_tip), S(&t, F_LIGHT_CACHE_TIP));
    vt_end(&t);
    LEAKCHECK();

    /* review LN2: a plain nonzero answer after a first not now drops the timer it armed */
    t_win_loc_init(&t);
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(!t.c->v.loc.known && t.c->v.loc.every);
    t_win_loc_decline(&t, 1);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(!t.c->v.loc.known && !t.c->v.loc.every);
    vt_end(&t);
    LEAKCHECK();

    /* review LN3: a host that doesn't measure arms no timer */
    t_win_loc_init(&t);
    t.no_slow_fact = 1;
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(!t.c->v.loc.known && !t.c->v.loc.every);
    vt_end(&t);
    LEAKCHECK();
}

/* review LN1: after a not now the core asks again itself (the DLL drops a late answer): a one-shot
 * retry, 30 s doubling to 10 minutes, reset by a good answer and by a new target, cancelled by a
 * plain no and at the end */
static void t_win_loc_this_pc(vt *t) {   /* the host's next good answer: This PC, no floor */
    pthread_mutex_lock(&t->mu);
    t->in_use = "C:\\Users\\x\\rbstemsplus";
    t->slow_drive = NULL;
    t->paused_floor = 0;
    pthread_mutex_unlock(&t->mu);
}
static void t_win_loc_retry(void) {
    /* a good E:, then Use This PC answered not now: E: stands; the retry publishes This PC */
    vt t;
    t_win_loc_init(&t);
    CHECK(vt_core(&t) != NULL);
    t.c->size_retry_test_ms = 20;
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    view_loc_memo *m = &t.c->v.loc;
    CHECKS(m->known && !m->retry && m->retry_ms == 30000 && !strcmp(S(&t, F_LIGHT_CACHE_TIP), paused_tip), S(&t, F_LIGHT_CACHE_TIP));
    t_win_loc_this_pc(&t);
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    view_location_remeasure(t.c);   /* Use This PC */
    for (int i = 0; i < 2000 && !(m->retry && !m->working); i++) wait_ms(&t, 1);
    CHECK(m->known && m->retry && !strcmp(loc_now(&t).in_use, "E:\\rbstemsplus") && loc_now(&t).floor == (5LL << 30));
    int measures = t.loc_measures;
    pthread_mutex_lock(&t.mu);
    t.slow_location = 1;   /* the late measure is over: the next ask answers */
    pthread_mutex_unlock(&t.mu);
    for (int i = 0; i < 2000 && strcmp(loc_now(&t).in_use, "C:\\Users\\x\\rbstemsplus"); i++) wait_ms(&t, 1);
    vt_settle(&t);
    CHECK(t.loc_measures > measures && !m->retry && m->retry_ms == 30000 && m->every);
    CHECK(!strcmp(loc_now(&t).in_use, "C:\\Users\\x\\rbstemsplus") && loc_now(&t).floor == 0 && !*loc_now(&t).drive);
    CHECKS(strcmp(S(&t, F_LIGHT_CACHE_TIP), paused_tip) && !strstr(S(&t, F_LIGHT_CACHE_TIP), "GLS"), S(&t, F_LIGHT_CACHE_TIP));
    vt_end(&t);
    LEAKCHECK();

    /* not now again and again: the delay doubles to 10 minutes and stays there */
    t_win_loc_init(&t);
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    CHECK(vt_core(&t) != NULL);
    t.c->size_retry_test_ms = 5;   /* past pump's 2 ms grace, so wait_ms returns between retries */
    m = &t.c->v.loc;
    ev(&t, "{\"ev\":\"start\"}");
    int bad = 0;   /* each retry's delay, by its generation: 30 s doubling, never past 10 minutes */
    for (int i = 0; i < 4000 && m->retry_gen < 12; i++) {
        wait_ms(&t, 1);
        if (!m->retry) continue;
        int64_t want = 30000LL << (m->retry_gen < 8 ? m->retry_gen - 1 : 7);
        if (want > 600000) want = 600000;
        bad += m->retry_armed_ms != want || m->retry_ms > 600000 ||
               m->retry_ms != (m->retry_armed_ms * 2 > 600000 ? 600000 : m->retry_armed_ms * 2);
    }
    CHECK(!bad && m->retry_gen >= 12);
    CHECK(m->retry_ms == 600000 && !m->known);
    /* a new target: back to 30 s, the armed 10-minute retry made again */
    t.c->size_retry_test_ms = 600000;   /* the next one stays armed */
    for (int i = 0; i < 2000 && !(m->retry && m->retry_armed_ms == 600000 && !m->working); i++) wait_ms(&t, 1);
    vt_settle(&t);
    uintptr_t gen = m->retry_gen;
    CHECK(m->retry && m->retry_armed_ms == 600000);
    view_location_remeasure(t.c);
    CHECK(m->retry && m->retry_armed_ms == 30000 && m->retry_ms == 60000 && m->retry_gen > gen);
    /* a good answer resets it */
    pthread_mutex_lock(&t.mu);
    t.slow_location = 1;
    pthread_mutex_unlock(&t.mu);
    vt_settle(&t);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(m->known && !m->retry && m->retry_ms == 30000);
    vt_end(&t);
    LEAKCHECK();

    /* a plain no cancels the retry */
    t_win_loc_init(&t);
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    m = &t.c->v.loc;
    CHECK(m->retry && m->retry_armed_ms == 30000 && m->retry_ms == 60000);
    t_win_loc_decline(&t, 1);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(!m->known && !m->retry && !m->every);
    vt_end(&t);
    LEAKCHECK();

    /* the end with a retry armed: cancelled, nothing left */
    t_win_loc_init(&t);
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(t.c->v.loc.retry);
    vt_end(&t);
    LEAKCHECK();

    /* a host that doesn't measure arms no retry */
    t_win_loc_init(&t);
    t.no_slow_fact = 1;
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(!t.c->v.loc.retry);
    vt_end(&t);
    LEAKCHECK();
}

/* A timer has claimed its firing, but its thread is paused before enqueueing the job. */
static void t_win_loc_every_late(void) {
    vt t;
    t_win_loc_init(&t);
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    CHECK(vt_core(&t) != NULL);
    t.c->size_every_test_ms = 20;
    t.hold_timer_post = 1;
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    pthread_mutex_lock(&t.mu);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    while (!t.timer_post_held && pthread_cond_timedwait(&t.cv, &t.mu, &until) == 0) {}
    pthread_mutex_unlock(&t.mu);
    CHECK(t.timer_post_held);
    view_loc_memo *m = &t.c->v.loc;
    t_win_loc_decline(&t, 1);
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(!m->every && !m->retry);
    t.c->size_every_test_ms = 600000;
    t.slow_location = 1;
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(m->known && m->every);
    void *replacement_ctx = m->every_ctx;
    pthread_mutex_lock(&t.mu);
    t.release_timer_post = 1;
    pthread_cond_broadcast(&t.cv);
    pthread_mutex_unlock(&t.mu);
    wait_ms(&t, 40);
    CHECKS(m->every_ctx == replacement_ctx, "old every job must preserve replacement timer");
    vt_end(&t);
    LEAKCHECK();
}

static void t_win_loc_retry_late(void) {
    for (int shutdown = 0; shutdown < 2; shutdown++) {
        vt t;
        t_win_loc_init(&t);
        t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
        CHECK(vt_core(&t) != NULL);
        t.c->size_retry_test_ms = 20;
        t.hold_timer_post = 1;
        ev(&t, "{\"ev\":\"start\"}");
        vt_settle(&t);
        pthread_mutex_lock(&t.mu);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 5;
        while (!t.timer_post_held && pthread_cond_timedwait(&t.cv, &t.mu, &until) == 0) {}
        pthread_mutex_unlock(&t.mu);
        CHECK(t.timer_post_held);
        view_loc_memo *m = &t.c->v.loc;
        void *replacement_ctx = NULL;
        uintptr_t gen = 0;
        if (shutdown) {
            core_free(t.c);
            t.c = NULL;
        } else {
            t_win_loc_decline(&t, 1);
            ev(&t, "{\"ev\":\"activated\"}");
            vt_settle(&t);
            CHECK(!m->every && !m->retry);
            t.c->size_retry_test_ms = 600000;
            t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
            ev(&t, "{\"ev\":\"activated\"}");
            vt_settle(&t);
            CHECK(m->retry);
            replacement_ctx = m->retry_ctx;
            gen = m->retry_gen;
        }
        pthread_mutex_lock(&t.mu);
        t.release_timer_post = 1;
        pthread_cond_broadcast(&t.cv);
        pthread_mutex_unlock(&t.mu);
        wait_ms(&t, 40);
        if (!shutdown) CHECK(m->retry_ctx == replacement_ctx && m->retry_gen == gen);
        vt_end(&t);
        LEAKCHECK();
    }
}

/* HF1: Windows' cheap sheet answers FACT_CACHE_TRACES (host_facts.c). A standard account with
 * Stems Cache's traces can't uninstall it or everything (the standard line says why); an
 * administrator can */
static void t_win_traces_gate(void) {
    for (int admin = 0; admin < 2; admin++) {
        vt t;
        vt_init(&t);
        t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
        t.admin = admin;
        t.traces = 1;
        t.ort_listed = 1;
        CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
        ev(&t, "{\"ev\":\"start\"}");
        CHECKS(FL(&t, F_UNINSTALL_ALL_ENABLED) == admin && FL(&t, F_UNINSTALL_CACHE_ENABLED) == admin, admin ? "admin" : "standard");
        int standard = strstr(S(&t, F_STATUS_LINE), "Reinstalling or removing Stems Cache needs an administrator account.") != NULL;
        CHECKS(standard == !admin, admin ? "admin: no standard line" : "standard: the standard line");
        int menu_on = (core_menu_state(t.c, M_APP_UNINSTALL, NULL, NULL) & CORE_MENU_ENABLED) != 0;
        CHECKS(menu_on == admin, "the menu item as the button");
        t.traces = 0;   /* no traces: everything can go, whoever the account */
        ev(&t, "{\"ev\":\"activated\"}");
        CHECKS(FL(&t, F_UNINSTALL_ALL_ENABLED) == 1 && FL(&t, F_UNINSTALL_CACHE_ENABLED) == 0, "no traces");
        vt_end(&t);
    }
}

/* The 1.5.0 Windows shell sends "prepare_tabs":true. A Manage-only host can omit it or send false.
 * Check both layouts, and the Mac's always-visible tabs in the same core build. */
static void t_win_prepare_tabs(void) {
    static const int32_t rows[] = {M_VIEW_STATUS, M_VIEW_PREPARE, M_PREPARE, M_PREP_PLAYLIST,
                                   M_PREP_CHECK, M_PREP_SEP, M_PREP_PAUSE, M_PREP_STOP};
    static const char *const configs[] = {
        "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}",
        "{\"version\":\"1.4.0\",\"os\":\"win\",\"prepare_tabs\":false,\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}",
        "{\"version\":\"1.5.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"},\"prepare_tabs\":true}",
        NULL,   /* the Mac's config */
    };
    for (int k = 0; k < 4; k++) {
        int shown = k >= 2;
        vt t;
        vt_init(&t);
        t.config = configs[k];
        CHECKS(vt_core(&t) != NULL, configs[k] ? configs[k] : "mac");
        ev(&t, "{\"ev\":\"start\"}");
        /* With Stems Cache installed, start on Status when the shell shows it. */
        CHECKS(I(&t, F_TAB) == (shown ? CORE_TAB_STATUS : CORE_TAB_MANAGE), "the start tab");
        if (k == 2) {
            CHECKS(core_menu_state(t.c, M_VIEW_STATUS, NULL, NULL) == CORE_MENU_ENABLED
                && core_menu_state(t.c, M_VIEW_PREPARE, NULL, NULL) == CORE_MENU_ENABLED,
                "1.5.0 Windows release: View > Status and View > Prepare are enabled at startup");
        }
        t.c->preparing = 1;   /* Pause and Stop are on while a run is going */
        for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
            uint32_t on = rows[i] == M_PREP_CHECK ? 0u : CORE_MENU_ENABLED;   /* Check Again: off while preparing */
            CHECK(core_menu_state(t.c, rows[i], NULL, NULL) == (shown ? on : CORE_MENU_HIDDEN));
        }
        t.c->preparing = 0;
        CHECK(core_menu_state(t.c, M_VIEW_MANAGE, NULL, NULL) == CORE_MENU_ENABLED);   /* the rest as before */
        CHECK(core_menu_state(t.c, M_MANAGE, NULL, NULL) == CORE_MENU_ENABLED);
        CHECK(core_menu_state(t.c, M_APP_UPDATE, NULL, NULL) == (CORE_MENU_ENABLED | CORE_MENU_HIDDEN));
        ev(&t, "{\"ev\":\"menu\",\"id\":\"tab_manage\"}");
        CHECK(I(&t, F_TAB) == CORE_TAB_MANAGE);
        ev(&t, "{\"ev\":\"menu\",\"id\":\"tab_prepare\"}");   /* Ctrl+2 */
        CHECK(I(&t, F_TAB) == (shown ? CORE_TAB_PREPARE : CORE_TAB_MANAGE));
        ev(&t, "{\"ev\":\"menu\",\"id\":\"tab_status\"}");
        CHECK(I(&t, F_TAB) == (shown ? CORE_TAB_STATUS : CORE_TAB_MANAGE));
        /* PM2: nor a click on the hidden tabs' controls, nor {"ev":"tab"} to them */
        ev(&t, "{\"ev\":\"menu\",\"id\":\"tab_manage\"}");
        ev(&t, "{\"ev\":\"click\",\"id\":\"tab_prepare\"}");
        CHECKS(I(&t, F_TAB) == (shown ? CORE_TAB_PREPARE : CORE_TAB_MANAGE), "a click on tab_prepare");
        ev(&t, "{\"ev\":\"tab\",\"tab\":2}");
        ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
        CHECKS(I(&t, F_TAB) == (shown ? CORE_TAB_PREPARE : CORE_TAB_MANAGE), "{\"ev\":\"tab\",\"tab\":1}");
        ev(&t, "{\"ev\":\"tab\",\"tab\":0}");
        CHECKS(I(&t, F_TAB) == (shown ? CORE_TAB_STATUS : CORE_TAB_MANAGE), "{\"ev\":\"tab\",\"tab\":0}");
        ev(&t, "{\"ev\":\"tab\",\"tab\":2}");
        CHECKS(I(&t, F_TAB) == CORE_TAB_MANAGE, "Manage: always");
        ev(&t, "{\"ev\":\"click\",\"id\":\"prepare_playlist\"}");
        CHECKS(I(&t, F_TAB) == (shown ? CORE_TAB_PREPARE : CORE_TAB_MANAGE), "a click on prepare_playlist");
        vt_end(&t);
    }
    /* Windows' key: refused with the Mac's os, and as anything but a boolean (twice: json.c's rule) */
    static const char *const bad[] = {
        "{\"os\":\"mac\",\"prepare_tabs\":true}",
        "{\"prepare_tabs\":false,\"os\":\"mac\"}",
        "{\"os\":\"win\",\"prepare_tabs\":1}",
        "{\"os\":\"win\",\"prepare_tabs\":\"true\"}",
        "{\"os\":\"win\",\"prepare_tabs\":true,\"prepare_tabs\":true}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        vt t;
        vt_init(&t);
        t.config = bad[i];
        CHECKS(vt_core(&t) == NULL, bad[i]);
        vt_end(&t);
    }
}
#endif

/* review MR6 (9e34800): the done line counts finished tracks only (a track cut short isn't done,
 * whatever the helper's own count), a run with a skipped track isn't "prepared", and the last
 * run's summary says so */
static void t_run_counts(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    vrun *rr = run_with(&t, "--keep 2");
    line(&t, rr, "{\"file\":\"/Music/a.mp3\",\"result\":\"prepared\",\"written\":4,\"already\":0}");
    line(&t, rr, "{\"file\":\"/Music/b.mp3\",\"result\":\"skipped\",\"why\":\"no room\"}");
    line(&t, rr, "{\"file\":\"/Music/c.mp3\",\"result\":\"stopped\"}");
    line(&t, rr, "{\"done\":3,\"kept_gb\":0.5}");
    CHECKS(strstr(S(&t, F_PREP_INFO), "\n1 track done, 1 skipped. Prepared playlists use 512 MB.") != NULL, S(&t, F_PREP_INFO));
    CHECK(logged(&t, "prepare: playlist 2: 1 track done, 1 skipped. Prepared playlists use 512 MB."));
    t.front = CORE_FRONT_WINDOW_LOADED | (CORE_TAB_PREPARE << CORE_FRONT_TAB_SHIFT);
    end_run(&t, rr, 0, "");
    CHECKS(t.notifies == 1 && !strcmp(t.notify_title, "“Friday” isn't fully prepared"), t.notify_title);
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"activated\"}");
    give_library(&t, 4);
    give_tracks(&t, 3);
    give_estimate(&t);
    CHECKS(!strcmp(S(&t, F_PREP_INFO_SUB), "Last time: 1 track done, 1 skipped."), S(&t, F_PREP_INFO_SUB));
    vt_end(&t);
    LEAKCHECK();
}

/* M71, facts by part: the kept sets and prepareFacts' own files are read only where 1.3.0 reads
 * them (the performance rule's guard, the M7 parity review's MP3). The start with Manage as the
 * first tab, an activation, a location change and a flow's facts with Manage in front: refresh's
 * rows only. An activation with Prepare in front: prepareFacts once more (R2 P1 K0); with Status in
 * front: its kept sets once and prepareFacts twice (StatusView.reload, R3 P2 K1). */
static void reads_now(vt *t, const char *what, int r, int p, int k) {
    char b[160];
    snprintf(b, sizeof b, "%s: refresh %d (want %d), prepare %d (want %d), kept %d (want %d)", what, t->refresh_reads, r, t->prepare_reads, p,
             t->kept_reads, k);
    CHECKS((r < 0 ? t->refresh_reads >= 1 : t->refresh_reads == r) && t->prepare_reads == p && t->kept_reads == k, b);
    t->refresh_reads = t->prepare_reads = t->kept_reads = 0;
}
static void t_fact_parts(void) {
    vt t;
    vt_init(&t);
    t.plus = t.cache = 0;   /* nothing installed: Manage is the first tab */
    t.kept_sets = "2\tNov 22, 2026\t3\n";
    t.front = FRONT(CORE_TAB_MANAGE);
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    core_commit(t.c);
    CHECK(I(&t, F_TAB) == CORE_TAB_MANAGE);
    reads_now(&t, "the start, Manage first", -1, 0, 0);
    ev(&t, "{\"ev\":\"shown\"}");
    vt_settle(&t);
    t.refresh_reads = t.prepare_reads = t.kept_reads = 0;
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    reads_now(&t, "activation, Manage in front", -1, 0, 0);
    t.paused_floor = 5LL << 30;   /* the location's seen key moves */
    ev(&t, "{\"ev\":\"location\"}");
    vt_settle(&t);
    reads_now(&t, "a location change, Manage in front", -1, 0, 0);
    t.answer[t.ndlg] = 1;   /* a flow's facts, then its confirmation cancelled */
    ev(&t, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    vt_settle(&t);
    reads_now(&t, "a flow, Manage in front", -1, 0, 0);
    vt_end(&t);
    LEAKCHECK();

    vt_init(&t);
    t.kept_sets = "2\tNov 22, 2026\t3\n";
    t.front = FRONT(CORE_TAB_PREPARE);
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    core_commit(t.c);
    ev(&t, "{\"ev\":\"shown\"}");
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    vt_settle(&t);
    t.refresh_reads = t.prepare_reads = t.kept_reads = 0;
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    reads_now(&t, "activation, Prepare in front", 2, 1, 0);
    t.front = FRONT(CORE_TAB_STATUS);
    ev(&t, "{\"ev\":\"tab\",\"tab\":0}");
    vt_settle(&t);
    t.refresh_reads = t.prepare_reads = t.kept_reads = 0;
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    reads_now(&t, "activation, Status in front", 3, 2, 1);
    vt_end(&t);
    LEAKCHECK();
}

/* review MR7 (0da5b7a): the run's log names the playlist by its ID and never holds a path: a
 * skipped track's reason and the helper's stderr show <path> */
static void t_run_log_private(void) {
    vt t;
    vt_init(&t);
    vt_core(&t);
    to_friday_estimated(&t);
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    vrun *rr = run_with(&t, "--keep 2");
    line(&t, rr, "{\"file\":\"/Music/b.mp3\",\"result\":\"skipped\",\"why\":\"/Volumes/GLS/Music/b.mp3: can't be read\"}");
    line(&t, rr, "{\"done\":0,\"kept_gb\":0.5}");
    end_run(&t, rr, 1, "/Users/x/Library/rekordbox/lib.dylib: not found");
    CHECK(logged(&t, "prepare: a track skipped: <path>: can't be read"));
    CHECK(logged(&t, "prepare: playlist 2: 0 tracks done, 1 skipped. Prepared playlists use 512 MB."));
    CHECK(logged(&t, "prepare: the helper stopped: <path>: not found"));
    int raw = 0;
    for (int i = 0; i < t.nlogs; i++) raw += strstr(t.logs[i], "/Volumes/") || strstr(t.logs[i], "/Users/") || strstr(t.logs[i], "Friday");
    CHECK(raw == 0);
    vt_end(&t);
    LEAKCHECK();
}

/* M72: checkWatcherOn on activation, measuring only (WATCHER_STATE, never WATCHER_CHECK's repair):
 * the reminders line follows Login Items, off then on, each change to off logged once */
static int count_logged(vt *t, const char *line) {
    int n = 0;
    for (int i = 0; i < t->nlogs; i++) n += !strcmp(t->logs[i], line);
    return n;
}
static void t_watcher_activation(void) {
#ifdef CORE_WIN_POLICY
    static const char *const off_line = "watcher: installed but not running (turned off in Startup apps, or not started): reinstall reminders are off";
#else
    static const char *const off_line = "watcher: installed but not running (turned off in Login Items, or not loaded): reinstall reminders are off";
#endif
    vt t;
    vt_init(&t);
    vt_core(&t);
    ev(&t, "{\"ev\":\"start\"}");
    core_commit(t.c);
    ev(&t, "{\"ev\":\"shown\"}");
    vt_settle(&t);
    CHECK(!strstr(S(&t, F_STATUS_LINE), "Reinstall reminders are off"));
    t.watcher_off = 1;
    t.watcher_steps = 0;
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(t.watcher_steps == 1 && strstr(S(&t, F_STATUS_LINE), "Reinstall reminders are off") && count_logged(&t, off_line) == 1);
    ev(&t, "{\"ev\":\"activated\"}");   /* still off: no new line in the log */
    vt_settle(&t);
    CHECK(count_logged(&t, off_line) == 1);
    t.watcher_off = 0;   /* turned on again in Login Items */
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    CHECK(!strstr(S(&t, F_STATUS_LINE), "Reinstall reminders are off") && count_logged(&t, off_line) == 1);
    vt_end(&t);
    LEAKCHECK();
}
#ifdef CORE_WIN_POLICY
/* every helper run still open answered as Friday's library, tracks and estimate say, until none is
 * left (a check started by another: the newest of each kind wins) */
static void answer_open_runs(vt *t) {
    static const char *const trk[] = {"{\"file\":\"/Music/a.mp3\"}", "{\"file\":\"/Music/b.mp3\"}", "{\"file\":\"/Music/c.mp3\"}"};
    static const char *const est[] = {EST_A, EST_B, EST_C, EST_END};
    for (int round = 0; round < 32; round++) {
        vrun *r = NULL;
        for (int i = 0; i < t->nruns && i < 128 && !r; i++)
            if (!t->runs[i].ended) r = &t->runs[i];
        if (!r) return;
        if (strstr(r->argv, "--playlists")) for (int i = 0; i < 4; i++) line(t, r, LIB[i]);
        else if (strstr(r->argv, "--tracks")) for (int i = 0; i < 3; i++) line(t, r, trk[i]);
        else if (strstr(r->argv, "--estimate")) for (int i = 0; i < 4; i++) line(t, r, est[i]);
        end_run(t, r, 0, "");
        vt_settle(t);
    }
}

/* review CW2: with Prepare (prepare:true), an R1 answer that changes the state checks the Prepare
 * tab again, quietly, so its problems follow it both ways: missing, then put back */
static void t_win_r1_prepare(void) {
    vt t;
    vt_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"prepare_tabs\":true,\"paths\":{\"home\":\"C:\\\\Users\\\\x\",\"support\":\"/Users/x/S\","
               "\"cache_default\":\"/Users/x/S/cache\",\"model\":\"/RB/hdemucs.onnx\",\"root_ort\":\"/L/ort.dll\","
               "\"rekordbox\":\"C:\\\\Program Files\\\\rekordbox\\\\rekordbox 7.2.4\",\"kept\":\"/Users/x/S/kept\"}}";
    t.ort_listed = 1;
    CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
    to_friday_estimated(&t);
    vt_settle(&t);
    CHECK(t.c->p.nproblems == 0 && FL(&t, F_PREP_PREPARE_ENABLED) == 1);
    /* missing: the answer lands after activation's own re-check, and checks again */
    t.r1_state = R1_LIVE_MISSING;
    ev(&t, "{\"ev\":\"activated\"}");
    vt_settle(&t);
    answer_open_runs(&t);
    CHECK(t.c->v.win_r1 == R1_LIVE_MISSING && t.c->p.nproblems > 0 && t.c->p.problems && strstr(t.c->p.problems, "onnxruntime.dll is missing"));
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 0);
    /* put back: the next answer clears it, and Prepare checks again with no R1 problem */
    t.r1_state = R1_OK;
    ev(&t, "{\"ev\":\"check_request\"}");
    vt_settle(&t);
    answer_open_runs(&t);
    CHECK(t.c->v.win_r1 == R1_OK && (!t.c->p.problems || !strstr(t.c->p.problems, "onnxruntime.dll")));
    CHECK(t.c->p.nproblems == 0 && FL(&t, F_PREP_PREPARE_ENABLED) == 1);
    vt_end(&t);
}

/* PF4: the version vanished after the estimate, while Prepare was still enabled. The click
 * refreshes the track list and refuses the run before the native helper guard is reached. */
static void t_win_prepare_version_moved(void) {
    vt t;
    vt_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"prepare_tabs\":true,\"paths\":{\"home\":\"C:\\\\Users\\\\x\","
               "\"support\":\"/Users/x/S\",\"cache_default\":\"/Users/x/S/cache\",\"model\":\"/RB/hdemucs.onnx\","
               "\"root_ort\":\"/L/ort.dll\",\"kept\":\"/Users/x/S/kept\"}}";
    t.ort_listed = 1;
    CHECK(vt_core(&t) != NULL && t.c->os == CORE_OS_WIN);
    to_friday_estimated(&t);
    vt_settle(&t);
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 1);
    int tracks = runs_with(&t, "--tracks");
    int estimates = runs_with(&t, "--estimate");
    t.rb_installed = 0;
    ev(&t, "{\"ev\":\"click\",\"id\":\"prepare\"}");
    CHECK(logged(&t, "prepare: no verified current rekordbox, the run isn't started"));
    CHECK(runs_with(&t, "--keep") == 0 && runs_with(&t, "--tracks") == tracks + 1 && runs_with(&t, "--estimate") == estimates);
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED) == 0);
    vt_end(&t);
    LEAKCHECK();
}
#endif

/* review MR5: each Settings flow that changes where or how stems are kept (Save, Clear on this
 * Mac, Clear on a drive, a new location, Use This Mac) checks the tab in front again (prepareChanged):
 * the Prepare tab's list, the Status tab's sets; with neither in front, nothing is checked */
static void t_settings_checks(void) {
    static const char *const names[] = {"save", "clear this Mac", "clear a drive", "a new location", "use this Mac"};
    for (int flow = 0; flow < 5; flow++)
        for (int tab = 0; tab < 3; tab++) {   /* the tab in front: Prepare, Status, Manage */
            vt t;
            vt_init(&t);
            t.kept_sets = "2\t\t3";
            vt_core(&t);
            to_friday_estimated(&t);
            if (tab == 1) {
                t.front = FRONT(CORE_TAB_STATUS);
                ev(&t, "{\"ev\":\"tab\",\"tab\":0}");
                give_library(&t, 4);
            } else if (tab == 2) {
                t.front = FRONT(CORE_TAB_MANAGE);
                ev(&t, "{\"ev\":\"tab\",\"tab\":2}");
            }
            dlg_reset(&t);
            static const int answers[5][3] = {{0, 0, 0}, {1, 0, 2}, {1, 0, 0}, {100, 0, 2}, {101, 0, 2}};
            for (int i = 0; i < 3; i++) t.answer[i] = answers[flow][i];
            if (flow == 2) t.set_clear = 3;
            if (flow == 4) t.set_location = 1;
            int lists = runs_with(&t, "--playlists"), checks = runs_with(&t, "--wait-read"), all = t.nruns;
            ev(&t, "{\"ev\":\"menu\",\"id\":\"settings\"}");
            vt_settle(&t);
            char what[96];
            snprintf(what, sizeof what, "%s, tab %d: %d runs, %d dialogs", names[flow], tab, t.nruns - all, t.ndlg);
            if (tab == 0) CHECKS(runs_with(&t, "--playlists") == lists + 1 && t.nruns == all + 1, what);
            else if (tab == 1) CHECKS(runs_with(&t, "--wait-read") > checks, what);
            else CHECKS(t.nruns == all, what);
            vt_end(&t);
            LEAKCHECK();
        }
}

/* each scenario through a volatile pointer: not inlined into view_tests, so their fake hosts
 * (a few KB each) don't add up in one stack frame */
#define RUN_V(t) do { void (*volatile t_)(void) = t; t_(); } while (0)

#ifdef CORE_WIN_POLICY
/* Lane C */
static void t_win_loc_remeasure_frees_result(void) {
    vt t;
    t_win_loc_init(&t);
    t.slow_drive = "Old drive";
    CHECK(vt_core(&t) != NULL);
    view_location_measure(t.c);
    vitem *work = vpop(&t);
    CHECK(work != NULL);
    if (work) {
        core_run(work->job);
        free(work);
    }
    t.slow_drive = "New drive";
    view_location_remeasure(t.c);
    vt_settle(&t);
    CHECKS(!strcmp(loc_now(&t).drive, "New drive"), "publish the replacement measurement");
    vt_end(&t);
#ifdef CORE_DEBUG
    CHECKS(core_debug_live_objects() == 0, "free the superseded measurement's result strings");
#endif
}

static void t_win_loc_remeasure_unchecked(void) {
    vt t;
    t_win_loc_init(&t);
    t.config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"prepare_tabs\":true,\"paths\":{\"home\":\"C:\\\\Users\\\\x\","
               "\"support\":\"/Users/x/S\",\"cache_default\":\"/Users/x/S/cache\",\"kept\":\"/Users/x/S/kept\"}}";
    t.cfg_location = 1;
    t.loc_kind = LOC_CHOSEN;
    t.paused_floor = -1;
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON && loc_now(&t).size_kind == 1);

    ev(&t, "{\"ev\":\"shown\"}");
    t.front = FRONT(CORE_TAB_PREPARE);
    ev(&t, "{\"ev\":\"tab\",\"tab\":1}");
    give_library(&t, 4);
    select_outline(&t, 1);
    give_tracks(&t, 3);
    CHECK(runs_with(&t, "--estimate") == 1);
    give_estimate(&t);
    CHECK(FL(&t, F_PREP_PREPARE_ENABLED));

    t.slow_drive = "Outdated";
    view_location_measure(t.c);
    vitem *work = vpop(&t);
    CHECK(work != NULL);
    if (work) {
        core_run(work->job);   /* an old measurement has answered, but its result is still queued */
        free(work);
    }
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    view_location_remeasure(t.c);
    CHECKS(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED, S(&t, F_LIGHT_CACHE_TIP));
    CHECK(!FL(&t, F_PREP_PREPARE_ENABLED));
    const char *argv[14];
    int32_t problems;
    prep_base_args_now(t.c, argv, &problems);
    CHECK(problems && !strcmp(argv[2], "--cache") && !*argv[3]);
    vt_settle(&t);
    CHECKS(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED, S(&t, F_LIGHT_CACHE_TIP));
    CHECKS(!strcmp(S(&t, F_PREP_INFO), "Checking where stems are saved…"), S(&t, F_PREP_INFO));
    CHECKS(!strcmp(loc_now(&t).drive, "GLS") && loc_now(&t).size_kind == 1 &&
           loc_now(&t).size_bytes == (3ll << 30), "keep the last published drive and size");
    vt_end(&t);
    LEAKCHECK();
}

static void t_win_loc_bad_config_unchecked(void) {
    vt t;
    t_win_loc_init(&t);
    t.cfg_location = 2;
    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECKS(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED &&
           !strcmp(S(&t, F_LIGHT_CACHE_TIP), "Checking where stems are saved…"), S(&t, F_LIGHT_CACHE_TIP));
    vt_end(&t);
    LEAKCHECK();
}

static void t_win_version_rows_unchecked_location(void) {
    vt t;
    t_win_loc_init(&t);
    t.cfg_location = 1;
    t.loc_kind = LOC_CHOSEN;
    t.paused_floor = -1;
    t.win_rows = "7.2.20\t0\t0\t0\t0\t0\t7.2.20.0354\n"
                 "7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n";
    CHECK(vt_core(&t) != NULL);
    ev(&t, "{\"ev\":\"start\"}");
    vt_settle(&t);
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON && CELLI(&t, C_WIN_RB_LOOK, 0) == LOOK_ON &&
          CELLI(&t, C_WIN_RB_LOOK, 1) == LOOK_ON);

    t_win_loc_decline(&t, CORE_SLOW_NOT_NOW);
    view_location_remeasure(t.c);
    vt_settle(&t);
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED && CELLI(&t, C_WIN_RB_LOOK, 0) == LOOK_LIMITED &&
          CELLI(&t, C_WIN_RB_LOOK, 1) == LOOK_LIMITED);
    CHECK(!strcmp(CELL(&t, C_WIN_RB_TIP, 0), "Checking where stems are saved…"));
    CHECK(!strcmp(S(&t, F_WIN_CACHE_SUMMARY), "Stems Cache on in 2 of 2 versions"));

    t.slow_location = 1;
    view_location_measure(t.c);
    vt_settle(&t);
    CHECK(I(&t, F_LIGHT_CACHE_LOOK) == LOOK_ON && CELLI(&t, C_WIN_RB_LOOK, 0) == LOOK_ON &&
          CELLI(&t, C_WIN_RB_LOOK, 1) == LOOK_ON);
    vt_end(&t);
    LEAKCHECK();
}
#endif

void view_tests(int *passed, int *failed) {
    g_passed = passed;
    g_failed = failed;
    RUN_V(t_refresh);
    RUN_V(t_sha_alloc_fail);
    RUN_V(t_location_changed);
    RUN_V(t_location_memo);
    RUN_V(t_folder_moved);
    RUN_V(t_size_walk);
    RUN_V(t_walk_one_at_a_time);
    RUN_V(t_walk_folder_and_tick);
    RUN_V(t_walk_again_fail);
    RUN_V(t_every_oom);
    RUN_V(t_locnow_snapshot);
    RUN_V(t_loc_long);
    RUN_V(t_loc_oom);
    RUN_V(t_first_size);
    RUN_V(t_settings_checks);
    RUN_V(t_run_counts);
    RUN_V(t_run_log_private);
    RUN_V(t_deadlines);
    RUN_V(t_click_gate);
    RUN_V(t_run_not_saved);
    RUN_V(t_action_checks);
    RUN_V(t_prepare_off_during_action);
    RUN_V(t_prepare_reserved);
    RUN_V(t_prepare_reserved_queued);
    RUN_V(t_action_end_mark);
    RUN_V(t_idle_reserved);
    RUN_V(t_flow_end_busy);
    RUN_V(t_busy_unreserved);
    RUN_V(t_mark_lost);
    RUN_V(t_mark_lost_abort);
    RUN_V(t_oom_sweep);
    RUN_V(t_status_quiet_during_action);
    RUN_V(t_clear_checks);
    RUN_V(t_drive_light);
    RUN_V(t_empty_playlist);
    RUN_V(t_work_in_front);
    RUN_V(t_choose_estimate);
    RUN_V(t_come_back_quiet);
    RUN_V(t_held_click);
    RUN_V(t_stale_estimate);
    RUN_V(t_folders);
    RUN_V(t_run);
    RUN_V(t_stop_and_wait);
    RUN_V(t_cant_check);
    RUN_V(t_until);
    RUN_V(t_until_past_midnight);
    RUN_V(t_helper_refused);
    RUN_V(t_free_with_runs);
    RUN_V(t_status_sets);
    RUN_V(t_status_run);
    RUN_V(t_doesnt_fit);
    RUN_V(t_no_space);
    RUN_V(t_refused);
    RUN_V(t_dialog_moved);
    RUN_V(t_dialog_facts);
    RUN_V(t_dialog_came_back);
    RUN_V(t_stop_keeping);
    RUN_V(t_stop_keeping_busy);
    RUN_V(t_help_urls);
    RUN_V(t_fact_parts);
    RUN_V(t_watcher_activation);
#ifdef CORE_WIN_POLICY
    RUN_V(t_win_ort_listed);
    RUN_V(t_win_version_rows);
    RUN_V(t_win_install_disabled_for_only_repair);
    RUN_V(t_win_repair_notice_uses_bridge_identity);
    RUN_V(t_win_ready_versions_standard);
    RUN_V(t_win_ready_versions_admin);
    RUN_V(t_win_nameless_light);
    RUN_V(t_win_install_note_drive);
    RUN_V(t_win_traces_gate);
    RUN_V(t_win_lib_unchecked);
    RUN_V(t_win_prepare_tabs);
    RUN_V(t_win_facts_warm);
    RUN_V(t_win_loc_first_timeout);
    RUN_V(t_win_settings_fresh_size);
    RUN_V(t_win_loc_not_now);
    RUN_V(t_win_loc_retry);
    RUN_V(t_win_loc_every_late);
    RUN_V(t_win_loc_retry_late);
    RUN_V(t_win_r1_prepare);
    RUN_V(t_win_prepare_version_moved);
    RUN_V(t_win_loc_remeasure_frees_result);
    RUN_V(t_win_loc_remeasure_unchecked);
    RUN_V(t_win_loc_bad_config_unchecked);
    RUN_V(t_win_version_rows_unchecked_location);
#endif
}
