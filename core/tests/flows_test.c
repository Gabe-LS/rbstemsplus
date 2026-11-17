/* The action flows' tests (M6): a fake host per flow, with real threads (a thread per worker job,
 * the flow's dialogs presented "on main" while the flow thread waits, as the Mac's presenter
 * does), scripted dialog answers, scripted step and admin results, and a trace of every host
 * call. A self-consistency check: each flow's sequence as read from 1.3.0's Swift (Actions,
 * LocationActions, Admin, Update, Report, Settings, Controller), its texts from M5's table and
 * composers; the oracle for sequences and frames is G0's harness traces (G2). Then every flow runs
 * again with each step failing (nothing changes after a failed check), each dialog cancelled and
 * core_free at each host call, and must end cleanly: the busy state off, nothing asked of the
 * host after the free, nothing leaked (CORE_DEBUG). Linked into core-test. */
#include "../internal.h"
#include "flows_test.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static int *g_passed, *g_failed;
#define CHECK(cond) do { if (cond) (*g_passed)++; else { (*g_failed)++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKS(cond, ctx) do { if (cond) (*g_passed)++; else { (*g_failed)++; printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond, (ctx)); } } while (0)

/* ---------------------------------------------------------------- the fake host */

typedef struct item { struct item *next; core_job *job; void (*fn)(void *); void *arg; } item;

typedef struct ff {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    item *head, *tail;
    pthread_t threads[512];
    int nthreads, running;
    pthread_t main_tid;
    core *c;
    int freed;
    /* cheap facts */
    int plus, cache, chosen_model, chosen_cache, traces, admin_user, cache_link;
    /* step results */
    int rb_open_n, updating_n, rb_checks, rb_open_from;   /* open for n checks, from the rb_open_from-th (1-based) */
    int updating_from;              /* updating_n's checks from this one on (1-based; 0: the first) */
    int payload_fetch, payload_found, have_age;
    int64_t payload_age;
    const char *payload_version, *app_version, *model_sha, *bridge_sha, *signed_by;
    int bridge_here, payload_due;
    int plus_ok, cache_ok;          /* BLOCKED's facts say it can be installed */
    int ort_unlisted;               /* Windows: BLOCKED's FACT_ORT_LISTED is 0 */
    int64_t space_need;             /* SPACE: bytes needed, 0 room */
    int space_rc;
    int download_rc;
    int model_copy_rc;
    int64_t model_copy_code;
    const char *model_copy_msg;
    int cache_prepare_rc;
    int restore_rc;
    int64_t restore_code, left_code;
    const char *restore_msg;
    int originals_saved, originals_verified;
    const char *originals_dir, *help_url;
    int config_write_rc, config_sets;
    int rb_folder, root_folder;
    const char *loc_dir, *loc_drive, *loc_name, *loc_shown, *loc_gate_id;
    int64_t loc_size_kind, loc_size;
    int loc_now_calls, loc2_from;      /* SO2: from the loc2_from-th LOCATION_NOW on (1-based; 0 never), */
    const char *loc2_dir, *loc2_gate_id;   /* ... this folder and marker id instead */
    int remove_location_rc, remove_own_rc, self_delete_rc;
    int app_fate, dock_flag;
    const char *app_path;
    const char *leftover_id, *leftover_label;
    int64_t request_age;
    const char *bridge_installed;
    int update_check, update_swap_rc, update_failed;
    const char *update_line;
    int welcomed;
    int64_t paused_floor;
    const char *paused_drive;
    /* settings and the location */
    const char *set_gb, *set_days, *set_label_path, *set_old_drive;
    int64_t set_size_kind, set_size, set_location;
    int set_unreadable, set_can_choose;
    int64_t set_clear;
    int clear_rc;
    const char *pick;              /* the folder picked, NULL: cancelled */
    int64_t probe_why;
    const char *probe_real, *probe_drive, *probe_mount;
    int64_t discover;
    const char *disc_dir, *disc_id, *disc_current, *disc_current_id, *disc_old_drive;
    int64_t size_dir, size_this_mac;
    int writable, create_rc, write_rc, this_mac_now;
    int64_t create_code;
    const char *create_err;
    /* admin: one outcome per attempt (the last repeats) */
    int32_t admin_rc[8];            /* | FAKE_NP: the run also says "Operation not permitted" (core_admin_note) */
    int nadmin_rc, admin_attempts;
    char admin_version[64], admin_bundle[64];
    int cache_after_admin;
    /* dialogs: answered by button title, in order; none left: no answer (Cancel) */
    const char *answers[32];
    const char *fields[2];         /* Settings' fields as typed */
    int row_answer[32];            /* >= 0: row 0's button j instead of answers[] */
    int nanswers, next_answer;
    int cancel_at;                 /* this dialog (0-based) answers nonzero; -1 none */
    int ndialogs;
    char dlg_title[32][160], dlg_msg[32][1600], dlg_buttons[32][320];
    char dlg_title0[2048];          /* the first dialog's whole title (SO12's long names) */
    int dlg_def[32], dlg_cancel[32], dlg_enabled[32];
    /* injected failures */
    int fail_step_at, nsteps;      /* the n-th step (1-based) returns 1 with no results */
    int free_at, ncalls, calls_after_free, late_calls;   /* faults, and calls already decided on when the free came */
    int release, holding, fetch_hits;   /* t_late_fetch: the held fetch let go; held now; held until a free (flows_test_fetch_hold) */
    int off_main_violations;
    /* the trace and the frames */
    char trace[65536];
    char ntrace[65536];             /* the steps only, numbered as they ran ("step#<n> …", F21) */
    size_t ntlen;
    int in_dialog;                  /* the dialog callback is inside: modal_open says 1 (F4) */
    int fetch_block, fetch_released;   /* PAYLOAD "fetch" waits until released (F9) */
    int fetch_parked;                  /* PAYLOAD "fetch" was reached (t_fetch_reserved, OF1) */
    int om2;   /* t_admin_busy_lost: 1 armed; 2 EF_BUSY lost at the action's busy marker; 3 all allocations fail after the admin run's progress */
    int tm_flag, tm_bridge_loc, tm_available, tm_folders, tm_delete_rc;   /* THIS_MAC_DELETE "facts" (F3) */
    int flip_chosen_at_rb;
    int hold_dialog_at, hold_released, dlg_held;   /* this dialog (0-based) stays up until released (F9) */          /* the n-th REKORDBOX check flips chosen() from Demucs v4 to Stems Cache (F1) */
    size_t tlen;
    char frames[16384];
    size_t flen;
    core_snap *held;
    int quit_steps;
    int slow_calls;
    const char *installed_sha;      /* CORE_SLOW_MODEL_SHA's answer, NULL: unreadable */
    int bump_status_gen_at;         /* this dialog (0-based): main moves the status gen while it is up */
    int change_chosen_at;           /* ... or chosen() changes (model off) */
    /* Windows (flows_win_test.c) */
    int rb_closing_n;               /* rekordbox lingers, window closed, for this many checks ... */
    int rb_closing_from;            /* ... from this check on (1-based; 0: the first) */
    int admin_still_closing, admin_restart;   /* the worker's @still-closing and @restart records */
    int traces_go;                  /* a Stems Cache uninstall that worked leaves no traces either */
    int watcher_off;                /* the reminders' state LOAD, UNLOAD and WATCHER_STATE measure (M72) */
    /* the watcher role (CORE_ROLE_WATCHER: WATCH_* steps) */
    int watch_again_n, watch_busy_n, watch_engine_ok, watch_rb_ok;
    const char *watch_marked, *watch_dismissed;
    char watch_remembered[512];
    /* the prepared playlists' check (WATCH_KEPT, WATCH_HELPER) */
    const char *kept_pid, *kept_checked, *kept_sets, *kept_noticed, *kept_running;
    const char *kept_playlists, *kept_est_playlist, *kept_est_own, *kept_err;   /* an estimate NULL: "" printed, kept_err said */
    int kept_keeps;
    char kept_seen[64];
    char *kept_written;             /* kept.noticed's new text, whole (heap; NULL: not written) */
    int kept_helper_runs;
    /* the report's lights (REPORT_FACTS) */
    int report_plus, report_loc, report_facts_rc, report_disabled;
    const char *report_drive, *report_engine;
    size_t report_tip_len;          /* REPORT_ZIP's cache tip, whole (the trace keeps 512 bytes) */
    int64_t report_floor;
    const char *report_sha;
} ff;
#define FAKE_NP 0x100

static void tr(ff *f, const char *fmt_piece, ...) {
    /* pieces joined, up to a NULL; no format strings from data */
    va_list ap;
    va_start(ap, fmt_piece);
    pthread_mutex_lock(&f->mu);
    for (const char *p = fmt_piece; p; p = va_arg(ap, const char *)) {
        size_t n = strlen(p);
        if (f->tlen + n + 2 < sizeof f->trace) {
            memcpy(f->trace + f->tlen, p, n);
            f->tlen += n;
        }
    }
    if (f->tlen + 2 < sizeof f->trace) f->trace[f->tlen++] = '\n';
    f->trace[f->tlen] = 0;
    pthread_mutex_unlock(&f->mu);
    va_end(ap);
}
#define TR(f, ...) tr((f), __VA_ARGS__, (const char *)NULL)

static void push(ff *f, core_job *job, void (*fn)(void *), void *arg) {
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

static int pump(ff *f) {   /* one turn: what is queued now (a held dialog re-queues itself for the next turn) */
    int n = 0;
    pthread_mutex_lock(&f->mu);
    int todo = 0;
    for (item *i = f->head; i; i = i->next) todo++;
    pthread_mutex_unlock(&f->mu);
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
        if (!f->freed && f->c) core_commit(f->c);   /* a render per job, so the tests see every status line */
    }
    return n;
}

static int running_of(ff *f) {
    pthread_mutex_lock(&f->mu);
    int n = f->running;
    pthread_mutex_unlock(&f->mu);
    return n;
}

/* pumps main until every worker thread has ended and the queue is empty (at most ~10 s) */
static void settle(ff *f) {
    for (int i = 0; i < 10000; i++) {
        int n = pump(f);
        if (!n && running_of(f) == 0) {
            if (!pump(f)) break;
        }
        if (!n) {   /* wait for a post or a worker's end (both signal cv), or 2 ms */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 2000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_mutex_lock(&f->mu);
            if (!f->head && f->running) pthread_cond_timedwait(&f->cv, &f->mu, &ts);
            pthread_mutex_unlock(&f->mu);
        }
    }
}

static int on_main(ff *f) { return pthread_equal(pthread_self(), f->main_tid); }

static void h_render(void *ctx, core_snap *snap, const core_delta *d) {
    ff *f = ctx;
    (void)d;
    size_t n;
    const char *st = core_str(snap, F_STATUS_LINE, &n);
    char line[700];
    snprintf(line, sizeof line, "%s|%d|%lld\n", st, (int)core_flag(snap, F_SPINNER), (long long)core_i64(snap, F_TAB));
    size_t k = strlen(line);
    size_t last = f->flen;   /* only frames that differ from the one before */
    while (last > 0 && f->frames[last - 1] != '\n') last--;
    if (last > 0) {
        size_t prev = last - 1;
        while (prev > 0 && f->frames[prev - 1] != '\n') prev--;
        if (last - prev == k && !memcmp(f->frames + prev, line, k)) k = 0;
    }
    if (k && f->flen + k + 1 < sizeof f->frames) {
        memcpy(f->frames + f->flen, line, k);
        f->flen += k;
        f->frames[f->flen] = 0;
    }
    core_snap_release(f->held);
    f->held = snap;
}

static uint32_t h_front(void *ctx) { (void)ctx; return CORE_FRONT_ACTIVE | CORE_FRONT_WINDOW_LOADED | (CORE_TAB_MANAGE << CORE_FRONT_TAB_SHIFT); }
static int32_t h_modal(void *ctx) {
    ff *f = ctx;
    pthread_mutex_lock(&f->mu);
    int open = f->in_dialog > 0;
    pthread_mutex_unlock(&f->mu);
    return open;
}

static void h_facts(void *ctx, core_facts *out) {
    ff *f = ctx;
    if (!on_main(f)) f->off_main_violations++;
    core_fact_flag(out, FACT_STEMS_PLUS_PRESENT, f->plus);
    core_fact_flag(out, FACT_STEMS_CACHE_PRESENT, f->cache);
    core_fact_flag(out, FACT_CHOSEN_MODEL, f->chosen_model);
    core_fact_flag(out, FACT_CHOSEN_CACHE, f->chosen_cache);
    core_fact_flag(out, FACT_CACHE_TRACES, f->traces);
    core_fact_flag(out, FACT_USER_IS_ADMIN, f->admin_user);
    core_fact_flag(out, FACT_CACHE_IS_LINK, f->cache_link);
    core_fact_flag(out, FACT_REKORDBOX_INSTALLED, 1);
    core_fact_str(out, FACT_REKORDBOX_VERSION, "7.2.10.0342", CORE_NTS);
}

static void h_post_main(void *ctx, core_job *job) { push(ctx, job, NULL, NULL); }

/* Whether this thread has been back in the core since core_free: a host call of it returned after
 * the free, or its worker job started after it (host_call). */
static _Thread_local int back_after_free;

typedef struct { ff *f; core_job *job; } wjob;
static void *worker(void *p) {
    wjob *w = p;
    ff *f = w->f;
    pthread_mutex_lock(&f->mu);
    if (f->freed) back_after_free = 1;   /* started after the free: core_run must drop it, so no call */
    pthread_mutex_unlock(&f->mu);
    core_run(w->job);
    free(w);
    pthread_mutex_lock(&f->mu);
    f->running--;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->mu);
    return NULL;
}

static void h_post_work(void *ctx, core_work kind, core_job *job) {
    ff *f = ctx;
    (void)kind;
    wjob *w = malloc(sizeof *w);
    w->f = f;
    w->job = job;
    pthread_mutex_lock(&f->mu);
    int i = f->nthreads++;
    f->running++;
    pthread_create(&f->threads[i], NULL, worker, w);
    pthread_mutex_unlock(&f->mu);
}

static void free_on_main(void *p) {
    ff *f = p;
    core_free(f->c);
    pthread_mutex_lock(&f->mu);
    f->freed = 1;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->mu);
}

/* Every host call a flow or worker makes (step, admin, dialog, pick_folder, slow_fact): counted; at
 * free_at, core_free runs on main first and the call waits for it.
 *
 * What counts as a fault after the free: a call from a thread that has been back in the core since
 * (the core checks its life before every host call, so it must have stopped), or from a job that
 * started after it (core_run drops those). A call from a job already past its check when the
 * free came (another thread's: the background payload fetch, between core_run's check and its
 * step) is allowed (core.h, core_free: "a running flow, or a worker job that has started, carries
 * on") and is only counted (late_calls): which thread
 * gets there first is the scheduler's, so no test may assume it. */
static void host_call(ff *f) {
    pthread_mutex_lock(&f->mu);
    if (f->freed) {
        f->late_calls++;
        if (back_after_free) f->calls_after_free++;
    }
    int n = ++f->ncalls;
    pthread_mutex_unlock(&f->mu);
    if (on_main(f)) f->off_main_violations++;
    if (n != f->free_at) return;
    push(f, NULL, free_on_main, f);
    pthread_mutex_lock(&f->mu);
    while (!f->freed) pthread_cond_wait(&f->cv, &f->mu);
    pthread_mutex_unlock(&f->mu);
}

/* the end of a host call: back in the core, after the free or not */
static void host_return(ff *f) {
    pthread_mutex_lock(&f->mu);
    if (f->freed) back_after_free = 1;
    pthread_mutex_unlock(&f->mu);
}

typedef struct { ff *f; const core_dialog *d; core_reply *r; pthread_mutex_t mu; pthread_cond_t cv; int done, rc; } present;

static void present_on_main(void *p) {
    present *pr = p;
    ff *f = pr->f;
    if (f->ndialogs == f->hold_dialog_at && !f->hold_released) {   /* still up: asked again next turn */
        f->dlg_held = 1;
        push(f, NULL, present_on_main, pr);
        return;
    }
    int k = f->ndialogs++;
    const core_dialog *d = pr->d;
    char buttons[320] = "";
    int nb = core_dlg_int(d, CORE_DLG_N_BUTTONS, 0, 0);
    uint32_t enabled = 0;
    for (int i = 0; i < nb; i++) {
        if (i) strncat(buttons, "|", sizeof buttons - strlen(buttons) - 1);
        strncat(buttons, core_dlg_text(d, CORE_DLG_BUTTON, i, 0, NULL), sizeof buttons - strlen(buttons) - 1);
        if (core_dlg_int(d, CORE_DLG_BUTTON_ENABLED, i, 0)) enabled |= 1u << i;
    }
    if (k == 0) snprintf(f->dlg_title0, sizeof f->dlg_title0, "%s", core_dlg_text(d, CORE_DLG_TITLE, 0, 0, NULL));
    if (k < 32) {
        snprintf(f->dlg_title[k], sizeof f->dlg_title[k], "%s", core_dlg_text(d, CORE_DLG_TITLE, 0, 0, NULL));
        snprintf(f->dlg_msg[k], sizeof f->dlg_msg[k], "%s", core_dlg_text(d, CORE_DLG_MESSAGE, 0, 0, NULL));
        snprintf(f->dlg_buttons[k], sizeof f->dlg_buttons[k], "%s", buttons);
        f->dlg_def[k] = core_dlg_int(d, CORE_DLG_DEFAULT, 0, 0);
        f->dlg_cancel[k] = core_dlg_int(d, CORE_DLG_CANCEL, 0, 0);
        f->dlg_enabled[k] = (int)enabled;
    }
    pr->rc = 0;
    const char *answer = f->next_answer < f->nanswers ? f->answers[f->next_answer] : NULL;
    int row = f->next_answer < f->nanswers ? f->row_answer[f->next_answer] : -1;
    f->next_answer++;
    if (k == f->bump_status_gen_at && f->c) f->c->gen[DOM_STATUS]++;
    if (k == f->change_chosen_at) f->chosen_model = !f->chosen_model;
    if (k == f->cancel_at || f->freed) pr->rc = 1;
    else if (row >= 0) core_reply_row_button(pr->r, 0, row);
    else if (answer)
        for (int i = 0; i < nb; i++) if (!strcmp(core_dlg_text(d, CORE_DLG_BUTTON, i, 0, NULL), answer)) core_reply_button(pr->r, i);
    if (core_dlg_int(d, CORE_DLG_N_FIELDS, 0, 0) == 2)
        for (int i = 0; i < 2; i++) core_reply_field(pr->r, i, f->fields[i] ? f->fields[i] : core_dlg_text(d, CORE_DLG_FIELD_VALUE, i, 0, NULL), CORE_NTS);
    if (core_dlg_int(d, CORE_DLG_N_ROWS, 0, 0) > 0) TR(f, "label ", core_dlg_text(d, CORE_DLG_ROW_LABEL, 0, 0, NULL));
    TR(f, "dialog ", core_dlg_text(d, CORE_DLG_TITLE, 0, 0, NULL), " [", buttons, "] -> ", pr->rc ? "(none)" : answer ? answer : row >= 0 ? "(row)" : "(none)");
    pthread_mutex_lock(&pr->mu);
    pr->done = 1;
    pthread_cond_signal(&pr->cv);
    pthread_mutex_unlock(&pr->mu);
}

static int32_t h_dialog_body(void *ctx, const core_dialog *d, core_reply *r) {
    ff *f = ctx;
    present pr = {f, d, r, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};
    pthread_mutex_lock(&f->mu);
    f->in_dialog++;
    pthread_mutex_unlock(&f->mu);
    push(f, NULL, present_on_main, &pr);
    pthread_mutex_lock(&pr.mu);
    while (!pr.done) pthread_cond_wait(&pr.cv, &pr.mu);
    pthread_mutex_unlock(&pr.mu);
    pthread_mutex_lock(&f->mu);
    f->in_dialog--;
    pthread_mutex_unlock(&f->mu);
    return pr.rc;
}

static int32_t h_pick_body(void *ctx, const char *start, size_t start_len, core_reply *r) {
    ff *f = ctx;
    (void)start_len;
    TR(f, "pick_folder ", start, " -> ", f->pick ? f->pick : "(cancelled)");
    if (!f->pick) return 1;
    core_reply_field(r, 0, f->pick, CORE_NTS);
    return 0;
}

static const char *step_name(core_step k) {
    static const char *const names[] = {"LAUNCH_AGENT_LOAD", "LAUNCH_AGENT_UNLOAD", "DOCK", "SELF_DELETE", "MODEL_COPY", "DOWNLOAD",
        "REPORT_ZIP", "OPEN_URL", "NOTIFY", "REVEAL", "REKORDBOX", "PAYLOAD", "BLOCKED", "SPACE", "PRUNE_PAYLOADS", "CACHE_PREPARE",
        "MODEL_RESTORE", "MODELS_INFO", "SET_CHOSEN", "CONFIG_WRITE", "CONFIG_SETS", "BUSY_MARKER", "STATE_LINE", "CACHE_FOLDERS",
        "LOCATION_NOW", "REMOVE_LOCATION", "REMOVE_OWN_FOLDERS", "APP_FATE", "LEFTOVER", "REQUEST", "BRIDGE_SHA", "UPDATE_CHECK",
        "UPDATE_SWAP", "UPDATE_RESULT", "WELCOME", "PAUSED", "SETTINGS", "CLEAR_THIS_MAC", "LOCATION_REMEASURE", "LOCATION_PROBE",
        "LOCATION_DISCOVER", "LOCATION_SIZE", "LOCATION_WRITABLE", "LOCATION_CREATE", "LOCATION_WRITE", "THIS_MAC_DELETE",
        "WATCHER_CHECK", "MIGRATE_LEGACY", "REMOVE_LEFTOVERS", "FREE_SPACE", "STOP_KEEPING", "DATE_TEXT", "WATCHER_STATE",
        "WATCH_LOCK", "WATCH_PAUSE", "WATCH_BUSY", "WATCH_FACTS", "WATCH_REMEMBER", "WATCH_REINSTALL", "WATCH_KEPT", "WATCH_HELPER", "REPORT_FACTS"};
    return (unsigned)k < sizeof names / sizeof *names ? names[k] : "?";
}

static void s(core_facts *o, core_fact id, const char *v) { if (v) core_fact_str(o, id, v, CORE_NTS); }

static int32_t h_step_body(void *ctx, core_step k, const char *const *args, int32_t nargs, core_facts *o) {
    ff *f = ctx;
    char a[512] = "";
    for (int i = 0; i < nargs; i++) {
        strncat(a, " ", sizeof a - strlen(a) - 1);
        strncat(a, args[i], sizeof a - strlen(a) - 1);
    }
    TR(f, "step ", step_name(k), a);
    if (k == CORE_STEP_BUSY_MARKER && nargs == 2) {   /* the action's busy marker: fx_busy's reservation next, then EF_BUSY */
        pthread_mutex_lock(&f->mu);
        int arm = f->om2 == 1;
        if (arm) f->om2 = 2;
        pthread_mutex_unlock(&f->mu);
        if (arm) core_test_fail_alloc_after(f->c, 1, 1);   /* the reservation goes through, EF_BUSY is lost */
    }
    pthread_mutex_lock(&f->mu);
    int n = ++f->nsteps;
    char line[640];
    snprintf(line, sizeof line, "step#%d %s%s\n", n, step_name(k), a);
    size_t ln = strlen(line);
    if (f->ntlen + ln + 1 < sizeof f->ntrace) {
        memcpy(f->ntrace + f->ntlen, line, ln + 1);
        f->ntlen += ln;
    }
    pthread_mutex_unlock(&f->mu);
    if (n == f->fail_step_at) return 1;
    const char *a0 = nargs > 0 ? args[0] : "";
    switch (k) {
    case CORE_STEP_LAUNCH_AGENT_LOAD:
    case CORE_STEP_LAUNCH_AGENT_UNLOAD:
    case CORE_STEP_WATCHER_STATE:   /* the reminders' state measured afterwards (M72) */
        core_fact_flag(o, FACT_STEP_FLAG, f->watcher_off);
        return 0;
    case CORE_STEP_WATCH_LOCK:
        if (!strcmp(a0, "again")) { core_fact_flag(o, FACT_STEP_FLAG, f->watch_again_n > 0); if (f->watch_again_n > 0) f->watch_again_n--; }
        if (!strcmp(a0, "retake")) core_fact_flag(o, FACT_STEP_FLAG, 1);
        return 0;
    case CORE_STEP_WATCH_BUSY:
        core_fact_flag(o, FACT_STEP_FLAG, f->watch_busy_n > 0);
        if (f->watch_busy_n > 0) f->watch_busy_n--;
        s(o, FACT_STEP_TEXT, f->watch_marked ? f->watch_marked : "");
        return 0;
    case CORE_STEP_WATCH_FACTS:
        core_fact_flag(o, FACT_CHOSEN_MODEL, f->chosen_model);
        core_fact_flag(o, FACT_CHOSEN_CACHE, f->chosen_cache);
        core_fact_flag(o, FACT_STEMS_PLUS_PRESENT, f->plus);
        core_fact_flag(o, FACT_STEMS_CACHE_PRESENT, f->cache);
        s(o, FACT_REKORDBOX_VERSION, "7.2.19.0342");
        s(o, FACT_ENGINE, "0002");
        core_fact_flag(o, FACT_ENGINE_SUPPORTED, f->watch_engine_ok);
        core_fact_flag(o, FACT_REKORDBOX_SUPPORTED, f->watch_rb_ok);
        s(o, FACT_ORT_PREFIX, "1.18");
        s(o, FACT_STEP_PATH, "7.2.19.0342\n7.2.10");
        s(o, FACT_STEP_URL, "0001");
        s(o, FACT_STEP_TEXT, f->watch_dismissed ? f->watch_dismissed : "");
        s(o, FACT_STEP_TEXT2, "1.4.0");
        return 0;
    case CORE_STEP_WATCH_REMEMBER:
        snprintf(f->watch_remembered, sizeof f->watch_remembered, "%s", a0);
        return 0;
    case CORE_STEP_WATCH_PAUSE: case CORE_STEP_WATCH_REINSTALL:
        return 0;
    case CORE_STEP_WATCH_KEPT:
        if (!strcmp(a0, "session")) { s(o, FACT_STEP_TEXT, f->kept_pid ? f->kept_pid : ""); s(o, FACT_STEP_TEXT2, f->kept_checked ? f->kept_checked : ""); }
        if (!strcmp(a0, "seen") && nargs > 1) snprintf(f->kept_seen, sizeof f->kept_seen, "%s", args[1]);
        if (!strcmp(a0, "sets")) s(o, FACT_KEPT_SETS, f->kept_sets ? f->kept_sets : "");
        if (!strcmp(a0, "facts")) {   /* prepareFacts(measured: false): everything in place, unless kept_keeps is 0 */
            core_fact_flag(o, FACT_APPLE_SILICON, 1);
            core_fact_flag(o, FACT_HELPER_THERE, 1);
            core_fact_flag(o, FACT_REKORDBOX_INSTALLED, 1);
            s(o, FACT_REKORDBOX_VERSION, "7.2.19.0342");
            core_fact_flag(o, FACT_REKORDBOX_SUPPORTED, 1);
            core_fact_flag(o, FACT_STEMS_CACHE_PRESENT, 1);
            core_fact_flag(o, FACT_STEMS_PLUS_PRESENT, 1);
            core_fact_flag(o, FACT_CHOSEN_CACHE, 1);
            s(o, FACT_ENGINE, "0002");
            core_fact_flag(o, FACT_BRIDGE_KEEPS, f->kept_keeps);
            core_fact_flag(o, FACT_ROOT_ORT_EXISTS, 1);
            core_fact_flag(o, FACT_CONFIG_ENABLED, 1);
            core_fact_flag(o, FACT_CONFIG_RB_MODEL, 1);
            s(o, FACT_CONFIG_MAX_GB, "20");
            core_fact_i64(o, FACT_CONFIG_LOCATION, 0);
            core_fact_i64(o, FACT_STEMS_QUALITY, 0);
            s(o, FACT_OUR_MODELS, "aa");
        }
        if (!strcmp(a0, "noticed")) s(o, FACT_STEP_TEXT, f->kept_noticed ? f->kept_noticed : "");
        if (!strcmp(a0, "notice") && nargs > 1) { free(f->kept_written); f->kept_written = strdup(args[1]); }
        if (!strcmp(a0, "running") && nargs > 1) core_fact_flag(o, FACT_STEP_FLAG, f->kept_running && !strcmp(args[1], f->kept_running));
        return 0;
    case CORE_STEP_WATCH_HELPER: {
        int playlists = 0, playlist = 0;
        f->kept_helper_runs++;
        for (int i = 1; i < nargs; i++) {
            if (!strcmp(args[i], "--playlists")) playlists = 1;
            if (!strcmp(args[i], "--playlist")) playlist = 1;
        }
        const char *out = playlists ? f->kept_playlists : playlist ? f->kept_est_playlist : f->kept_est_own;
        s(o, FACT_STEP_TEXT, out ? out : "");
        s(o, FACT_STEP_TEXT2, out ? "" : (f->kept_err ? f->kept_err : ""));
        core_fact_i64(o, FACT_STEP_CODE, out ? 0 : 1);
        return 0;
    }
    case CORE_STEP_REKORDBOX:
        f->rb_checks++;
        if (f->rb_checks == f->flip_chosen_at_rb) { f->chosen_model = 0; f->chosen_cache = 1; }
        int open = f->rb_open_n > 0 && f->rb_checks >= f->rb_open_from;
        int upd = f->updating_n > 0 && f->rb_checks >= f->updating_from;
        core_fact_flag(o, FACT_REKORDBOX_OPEN, open);
        core_fact_flag(o, FACT_PIONEER_UPDATING, upd);
#ifdef CORE_WIN_POLICY
        if (f->rb_closing_n > 0 && f->rb_checks >= f->rb_closing_from) {
            core_fact_flag(o, FACT_REKORDBOX_OPEN, 1);
            core_fact_flag(o, FACT_REKORDBOX_CLOSING, 1);
            f->rb_closing_n--;
        }
#endif
        if (open) f->rb_open_n--;
        if (upd) f->updating_n--;
        return 0;
    case CORE_STEP_PAYLOAD:
        if (!strcmp(a0, "fetch")) {
            pthread_mutex_lock(&f->mu);
            f->fetch_parked = 1;   /* the fetch worker is in its step (past its reservation) */
            pthread_cond_broadcast(&f->cv);
            while (f->fetch_block && !f->fetch_released) pthread_cond_wait(&f->cv, &f->mu);
            pthread_mutex_unlock(&f->mu);
        }
        core_fact_i64(o, FACT_PAYLOAD_FETCH, !strcmp(a0, "latest") ? f->payload_fetch : 2);
        core_fact_flag(o, FACT_PAYLOAD_FOUND, f->payload_found);
        if (f->have_age) core_fact_i64(o, FACT_PAYLOAD_AGE, f->payload_age);
        s(o, FACT_PAYLOAD_VERSION, f->payload_version);
        s(o, FACT_PAYLOAD_APP_VERSION, f->app_version);
        s(o, FACT_PAYLOAD_SIGNED_BY, f->signed_by);
        s(o, FACT_PAYLOAD_MODEL_SHA, f->model_sha);
        s(o, FACT_STEP_PATH, "/Users/x/Library/Pioneer/rekordbox/hdemucs.onnx");
        s(o, FACT_PAYLOAD_BRIDGE_SHA, f->bridge_sha);
        core_fact_flag(o, FACT_PAYLOAD_BRIDGE_HERE, f->bridge_here);
        core_fact_flag(o, FACT_STEP_FLAG, f->payload_due);
        return 0;
    case CORE_STEP_BLOCKED:
        if (!strcmp(a0, "plus")) {
            s(o, FACT_ENGINE, "2.0.1");
            core_fact_flag(o, FACT_ENGINE_SUPPORTED, f->plus_ok);
        } else {
            core_fact_flag(o, FACT_REKORDBOX_SUPPORTED, f->cache_ok);
            core_fact_flag(o, FACT_ROOT_ORT_EXISTS, 1);
            core_fact_flag(o, FACT_LIB_EXISTS, 1);
            core_fact_flag(o, FACT_LIB_SIGNED, 1);
            s(o, FACT_ORT_VERSION, "1.17.3");
            /* Windows: a prefix the ORT's version doesn't match, so a listed ORT shows it isn't read */
            s(o, FACT_ORT_PREFIX, f->c && f->c->os == CORE_OS_WIN ? "9.99" : "1.17");
#ifdef CORE_WIN_POLICY
            core_fact_flag(o, FACT_ORT_LISTED, !f->ort_unlisted);   /* Windows: by sha256, never the prefix */
#endif
        }
        return 0;
    case CORE_STEP_SPACE: core_fact_i64(o, FACT_STEP_CODE, f->space_need); return f->space_rc;
    case CORE_STEP_DOWNLOAD: case CORE_STEP_UPDATE_SWAP:
        if (f->c && !f->freed) {
            core_flow_progress(f->c, CORE_PROGRESS_DOWNLOAD, -1, 0);
            core_flow_progress(f->c, CORE_PROGRESS_DOWNLOAD, -2, 0);
            core_flow_progress(f->c, CORE_PROGRESS_DOWNLOAD, 50, 100);
        }
        return k == CORE_STEP_DOWNLOAD ? f->download_rc : f->update_swap_rc;
    case CORE_STEP_MODEL_COPY: core_fact_i64(o, FACT_STEP_CODE, f->model_copy_code); s(o, FACT_STEP_TEXT, f->model_copy_msg); return f->model_copy_rc;
    case CORE_STEP_CACHE_PREPARE: s(o, FACT_STEP_TEXT, "ortsha"); return f->cache_prepare_rc;
    case CORE_STEP_MODEL_RESTORE:
        core_fact_i64(o, FACT_STEP_CODE, f->restore_code);
        s(o, FACT_STEP_TEXT, f->restore_msg);
        return f->restore_rc;
    case CORE_STEP_MODELS_INFO:
        core_fact_flag(o, FACT_ORIGINALS_SAVED, f->originals_saved);
        if (!strcmp(a0, "saved")) return 0;
        core_fact_flag(o, FACT_ORIGINALS_VERIFIED, f->originals_verified);
        core_fact_i64(o, FACT_STEP_CODE, f->left_code);
        s(o, FACT_STEP_PATH, f->originals_dir);
        s(o, FACT_STEP_URL, f->help_url);
        return 0;
    case CORE_STEP_SET_CHOSEN:
        f->chosen_model = !strcmp(args[0], "1");
        f->chosen_cache = !strcmp(args[1], "1");
        return 0;
    case CORE_STEP_CONFIG_WRITE:
        if (nargs == 4) s(o, FACT_STEP_TEXT, "max_gb=X max_days=Y");
        s(o, FACT_STEP_PATH, "/Users/x/config.ini");
        return f->config_write_rc;
    case CORE_STEP_CONFIG_SETS: core_fact_flag(o, FACT_STEP_FLAG, f->config_sets); return 0;
    case CORE_STEP_STATE_LINE: s(o, FACT_STEP_TEXT, "STATE"); return 0;
    case CORE_STEP_CACHE_FOLDERS:
        core_fact_flag(o, FACT_STEP_FLAG, f->rb_folder);
        core_fact_flag(o, FACT_STEP_FLAG2, f->root_folder);
        return 0;
    case CORE_STEP_LOCATION_NOW: {
        int later = f->loc2_from && ++f->loc_now_calls >= f->loc2_from;
        s(o, FACT_LOC_DIR, later && f->loc2_dir ? f->loc2_dir : f->loc_dir);
        s(o, FACT_LOC_DRIVE, f->loc_drive);
        s(o, FACT_STEP_TEXT2, f->loc_name);
        s(o, FACT_STEP_TEXT, later && f->loc2_dir ? f->loc2_dir : f->loc_shown);
        core_fact_i64(o, FACT_LOC_SIZE_KIND, f->loc_size_kind);
        core_fact_i64(o, FACT_LOC_SIZE, f->loc_size);
        s(o, FACT_LOC_GATE_ID, later ? f->loc2_gate_id : f->loc_gate_id);
        return 0;
    }
    case CORE_STEP_REMOVE_LOCATION:
        if (f->c && !f->freed) core_flow_progress(f->c, CORE_PROGRESS_DELETE, 7, 0);
        if (f->remove_location_rc) {
            core_fact_i64(o, FACT_STEP_CODE, 4);
            s(o, FACT_STEP_TEXT2, "Demucs");
            s(o, FACT_STEP_TEXT, "Resource busy");
        }
        return f->remove_location_rc;
    case CORE_STEP_REMOVE_OWN_FOLDERS: return f->remove_own_rc;
    case CORE_STEP_APP_FATE:
        core_fact_i64(o, FACT_STEP_CODE, f->app_fate);
        s(o, FACT_STEP_PATH, f->app_path);
        return 0;
    case CORE_STEP_DOCK: core_fact_flag(o, FACT_STEP_FLAG, f->dock_flag); return 0;
    case CORE_STEP_SELF_DELETE: f->quit_steps += !f->self_delete_rc; return f->self_delete_rc;   /* 0: the app quits */
    case CORE_STEP_LEFTOVER:
        s(o, FACT_STEP_TEXT, f->leftover_id);
        s(o, FACT_STEP_TEXT2, f->leftover_label);
        return 0;
    case CORE_STEP_REQUEST:
        if (!strcmp(a0, "take")) f->request_age = -1;
        else core_fact_i64(o, FACT_STEP_CODE, f->request_age);
        return 0;
    case CORE_STEP_BRIDGE_SHA: s(o, FACT_STEP_TEXT, f->bridge_installed); return 0;
    case CORE_STEP_UPDATE_CHECK: core_fact_i64(o, FACT_STEP_CODE, f->update_check); return 0;
    case CORE_STEP_UPDATE_RESULT:
        core_fact_flag(o, FACT_STEP_FLAG, f->update_failed);
        s(o, FACT_STEP_TEXT, f->update_line);
        return 0;
    case CORE_STEP_WELCOME:
        if (!strcmp(a0, "check")) core_fact_flag(o, FACT_STEP_FLAG, f->welcomed);
        else f->welcomed = 1;
        return 0;
    case CORE_STEP_PAUSED:
        core_fact_i64(o, FACT_STEP_CODE, f->paused_floor);
        s(o, FACT_STEP_TEXT, f->paused_drive);
        return 0;
    case CORE_STEP_REPORT_FACTS:   /* Demucs v4 off, Stems Cache on and chosen; the chosen drive not connected */
        core_fact_flag(o, FACT_STEMS_PLUS_PRESENT, f->report_plus);
        core_fact_flag(o, FACT_STEMS_CACHE_PRESENT, 1);
        core_fact_flag(o, FACT_CHOSEN_CACHE, 1);
        core_fact_flag(o, FACT_CONFIG_ENABLED, !f->report_disabled);
        core_fact_flag(o, FACT_CONFIG_RB_MODEL, 1);
        s(o, FACT_ENGINE, f->report_engine ? f->report_engine : "0002");
        if (f->report_drive) s(o, FACT_LOC_DRIVE, f->report_drive);
        s(o, FACT_BRIDGE_MODELS, "aa\nbb");
        s(o, FACT_STEP_TEXT, f->report_sha);
        if (f->report_loc) {
            core_fact_flag(o, FACT_LOC_MEASURED, 1);
            core_fact_i64(o, FACT_LOC_KIND, LOC_CHOSEN);
            core_fact_i64(o, FACT_LOC_PROBE, PROBE_NOT_CONNECTED);
            s(o, FACT_LOC_STATUS_DRIVE, "GLS");
            s(o, FACT_LOC_DRIVE, "GLS");
        }
        core_fact_i64(o, FACT_LOC_PAUSED_FLOOR, f->report_floor ? f->report_floor : -1);
        return f->report_facts_rc;
    case CORE_STEP_REPORT_ZIP:
        f->report_tip_len = nargs > 3 ? strlen(args[3]) : 0;
        s(o, FACT_STEP_PATH, "/Users/x/Desktop/r.zip");
        s(o, FACT_STEP_TEXT, "~/Desktop/r.zip");
        s(o, FACT_STEP_URL, "https://example.invalid/issue");
        return f->model_copy_rc;
    case CORE_STEP_SETTINGS:
        s(o, FACT_SET_GB, f->set_gb);
        s(o, FACT_SET_DAYS, f->set_days);
        core_fact_flag(o, FACT_SET_UNREADABLE, f->set_unreadable);
        core_fact_flag(o, FACT_SET_CAN_CHOOSE, f->set_can_choose);
        core_fact_i64(o, FACT_SET_SIZE_KIND, f->set_size_kind);
        core_fact_i64(o, FACT_SET_SIZE, f->set_size);
        core_fact_i64(o, FACT_SET_CLEAR, f->set_clear);
        core_fact_i64(o, FACT_SET_LOCATION, f->set_location);
        s(o, FACT_SET_LABEL_PATH, f->set_label_path);
        s(o, FACT_STEP_TEXT, f->set_old_drive ? f->set_old_drive : (f->set_location == 1 ? "GLS" : NULL));
        s(o, FACT_LOC_DIR, f->loc_dir);
        s(o, FACT_LOC_GATE_ID, f->loc_gate_id);
        s(o, FACT_LOC_DRIVE, f->loc_drive);
        s(o, FACT_STEP_PATH, f->loc_shown);
        return 0;
    case CORE_STEP_CLEAR_THIS_MAC: core_fact_flag(o, FACT_STEP_FLAG, f->cache_link); return f->clear_rc;
    case CORE_STEP_LOCATION_PROBE:
        core_fact_i64(o, FACT_STEP_CODE, f->probe_why);
        s(o, FACT_STEP_TEXT, f->probe_real);
        s(o, FACT_STEP_TEXT2, "~/picked");
        s(o, FACT_LOC_DRIVE, f->probe_drive);
        s(o, FACT_STEP_PATH, f->probe_mount);
        return 0;
    case CORE_STEP_LOCATION_DISCOVER:
        core_fact_i64(o, FACT_STEP_CODE, f->discover);
        s(o, FACT_LOC_DIR, f->disc_dir);
        s(o, FACT_LOC_GATE_ID, f->disc_id);
        s(o, FACT_STEP_TEXT, f->disc_current);
        s(o, FACT_STEP_TEXT2, f->disc_current_id);
        s(o, FACT_LOC_DRIVE, f->disc_old_drive);
        s(o, FACT_STEP_PATH, "<found>");
        return 0;
    case CORE_STEP_LOCATION_SIZE: core_fact_i64(o, FACT_STEP_CODE, !strcmp(a0, "this_mac") ? f->size_this_mac : f->size_dir); return 0;
    case CORE_STEP_LOCATION_WRITABLE: core_fact_flag(o, FACT_STEP_FLAG, f->writable); return 0;
    case CORE_STEP_LOCATION_CREATE:
        s(o, FACT_LOC_DIR, "/Volumes/GLS/DJ/RB Stems Plus Cache");
        s(o, FACT_LOC_GATE_ID, "NEWID");
        core_fact_i64(o, FACT_STEP_CODE, f->create_code);
        s(o, FACT_STEP_TEXT, f->create_err);
        s(o, FACT_STEP_PATH, "<real>");
        return f->create_rc;
    case CORE_STEP_LOCATION_WRITE:
        s(o, FACT_STEP_TEXT, "<dir>");
        s(o, FACT_STEP_PATH, "/Users/x/config.ini");
        return f->write_rc;
    case CORE_STEP_THIS_MAC_DELETE:
        if (!strcmp(a0, "facts")) {
            core_fact_flag(o, FACT_STEP_FLAG, f->tm_flag);
            core_fact_flag(o, FACT_STEP_FLAG2, f->tm_bridge_loc);
            core_fact_i64(o, FACT_STEP_CODE, f->tm_available);
            s(o, FACT_STEP_TEXT, f->tm_available ? "chosen" : "notConnected");
            core_fact_i64(o, FACT_THIS_MAC_MODEL_FOLDERS, f->tm_folders);
            return 0;
        }
        if (!strcmp(a0, "delete")) return f->tm_delete_rc;
        core_fact_flag(o, FACT_STEP_FLAG, f->this_mac_now);
        return 0;
    default: return 0;
    }
}

static int32_t h_admin_body(void *ctx, int32_t kind, const char *version, const char *bundle_id, const char *bridge_sha, const char *ort_sha, int32_t force) {
    ff *f = ctx;
    char k[8];
    snprintf(k, sizeof k, "%d", (int)kind);
    TR(f, "admin ", k, " ", version ? version : "-", " ", bundle_id ? bundle_id : "-", " ", bridge_sha ? bridge_sha : "-", " ",
       ort_sha ? ort_sha : "-", " ", force ? "1" : "0");
    if (kind == CORE_ADMIN_FORGET) return 0;
    int i = f->admin_attempts++;
    int32_t rc = f->nadmin_rc ? f->admin_rc[i < f->nadmin_rc ? i : f->nadmin_rc - 1] : 0;
    if (rc >= 0 && (rc & FAKE_NP) && f->c && !f->freed) core_admin_note(f->c, CORE_ADMIN_NOTE_NOT_PERMITTED);
    if (rc >= 0) rc &= ~FAKE_NP;
    if (rc >= 0 && rc != 1223 && f->c && !f->freed) core_flow_progress(f->c, CORE_PROGRESS_ADMIN_ATTEMPT, 1, 0);   /* the password accepted (1223: UAC declined, Windows) */
    pthread_mutex_lock(&f->mu);
    int om2 = f->om2 == 2;
    if (om2) f->om2 = 3;
    pthread_mutex_unlock(&f->mu);
    if (om2) core_test_fail_allocs(f->c, 1 << 20);   /* the flow's next query fails: it stops */
#ifdef CORE_WIN_POLICY
    if (rc >= 0 && f->admin_still_closing && f->c && !f->freed) core_flow_progress(f->c, CORE_PROGRESS_STILL_CLOSING, 0, 0);
    if (rc >= 0 && f->admin_still_closing > 1 && f->c && !f->freed) core_flow_progress(f->c, CORE_PROGRESS_STILL_CLOSING, 1, 0);   /* ... closed */
    if (rc >= 0 && f->admin_restart && f->c && !f->freed) core_admin_note(f->c, CORE_ADMIN_NOTE_RESTART_PENDING);
#endif
    if (rc == 0 && (kind == CORE_ADMIN_CACHE_INSTALL || kind == CORE_ADMIN_CACHE_REINSTALL)) f->cache = f->cache_after_admin;
    if (rc == 0 && kind == CORE_ADMIN_CACHE_UNINSTALL) f->cache = 0;
    if (rc == 0 && kind == CORE_ADMIN_CACHE_UNINSTALL && f->traces_go) f->traces = 0;
    return rc;
}

static int32_t h_slow_body(void *ctx, core_slow kind, const char *key, size_t key_len, core_facts *out) {
    ff *f = ctx;
    (void)key_len;
    TR(f, "slow ", kind == CORE_SLOW_MODEL_SHA ? "MODEL_SHA " : "? ", key);
    f->slow_calls++;
    if (!f->installed_sha) return 1;
    core_fact_str(out, FACT_SHA256, f->installed_sha, CORE_NTS);
    return 0;
}

static void h_close_reply(void *ctx, core_close what, int32_t proceed) {
    if (what == CORE_CLOSE_WATCHER_DONE) TR((ff *)ctx, proceed ? "close_reply WATCHER_DONE 1" : "close_reply WATCHER_DONE 0");
}

static void h_log(void *ctx, const char *line, size_t len) {
    ff *f = ctx;
    char b[1200];
    snprintf(b, sizeof b, "%.*s", (int)len, line);
    TR(f, "log ", b);
}

static const char *CONFIG = "{\"version\":\"1.4.0\",\"os\":\"mac\",\"paths\":{\"home\":\"/Users/x\"}}";
static const char *ff_config;   /* another start config (flows_win_test.c's "os":"win"), NULL: CONFIG */
static core_role ff_role = CORE_ROLE_APP;   /* t_watcher: CORE_ROLE_WATCHER */
static int ff_no_admin;         /* the table has no admin (flows_win_test.c's native admin) */

/* the counted host calls: host_call before, host_return after */
static int32_t h_dialog(void *ctx, const core_dialog *d, core_reply *r) {
    host_call(ctx);
    int32_t rc = h_dialog_body(ctx, d, r);
    host_return(ctx);
    return rc;
}
static int32_t h_pick(void *ctx, const char *start, size_t start_len, core_reply *r) {
    host_call(ctx);
    int32_t rc = h_pick_body(ctx, start, start_len, r);
    host_return(ctx);
    return rc;
}
static int32_t h_step(void *ctx, core_step k, const char *const *args, int32_t nargs, core_facts *o) {
    /* the Prepare tab's date style: asked beside the flows, not one of their calls (the matrix stays
     * deterministic); this host has none */
    if (k == CORE_STEP_DATE_TEXT) return 1;
    host_call(ctx);
    int32_t rc = h_step_body(ctx, k, args, nargs, o);
    host_return(ctx);
    return rc;
}
static int32_t h_admin(void *ctx, int32_t kind, const char *version, const char *bundle_id, const char *bridge_sha, const char *ort_sha, int32_t force) {
    host_call(ctx);
    int32_t rc = h_admin_body(ctx, kind, version, bundle_id, bridge_sha, ort_sha, force);
    host_return(ctx);
    return rc;
}
static int32_t h_slow(void *ctx, core_slow kind, const char *key, size_t key_len, core_facts *out) {
    /* the location: the cheap facts' snapshot, as a host without the memo; not counted, so the
     * matrix's calls stay the flows' own (the measure after an action runs beside them) */
    if (kind == CORE_SLOW_LOCATION) return 1;
    host_call(ctx);
    int32_t rc = h_slow_body(ctx, kind, key, key_len, out);
    host_return(ctx);
    return rc;
}

static void ff_init(ff *f) {
    memset(f, 0, sizeof *f);
    pthread_mutex_init(&f->mu, NULL);
    pthread_cond_init(&f->cv, NULL);
    f->main_tid = pthread_self();
    f->admin_user = 1;
    f->payload_fetch = 0;
    f->payload_found = 1;
    f->payload_version = "1.4.0";
    f->app_version = "1.4.0";
    f->signed_by = "everyday";
    f->model_sha = "1111111111111111111111111111111111111111111111111111111111111111";
    f->bridge_sha = "2222222222222222222222222222222222222222222222222222222222222222";
    f->installed_sha = NULL;
    f->plus_ok = f->cache_ok = 1;
    f->loc_size_kind = -1;
    f->set_size_kind = -1;
    f->bump_status_gen_at = f->change_chosen_at = -1;
    f->hold_dialog_at = -1;
    f->paused_floor = -1;
    f->request_age = -1;
    f->welcomed = 1;
    f->cancel_at = -1;
    f->cache_after_admin = 1;
    f->writable = 1;
    for (int i = 0; i < 32; i++) f->row_answer[i] = -1;
    core_host h;
    memset(&h, 0, sizeof h);
    h.size = sizeof h;
    h.ctx = f;
    h.render = h_render;
    h.front_facts = h_front;
    h.modal_open = h_modal;
    h.facts = h_facts;
    h.post_main = h_post_main;
    h.post_work = h_post_work;
    h.dialog = h_dialog;
    h.pick_folder = h_pick;
    h.step = h_step;
    h.admin = ff_no_admin ? NULL : h_admin;
    h.slow_fact = h_slow;
    h.close_reply = h_close_reply;
    h.log = h_log;
    f->c = core_new(&h, ff_role, ff_config ? ff_config : CONFIG, CORE_NTS);
    /* these scenarios test the flows, not the gate: they never send "start", so no refresh has
     * turned a button on (view_test.c tests the gate) */
    if (f->c) f->c->click_gate_off = 1;
}

static void answers(ff *f, int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) f->answers[f->nanswers++] = va_arg(ap, const char *);
    va_end(ap);
}

static void ff_end(ff *f) {
    settle(f);
    for (int i = 0; i < f->nthreads; i++) pthread_join(f->threads[i], NULL);
    f->nthreads = 0;
    if (!f->freed) core_free(f->c);
    settle(f);   /* jobs posted at the end: dropped */
    core_snap_release(f->held);
    f->held = NULL;
    pthread_mutex_destroy(&f->mu);
    pthread_cond_destroy(&f->cv);
    free(f->kept_written);
    f->kept_written = NULL;
}

static int ev(ff *f, const char *json) { return core_event(f->c, json, CORE_NTS); }

static int has(const ff *f, const char *line) {
    const char *p = f->trace;
    size_t n = strlen(line);
    while ((p = strstr(p, line))) {
        if ((p == f->trace || p[-1] == '\n') && (p[n] == '\n' || p[n] == 0)) return 1;
        p += n;
    }
    return 0;
}

/* a trace line that starts with head and ends with tail */
static int has_suffix_line(const ff *f, const char *head, const char *tail) {
    for (const char *p = f->trace; (p = strstr(p, head)); p++) {
        if (p != f->trace && p[-1] != '\n') continue;
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p), k = strlen(tail);
        if (n >= k && !memcmp(p + n - k, tail, k)) return 1;
    }
    return 0;
}

/* the lines, in this order (others between them allowed) */
static int in_order(const ff *f, int n, ...) {
    va_list ap;
    va_start(ap, n);
    const char *p = f->trace;
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        const char *line = va_arg(ap, const char *);
        size_t k = strlen(line);
        const char *q = p;
        while ((q = strstr(q, line)) && !((q == f->trace || q[-1] == '\n') && (q[k] == '\n' || q[k] == 0))) q += k;
        if (!q) {
            printf("  missing (in order): %s\n", line);
            ok = 0;
        } else p = q + k;
    }
    va_end(ap);
    return ok;
}

static int frame(const ff *f, const char *line) {
    char b[700];
    snprintf(b, sizeof b, "%s\n", line);
    return strstr(f->frames, b) != NULL;
}

static int count_of(const ff *f, const char *piece) {
    int n = 0;
    for (const char *p = f->trace; (p = strstr(p, piece)); p += strlen(piece)) n++;
    return n;
}

/* the end state every flow must reach */
static void ended_clean(ff *f, const char *ctx) {
    core *c = f->c;
    CHECKS(!c->flow_running && !c->action_running, ctx);
    CHECKS(atomic_load(&c->life->flows_out) == 0, ctx);
    CHECKS(f->off_main_violations == 0, ctx);
    if (f->held && !f->quit_steps) CHECKS(core_flag(f->held, F_SPINNER) == 0, ctx);
}

#define DUMP(f) printf("--- trace\n%s--- frames\n%s---\n", (f)->trace, (f)->frames)

/* ---------------------------------------------------------------- pure decisions */

/* cacheUpdateDue's and queuedStart's 1.3.0 cases (app/Tests/main.swift until M7's switch-over) */
static int g_blocked, g_blocked_asked;
static int stub_blocked(void *ctx) { (void)ctx; g_blocked_asked++; return g_blocked; }
static int due(int present, int chosen, const char *installed, const char *latest, int app_update, int blocked) {
    g_blocked = blocked;
    return flows_cache_update_due(present, chosen, installed, latest, app_update, stub_blocked, NULL);
}
typedef struct { const int *updating; int nupdating, open_for, asked; const int *answers; int nanswers; } queued_ctx;
static int q_up(void *p) { queued_ctx *q = p; int u = q->updating[0]; if (q->nupdating > 1) { q->updating++; q->nupdating--; } return u; }
static int q_rb(void *p) { queued_ctx *q = p; return q->open_for-- > 0; }
static int q_ask(void *p) { queued_ctx *q = p; q->asked++; if (!q->nanswers) return 0; q->nanswers--; return *q->answers++; }
static int queued(const int *updating, int nup, int open_for, const int *answers, int nans, int *asked) {
    queued_ctx q = {updating, nup, open_for, 0, answers, nans};
    int r = flows_queued_start(q_up, q_rb, q_ask, &q);
    *asked = q.asked;
    return r;
}
typedef struct { int blocked, asked; } due_probe;
static int due_blocked(void *ctx) {
    due_probe *p = ctx;
    p->asked++;
    return p->blocked;
}

static void t_pure(void) {
    CHECK(flows_version_newer("1.0.10", "1.0.9") && !flows_version_newer("1.0.9", "1.0.10") && !flows_version_newer("1.4.0", "1.4.0"));
    CHECK(flows_version_newer("1.5.0", "1.4.12") && flows_version_newer("2", "1.9.9") && !flows_version_newer("", "1.0"));
    CHECK(flows_is_sha256("1111111111111111111111111111111111111111111111111111111111111111"));
    CHECK(!flows_is_sha256("111111111111111111111111111111111111111111111111111111111111111A") && !flows_is_sha256("11"));
    CHECK(flows_cache_from_saved_payload(0, 1, 0, 1) && flows_cache_from_saved_payload(0, 1, 7 * 86400, 1));
    CHECK(!flows_cache_from_saved_payload(0, 1, 7 * 86400 + 1, 1) && !flows_cache_from_saved_payload(0, 1, -1, 1));
    CHECK(!flows_cache_from_saved_payload(1, 1, 10, 1) && !flows_cache_from_saved_payload(0, 0, 10, 1) && !flows_cache_from_saved_payload(0, 1, 10, 0));
    /* driveDeletionAllowed: 1.3.0's cases (app/Tests/LocationTests.swift until M7's switch-over) */
    const char *gls = "/Volumes/GLS/RB Stems Plus Cache.noindex", *id = "0F8FAD5B-D9CB-469F-A165-70867728950E";
    const char *id_lower = "0f8fad5b-d9cb-469f-a165-70867728950e", *id2 = "7C9E6679-7425-40DE-944B-E07FC1F90AE7";
    CHECK(!flows_drive_deletion_allowed(NULL, NULL, gls, id));          /* a drive the confirmation didn't name stays */
    CHECK(!flows_drive_deletion_allowed(gls, id, NULL, NULL));          /* the named drive gone since: nothing to delete */
    CHECK(flows_drive_deletion_allowed(gls, id, gls, id_lower));        /* the named folder, found again: deleted */
    CHECK(!flows_drive_deletion_allowed(gls, id, gls, id2)              /* another marker or folder than the one named: kept */
          && !flows_drive_deletion_allowed(gls, id, "/Volumes/GLS 2/RB Stems Plus Cache.noindex", id));
    /* cacheUpdateDue: offered when rekordbox's bridge isn't the latest payload.json's */
    const char *old = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", *new = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    CHECK(due(1, 1, old, new, 0, 0));                                   /* rekordbox's isn't the latest: offered */
    CHECK(!due(1, 1, new, new, 0, 0));                                  /* rekordbox's is the latest: not offered */
    CHECK(!due(0, 1, old, new, 0, 0) && !due(1, 0, old, new, 0, 0));    /* not in rekordbox, or another account's choice */
    CHECK(!due(1, 1, NULL, new, 0, 0) && !due(1, 1, "", new, 0, 0));    /* rekordbox's library unreadable */
    CHECK(!due(1, 1, old, "not a checksum", 0, 0));                     /* a payload.json checksum that isn't one */
    CHECK(!due(1, 1, old, new, 1, 0));                                  /* an RB Stems Plus update waiting (the new app asks) */
    CHECK(!due(1, 1, old, new, 0, 1));                                  /* Stems Cache can't be installed here now */
    g_blocked_asked = 0;
    (void)due(1, 1, new, new, 0, 0);
    CHECK(g_blocked_asked == 0);                                        /* the slow install check isn't run when nothing is due */
    /* queuedStart: an action asked for earlier waits for rekordbox to be closed, never dead-ends */
    const int no[] = {0}, yes[] = {1}, no_yes[] = {0, 1}, tt[] = {1, 1}, tf[] = {1, 0};
    int asked;
    CHECK(queued(no, 1, 0, NULL, 0, &asked) == 0 && asked == 0);        /* rekordbox closed: starts at once, nothing asked */
    CHECK(queued(no, 1, 2, tt, 2, &asked) == 0 && asked == 2);          /* open: Try Again until it is closed, then starts */
    CHECK(queued(no, 1, 3, tf, 2, &asked) == 2 && asked == 2);          /* Cancel at rekordbox open: not started */
    CHECK(queued(yes, 1, 1, yes, 1, &asked) == 1 && asked == 0);        /* rekordbox updating: refused, nothing asked */
    CHECK(queued(no_yes, 2, 1, yes, 1, &asked) == 1 && asked == 1);     /* an update started while waiting: refused */
    /* the event parser's limits stay; json_parse_lim raises an array's (kept.noticed), never an object's */
    {
        arena a = {0};
        jval v;
        char *arr = malloc(64 * 1024 + 64);
        size_t k = 0;
        arr[k++] = '[';
        for (int i = 0; i < 257; i++) { if (i) arr[k++] = ','; arr[k++] = '1'; }
        arr[k++] = ']';
        CHECK(json_parse(&a, arr, k, &v) == CORE_E_PARSE);                                   /* 257 members */
        CHECK(json_parse_lim(&a, arr, k, &(json_lim){64 * 1024, 100000, SIZE_MAX, 16}, &v) == CORE_OK && v.n == 257);
        k = 0;
        arr[k++] = '{';
        for (int i = 0; i < 257; i++) k += (size_t)snprintf(arr + k, 64, "%s\"k%d\":1", i ? "," : "", i);
        arr[k++] = '}';
        CHECK(json_parse_lim(&a, arr, k, &(json_lim){64 * 1024, 100000, SIZE_MAX, 16}, &v) == CORE_E_PARSE);          /* an object keeps 256 */
        memset(arr, ' ', 64 * 1024 + 1);
        arr[0] = '1';
        CHECK(json_parse(&a, arr, 64 * 1024 + 1, &v) == CORE_E_PARSE);                       /* 64 KiB + 1 */
        CHECK(json_parse_lim(&a, arr, 64 * 1024 + 1, &(json_lim){32 << 20, 256, SIZE_MAX, 16}, &v) == CORE_OK);
        free(arr);
        /* kept.noticed's limits: any size, only an object of arrays deep, at most the helper's 5 full sets */
        {
            json_lim kl = {SIZE_MAX, 100000, 1 + 256 + 5 * 100000, 2};
            char *b = malloc(6 * 400020);
            size_t n = 0;
            b[n++] = '{';
            for (int o = 0; o < 6; o++) {
                n += (size_t)snprintf(b + n, 16, "%s\"s%d\":[", o ? "," : "", o);
                for (int i = 0; i < 100000; i++) { if (i) b[n++] = ','; memcpy(b + n, "\"a\"", 3); n += 3; }
                b[n++] = ']';
                b[n] = '}';
                arena a2 = {0};
                CHECK(json_parse_lim(&a2, b, n + 1, &kl, &v) == (o < 5 ? CORE_OK : CORE_E_PARSE));   /* 5 full sets; a 6th is over */
                CHECK(a2.used_total < (128u << 20));
                arena_free(&a2);
            }
            arena a2 = {0};
            CHECK(json_parse_lim(&a2, "{\"a\":[[]]}", 10, &kl, &v) == CORE_E_PARSE);              /* a container in a set */
            CHECK(json_parse(&a2, "{\"a\":[[]]}", 10, &v) == CORE_OK);                           /* an event's depth stays 16 */
            arena_free(&a2);
            free(b);
        }
        arena_free(&a);
    }
    /* notReadyTracks, newlyNotReady and noPaths: 1.3.0's cases (the watcher's prepared playlists' check) */
    {
        arena a = {0};
        prep_estimate e;
        prep_estimate_init(&e);
        const char *lines[] = {"{\"file\":\"/a.mp3\",\"result\":\"ready\"}", "{\"file\":\"/b.mp3\",\"result\":\"prepare\"}",
                               "{\"file\":\"/c.mp3\",\"result\":\"skipped\",\"why\":\"gone\"}", "{\"file\":\"/Volumes/D/d.mp3\",\"result\":\"unchecked\"}",
                               "{\"estimate\":4,\"ready\":1,\"prepare\":1,\"skipped\":1,\"unchecked\":1}"};
        for (int i = 0; i < 5; i++) CHECK(prep_estimate_line(&a, &e, lines[i], strlen(lines[i])) == 0);
        pstr nr[8];
        int32_t n = w_kept_not_ready(&e, nr, 8);
        CHECK(n == 2 && !strcmp(nr[0].s, "/b.mp3") && !strcmp(nr[1].s, "/c.mp3"));   /* to prepare or skipped, not an unchecked one */
        pstr b = {"/b", 2}, c = {"/c", 2}, d = {"/d", 2}, bc[] = {b, c}, bd[] = {b, d};
        CHECK(w_kept_newly(&b, 1, NULL, 0) && !w_kept_newly(&b, 1, &b, 1) && !w_kept_newly(&b, 1, bc, 2)
              && w_kept_newly(bd, 2, &b, 1) && !w_kept_newly(NULL, 0, &b, 1));          /* only a track the last notice didn't cover */
        /* KR2: a 100000-path history in reverse order, sorted, then looked up (no timing assert) */
        {
            enum { HN = 100000 };
            char (*names)[24] = malloc(sizeof *names * HN);
            pstr *nowv = malloc(sizeof *nowv * HN), *was = malloc(sizeof *was * HN);
            for (int i = 0; i < HN; i++) {
                int k = snprintf(names[i], sizeof *names, "/Music/t%d.mp3", i);
                nowv[i] = (pstr){names[i], (size_t)k};
                was[HN - 1 - i] = nowv[i];
            }
            w_kept_sort(was, HN);
            CHECK(!w_kept_newly(nowv, HN, was, HN));                                   /* the history covers now */
            pstr *was1 = malloc(sizeof *was1 * HN);
            for (int i = 0, k = 0; i < HN; i++) if (i != 4242) was1[k++] = nowv[HN - 1 - i];
            w_kept_sort(was1, HN - 1);
            CHECK(w_kept_newly(nowv, HN, was1, HN - 1));                               /* one path missing */
            free(was1); free(was); free(nowv); free(names);
        }
        arena_free(&a);
        char nb[256];
        tbuf t;
        tb_init(&t, CORE_OS_MAC, nb, sizeof nb);
        w_no_paths(&t, "macOS is asking whether RB Stems Plus may read /Volumes/My Drive: answer its prompt, then check again");
        CHECK(!strcmp(nb, "macOS is asking whether RB Stems Plus may read <path>: answer its prompt, then check again"));
        tb_init(&t, CORE_OS_MAC, nb, sizeof nb);
        w_no_paths(&t, "can't read /Users/dj/Music/Caf\xc3\xa9 \xe2\x99\xab.flac");
        CHECK(!strcmp(nb, "can't read <path>"));
        tb_init(&t, CORE_OS_MAC, nb, sizeof nb);
        w_no_paths(&t, "the file isn't there (moved, renamed, or its drive isn't connected)");
        CHECK(!strcmp(nb, "the file isn't there (moved, renamed, or its drive isn't connected)"));
    }
    /* missingParts: 1.3.0's cases (the watcher's Reinstall Now: only what went missing) */
    int m, k;
    flows_missing_parts(1, 1, 0, 1, &m, &k); CHECK(m && !k);     /* Demucs v4 off: Demucs v4 alone, the intact Stems Cache left alone */
    flows_missing_parts(1, 1, 1, 0, &m, &k); CHECK(!m && k);     /* after a rekordbox update: Stems Cache alone */
    flows_missing_parts(1, 1, 0, 0, &m, &k); CHECK(m && k);      /* both off: both */
    flows_missing_parts(1, 0, 1, 0, &m, &k); CHECK(!m && !k);    /* never what wasn't chosen */
    flows_missing_parts(0, 0, 0, 0, &m, &k); CHECK(!m && !k);    /* nothing chosen went missing: nothing */
    /* reinstallLabel's words for those parts (reinstall_label: these texts) */
    char buf[128];
    size_t n = 0;
    CHECK(core_text(CORE_OS_MAC, T_BTN_REINSTALL_PLUS, NULL, 0, buf, sizeof buf, &n) == CORE_OK && !strcmp(buf, "Reinstall Demucs v4"));
    CHECK(core_text(CORE_OS_MAC, T_BTN_REINSTALL_CACHE, NULL, 0, buf, sizeof buf, &n) == CORE_OK && !strcmp(buf, "Reinstall Stems Cache"));
    char both[64];
    CHECK(core_text(CORE_OS_MAC, T_FEATURES_BOTH, NULL, 0, both, sizeof both, &n) == CORE_OK);
    core_slot sl = {CORE_SLOT_features, 0, both, CORE_NTS};
    CHECK(core_text(CORE_OS_MAC, T_REINSTALL_FEATURES, &sl, 1, buf, sizeof buf, &n) == CORE_OK && !strcmp(buf, "Reinstall Demucs v4 + Stems Cache"));
    /* SO9: cacheUpdateDue's table (app/Tests/main.swift), the slow blocked check asked last */
    static const char *const OLD = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char *const NEW = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    static const struct {
        const char *what;
        int present, chosen;
        const char *installed, *latest;
        int app_update, blocked, due, asked;
    } due_cases[] = {
        {"rekordbox's isn't the latest: offered", 1, 1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, 0, 0, 1, 1},
        {"rekordbox's is the latest: not offered, blocked not asked", 1, 1, NULL, NULL, 0, 0, 0, 0},
        {"not in rekordbox", 0, 1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, 0, 0, 0, 0},
        {"another account's choice", 1, 0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, 0, 0, 0, 0},
        {"rekordbox's unreadable", 1, 1, "", NULL, 0, 0, 0, 0},
        {"payload.json's checksum isn't one", 1, 1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "not a checksum", 0, 0, 0, 0},
        {"an RB Stems Plus update waiting", 1, 1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, 1, 0, 0, 0},
        {"Stems Cache can't be installed here now", 1, 1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, 0, 1, 0, 1},
    };
    for (size_t i = 0; i < sizeof due_cases / sizeof *due_cases; i++) {
        due_probe p = {due_cases[i].blocked, 0};
        int due = flows_cache_update_due(due_cases[i].present, due_cases[i].chosen, due_cases[i].installed ? due_cases[i].installed : NEW,
                                         due_cases[i].latest ? due_cases[i].latest : NEW, due_cases[i].app_update, due_blocked, &p);
        CHECKS(due == due_cases[i].due && p.asked == due_cases[i].asked, due_cases[i].what);
    }
    due_probe p = {0, 0};
    CHECK(!flows_cache_update_due(1, 1, NULL, NEW, 0, due_blocked, &p) && !p.asked);   /* no bridge read at all */
    CHECK(strcmp(OLD, NEW) && flows_is_sha256(OLD));
}

/* ---------------------------------------------------------------- install */

static void t_install_plus(void) {
    ff f;
    ff_init(&f);
    answers(&f, 1, "Install");
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}") == CORE_OK);
    CHECK(f.c->flow_running && !f.c->action_running);   /* a confirmation first: the buttons as they were */
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "Install Demucs v4?"));
    CHECK(!strcmp(f.dlg_msg[0], "Replaces rekordbox's stems model with Demucs v4. rekordbox's own model is kept, so it can be put back."));
    CHECK(!strcmp(f.dlg_buttons[0], "Install|Cancel") && f.dlg_def[0] == 0 && f.dlg_cancel[0] == 1);
    CHECK(in_order(&f, 16, "step REKORDBOX", "dialog Install Demucs v4? [Install|Cancel] -> Install",
                   "step BUSY_MARKER install-plus Install Demucs v4", "step PAYLOAD latest",
                   "log using the payload.json just downloaded (payload 1.4.0)", "step BLOCKED plus", "step SPACE 1 0",
                   "step DOWNLOAD model", "step PRUNE_PAYLOADS", "step REKORDBOX", "step MODEL_COPY", "step SET_CHOSEN 1 0",
                   "step LAUNCH_AGENT_LOAD", "step STATE_LINE", "log state: STATE", "step BUSY_MARKER"));
    CHECK(count_of(&f, "step BLOCKED cache") == 0 && count_of(&f, "admin ") == 0);
    CHECK(frame(&f, "Installing…|1|2") && frame(&f, "Checking for the latest files…|1|2"));
    CHECK(frame(&f, "Checking the Demucs v4 model…|1|2") && frame(&f, "Downloading the Demucs v4 model…|1|2"));
    CHECK(frame(&f, "Downloading the Demucs v4 model… 50%|1|2") && frame(&f, "Installing Demucs v4…|1|2") && frame(&f, "|0|2"));
    CHECK(f.chosen_model && !f.chosen_cache);
    ended_clean(&f, "install plus");
    ff_end(&f);
}

static void t_install_cache_admin(void) {
    ff f;
    ff_init(&f);
    f.nadmin_rc = 2;
    f.admin_rc[0] = 4 | FAKE_NP;   /* App Management refused it */
    f.admin_rc[1] = 0;
    f.paused_floor = 20000000000;
    f.paused_drive = "GLS";
    answers(&f, 2, "Install", "Try Again");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Install Stems Cache?"));
    CHECK(!strcmp(f.dlg_msg[0], "Saves the stems rekordbox separates, with either stems model, so they load much faster the next time. "
                                "Stems Cache will stop saving stems while \"GLS\" has less than 20 GB free."));
    CHECK(!strcmp(f.dlg_title[1], "Permission needed") && !strcmp(f.dlg_buttons[1], "Try Again|Cancel"));
    CHECK(in_order(&f, 16, "step BLOCKED cache", "step SPACE 0 1", "step DOWNLOAD bridge", "step CACHE_PREPARE",
                   "step REKORDBOX", "log admin run (install), attempt 1",
                   "admin 0 7.2.10.0342 com.pioneerdj.rekordboxdj 2222222222222222222222222222222222222222222222222222222222222222 ortsha 0",
                   "dialog Permission needed [Try Again|Cancel] -> Try Again", "log   user chose: Try Again", "step BUSY_MARKER install-cache Install Stems Cache",
                   "step REKORDBOX", "log admin run (install), attempt 2", "admin 4 - - - - 0", "step SET_CHOSEN 0 1",
                   "step CONFIG_WRITE rekordbox_model 1", "log Stems Cache: saving rekordbox's own model's stems too (rekordbox_model=1): ok"));
    CHECK(frame(&f, "Installing Stems Cache (this takes about a minute)…|1|2") && frame(&f, "|0|2") && frame(&f, "Trying again…|1|2"));
    CHECK(frame(&f, "Preparing Stems Cache…|1|2"));
    CHECK(f.chosen_cache && has(&f, "step LAUNCH_AGENT_LOAD"));
    ended_clean(&f, "install cache");
    ff_end(&f);
}

/* the alerts that end an install, each with its log line */
static void t_install_failures(void) {
    struct { int found, fetch, have_age, bridge_here; int64_t space; int dl; int copy; int prep; const char *title, *msg, *log; } cases[] = {
        {0, 2, 0, 0, 0, 0, 0, 0, "Download failed", "Nothing was changed. Check your internet connection and try again.", "log no payload.json: nothing changed"},
        {0, 1, 0, 0, 0, 0, 0, 0, "Download failed", "The download couldn't be verified. Nothing was changed. Try again later.", "log no payload.json: nothing changed"},
        {1, 2, 1, 0, 0, 0, 0, 0, "Download failed", "Nothing was changed. Check your internet connection and try again.",
         "log Stems Cache needs payload.json downloaded now, or offline one downloaded within the last 7 days with its files here: nothing changed"},
        {1, 0, 0, 0, 2415919104, 0, 0, 0, "Low disk space", "RB Stems Plus needs about 2.3 GB free. Nothing was changed. Free up some space and try again.",
         "log not enough free space (about 2.3 GB needed): nothing changed"},
        {1, 0, 0, 0, 0, 1, 0, 0, "Download failed", "Nothing was changed. Check your internet connection and try again.", "log download failed: nothing changed"},
        {1, 0, 0, 0, 0, 2, 0, 0, "Low disk space", "The disk is full. Nothing was changed. Free up some space and try again.", "log download: the disk is full: nothing changed"},
        {1, 0, 0, 0, 0, 3, 0, 0, "Download failed", "A file didn't download correctly. Nothing was changed. Try again later.", "log download damaged: nothing changed"},
        {1, 0, 0, 0, 0, 0, 0, 1, "Install failed", "Stems Cache couldn't be installed. See the log in this window.",
         "log Stems Cache not installed: the downloaded bridge changed after it was checked"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ff f;
        ff_init(&f);
        f.payload_found = cases[i].found;
        f.payload_fetch = cases[i].fetch;
        f.have_age = cases[i].have_age;
        f.payload_age = 100;
        f.bridge_here = cases[i].bridge_here;
        f.space_need = cases[i].space;
        f.download_rc = cases[i].dl;
        f.cache_prepare_rc = cases[i].prep;
        answers(&f, 2, "Install", "OK");
        ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
        settle(&f);
        CHECKS(f.ndialogs == 2 && !strcmp(f.dlg_title[1], cases[i].title) && !strcmp(f.dlg_msg[1], cases[i].msg) && !strcmp(f.dlg_buttons[1], "OK"), cases[i].log);
        CHECKS(has(&f, cases[i].log), cases[i].log);
        ended_clean(&f, cases[i].log);
        if (*g_failed) DUMP(&f);
        ff_end(&f);
    }
    /* offline, with a saved payload.json fit for Stems Cache: it goes on, and says so */
    ff f;
    ff_init(&f);
    f.payload_fetch = 2;
    f.have_age = 1;
    f.payload_age = 7200;
    f.bridge_here = 1;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(has(&f, "log offline: using the saved payload.json (payload 1.4.0, downloaded 2 hours ago, signed with the everyday key)"));
    CHECK(has(&f, "step CACHE_PREPARE"));
    ended_clean(&f, "offline install");
    ff_end(&f);
    /* the model copy fails: its message, in the reinstall's words */
    ff_init(&f);
    f.plus = 1;
    f.chosen_model = 1;
    f.model_copy_rc = 1;
    f.model_copy_code = CORE_MODEL_NOT_COPIED_WHY;
    f.model_copy_msg = "Operation not permitted";
    answers(&f, 2, "Reinstall", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"reinstall_plus\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Reinstall Demucs v4?") && !strcmp(f.dlg_msg[0], "Installs the latest version of Demucs v4 again."));
    CHECK(!strcmp(f.dlg_title[1], "Reinstall failed") && !strcmp(f.dlg_msg[1], "Demucs v4 couldn't be reinstalled. The Demucs v4 model couldn't be copied: Operation not permitted. Nothing was changed."));
    CHECK(has(&f, "log Demucs v4 not installed: The Demucs v4 model couldn't be copied: Operation not permitted.") && has(&f, "step BUSY_MARKER reinstall-plus Reinstall Demucs v4"));
    ended_clean(&f, "model copy fails");
    ff_end(&f);
}

static void t_preflight(void) {
    ff f;
    ff_init(&f);
    f.rb_open_n = 1;
    answers(&f, 1, "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "rekordbox open") &&
          !strcmp(f.dlg_msg[0], "Quit rekordbox (also in other accounts on this Mac), then try again.") && !strcmp(f.dlg_buttons[0], "OK"));
    CHECK(count_of(&f, "step BUSY_MARKER") == 0 && !strstr(f.frames, "|1|"));
    ended_clean(&f, "preflight open");
    ff_end(&f);
    ff_init(&f);
    f.updating_n = 1;
    answers(&f, 1, "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "rekordbox updating") &&
          !strcmp(f.dlg_msg[0], "Wait until rekordbox's installer or updater has finished, then try again."));
    ended_clean(&f, "preflight updating");
    ff_end(&f);
    /* a Prepare run is stopped first, if the user agrees */
    ff_init(&f);
    core_test_set_preparing(f.c, 1);
    answers(&f, 1, "Cancel");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "“” is being prepared") && !strcmp(f.dlg_buttons[0], "Stop and Continue|Cancel"));
    CHECK(f.c->preparing && count_of(&f, "step ") == 0);
    ff_end(&f);
    /* SO12: the whole name, a 600-byte multibyte one too */
    ff_init(&f);
    core_test_set_preparing(f.c, 1);
    {
        char name[700] = "", want[800];
        for (int i = 0; i < 200; i++) strcat(name, "\xe6\x97\xa5");   /* 日: 3 bytes each */
        snprintf(want, sizeof want, "\xe2\x80\x9c%s\xe2\x80\x9d is being prepared", name);
        f.c->p.run_pl.name = strdup(name);   /* the run's playlist (prep_free clears it) */
        f.c->p.has_run_pl = 1;
        answers(&f, 1, "Cancel");
        ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
        settle(&f);
        CHECK(strlen(name) == 600 && f.ndialogs == 1 && !strcmp(f.dlg_title0, want));
    }
    ff_end(&f);
    /* rekordbox opened during the downloads: asked until closed */
    ff_init(&f);
    answers(&f, 2, "Install", "Cancel");
    f.rb_open_n = 1;
    f.rb_open_from = 2;
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    settle(&f);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_title[1], "rekordbox open") &&
          !strcmp(f.dlg_msg[1], "Quit rekordbox (also in other accounts on this Mac), then click Try Again.") && !strcmp(f.dlg_buttons[1], "Try Again|Cancel"));
    CHECK(has(&f, "log rekordbox is open: nothing changed") && !has(&f, "step MODEL_COPY"));
    ended_clean(&f, "rekordbox opened");
    ff_end(&f);
}

/* ---------------------------------------------------------------- uninstall */

static void t_uninstall_plus(void) {
    ff f;
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    answers(&f, 1, "Uninstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Uninstall Demucs v4?") && !strcmp(f.dlg_msg[0], "Puts rekordbox's own stems model back."));
    CHECK(in_order(&f, 6, "step BUSY_MARKER uninstall-plus Uninstall Demucs v4", "step REKORDBOX", "step MODEL_RESTORE", "step SET_CHOSEN 0 0",
                   "step LAUNCH_AGENT_UNLOAD", "log state: STATE"));
    CHECK(frame(&f, "Uninstalling…|1|2"));
    ended_clean(&f, "uninstall plus");
    ff_end(&f);
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    f.restore_rc = 1;
    f.restore_code = CORE_MODEL_NOT_BACK_WHY;
    f.restore_msg = "Resource busy";
    f.help_url = "https://help/#x";
    answers(&f, 2, "Uninstall", "Open Help");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Uninstall failed") && !strcmp(f.dlg_buttons[1], "OK|Open Help") &&
          !strcmp(f.dlg_msg[1], "Demucs v4 couldn't be removed. rekordbox's own stems model couldn't be put back: Resource busy. To get rekordbox's own stems model back, click Open Help."));
    CHECK(in_order(&f, 3, "step MODELS_INFO", "step OPEN_URL https://help/#x", "log state: STATE") && !has(&f, "step SET_CHOSEN 0 0"));
    CHECK(!has(&f, "step LAUNCH_AGENT_UNLOAD"));   /* Demucs v4 still chosen: the watcher stays */
    ended_clean(&f, "uninstall plus fails");
    ff_end(&f);
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    f.restore_rc = 1;
    f.restore_code = CORE_MODEL_NO_SPACE_BACK;
    answers(&f, 2, "Uninstall", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_msg[1], "Demucs v4 couldn't be removed. There isn't enough free space to put rekordbox's own stems model back. Free up some space and try again.") && !strcmp(f.dlg_buttons[1], "OK"));
    ended_clean(&f, "uninstall plus no space");
    ff_end(&f);
}

static void t_uninstall_cache(void) {
    ff f;
    ff_init(&f);
    f.cache = f.chosen_cache = f.traces = f.rb_folder = 1;
    f.config_sets = 1;
    answers(&f, 1, "Uninstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Uninstall Stems Cache?") &&
          !strcmp(f.dlg_msg[0], "Stops saving stems and puts rekordbox's original files back. The stems already saved are kept."));
    CHECK(in_order(&f, 9, "step BUSY_MARKER uninstall-cache Uninstall Stems Cache", "step CACHE_FOLDERS", "log admin run (uninstall), attempt 1",
                   "admin 2 7.2.10.0342 com.pioneerdj.rekordboxdj - - 0", "admin 4 - - - - 0", "step SET_CHOSEN 0 0", "step CONFIG_SETS rekordbox_model",
                   "step CONFIG_WRITE rekordbox_model 0", "log state: STATE"));
    CHECK(frame(&f, "Uninstalling Stems Cache…|1|2"));
    ended_clean(&f, "uninstall cache");
    ff_end(&f);
    /* the root run refuses: its own reason */
    ff_init(&f);
    f.cache = f.chosen_cache = f.traces = f.rb_folder = 1;
    f.nadmin_rc = 1;
    f.admin_rc[0] = 8;
    answers(&f, 2, "Uninstall", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "rekordbox updating"));
    CHECK(!has(&f, "step SET_CHOSEN 0 0") && has(&f, "admin 4 - - - - 0"));
    ended_clean(&f, "uninstall cache refused");
    ff_end(&f);
    /* a failure without a reason of its own, then rolled back */
    ff_init(&f);
    f.cache = f.chosen_cache = f.traces = f.rb_folder = 1;
    f.nadmin_rc = 1;
    f.admin_rc[0] = 6;
    answers(&f, 2, "Uninstall", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Uninstall failed") &&
          !strcmp(f.dlg_msg[1], "Stems Cache couldn't be removed. rekordbox has its original files again. See the log in this window."));
    ff_end(&f);
    /* rekordbox gone: the root folder alone, if the user agrees */
    ff_init(&f);
    f.cache = 0;
    f.chosen_cache = f.traces = 1;
    f.rb_folder = 0;
    f.root_folder = 1;
    answers(&f, 2, "Uninstall", "Remove");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "rekordbox missing") && !strcmp(f.dlg_buttons[1], "Cancel|Remove") && f.dlg_cancel[1] == -1);
    CHECK(in_order(&f, 4, "log rekordbox isn't in the Applications folder: asking to remove the copy of its files", "log   user chose: Remove",
                   "log admin run (remove-root-folder), attempt 1", "admin 3   - - 0"));
    CHECK(frame(&f, "Removing the copy of rekordbox's files…|1|2"));
    ended_clean(&f, "root folder");
    ff_end(&f);
    /* a standard account: not run, said so */
    ff_init(&f);
    f.cache = f.chosen_cache = f.traces = f.rb_folder = 1;
    f.admin_user = 0;
    answers(&f, 2, "Uninstall", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    settle(&f);
    CHECK(has(&f, "log   FAILED: Stems Cache needs an administrator account.") && count_of(&f, "admin 2") == 0);
    CHECK(!strcmp(f.dlg_title[1], "Uninstall failed") && !strcmp(f.dlg_msg[1], "Stems Cache couldn't be removed. Stems Cache needs an administrator account."));
    ff_end(&f);
    /* cancelled at the password: no alert */
    ff_init(&f);
    f.cache = f.chosen_cache = f.traces = f.rb_folder = 1;
    f.nadmin_rc = 1;
    f.admin_rc[0] = -2;
    answers(&f, 1, "Uninstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && has(&f, "admin 4 - - - - 0"));
    ended_clean(&f, "password cancelled");
    ff_end(&f);
}

static void t_uninstall_all(void) {
    ff f;
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    f.originals_saved = 1;
    f.loc_dir = "/Volumes/GLS/RB Stems Plus Cache";
    f.loc_drive = f.loc_name = "GLS";
    f.loc_shown = "/Volumes/GLS/RB Stems Plus Cache";
    f.loc_size_kind = 1;
    f.loc_size = 1288490189;
    f.loc_gate_id = "ABC";
    f.app_fate = 0;
    f.app_path = "/Applications/RB Stems Plus.app";
    f.dock_flag = 1;
    answers(&f, 2, "Uninstall", "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Uninstall RB Stems Plus?") && !strcmp(f.dlg_buttons[0], "Uninstall|Cancel"));
    CHECK(!strcmp(f.dlg_msg[0], "rekordbox goes back to exactly how it was. Your music and rekordbox library aren't touched.\n\n"
                                "RB Stems Plus will be deleted, including all stems saved in its folder on \"GLS\" (about 1.2 GB), also any saved there by other Macs or accounts."));
    CHECK(frame(&f, "Checking where stems are saved…|1|0"));
    CHECK(in_order(&f, 15, "step MODELS_INFO saved", "step LOCATION_NOW", "dialog Uninstall RB Stems Plus? [Uninstall|Cancel] -> Uninstall",
                   "step BUSY_MARKER uninstall-all Uninstall RB Stems Plus Completely", "step REKORDBOX", "step MODEL_RESTORE", "step SET_CHOSEN 0 0",
                   "step LAUNCH_AGENT_UNLOAD", "log final state: STATE", "step LOCATION_NOW",
                   "step REMOVE_LOCATION /Volumes/GLS/RB Stems Plus Cache ABC 1", "log stems on /Volumes/GLS/RB Stems Plus Cache deleted: ok",
                   "step REMOVE_OWN_FOLDERS 0", "step DOCK /Applications/RB Stems Plus.app", "step SELF_DELETE /Applications/RB Stems Plus.app 1"));
    CHECK(frame(&f, "Deleting the stems saved on \"GLS\"… 7 files|1|2"));
    CHECK(!strcmp(f.dlg_title[1], "RB Stems Plus is uninstalled") && !strcmp(f.dlg_msg[1], "rekordbox is back to how it was.") && !strcmp(f.dlg_buttons[1], "Done"));
    CHECK(f.quit_steps == 1);
    ff_end(&f);
    /* the self-delete can't start (its after-exit cleanup didn't launch): the app stays open and
     * isn't left busy */
    ff_init(&f);
    f.app_fate = 0;
    f.app_path = "/Applications/RB Stems Plus.app";
    f.dock_flag = 1;
    f.self_delete_rc = 1;
    answers(&f, 3, "Uninstall", "Done", "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(in_order(&f, 4, "step SELF_DELETE /Applications/RB Stems Plus.app 1", "log self-delete couldn't start: RB Stems Plus stays open",
                   "step BUSY_MARKER", "dialog RB Stems Plus is uninstalled [OK] -> OK"));
    /* one line says what's left (owner decided, 2026-11-12) */
    CHECK(f.ndialogs == 3 && !strcmp(f.dlg_title[2], "RB Stems Plus is uninstalled") && !strcmp(f.dlg_buttons[2], "OK")
          && !strcmp(f.dlg_msg[2], "RB Stems Plus's own folder couldn't be removed: move RB Stems Plus to the Trash."));
    CHECK(f.quit_steps == 0 && f.held && core_flag(f.held, F_SPINNER) == 0);
    ended_clean(&f, "self-delete failed");
    ff_end(&f);
    /* the app was never to be deleted (another account's, or it can't delete itself): the end
     * dialog said what stays; the failed self-delete adds no alert */
    for (int fate = 1; fate <= 2; fate++) {
        ff_init(&f);
        f.app_fate = fate;
        f.app_path = "/Applications/RB Stems Plus.app";
        f.self_delete_rc = 1;
        answers(&f, 3, "Uninstall", "Done", "OK");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        CHECK(has(&f, "step SELF_DELETE  0") && has(&f, "log self-delete couldn't start: RB Stems Plus stays open"));
        CHECK(f.ndialogs == 2);
        CHECK(f.quit_steps == 0 && f.held && core_flag(f.held, F_SPINNER) == 0);
        ended_clean(&f, fate == 1 ? "self-delete failed, another account's app" : "self-delete failed, can't delete itself");
        ff_end(&f);
    }
    /* another account's Stems Cache kept; the drive gone; the app another account's */
    ff_init(&f);
    f.cache = f.traces = 1;
    f.chosen_cache = 0;
    f.loc_dir = "/Volumes/GLS/RB Stems Plus Cache";
    f.loc_drive = f.loc_name = "GLS";
    f.app_fate = 1;
    f.cache_link = 1;
    answers(&f, 3, "Uninstall", "Keep", "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_msg[0], "rekordbox gets its own stems model back. Stems Cache was installed from another account on this Mac. Your music and rekordbox "
                                "library aren't touched.\n\nRB Stems Plus will be deleted. Its stems on \"GLS\", which isn't connected, stay: connect it first to delete them too."));
    CHECK(!strcmp(f.dlg_title[1], "Stems Cache from another account") && !strcmp(f.dlg_buttons[1], "Remove|Keep"));
    CHECK(has(&f, "log Stems Cache wasn't installed from this account: user chose Keep"));
    CHECK(!strcmp(f.dlg_msg[2], "rekordbox has its own stems model again. Stems Cache stays for the other account. RB Stems Plus stays in the Applications folder: "
                                "it belongs to another account on this Mac. ~/Library/Caches/rbstemsplus is a link, so RB Stems Plus leaves it and what it points to alone. "
                                "The stems saved on \"GLS\" stay: it isn't connected. To delete them, delete the folder \"/Volumes/GLS/RB Stems Plus Cache\" in Finder."));
    CHECK(has(&f, "step SELF_DELETE  0") && !has(&f, "step DOCK"));
    ff_end(&f);
    /* rekordbox gone and the copy of its files kept: stops */
    ff_init(&f);
    f.traces = f.chosen_cache = 1;
    f.root_folder = 1;
    answers(&f, 3, "Uninstall", "Cancel", "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[2], "Uninstall stopped") && !strcmp(f.dlg_msg[2], "The copy of rekordbox's files was kept, so nothing else was deleted. "
                                                                                 "If you moved rekordbox, put it back in the Applications folder, then uninstall again."));
    CHECK(in_order(&f, 3, "log the copy of rekordbox's files stays: stopping, nothing else deleted", "step BUSY_MARKER", "dialog Uninstall stopped [OK] -> OK"));
    ended_clean(&f, "kept root folder");
    ff_end(&f);
    /* Stems Cache couldn't be removed: stops, with the root script's reason */
    ff_init(&f);
    f.cache = f.traces = f.chosen_cache = f.rb_folder = 1;
    f.nadmin_rc = 1;
    f.admin_rc[0] = 7;
    answers(&f, 2, "Uninstall", "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_msg[1], "Stems Cache couldn't be removed, so nothing else was deleted. "
                                                   "RB Stems Plus is already changing rekordbox. Wait a moment, then try again."));
    CHECK(has(&f, "log Stems Cache could not be removed: stopping, nothing else deleted"));
    ended_clean(&f, "cache not removed");
    ff_end(&f);
}

/* SO2: driveDeletionAllowed's table (LocationTests.swift), through the uninstall: the drive's stems
 * are deleted only when the folder found again just before is the one the confirmation named (its
 * marker id in any case); else REMOVE_LOCATION never runs and the final alert says they stay */
static void t_uninstall_all_drive_named(void) {
    static const struct {
        const char *ctx, *first_id, *dir2, *id2;
        int deleted;
    } cases[] = {
        {"connected since the confirmation (none named)", NULL, NULL, "ABC", 0},
        {"another folder", "ABC", "/Volumes/GLS 2/RB Stems Plus Cache", "ABC", 0},
        {"another marker", "ABC", NULL, "ABD", 0},
        {"the named folder, its id in another case", "ABC", NULL, "abc", 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ff f;
        ff_init(&f);
        f.loc_dir = f.loc_shown = "/Volumes/GLS/RB Stems Plus Cache";
        f.loc_drive = f.loc_name = "GLS";
        f.loc_size_kind = 1;
        f.loc_size = 1288490189;
        f.loc_gate_id = cases[i].first_id;
        f.loc2_from = 2;   /* the confirmation's LOCATION_NOW, then the one just before the deletion */
        f.loc2_dir = cases[i].dir2;
        f.loc2_gate_id = cases[i].id2;
        f.app_fate = 1;
        f.app_path = "/Applications/RB Stems Plus.app";
        answers(&f, 2, "Uninstall", "Done");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        CHECKS(count_of(&f, "step LOCATION_NOW") == 2 && f.ndialogs == 2, cases[i].ctx);
        const char *dir = cases[i].dir2 ? cases[i].dir2 : "/Volumes/GLS/RB Stems Plus Cache";
        char kept[512];
        snprintf(kept, sizeof kept, "The stems saved on \"GLS\" stay. To delete them, delete the folder \"%s\" in Finder.", dir);
        if (cases[i].deleted) {
            char rm[256];
            snprintf(rm, sizeof rm, "step REMOVE_LOCATION %s abc 1", dir);
            CHECKS(has(&f, rm) && !strstr(f.dlg_msg[1], kept), cases[i].ctx);
        } else {
            char log[256];
            snprintf(log, sizeof log, "log stems on %s kept: the confirmation didn't name them", dir);
            CHECKS(count_of(&f, "step REMOVE_LOCATION") == 0 && has(&f, log), cases[i].ctx);
            CHECKS(strstr(f.dlg_msg[1], kept) != NULL, cases[i].ctx);   /* T_LEFT_DRIVE_NOT_NAMED */
        }
        ended_clean(&f, cases[i].ctx);
        ff_end(&f);
    }
}

static void t_remove_anyway(void) {
    ff f;
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    f.restore_rc = 1;
    f.restore_code = CORE_MODEL_UNKNOWN;
    f.originals_saved = f.originals_verified = 1;
    f.left_code = 3;
    f.originals_dir = "~/Library/Application Support/rbstemsplus/originals";
    f.help_url = "https://help/#remove-anyway";
    f.app_fate = 2;   /* can't delete itself */
    f.loc_dir = "/Volumes/GLS/RB Stems Plus Cache";
    f.loc_drive = f.loc_name = "GLS";
    f.loc_size_kind = 1;
    f.loc_size = 2147483648;
    f.loc_gate_id = "ABC";
    answers(&f, 3, "Uninstall", "Remove RB Stems Plus Anyway", "Open Help");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Uninstall stopped") && !strcmp(f.dlg_buttons[1], "Cancel|Remove RB Stems Plus Anyway|Open Help") && f.dlg_cancel[1] == -1);
    CHECK(!strcmp(f.dlg_msg[1], "rekordbox's stems model isn't one RB Stems Plus knows, so it was left as it is. Nothing was deleted.\n\n"
                                "Remove RB Stems Plus Anyway deletes only its own files, including all stems saved in its folder on \"GLS\" (about 2.0 GB), "
                                "also any saved there by other Macs or accounts, and keeps its saved copy of rekordbox's own stems model. "
                                "To get rekordbox's own stems model back, click Open Help."));
    CHECK(has(&f, "log   user chose: Remove RB Stems Plus Anyway (the saved copy of Pioneer's model stays)"));
    CHECK(!strcmp(f.dlg_title[2], "RB Stems Plus is removed") && !strcmp(f.dlg_buttons[2], "Done|Open Help"));
    CHECK(!strcmp(f.dlg_msg[2], "rekordbox's stems model isn't one RB Stems Plus knows. The saved copy of rekordbox's own stems model stays in "
                                "~/Library/Application Support/rbstemsplus/originals. To put it back, click Open Help. RB Stems Plus couldn't delete itself: drag it to the Trash."));
    CHECK(in_order(&f, 3, "step REMOVE_LOCATION /Volumes/GLS/RB Stems Plus Cache ABC 1", "step REMOVE_OWN_FOLDERS 1", "step OPEN_URL https://help/#remove-anyway"));
    ff_end(&f);
    /* Open Help there: stops, nothing deleted */
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    f.restore_rc = 1;
    f.restore_code = CORE_MODEL_NO_SAVED_COPY;
    f.left_code = 0;
    f.help_url = "https://help/#no-saved";
    answers(&f, 2, "Uninstall", "Open Help");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_msg[1], "There's no saved copy of rekordbox's own stems model on this Mac. Nothing was deleted.\n\nRemove RB Stems Plus Anyway deletes only its own files. rekordbox has no stems model. "
                                "To get rekordbox's own stems model back, click Open Help."));
    CHECK(in_order(&f, 2, "step BUSY_MARKER", "step OPEN_URL https://help/#no-saved") && !has(&f, "step REMOVE_OWN_FOLDERS 0"));
    ended_clean(&f, "anyway help");
    ff_end(&f);
    /* the model left behind is the failure's own sentence (can't be read): said once (review MS10) */
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    f.restore_rc = 1;
    f.restore_code = CORE_MODEL_UNREADABLE;
    f.left_code = 1;
    f.help_url = "https://help/#no-saved";
    answers(&f, 2, "Uninstall", "Open Help");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_msg[1], "rekordbox's stems model can't be read. Nothing was deleted.\n\nRemove RB Stems Plus Anyway deletes only its own files. "
                                "To get rekordbox's own stems model back, click Open Help."));
    ended_clean(&f, "anyway help, the failure said once");
    ff_end(&f);
    /* removed anyway, and the self-delete can't start: the alert under the removal's own title */
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    f.restore_rc = 1;
    f.restore_code = CORE_MODEL_UNKNOWN;
    f.originals_saved = f.originals_verified = 1;
    f.left_code = 3;
    f.originals_dir = "~/Library/Application Support/rbstemsplus/originals";
    f.help_url = "https://help/#remove-anyway";
    f.app_fate = 0;
    f.app_path = "/Applications/RB Stems Plus.app";
    f.self_delete_rc = 1;
    f.loc_dir = "/Volumes/GLS/RB Stems Plus Cache";
    f.loc_drive = f.loc_name = "GLS";
    f.loc_size_kind = 1;
    f.loc_size = 2147483648;
    f.loc_gate_id = "ABC";
    answers(&f, 4, "Uninstall", "Remove RB Stems Plus Anyway", "Done", "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(in_order(&f, 3, "log self-delete couldn't start: RB Stems Plus stays open", "step BUSY_MARKER",
                   "dialog RB Stems Plus is removed [OK] -> OK"));
    CHECK(f.ndialogs == 4 && !strcmp(f.dlg_msg[3], "RB Stems Plus's own folder couldn't be removed: move RB Stems Plus to the Trash."));
    CHECK(f.quit_steps == 0 && f.held && core_flag(f.held, F_SPINNER) == 0);
    ended_clean(&f, "anyway, self-delete failed");
    ff_end(&f);
}

/* ---------------------------------------------------------------- start-up: welcome, update result, resume, request, cache offer */

static void t_startup(void) {
    ff f;
    ff_init(&f);
    f.welcomed = 0;
    f.update_failed = 1;
    f.update_line = "failed swap\n";
    answers(&f, 2, "Continue", "OK");
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Welcome to RB Stems Plus") && !strcmp(f.dlg_buttons[0], "Continue"));
    CHECK(!strcmp(f.dlg_title[1], "Update failed") &&
          !strcmp(f.dlg_msg[1], "RB Stems Plus wasn't changed. The new version couldn't take this one's place. If macOS said RB Stems Plus was prevented from "
                                "modifying apps, allow it, then choose Update RB Stems Plus again. Otherwise try again later."));
    CHECK(in_order(&f, 6, "step WELCOME check", "step WELCOME done", "step UPDATE_RESULT", "log the update couldn't replace the app (failed swap)",
                   "step LEFTOVER", "step PAYLOAD check"));
    CHECK(f.c->fl.startup_done);
    ended_clean(&f, "startup");
    ff_end(&f);
    /* an interrupted action: resumed once the start-up block is done */
    ff_init(&f);
    f.leftover_id = "install-plus";
    f.leftover_label = "Install Demucs v4";
    f.request_age = 10;
    answers(&f, 1, "Continue");
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Action interrupted") && !strcmp(f.dlg_msg[0], "RB Stems Plus quit before \"Install Demucs v4\" was done. Continue it now?"));
    CHECK(in_order(&f, 9, "log the last run quit during: Install Demucs v4 (install-plus)", "log   user chose: Continue", "step REKORDBOX",
                   "step REQUEST take", "step BUSY_MARKER install-plus Install Demucs v4", "step PAYLOAD check", "step PAYLOAD latest",
                   "step MODEL_COPY", "log state: STATE"));
    CHECK(count_of(&f, "step BRIDGE_SHA") == 0);   /* the cache offer skipped: an action runs */
    ended_clean(&f, "resume");
    ff_end(&f);
    /* Cancel: the marker goes */
    ff_init(&f);
    f.leftover_id = "uninstall-all";
    f.leftover_label = "Uninstall RB Stems Plus Completely";
    answers(&f, 1, "Cancel");
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(in_order(&f, 3, "log   user chose: Cancel", "step BUSY_MARKER", "step REQUEST"));
    ended_clean(&f, "resume cancelled");
    ff_end(&f);
    /* the watcher's request: what went missing, reinstalled */
    ff_init(&f);
    f.chosen_model = 1;
    f.plus = 0;
    f.request_age = 30;
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(in_order(&f, 6, "step REQUEST", "log watcher asked to reinstall", "step REKORDBOX", "step REQUEST take",
                   "step BUSY_MARKER reinstall-missing Reinstall Demucs v4", "step PAYLOAD latest"));
    CHECK(frame(&f, "Reinstalling…|1|2") && f.ndialogs == 0);
    ff_end(&f);
    /* an old request is dropped */
    ff_init(&f);
    f.chosen_model = 1;
    f.request_age = 7200;
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(has(&f, "log watcher's reinstall request is 120 minutes old: ignored") && !has(&f, "step PAYLOAD latest"));
    ff_end(&f);
    /* the Stems Cache update offer */
    ff_init(&f);
    f.cache = f.chosen_cache = 1;
    f.bridge_installed = "3333333333333333333333333333333333333333333333333333333333333333";
    answers(&f, 1, "Update");
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Stems Cache update") && !strcmp(f.dlg_buttons[0], "Update|Not Now") && f.dlg_cancel[0] == 1);
    CHECK(in_order(&f, 6, "step BRIDGE_SHA", "step BLOCKED cache",
                   "log Stems Cache: an update is available (in rekordbox 333333333333, payload 1.4.0 222222222222)", "log   user chose: Update",
                   "step BUSY_MARKER update-cache Update Stems Cache", "admin 1 7.2.10.0342 com.pioneerdj.rekordboxdj 2222222222222222222222222222222222222222222222222222222222222222 ortsha 1"));
    CHECK(frame(&f, "Updating Stems Cache…|1|2") && frame(&f, "Reinstalling Stems Cache (this takes about a minute)…|1|2"));
    CHECK(f.c->fl.cache_update_asked);
    ff_end(&f);
    /* a newer app pending: no offer; the notice */
    ff_init(&f);
    f.cache = f.chosen_cache = 1;
    f.bridge_installed = "3333333333333333333333333333333333333333333333333333333333333333";
    f.app_version = "1.5.0";
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(f.ndialogs == 0 && has(&f, "log RB Stems Plus 1.5.0 is available") && f.c->fl.update_version && !strcmp(f.c->fl.update_version, "1.5.0"));
    CHECK(core_flag(f.held, F_UPDATE_VISIBLE));
    ff_end(&f);
}

/* ---------------------------------------------------------------- the update, Create Report */

/* review MR8: Update during a Prepare run asks to stop it first ("Stop and Update"); Cancel leaves
 * the run going and swaps nothing; Stop logs it, and the swap comes only once the helper has ended */
static int has_now(ff *f, const char *line) {   /* has(), while a flow thread may still write the trace */
    pthread_mutex_lock(&f->mu);
    int r = has(f, line);
    pthread_mutex_unlock(&f->mu);
    return r;
}
static void t_update_stops_preparing(void) {
    ff f;
    ff_init(&f);
    f.app_version = "1.5.0";
    core_test_set_preparing(f.c, 1);
    f.c->p.running = 1;   /* a run under way (its helper ends when the test says) */
    answers(&f, 2, "Update", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"update\"}");
    settle(&f);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_buttons[1], "Stop and Update|Cancel"));
    CHECK(!has(&f, "step UPDATE_SWAP") && !has(&f, "log prepare: stopped for update") && f.c->preparing && f.c->p.running);
    f.c->p.running = 0;
    ff_end(&f);
    ff_init(&f);
    f.app_version = "1.5.0";
    core_test_set_preparing(f.c, 1);
    f.c->p.running = 1;
    answers(&f, 2, "Update", "Stop and Update");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"update\"}");
    for (int i = 0; i < 2000 && !has_now(&f, "log prepare: stopped for update"); i++) {
        pump(&f);
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < 150; i++) {   /* the flow waits for the run's end (stopAndWait polls every 100 ms) */
        pump(&f);
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
    }
    CHECK(has_now(&f, "log prepare: stopped for update") && !has_now(&f, "step UPDATE_SWAP"));
    f.c->p.running = 0;   /* the helper ended */
    settle(&f);
    CHECK(in_order(&f, 2, "log prepare: stopped for update", "step UPDATE_SWAP"));
    ff_end(&f);
}

/* CF1: the flow's end turns a busy state left on off (flows_flow_end_busy), except at the two
 * endings where the app quits: the update's swap went through, and Uninstall All's self-delete
 * started. There the spinner turns until the app is gone */
static void t_quit_keeps_busy(void) {
    ff f;
    ff_init(&f);
    f.app_version = "1.5.0";   /* update_swap_rc 0: the swap went through */
    answers(&f, 1, "Update");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"update\"}");
    settle(&f);
    CHECK(has(&f, "step UPDATE_SWAP") && !f.c->flow_running && atomic_load(&f.c->life->flows_out) == 0);
    CHECK(f.held && core_flag(f.held, F_SPINNER) == 1);
    ff_end(&f);
    ff_init(&f);
    f.app_fate = 0;
    f.app_path = "/Applications/RB Stems Plus.app";
    f.dock_flag = 1;
    answers(&f, 2, "Uninstall", "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(f.quit_steps == 1 && !f.c->flow_running && atomic_load(&f.c->life->flows_out) == 0);
    CHECK(f.held && core_flag(f.held, F_SPINNER) == 1);
    ff_end(&f);
}

/* OM2: Install Stems Cache's EF_BUSY is lost, the admin run's accepted password still shows busy
 * (the spinner, the status line), then every allocation fails and the flow stops at its next
 * query: the flow's end turns busy off, as it does when EF_BUSY came */
static void t_admin_busy_lost(void) {
    ff f;
    ff_init(&f);
    f.paused_floor = 20000000000;
    f.paused_drive = "GLS";
    answers(&f, 1, "Install");
    pthread_mutex_lock(&f.mu);
    f.om2 = 1;
    pthread_mutex_unlock(&f.mu);
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    int armed = 0;
    for (int i = 0; i < 5000 && !armed; i++) {
        pump(&f);
        pthread_mutex_lock(&f.mu);
        armed = f.om2 == 3;
        pthread_mutex_unlock(&f.mu);
        if (!armed) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    }
    CHECK(armed);
    /* main isn't run meanwhile: the admin progress and the flow's end wait in its queue, in order */
    for (int i = 0; i < 5000 && atomic_load(&f.c->life->flows_out) > 0; i++) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    core_test_fail_allocs(f.c, 0);
    settle(&f);
    CHECK(f.admin_attempts == 1 && !f.c->flow_running && !f.c->action_running);
    CHECK(f.held && core_flag(f.held, F_SPINNER) == 0);
    size_t n = 0;
    const char *st = f.held ? core_str(f.held, F_STATUS_LINE, &n) : NULL;
    CHECK(st && strcmp(st, "Installing Stems Cache (this takes about a minute)…") != 0);
    ff_end(&f);
}

static void t_update_report(void) {
    ff f;
    ff_init(&f);
    f.app_version = "1.5.0";
    f.update_swap_rc = 1;
    answers(&f, 2, "Update", "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"update\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Update RB Stems Plus to 1.5.0?") && !strcmp(f.dlg_msg[0], "Replaces this version with RB Stems Plus 1.5.0."));
    CHECK(!strcmp(f.dlg_title[1], "Download failed") && !strcmp(f.dlg_msg[1], "Nothing was changed. Check your internet connection and try again."));
    CHECK(frame(&f, "Updating RB Stems Plus…|1|2") && frame(&f, "Downloading RB Stems Plus 1.5.0… 50%|1|2"));
    CHECK(in_order(&f, 3, "step UPDATE_CHECK", "step BUSY_MARKER", "step UPDATE_SWAP"));
    ended_clean(&f, "update failed");
    ff_end(&f);
    ff_init(&f);
    f.app_version = "1.5.0";
    f.update_check = 2;
    answers(&f, 1, "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"update\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "Administrator needed") && !strcmp(f.dlg_msg[0], "Only an administrator can update RB Stems Plus on this Mac."));
    ff_end(&f);
    ff_init(&f);   /* no newer app: nothing */
    ev(&f, "{\"ev\":\"menu\",\"id\":\"update\"}");
    settle(&f);
    CHECK(f.ndialogs == 0 && !has(&f, "step UPDATE_CHECK"));
    ff_end(&f);
    ff_init(&f);
    answers(&f, 1, "Open GitHub Issue");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(frame(&f, "Creating a report…|1|2"));
    CHECK(!strcmp(f.dlg_title[0], "Report saved") &&
          !strcmp(f.dlg_msg[0], "It's saved as ~/Desktop/r.zip. To get help, click Open GitHub Issue, then drag the report into the issue."));
    CHECK(in_order(&f, 6, "step REPORT_FACTS", "step REPORT_ZIP 2 Demucs v4 is off. 0 Stems Cache is on. 0", "step BUSY_MARKER",
                   "log report: ~/Desktop/r.zip", "step REVEAL /Users/x/Desktop/r.zip", "step OPEN_URL https://example.invalid/issue"));
    ended_clean(&f, "report");
    ff_end(&f);
    /* the lights measured now: rekordbox's own model with a model its bridge doesn't list; the
     * chosen drive not connected (the light's text is the location's: the link names its state);
     * paused for lack of space */
    ff_init(&f);
    f.report_sha = "cc";
    answers(&f, 1, "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(has(&f, "step REPORT_ZIP 2 Demucs v4 is off. 1 Stems Cache doesn't support STEMS Engine 0002 yet. When an RB Stems Plus update supports it, click Reinstall Stems Cache. 0"));
    ff_end(&f);
    ff_init(&f);
    f.report_plus = f.report_loc = 1;
    answers(&f, 1, "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(strstr(f.trace, "step REPORT_ZIP 0 Demucs v4 is on. 1 ") && strstr(f.trace, "GLS") && has_suffix_line(&f, "step REPORT_ZIP ", " 1"));
    ff_end(&f);
    ff_init(&f);
    f.report_plus = 1;
    f.report_floor = 50000000000;
    answers(&f, 1, "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(strstr(f.trace, "step REPORT_ZIP 0 Demucs v4 is on. 1 ") && has_suffix_line(&f, "step REPORT_ZIP ", " 0") && !strstr(f.trace, "Stems Cache is on.") &&
          strstr(f.trace, "1 Stems Cache stopped saving stems: your disk has less than 50 GB free. 0"));
    ff_end(&f);
    ff_init(&f);
    f.report_plus = 1; f.report_disabled = 1;
    f.report_floor = 50000000000;
    answers(&f, 1, "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(has(&f, "step REPORT_ZIP 0 Demucs v4 is on. 0 Stems Cache is on. 0"));
    ff_end(&f);
    ff_init(&f);
    f.report_plus = 1; f.report_drive = "GLS";
    f.report_floor = 50000000000;
    answers(&f, 1, "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(has(&f, "step REPORT_ZIP 0 Demucs v4 is on. 1 Stems Cache stopped saving stems: \"GLS\" has less than 50 GB free. 0"));
    ff_end(&f);
    {   /* an engine version past COMPOSE's 16 KiB: the tip is whole */
        static char eng[17001];
        memset(eng, '7', sizeof eng - 1);
        ff_init(&f);
        f.report_sha = "cc"; f.report_engine = eng;
        answers(&f, 1, "Done");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
        settle(&f);
        CHECK(strstr(f.trace, "1 Stems Cache doesn't support STEMS Engine 7777") && f.report_tip_len > sizeof eng);
        ff_end(&f);
    }
    /* the measurement failed: no zip, the report failed */
    ff_init(&f);
    f.report_facts_rc = 1;
    answers(&f, 1, "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(!has(&f, "step REPORT_ZIP") && !strstr(f.trace, "step REPORT_ZIP ") && !strcmp(f.dlg_title[0], "Report failed"));
    ff_end(&f);
    ff_init(&f);
    f.model_copy_rc = 1;   /* the fake's report result */
    answers(&f, 1, "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Report failed") && !strcmp(f.dlg_msg[0], "The report couldn't be created. See the log in this window."));
    ff_end(&f);
}

/* ---------------------------------------------------------------- Settings and the location */

static void t_settings(void) {
    ff f;
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_size_kind = 1;
    f.set_size = 1288490189;
    f.set_clear = 1;
    f.set_can_choose = 1;
    f.fields[0] = " 1,5 ";
    f.fields[1] = "45";
    answers(&f, 1, "Save");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Settings") && !strcmp(f.dlg_msg[0], "Stems Cache uses these settings the next time rekordbox opens."));
    CHECK(!strcmp(f.dlg_buttons[0], "Save|Clear Saved Stems (about 1.2 GB)|Cancel") && f.dlg_cancel[0] == 2 && f.dlg_enabled[0] == 7);
    CHECK(in_order(&f, 3, "step SETTINGS open", "step CONFIG_WRITE max_gb 1.5 max_days 45", "log settings: max_gb=X max_days=Y"));
    ended_clean(&f, "settings save");
    ff_end(&f);
    /* out of range, then Cancel */
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_location = 1;
    f.set_label_path = "DJ";
    f.fields[0] = "0";
    answers(&f, 3, "Save", "OK", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Size out of range") && !strcmp(f.dlg_msg[1], "Enter a size from 0.1 to 100,000 GB.") && !strcmp(f.dlg_title[2], "Settings"));
    CHECK(!has(&f, "step CONFIG_WRITE max_gb 0 max_days 30") && has(&f, "step SETTINGS again"));
    CHECK((f.dlg_enabled[0] & 2) == 0);   /* nothing to clear */
    ff_end(&f);
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_location = 1;
    f.set_label_path = "DJ";
    f.fields[1] = "36501";
    answers(&f, 3, "Save", "OK", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Days out of range") && !strcmp(f.dlg_msg[1], "Enter a number of days from 1 to 36,500."));
    ff_end(&f);
    /* unreadable: only Cancel */
    ff_init(&f);
    f.set_unreadable = 1;
    f.set_location = 2;
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_msg[0], "Stems Cache can't read its settings (config.ini), so nothing can be changed here. See Help › RB Stems Plus Troubleshooting."));
    CHECK(f.dlg_enabled[0] == 4);
    ff_end(&f);
    /* Clear on this Mac, then back to the sheet */
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_size_kind = 1;
    f.set_size = 1288490189;
    f.set_clear = 1;
    answers(&f, 3, "Clear Saved Stems (about 1.2 GB)", "Clear", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Clear saved stems (about 1.2 GB)?") && !strcmp(f.dlg_buttons[1], "Clear|Cancel") &&
          !strcmp(f.dlg_msg[1], "Deletes the saved stems. rekordbox separates those tracks again the next time you load them."));
    CHECK(in_order(&f, 4, "step REKORDBOX", "step CLEAR_THIS_MAC", "log saved stems cleared (about 1.2 GB): ok", "step LOCATION_REMEASURE"));
    CHECK(f.ndialogs == 3 && !strcmp(f.dlg_title[2], "Settings"));
    ff_end(&f);
    /* Clear on a drive: in the background */
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_location = 1;
    f.set_label_path = "DJ";
    f.set_clear = 3;
    f.loc_dir = "/Volumes/GLS/RB Stems Plus Cache";
    f.loc_gate_id = "ABC";
    f.loc_drive = "GLS";
    f.loc_shown = "/Volumes/GLS/RB Stems Plus Cache";
    f.remove_location_rc = 1;
    answers(&f, 3, "Clear Saved Stems", "Clear", "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_msg[1], "Deletes all stems saved in this folder on \"GLS\", including any saved there by other Macs or accounts."));
    CHECK(frame(&f, "Clearing saved stems on \"GLS\"…|1|2") && frame(&f, "Clearing saved stems on \"GLS\"… 7 files|1|2"));
    CHECK(in_order(&f, 5, "step REMOVE_LOCATION /Volumes/GLS/RB Stems Plus Cache ABC 0", "log saved stems on /Volumes/GLS/RB Stems Plus Cache cleared: FAILED (Demucs: Resource busy)",
                   "step BUSY_MARKER", "step LOCATION_REMEASURE", "dialog Saved stems not cleared [OK] -> OK"));
    CHECK(!strcmp(f.dlg_msg[2], "Some saved stems on \"GLS\" couldn't be deleted (Demucs: Resource busy)."));
    ended_clean(&f, "clear drive");
    ff_end(&f);
    /* Use This Mac */
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_location = 1;
    f.set_label_path = "DJ";
    f.set_old_drive = "GLS";
    f.row_answer[0] = 1;
    answers(&f, 3, "", "Use This Mac", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Use This Mac?") && !strcmp(f.dlg_msg[1], "Stems Cache will save stems on this Mac. The stems saved on \"GLS\" stay there."));
    CHECK(in_order(&f, 4, "step LOCATION_WRITE    0", "log settings: stems are saved on this Mac from rekordbox's next start", "step LOCATION_REMEASURE", "step SETTINGS again"));
    ff_end(&f);
    /* Change…: refused, then a new folder */
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_location = 1;
    f.set_label_path = "DJ";
    f.row_answer[0] = 0;
    f.pick = "/Volumes/GLS/DJ";
    f.probe_why = 3;
    f.probe_drive = "GLS";
    answers(&f, 3, "", "OK", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(has(&f, "pick_folder /Volumes -> /Volumes/GLS/DJ") && has(&f, "log settings: ~/picked refused (readOnly)"));
    CHECK(!strcmp(f.dlg_title[1], "Can't save stems there") && !strcmp(f.dlg_msg[1], "\"GLS\" is read-only."));
    CHECK(frame(&f, "Checking the folder…|1|0"));
    ff_end(&f);
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_location = 1;
    f.set_label_path = "DJ";
    f.row_answer[0] = 0;
    f.pick = "/Volumes/GLS/DJ";
    f.probe_real = "/Volumes/GLS/DJ";
    f.probe_drive = "GLS";
    f.probe_mount = "/Volumes/GLS";
    f.discover = 0;
    f.size_this_mac = 3LL << 30;
    f.this_mac_now = 1;
    answers(&f, 3, "", "Use \"GLS\"", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Use \"GLS\"?") && !strcmp(f.dlg_buttons[1], "Use \"GLS\"|Cancel"));
    CHECK(!strcmp(f.dlg_msg[1], "Stems Cache will save stems on \"GLS\". The 3.0 GB saved on this Mac will be deleted: rekordbox separates those tracks again the next time you load them."));
    CHECK(in_order(&f, 9, "step LOCATION_PROBE /Volumes/GLS/DJ", "step LOCATION_DISCOVER /Volumes/GLS/DJ /Volumes/GLS", "step LOCATION_SIZE this_mac",
                   "step LOCATION_CREATE /Volumes/GLS/DJ", "step LOCATION_WRITE /Volumes/GLS/DJ/RB Stems Plus Cache NEWID /Volumes/GLS 1",
                   "log settings: stems are saved in <dir> from rekordbox's next start (a new folder)", "step THIS_MAC_DELETE delete",
                   "log settings: the stems saved on this Mac deleted: ok", "step LOCATION_REMEASURE"));
    CHECK(frame(&f, "Looking for saved stems on \"GLS\"…|1|0") && frame(&f, "Deleting the stems saved on this Mac…|1|0"));
    ended_clean(&f, "change location");
    ff_end(&f);
    /* the same location, renamed: only cache_dir changes */
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_location = 1;
    f.set_label_path = "DJ";
    f.row_answer[0] = 0;
    f.pick = "/Volumes/GLS2";
    f.probe_real = "/Volumes/GLS2";
    f.probe_drive = "GLS2";
    f.probe_mount = "/Volumes/GLS2";
    f.discover = 2;
    f.disc_dir = "/Volumes/GLS2/RB Stems Plus Cache";
    f.disc_id = "abc";
    f.disc_current = "/Volumes/GLS/RB Stems Plus Cache";
    f.disc_current_id = "ABC";
    f.disc_old_drive = "GLS";
    f.size_dir = 40LL << 20;
    answers(&f, 3, "", "Use Them", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Saved stems found") && !strcmp(f.dlg_msg[1], "Found your saved stems on \"GLS2\" (40 MB)."));
    CHECK(has(&f, "step LOCATION_WRITE /Volumes/GLS2/RB Stems Plus Cache abc /Volumes/GLS2 0") && !has(&f, "step THIS_MAC_DELETE check"));
    ff_end(&f);
}

/* ---------------------------------------------------------------- the watcher's prompt */

typedef struct { int mo, co, eok, rok, pw; const char *title, *msg, *buttons; int left2; } wcase;
typedef struct { ff *f; wcase w; int r, dont; core_life *life; } wrun;
static void watcher_flow(core_life *life, void *arg) {
    wrun *w = arg;
    w->r = flows_watcher_prompt(life, w->w.mo, w->w.co, "2.0.1", "7.2.10", w->w.eok, w->w.rok, w->w.pw, &w->dont);
}
static void t_watcher_prompt(void) {
    static const wcase cases[] = {
        {1, 0, 0, 1, 0, "rekordbox installed a new STEMS Engine", "Demucs v4 is off. It isn't available for STEMS Engine 2.0.1 yet. RB Stems Plus checks again each "
         "time you open it, and asks you to reinstall once it can.", "OK|Don't Ask Again for This Version", 1},
        {0, 1, 1, 0, 0, "rekordbox was updated", "Stems Cache is off. It doesn't support rekordbox 7.2.10 yet. RB Stems Plus checks again each time you open it, "
         "and asks you to reinstall once it can.", "OK|Don't Ask Again for This Version", 1},
        {1, 1, 0, 0, 0, "rekordbox was updated", "Demucs v4 and Stems Cache are off. Demucs v4 isn't available for STEMS Engine 2.0.1 yet, and Stems Cache doesn't "
         "support rekordbox 7.2.10 yet. RB Stems Plus checks again each time you open it, and asks you to reinstall once it can.", "OK|Don't Ask Again for This Version", 1},
        {1, 1, 1, 0, 0, "rekordbox was updated", "Demucs v4 and Stems Cache are off. Reinstall Demucs v4 to separate tracks with it. Stems Cache doesn't support "
         "rekordbox 7.2.10 yet, so it stays off for now.", "Reinstall Now|Remind Me Later|Don't Ask Again for This Version", 0},
        {1, 1, 0, 1, 0, "rekordbox was updated", "Demucs v4 and Stems Cache are off. Reinstall Stems Cache to reuse the stems it has saved. Demucs v4 isn't available "
         "for STEMS Engine 2.0.1 yet, so it stays off for now.", "Reinstall Now|Remind Me Later|Don't Ask Again for This Version", 0},
        {1, 1, 1, 1, 0, "rekordbox was updated", "Demucs v4 and Stems Cache are off. Reinstall them to separate tracks with Demucs v4 and reuse the stems Stems Cache "
         "has saved.", "Reinstall Now|Remind Me Later|Don't Ask Again for This Version", 0},
        {0, 1, 1, 1, 1, "rekordbox was updated", "Stems Cache is off (Demucs v4 still works). Reinstall it to reuse the stems it has saved.",
         "Reinstall Now|Remind Me Later|Don't Ask Again for This Version", 0},
        {1, 0, 1, 1, 0, "rekordbox put its own stems model back", "Demucs v4 is off. Reinstall it to separate tracks with Demucs v4 instead of rekordbox's own model.",
         "Reinstall Now|Remind Me Later|Don't Ask Again for This Version", 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ff f;
        ff_init(&f);
        answers(&f, 1, "Don't Ask Again for This Version");
        wrun *w = calloc(1, sizeof *w);
        w->w = cases[i];
        w->r = 99;
        core_life *l = life_retain(f.c->life);
        CHECK(flow_start_ex(f.c, CORE_WORK_FLOW, watcher_flow, w, NULL, 0) == CORE_OK);
        settle(&f);
        CHECKS(!strcmp(f.dlg_title[0], cases[i].title) && !strcmp(f.dlg_msg[0], cases[i].msg) && !strcmp(f.dlg_buttons[0], cases[i].buttons), cases[i].msg);
        CHECKS(w->dont == 1 && w->r == (cases[i].left2 ? -1 : 2), cases[i].msg);
        CHECKS(f.dlg_cancel[0] == (cases[i].left2 ? -1 : 1), cases[i].msg);
        ff_end(&f);
        life_release(l);
    }
}


/* Q1, Q19: the root runs get rekordbox's version and bundle id; the model's sha is the memoised slow fact */
static void t_admin_args(void) {
    ff f;
    ff_init(&f);
    f.installed_sha = "1111111111111111111111111111111111111111111111111111111111111111";   /* the payload's model: in place */
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    settle(&f);
    CHECK(has(&f, "slow MODEL_SHA /Users/x/Library/Pioneer/rekordbox/hdemucs.onnx") && !has(&f, "step DOWNLOAD model") && !has(&f, "step MODEL_COPY"));
    CHECK(has(&f, "log Demucs v4: already in place"));
    ff_end(&f);
    ff_init(&f);   /* forced: no sha asked */
    f.plus = f.chosen_model = 1;
    answers(&f, 1, "Reinstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"reinstall_plus\"}");
    settle(&f);
    CHECK(f.slow_calls == 0 && has(&f, "step MODEL_COPY"));
    ff_end(&f);
    ff_init(&f);
    f.cache = f.chosen_cache = f.traces = 1;
    f.root_folder = 1;
    f.rb_folder = 1;
    f.nadmin_rc = 2;
    f.admin_rc[0] = ROOT_REKORDBOX_MISSING;
    answers(&f, 2, "Uninstall", "Remove");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    settle(&f);
    CHECK(in_order(&f, 4, "admin 2 7.2.10.0342 com.pioneerdj.rekordboxdj - - 0", "admin 4 - - - - 0", "admin 3   - - 0", "admin 4 - - - - 0"));
    ff_end(&f);
}

/* SO15: removeCache with no copy of rekordbox's files left asks nothing and runs nothing more:
 * rekordbox and that folder both gone; rekordbox gone during the uninstall's run, no folder */
static void t_uninstall_cache_gone(void) {
    for (int during = 0; during < 2; during++) {
        const char *ctx = during ? "gone during the run, no root folder" : "both gone";
        ff f;
        ff_init(&f);
        f.cache = during;
        f.chosen_cache = f.traces = 1;
        f.rb_folder = during;
        f.root_folder = 0;
        if (during) {
            f.nadmin_rc = 1;
            f.admin_rc[0] = ROOT_REKORDBOX_MISSING;
        }
        answers(&f, 2, "Uninstall", "Remove");
        ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
        settle(&f);
        CHECKS(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "Uninstall Stems Cache?"), ctx);   /* no "rekordbox missing" */
        CHECKS(!has(&f, "log rekordbox isn't in the Applications folder: asking to remove the copy of its files"), ctx);
        CHECKS(count_of(&f, "admin 3 ") == 0 && count_of(&f, "step CACHE_FOLDERS") == 1 + during, ctx);
        CHECKS(count_of(&f, "admin 2 ") == during, ctx);   /* the uninstall's own run only while rekordbox was there */
        CHECKS(has(&f, "log state: STATE"), ctx);
        ended_clean(&f, ctx);
        ff_end(&f);
    }
}

/* Q3: a rekordbox check that fails stops the action; nothing changes after it */
static void t_rb_check_fails(void) {
    ff f;
    ff_init(&f);
    f.fail_step_at = 1;   /* preflight's REKORDBOX */
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    settle(&f);
    CHECK(has(&f, "log   rekordbox check failed: stopped") && f.ndialogs == 0 && !has(&f, "step BUSY_MARKER install-plus Install Demucs v4"));
    ff_end(&f);
}

/* Q6: a confirmation answered after chosen() or the status gen moved stops quietly */
static void t_changed_while_asking(void) {
    ff f;
    ff_init(&f);
    f.change_chosen_at = 0;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    settle(&f);
    CHECK(has(&f, "log   changed while asking: stopped") && f.ndialogs == 1 && !has(&f, "step PAYLOAD latest"));
    CHECK(has(&f, "step BUSY_MARKER") && !has(&f, "step BUSY_MARKER install-plus Install Demucs v4"));
    ended_clean(&f, "changed while asking");
    ff_end(&f);
    ff_init(&f);
    f.bump_status_gen_at = 0;
    f.plus = f.chosen_model = 1;
    answers(&f, 1, "Uninstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}");
    settle(&f);
    CHECK(has(&f, "log   changed while asking: stopped") && !has(&f, "step MODEL_RESTORE"));
    ff_end(&f);
}

/* Q12: activation asks the watcher's request, with reopen's guards; Q9: a payload.json fetched during a
 * flow offers the cache update once it ends */
static void t_activation_and_offer(void) {
    ff f;
    ff_init(&f);
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    f.chosen_model = 1;
    f.request_age = 5;
    CHECK(ev(&f, "{\"ev\":\"activated\"}") == CORE_OK);
    settle(&f);
    CHECK(in_order(&f, 2, "log watcher asked to reinstall", "step BUSY_MARKER reinstall-missing Reinstall Demucs v4"));
    ff_end(&f);
    ff_init(&f);   /* F15: "shown" twice */
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(has(&f, "log core: shown again: ignored") && count_of(&f, "step WATCHER_CHECK") == 1);
    ff_end(&f);
}

/* SO9: rekordbox already has payload.json's bridge: the start-up's cache offer reads both
 * checksums and stops there, with no slow install check (BLOCKED) and no question */
static void t_offer_matching(void) {
    ff f;
    ff_init(&f);
    f.cache = f.chosen_cache = 1;
    f.bridge_installed = f.bridge_sha;   /* the payload's own */
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(has(&f, "step BRIDGE_SHA") && count_of(&f, "step BLOCKED") == 0);
    CHECK(count_of(&f, "dialog Stems Cache update") == 0 && !f.c->fl.cache_update_asked && !has(&f, "log Stems Cache: an update is available"));
    ended_clean(&f, "matching checksums");
    ff_end(&f);
}

/* SO10: the watcher's request (readyForQueuedAction) isn't started while rekordbox's installer or
 * updater runs: updating from the first check, or once rekordbox was closed after a Try Again */
static void t_request_updating(void) {
    for (int after_try = 0; after_try < 2; after_try++) {
        const char *ctx = after_try ? "updating after Try Again" : "updating from the start";
        ff f;
        ff_init(&f);
        f.chosen_model = 1;
        f.plus = 0;
        f.request_age = 30;
        f.updating_n = 1;
        if (after_try) {
            f.rb_open_n = 1;
            f.rb_open_from = 2;    /* the first check: not updating; the loop's: open (Try Again) */
            f.updating_from = 4;   /* the loop's next: closed; the check after it: updating */
            answers(&f, 2, "Try Again", "OK");
        } else answers(&f, 1, "OK");
        ev(&f, "{\"ev\":\"shown\"}");
        settle(&f);
        CHECKS(has(&f, "log watcher asked to reinstall") && has(&f, "log   rekordbox is updating: not started"), ctx);
        CHECKS(count_of(&f, "step BUSY_MARKER") == 0 && !has(&f, "step PAYLOAD latest"), ctx);
        CHECKS(f.ndialogs == 1 + after_try && !strcmp(f.dlg_title[after_try], "rekordbox updating"), ctx);
        if (after_try) CHECKS(!strcmp(f.dlg_title[0], "rekordbox open") && !strcmp(f.dlg_buttons[0], "Try Again|Cancel"), ctx);
        CHECKS(has(&f, "step REQUEST take"), ctx);   /* turned down: handled */
        ended_clean(&f, ctx);
        ff_end(&f);
    }
}

/* SO14: Reinstall Now reinstalls only what went missing: both chosen, Demucs v4 still there and
 * Stems Cache gone (a rekordbox update) is Stems Cache alone, the model untouched */
static void t_request_cache_only(void) {
    ff f;
    ff_init(&f);
    f.chosen_model = f.chosen_cache = 1;
    f.plus = 1;
    f.cache = 0;
    f.request_age = 30;
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(in_order(&f, 3, "log watcher asked to reinstall", "step REQUEST take", "step BUSY_MARKER reinstall-missing Reinstall Stems Cache"));
    CHECK(count_of(&f, "step BUSY_MARKER reinstall-missing") == 1 && count_of(&f, "step DOWNLOAD model") == 0 && count_of(&f, "step MODEL_COPY") == 0);
    ended_clean(&f, "cache only");
    ff_end(&f);
}

/* Q20: startupChecks' work block first, in its order, before anything is resumed */
static void t_startup_work(void) {
    ff f;
    ff_init(&f);
    f.chosen_cache = 1;
    f.leftover_id = "install-plus";
    f.leftover_label = "Install Demucs v4";
    answers(&f, 1, "Cancel");
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(in_order(&f, 9, "step WATCHER_CHECK", "step MIGRATE_LEGACY", "step REMOVE_LEFTOVERS", "step THIS_MAC_DELETE facts",
                   "step CONFIG_SETS rekordbox_model", "step CONFIG_WRITE rekordbox_model 1",
                   "log Stems Cache: turned on for rekordbox's own stems model too (rekordbox_model=1): ok", "step SETTINGS open", "step LEFTOVER"));
    ff_end(&f);
}

/* ---------------------------------------------------------------- every flow: a failing step, a cancelled dialog, core_free mid-flow */

typedef struct { const char *name, *event; void (*setup)(ff *f); } scenario;

static void sc_install(ff *f) { answers(f, 4, "Install", "Try Again", "OK", "OK"); f->nadmin_rc = 2; f->admin_rc[0] = 1 | FAKE_NP; f->paused_floor = 1; }
static void sc_reinstall(ff *f) { f->plus = f->cache = f->chosen_model = f->chosen_cache = 1; answers(f, 3, "Reinstall", "Try Again", "OK"); f->rb_open_n = 0; }
static void sc_uninstall_plus(ff *f) { f->plus = f->chosen_model = 1; f->restore_rc = 1; f->restore_msg = "m"; answers(f, 2, "Uninstall", "Open Help"); }
static void sc_uninstall_cache(ff *f) { f->cache = f->chosen_cache = f->traces = f->rb_folder = f->root_folder = 1; f->nadmin_rc = 1; f->admin_rc[0] = 9; answers(f, 3, "Uninstall", "Remove", "OK"); }
static void sc_uninstall_all(ff *f) {
    f->cache = f->traces = f->rb_folder = 1;
    f->plus = f->chosen_model = 1;
    f->restore_rc = 1;
    f->restore_msg = "m";
    f->loc_dir = "/Volumes/GLS/C";
    f->loc_gate_id = "A";
    f->loc_drive = f->loc_name = "GLS";
    f->originals_verified = 1;
    f->app_fate = 0;
    f->app_path = "/Applications/RB Stems Plus.app";
    answers(f, 5, "Uninstall", "Remove", "Remove RB Stems Plus Anyway", "Done", "OK");
}
static void sc_startup(ff *f) {
    f->welcomed = 0;
    f->update_failed = 1;
    f->update_line = "failed app";
    f->leftover_id = "reinstall-missing";
    f->leftover_label = "Reinstall Demucs v4";
    f->chosen_model = 1;
    answers(f, 4, "Continue", "OK", "Continue", "OK");
}
static void sc_offer(ff *f) {
    f->cache = f->chosen_cache = 1;
    f->bridge_installed = "3333333333333333333333333333333333333333333333333333333333333333";
    f->payload_due = 1;
    answers(f, 2, "Update", "OK");
}
static void sc_update(ff *f) { f->app_version = "1.5.0"; f->update_swap_rc = 3; answers(f, 2, "Update", "OK"); }
static void sc_report(ff *f) { answers(f, 1, "Open GitHub Issue"); }
static void sc_settings(ff *f) {
    f->set_gb = "20";
    f->set_days = "30";
    f->set_location = 1;
    f->set_label_path = "";
    f->set_clear = 3;
    f->loc_dir = "/Volumes/GLS/C";
    f->loc_gate_id = "A";
    f->loc_drive = "GLS";
    f->pick = "/Volumes/GLS";
    f->probe_real = "/Volumes/GLS";
    f->probe_drive = "GLS";
    f->probe_mount = "/Volumes/GLS";
    f->discover = 2;
    f->disc_dir = "/Volumes/GLS/RB Stems Plus Cache";
    f->disc_id = "X";
    f->this_mac_now = 1;
    f->row_answer[0] = 0;
    f->row_answer[2] = 1;
    answers(f, 7, "", "Use \"GLS\"", "", "Use This Mac", "Clear Saved Stems", "Clear", "OK");
}

static const scenario scenarios[] = {
    {"install cache", "{\"ev\":\"click\",\"id\":\"install_cache\"}", sc_install},
    {"reinstall", "{\"ev\":\"click\",\"id\":\"reinstall\"}", sc_reinstall},
    {"uninstall plus", "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}", sc_uninstall_plus},
    {"uninstall cache", "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}", sc_uninstall_cache},
    {"uninstall all", "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}", sc_uninstall_all},
    {"startup", "{\"ev\":\"shown\"}", sc_startup},
    {"cache offer", "{\"ev\":\"shown\"}", sc_offer},
    {"update", "{\"ev\":\"menu\",\"id\":\"update\"}", sc_update},
    {"report", "{\"ev\":\"menu\",\"id\":\"create_report\"}", sc_report},
    {"settings", "{\"ev\":\"menu\",\"id\":\"settings\"}", sc_settings},
};

/* one run; mode 0: as scripted (counts the steps, dialogs and host calls), 1: step k fails,
 * 2: dialog k cancelled, 3: core_free at host call k */
static void *late_calls(void *p) {
    core_flow_progress(p, CORE_PROGRESS_DOWNLOAD, 1, 2);
    core_admin_note(p, CORE_ADMIN_NOTE_NOT_PERMITTED);
    return NULL;
}

static int matrix_fetch_hits;   /* the held fetches that a core_free met, summed (t_late_fetch) */

/* flows_test_fetch_hold: the background payload fetch waits, past core_run's liveness check and
 * before its step, until a core_free has come or the run lets it go */
static void fetch_hold(void *ctx) {
    ff *f = ctx;
    pthread_mutex_lock(&f->mu);
    f->holding++;
    pthread_cond_broadcast(&f->cv);
    while (!f->freed && !f->release) pthread_cond_wait(&f->cv, &f->mu);
    f->fetch_hits += f->freed;
    f->holding--;
    pthread_mutex_unlock(&f->mu);
}

/* pumps main until every worker but a held fetch has ended and the queue is empty (at most ~10 s) */
static void settle_but_held(ff *f) {
    for (int i = 0; i < 10000; i++) {
        int n = pump(f);
        pthread_mutex_lock(&f->mu);
        int idle = !f->head && f->running <= f->holding;
        pthread_mutex_unlock(&f->mu);
        if (!n && idle) break;
        if (!n) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 2000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_mutex_lock(&f->mu);
            if (!f->head && f->running > f->holding) pthread_cond_timedwait(&f->cv, &f->mu, &ts);
            pthread_mutex_unlock(&f->mu);
        }
    }
}

static void run_one(const scenario *sc, int mode, int k, int *nsteps, int *ndialogs, int *ncalls) {
    ff f;
    ff_init(&f);
    sc->setup(&f);
    if (mode == 1) f.fail_step_at = k;
    if (mode == 2) f.cancel_at = k;
    if (mode == 3) f.free_at = k;
    core_life *l = life_retain(f.c->life);
    ev(&f, sc->event);
    /* a fetch still held once everything else has ended is let go, so a run without a free (or
     * with one after it) can't hang */
    settle_but_held(&f);
    pthread_mutex_lock(&f.mu);
    f.release = 1;
    pthread_cond_broadcast(&f.cv);
    pthread_mutex_unlock(&f.mu);
    settle(&f);
    for (int i = 0; i < f.nthreads; i++) pthread_join(f.threads[i], NULL);
    f.nthreads = 0;
    char ctx[96];
    snprintf(ctx, sizeof ctx, "%s, mode %d, at %d", sc->name, mode, k);
    if (mode == 1) {   /* Q3: the step that failed (by its number, F21), and what followed it */
        char want[32], failed_name[64] = "";
        snprintf(want, sizeof want, "step#%d ", k);
        const char *hit = strstr(f.ntrace, want);
        if (hit) sscanf(hit + strlen(want), "%63s", failed_name);
        int check_failed = !strcmp(failed_name, "REKORDBOX") || !strcmp(failed_name, "SPACE") || !strcmp(failed_name, "BLOCKED");
        /* the failed step's place in the full trace: the k-th "step " line there runs in the same order
         * on the flow thread, since only the flow's steps (not the background fetch's) can fail
         * before a dialog; walk the trace to it */
        char *line = f.trace;
        int seen = 0, n = 0;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (!strncmp(line, "dialog ", 7)) seen = 0;   /* a new decision of the user's: a new action */
            if (!strncmp(line, "step ", 5) && ++n == k) seen = check_failed;
            else if (seen && n >= k) {
                CHECKS(strncmp(line, "step MODEL_COPY", 15) && strncmp(line, "step DOWNLOAD", 13) && strncmp(line, "admin 0", 7) &&
                       strncmp(line, "admin 1", 7) && strncmp(line, "admin 2", 7) && strncmp(line, "admin 3", 7) && strncmp(line, "step REMOVE_", 12), ctx);
                if (!strcmp(failed_name, "REKORDBOX")) CHECKS(!strstr(line, "rekordbox is open"), ctx);   /* F11 */
            }
            line = nl ? nl + 1 : NULL;
        }
        if (!strcmp(failed_name, "MODELS_INFO"))   /* F14: an unknown result doesn't claim there's no saved copy */
            for (int d = 0; d < f.ndialogs && d < 32; d++) CHECKS(!strstr(f.dlg_msg[d], "There's no saved copy of rekordbox's own stems model to put back."), ctx);
    }
    if (getenv("FLOWS_PROGRESS")) { fputs(ctx, stderr); fputs("\n", stderr); }
    if (f.freed) {
        CHECKS(f.calls_after_free == 0 && atomic_load(&l->flows_out) == 0, ctx);
        /* F21: a late progress and a late admin note from a worker after the free change nothing */
        int note_before = atomic_load(&l->admin_note);
        pthread_t late;
        pthread_create(&late, NULL, late_calls, f.c);
        pthread_join(late, NULL);
        CHECKS(atomic_load(&l->admin_note) == note_before, ctx);
    } else {
        ended_clean(&f, ctx);
    }
    if (nsteps) *nsteps = f.nsteps;
    if (ndialogs) *ndialogs = f.ndialogs;
    if (ncalls) *ncalls = f.ncalls;
    if (atomic_load(&flows_test_fetch_hold)) {   /* held: only the fetch the free met may call late */
        CHECKS(f.late_calls <= f.fetch_hits, ctx);
        matrix_fetch_hits += f.fetch_hits;
    }
    ff_end(&f);
    life_release(l);
}

/* The cache offer's free matrix again with the background payload fetch held, by a handshake,
 * between core_run's liveness check and its step until the free comes: a core_free then lands
 * inside that window for certain (the window that made "cache offer, mode 3, at 22" fail now and
 * then under TSan). The fetch may then make its one step (a late call) or, a stricter core, none;
 * a second call, or one from the flow after the free, is still a fault (run_one). */
static void t_late_fetch(void) {
    const scenario *sc = NULL;
    for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; i++)
        if (!strcmp(scenarios[i].name, "cache offer")) sc = &scenarios[i];
    CHECK(sc != NULL);
    if (!sc) return;
    atomic_store(&flows_test_fetch_hold, fetch_hold);
    int nc = 0;
    run_one(sc, 0, 0, NULL, NULL, &nc);
    matrix_fetch_hits = 0;
    for (int k = 1; k <= nc; k++) run_one(sc, 3, k, NULL, NULL, NULL);
    atomic_store(&flows_test_fetch_hold, NULL);
    CHECK(nc > 0 && matrix_fetch_hits > 0);   /* frees met the held fetch: the window was tested */
}

static void t_matrix(void) {
    for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; i++) {
        int ns = 0, nd = 0, nc = 0;
        run_one(&scenarios[i], 0, 0, &ns, &nd, &nc);
        CHECKS(ns > 0 && nc > 0, scenarios[i].name);
        for (int k = 1; k <= ns; k++) run_one(&scenarios[i], 1, k, NULL, NULL, NULL);
        for (int k = 0; k < nd; k++) run_one(&scenarios[i], 2, k, NULL, NULL, NULL);
        for (int k = 1; k <= nc; k++) run_one(&scenarios[i], 3, k, NULL, NULL, NULL);
    }
}

/* F4: clicks during a flow that then asks or turns busy are dropped, as 1.3.0's modal session dropped them */
static void t_one_at_a_time(void) {
    ff f;
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    answers(&f, 3, "Cancel", "Cancel", "Cancel");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");   /* still in its preflight: nothing asked yet */
    CHECK(f.c->flow_running);
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}") == CORE_OK);
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"reinstall_plus\"}") == CORE_OK);
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "Install Demucs v4?"));
    CHECK(count_of(&f, "log core: a click while an action runs is ignored") == 2);
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"no_such_button\"}") == CORE_OK && f.ndialogs == 1);   /* not a flow's: M4's */
    CHECK(ev(&f, "{\"ev\":\"click\"}") == CORE_E_ARG);
    ended_clean(&f, "one at a time");
    ff_end(&f);
}

/* F4: a double click gives one action */
static void t_double_clicks(void) {
    ff f;
    ff_init(&f);
    answers(&f, 2, "Install", "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    settle(&f);
    CHECK(count_of(&f, "dialog Install Demucs v4?") == 1 && count_of(&f, "step MODEL_COPY") <= 1);
    ff_end(&f);
    ff_init(&f);
    answers(&f, 2, "Done", "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(count_of(&f, "step REPORT_ZIP") == 1);
    ff_end(&f);
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(count_of(&f, "dialog Settings [") == 1);
    ff_end(&f);
}

/* F4, F5: a click while a flow runs that ends with no dialog and no busy is replayed after it;
 * the clicks after it stay queued behind the replayed flow */
static void t_delayed_clicks(void) {
    ff f;
    ff_init(&f);
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    f.chosen_model = f.plus = 1;   /* nothing is missing: the request flow is silent */
    f.request_age = 5;
    ev(&f, "{\"ev\":\"activated\"}");
    CHECK(f.c->flow_running);
    ev(&f, "{\"ev\":\"menu\",\"id\":\"update\"}");          /* no newer app: silent too */
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}");
    settle(&f);
    CHECK(count_of(&f, "log core: a click while a flow runs is queued") == 2);
    CHECK(in_order(&f, 3, "log watcher asked to reinstall, but nothing is missing: ignored", "step PAYLOAD saved",
                   "dialog Uninstall Demucs v4? [Uninstall|Cancel] -> (none)"));
    CHECK(count_of(&f, "dialog Uninstall Demucs v4?") == 1);
    ff_end(&f);
}

/* F1: one snapshot is the source of the text, the guard and the action; a change while the
 * confirmation is up stops the action */
static void t_guard_snapshot(void) {
    ff f;
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    f.flip_chosen_at_rb = 1;   /* preflight's check: before the snapshot, so the text names the new choice */
    answers(&f, 1, "Reinstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"reinstall\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[0], "Reinstall Stems Cache?") && has(&f, "step BUSY_MARKER reinstall Reinstall Stems Cache"));
    CHECK(!has(&f, "step BLOCKED plus") && !has(&f, "log   changed while asking: stopped"));
    ff_end(&f);
    ff_init(&f);
    f.plus = f.chosen_model = 1;
    f.change_chosen_at = 0;   /* while the confirmation is up */
    answers(&f, 1, "Reinstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"reinstall\"}");
    settle(&f);
    CHECK(has(&f, "log   changed while asking: stopped") && !has(&f, "step DOWNLOAD model") && count_of(&f, "admin ") == 0);
    ff_end(&f);
}

/* F2, F6: the location confirmations are guarded; after an invalidation nothing more is shown */
static void t_guard_location(void) {
    ff f;
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_location = 1;
    f.set_old_drive = "GLS";
    f.row_answer[0] = 1;
    f.bump_status_gen_at = 1;   /* during "Use This Mac?" */
    answers(&f, 3, "", "Use This Mac", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(has(&f, "log   changed while asking: stopped") && !has(&f, "step LOCATION_WRITE    0") && f.ndialogs == 2);
    ff_end(&f);
    ff_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.set_clear = 1;
    f.bump_status_gen_at = 1;   /* during the Clear confirmation */
    answers(&f, 3, "Clear Saved Stems", "Clear", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(count_of(&f, "log   changed while asking: stopped") == 1 && !has(&f, "step CLEAR_THIS_MAC") && f.ndialogs == 2);
    ff_end(&f);
}

/* F7: the label from the drive and the folders below it */
static void t_location_label(void) {
    ff f;
    ff_init(&f);
    f.set_location = 1;
    f.set_old_drive = "GLS";
    f.set_label_path = "DJ/Stems";
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(has(&f, "label Stems Cache location: GLS › DJ › Stems"));
    ff_end(&f);
    ff_init(&f);
    f.set_location = 1;
    f.set_old_drive = "GLS";
    f.set_label_path = "";
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(has(&f, "label Stems Cache location: GLS"));
    ff_end(&f);
}

/* F3: the launch retry of This Mac's deletion */
static void t_this_mac_retry(void) {
    ff f;
    ff_init(&f);   /* (a) flag, available, bridge location, rekordbox closed */
    f.tm_flag = f.tm_available = f.tm_bridge_loc = 1;
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(in_order(&f, 5, "step THIS_MAC_DELETE facts", "step REKORDBOX", "step THIS_MAC_DELETE delete", "step THIS_MAC_DELETE unflag",
                   "log the stems left on this Mac deleted: ok"));
    ff_end(&f);
    ff_init(&f);   /* (b) the rekordbox check fails */
    f.tm_flag = f.tm_available = f.tm_bridge_loc = 1;
    f.fail_step_at = 5;   /* WATCHER_CHECK, MIGRATE_LEGACY, REMOVE_LEFTOVERS, THIS_MAC_DELETE facts, then REKORDBOX */
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(has(&f, "log   rekordbox check failed: stopped") && has(&f, "log the stems left on this Mac aren't deleted yet: not now (chosen)"));
    CHECK(!has(&f, "step THIS_MAC_DELETE delete"));
    ff_end(&f);
    ff_init(&f);   /* (c) no flag, but leftovers to clean */
    f.tm_available = f.tm_bridge_loc = f.tm_folders = 1;
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(in_order(&f, 4, "step THIS_MAC_DELETE defer", "log stems an older Stems Cache saved on this Mac while a drive was chosen: deleting them",
                   "step THIS_MAC_DELETE delete", "step THIS_MAC_DELETE unflag"));
    ff_end(&f);
    ff_init(&f);   /* (d) no flag, nothing to clean */
    f.tm_available = f.tm_bridge_loc = 1;
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(has(&f, "step THIS_MAC_DELETE facts") && count_of(&f, "step THIS_MAC_DELETE") == 1 && !has(&f, "step REKORDBOX"));
    ff_end(&f);
}

/* F9: a payload.json fetched while a flow runs: the offer waits for the flow's end */
static void pump_until_idle(ff *f) {
    for (int i = 0; i < 5000; i++) {
        if (!pump(f) && !f->c->flow_running) break;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
}
static void t_offer_after_flow(void) {
    for (int negative = 0; negative < 2; negative++) {
        ff f;
        ff_init(&f);
        f.cache = f.chosen_cache = 1;
        f.bridge_installed = "3333333333333333333333333333333333333333333333333333333333333333";
        f.payload_found = 0;    /* no saved payload.json at start-up: no offer of its own */
        f.payload_due = 1;      /* ... but a download is due: it waits in the fake */
        f.fetch_block = 1;
        ev(&f, "{\"ev\":\"shown\"}");
        pump_until_idle(&f);
        f.payload_found = 1;
        f.set_gb = "20";
        f.set_days = "30";
        f.hold_dialog_at = 0;   /* Settings stays up */
        ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
        for (int i = 0; i < 5000 && !f.dlg_held; i++) { pump(&f); struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
        pthread_mutex_lock(&f.mu);
        f.fetch_released = 1;
        pthread_cond_broadcast(&f.cv);
        pthread_mutex_unlock(&f.mu);
        for (int i = 0; i < 5000 && !f.c->fl.cache_offer_pending; i++) { pump(&f); struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
        CHECK(f.c->fl.cache_offer_pending == 1 && count_of(&f, "dialog Stems Cache update") == 0);
        if (negative) f.c->fl.cache_update_asked = 1;   /* asked meanwhile: no offer */
        f.hold_released = 1;    /* Settings is answered: Cancel */
        settle(&f);
        CHECK(f.c->fl.cache_offer_pending == 0);
        CHECK(count_of(&f, "dialog Stems Cache update") == (negative ? 0 : 1));
        ff_end(&f);
    }
}

/* OOM sweep: the next job allocation fails right after the background fetch's step. fetched_job
 * was reserved before the step, so the payload.json fetched still reaches main and its cache
 * offer is asked (lost, it waited for the next launch). Main isn't pumped meanwhile, so nothing
 * else allocates before the fetch has posted or used the failure. The failure is armed only once
 * the fetch worker is parked in its step, past its reservation (OF1: a late worker took it) */
static void t_fetch_reserved(void) {
    ff f;
    ff_init(&f);
    f.cache = f.chosen_cache = 1;
    f.bridge_installed = "3333333333333333333333333333333333333333333333333333333333333333";
    f.payload_found = 0;    /* no saved payload.json at start-up: no offer of its own */
    f.payload_due = 1;      /* a download is due: it waits in the fake */
    f.fetch_block = 1;
    ev(&f, "{\"ev\":\"shown\"}");
    pump_until_idle(&f);
    CHECK(!f.c->flow_running && count_of(&f, "dialog Stems Cache update") == 0);
    f.payload_found = 1;    /* the download brings one */
    int parked = 0;
    for (int i = 0; i < 5000 && !parked; i++) {
        pthread_mutex_lock(&f.mu);
        parked = f.fetch_parked;
        pthread_mutex_unlock(&f.mu);
        if (!parked) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    }
    CHECK(parked);
    CHECK(core_test_fail_alloc_after(f.c, 0, 1) == CORE_OK);
    pthread_mutex_lock(&f.mu);
    f.fetch_released = 1;
    pthread_cond_broadcast(&f.cv);
    pthread_mutex_unlock(&f.mu);
    int posted = 0;
    for (int i = 0; i < 5000 && !posted && atomic_load(&f.c->life->fail_after) >= 0; i++) {
        pthread_mutex_lock(&f.mu);
        posted = f.head != NULL;
        pthread_mutex_unlock(&f.mu);
        if (!posted) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    }
    core_test_fail_alloc_after(f.c, -1, 0);
    settle(&f);
    CHECK(count_of(&f, "dialog Stems Cache update") == 1);
    ff_end(&f);
}

#ifdef CORE_WIN_POLICY
static void flows_win_tests(void);
#endif

/* each test through a volatile pointer: never inlined, so its fake host (tens of KB, more under
 * ASan) is on its own frame, not summed into one caller's frame past the 8 MB stack */
#define RUN(t) do { if (getenv("FLOWS_PROGRESS")) fputs(#t "\n", stderr); void (*volatile t_)(void) = t; t_(); } while (0)

/* M72: the reminders line follows the watcher's state installWatcher and removeWatcher measure
 * afterwards (their updateWatcherState): off, then on again after a Reinstall; off, then cleared
 * once the last feature is removed (the LaunchAgent goes). Each change to off is logged once. */
static int reminders_shown(ff *f) {
    core_snap *s = core_snap_current(f->c);
    int on = s && strstr(core_str(s, F_STATUS_LINE, NULL), "Reinstall reminders are off") != NULL;
    core_snap_release(s);
    return on;
}
static void t_watcher_follows(void) {
    ff f;
    ff_init(&f);
    f.watcher_off = 1;
    answers(&f, 2, "Install", "Reinstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    settle(&f);
    CHECK(has(&f, "step LAUNCH_AGENT_LOAD") && reminders_shown(&f) && count_of(&f, "log watcher: installed but not running (turned off in Login Items, or not loaded): reinstall reminders are off") == 1);
    f.plus = 1;
    f.watcher_off = 0;
    ev(&f, "{\"ev\":\"click\",\"id\":\"reinstall_plus\"}");
    settle(&f);
    CHECK(count_of(&f, "step LAUNCH_AGENT_LOAD") == 2 && !reminders_shown(&f) && count_of(&f, "log watcher: installed but not running (turned off in Login Items, or not loaded): reinstall reminders are off") == 1);
    ended_clean(&f, "watcher off, then on after Reinstall");
    ff_end(&f);
    ff_init(&f);
    f.watcher_off = 1;
    answers(&f, 2, "Install", "Uninstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_plus\"}");
    settle(&f);
    CHECK(reminders_shown(&f));
    f.plus = 1;
    f.watcher_off = 0;   /* the LaunchAgent removed: nothing turned off any more */
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_plus\"}");
    settle(&f);
    CHECK(has(&f, "step LAUNCH_AGENT_UNLOAD") && !reminders_shown(&f) && count_of(&f, "log watcher: installed but not running (turned off in Login Items, or not loaded): reinstall reminders are off") == 1);
    ended_clean(&f, "watcher off, then the last feature removed");
    ff_end(&f);
}

/* ---------------------------------------------------------------- the watcher role (Watcher.swift's watcherCheck) */

static void w_run(ff *f) {
    CHECK(core_event(f->c, "{\"ev\":\"start\"}", CORE_NTS) == CORE_OK);
    settle(f);
}

static void t_watcher(void) {
    const char *run_off = "log run: RB Stems Plus Watcher 1.4.0; rekordbox 7.2.19.0342, engine 0002, Demucs v4 off, Stems Cache on; chosen: model=true cache=true";
    ff_role = CORE_ROLE_WATCHER;
    ff f;
    /* nothing went missing: the run line, nothing asked, the lock let go, the watcher done */
    ff_init(&f);
    CHECK(f.c != NULL);
    f.chosen_model = f.chosen_cache = f.plus = f.cache = 1;
    w_run(&f);
    CHECK(in_order(&f, 7, "step REKORDBOX", "step WATCH_BUSY", "step REKORDBOX", "step WATCH_LOCK clear_again", "step WATCH_FACTS",
                   "log nothing to ask", "step WATCH_LOCK release"));
    CHECK(has(&f, "log run: RB Stems Plus Watcher 1.4.0; rekordbox 7.2.19.0342, engine 0002, Demucs v4 on, Stems Cache on; chosen: model=true cache=true"));
    CHECK(in_order(&f, 2, "step WATCH_LOCK again", "close_reply WATCHER_DONE 1") && f.ndialogs == 0);
    ff_end(&f);

    /* Demucs v4 went missing, its engine listed: the question, Reinstall Now: the request, the app
     * opened after this process, no more rounds */
    ff_init(&f);
    f.chosen_model = f.chosen_cache = f.cache = 1;
    f.watch_engine_ok = f.watch_rb_ok = 1;
    f.watch_again_n = 1;
    answers(&f, 1, "Reinstall Now");
    w_run(&f);
    CHECK(has(&f, run_off) && has(&f, "log asking: model:0002"));
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "rekordbox put its own stems model back")
          && !strcmp(f.dlg_msg[0], "Demucs v4 is off. Reinstall it to separate tracks with Demucs v4 instead of rekordbox's own model.")
          && !strcmp(f.dlg_buttons[0], "Reinstall Now|Remind Me Later|Don't Ask Again for This Version"));
    CHECK(in_order(&f, 4, "log user chose: Reinstall Now", "step WATCH_REINSTALL", "step WATCH_LOCK release", "close_reply WATCHER_DONE 1"));
    CHECK(!has(&f, "step WATCH_LOCK again"));   /* the app takes over: no look at the again-mark */
    ff_end(&f);

    /* Don't Ask Again for This Version: the key added to dismissed's text, written whole */
    ff_init(&f);
    f.chosen_model = 1;
    f.watch_engine_ok = f.watch_rb_ok = 1;
    f.watch_dismissed = "cache:7.2.10\n";
    answers(&f, 1, "Don't Ask Again for This Version");
    w_run(&f);
    CHECK(!strcmp(f.watch_remembered, "cache:7.2.10\nmodel:0002\n"));
    ff_end(&f);

    /* Remind Me Later, then a start came in during the check: the second round doesn't ask again */
    ff_init(&f);
    f.chosen_model = 1;
    f.watch_engine_ok = f.watch_rb_ok = 1;
    f.watch_again_n = 1;
    answers(&f, 1, "Remind Me Later");
    w_run(&f);
    CHECK(f.ndialogs == 1 && in_order(&f, 4, "log user chose: Remind Me Later", "step WATCH_LOCK retake", "log something changed during the check: looking again",
                                      "log still missing (model:0002), put off a moment ago: not asking again now"));
    CHECK(has(&f, "close_reply WATCHER_DONE 1"));
    ff_end(&f);

    /* the engine isn't listed: told it can't be reinstalled yet; OK remembers the unsupported key */
    ff_init(&f);
    f.chosen_model = 1;
    f.watch_rb_ok = 1;
    answers(&f, 1, "OK");
    w_run(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_buttons[0], "OK|Don't Ask Again for This Version")
          && !strcmp(f.dlg_title[0], "rekordbox installed a new STEMS Engine"));
    CHECK(!strcmp(f.watch_remembered, "model:0002 unsupported with 7.2.19.0342,7.2.10,0001,1.18\n"));
    ff_end(&f);
    /* ... and once remembered, not asked again */
    ff_init(&f);
    f.chosen_model = 1;
    f.watch_rb_ok = 1;
    f.watch_dismissed = "model:0002 unsupported with 7.2.19.0342,7.2.10,0001,1.18\n";
    w_run(&f);
    CHECK(f.ndialogs == 0 && has(&f, "log nothing to ask"));
    ff_end(&f);

    /* RB Stems Plus busy past its 10 minutes: not asking; an uninstall left half-done: not asking */
    ff_init(&f);
    f.chosen_model = 1;
    f.watch_busy_n = 1000;
    w_run(&f);
    CHECK(has(&f, "log waited 600 s for an update or RB Stems Plus to finish") && has(&f, "log RB Stems Plus is still busy: not asking"));
    CHECK(!has(&f, "step WATCH_FACTS") && has(&f, "close_reply WATCHER_DONE 1"));
    ff_end(&f);
    ff_init(&f);
    f.chosen_model = 1;
    f.watch_marked = "uninstall-cache";
    w_run(&f);
    CHECK(has(&f, "log an uninstall didn't finish (uninstall-cache): not asking") && !has(&f, "step WATCH_FACTS"));
    ff_end(&f);

    /* rekordbox open: the prepared playlists' check of its session, waiting until it quits */
    ff_init(&f);
    f.chosen_model = f.plus = 1;
    f.rb_open_n = 4;   /* the first look (updating) counts one: open at the next three */
    f.rb_open_from = 1;
    w_run(&f);
    CHECK(in_order(&f, 4, "step WATCH_KEPT session", "log rekordbox is open: waiting until it quits", "step WATCH_PAUSE 10", "step WATCH_PAUSE 3"));
    CHECK(has(&f, "log nothing to ask") && !has(&f, "step WATCH_KEPT sets"));   /* no rekordbox session id: nothing checked */
    ff_end(&f);
    ff_role = CORE_ROLE_APP;
}

/* the prepared playlists' check as rekordbox opens (Watcher.swift's checkKeptSets until M7's switch-over) */
static const char *KEPT_CONFIG = "{\"version\":\"1.4.0\",\"os\":\"mac\",\"paths\":{\"home\":\"/Users/x\",\"cache_default\":\"/c\","
                                 "\"model\":\"/m/hdemucs.onnx\",\"root_ort\":\"/o/ort.dylib\",\"rekordbox\":\"/A/rekordbox.app\",\"kept\":\"/k\"}}";
static const char *KEPT_SETS = "42\t\t4\n77\t\t0";
static const char *KEPT_PLAYLISTS = "{\"playlist\":\"1\",\"parent\":\"root\",\"name\":\"Folder\",\"kind\":\"folder\",\"tracks\":0}\n"
                                    "{\"playlist\":\"42\",\"parent\":\"1\",\"name\":\"Friday\",\"kind\":\"playlist\",\"tracks\":4}\nnot json\n";
static const char *KEPT_NOT_READY = "{\"file\":\"/a.mp3\",\"result\":\"ready\"}\n{\"file\":\"/b.mp3\",\"result\":\"prepare\"}\n"
                                    "{\"file\":\"/c.mp3\",\"result\":\"skipped\",\"why\":\"gone\"}\n{\"file\":\"/d.mp3\",\"result\":\"unchecked\"}\n"
                                    "{\"estimate\":4,\"ready\":1,\"prepare\":1,\"skipped\":1,\"unchecked\":1}\n";
static const char *KEPT_READY = "{\"file\":\"/e.mp3\",\"result\":\"ready\"}\n{\"estimate\":1,\"ready\":1,\"prepare\":0,\"skipped\":0}\n";
static const char *KEPT_BASE = " --model /m/hdemucs.onnx --cache /c --max-gb 20 --kept-dir /k --ort /o/ort.dylib --rekordbox /A/rekordbox.app";

#define KW(f) ((f)->kept_written ? (f)->kept_written : "")

/* how many times s is in the trace (notify lines hold the notice's whole text: strstr, not has()) */
static int trace_count(const ff *f, const char *s) {
    int n = 0;
    for (const char *p = f->trace; (p = strstr(p, s)); p += strlen(s)) n++;
    return n;
}

/* an estimate of n tracks to prepare (long: long paths), and the noticed map covering them:
 * JSONEncoder's shape when slashes ("\/"), the core's otherwise; heap, the caller frees */
static char *kept_est(int n, int lng) {
    size_t cap = (size_t)n * 160 + 256, k = 0;
    char *b = malloc(cap);
    for (int i = 0; i < n; i++)
        k += (size_t)snprintf(b + k, cap - k, lng ? "{\"file\":\"/Users/dj/Music/A rather long folder name for this test/Sub folder two/track-%05d.flac\",\"result\":\"prepare\"}\n"
                                                : "{\"file\":\"/t%d\",\"result\":\"prepare\"}\n", i);
    snprintf(b + k, cap - k, "{\"estimate\":%d,\"ready\":0,\"prepare\":%d,\"skipped\":0}\n", n, n);
    return b;
}
static char *kept_map(int n, int lng, int slashes) {
    size_t cap = (size_t)n * 170 + 64, k = 0;
    char *b = malloc(cap);
    const char *sl = slashes ? "\\/" : "/";
    k += (size_t)snprintf(b, cap, "{\"42\":[");
    for (int i = 0; i < n; i++)
        k += lng ? (size_t)snprintf(b + k, cap - k, "%s\"%sUsers%sdj%sMusic%sA rather long folder name for this test%sSub folder two%strack-%05d.flac\"",
                                    i ? "," : "", sl, sl, sl, sl, sl, sl, i)
                 : (size_t)snprintf(b + k, cap - k, "%s\"%st%d\"", i ? "," : "", sl, i);
    snprintf(b + k, cap - k, "]}");
    return b;
}

static void kept_init(ff *f) {
    ff_init(f);
    f->chosen_model = f->plus = 1;
    f->rb_open_n = 2;   /* open at the settle's look: the session's check, then closed */
    f->rb_open_from = 1;
    f->kept_pid = "123";
    f->kept_sets = KEPT_SETS;
    f->kept_keeps = 1;
    f->kept_playlists = KEPT_PLAYLISTS;
    f->kept_est_playlist = KEPT_NOT_READY;
    f->kept_est_own = KEPT_READY;
}

static void t_kept(void) {
    ff_role = CORE_ROLE_WATCHER;
    ff_config = KEPT_CONFIG;
    ff f;
    char want[512];
    /* a new session: the session seen, each set checked (the playlist as it is now; one whose
     * playlist can't be read: the set's own files); one not ready: a notification, what it
     * covered remembered */
    kept_init(&f);
    f.kept_est_playlist = NULL;   /* 77's playlist can't be read ... */
    f.kept_err = "no playlist";
    w_run(&f);
    CHECK(!strcmp(f.kept_seen, "123"));
    /* every set's playlist unreadable here: each falls back to its own files (ready) */
    snprintf(want, sizeof want, "step WATCH_HELPER 60%s --quick --keep 42 --playlist 42", KEPT_BASE);
    CHECK(in_order(&f, 4, "step WATCH_KEPT seen 123", "step WATCH_KEPT sets", "step WATCH_KEPT facts", "step WATCH_HELPER 30 --playlists"));
    CHECK(has(&f, want));
    snprintf(want, sizeof want, "step WATCH_HELPER 60%s --quick --keep 42", KEPT_BASE);
    CHECK(has(&f, want));
    CHECK(has(&f, "log prepared playlist 42: Ready") && has(&f, "log prepared playlist 77: Ready"));
    CHECK(!has(&f, "step WATCH_KEPT notify") && !strcmp(KW(&f), "{}"));
    CHECK(has(&f, "close_reply WATCHER_DONE 1"));
    ff_end(&f);

    /* 42 has tracks not ready: its state logged, a notification with its playlist's name, the
     * tracks remembered; 77 (not in the library's playlists) ready */
    kept_init(&f);
    f.kept_sets = "42\t\t4";
    w_run(&f);
    CHECK(has(&f, "log prepared playlist 42: 1 track to prepare, 1 can't be read"));
    CHECK(has(&f, "step WATCH_KEPT notify \xe2\x80\x9c" "Friday\xe2\x80\x9d isn't fully prepared 2 tracks aren't fully prepared: rekordbox may separate them as you play. Open RB Stems Plus to see why and prepare it again."));
    CHECK(!strcmp(KW(&f), "{\"42\":[\"/b.mp3\",\"/c.mp3\"]}"));
    ff_end(&f);
    /* ... and a set not in the library's playlists is named by its id */
    kept_init(&f);
    f.kept_sets = "77\t\t4";
    w_run(&f);
    CHECK(has(&f, "step WATCH_KEPT notify \xe2\x80\x9cPlaylist 77\xe2\x80\x9d isn't fully prepared 2 tracks aren't fully prepared: rekordbox may separate them as you play. Open RB Stems Plus to see why and prepare it again."));
    ff_end(&f);

    /* the same tracks not ready as the last notice covered (as JSONEncoder wrote it): no
     * notification again; one more: a notification */
    kept_init(&f);
    f.kept_sets = "42\t\t4";
    f.kept_noticed = "{\"42\":[\"\\/b.mp3\",\"\\/c.mp3\",\"\\/x.mp3\"]}";
    w_run(&f);
    CHECK(has(&f, "log prepared playlist 42: 1 track to prepare, 1 can't be read") && !has(&f, "step WATCH_KEPT notify"));
    CHECK(!strcmp(KW(&f), "{\"42\":[\"/b.mp3\",\"/c.mp3\"]}"));
    ff_end(&f);
    kept_init(&f);
    f.kept_sets = "42\t\t4";
    f.kept_noticed = "{\"42\":[\"/b.mp3\"],\"9\":[\"/z.mp3\"]}";
    w_run(&f);
    CHECK(f.nsteps > 0 && strstr(f.trace, "step WATCH_KEPT notify ") != NULL);
    CHECK(!strcmp(KW(&f), "{\"42\":[\"/b.mp3\",\"/c.mp3\"]}"));   /* a set no longer kept: forgotten */
    ff_end(&f);
    /* a kept.noticed that isn't JSONEncoder's map: none, so the notification comes */
    kept_init(&f);
    f.kept_sets = "42\t\t4";
    f.kept_noticed = "{\"42\":\"/b.mp3\"}";
    w_run(&f);
    CHECK(strstr(f.trace, "step WATCH_KEPT notify ") != NULL);
    ff_end(&f);

    /* the set being prepared now: not checked, what the last notice covered kept */
    kept_init(&f);
    f.kept_sets = "42\t\t4";
    f.kept_running = "42";
    f.kept_noticed = "{\"42\":[\"/q.mp3\"]}";
    w_run(&f);
    CHECK(has(&f, "log prepared playlist 42: being prepared, not checked") && !has(&f, "step WATCH_KEPT notify"));
    CHECK(!strcmp(KW(&f), "{\"42\":[\"/q.mp3\"]}"));
    snprintf(want, sizeof want, "step WATCH_HELPER 60%s --quick --keep 42 --playlist 42", KEPT_BASE);
    CHECK(!has(&f, want));
    ff_end(&f);

    /* a set that can't be checked at all: said, its paths masked; nothing notified */
    kept_init(&f);
    f.kept_sets = "42\t\t4";
    f.kept_est_playlist = f.kept_est_own = NULL;
    f.kept_err = "can't read /Users/dj/Music/a.flac";
    w_run(&f);
    CHECK(has(&f, "log prepared playlist 42: couldn't be checked (can't read <path>)") && !has(&f, "step WATCH_KEPT notify"));
    ff_end(&f);

    /* preparing can't work now: said once, nothing run */
    kept_init(&f);
    f.kept_keeps = 0;
    w_run(&f);
    CHECK(has(&f, "log prepared playlists not checked: Reinstall Stems Cache: this installation can't keep prepared playlists."));
    CHECK(f.kept_helper_runs == 0 && !f.kept_written);
    ff_end(&f);

    /* nothing prepared: the session seen all the same, nothing else read */
    kept_init(&f);
    f.kept_sets = "";
    w_run(&f);
    CHECK(!strcmp(f.kept_seen, "123") && !has(&f, "step WATCH_KEPT facts") && f.kept_helper_runs == 0);
    ff_end(&f);

    /* the session already checked (kept.checked), or no rekordbox: nothing */
    kept_init(&f);
    f.kept_checked = "123";
    w_run(&f);
    CHECK(has(&f, "step WATCH_KEPT session") && !has(&f, "step WATCH_KEPT sets") && f.kept_seen[0] == 0);
    ff_end(&f);
    /* the in-process memo: kept.checked can't be written (it stays ""), rekordbox stays open over
     * several settle turns: the session is checked once (KS5) */
    kept_init(&f);
    f.kept_sets = "42\t\t4";
    f.rb_open_n = 5;
    w_run(&f);
    CHECK(trace_count(&f, "step WATCH_KEPT session") >= 3 && trace_count(&f, "step WATCH_KEPT sets") == 1);
    CHECK(f.kept_helper_runs == 2 && trace_count(&f, "step WATCH_KEPT notify ") == 1);
    ff_end(&f);

    /* big notices (KS1) and big histories (KS2): written whole, read back whole */
    struct { int n, lng; } big[] = {{300, 1}, {800, 1}, {257, 0}, {256, 0}};
    for (size_t i = 0; i < sizeof big / sizeof *big; i++) {
        char *est = kept_est(big[i].n, big[i].lng);
        kept_init(&f);
        f.kept_sets = "42\t\t4";
        f.kept_est_playlist = est;
        w_run(&f);
        CHECKS(trace_count(&f, "step WATCH_KEPT notify ") == 1 && f.kept_written && strlen(f.kept_written) > 2, "big notice: notified, written");
        char *mine = strdup(KW(&f)), *expect = kept_map(big[i].n, big[i].lng, 0);
        CHECKS(!strcmp(mine, expect), "big notice: the whole map written");
        ff_end(&f);
        /* ... read back: no notification, the same bytes again */
        kept_init(&f);
        f.kept_sets = "42\t\t4";
        f.kept_est_playlist = est;
        f.kept_noticed = mine;
        w_run(&f);
        CHECKS(trace_count(&f, "step WATCH_KEPT notify ") == 0 && !strcmp(KW(&f), mine), "big history: read whole, no notification, the same map");
        ff_end(&f);
        /* ... and as 1.3.0's JSONEncoder wrote it ("\/") */
        char *swift = kept_map(big[i].n, big[i].lng, 1);
        kept_init(&f);
        f.kept_sets = "42\t\t4";
        f.kept_est_playlist = est;
        f.kept_noticed = swift;
        w_run(&f);
        CHECKS(trace_count(&f, "step WATCH_KEPT notify ") == 0 && !strcmp(KW(&f), mine), "a 1.3.0 history: read whole, no notification");
        ff_end(&f);
        free(swift);
        free(mine);
        free(expect);
        free(est);
    }

    /* a long helper error, masked whole (KS3) */
    kept_init(&f);
    f.kept_sets = "42\t\t4";
    f.kept_est_playlist = f.kept_est_own = NULL;
    char *err = malloc(40001);
    memset(err, 'x', 40000);
    err[40000] = 0;
    memcpy(err, "can't read /Users/dj/a.flac; ", 29);
    f.kept_err = err;
    w_run(&f);
    CHECK(strstr(f.trace, "log prepared playlist 42: couldn't be checked (can't read <path>; xxxx") != NULL);
    ff_end(&f);
    free(err);
    ff_config = NULL;
    ff_role = CORE_ROLE_APP;
}

void flows_tests(int *passed, int *failed) {
    g_passed = passed;
    g_failed = failed;
    if (getenv("FLOWS_PROGRESS")) fputs("flows: start\n", stderr);
    RUN(t_pure);
    RUN(t_kept);
    RUN(t_install_plus);
    RUN(t_install_cache_admin);
    RUN(t_install_failures);
    RUN(t_preflight);
    RUN(t_uninstall_plus);
    RUN(t_watcher_follows);
    RUN(t_uninstall_cache);
    RUN(t_uninstall_all);
    RUN(t_uninstall_all_drive_named);
    RUN(t_request_updating);
    RUN(t_request_cache_only);
    RUN(t_uninstall_cache_gone);
    RUN(t_offer_matching);
    RUN(t_remove_anyway);
    RUN(t_startup);
    RUN(t_update_report);
    RUN(t_quit_keeps_busy);
    RUN(t_admin_busy_lost);
    RUN(t_update_stops_preparing);
    RUN(t_settings);
    RUN(t_watcher_prompt);
    RUN(t_one_at_a_time);
    RUN(t_double_clicks);
    RUN(t_delayed_clicks);
    RUN(t_guard_snapshot);
    RUN(t_guard_location);
    RUN(t_location_label);
    RUN(t_this_mac_retry);
    RUN(t_offer_after_flow);
    RUN(t_fetch_reserved);
    RUN(t_admin_args);
    RUN(t_rb_check_fails);
    RUN(t_changed_while_asking);
    RUN(t_activation_and_offer);
    RUN(t_startup_work);
    RUN(t_watcher);
#ifdef CORE_WIN_POLICY
    flows_win_tests();   /* the Windows flow variants on this fake host (W-core) */
#endif
    long live = core_debug_live_objects();
    CHECK(live == 0 || live == -1);   /* CORE_DEBUG: every counted object released; -1 otherwise */
    RUN(t_matrix);
    RUN(t_late_fetch);
    live = core_debug_live_objects();
    CHECK(live == 0 || live == -1);
#ifdef CORE_DEBUG
    CHECK(live == 0);
#endif
}

#ifdef CORE_WIN_POLICY
#include "flows_win_test.c"   /* the Windows flow variants' tests: this file's fake host */
#endif
