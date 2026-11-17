/* The action flows (mac-core §3.7, M6): today's Actions, LocationActions, Settings, Update, Report
 * and the start-up sheets, as flows on the flow thread. Each flow keeps 1.3.0's sequence, its
 * dialogs' order, texts and buttons, its status lines and its log lines; the platform work is
 * host steps (core.h, "The flows' steps"), the root runs are host->admin attempts, the dialogs are
 * host->dialog. Every text comes from texts.def through M5's composers (pure.h); the steps return
 * codes, numbers and raw facts. After every dialog the flow sends revalidate and waits for main's
 * answer: once the core is gone (core_free) the flow stops there, calling nothing more of the host,
 * and a confirmation whose state moved meanwhile (the status gen, chosen()) stops quietly.
 *
 * A flow never touches the core's state: it reads the cheap facts through a query that runs on
 * main (host->facts there), and changes the state shown (the status line, the spinner, the busy
 * state, the tab, the notice) through jobs it posts to main, in order. A flow that starts with
 * what was a main-thread dialog in 1.3.0 (a confirmation, preflight's alerts) doesn't turn the
 * busy state on: only start() and busy() do, as today, so the buttons look as they did.
 *
 * What stays M4's (marked "M4:"): the refresh's decisions behind the busy state (buttons, the
 * status line between actions), prepareChanged and the Prepare run's stop. */
#include "internal.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#define RB_BUNDLE_ID "com.pioneerdj.rekordboxdj"   /* Scripts.swift rbBundleID: root checks rekordbox's Info.plist says it */

/* ---------------------------------------------------------------- the flow's context */

typedef struct { const char *dir, *id; } gate;   /* a LocationGate: dir NULL for none */

enum { FX_END_UNWAIT = 0, FX_END_STARTUP = 1, FX_ENDS = 2 };   /* EF_REQUEST_WAITING 0, EF_STARTUP_DONE */
typedef struct fx {
    core_life *life;
    core_os os;
    arena a;                    /* the flow's own memory: texts, facts, answers */
    int gone;                   /* the core is gone: nothing more is asked of the host */
    int changed;                /* a confirmation's state moved while it was up: sticky for the rest of the
                                   flow by design (F6), so nothing more is shown; never reset */
    char own[32];               /* the app's own version */
    int guard_on;               /* revalidate also checks the state the confirmation was about */
    uint64_t guard_gen;         /* the status gen */
    int guard_model, guard_cache;   /* chosen() */
    int snapped;                /* a guard snapshot is pending for the next confirmation (F1) */
    const char *act_id, *act_label;   /* the action changing things (the busy marker), or NULL */
    int busy;                   /* busy(msg) is on */
    core_job *idle_job;         /* busy(nil)'s EF_IDLE, reserved by fx_busy so it can't be lost (AE3), or NULL */
    core_job *ends[FX_ENDS];    /* end effects reserved up front (FX_END_*), posted where they belong or at the flow's end */
#ifdef CORE_WIN_POLICY
    int win_restart;            /* Windows: an admin run said its last file goes at the next restart (I20) */
    int win_rb_closing;         /* Windows: the last rekordbox check still saw it closing (its window gone) */
    uint64_t win_deadline;      /* Windows: this flow's one deadline for rekordbox's linger (mono ns), 0 none yet */
#endif
} fx;

#ifdef CORE_WIN_POLICY
/* the Windows flow variants (W-core): flows_win.c, included at the end of this file */
static int32_t win_after_admin(fx *x, int32_t r, int *not_permitted);
static int win_rb_open(fx *x, int *updating);
static void win_offer_update_first(fx *x);
static int win_flows_event(struct core *c, const char *id, size_t n);
static void win_flow_ended(struct core *c);
static void win_restore_flow(fx *x);
static int win_rb_updating(fx *x, int *updating);
static const char *win_rb_open_text(fx *x, core_text_id id);
#endif

/* Windows: the checks that only read "updating" (or only whether rekordbox runs) never wait for a
 * closing rekordbox, and the "rekordbox open" texts say when it is still closing (review C12, C14).
 * On the Mac they are the plain calls. */
#ifdef CORE_WIN_POLICY
#define RB_UPDATING(x, up) win_rb_updating((x), (up))
#define RB_OPEN_TX(x, id) win_rb_open_text((x), (id))
#else
#define RB_UPDATING(x, up) rb_open((x), (up))
#define RB_OPEN_TX(x, id) TX((x), (id))
#endif

static int fx_ok(fx *x) {
    if (!x->gone && !life_alive(x->life)) x->gone = 1;
    return !x->gone;
}

static char *dup_s(fx *x, const char *s) {
    char *p = arena_strdup(&x->a, s ? s : "", s ? strlen(s) : 0);
    return p ? p : (char *)"";
}

/* the pieces joined, up to a NULL (log lines) */
static const char *cat(fx *x, ...) {
    va_list ap;
    size_t n = 0;
    va_start(ap, x);
    for (const char *p; (p = va_arg(ap, const char *));) n += strlen(p);
    va_end(ap);
    char *b = arena_alloc(&x->a, n + 1), *o = b;
    if (!b) return "";
    va_start(ap, x);
    for (const char *p; (p = va_arg(ap, const char *));) {
        size_t k = strlen(p);
        memcpy(o, p, k);
        o += k;
    }
    va_end(ap);
    *o = 0;
    return b;
}
#define CAT(x, ...) cat((x), __VA_ARGS__, (const char *)NULL)

static const char *num(fx *x, int64_t v) {
    char b[32];
    snprintf(b, sizeof b, "%lld", (long long)v);   /* a fixed format, a number */
    return dup_s(x, b);
}

/* a text built by M5's builder goes into the flow's memory ("" on an error: a bug, CORE_DEBUG aborts) */
/* the composing buffer is the heap's, not the flow thread's stack (a GCD thread's is 512 KB) */
enum { COMPOSE_CAP = 16384 };
#define COMPOSE(x, out, ...) do { \
    char *cb_ = malloc(COMPOSE_CAP); tbuf tb_; \
    tb_init(&tb_, (x)->os, cb_, cb_ ? COMPOSE_CAP : 0); \
    if (!cb_) tb_.err = CORE_E_NOMEM; \
    { tbuf *B = &tb_; __VA_ARGS__; } \
    (out) = cb_ && !tb_.err ? dup_s((x), cb_) : ""; \
    CORE_CHECK(!tb_.err || tb_.err == CORE_E_NOMEM); \
    free(cb_); \
} while (0)

/* measured, then filled into the flow's arena (no fixed cap); NULL when it can't be built */
#define COMPOSE_FIT(x, out, ...) do { tbuf tb_; tb_init(&tb_, (x)->os, NULL, 0); \
    { tbuf *B = &tb_; __VA_ARGS__; } size_t n_ = tb_.len; char *b_ = arena_alloc(&(x)->a, n_ + 1); \
    (out) = NULL; if (b_) { tb_init(&tb_, (x)->os, b_, n_ + 1); { tbuf *B = &tb_; __VA_ARGS__; } \
    if (!tb_.err) (out) = b_; } } while (0)

/* texts.def's text with its slots, measured first (no fixed buffer) */
static const char *tx_(fx *x, core_text_id id, const core_slot *s, int32_t n) {
    size_t len = 0;
    int32_t r = core_text(x->os, id, s, n, NULL, 0, &len);
    CORE_CHECK(r == CORE_OK || r == CORE_E_SPACE);
    if (r != CORE_OK && r != CORE_E_SPACE) return "";
    char *b = arena_alloc(&x->a, len + 1);
    if (!b || core_text(x->os, id, s, n, b, len + 1, &len) != CORE_OK) return "";
    return b;
}
#define SL(name, v) SLOT_S(name, (v) ? (v) : "")
/* a text that may name rekordbox's folder: its {dir} on Windows (w_rb_dir_slot) */
static const char *tx_rb(fx *x, core_text_id id) {
    char d[CORE_WIN_PROGRAMS_MAX + 16];
    core_slot sl;
    int32_t k = w_rb_dir_slot(x->os, id, CORE_PROGRAMS(&x->life->c), d, sizeof d, &sl);
    return tx_(x, id, k ? &sl : NULL, k);
}
#define TX(x, id) tx_((x), (id), NULL, 0)
#define TX1(x, id, s1) tx_((x), (id), (const core_slot[]){s1}, 1)
#define TX2(x, id, s1, s2) tx_((x), (id), (const core_slot[]){s1, s2}, 2)

static void lg(fx *x, const char *line) {
    if (fx_ok(x)) core_log(x->life, line);
}
#define LOG(x, ...) lg((x), CAT((x), __VA_ARGS__))

/* ---------------------------------------------------------------- effects: jobs posted to main, in order */

enum { EF_STATUS, EF_BUSY, EF_IDLE, EF_SPINNER, EF_SPINNER_IF_BUSY, EF_PERMISSION, EF_UPDATE_FOUND, EF_ADMIN_STATUS,
       EF_DL_WHAT, EF_DELETE_TEXTS, EF_PREPARE_CHANGED, EF_ACTION_MARK, EF_STARTUP_DONE, EF_REQUEST_WAITING,
       EF_CACHE_ASKED, EF_REFRESH, EF_MODEL_NOTICE, EF_WATCHER_OFF, EF_REMEASURE };

typedef struct { int kind; int64_t n; int32_t t1, t2; char s[]; } effect_msg;

static void set_owned(char **slot, const char *s) {
    free(*slot);
    *slot = s ? strdup(s) : NULL;
}

/* the refresh behind the busy state and between actions (Controller.refresh, M4's view.c) */
static void hook_refresh(core *c) { view_refresh(c); }
/* prepareChanged(quiet:): the Prepare and Status tabs check again (M4's view.c) */
static void hook_prepare_changed(core *c, int quiet) { view_prepare_changed(c, quiet); }

/* busy(nil) on main: the buttons back, and what the action changed checked (view.c). marked: the
 * action's mark travels with its EF_IDLE too, so a lost EF_ACTION_MARK still checks again (CF4) */
static void busy_off(core *c, int marked) {
    flows_state *fl = &c->fl;
    marked = marked || fl->action_marked;
    c->action_running = 0;
    fl->action_marked = 0;
    fl->busy_shown = 0;
    model_set_str(&c->m, F_STATUS_LINE, "", 0);
    model_set_i64(&c->m, F_SPINNER, 0);
    view_busy_off(c, marked);
}

void flows_flow_end_busy(core *c) {
    int quits = atomic_exchange(&c->life->flow_quits, 0);
    int marked = atomic_exchange(&c->life->flow_marked, 0);   /* OM3: an action stopped while busy */
    if (c->fl.startup_running) {   /* OM1: the start-up flow ended, however it went */
        c->fl.startup_running = 0;
        c->fl.startup_done = 1;
    }
    if ((c->fl.busy_shown || marked) && !quits) busy_off(c, marked);
}

static void effect_job(core *c, void *payload) {
    effect_msg *m = payload;
    flows_state *fl = &c->fl;
    model *md = &c->m;
    switch (m->kind) {
    case EF_STATUS: model_set_str(md, F_STATUS_LINE, m->s, strlen(m->s)); break;
    case EF_BUSY:   /* busy(msg) and busyInFront(msg) (Controller.swift:290-314) */
        if (m->n) view_show_tab(c, CORE_TAB_MANAGE);
        c->action_running = 1;
        model_set_str(md, F_STATUS_LINE, m->s, strlen(m->s));
        model_set_i64(md, F_SPINNER, 1);
        fl->busy_shown = 1;
        view_busy_on(c);   /* every window button off */
        prep_update(c);    /* busy's update of the Prepare tab: Prepare is off during an action */
        break;
    case EF_IDLE: busy_off(c, (int)m->n); break;   /* busy(nil) */
    case EF_SPINNER: model_set_i64(md, F_SPINNER, m->n); break;
    case EF_SPINNER_IF_BUSY: model_set_i64(md, F_SPINNER, c->action_running); break;
    case EF_PERMISSION:   /* "still busy: the marker stays" (Admin.swift:157) */
        model_set_i64(md, F_SPINNER, 0);
        model_set_str(md, F_STATUS_LINE, "", 0);
        break;
    case EF_UPDATE_FOUND:   /* updateFound (Update.swift:115-121): the notice and the menu item */
        if (m->s[0] && !fl->update_version) {
            const char *pre = "RB Stems Plus ", *post = " is available";
            size_t n = strlen(pre) + strlen(m->s) + strlen(post) + 1;
            char *line = malloc(n);
            if (line) {
                snprintf(line, n, "%s%s%s", pre, m->s, post);   /* a fixed format */
                core_log(c->life, line);
                free(line);
            }
        }
        set_owned(&fl->update_version, m->s[0] ? m->s : NULL);
        model_set_i64(md, F_UPDATE_VISIBLE, m->s[0] != 0);
        hook_refresh(c);   /* M4: the notice line (T_UPDATE_NOTICE) */
        break;
    case EF_ADMIN_STATUS: set_owned(&fl->admin_status, m->s); break;
    case EF_DL_WHAT: set_owned(&fl->dl_what, m->s); break;
    case EF_DELETE_TEXTS:
        fl->del_text = m->t1;
        fl->del_text_n = m->t2;
        set_owned(&fl->del_drive, m->s);
        break;
    case EF_PREPARE_CHANGED: hook_prepare_changed(c, (int)m->n); break;
    case EF_ACTION_MARK: fl->action_marked = 1; break;
    case EF_STARTUP_DONE: fl->startup_done = 1; break;
    case EF_REQUEST_WAITING: fl->request_waiting = (int)m->n; break;
    case EF_CACHE_ASKED: fl->cache_update_asked = 1; break;
    case EF_REFRESH: hook_refresh(c); break;
    case EF_MODEL_NOTICE: fl->model_notice = 1; hook_refresh(c); break;   /* T_NOTICE_NO_SAVED_MODEL in the status line */
    case EF_WATCHER_OFF: view_watcher_off(c, (int)m->n); break;            /* updateWatcherState's main half */
    case EF_REMEASURE: if (c->v.loc.known) view_location_remeasure(c); break;   /* invalidateSize, update(size: true) */
    default: break;
    }
    core_mark_changed(c);
}

static void fx_effect3(fx *x, int kind, int64_t n, const char *s, int32_t t1, int32_t t2) {
    if (!fx_ok(x)) return;
    size_t k = s ? strlen(s) : 0;
    effect_msg *m = malloc(sizeof *m + k + 1);
    if (!m) return;   /* a lost status update, not a state: the flow's end still clears the busy state */
    m->kind = kind;
    m->n = n;
    m->t1 = t1;
    m->t2 = t2;
    memcpy(m->s, s ? s : "", k + 1);
    core_post_main(x->life, effect_job, m, NULL, 0, 0);
}
static void fx_effect(fx *x, int kind, int64_t n, const char *s) { fx_effect3(x, kind, n, s, -1, -1); }
static void fx_status(fx *x, const char *s) { fx_effect(x, EF_STATUS, 0, s); }

/* ---------------------------------------------------------------- queries: run on main, the flow waits */

static int q_facts(core *c, void *arg) {
    CALLBACK(c, c->life->host.facts(c->life->host.ctx, (core_facts *)arg));
    view_loc_overlay(c, arg);   /* the location as the core's memo has it (M4) */
    return 1;
}

/* the core's location memo now (M4): known 0 when the host keeps its own snapshot; a lost query
 * (out of memory) stops the flow, as fx_facts', never the host's path (MR1) */
static void fx_loc(fx *x, view_loc_now *n) {
    memset(n, 0, sizeof *n);
    n->drive = n->in_use = "";
    view_loc_q q = {n, &x->a};   /* the memo's strings copied into the flow's arena, whole (MR11) */
    if (fx_ok(x) && !flow_query(x->life, view_loc_query, &q, 0)) {
        n->known = 0;
        x->gone = 1;   /* lost (out of memory): the flow stops, as fx_facts */
    }
}

/* a step's StemsSize kind 4 is "the core's size" (the memo's), M4 */
static int64_t size_kind_of(fx *x, const core_facts *f, core_fact kind_f, core_fact bytes_f, int64_t *bytes, const view_loc_now *memo);

/* the cheap facts now, read on main as refresh reads them */
static int fx_facts(fx *x, core_facts *f) {
    facts_init(f, &x->a);
    f->parts = CORE_FACTS_REFRESH;   /* the flows read refresh's rows only (M71) */
    if (!fx_ok(x)) return 0;
    if (!flow_query(x->life, q_facts, f, 0)) {
        x->gone = 1;
        return 0;
    }
    return 1;
}

static int fact_flag(const core_facts *f, core_fact id) { return f->set[id] && f->i[id] != 0; }
static int64_t fact_i64(const core_facts *f, core_fact id, int64_t none) { return f->set[id] ? f->i[id] : none; }
static const char *fact_str(const core_facts *f, core_fact id) { return f->set[id] && f->s[id] ? f->s[id] : ""; }
static const char *fact_str_or_null(const core_facts *f, core_fact id) { return f->set[id] && f->s[id] ? f->s[id] : NULL; }

/* revalidate: the core is there, and, while a confirmation is up, its state still holds */
static int q_reval(core *c, void *arg) {
    fx *x = arg;
    if (!x->guard_on) return 1;
    if (c->gen[DOM_STATUS] != x->guard_gen) return 0;
    core_facts f;
    facts_init(&f, &c->scratch);
    f.parts = CORE_FACTS_REFRESH;
    CALLBACK(c, c->life->host.facts(c->life->host.ctx, &f));
    return fact_flag(&f, FACT_CHOSEN_MODEL) == x->guard_model && fact_flag(&f, FACT_CHOSEN_CACHE) == x->guard_cache;
}

static int q_status_gen(core *c, void *arg) {
    *(uint64_t *)arg = c->gen[DOM_STATUS];
    return 1;
}

/* after every dialog: 1 when the state the dialog was about still holds; 0 stops the flow */
static int fx_revalidate(fx *x) {
    if (!fx_ok(x)) return 0;
    if (flow_query(x->life, q_reval, x, -1) != 1) {
        if (!life_alive(x->life)) x->gone = 1;   /* the core is gone */
        else x->changed = 1;                      /* the state moved: the flow stops, the core re-checks */
        return 0;
    }
    return 1;
}

typedef struct { int startup_done, request_waiting, cache_asked, action_running, flow_preparing, modal; } main_flags;
static int q_flags(core *c, void *arg) {
    main_flags *f = arg;
    f->startup_done = c->fl.startup_done;
    f->request_waiting = c->fl.request_waiting;
    f->cache_asked = c->fl.cache_update_asked;
    f->action_running = c->action_running;
    f->flow_preparing = c->preparing;
    if (!c->preparing && !c->fl.prep_reserved) {   /* SO1: in the query that read it, so no Prepare starts before the action */
        c->fl.prep_reserved = 1;
        prep_update(c);
    }
    int open;
    CALLBACK(c, open = c->life->host.modal_open(c->life->host.ctx));
    f->modal = open != 0;
    return 1;
}

/* a run the test hooks made (core_test_set_preparing) has no helper: it ends here; a real run is
 * stopped and waited for by flow_stop_and_wait */
static int q_stop_preparing(core *c, void *arg) {
    (void)arg;
    if (!prep_running(c)) c->preparing = c->paused = 0;
    c->fl.prep_reserved = 1;   /* SO1: nothing starts again until the flow that stopped it ends */
    if (!prep_running(c)) prep_update(c);   /* a run still stopping keeps Prepare off; its end updates */
    core_mark_changed(c);
    return 1;
}

/* ---------------------------------------------------------------- host calls */

static int32_t fx_step_v(fx *x, core_step k, core_facts *out, int n, va_list ap) {
    facts_init(out, &x->a);
    const char *args[8];
    for (int i = 0; i < n && i < 8; i++) args[i] = va_arg(ap, const char *);
    if (!fx_ok(x)) return -1;
    if (!x->life->host.step || (x->life->native & NATIVE_STEP)) return CORE_E_STATE;   /* no step entry: fails cleanly */
    return x->life->host.step(x->life->host.ctx, k, args, n, out);
}

static int32_t fx_step(fx *x, core_step k, core_facts *out, int n, ...) {
    va_list ap;
    va_start(ap, n);
    int32_t r = fx_step_v(x, k, out, n, ap);
    va_end(ap);
    return r;
}

/* a step whose results aren't read */
static int32_t fx_do(fx *x, core_step k, int n, ...) {
    core_facts out;
    va_list ap;
    va_start(ap, n);
    int32_t r = fx_step_v(x, k, &out, n, ap);
    va_end(ap);
    return r;
}

/* installWatcher / removeWatcher with their updateWatcherState (M72): the reminders' state the
 * step measured afterwards, when it says one */
static void fx_watcher_step(fx *x, core_step k) {
    core_facts w;
    if (fx_step(x, k, &w, 0) == 0 && w.set[FACT_STEP_FLAG]) fx_effect(x, EF_WATCHER_OFF, fact_flag(&w, FACT_STEP_FLAG), "");
}

/* ---------------------------------------------------------------- dialogs */

static int is_escape(fx *x, const char *t) {   /* Sheet.swift:91: these buttons answer Esc */
    return !strcmp(t, TX(x, T_CANCEL)) || !strcmp(t, TX(x, T_W_REMIND_LATER)) || !strcmp(t, TX(x, T_NOT_NOW));
}

static void dlg_init(core_dialog *d, core_text_id id, const char *title, const char *msg, int inline_) {
    memset(d, 0, sizeof *d);
    d->id = id;
    d->title = title;
    d->message = msg ? msg : "";
    d->def = 0;
    d->cancel = -1;
    d->inline_ = inline_;
}

static void dlg_button(fx *x, core_dialog *d, const char *t) {
    if (d->nbuttons >= DLG_MAX_BUTTONS) return;
    int i = d->nbuttons++;
    d->buttons[i] = t;
    if (i > 0 && d->cancel < 0 && is_escape(x, t)) d->cancel = i;
    if (d->inline_ && !strcmp(t, TX(x, T_W_DONT_ASK))) d->left_buttons |= 1u << i;
}

/* shows d; the reply in r; then revalidate. 0 answered; -1 not (Cancel, shutdown, gone, changed) */
static int fx_present(fx *x, const core_dialog *d, core_reply *r) {
    reply_init(r, &x->a);
    if (x->changed || !fx_ok(x) || !x->life->host.dialog) return -1;
    atomic_store(&x->life->asked, 1);   /* F4: clicks behind this flow are dropped now, as 1.3.0's modal session dropped them */
    int32_t rc = x->life->host.dialog(x->life->host.ctx, d, r);
    if (!fx_revalidate(x)) return -1;
    return rc == 0 ? 0 : -1;
}

/* a dialog with buttons only: the index of the button pressed, or -1 */
static int ask_d(fx *x, core_dialog *d) {
    core_reply r;
    if (fx_present(x, d, &r) || r.button < 0 || r.button >= d->nbuttons) return -1;
    return r.button;
}

static int ask_t(fx *x, core_text_id id, const char *title, const char *msg, int nb, ...) {
    core_dialog d;
    dlg_init(&d, id, title, msg, 0);
    va_list ap;
    va_start(ap, nb);
    for (int i = 0; i < nb; i++) dlg_button(x, &d, va_arg(ap, const char *));
    va_end(ap);
    return ask_d(x, &d);
}
#define ASK(x, id, msg, ...) ask_t((x), (id), TX((x), (id)), (msg), __VA_ARGS__)

static void alert_ok(fx *x, core_text_id id, const char *msg) { (void)ASK(x, id, msg, 1, TX(x, T_OK)); }

/* the button's title as the log writes "user chose: …" ("" when none was pressed) */
static const char *chose(const core_dialog *d, int b) { return b >= 0 && b < d->nbuttons ? d->buttons[b] : ""; }

/* Q6: a confirmation before an action is about the state it was asked on (the status gen,
 * chosen()). F1: one snapshot is the source of the text, the guard and the action: guard_snap
 * reads the gen, then the cheap facts (into *out when given), and keeps them; guard_arm makes
 * revalidate compare against them. If they moved while the dialog was up, the action stops
 * quietly and the core re-checks. */
static int guard_snap(fx *x, core_facts *out) {
    core_facts mine, *f = out ? out : &mine;
    uint64_t gen = 0;
    if (!fx_ok(x) || !flow_query(x->life, q_status_gen, &gen, 0) || !fx_facts(x, f)) return 0;
    x->guard_on = 0;
    x->guard_gen = gen;
    x->guard_model = fact_flag(f, FACT_CHOSEN_MODEL);
    x->guard_cache = fact_flag(f, FACT_CHOSEN_CACHE);
    x->snapped = 1;
    return 1;
}

static void guard_arm(fx *x) { x->guard_on = 1; }

static int disarm_guard(fx *x, int answer) {
    x->guard_on = 0;
    x->snapped = 0;
    if (x->changed && fx_ok(x)) {
        fx_do(x, CORE_STEP_BUSY_MARKER, 0);
        fx_status(x, "");
        LOG(x, "  changed while asking: stopped");
        return -1;
    }
    return answer;
}

/* confirm(action, verb, info): "<action>?" [verb] [Cancel] (Controller.swift:343) */
static int confirm(fx *x, const char *label, const char *verb, const char *info) {
    if (!x->snapped && !guard_snap(x, NULL)) return 0;
    guard_arm(x);
    int b = ask_t(x, T_CONFIRM, TX1(x, T_CONFIRM, SL(action, label)), info, 2, verb, TX(x, T_CANCEL));
    return disarm_guard(x, b) == 0;
}

/* ---------------------------------------------------------------- busy state */

/* an effect job reserved now and posted later, so it can't be lost to an allocation then; NULL
 * when the reservation itself fails (the post then falls back to fx_effect) */
static core_job *fx_reserve(fx *x, int kind, int64_t n) {
    if (!fx_ok(x)) return NULL;
    effect_msg *m = calloc(1, sizeof *m + 1);
    if (!m) return NULL;
    m->kind = kind;
    m->n = n;
    m->t1 = m->t2 = -1;
    core_job *j = job_reserve(x->life, effect_job, m, NULL);
    if (!j) free(m);
    return j;
}
static void fx_post_reserved(fx *x, core_job **slot, int kind, int64_t n) {
    core_job *j = *slot;
    *slot = NULL;
    if (!j) {   /* no reservation: a new post, also for a flow stopped by a lost query */
        int gone = x->gone;
        x->gone = 0;
        fx_effect(x, kind, n, "");
        x->gone = gone;
    } else if (life_alive(x->life)) job_post(x->life, j);   /* not fx_ok: a flow stopped by a lost query still ends cleanly */
    else core_job_drop(j);
}

/* busy(msg) / busyInFront(msg): the marker names the action (none: removed), then main shows it */
static void fx_busy(fx *x, const char *msg, int front) {
    atomic_store(&x->life->asked, 1);   /* F4: busy on: the buttons are off, so clicks behind it are dropped */
    if (x->act_id) fx_do(x, CORE_STEP_BUSY_MARKER, 2, x->act_id, x->act_label);
    else fx_do(x, CORE_STEP_BUSY_MARKER, 0);
    /* AE3: busy(nil)'s EF_IDLE reserved now, so running out of memory later can't leave the
     * spinner, the status line and the buttons busy (fx_idle falls back to a new one). It carries
     * the action's mark (CF4); when even this fails, the flow's end turns busy off (CF2) */
    if (!x->idle_job) x->idle_job = fx_reserve(x, EF_IDLE, x->act_id != NULL);
    fx_effect(x, EF_BUSY, front, msg);
    x->busy = 1;
}

/* busy(nil) */
static void fx_idle(fx *x) {
    int act = x->act_id != NULL;
    x->act_id = x->act_label = NULL;
    fx_do(x, CORE_STEP_BUSY_MARKER, 0);
    fx_post_reserved(x, &x->idle_job, EF_IDLE, act);
    x->busy = 0;
}

/* a flow's end (OOM sweep): a reserved end effect it never posted (the request no longer waiting,
 * the start-up done) is posted now. A flow stopped while busy (a query or a post lost to an
 * allocation) doesn't post busy(nil) here: the flow's end on main runs it (flows_flow_end_busy),
 * unless the flow ended with the app quitting */
static void fx_release(fx *x) {
    /* OM3: an action stopped while busy: its end checks again (its mark went with the idle job) */
    if (x->busy && x->act_id) atomic_store(&x->life->flow_marked, 1);
    core_job_drop(x->idle_job);
    x->idle_job = NULL;
    for (int i = 0; i < FX_ENDS; i++) {
        if (x->ends[i]) {
            if (life_alive(x->life)) job_post(x->life, x->ends[i]);
            else core_job_drop(x->ends[i]);
            x->ends[i] = NULL;
        }
    }
    arena_free(&x->a);
}

/* start(id, label, status): the busy marker names the action, and the Manage tab shows it */
static void fx_start(fx *x, const char *id, const char *label, core_text_id status) {
    x->act_id = id;
    x->act_label = label;
    fx_effect(x, EF_ACTION_MARK, 0, "");
    fx_busy(x, TX(x, status), 1);
}

/* inBackground(status) (LocationActions.swift:65): busy while the step runs, unless already busy */
static int bg_begin_s(fx *x, const char *status) {
    int was = x->busy;
    if (!was) fx_busy(x, status, 0);
    return was;
}
static int bg_begin(fx *x, core_text_id status) { return bg_begin_s(x, TX(x, status)); }
static void bg_end(fx *x, int was) {
    if (!was) fx_idle(x);
}

/* ---------------------------------------------------------------- pure decisions */

int flows_is_sha256(const char *s) {
    if (!s || strlen(s) != 64) return 0;
    for (; *s; s++) if (!strchr("0123456789abcdef", *s)) return 0;
    return 1;
}

/* String.compare(_, options: .numeric): digit runs compare by value */
static int numeric_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0' && isdigit((unsigned char)a[1])) a++;
            while (*b == '0' && isdigit((unsigned char)b[1])) b++;
            size_t na = 0, nb = 0;
            while (isdigit((unsigned char)a[na])) na++;
            while (isdigit((unsigned char)b[nb])) nb++;
            if (na != nb) return na < nb ? -1 : 1;
            int d = memcmp(a, b, na);
            if (d) return d < 0 ? -1 : 1;
            a += na;
            b += nb;
        } else if (*a != *b) {
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        } else {
            a++;
            b++;
        }
    }
    return *a ? 1 : *b ? -1 : 0;
}

int flows_version_newer(const char *a, const char *b) { return a && b && numeric_cmp(a, b) > 0; }

/* cacheFromSavedPayload (Actions.swift:689) */
int flows_cache_from_saved_payload(int unverified, int have_age, int64_t age, int bridge_verified) {
    if (unverified || !bridge_verified || !have_age) return 0;
    return age >= 0 && age <= 7 * 86400;
}

/* spaceShort's figure: (need + spare) / GiB, rounded up to 0.1, as "%.1f" */
static const char *space_gb(fx *x, int64_t bytes) {
    char g[64];
    fmt_fixed(ceil((double)bytes / 1073741824.0 * 10) / 10, 1, g);
    return dup_s(x, g);
}

/* the saved stems' size text (StemsSize.text, "" none) */
static const char *stems_size(fx *x, int64_t kind, int64_t bytes) {
    const char *out = "";
    if (kind >= 0) COMPOSE(x, out, w_stems_size(B, (int32_t)kind, bytes));
    return out;
}

/* Models.swift's message for a step's outcome (M5's T_MODEL_*), with the OS error where it has one */
static const char *model_message(fx *x, int64_t code, const char *why, int64_t fallback) {
    static const core_text_id ids[] = {0, T_MODEL_UNKNOWN, T_MODEL_UNREADABLE, T_MODEL_NOT_A_FILE, T_MODEL_NO_SPACE_SAVE, T_MODEL_NOT_SAVED,
        T_MODEL_NO_SPACE_COPY, T_MODEL_NOT_COPIED_WHY, T_MODEL_COPY_BAD, T_MODEL_CHANGED, T_MODEL_NOT_PLACED_WHY, T_MODEL_NO_SAVED_COPY,
        T_MODEL_NO_SPACE_BACK, T_MODEL_NOT_BACK, T_MODEL_NOT_BACK_WHY, T_MODEL_NOT_VERIFIED, T_MODEL_SAVED_DAMAGED};
    if (code < 1 || code >= (int64_t)(sizeof ids / sizeof *ids)) code = fallback;   /* an outcome the core doesn't know */
    int with_why = code == CORE_MODEL_NOT_COPIED_WHY || code == CORE_MODEL_NOT_PLACED_WHY || code == CORE_MODEL_NOT_BACK_WHY;
    return with_why ? TX1(x, ids[code], SL(why, why)) : TX(x, ids[code]);
}

/* modelLeftBehind (Models.swift:285) from MODELS_INFO's code */
static const char *model_left_behind(fx *x, int64_t code) {
    return TX(x, code == 0 ? T_MODEL_LEFT_NONE : code == 1 ? T_MODEL_UNREADABLE : code == 2 ? T_MODEL_LEFT_OURS : T_MODEL_LEFT_UNKNOWN);
}

/* REMOVE_LOCATION's reason, as removeInLocation words it */
static const char *removal_why(fx *x, const core_facts *r) {
    int64_t code = fact_i64(r, FACT_STEP_CODE, 7);
    const char *name = fact_str(r, FACT_STEP_TEXT2), *err = fact_str(r, FACT_STEP_TEXT);
    switch (code) {
    case 1: return TX(x, T_REMOVE_R_NOT_CONNECTED);
    case 2: return TX(x, T_REMOVE_R_NOT_OPENED);
    case 3: return TX(x, T_REMOVE_R_NOT_READ);
    case 4: return TX2(x, T_REMOVE_R_ENTRY, SL(name, name), SL(why, err));
    case 5: return TX1(x, T_REMOVE_R_OTHER_DRIVE, SL(name, name));
    case 6: return TX(x, T_REMOVE_R_MARKER);
    default: return TX(x, T_REMOVE_R_SOME);
    }
}

/* cacheUpdateDue (Actions.swift:662): blocked is asked last (it runs codesign) */
static int32_t cache_why(fx *x, const core_facts *cf, const char **line);
int flows_cache_update_due(int present, int chosen, const char *installed, const char *latest, int app_update_pending,
                           int (*blocked)(void *ctx), void *ctx) {
    if (!present || !chosen || app_update_pending || !installed || !installed[0] || !latest || !flows_is_sha256(latest) || !strcmp(installed, latest))
        return 0;
    return !blocked(ctx);
}

typedef struct { fx *x; const core_facts *cf; } due_ctx;
static int due_blocked(void *p) { due_ctx *d = p; return cache_why(d->x, d->cf, NULL) != FEAT_OK; }

static int cache_update_due(fx *x, const core_facts *cf, const char *installed, const char *latest, int app_update_pending) {
    due_ctx d = {x, cf};
    return flows_cache_update_due(fact_flag(cf, FACT_STEMS_CACHE_PRESENT), fact_flag(cf, FACT_CHOSEN_CACHE), installed, latest,
                                  app_update_pending, due_blocked, &d);
}

/* ---------------------------------------------------------------- what can be installed (Compat.swift, M5's decisions) */

/* FEAT_CHECK_FAILED: the BLOCKED step itself failed (Q3: the action stops, nothing changes) */
enum { FEAT_CHECK_FAILED = -1 };

static int32_t plus_why(fx *x, const core_facts *cf, const char **line) {
    core_facts f;
    if (fx_step(x, CORE_STEP_BLOCKED, &f, 1, "plus") != 0) return FEAT_CHECK_FAILED;
    plus_facts pf = {fact_flag(cf, FACT_REKORDBOX_INSTALLED), fact_str(&f, FACT_ENGINE), fact_flag(&f, FACT_ENGINE_SUPPORTED)};
    int32_t why = plus_blocked(&pf);
    if (line) COMPOSE(x, *line, w_feature_why(B, why, pf.engine, fact_str(cf, FACT_REKORDBOX_VERSION), NULL));
    return why;
}

static int32_t cache_why(fx *x, const core_facts *cf, const char **line) {
    core_facts f;
    if (fx_step(x, CORE_STEP_BLOCKED, &f, 1, "cache") != 0) return FEAT_CHECK_FAILED;
    cache_facts k = {fact_flag(cf, FACT_USER_IS_ADMIN), fact_flag(cf, FACT_REKORDBOX_INSTALLED), fact_str(cf, FACT_REKORDBOX_VERSION),
                     fact_flag(&f, FACT_REKORDBOX_SUPPORTED), fact_flag(cf, FACT_STEMS_CACHE_PRESENT), fact_flag(&f, FACT_ROOT_ORT_EXISTS),
                     fact_flag(&f, FACT_PROTOTYPE_EXISTS), fact_flag(&f, FACT_LIB_EXISTS), fact_flag(&f, FACT_LIB_SIGNED),
                     fact_str_or_null(&f, FACT_ORT_VERSION), fact_str(&f, FACT_ORT_PREFIX)};
#ifdef CORE_WIN_POLICY
    int32_t why = x->os == CORE_OS_WIN ? cache_blocked_win(&k, fact_flag(&f, FACT_ORT_LISTED), 0) : cache_blocked(&k);
#else
    int32_t why = cache_blocked(&k);
#endif
    if (line) COMPOSE(x, *line, w_feature_why(B, why, "", k.rekordbox_version, k.ort_version ? k.ort_version : "unknown"));
    return why;
}

/* ---------------------------------------------------------------- before an action */

/* rekordboxOpen() and pioneerUpdating() now; -1 when they couldn't be checked (Q3: not safe) */
static int rb_open(fx *x, int *updating) {
#ifdef CORE_WIN_POLICY
    if (x->os == CORE_OS_WIN) return win_rb_open(x, updating);   /* rekordbox's linger allowed for */
#endif
    core_facts f;
    int32_t rc = fx_step(x, CORE_STEP_REKORDBOX, &f, 0);
    if (updating) *updating = fact_flag(&f, FACT_PIONEER_UPDATING);
    if (rc != 0) {
        LOG(x, "  rekordbox check failed: stopped");
        return -1;
    }
    return fact_flag(&f, FACT_REKORDBOX_OPEN);
}

/* confirmStopPreparing(verb) (Controller.swift:363): true when nothing is being prepared now */
static int confirm_stop_preparing(fx *x, core_text_id button, const char *verb) {
    main_flags mf = {0};
    if (!fx_ok(x) || !flow_query(x->life, q_flags, &mf, 0)) return 0;
    if (!mf.flow_preparing) return 1;
    core_dialog d;
    char *playlist = flow_run_name(x->life);   /* the run's whole playlist name (SO12) */
    const char *title = TX1(x, T_STOP_PREPARING_TITLE, SL(playlist, playlist));   /* copied into the flow's arena */
    free(playlist);
    dlg_init(&d, T_STOP_PREPARING_TITLE, title, TX(x, T_STOP_PREPARING_MSG), 0);
    dlg_button(x, &d, TX(x, button));
    dlg_button(x, &d, TX(x, T_CANCEL));
    if (ask_d(x, &d) != 0) return 0;   /* "!= Cancel": a stop needs the button; no answer is Cancel */
    LOG(x, "prepare: stopped for ", verb);
    if (!fx_ok(x) || !flow_query(x->life, q_stop_preparing, NULL, 0)) return 0;
    return flow_stop_and_wait(x->life);   /* stopAndWait: off main, on the flow's own thread */
}

/* preflight() (Actions.swift:261) */
static int preflight(fx *x) {
    if (!confirm_stop_preparing(x, T_STOP_AND_CONTINUE, "continue")) return 0;
    int updating;
    int open = rb_open(x, &updating);
    if (open < 0 || !fx_ok(x)) return 0;
    if (updating) {
        alert_ok(x, T_ROOT_UPDATING_TITLE, TX(x, T_ROOT_UPDATING_MSG));
        return 0;
    }
    if (open) {
        alert_ok(x, T_RB_OPEN_TITLE, RB_OPEN_TX(x, T_RB_OPEN_MSG));
        return 0;
    }
    return 1;
}

/* rekordboxClosed() (Actions.swift:272): asks until rekordbox is closed; 1 closed, 0 the user
 * cancelled, -1 it couldn't be checked */
static int rekordbox_closed(fx *x) {
    int open;
    while ((open = rb_open(x, NULL)) > 0) {
        fx_effect(x, EF_SPINNER, 0, "");
        int b = ASK(x, T_RB_OPEN_TITLE, RB_OPEN_TX(x, T_RB_OPEN_TRY_AGAIN), 2, TX(x, T_TRY_AGAIN), TX(x, T_CANCEL));
        fx_effect(x, EF_SPINNER_IF_BUSY, 0, "");
        if (b != 0) return 0;
    }
    if (open < 0) return -1;
    return fx_ok(x) ? 1 : 0;
}

/* SO1 for the queued actions: 1.3.0 ran readyForQueuedAction() and start() back to back on main, so
 * no Prepare could start between them; a run already going is left alone, as 1.3.0 did */
static int q_reserve(core *c, void *arg) {
    (void)arg;
    if (!c->fl.prep_reserved) {
        c->fl.prep_reserved = 1;
        prep_update(c);
    }
    return 1;
}

/* readyForQueuedAction() (Actions.swift:286), with queuedStart's order */
int flows_queued_start(int (*updating)(void *ctx), int (*rekordbox_open)(void *ctx), int (*try_again)(void *ctx), void *ctx) {
    int u = updating(ctx);
    if (u < 0) return -1;
    if (u) return 1;
    int open;
    while ((open = rekordbox_open(ctx)) > 0)
        if (!try_again(ctx)) return 2;
    if (open < 0) return -1;
    if ((u = updating(ctx)) < 0) return -1;
    return u ? 1 : 0;
}

static int q_updating(void *p) {
    int u;
    return RB_UPDATING((fx *)p, &u) < 0 ? -1 : u;
}
static int q_open(void *p) { return rb_open((fx *)p, NULL); }
static int q_try_again(void *p) {
    fx *x = p;
    return ASK(x, T_RB_OPEN_TITLE, RB_OPEN_TX(x, T_RB_OPEN_TRY_AGAIN), 2, TX(x, T_TRY_AGAIN), TX(x, T_CANCEL)) == 0;
}

static int ready_for_queued_action(fx *x) {   /* 1 start, 0 not now, -1 rekordbox couldn't be checked */
    if (!fx_ok(x) || !flow_query(x->life, q_reserve, NULL, 0)) return -1;   /* quitting: the request and the marker stay for the next launch */
    int result = flows_queued_start(q_updating, q_open, q_try_again, x);   /* 0 start, 1 updating, 2 cancelled */
    if (result < 0) return -1;
    if (!fx_ok(x)) return 0;
    if (result == 1) {
        LOG(x, "  rekordbox is updating: not started");
        alert_ok(x, T_ROOT_UPDATING_TITLE, TX(x, T_ROOT_UPDATING_MSG));
        return 0;
    }
    if (result == 2) {
        LOG(x, "  rekordbox is open, the user cancelled: not started");
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- the root runs (Admin.swift:131-181) */

typedef enum { ROOT_DONE, ROOT_FAILED, ROOT_MISSING } root_kind;
typedef struct { root_kind kind; int32_t code; } root_outcome;
enum { RX_NOT_RUN = -1, RX_CANCELLED = -2 };   /* Admin.swift: notRun, cancelled */

static const char *admin_name(int32_t kind) {
    switch (kind) {
    case CORE_ADMIN_CACHE_INSTALL: return "install";
    case CORE_ADMIN_CACHE_REINSTALL: return "reinstall";
    case CORE_ADMIN_REMOVE_ROOT_FOLDER: return "remove-root-folder";
#ifdef CORE_WIN_POLICY
    case CORE_ADMIN_RESTORE: return "restore";
#endif
    default: return "uninstall";
    }
}

typedef struct { const char *bridge_sha, *ort_sha; int force; } root_args;

/* one attempt (admin()): not run for a standard account; else host->admin. code: the script's
 * exit code, RX_NOT_RUN or RX_CANCELLED; not_permitted: App Management refused it (core_admin_note) */
static int32_t admin_attempt(fx *x, int32_t kind, const root_args *ra, int *not_permitted) {
    *not_permitted = 0;
    core_facts f;
    if (!fx_facts(x, &f)) return RX_CANCELLED;
    if (!fact_flag(&f, FACT_USER_IS_ADMIN)) {   /* adminBlocked(): sudo would refuse; no password asked */
        LOG(x, "  FAILED: ", TX(x, T_NEEDS_ADMIN));
        return RX_NOT_RUN;
    }
    core_text_id st, ft;
    root_run_texts(admin_name(kind), &st, &ft);
    fx_effect(x, EF_ADMIN_STATUS, 0, TX(x, st));   /* shown when the host reports the password accepted */
    if (!fx_ok(x)) return RX_CANCELLED;
    if (!x->life->host.admin || (x->life->native & NATIVE_ADMIN)) {
        LOG(x, "  FAILED: no admin runs here");
        return RX_NOT_RUN;
    }
    /* Q1: rekordbox's version as the root script checks it, and rekordbox's bundle id */
    const char *version = "", *bundle = "";
    if (kind != CORE_ADMIN_REMOVE_ROOT_FOLDER) {
        version = fact_str(&f, FACT_REKORDBOX_VERSION);
        bundle = x->os == CORE_OS_MAC ? RB_BUNDLE_ID : NULL;
    } else if (x->os != CORE_OS_MAC) bundle = NULL;
    atomic_store(&x->life->admin_note, 0);   /* the note is cleared before each attempt */
    int32_t r = x->life->host.admin(x->life->host.ctx, kind, version, bundle, ra ? ra->bridge_sha : NULL, ra ? ra->ort_sha : NULL,
                                    ra ? ra->force : 0);
    *not_permitted = (atomic_load(&x->life->admin_note) & CORE_ADMIN_NOTE_NOT_PERMITTED) != 0;
#ifdef CORE_WIN_POLICY
    if (x->os == CORE_OS_WIN) r = win_after_admin(x, r, not_permitted);   /* UAC declined: the user stopped */
#endif
    return r < 0 ? RX_CANCELLED : r;
}

/* runRoot (Admin.swift:141): up to 4 attempts while App Management blocks the run, rekordbox
 * checked before each; the password forgotten at the end; an alert unless the user stopped it */
static root_outcome run_root(fx *x, int32_t kind, const root_args *ra, const char *failure, int alert_missing) {
    int32_t code = 0;
    int have = 0;
#ifdef CORE_WIN_POLICY
    int ran_again = 0;
#endif
    for (int attempt = 1; attempt <= 4; attempt++) {
        int rc = rekordbox_closed(x);
        if (rc <= 0) {
            if (rc == 0) LOG(x, "  rekordbox is open: stopped");   /* a failed check said so itself (F11) */
            return (root_outcome){ROOT_FAILED, RX_CANCELLED};
        }
        LOG(x, "admin run (", admin_name(kind), "), attempt ", num(x, attempt));
        int np;
        code = admin_attempt(x, kind, ra, &np);
        have = 1;
        if (code == 0) break;
#ifdef CORE_WIN_POLICY
        if (x->os == CORE_OS_WIN && code == ROOT_REKORDBOX_OPEN) {
            /* each attempt is a UAC prompt, and the worker also counts the DLL in use, so the core
             * may see rekordbox closed: the user asks for each further elevation (review C2) */
            if (attempt == 4) break;
            fx_effect(x, EF_SPINNER, 0, "");   /* as rekordbox_closed's own ask */
            int b = ASK(x, T_RB_OPEN_TITLE, TX(x, T_RB_OPEN_TRY_AGAIN), 2, TX(x, T_TRY_AGAIN), TX(x, T_CANCEL));
            fx_effect(x, EF_SPINNER_IF_BUSY, 0, "");
            LOG(x, "  rekordbox open for the worker: user chose ", b == 0 ? TX(x, T_TRY_AGAIN) : b == 1 ? TX(x, T_CANCEL) : "");
            if (b != 0) {
                code = RX_CANCELLED;
                break;
            }
            continue;
        }
#endif
        if (code == ROOT_REKORDBOX_OPEN) continue;   /* rekordbox was opened just now: ask to quit it */
#ifdef CORE_WIN_POLICY
        if (x->os == CORE_OS_WIN) {   /* no re-sign loop: Windows has no App Management */
            if (code == ROOT_WIN_RUN_AGAIN && !ran_again) {   /* install-worker put the app's own worker in place */
                ran_again = 1;
                continue;
            }
            break;
        }
#endif
        if (!np || attempt == 4) break;
        fx_effect(x, EF_PERMISSION, 0, "");
        core_dialog d;
        dlg_init(&d, T_PERM_TITLE, TX(x, T_PERM_TITLE), TX(x, T_PERM_MSG), 0);
        dlg_button(x, &d, TX(x, T_TRY_AGAIN));
        dlg_button(x, &d, TX(x, T_CANCEL));
        int b = ask_d(x, &d);
        LOG(x, "  user chose: ", chose(&d, b));
        if (b != 0) {
            code = RX_CANCELLED;
            break;
        }
        fx_busy(x, TX(x, T_TRYING_AGAIN), 0);
    }
    /* forget the password: nothing else can reuse it, and the next action asks once again */
#ifdef CORE_WIN_POLICY
    if (x->os != CORE_OS_WIN)   /* UAC: no password to forget */
#endif
    if (fx_ok(x) && x->life->host.admin && !(x->life->native & NATIVE_ADMIN))
        x->life->host.admin(x->life->host.ctx, CORE_ADMIN_FORGET, NULL, NULL, NULL, NULL, 0);
    if (!have || code == 0) return (root_outcome){ROOT_DONE, 0};
#ifdef CORE_WIN_POLICY
    if (x->os == CORE_OS_WIN && (code == ROOT_WIN_PROTOCOL || code == ROOT_WIN_OLDER)) {
        win_offer_update_first(x);   /* the offer is the alert: the callers show none (RX_CANCELLED) */
        return (root_outcome){ROOT_FAILED, RX_CANCELLED};
    }
#endif
    int missing = code == ROOT_REKORDBOX_MISSING;
    if (failure && code != RX_CANCELLED && (!missing || alert_missing)) {
        core_text_id pt, pb;
        if (root_problem(code, &pt, &pb)) alert_ok(x, pt, tx_rb(x, pb));
        else {
            core_text_id st, ft;
            root_run_texts(admin_name(kind), &st, &ft);
            const char *why = code == RX_NOT_RUN ? TX(x, T_NEEDS_ADMIN) : code == ROOT_ROLLED_BACK ? TX(x, T_ROOT_ROLLED_BACK) : TX(x, T_SEE_LOG);
            alert_ok(x, ft, TX2(x, T_FAILURE_WHY, SL(message, failure), SL(why, why)));
        }
    }
    return missing ? (root_outcome){ROOT_MISSING, ROOT_REKORDBOX_MISSING} : (root_outcome){ROOT_FAILED, code};
}

/* cachePausedNow() and the drive, for the confirmations that name Stems Cache */
static int64_t paused_now(fx *x, const char **drive) {
    view_loc_now n;
    fx_loc(x, &n);
    if (n.known) {   /* the snapshot is the core's (M4) */
        *drive = n.drive;   /* in the flow's arena already */
#ifdef CORE_WIN_POLICY
        /* the floor is the folder in use's (native_location.c's in_use): a chosen drive's only when
         * it is available, its name then "" too (the nameless form); else this PC's disk, NULL
         * (review NT3, NT5) */
        if (x->os == CORE_OS_WIN) *drive = n.drive_set && n.kind == LOC_CHOSEN && n.probe == PROBE_AVAILABLE ? n.drive : NULL;
#endif
        return n.floor;
    }
    core_facts f;
    fx_step(x, CORE_STEP_PAUSED, &f, 0);
    *drive = fact_str(&f, FACT_STEP_TEXT);
#ifdef CORE_WIN_POLICY
    if (x->os == CORE_OS_WIN && !(*drive)[0]) *drive = NULL;   /* a host's own snapshot: "" is this PC's disk, as before */
#endif
    return fact_i64(&f, FACT_STEP_CODE, -1);
}

static const char *install_note(fx *x, int plus, int cache, int reinstall, int64_t floor, const char *drive) {
    const char *s, *d = drive && drive[0] ? drive : NULL;
#ifdef CORE_WIN_POLICY
    if (x->os == CORE_OS_WIN) d = drive;   /* paused_now keyed it: "" is a drive in use with no name (review NT3) */
#endif
    COMPOSE(x, s, w_install_note(B, plus, cache, reinstall, floor >= 0, floor, d));
    return s;
}

/* ---------------------------------------------------------------- actions */

typedef enum { A_INSTALL, A_UNINSTALL_PLUS, A_UNINSTALL_CACHE, A_UNINSTALL_ALL } action_kind;

typedef struct {
    action_kind kind;
    const char *id, *label;
    core_text_id status;
    int model, cache, force, again;   /* A_INSTALL */
    gate confirmed;                    /* A_UNINSTALL_ALL */
} action;

static void install_body(fx *x, const action *a);
static void uninstall_plus_body(fx *x);
static void uninstall_cache_body(fx *x);
static void uninstall_all_body(fx *x, gate confirmed);

static void action_start(fx *x, const action *a) { fx_start(x, a->id, a->label, a->status); }

static void action_body(fx *x, const action *a) {
    switch (a->kind) {
    case A_INSTALL: install_body(x, a); break;
    case A_UNINSTALL_PLUS: uninstall_plus_body(x); break;
    case A_UNINSTALL_CACHE: uninstall_cache_body(x); break;
    case A_UNINSTALL_ALL: uninstall_all_body(x, a->confirmed); break;
    }
}

static void action_run(fx *x, const action *a) {
    action_start(x, a);
    action_body(x, a);
}

static action install_action(const char *id, const char *label, core_text_id status, int model, int cache, int force, int again) {
    action a;
    memset(&a, 0, sizeof a);
    a.kind = A_INSTALL;
    a.id = id;
    a.label = label;
    a.status = status;
    a.model = model;
    a.cache = cache;
    a.force = force;
    a.again = again;
    return a;
}

/* startInstall: a part already in place is skipped */
static action start_install(const char *id, const char *label, int model, int cache) {
    return install_action(id, label, T_INSTALLING, model, cache, 0, 0);
}
/* startReinstallOne: that feature only, refreshed */
static action start_reinstall_one(const char *id, const char *label, int model) {
    return install_action(id, label, T_REINSTALLING, model, !model, 1, 1);
}
static action start_cache_update(fx *x) {
    return install_action("update-cache", TX(x, T_ACTION_UPDATE_CACHE), T_UPDATING_CACHE, 0, 1, 1, 1);
}
static action simple_action(action_kind k, const char *id, const char *label, core_text_id status) {
    action a;
    memset(&a, 0, sizeof a);
    a.kind = k;
    a.id = id;
    a.label = label;
    a.status = status;
    return a;
}

/* names (Controller.swift:258): what Reinstall names */
static const char *reinstall_label(fx *x, int model, int cache) {
    if (model && !cache) return TX(x, T_BTN_REINSTALL_PLUS);
    if (cache && !model) return TX(x, T_BTN_REINSTALL_CACHE);
    return TX1(x, T_REINSTALL_FEATURES, SL(features, TX(x, T_FEATURES_BOTH)));
}

/* reinstallChosen(): exactly what the user chose, refreshed (the Reinstall button) */
static action reinstall_chosen_mk(fx *x, int m, int k) {
    return install_action("reinstall", reinstall_label(x, m || !k, k), T_REINSTALLING, m, k, 1, 1);
}
/* the resume's: what is chosen now */
static action reinstall_chosen(fx *x) {
    core_facts f;
    fx_facts(x, &f);
    return reinstall_chosen_mk(x, fact_flag(&f, FACT_CHOSEN_MODEL), fact_flag(&f, FACT_CHOSEN_CACHE));
}

/* missingParts (Actions.swift:670): what this account chose and went missing */
void flows_missing_parts(int chosen_model, int chosen_cache, int plus_present, int cache_present, int *model, int *cache) {
    *model = chosen_model && !plus_present;
    *cache = chosen_cache && !cache_present;
}

/* reinstallMissing(): what the user chose and went missing; 0 when nothing is */
static int reinstall_missing(fx *x, action *out) {
    core_facts f;
    if (!fx_facts(x, &f)) return 0;
    int m, k;
    flows_missing_parts(fact_flag(&f, FACT_CHOSEN_MODEL), fact_flag(&f, FACT_CHOSEN_CACHE), fact_flag(&f, FACT_STEMS_PLUS_PRESENT),
                        fact_flag(&f, FACT_STEMS_CACHE_PRESENT), &m, &k);
    if (!m && !k) {
        LOG(x, "  nothing is missing: nothing reinstalled");
        return 0;
    }
    *out = install_action("reinstall-missing", reinstall_label(x, m, k), T_REINSTALLING, m, k, 0, 1);
    return 1;
}

/* ---------------------------------------------------------------- install (Actions.swift:15-125) */

/* the download step with its status lines */
static int32_t download(fx *x, const char *which, const char *what) {
    fx_effect(x, EF_DL_WHAT, 0, what);
    return fx_do(x, CORE_STEP_DOWNLOAD, 1, which);
}

static void install_body(fx *x, const action *a) {
    int want_model = a->model, want_cache = a->cache, force = a->force, again = a->again;
    core_facts cf;
    if (!fx_facts(x, &cf)) return;
    int opt_in_only = want_cache && !force && fact_flag(&cf, FACT_STEMS_CACHE_PRESENT);
    core_text_id failed_title = again ? T_REINSTALL_FAILED : T_INSTALL_FAILED;
    fx_status(x, TX(x, T_CHECKING_LATEST));
    core_facts p;
    fx_step(x, CORE_STEP_PAYLOAD, &p, 1, "latest");
    if (!fx_ok(x)) return;
    int64_t fetch = fact_i64(&p, FACT_PAYLOAD_FETCH, 2);
    int fresh = fetch == 0, unverified = fetch == 1;
    core_text_id fail_msg = unverified ? T_UNVERIFIED_MSG : T_OFFLINE_MSG;
    if (!fact_flag(&p, FACT_PAYLOAD_FOUND)) {
        LOG(x, "no payload.json: nothing changed");
        alert_ok(x, T_DOWNLOAD_FAILED, TX(x, fail_msg));
        fx_idle(x);
        return;
    }
    int have_age = p.set[FACT_PAYLOAD_AGE];
    int64_t age = fact_i64(&p, FACT_PAYLOAD_AGE, 0);
    if (!fresh && want_cache && !opt_in_only &&
        !flows_cache_from_saved_payload(unverified, have_age, age, fact_flag(&p, FACT_PAYLOAD_BRIDGE_HERE))) {
        LOG(x, "Stems Cache needs payload.json downloaded now, or offline one downloaded within the last 7 days with its files here: nothing changed");
        alert_ok(x, T_DOWNLOAD_FAILED, TX(x, fail_msg));
        fx_idle(x);
        return;
    }
    const char *pv = fact_str(&p, FACT_PAYLOAD_VERSION), *by = fact_str(&p, FACT_PAYLOAD_SIGNED_BY);
    if (fresh) LOG(x, "using the payload.json just downloaded (payload ", pv, ")");
    else LOG(x, "offline: using the saved payload.json (payload ", pv, ", downloaded ", num(x, age / 3600), " hours ago, signed with the ",
             by[0] ? by : "?", " key)");
    const char *app_v = fact_str(&p, FACT_PAYLOAD_APP_VERSION);
    fx_effect(x, EF_UPDATE_FOUND, 0, flows_version_newer(app_v, x->own) ? app_v : "");   /* the notice follows the latest payload.json */
    const char *why = "";
    int32_t pw = want_model ? plus_why(x, &cf, &why) : FEAT_OK;
    if (pw != FEAT_OK && pw != FEAT_CHECK_FAILED) {
        LOG(x, "Demucs v4 not installed: ", why);
        want_model = 0;
    }
    int32_t kw = pw != FEAT_CHECK_FAILED && want_cache && !opt_in_only ? cache_why(x, &cf, &why) : FEAT_OK;
    if (kw != FEAT_OK && kw != FEAT_CHECK_FAILED) {
        LOG(x, "Stems Cache not installed: ", why);
        want_cache = 0;
    }
    if (pw == FEAT_CHECK_FAILED || kw == FEAT_CHECK_FAILED) {
        LOG(x, "  feature check failed: stopped");   /* Q3: not safe to go on */
        fx_idle(x);
        return;
    }
    if (!fx_facts(x, &cf)) return;
    int ch_model = fact_flag(&cf, FACT_CHOSEN_MODEL), ch_cache = fact_flag(&cf, FACT_CHOSEN_CACHE);
    int copy_model = want_model && force;
    if (want_model && !force) {   /* Q19: the model's sha256, memoised by path, size and mtime */
        const char *installed = "";
        core_facts sf;
        facts_init(&sf, &x->a);
        const char *path = fact_str(&p, FACT_STEP_PATH);
        if (fx_ok(x) && x->life->host.slow_fact && !(x->life->native & NATIVE_SLOW_FACT) &&
            x->life->host.slow_fact(x->life->host.ctx, CORE_SLOW_MODEL_SHA, path, CORE_NTS, &sf) == 0)
            installed = fact_str(&sf, FACT_SHA256);
        const char *expected = fact_str(&p, FACT_PAYLOAD_MODEL_SHA);
        copy_model = !flows_is_sha256(installed) || !flows_is_sha256(expected) || strcmp(installed, expected) != 0;
    }
    core_facts s;
    if (fx_step(x, CORE_STEP_SPACE, &s, 2, copy_model ? "1" : "0", want_cache && !opt_in_only ? "1" : "0") != 0) {
        LOG(x, "  free space check failed: stopped");   /* Q3: not safe to go on */
        fx_idle(x);
        return;
    }
    int64_t need = fact_i64(&s, FACT_STEP_CODE, 0);
    if (need > 0) {
        const char *gb = space_gb(x, need);
        LOG(x, "not enough free space (about ", gb, " GB needed): nothing changed");
        alert_ok(x, T_LOW_DISK, TX1(x, T_LOW_DISK_NEED_MSG, SL(gb, gb)));
        fx_idle(x);
        return;
    }
    int32_t got = 0;
    if (copy_model) got = download(x, "model", TX(x, T_WHAT_MODEL));
    if (got == 0 && want_cache && !opt_in_only) got = download(x, "bridge", TX(x, T_FEATURE_CACHE));
    if (!fx_ok(x)) return;
    switch (got) {
    case 0: break;
    case 2:
        LOG(x, "download: the disk is full: nothing changed");
        alert_ok(x, T_LOW_DISK, TX(x, T_DISK_FULL_MSG));
        fx_idle(x);
        return;
    case 3:
        LOG(x, "download damaged: nothing changed");
        alert_ok(x, T_DOWNLOAD_FAILED, TX(x, T_DAMAGED_MSG));
        fx_idle(x);
        return;
    default:   /* offline, and any other failure */
        LOG(x, "download failed: nothing changed");
        alert_ok(x, T_DOWNLOAD_FAILED, TX(x, T_OFFLINE_MSG));
        fx_idle(x);
        return;
    }
    fx_do(x, CORE_STEP_PRUNE_PAYLOADS, 0);   /* the last good payload is now this one (when its model is here) */
    /* the downloads may have taken minutes: rekordbox may have been opened meanwhile */
    int closed = rekordbox_closed(x);
    if (closed <= 0) {
        if (closed == 0) LOG(x, "rekordbox is open: nothing changed");
        fx_idle(x);
        return;
    }
    if (want_model) {
        if (copy_model) {
            fx_status(x, TX(x, T_INSTALLING_PLUS));
            core_facts r;
            if (fx_step(x, CORE_STEP_MODEL_COPY, &r, 0) != 0) {
                if (!fx_ok(x)) return;
                const char *msg = model_message(x, fact_i64(&r, FACT_STEP_CODE, CORE_MODEL_COPY_BAD), fact_str(&r, FACT_STEP_TEXT), CORE_MODEL_COPY_BAD);
                LOG(x, "Demucs v4 not installed: ", msg);
                alert_ok(x, failed_title, TX1(x, again ? T_PLUS_NOT_DONE_REINSTALLED : T_PLUS_NOT_DONE_INSTALLED, SL(message, msg)));
                fx_idle(x);
                return;
            }
            ch_model = 1;
        } else {
            LOG(x, "Demucs v4: already in place");
            ch_model = 1;
        }
    }
    if (want_cache && opt_in_only) {
        LOG(x, "Stems Cache: already in rekordbox (installed from another account): turned on for this account");
        ch_cache = 1;
    } else if (want_cache) {
        fx_status(x, TX(x, T_PREPARING_CACHE));
        core_facts pr;
        int32_t rc = fx_step(x, CORE_STEP_CACHE_PREPARE, &pr, 0);
        if (!fx_ok(x)) return;
        if (rc == 0) {
            root_args ra = {fact_str(&p, FACT_PAYLOAD_BRIDGE_SHA), fact_str(&pr, FACT_STEP_TEXT), force};
            root_outcome o = run_root(x, again ? CORE_ADMIN_CACHE_REINSTALL : CORE_ADMIN_CACHE_INSTALL, &ra,
                                      TX(x, again ? T_CACHE_NOT_REINSTALLED : T_CACHE_NOT_INSTALLED), 1);
            if (o.kind == ROOT_DONE && fx_facts(x, &cf) && fact_flag(&cf, FACT_STEMS_CACHE_PRESENT)) ch_cache = 1;
        } else {
            LOG(x, "Stems Cache not installed: the downloaded bridge changed after it was checked");
            alert_ok(x, failed_title, TX(x, again ? T_CACHE_NOT_REINSTALLED_LOG : T_CACHE_NOT_INSTALLED_LOG));
        }
    }
    if (!fx_ok(x)) return;
    fx_do(x, CORE_STEP_SET_CHOSEN, 2, ch_model ? "1" : "0", ch_cache ? "1" : "0");
    if (want_cache && ch_cache) {
        int ok = fx_do(x, CORE_STEP_CONFIG_WRITE, 2, "rekordbox_model", "1") == 0;
        LOG(x, "Stems Cache: saving rekordbox's own model's stems too (rekordbox_model=1): ", ok ? "ok" : "FAILED");
    }
    if (ch_model || ch_cache) fx_watcher_step(x, CORE_STEP_LAUNCH_AGENT_LOAD);
    core_facts st;
    fx_step(x, CORE_STEP_STATE_LINE, &st, 0);
    LOG(x, "state: ", fact_str(&st, FACT_STEP_TEXT));
    fx_idle(x);
}

/* ---------------------------------------------------------------- uninstall (Actions.swift:373-569) */

static void log_state(fx *x, const char *prefix) {
    core_facts st;
    fx_step(x, CORE_STEP_STATE_LINE, &st, 0);
    LOG(x, prefix, fact_str(&st, FACT_STEP_TEXT));
}

/* Demucs v4 out: rekordbox's own model back; Stems Cache stays. No password. */
static void uninstall_plus_body(fx *x) {
    int closed = rekordbox_closed(x);
    if (closed <= 0) {
        if (closed == 0) LOG(x, "rekordbox is open: Demucs v4 left in place");
        fx_idle(x);
        return;
    }
    core_facts r, f;
    int32_t rc = fx_step(x, CORE_STEP_MODEL_RESTORE, &r, 0);
    if (!fx_ok(x)) return;
    if (rc == 0) {
        if (!fx_facts(x, &f)) return;
        fx_do(x, CORE_STEP_SET_CHOSEN, 2, "0", fact_flag(&f, FACT_CHOSEN_CACHE) ? "1" : "0");
    } else {
        int64_t code = fact_i64(&r, FACT_STEP_CODE, CORE_MODEL_NOT_BACK);
        const char *msg = model_message(x, code, fact_str(&r, FACT_STEP_TEXT), CORE_MODEL_NOT_BACK);
        if (code == CORE_MODEL_NO_SPACE_BACK) {
            alert_ok(x, T_UNINSTALL_FAILED, TX1(x, T_PLUS_NOT_REMOVED_SPACE, SL(message, msg)));
        } else {
            int b = ASK(x, T_UNINSTALL_FAILED, TX1(x, T_PLUS_NOT_REMOVED_HELP, SL(message, msg)), 2, TX(x, T_OK), TX(x, T_OPEN_HELP));
            if (b == 1) {
                core_facts mi;
                fx_step(x, CORE_STEP_MODELS_INFO, &mi, 0);
                fx_do(x, CORE_STEP_OPEN_URL, 1, fact_str(&mi, FACT_STEP_URL));
            }
        }
    }
    if (!fx_facts(x, &f)) return;
    if (!fact_flag(&f, FACT_CHOSEN_MODEL) && !fact_flag(&f, FACT_CHOSEN_CACHE)) fx_watcher_step(x, CORE_STEP_LAUNCH_AGENT_UNLOAD);   /* nothing left to watch */
    log_state(x, "state: ");
    fx_idle(x);
}

typedef enum { CR_REMOVED, CR_FAILED, CR_KEPT_ROOT_FOLDER } cache_removal_kind;
typedef struct { cache_removal_kind kind; int32_t code; } cache_removal;

/* offerRootFolderCleanup (Actions.swift:549) */
static int offer_root_folder_cleanup(fx *x) {
    core_dialog d;
    dlg_init(&d, T_ROOT_MISSING_TITLE, TX(x, T_ROOT_MISSING_TITLE), tx_rb(x, T_ROOT_FOLDER_CLEANUP_MSG), 0);
    dlg_button(x, &d, TX(x, T_CANCEL));
    dlg_button(x, &d, TX(x, T_REMOVE));
    int b = ask_d(x, &d);
    LOG(x, "  user chose: ", chose(&d, b));
    return b == 1;
}

/* removeCache (Actions.swift:755) over runUninstallCache's steps (:560) */
static cache_removal run_uninstall_cache(fx *x, const char *failure, const char *root_folder_failure) {
    core_facts f;
    fx_step(x, CORE_STEP_CACHE_FOLDERS, &f, 0);
    if (fact_flag(&f, FACT_STEP_FLAG)) {   /* rekordbox's folder is there */
        root_outcome o = run_root(x, CORE_ADMIN_CACHE_UNINSTALL, NULL, failure, 0);
        if (o.kind == ROOT_DONE) return (cache_removal){CR_REMOVED, 0};
        if (o.kind == ROOT_FAILED) return (cache_removal){CR_FAILED, o.code};
        fx_step(x, CORE_STEP_CACHE_FOLDERS, &f, 0);   /* gone during the run */
    }
    if (!fact_flag(&f, FACT_STEP_FLAG2)) return (cache_removal){CR_REMOVED, 0};
    LOG(x, "rekordbox isn't in the Applications folder: asking to remove the copy of its files");
    if (!offer_root_folder_cleanup(x)) return (cache_removal){CR_KEPT_ROOT_FOLDER, 0};
    root_outcome o = run_root(x, CORE_ADMIN_REMOVE_ROOT_FOLDER, NULL, root_folder_failure, 0);
    if (o.kind == ROOT_DONE) return (cache_removal){CR_REMOVED, 0};
    return (cache_removal){CR_FAILED, o.kind == ROOT_MISSING ? ROOT_REKORDBOX_MISSING : o.code};
}

static void uninstall_cache_body(fx *x) {
    run_uninstall_cache(x, TX(x, T_CACHE_NOT_REMOVED), TX(x, T_ROOT_FOLDER_FAILURE));
    core_facts f;
    if (!fx_facts(x, &f)) return;
    if (!fact_flag(&f, FACT_STEMS_CACHE_PRESENT)) {
        fx_do(x, CORE_STEP_SET_CHOSEN, 2, fact_flag(&f, FACT_CHOSEN_MODEL) ? "1" : "0", "0");
        core_facts s;
        fx_step(x, CORE_STEP_CONFIG_SETS, &s, 1, "rekordbox_model");
        if (fact_flag(&s, FACT_STEP_FLAG)) fx_do(x, CORE_STEP_CONFIG_WRITE, 2, "rekordbox_model", "0");
    }
    log_state(x, "state: ");
    fx_idle(x);
}

/* removeOtherAccountsCache (Actions.swift:469) */
static int remove_other_accounts_cache(fx *x) {
    core_facts f;
    if (!fx_facts(x, &f)) return 0;
    if (fact_flag(&f, FACT_CHOSEN_CACHE)) return 1;
    core_dialog d;
    dlg_init(&d, T_OTHER_ACCOUNT_TITLE, TX(x, T_OTHER_ACCOUNT_TITLE), TX(x, T_OTHER_ACCOUNT_MSG), 0);
    dlg_button(x, &d, TX(x, T_REMOVE));
    dlg_button(x, &d, TX(x, T_KEEP));
    int b = ask_d(x, &d);
    LOG(x, "Stems Cache wasn't installed from this account: user chose ", chose(&d, b));
    return b == 0;
}

/* LocationMonitor.current() beside the snapshot */
typedef struct { const char *dir, *drive, *name, *shown, *size; gate g; } loc_now;
static loc_now location_now(fx *x) {
    core_facts f;
    view_loc_now n;
    fx_loc(x, &n);
    if (n.known) fx_step(x, CORE_STEP_LOCATION_NOW, &f, 1, n.in_use);   /* the memo's folder in use: "same" is the host's */
    else fx_step(x, CORE_STEP_LOCATION_NOW, &f, 0);
    loc_now l;
    l.dir = fact_str(&f, FACT_LOC_DIR);
    l.drive = fact_str(&f, FACT_LOC_DRIVE);
    l.name = fact_str(&f, FACT_STEP_TEXT2);
    l.shown = fact_str(&f, FACT_STEP_TEXT);
    int64_t bytes;
    int64_t kind = size_kind_of(x, &f, FACT_LOC_SIZE_KIND, FACT_LOC_SIZE, &bytes, n.known ? &n : NULL);   /* the snapshot args[0] came from */
    l.size = stems_size(x, kind, bytes);
    const char *id = fact_str(&f, FACT_LOC_GATE_ID);
    l.g = id[0] && l.dir[0] ? (gate){l.dir, id} : (gate){NULL, NULL};
    return l;
}

static int same_id(const char *a, const char *b) {
    if (strlen(a) != strlen(b)) return 0;
    for (; *a; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return 1;
}

/* driveDeletionAllowed (Location.swift:738): the folder the confirmation named, found again (its
 * marker id compared without case); NULL dir: none */
int flows_drive_deletion_allowed(const char *confirmed_dir, const char *confirmed_id, const char *fresh_dir, const char *fresh_id) {
    return confirmed_dir && fresh_dir && confirmed_id && fresh_id && !strcmp(confirmed_dir, fresh_dir) && same_id(confirmed_id, fresh_id);
}

static int drive_deletion_allowed(gate confirmed, gate fresh) {
    return flows_drive_deletion_allowed(confirmed.dir, confirmed.id, fresh.dir, fresh.id);
}

/* the deletion's status lines while REMOVE_LOCATION runs */
#ifdef CORE_WIN_POLICY
/* Windows' drive with no name (""): its text's nameless form, with no {drive} (review NR2, NT1) */
static int fx_nameless(const fx *x, const char *drive) { return x->os == CORE_OS_WIN && drive && !drive[0]; }
#endif

static void delete_texts(fx *x, core_text_id first, core_text_id with_n, const char *drive) {
#ifdef CORE_WIN_POLICY
    if (fx_nameless(x, drive)) {   /* the nameless ids are stored: the progress then passes only {n} (review NT1) */
        fx_effect3(x, EF_DELETE_TEXTS, 0, drive, nameless_text(first), nameless_text(with_n));
        fx_status(x, TX(x, nameless_text(first)));
        return;
    }
#endif
    fx_effect3(x, EF_DELETE_TEXTS, 0, drive, first, with_n);
    fx_status(x, TX1(x, first, SL(drive, drive)));
}

/* removeOwnFiles (Actions.swift:501): this account's own files, then the app once it has quit */
static void remove_own_files(fx *x, core_text_id title, const char *about, const char *help, int keep_originals, gate confirmed) {
    fx_watcher_step(x, CORE_STEP_LAUNCH_AGENT_UNLOAD);
    log_state(x, "final state: ");
    core_facts f;
    if (!fx_facts(x, &f)) return;
    int others_cache = fact_flag(&f, FACT_CACHE_TRACES) && !fact_flag(&f, FACT_CHOSEN_CACHE), cache_link = fact_flag(&f, FACT_CACHE_IS_LINK);
    const char *left[2];
    int nleft = 0;
    loc_now l = location_now(x);
    if (l.dir[0]) {
        const char *drive = l.name;
        if (l.g.dir && drive_deletion_allowed(confirmed, l.g)) {
            delete_texts(x, T_DELETING_DRIVE, T_DELETING_DRIVE_N, drive);
            core_facts r;
            int32_t rc = fx_step(x, CORE_STEP_REMOVE_LOCATION, &r, 3, l.g.dir, l.g.id, "1");
            if (rc == 0) LOG(x, "stems on ", l.shown, " deleted: ok");
            else LOG(x, "stems on ", l.shown, " deleted: FAILED (", removal_why(x, &r), ")");
#ifdef CORE_WIN_POLICY
            if (rc != 0 && fx_nameless(x, drive)) left[nleft++] = TX1(x, nameless_text(T_LEFT_DRIVE_PARTLY), SL(dir, l.dir));
            else
#endif
            if (rc != 0) left[nleft++] = TX2(x, T_LEFT_DRIVE_PARTLY, SL(drive, drive), SL(dir, l.dir));
        } else if (!l.g.dir) {
#ifdef CORE_WIN_POLICY
            if (x->os == CORE_OS_WIN && drive && !drive[0]) left[nleft++] = TX1(x, nameless_text(T_LEFT_DRIVE_NOT_CONNECTED), SL(dir, l.dir));
            else
#endif
            left[nleft++] = TX2(x, T_LEFT_DRIVE_NOT_CONNECTED, SL(drive, drive), SL(dir, l.dir));
        } else {
            LOG(x, "stems on ", l.shown, " kept: the confirmation didn't name them");
#ifdef CORE_WIN_POLICY
            if (fx_nameless(x, drive)) left[nleft++] = TX1(x, nameless_text(T_LEFT_DRIVE_NOT_NAMED), SL(dir, l.dir));
            else
#endif
            left[nleft++] = TX2(x, T_LEFT_DRIVE_NOT_NAMED, SL(drive, drive), SL(dir, l.dir));
        }
    }
    /* nothing is logged after this, but a SELF_DELETE that couldn't start (the app stays open) */
    if (fx_do(x, CORE_STEP_REMOVE_OWN_FOLDERS, 1, keep_originals ? "1" : "0") != 0)
        left[nleft++] = TX(x, T_LEFT_LIBRARY);
    if (!fx_ok(x)) return;
    core_facts af;
    fx_step(x, CORE_STEP_APP_FATE, &af, 0);
    int64_t fate = fact_i64(&af, FACT_STEP_CODE, FATE_CANT_DELETE);
    const char *me = fate == FATE_DELETED ? fact_str(&af, FACT_STEP_PATH) : "";
#ifdef CORE_WIN_POLICY
    if (x->os == CORE_OS_WIN && x->win_restart) about = CAT(x, about ? about : "", TX(x, T_WIN_REMOVED_AT_RESTART));   /* I20 */
#endif
    const char *text;
    COMPOSE(x, text, w_own_files_removed(B, about, (int32_t)fate, others_cache, cache_link, left, nleft));
    core_dialog d;
    dlg_init(&d, title, TX(x, title), text, 0);
    dlg_button(x, &d, TX(x, T_DONE));
    if (help) dlg_button(x, &d, TX(x, T_OPEN_HELP));
    int b = ask_d(x, &d);
    if (b == 1 && help) fx_do(x, CORE_STEP_OPEN_URL, 1, help);
    if (!fx_ok(x)) return;
    int undocked = 0;
#ifdef CORE_WIN_POLICY
    if (me[0] && x->os != CORE_OS_WIN) {   /* no Dock on Windows: the step isn't asked */
#else
    if (me[0]) {
#endif
        core_facts dk;
        fx_step(x, CORE_STEP_DOCK, &dk, 1, me);
        undocked = fact_flag(&dk, FACT_STEP_FLAG);
    }
    /* the app quits: this quit waits for nothing. A step that fails (its after-exit cleanup
     * couldn't start) leaves the app open, and it isn't left busy; when the app was to be deleted,
     * one line says what's left, in an alert as the uninstall's own end is said (owner decided,
     * 2026-11-12) */
    int32_t sd = fx_do(x, CORE_STEP_SELF_DELETE, 2, me, undocked ? "1" : "0");
    if (sd == 0) atomic_store(&x->life->flow_quits, 1);   /* the app quits: busy stays on to the end */
    else if (fx_ok(x)) {
        LOG(x, "self-delete couldn't start: RB Stems Plus stays open");
        fx_idle(x);
        if (me[0]) alert_ok(x, title, TX(x, T_SELF_DELETE_FAILED));   /* only when the app was to go: else the end dialog said what stays */
    }
}

/* removeAnywayChoice + offerRemoveAnyway (Actions.swift:479, 803) */
static void offer_remove_anyway(fx *x, const core_facts *r) {
    loc_now l = location_now(x);
    const char *note = "";   /* locationRemovalNote */
    if (l.dir[0]) COMPOSE(x, note, w_location_removal_note(B, l.drive, l.g.dir != NULL, l.size));
    int64_t code = fact_i64(r, FACT_STEP_CODE, CORE_MODEL_NOT_BACK);
    const char *msg = model_message(x, code, fact_str(r, FACT_STEP_TEXT), CORE_MODEL_NOT_BACK);
    if (code == CORE_MODEL_NO_SPACE_BACK) {   /* no space: freeing some and trying again works */
        alert_ok(x, T_UNINSTALL_STOPPED, TX1(x, T_ANYWAY_NO_SPACE, SL(message, msg)));
        fx_idle(x);
        return;
    }
    core_facts mi;
    fx_step(x, CORE_STEP_MODELS_INFO, &mi, 0);
    const char *stays = model_left_behind(x, fact_i64(&mi, FACT_STEP_CODE, 3));
    int keep = fact_flag(&mi, FACT_ORIGINALS_VERIFIED);
    const char *url = fact_str(&mi, FACT_STEP_URL);
    const char *text = CAT(x, TX1(x, T_ANYWAY_INTRO, SL(message, msg)), note,
                           keep ? CAT(x, note[0] ? "," : "", TX(x, T_ANYWAY_KEEPS_SAVED)) : ". ",
                           /* said just before: the unknown model, or the very sentence of the failure */
                           code == CORE_MODEL_UNKNOWN || !strcmp(stays, msg) ? "" : CAT(x, stays, " "),
                           TX(x, T_ANYWAY_GET_BACK));
    core_dialog d;
    dlg_init(&d, T_UNINSTALL_STOPPED, TX(x, T_UNINSTALL_STOPPED), text, 0);
    dlg_button(x, &d, TX(x, T_CANCEL));
    dlg_button(x, &d, TX(x, T_ANYWAY_BTN));
    dlg_button(x, &d, TX(x, T_OPEN_HELP));
    int b = ask_d(x, &d);
    LOG(x, "  user chose: ", chose(&d, b), keep ? " (the saved copy of Pioneer's model stays)" : "");
    if (b == 2) {   /* stops, nothing deleted: the page ends with "uninstall again" */
        fx_idle(x);
        fx_do(x, CORE_STEP_OPEN_URL, 1, url);
        return;
    }
    if (b != 1) {
        fx_idle(x);
        return;
    }
    const char *about = keep ? TX2(x, T_ANYWAY_ABOUT_KEPT, SL(message, stays), SL(dir, fact_str(&mi, FACT_STEP_PATH)))
                             : TX1(x, T_ANYWAY_ABOUT, SL(message, stays));
    remove_own_files(x, T_REMOVED_TITLE, about, url, keep, l.g);
}

static void uninstall_all_body(fx *x, gate confirmed) {
    /* Stems Cache, or only what it left in /Library after a rekordbox update removed it */
    int keep_cache = 0;
    cache_removal removal = {CR_REMOVED, 0};
    core_facts f;
    if (!fx_facts(x, &f)) return;
    if (fact_flag(&f, FACT_CACHE_TRACES)) {
        if (remove_other_accounts_cache(x)) {
            removal = run_uninstall_cache(x, NULL, NULL);
            if (removal.kind == CR_KEPT_ROOT_FOLDER) {
                LOG(x, "the copy of rekordbox's files stays: stopping, nothing else deleted");
                fx_idle(x);
                alert_ok(x, T_UNINSTALL_STOPPED, TX(x, T_KEPT_ROOT_FOLDER_MSG));
                return;
            }
            if (!fx_facts(x, &f)) return;
            if (!fact_flag(&f, FACT_STEMS_CACHE_PRESENT)) fx_do(x, CORE_STEP_SET_CHOSEN, 2, fact_flag(&f, FACT_CHOSEN_MODEL) ? "1" : "0", "0");
        } else keep_cache = 1;
    }
    if (!fx_facts(x, &f)) return;
    if (fact_flag(&f, FACT_CACHE_TRACES) && !keep_cache) {
        LOG(x, "Stems Cache could not be removed: stopping, nothing else deleted");
        fx_idle(x);
        if (removal.kind == CR_FAILED && removal.code == RX_CANCELLED) return;   /* the user knows: no alert */
        const char *why;
        COMPOSE(x, why, w_cache_failure_reason(B, removal.kind == CR_FAILED, removal.code, CORE_PROGRAMS(&x->life->c)));
        alert_ok(x, T_UNINSTALL_STOPPED, TX1(x, T_UNINSTALL_STOPPED_CACHE, SL(why, why)));
        return;
    }
    int closed = rekordbox_closed(x);
    if (closed <= 0) {
        if (closed == 0) LOG(x, "rekordbox is open: stopping, nothing else deleted");
        fx_idle(x);
        return;
    }
    /* our files (and Pioneer's saved model with them) go only once rekordbox's model is verifiably not ours */
    core_facts r;
    int32_t rc = fx_step(x, CORE_STEP_MODEL_RESTORE, &r, 0);
    if (!fx_ok(x)) return;
    if (rc != 0) {
        LOG(x, "rekordbox's own model isn't back: stopping, nothing else deleted");
        offer_remove_anyway(x, &r);
        return;
    }
    if (!fx_facts(x, &f)) return;
    fx_do(x, CORE_STEP_SET_CHOSEN, 2, "0", fact_flag(&f, FACT_CHOSEN_CACHE) ? "1" : "0");
    remove_own_files(x, T_UNINSTALLED_TITLE, TX(x, keep_cache ? T_UNINSTALLED_OTHERS_CACHE : T_UNINSTALLED_BACK), NULL, 0, confirmed);
}

/* ---------------------------------------------------------------- the click flows */

typedef enum {
    F_INSTALL_PLUS, F_INSTALL_CACHE, F_REINSTALL, F_REINSTALL_PLUS, F_REINSTALL_CACHE, F_UNINSTALL_PLUS,
    F_UNINSTALL_CACHE, F_UNINSTALL_ALL, F_SETTINGS, F_UPDATE, F_REPORT, F_STARTUP, F_REQUEST, F_CACHE_OFFER
#ifdef CORE_WIN_POLICY
    , F_WIN_RESTORE   /* R1's "Put Back Pioneer's File" (W-core) */
#endif
} flow_kind;

typedef struct { flow_kind kind; char own[32]; } flow_arg;

static void settings_flow(fx *x);
static void update_flow(fx *x);
static void report_flow(fx *x);
static void startup_flow(fx *x);
static int check_request(fx *x, action *pending);
static int offer_cache_update(fx *x, action *pending);

static void click_flow(fx *x, flow_kind k) {
    core_facts f;
    const char *drive;
    switch (k) {
    case F_INSTALL_PLUS: {
        const char *label = TX(x, T_BTN_INSTALL_PLUS);
        if (!preflight(x) || !confirm(x, label, TX(x, T_INSTALL), install_note(x, 1, 0, 0, -1, NULL))) return;
        action a = start_install("install-plus", label, 1, 0);
        action_run(x, &a);
        return;
    }
    case F_INSTALL_CACHE: {
        const char *label = TX(x, T_BTN_INSTALL_CACHE);
        if (!preflight(x)) return;
        int64_t floor = paused_now(x, &drive);   /* Stems Cache would start paused: said before it is installed */
        if (!confirm(x, label, TX(x, T_INSTALL), install_note(x, 0, 1, 0, floor, drive))) return;
        action a = start_install("install-cache", label, 0, 1);
        action_run(x, &a);
        return;
    }
    case F_REINSTALL: {
        if (!preflight(x)) return;
        int64_t floor = paused_now(x, &drive);
        if (!guard_snap(x, &f)) return;   /* F1: the text, the guard and the action from one snapshot */
        int m = fact_flag(&f, FACT_CHOSEN_MODEL), c = fact_flag(&f, FACT_CHOSEN_CACHE);
        const char *label = reinstall_label(x, m || !c, c);
        if (!confirm(x, label, TX(x, T_REINSTALL), install_note(x, m || !c, c, 1, floor, drive))) return;
        action a = reinstall_chosen_mk(x, m, c);
        action_run(x, &a);
        return;
    }
    case F_REINSTALL_PLUS: {
        const char *label = TX(x, T_BTN_REINSTALL_PLUS);
        if (!preflight(x) || !confirm(x, label, TX(x, T_REINSTALL), install_note(x, 1, 0, 1, -1, NULL))) return;
        action a = start_reinstall_one("reinstall-plus", label, 1);
        action_run(x, &a);
        return;
    }
    case F_REINSTALL_CACHE: {
        const char *label = TX(x, T_BTN_REINSTALL_CACHE);
        if (!preflight(x)) return;
        int64_t floor = paused_now(x, &drive);
        if (!confirm(x, label, TX(x, T_REINSTALL), install_note(x, 0, 1, 1, floor, drive))) return;
        action a = start_reinstall_one("reinstall-cache", label, 0);
        action_run(x, &a);
        return;
    }
    case F_UNINSTALL_PLUS: {
        const char *label = TX(x, T_BTN_UNINSTALL_PLUS), *note;
        COMPOSE(x, note, w_uninstall_note(B, 0));
        if (!preflight(x) || !confirm(x, label, TX(x, T_UNINSTALL), note)) return;
        action a = simple_action(A_UNINSTALL_PLUS, "uninstall-plus", label, T_UNINSTALLING);
        action_run(x, &a);
        return;
    }
    case F_UNINSTALL_CACHE: {
        const char *label = TX(x, T_BTN_UNINSTALL_CACHE), *note;
        COMPOSE(x, note, w_uninstall_note(B, 1));
        if (!preflight(x) || !confirm(x, label, TX(x, T_UNINSTALL), note)) return;
        action a = simple_action(A_UNINSTALL_CACHE, "uninstall-cache", label, T_RUN_UNINSTALLING_CACHE);
        action_run(x, &a);
        return;
    }
    case F_UNINSTALL_ALL: {
        if (!preflight(x) || !guard_snap(x, &f)) return;   /* F1: the snapshot the text comes from */
        /* the stems it saved, so the user sees what is freed */
        int others_cache = fact_flag(&f, FACT_CACHE_TRACES) && !fact_flag(&f, FACT_CHOSEN_CACHE);
        int no_saved_model = 0;
        if (fact_flag(&f, FACT_STEMS_PLUS_PRESENT)) {
            core_facts mi;
            int rc = fx_step(x, CORE_STEP_MODELS_INFO, &mi, 1, "saved");
            no_saved_model = rc == 0 && !fact_flag(&mi, FACT_ORIGINALS_SAVED);   /* F14: unknown isn't "none" */
        }
        /* where the stems are, checked now: the confirmation names what will be deleted */
        int was = bg_begin(x, T_CHECKING_WHERE);
        loc_now l = location_now(x);
        bg_end(x, was);
        const char *msg;
        COMPOSE(x, msg, w_uninstall_all_message(B, others_cache, no_saved_model, l.size, l.dir[0] ? l.drive : NULL, l.g.dir != NULL));
        guard_arm(x);
        int b = ASK(x, T_UNINSTALL_ALL_Q, msg, 2, TX(x, T_UNINSTALL), TX(x, T_CANCEL));
        if (disarm_guard(x, b) != 0) return;
        action a = simple_action(A_UNINSTALL_ALL, "uninstall-all", TX(x, T_BTN_UNINSTALL_ALL), T_UNINSTALLING_ALL);
        a.confirmed = l.g;
        action_run(x, &a);
        return;
    }
    case F_SETTINGS: settings_flow(x); return;
    case F_UPDATE: update_flow(x); return;
    case F_REPORT: report_flow(x); return;
    case F_STARTUP: startup_flow(x); return;
    case F_REQUEST: {
        action a;
        if (check_request(x, &a)) action_run(x, &a);
        return;
    }
    case F_CACHE_OFFER: {
        action a;
        if (offer_cache_update(x, &a)) action_run(x, &a);
        return;
    }
#ifdef CORE_WIN_POLICY
    case F_WIN_RESTORE: win_restore_flow(x); return;
#endif
    }
}

static void flow_main(core_life *life, void *arg) {
    flow_arg *fa = arg;
    fx x;
    memset(&x, 0, sizeof x);
    x.life = life;
    x.os = life->os;
    memcpy(x.own, fa->own, sizeof x.own);
    flow_kind k = fa->kind;
    free(fa);   /* Q24: the flow's own copy is taken */
    click_flow(&x, k);
    fx_release(&x);
}

/* flow_work hands the arg to flow_main, which frees it; a dropped job frees it with this */
static void free_flow_arg(void *a) { free(a); }

static int start_flow(core *c, flow_kind k) {
    flow_arg *a = calloc(1, sizeof *a);
    if (!a) return CORE_E_NOMEM;
    a->kind = k;
    snprintf(a->own, sizeof a->own, "%s", c->version ? c->version : "");
    int r = flow_start_ex(c, CORE_WORK_FLOW, flow_main, a, free_flow_arg, 0);
    if (!r && k == F_STARTUP) c->fl.startup_running = 1;   /* OM1: its end marks the start-up done */
    return r;
}

/* ---------------------------------------------------------------- an action left half-done (Actions.swift:575) */

static int check_resume(fx *x, action *pending) {
    core_facts f;
    fx_step(x, CORE_STEP_LEFTOVER, &f, 0);   /* also forgets a password the last run kept */
    const char *id = fact_str(&f, FACT_STEP_TEXT), *label = fact_str(&f, FACT_STEP_TEXT2);
    if (!id[0] || !fx_ok(x)) return 0;
#ifdef CORE_WIN_POLICY
    if (x->os == CORE_OS_WIN && !strcmp(id, "restore")) {   /* R1's check offers it again by itself: no dialog (review C3) */
        LOG(x, "the last run quit during: ", label, " (restore): R1's check offers it again");
        fx_do(x, CORE_STEP_BUSY_MARKER, 0);
        return 0;
    }
#endif
    LOG(x, "the last run quit during: ", label, " (", id, ")");
    core_dialog d;
    dlg_init(&d, T_ACTION_INTERRUPTED, TX(x, T_ACTION_INTERRUPTED), TX1(x, T_ACTION_INTERRUPTED_MSG, SL(label, label)), 0);
    dlg_button(x, &d, TX(x, T_CONTINUE));
    dlg_button(x, &d, TX(x, T_CANCEL));
    int b = ask_d(x, &d);
    LOG(x, "  user chose: ", chose(&d, b));
    /* the marker stays until the action starts or the user says no; while rekordbox is updating
     * it stays too, and the next launch offers the action again */
    if (b != 0) {
        fx_do(x, CORE_STEP_BUSY_MARKER, 0);
        return 0;
    }
    int ready = ready_for_queued_action(x);
    if (ready <= 0) {   /* a failed check (-1) keeps the marker: the next launch offers it again */
        int updating;
        if (ready == 0 && RB_UPDATING(x, &updating) >= 0 && !updating) fx_do(x, CORE_STEP_BUSY_MARKER, 0);
        return 0;
    }
    if (!strcmp(id, "install")) *pending = start_install(id, label, 1, 1);   /* 1.0's: both */
    else if (!strcmp(id, "install-plus")) *pending = start_install(id, label, 1, 0);
    else if (!strcmp(id, "install-cache")) *pending = start_install(id, label, 0, 1);
    else if (!strcmp(id, "reinstall")) *pending = reinstall_chosen(x);
    else if (!strcmp(id, "reinstall-plus")) *pending = start_reinstall_one(id, label, 1);
    else if (!strcmp(id, "reinstall-cache")) *pending = start_reinstall_one(id, label, 0);
    else if (!strcmp(id, "reinstall-missing")) {
        if (!reinstall_missing(x, pending)) {
            fx_do(x, CORE_STEP_BUSY_MARKER, 0);
            return 0;
        }
    } else if (!strcmp(id, "update-cache")) *pending = start_cache_update(x);
    else if (!strcmp(id, "uninstall-plus")) *pending = simple_action(A_UNINSTALL_PLUS, id, label, T_UNINSTALLING);
    else if (!strcmp(id, "uninstall-cache")) *pending = simple_action(A_UNINSTALL_CACHE, id, label, T_RUN_UNINSTALLING_CACHE);
    else if (!strcmp(id, "uninstall-all")) *pending = simple_action(A_UNINSTALL_ALL, id, TX(x, T_BTN_UNINSTALL_ALL), T_UNINSTALLING_ALL);
    else {
        LOG(x, "  unknown action: not resumed");
        fx_do(x, CORE_STEP_BUSY_MARKER, 0);
        return 0;
    }
    pending->id = dup_s(x, pending->id);
    pending->label = dup_s(x, pending->label);
    return 1;
}

/* ---------------------------------------------------------------- the watcher's request (Actions.swift:612) */

static int check_request(fx *x, action *pending) {
    core_facts f;
    fx_step(x, CORE_STEP_REQUEST, &f, 0);
    int64_t age = fact_i64(&f, FACT_STEP_CODE, -1);
    if (age < 0 || !fx_ok(x)) return 0;
    if (age > 3600) {
        fx_do(x, CORE_STEP_REQUEST, 1, "take");
        LOG(x, "watcher's reinstall request is ", num(x, age / 60), " minutes old: ignored");
        return 0;
    }
    core_facts cf;
    if (!fx_facts(x, &cf)) return 0;
    int m = fact_flag(&cf, FACT_CHOSEN_MODEL) && !fact_flag(&cf, FACT_STEMS_PLUS_PRESENT);
    int k = fact_flag(&cf, FACT_CHOSEN_CACHE) && !fact_flag(&cf, FACT_STEMS_CACHE_PRESENT);
    if (!m && !k) {
        fx_do(x, CORE_STEP_REQUEST, 1, "take");
        LOG(x, "watcher asked to reinstall, but nothing is missing: ignored");
        return 0;
    }
    LOG(x, "watcher asked to reinstall");
    /* waiting, and its end reserved first: a lost "no longer waiting" would block every later
     * request (OOM sweep); without the reservation it isn't marked waiting at all */
    x->ends[FX_END_UNWAIT] = fx_reserve(x, EF_REQUEST_WAITING, 0);
    if (x->ends[FX_END_UNWAIT]) fx_effect(x, EF_REQUEST_WAITING, 1, "");
    int go = ready_for_queued_action(x);
    if (x->ends[FX_END_UNWAIT]) fx_post_reserved(x, &x->ends[FX_END_UNWAIT], EF_REQUEST_WAITING, 0);
    if (go >= 0) fx_do(x, CORE_STEP_REQUEST, 1, "take");   /* started now, or turned down: handled; a failed check leaves it */
    if (go <= 0) return 0;
    return reinstall_missing(x, pending);     /* what is missing now (rekordbox may have changed meanwhile) */
}

/* ---------------------------------------------------------------- the Stems Cache update (Actions.swift:635) */

/* the guards on main (startupDone, no action, no waiting request, not asked, no sheet) were
 * checked before the flow started; then the saved payload.json and the bridge in rekordbox */
static int offer_cache_update(fx *x, action *pending) {
    core_facts p, cf, b;
    fx_step(x, CORE_STEP_PAYLOAD, &p, 1, "saved");
    if (!fact_flag(&p, FACT_PAYLOAD_FOUND) || !fx_facts(x, &cf)) return 0;
    fx_step(x, CORE_STEP_BRIDGE_SHA, &b, 0);
    const char *installed = fact_str(&b, FACT_STEP_TEXT), *latest = fact_str(&p, FACT_PAYLOAD_BRIDGE_SHA);
    if (!cache_update_due(x, &cf, installed, latest, flows_version_newer(fact_str(&p, FACT_PAYLOAD_APP_VERSION), x->own))) return 0;
    char i12[13], l12[13];
    snprintf(i12, sizeof i12, "%s", installed[0] ? installed : "?");
    snprintf(l12, sizeof l12, "%s", latest);
    LOG(x, "Stems Cache: an update is available (in rekordbox ", i12, ", payload ", fact_str(&p, FACT_PAYLOAD_VERSION), " ", l12, ")");
    fx_effect(x, EF_CACHE_ASKED, 0, "");
    core_dialog d;
    dlg_init(&d, T_CACHE_UPDATE_TITLE, TX(x, T_CACHE_UPDATE_TITLE), TX(x, T_CACHE_UPDATE_MSG), 0);
    dlg_button(x, &d, TX(x, T_UPDATE));
    dlg_button(x, &d, TX(x, T_NOT_NOW));
    int r = ask_d(x, &d);
    LOG(x, "  user chose: ", chose(&d, r));
    if (r != 0 || ready_for_queued_action(x) <= 0) return 0;
    *pending = start_cache_update(x);
    return 1;
}

static int q_offer_guards(core *c, void *arg) {
    (void)arg;
    int open;
    CALLBACK(c, open = c->life->host.modal_open(c->life->host.ctx));
    return c->fl.startup_done && !c->action_running && !c->fl.request_waiting && !c->fl.cache_update_asked && !open;
}

/* ---------------------------------------------------------------- the update check and the update (Update.swift) */

typedef struct { char own[32]; } check_arg;

static void fetched_job(core *c, void *payload);

/* fetched_job's payload, reserved before the fetch (OOM sweep): a payload.json fetched can't be
 * lost on its way to main, with the update notice and the cache offer it brings. m: the notice,
 * NULL when it couldn't be made (the offer is still asked) */
typedef struct { effect_msg *m; } fetched_msg;
static void fetched_drop(void *payload) { free(((fetched_msg *)payload)->m); }

#ifdef CORE_TEST_HOOKS
void (*_Atomic flows_test_fetch_hold)(void *ctx);
#endif

/* The background download of payload.json (checkForUpdate): not the flow thread, actions mustn't
 * wait. It runs beside the flow: a core_free that comes after core_run's liveness check may meet
 * its one step on the way (core.h, core_free: "a running flow, or a worker job that has started,
 * carries on"); it makes no call after that. */
static void update_fetch_work(core_life *life, void *payload) {
    check_arg *a = payload;
    if (!life->host.step || (life->native & NATIVE_STEP)) return;
#ifdef CORE_TEST_HOOKS
    void (*h)(void *) = atomic_load(&flows_test_fetch_hold);
    if (h) h(life->host.ctx);
#endif
    fetched_msg *fm = calloc(1, sizeof *fm);
    core_job *done = fm ? job_reserve(life, fetched_job, fm, fetched_drop) : NULL;
    if (!done) {   /* out of memory before the fetch: none this run, as when the download fails */
        free(fm);
        return;
    }
    arena ar = {0};
    core_facts f;
    facts_init(&f, &ar);
    static const char *const args[] = {"fetch"};
    if (life->host.step(life->host.ctx, CORE_STEP_PAYLOAD, args, 1, &f) == 0 && f.set[FACT_PAYLOAD_FOUND] && f.i[FACT_PAYLOAD_FOUND]) {
        const char *v = f.set[FACT_PAYLOAD_APP_VERSION] && f.s[FACT_PAYLOAD_APP_VERSION] ? f.s[FACT_PAYLOAD_APP_VERSION] : "";
        const char *newer = flows_version_newer(v, a->own) ? v : "";
        size_t k = strlen(newer);
        effect_msg *m = malloc(sizeof *m + k + 1);
        if (m) {
            m->kind = EF_UPDATE_FOUND;
            m->n = 0;
            m->t1 = m->t2 = -1;
            memcpy(m->s, newer, k + 1);
        }
        fm->m = m;
        job_post(life, done);
        done = NULL;
    }
    core_job_drop(done);   /* nothing fetched: nothing to post */
    arena_free(&ar);
}

/* main: offerCacheUpdate() with its guards; a flow running now: once it ends (Q9) */
static void offer_cache_update_now(core *c) {
    if (c->flow_running) {
        c->fl.cache_offer_pending = 1;
        return;
    }
    c->fl.cache_offer_pending = 0;
    if (q_offer_guards(c, NULL)) start_flow(c, F_CACHE_OFFER);
}

/* main: updateFound(m), refresh(), offerCacheUpdate() */
static void fetched_job(core *c, void *payload) {
    fetched_msg *fm = payload;
    if (fm->m) {
        effect_job(c, fm->m);
        free(fm->m);
        fm->m = NULL;
    }
    offer_cache_update_now(c);
}

static void check_for_update(fx *x) {
    core_facts p;
    fx_step(x, CORE_STEP_PAYLOAD, &p, 1, "check");
    if (fact_flag(&p, FACT_PAYLOAD_FOUND)) {
        const char *v = fact_str(&p, FACT_PAYLOAD_APP_VERSION);
        fx_effect(x, EF_UPDATE_FOUND, 0, flows_version_newer(v, x->own) ? v : "");
    }
    if (!fact_flag(&p, FACT_STEP_FLAG) || !fx_ok(x)) return;   /* manifestDue */
    check_arg *a = calloc(1, sizeof *a);
    if (!a) return;
    memcpy(a->own, x->own, sizeof a->own);
    core_post_work(x->life, CORE_WORK_BACKGROUND, update_fetch_work, a, NULL);
}

/* updateApp (Update.swift:123) */
static void update_flow(fx *x) {
    core_facts p, u;
    fx_step(x, CORE_STEP_PAYLOAD, &p, 1, "saved");
    const char *v = fact_str(&p, FACT_PAYLOAD_APP_VERSION);
    if (!fact_flag(&p, FACT_PAYLOAD_FOUND) || !flows_version_newer(v, x->own)) return;
    v = dup_s(x, v);
    if (fx_step(x, CORE_STEP_UPDATE_CHECK, &u, 0)) {   /* not checked: never on to the download */
        LOG(x, "update check failed: stopped");
        return;
    }
    int64_t why = fact_i64(&u, FACT_STEP_CODE, 1);
    if (!fx_ok(x)) return;
    if (why == 1) {   /* only a valid app is replaced: not a translocated or bare copy */
        alert_ok(x, T_NOT_IN_APPLICATIONS, TX(x, T_NOT_IN_APPLICATIONS_MSG));
        return;
    }
    if (why == 2) {
        alert_ok(x, T_ADMIN_NEEDED, TX(x, T_ADMIN_NEEDED_MSG));
        return;
    }
    if (!confirm(x, TX1(x, T_UPDATE_TO, SL(version, v)), TX(x, T_UPDATE), TX1(x, T_UPDATE_TO_MSG, SL(version, v)))) return;
    /* the update quits the app: a playlist being prepared is stopped first, before anything is downloaded */
    if (!confirm_stop_preparing(x, T_STOP_AND_UPDATE, "update")) return;
    fx_busy(x, TX(x, T_UPDATING_APP), 1);   /* no action marker: nothing in rekordbox changes */
    fx_effect(x, EF_DL_WHAT, 0, TX1(x, T_WHAT_APP, SL(version, v)));
    int32_t r = fx_do(x, CORE_STEP_UPDATE_SWAP, 0);   /* returns only when it failed */
    if (!fx_ok(x)) return;
    if (r == 0) {   /* the app quits: busy stays on to the end */
        atomic_store(&x->life->flow_quits, 1);
        return;
    }
    if (r == 1) alert_ok(x, T_DOWNLOAD_FAILED, TX(x, T_OFFLINE_MSG));
    else if (r == 2) alert_ok(x, T_LOW_DISK, TX(x, T_DISK_FULL_MSG));
    else alert_ok(x, T_DOWNLOAD_FAILED, TX(x, T_UPDATE_BAD_DOWNLOAD));
    fx_idle(x);
}

static void check_update_result(fx *x) {
    core_facts f;
    fx_step(x, CORE_STEP_UPDATE_RESULT, &f, 0);
    if (!fact_flag(&f, FACT_STEP_FLAG) || !fx_ok(x)) return;
    const char *line = fact_str(&f, FACT_STEP_TEXT);
    /* trimmed, as the log writes it */
    const char *b = line, *e = line + strlen(line);
    while (*b && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    char *t = arena_strdup(&x->a, b, (size_t)(e - b));
    LOG(x, "the update couldn't replace the app (", t ? t : "", ")");
    const char *msg;
    COMPOSE(x, msg, w_update_failed_message(B, update_failure_reason(line, strlen(line))));
    alert_ok(x, T_UPDATE_FAILED, msg);
}

/* ---------------------------------------------------------------- Create Report (Report.swift:14) */

/* the report's lights, as the window's refresh() shows them, from what REPORT_FACTS measured now
 * (reportFacts' plusLight and cacheLight): REPORT_ZIP's arguments */
typedef struct { const char *plus_look, *plus_tip, *cache_look, *cache_tip, *cache_is_location; } report_lights;
static report_lights w_report_lights(fx *x, const core_facts *m) {
    static const char *const looks[] = {"0", "1", "2"};
    report_lights l;
    int32_t pl = 0, cl = 0, has_loc = 0;
    COMPOSE(x, l.plus_tip, pl = light_plus(B, fact_flag(m, FACT_STEMS_PLUS_PRESENT)));
    const char *loc = NULL;
    if (fact_flag(m, FACT_LOC_MEASURED)) {   /* locationLight of the snapshot measured now */
        loc_status st = {(int32_t)fact_i64(m, FACT_LOC_KIND, LOC_THIS_MAC), (int32_t)fact_i64(m, FACT_LOC_PROBE, PROBE_AVAILABLE),
                         fact_str_or_null(m, FACT_LOC_STATUS_DRIVE)};
        loc_bridge br = {fact_str_or_null(m, FACT_LOC_BRIDGE_STATE), fact_str_or_null(m, FACT_LOC_BRIDGE_REASON),
                         fact_str_or_null(m, FACT_LOC_BRIDGE_DRIVE)};
        const char *t;
        COMPOSE(x, t, has_loc = location_light(B, &st, br.state ? &br : NULL, fact_flag(m, FACT_REKORDBOX_RUNNING)));
        if (has_loc) loc = t;
    }
    int cache = fact_flag(m, FACT_STEMS_CACHE_PRESENT);
    const char *models = fact_str_or_null(m, FACT_BRIDGE_MODELS), *list[64];
    int32_t n = 0;
    char *copy = models ? dup_s(x, models) : NULL;
    for (char *s = copy, *e; s && *s && n < 64; s = e) {
        e = strchr(s, '\n');
        if (e) *e++ = 0; else e = s + strlen(s);
        if (*s) list[n++] = s;
    }
    const char *sha = fact_str(m, FACT_STEP_TEXT);
    int64_t floor = fact_i64(m, FACT_LOC_PAUSED_FLOOR, -1);
    cache_light_in in = {
        .installed = cache, .plus_on = fact_flag(m, FACT_STEMS_PLUS_PRESENT), .chosen = fact_flag(m, FACT_CHOSEN_CACHE),
        .rekordbox_model = fact_flag(m, FACT_CONFIG_RB_MODEL), .has_bridge_models = models != NULL, .bridge_models = list,
        .n_bridge_models = n, .model = *sha ? sha : NULL, .engine = fact_str(m, FACT_ENGINE),
        .has_paused_floor = cache && fact_flag(m, FACT_CONFIG_ENABLED) && floor >= 0, .paused_floor = floor,
        .location = loc, .drive = fact_str_or_null(m, FACT_LOC_DRIVE)};
    COMPOSE_FIT(x, l.cache_tip, cl = light_cache(B, &in));   /* the engine's text has no bound */
    if (!l.cache_tip) l.cache_tip = "";
    l.plus_look = looks[pl >= 0 && pl <= 2 ? pl : 2];
    l.cache_look = looks[cl >= 0 && cl <= 2 ? cl : 2];
    l.cache_is_location = loc && !strcmp(loc, l.cache_tip) ? "1" : "0";
    return l;
}

static void report_flow(fx *x) {
    fx_busy(x, TX(x, T_CREATING_REPORT), 1);   /* no action: no busy marker */
    core_facts m, r;
    int32_t rc = fx_step(x, CORE_STEP_REPORT_FACTS, &m, 0);
    if (rc == 0) {
        report_lights l = w_report_lights(x, &m);
        rc = fx_step(x, CORE_STEP_REPORT_ZIP, &r, 5, l.plus_look, l.plus_tip, l.cache_look, l.cache_tip, l.cache_is_location);
    }
    fx_idle(x);
    if (!fx_ok(x)) return;
    if (rc != 0) {
        alert_ok(x, T_REPORT_FAILED, TX(x, T_REPORT_FAILED_MSG));
        return;
    }
    const char *shown = fact_str(&r, FACT_STEP_TEXT);
    LOG(x, "report: ", shown);
    int b = ASK(x, T_REPORT_SAVED, TX1(x, T_REPORT_SAVED_MSG, SL(path, shown)), 2, TX(x, T_OPEN_ISSUE), TX(x, T_DONE));
    if (b != 0) return;
    fx_do(x, CORE_STEP_REVEAL, 1, fact_str(&r, FACT_STEP_PATH));   /* to drag into the issue */
    if (fact_str(&r, FACT_STEP_URL)[0]) fx_do(x, CORE_STEP_OPEN_URL, 1, fact_str(&r, FACT_STEP_URL));
}

/* ---------------------------------------------------------------- start-up (Controller.swift:152-176) */

/* retryThisMacDeletion (LocationActions.swift:208): This Mac's stems left after a switch to a
 * drive, deleted at launch under 3.3.3's conditions (F3: the host gives the facts, the core decides) */
static void this_mac_retry(fx *x) {
    core_facts f;
    if (fx_step(x, CORE_STEP_THIS_MAC_DELETE, &f, 1, "facts") != 0) return;
    int flag = fact_flag(&f, FACT_STEP_FLAG), bridge_location = fact_flag(&f, FACT_STEP_FLAG2);
    int available = fact_i64(&f, FACT_STEP_CODE, 0) == 1;
    const char *code = f.set[FACT_STEP_TEXT] ? fact_str(&f, FACT_STEP_TEXT) : "?";
    if (!flag && this_mac_needs_cleanup(available, bridge_location, fact_i64(&f, FACT_THIS_MAC_MODEL_FOLDERS, 0) != 0)) {
        fx_do(x, CORE_STEP_THIS_MAC_DELETE, 1, "defer");
        LOG(x, "stems an older Stems Cache saved on this Mac while a drive was chosen: deleting them");
        flag = 1;
    }
    if (!flag) return;
    int open = RB_UPDATING(x, NULL);   /* -1: couldn't be checked, counted as running (Q3) */
    if (!may_retry_this_mac_deletion(1, available, bridge_location, open != 0)) {
        LOG(x, "the stems left on this Mac aren't deleted yet: not now (", code, ")");
        return;
    }
    int ok = fx_do(x, CORE_STEP_THIS_MAC_DELETE, 1, "delete") == 0;
    if (ok) fx_do(x, CORE_STEP_THIS_MAC_DELETE, 1, "unflag");
    LOG(x, "the stems left on this Mac deleted: ", ok ? "ok" : "FAILED (tried again at the next start)");
}

/* startupChecks' work block (Controller.swift:157-176), in its order, before anything is resumed */
static void startup_work(fx *x) {
    core_facts wf;   /* the watcher registered for this app's code, before a watched folder changes */
    if (fx_step(x, CORE_STEP_WATCHER_CHECK, &wf, 0) == 0) fx_effect(x, EF_WATCHER_OFF, fact_flag(&wf, FACT_STEP_FLAG), "");
    core_facts m;
    if (fx_step(x, CORE_STEP_MIGRATE_LEGACY, &m, 0) == 0 && fact_flag(&m, FACT_STEP_FLAG)) fx_effect(x, EF_MODEL_NOTICE, 0, "");
    fx_do(x, CORE_STEP_REMOVE_LEFTOVERS, 0);
    this_mac_retry(x);
    /* an account that chose Stems Cache before it saved rekordbox's own model's stems: on for it too */
    core_facts cf, s;
    if (fx_facts(x, &cf) && fact_flag(&cf, FACT_CHOSEN_CACHE) && fx_step(x, CORE_STEP_CONFIG_SETS, &s, 1, "rekordbox_model") == 0 &&
        !fact_flag(&s, FACT_STEP_FLAG)) {
        int ok = fx_do(x, CORE_STEP_CONFIG_WRITE, 2, "rekordbox_model", "1") == 0;
        LOG(x, "Stems Cache: turned on for rekordbox's own stems model too (rekordbox_model=1): ", ok ? "ok" : "FAILED");
    }
    log_state(x, "state: ");
    fx_do(x, CORE_STEP_SETTINGS, 1, "open");   /* readConfig(log): says which config.ini lines are ignored */
}

static void startup_flow(fx *x) {
    /* the start-up's end reserved first (OOM sweep), the fast path: it marks the start-up done
     * before the flow's own cache-offer query. The backstop is the flow's end on main
     * (flows_flow_end_busy, OM1), so even with this and its fallback post lost the watcher's
     * requests and the cache offer are asked this run */
    x->ends[FX_END_STARTUP] = fx_reserve(x, EF_STARTUP_DONE, 0);
    startup_work(x);
    core_facts w;
    /* the first launch's welcome, never again after */
    fx_step(x, CORE_STEP_WELCOME, &w, 1, "check");
    if (!fact_flag(&w, FACT_STEP_FLAG) && fx_ok(x)) {
        (void)ASK(x, T_WELCOME_TITLE, TX(x, T_WELCOME_MSG), 1, TX(x, T_CONTINUE));
        fx_do(x, CORE_STEP_WELCOME, 1, "done");
    }
    check_update_result(x);
    action pending;
    int have = check_resume(x, &pending);
    if (have) {
        fx_do(x, CORE_STEP_REQUEST, 1, "take");   /* the resumed action covers it */
        action_start(x, &pending);                 /* start() is synchronous: the busy state is on before the rest */
    }
    fx_post_reserved(x, &x->ends[FX_END_STARTUP], EF_STARTUP_DONE, 0);
    if (!have && check_request(x, &pending)) {
        have = 1;
        action_start(x, &pending);
    }
    if (!have) {   /* offerCacheUpdate: skipped while an action runs */
        if (fx_ok(x) && flow_query(x->life, q_offer_guards, NULL, 0) && offer_cache_update(x, &pending)) {
            have = 1;
            action_start(x, &pending);
        }
    }
    check_for_update(x);
    fx_effect(x, EF_REFRESH, 0, "");   /* the update notice, if any */
    if (have) action_body(x, &pending);   /* the action's body, on the work queue after the start-up block */
}

/* ---------------------------------------------------------------- Settings (Settings.swift, LocationActions.swift) */

/* trimmingCharacters(in: .whitespaces): spaces and tabs, and Unicode's other space separators */
static size_t space_at(const char *p) {
    unsigned char c = (unsigned char)p[0];
    if (c == ' ' || c == '\t') return 1;
    if (c == 0xC2 && (unsigned char)p[1] == 0xA0) return 2;                                      /* U+00A0 */
    if (c == 0xE1 && (unsigned char)p[1] == 0x9A && (unsigned char)p[2] == 0x80) return 3;       /* U+1680 */
    if (c == 0xE2 && (unsigned char)p[1] == 0x80 && (((unsigned char)p[2] >= 0x80 && (unsigned char)p[2] <= 0x8A) || (unsigned char)p[2] == 0xAF)) return 3;
    if (c == 0xE2 && (unsigned char)p[1] == 0x81 && (unsigned char)p[2] == 0x9F) return 3;       /* U+205F */
    if (c == 0xE3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x80) return 3;       /* U+3000 */
    return 0;
}

static const char *trim_spaces(fx *x, const char *s) {
    size_t k;
    while ((k = space_at(s))) s += k;
    size_t n = strlen(s), end = 0;
    for (size_t i = 0; i < n;) {
        k = space_at(s + i);
        if (k) i += k;
        else {
            i++;
            end = i;
        }
    }
    char *p = arena_strdup(&x->a, s, end);
    return p ? p : "";
}

/* Double(text with "," as a decimal mark too) in [lo, hi]; out: the text as written to config.ini */
static int parse_in_range(fx *x, const char *s, double lo, double hi, const char **out) {
    char *t = dup_s(x, s);
    for (char *p = t; *p; p++) if (*p == ',') *p = '.';
    *out = t;
    if (!*t || isspace((unsigned char)*t)) return 0;
    char *end;
    double v = strtod(t, &end);   /* the C locale: a point */
    if (*end) return 0;
    return v >= lo && v <= hi;
}

static void remeasure(fx *x) {
    fx_do(x, CORE_STEP_LOCATION_REMEASURE, 0);   /* a host's own monitor */
    fx_effect(x, EF_REMEASURE, 0, "");           /* the core's memo (M4) */
}

/* memo: the memo snapshot the step was asked with (its kind 4 belongs to that one), NULL: read now */
static int64_t size_kind_of(fx *x, const core_facts *f, core_fact kind_f, core_fact bytes_f, int64_t *bytes, const view_loc_now *memo) {
    int64_t kind = fact_i64(f, kind_f, -1);
    *bytes = fact_i64(f, bytes_f, 0);
    if (kind != 4) return kind;
    view_loc_now n;
    if (!memo) {
        fx_loc(x, &n);
        memo = &n;
    }
    *bytes = memo->size_bytes;
    return memo->known ? memo->size_kind : -1;
}

/* clearLocation (LocationActions.swift:194): in the background, the window showing how far it is */
static void clear_location(fx *x, const char *dir, const char *id, const char *shown, const char *drive) {
#ifdef CORE_WIN_POLICY
    if (fx_nameless(x, drive)) {   /* the nameless ids are stored: the progress then passes only {n} (review NT1) */
        fx_busy(x, TX(x, nameless_text(T_CLEARING_DRIVE)), 1);
        fx_effect3(x, EF_DELETE_TEXTS, 0, drive, nameless_text(T_CLEARING_DRIVE), nameless_text(T_CLEARING_DRIVE_N));
    } else {
#endif
    fx_busy(x, TX1(x, T_CLEARING_DRIVE, SL(drive, drive)), 1);
    fx_effect3(x, EF_DELETE_TEXTS, 0, drive, T_CLEARING_DRIVE, T_CLEARING_DRIVE_N);
#ifdef CORE_WIN_POLICY
    }
#endif
    core_facts r;
    int32_t rc = fx_step(x, CORE_STEP_REMOVE_LOCATION, &r, 3, dir, id, "0");
    const char *why = rc ? removal_why(x, &r) : "";
    if (rc == 0) LOG(x, "saved stems on ", shown, " cleared: ok");
    else LOG(x, "saved stems on ", shown, " cleared: FAILED (", why, ")");
    fx_idle(x);
    fx_effect(x, EF_PREPARE_CHANGED, 0, "");
    remeasure(x);
#ifdef CORE_WIN_POLICY
    if (rc != 0 && fx_nameless(x, drive)) alert_ok(x, T_NOT_CLEARED, TX1(x, nameless_text(T_NOT_CLEARED_DRIVE_MSG), SL(why, why)));
    else
#endif
    if (rc != 0) alert_ok(x, T_NOT_CLEARED, TX2(x, T_NOT_CLEARED_DRIVE_MSG, SL(drive, drive), SL(why, why)));
}

static int64_t location_size(fx *x, const char *dir) {
    core_facts f;
    fx_step(x, CORE_STEP_LOCATION_SIZE, &f, 1, dir);
    return fact_i64(&f, FACT_STEP_CODE, 0);
}

/* commitLocation (LocationActions.swift:144) */
static void commit_location(fx *x, const char *dir, const char *id, const char *mount, int created, int delete_this_mac) {
    core_facts w;
    if (fx_step(x, CORE_STEP_LOCATION_WRITE, &w, 4, dir, id, mount, created ? "1" : "0") != 0) {
        if (!fx_ok(x)) return;
        LOG(x, "settings: couldn't write ", fact_str(&w, FACT_STEP_PATH));
        alert_ok(x, T_SETTINGS_NOT_SAVED, TX(x, T_SETTINGS_NOT_SAVED_MSG));
        return;
    }
    fx_effect(x, EF_PREPARE_CHANGED, 0, "");
    LOG(x, "settings: stems are saved in ", fact_str(&w, FACT_STEP_TEXT), " from rekordbox's next start", created ? " (a new folder)" : "");
    if (delete_this_mac) {
        core_facts c;
        fx_step(x, CORE_STEP_THIS_MAC_DELETE, &c, 1, "check");
        if (fact_flag(&c, FACT_STEP_FLAG)) {
            int was = bg_begin(x, T_DELETING_THIS_MAC);
            int ok = fx_do(x, CORE_STEP_THIS_MAC_DELETE, 1, "delete") == 0;
            bg_end(x, was);
            LOG(x, "settings: the stems saved on this Mac deleted: ", ok ? "ok" : "FAILED (tried again at the next start)");
        } else {
            fx_do(x, CORE_STEP_THIS_MAC_DELETE, 1, "defer");
            LOG(x, "settings: the stems saved on this Mac stay for now: the installed Stems Cache is older and saves there (deleted once it's updated)");
        }
    }
    remeasure(x);
    fx_effect(x, EF_REFRESH, 0, "");
}

static const char *const refusal_names[] = {"", "internalDisk", "network", "readOnly", "timeMachine", "insideOurFolder", "link", "fileSystem", "path", "unreadable"};

/* changeLocation (LocationActions.swift:76) */
static void change_location(fx *x) {
    if (!preflight(x)) return;
    if (!x->life->host.pick_folder || !fx_ok(x) || x->changed) return;
    core_reply pr;
    reply_init(&pr, &x->a);
    atomic_store(&x->life->asked, 1);
    int32_t rc = x->life->host.pick_folder(x->life->host.ctx, "/Volumes", CORE_NTS, &pr);
    if (!fx_revalidate(x) || rc != 0 || !pr.fields[0]) return;
    const char *folder = pr.fields[0];
    core_facts pf;
    int was = bg_begin(x, T_CHECKING_FOLDER);
    fx_step(x, CORE_STEP_LOCATION_PROBE, &pf, 1, folder);
    bg_end(x, was);
    int64_t why = fact_i64(&pf, FACT_STEP_CODE, 9);
    const char *drive = fact_str(&pf, FACT_LOC_DRIVE);
    if (!fx_ok(x)) return;
#ifdef CORE_WIN_POLICY
    /* neither a label nor a letter (owner, 2026-11-15; review NT1): one refusal, also for the
     * probe's refusals that would name the drive (3 read-only, 7 file system; review N21) */
    if ((why == 0 || why == 3 || why == 7) && location_refuse_nameless(x->os, drive)) {
        LOG(x, "settings: ", fact_str(&pf, FACT_STEP_TEXT2), " refused (nameless)");
        alert_ok(x, T_CANT_SAVE_THERE, TX(x, T_WIN_REFUSE_NAMELESS));
        return;
    }
#endif
    if (why != 0) {
        if (why < 1 || why > 9) why = 9;
        LOG(x, "settings: ", fact_str(&pf, FACT_STEP_TEXT2), " refused (", refusal_names[why], ")");
        const char *msg;
        COMPOSE(x, msg, w_location_refusal(B, (int32_t)why - 1, drive));
        alert_ok(x, T_CANT_SAVE_THERE, msg);
        return;
    }
    const char *real = fact_str(&pf, FACT_STEP_TEXT), *mount = fact_str(&pf, FACT_STEP_PATH);
    core_facts df;
    was = bg_begin_s(x, TX1(x, T_LOOKING_FOR_STEMS, SL(drive, drive)));
    fx_step(x, CORE_STEP_LOCATION_DISCOVER, &df, 2, real, mount);
    bg_end(x, was);
    if (!fx_ok(x)) return;
    int64_t found = fact_i64(&df, FACT_STEP_CODE, 0);
    const char *dir = fact_str(&df, FACT_LOC_DIR), *id = fact_str(&df, FACT_LOC_GATE_ID);
    const char *current = fact_str(&df, FACT_STEP_TEXT), *current_id = fact_str(&df, FACT_STEP_TEXT2);
    const char *old_drive = current[0] ? fact_str(&df, FACT_LOC_DRIVE) : NULL;
    if (found == 1) {
        alert_ok(x, T_CANT_SAVE_THERE, TX(x, T_NOT_OURS_MSG));
        return;
    }
    if (found == 2 && current[0] && !strcmp(dir, current) && same_id(id, current_id)) {
        LOG(x, "settings: ", fact_str(&df, FACT_STEP_PATH), " is already where stems are saved");
        return;
    }
    if (found == 2 && current_id[0] && same_id(id, current_id)) {
        /* the same location, renamed or mounted elsewhere: only cache_dir changes */
        was = bg_begin(x, T_MEASURING);
        int64_t size = location_size(x, dir);
        bg_end(x, was);
        if (!guard_snap(x, NULL)) return;   /* F2: the text describes what the action then commits */
        const char *msg;
        if (size > 0) {
            const char *words;
            COMPOSE(x, words, w_size_words(B, size));
            msg = TX2(x, T_FOUND_SIZE_MSG, SL(drive, drive), SL(size, words));
        } else msg = TX1(x, T_FOUND_MSG, SL(drive, drive));
        guard_arm(x);
        int b = ASK(x, T_FOUND_TITLE, msg, 2, TX(x, T_USE_THEM), TX(x, T_CANCEL));
        if (disarm_guard(x, b) != 0 || rekordbox_closed(x) <= 0) return;
        commit_location(x, dir, id, mount, 0, 0);
        return;
    }
    const char *use = TX1(x, T_USE_DRIVE, SL(drive, drive)), *use_title = TX1(x, T_USE_DRIVE_Q, SL(drive, drive));
    const char *msg;
    if (found == 2) {
        core_facts wf;
        was = bg_begin(x, T_CHECKING_FOLDER);
        fx_step(x, CORE_STEP_LOCATION_WRITABLE, &wf, 1, dir);
        bg_end(x, was);
        if (!fact_flag(&wf, FACT_STEP_FLAG)) {
            if (fx_ok(x)) alert_ok(x, T_CANT_SAVE_THERE, TX(x, T_OTHER_OWNER_MSG));
            return;
        }
        was = bg_begin(x, T_MEASURING);
        int64_t s0 = location_size(x, dir), s1 = location_size(x, "this_mac");
        bg_end(x, was);
        if (!guard_snap(x, NULL)) return;
        COMPOSE(x, msg, w_switch_message(B, drive, 1, s0, current[0] ? 0 : s1, old_drive));
        guard_arm(x);
        int b = ask_t(x, T_USE_DRIVE_Q, use_title, msg, 2, use, TX(x, T_CANCEL));
        if (disarm_guard(x, b) != 0 || rekordbox_closed(x) <= 0) return;
        commit_location(x, dir, id, mount, 0, !current[0]);
        return;
    }
    int64_t this_mac = 0;
    if (!current[0]) {
        was = bg_begin(x, T_MEASURING);
        this_mac = location_size(x, "this_mac");
        bg_end(x, was);
    }
    if (!guard_snap(x, NULL)) return;
    COMPOSE(x, msg, w_switch_message(B, drive, 0, 0, this_mac, old_drive));
    guard_arm(x);
    int b = ask_t(x, T_USE_DRIVE_Q, use_title, msg, 2, use, TX(x, T_CANCEL));
    if (disarm_guard(x, b) != 0 || rekordbox_closed(x) <= 0) return;
    /* nothing is created before the user confirms */
    core_facts mf;
    if (fx_step(x, CORE_STEP_LOCATION_CREATE, &mf, 1, real) != 0) {
        if (!fx_ok(x)) return;
        int64_t code = fact_i64(&mf, FACT_STEP_CODE, 2);
        const char *err = fact_str(&mf, FACT_STEP_TEXT);
        const char *e = code == 1 ? "exists" : code == 3 ? TX1(x, T_CREATE_R_MARKER, SL(why, err)) : err[0] ? err : "?";
        LOG(x, "settings: couldn't create our folder in ", fact_str(&mf, FACT_STEP_PATH), ": ", e);
        alert_ok(x, T_CANT_SAVE_THERE, code == 1 ? TX(x, T_APPEARED_MSG) : TX2(x, T_CANT_CREATE_MSG, SL(drive, drive), SL(why, e)));
        return;
    }
    commit_location(x, dup_s(x, fact_str(&mf, FACT_LOC_DIR)), dup_s(x, fact_str(&mf, FACT_LOC_GATE_ID)), mount, 1, !current[0]);
}

/* useThisMac (LocationActions.swift:173) */
static void use_this_mac(fx *x) {
    if (!preflight(x)) return;
    core_facts st;
    fx_step(x, CORE_STEP_SETTINGS, &st, 1, "open");   /* readConfigState(log) */
    const char *old_drive = fact_str(&st, FACT_STEP_TEXT), *msg;
    const char *od = old_drive[0] ? old_drive : NULL;
#ifdef CORE_WIN_POLICY
    if (x->os == CORE_OS_WIN && fact_i64(&st, FACT_SET_LOCATION, 0) == 1) od = old_drive;   /* a chosen drive with no name: its nameless sentence (review NT4) */
#endif
    if (!guard_snap(x, NULL)) return;
    COMPOSE(x, msg, w_use_this_mac_message(B, od));
    guard_arm(x);
    int b = ASK(x, T_USE_THIS_MAC_Q, msg, 2, TX(x, T_SETTINGS_USE_THIS_MAC), TX(x, T_CANCEL));
    if (disarm_guard(x, b) != 0 || rekordbox_closed(x) <= 0) return;
    core_facts w;
    if (fx_step(x, CORE_STEP_LOCATION_WRITE, &w, 4, "", "", "", "0") != 0) {
        if (fx_ok(x)) alert_ok(x, T_SETTINGS_NOT_SAVED, TX(x, T_SETTINGS_NOT_SAVED_MSG));
        return;
    }
    LOG(x, "settings: stems are saved on this Mac from rekordbox's next start");
    fx_effect(x, EF_PREPARE_CHANGED, 0, "");
    remeasure(x);
    fx_effect(x, EF_REFRESH, 0, "");
}

/* locationLabel (LocationActions.swift:16) */
static const char *location_label(fx *x, const core_facts *s) {
    switch (fact_i64(s, FACT_SET_LOCATION, 0)) {
    case 1: {   /* ([drive] + below).joined(separator: " › ") */
        const char *path = fact_str(s, FACT_STEP_TEXT), *sep = TX(x, T_LOCATION_SEP);
        for (const char *p = fact_str(s, FACT_SET_LABEL_PATH); *p;) {
            size_t n = 0;
            while (p[n] && p[n] != '/' && !(x->os == CORE_OS_WIN && p[n] == '\\')) n++;
            if (n) path = CAT(x, path, sep, arena_strdup(&x->a, p, n));
            p += n + (p[n] != 0);
        }
#ifdef CORE_WIN_POLICY
        /* a chosen drive with no name: "Stems Cache's drive", then " › " and the folders below (review NT2) */
        if (x->os == CORE_OS_WIN && !fact_str(s, FACT_STEP_TEXT)[0]) return TX1(x, nameless_text(T_LOCATION_LABEL), SL(path, path));
#endif
        return TX1(x, T_LOCATION_LABEL, SL(path, path));
    }
    case 2: return TX(x, T_LOCATION_UNREADABLE);
    case 3: return TX(x, T_LOCATION_MALFORMED);
    default: return TX(x, T_LOCATION_THIS_MAC);
    }
}

static void settings_flow(fx *x) {
    core_facts s;
    fx_step(x, CORE_STEP_SETTINGS, &s, 1, "open");   /* readConfig(log): says which config.ini lines are ignored */
    const char *gb = dup_s(x, fact_str(&s, FACT_SET_GB)), *days = dup_s(x, fact_str(&s, FACT_SET_DAYS));
    for (int round = 0; round < 1000 && fx_ok(x) && !x->changed; round++) {
        if (round) fx_step(x, CORE_STEP_SETTINGS, &s, 1, "again");
        int unreadable = fact_flag(&s, FACT_SET_UNREADABLE), can_choose = fact_flag(&s, FACT_SET_CAN_CHOOSE);
        int64_t target = fact_i64(&s, FACT_SET_CLEAR, 0);
        int64_t bytes, kind = size_kind_of(x, &s, FACT_SET_SIZE_KIND, FACT_SET_SIZE, &bytes, NULL);
        const char *size = stems_size(x, kind, bytes);
        const char *message = TX(x, T_SETTINGS_MSG);
        if (unreadable) message = TX(x, T_SETTINGS_UNREADABLE);
        else if (!can_choose) message = CAT(x, message, TX(x, T_SETTINGS_REINSTALL_TO_CHOOSE));
        const char *clear = TX1(x, T_SETTINGS_CLEAR, SL(size, size));
        core_dialog d;
        dlg_init(&d, T_SETTINGS_TITLE, TX(x, T_SETTINGS_TITLE), message, 0);
        dlg_button(x, &d, TX(x, T_SAVE));
        dlg_button(x, &d, clear);
        dlg_button(x, &d, TX(x, T_CANCEL));
        d.nfields = 2;
        d.field_label[0] = TX(x, T_SETTINGS_SIZE_FIELD);
        d.field_value[0] = gb;
        d.field_label[1] = TX(x, T_SETTINGS_DAYS_FIELD);
        d.field_value[1] = days;
        d.nrows = 1;
        d.row_label[0] = location_label(x, &s);
        d.row_nbuttons[0] = 2;
        d.row_buttons[0][0] = TX(x, T_SETTINGS_CHANGE);
        d.row_buttons[0][1] = TX(x, T_SETTINGS_USE_THIS_MAC);
        dlg_settings_enable(&d, !unreadable, target == 1 || target == 3, can_choose, fact_i64(&s, FACT_SET_LOCATION, 0) == 0);
        const char *gate_dir = dup_s(x, fact_str(&s, FACT_LOC_DIR)), *gate_id = dup_s(x, fact_str(&s, FACT_LOC_GATE_ID));
        const char *clear_drive = dup_s(x, fact_str(&s, FACT_LOC_DRIVE)), *gate_shown = dup_s(x, fact_str(&s, FACT_STEP_PATH));
        core_reply r;
        if (fx_present(x, &d, &r)) return;
        gb = trim_spaces(x, r.fields[0] ? r.fields[0] : gb);
        days = trim_spaces(x, r.fields[1] ? r.fields[1] : days);
        if (r.row == 0 && r.row_button == 0) {   /* Change…: back to the settings, with the fields as they were */
            change_location(x);
            if (x->changed) return;
            continue;
        }
        if (r.row == 0 && r.row_button == 1) {
            use_this_mac(x);
            if (x->changed) return;
            continue;
        }
        if (r.button == 0) {   /* Save */
            const char *g, *dd;
            if (!parse_in_range(x, gb, 0.1, 100000.0, &g)) {
                alert_ok(x, T_SIZE_OUT_OF_RANGE, TX(x, T_SIZE_OUT_OF_RANGE_MSG));
                continue;
            }
            if (!parse_in_range(x, days, 1.0, 36500.0, &dd)) {
                alert_ok(x, T_DAYS_OUT_OF_RANGE, TX(x, T_DAYS_OUT_OF_RANGE_MSG));
                continue;
            }
            core_facts w;
            if (fx_step(x, CORE_STEP_CONFIG_WRITE, &w, 4, "max_gb", g, "max_days", dd) == 0) {
                LOG(x, "settings: ", fact_str(&w, FACT_STEP_TEXT));
                fx_effect(x, EF_PREPARE_CHANGED, 0, "");
            } else if (fx_ok(x)) {
                LOG(x, "settings: couldn't write ", fact_str(&w, FACT_STEP_PATH));
                alert_ok(x, T_SETTINGS_NOT_SAVED, TX(x, T_SETTINGS_NOT_SAVED_MSG));
            }
            return;
        }
        if (r.button == 1) {   /* Clear Saved Stems */
            const char *label = TX1(x, T_CLEAR_Q, SL(size, size));
            if (target == 3) {
                const char *cm;
                COMPOSE(x, cm, w_clear_location_message(B, clear_drive));
                if (!confirm(x, label, TX(x, T_CLEAR), cm)) {
                    if (x->changed) return;
                    continue;
                }
                if (!preflight(x)) continue;   /* the bridge works in that folder while rekordbox runs */
                clear_location(x, gate_dir, gate_id, gate_shown, clear_drive);
                return;                         /* it runs in the background; the window shows it */
            }
            if (!confirm(x, label, TX(x, T_CLEAR), TX(x, T_CLEAR_THIS_MAC_MSG))) {
                if (x->changed) return;
                continue;
            }
            if (!preflight(x)) continue;
            core_facts cr;
            int ok = fx_step(x, CORE_STEP_CLEAR_THIS_MAC, &cr, 0) == 0;
            int link = fact_flag(&cr, FACT_STEP_FLAG);
            LOG(x, "saved stems cleared", size, ": ", ok ? "ok" : link ? "FAILED (the folder is a link)" : "FAILED");
            if (!ok) alert_ok(x, T_NOT_CLEARED, TX(x, link ? T_CACHE_LINK : T_NOT_CLEARED_MSG));
            fx_effect(x, EF_PREPARE_CHANGED, 0, "");
            remeasure(x);
            continue;   /* back to the settings, now without stems */
        }
        return;   /* Cancel */
    }
}

/* ---------------------------------------------------------------- the watcher's prompt (Watcher.swift:247) */

int flows_watcher_prompt(core_life *life, int model_off, int cache_off, const char *engine, const char *rekordbox,
                         int engine_ok, int rekordbox_ok, int plus_works, int *dont_ask) {
    fx x;
    memset(&x, 0, sizeof x);
    x.life = life;
    x.os = life->os;
    const char *short_rb, *msg;
    COMPOSE(&x, short_rb, w_short_version(B, rekordbox ? rekordbox : ""));
    watcher_dialog wd;
    COMPOSE(&x, msg, watcher_dialog_build(B, &wd, model_off, cache_off, engine ? engine : "", short_rb, engine_ok, rekordbox_ok, plus_works));
    core_dialog d;
    dlg_init(&d, wd.title, TX(&x, wd.title), msg, 1);
    for (int32_t i = 0; i < wd.nbuttons; i++) dlg_button(&x, &d, TX(&x, wd.buttons[i]));
    int b = ask_d(&x, &d);
    if (dont_ask) *dont_ask = b == d.nbuttons - 1;
    int r = wd.blocked ? -1 : b;   /* 0 Reinstall Now, 1 Remind Me Later, 2 Don't Ask Again; -1 none to start */
    fx_release(&x);
    return r;
}

/* ---------------------------------------------------------------- the watcher's rounds (Watcher.swift's runWatcher, watcherCheck) */

enum { WATCH_MAX_ROUNDS = 3 };   /* watcherMaxRounds */

typedef struct {
    fx *x;
    char put_off[2048];   /* the keys answered Remind Me Later in this process: one a line */
    char kept_pid[32];    /* the rekordbox session whose prepared playlists this process checked (keptCheckedPid) */
} watch_ctx;

static void w_log(void *p, const char *line) { lg(((watch_ctx *)p)->x, line); }

static int32_t w_flag(watch_ctx *w, core_step k, const char *arg) {
    core_facts f;
    if (fx_step(w->x, k, &f, arg ? 1 : 0, arg) != 0) return 0;
    return fact_flag(&f, FACT_STEP_FLAG);
}
static void w_release(void *p) { fx_do(((watch_ctx *)p)->x, CORE_STEP_WATCH_LOCK, 1, "release"); }
static int32_t w_again(void *p) { return w_flag(p, CORE_STEP_WATCH_LOCK, "again"); }
static int32_t w_retake(void *p) { return w_flag(p, CORE_STEP_WATCH_LOCK, "retake"); }

/* watcherSettle's: pioneerUpdating(), rekordboxOpen() (each its own look, as 1.3.0 calls them) */
static int32_t w_rekordbox(watch_ctx *w, core_fact which) {
    core_facts f;
    if (fx_step(w->x, CORE_STEP_REKORDBOX, &f, 0) != 0) return 0;   /* a failed look: neither (pgrep's own failure reads so too) */
    return fact_flag(&f, which);
}
static int32_t w_updating(void *p) { return w_rekordbox(p, FACT_PIONEER_UPDATING); }
static int32_t w_open(void *p) { return w_rekordbox(p, FACT_REKORDBOX_OPEN); }
static int32_t w_busy(void *p) { return w_flag(p, CORE_STEP_WATCH_BUSY, NULL); }
static void w_pause(void *p, int32_t seconds) {
    char n[16];
    snprintf(n, sizeof n, "%d", (int)seconds);
    fx_do(((watch_ctx *)p)->x, CORE_STEP_WATCH_PAUSE, 1, n);
}
/* halfUninstalled: the busy marker's action, when it is an uninstall (the next launch offers to
 * finish it, never to reinstall what it was removing) */
static int32_t w_half(void *p, char *id, size_t cap) {
    core_facts f;
    if (fx_step(((watch_ctx *)p)->x, CORE_STEP_WATCH_BUSY, &f, 0) != 0) return 0;
    const char *a = fact_str(&f, FACT_STEP_TEXT);
    if (strncmp(a, "uninstall", 9) != 0) return 0;
    snprintf(id, cap, "%s", a);
    return 1;
}
/* ---------------------------------------------------------------- the prepared playlists' check (Watcher.swift's checkKeptSets)
 *
 * As rekordbox opens: whether each prepared playlist is still ready (the helper's --quick estimate:
 * nothing is read where macOS would ask first). One that isn't gets a notification, never a dialog
 * over rekordbox. Once per rekordbox session; silent when nothing is prepared or preparing can't
 * work now (the app says why). The host reads and writes (WATCH_KEPT, WATCH_HELPER). */

/* the helper keeps at most 5 sets (helper/kept.h); 256 is headroom */
enum { KEPT_SETS_MAX = 256, KEPT_FILES_MAX = 100000, KEPT_HELPER_SETS = 5 };
/* kept.noticed as read: any size (1.3.0), at most what the writer makes for the helper's sets, and
 * only an object of arrays deep, so a malformed file can't build a big tree */
static const json_lim kept_lim = {SIZE_MAX, KEPT_FILES_MAX, 1 + KEPT_SETS_MAX + (size_t)KEPT_HELPER_SETS * KEPT_FILES_MAX, 2};

/* a step with up to 32 arguments from an array (the helper's) */
static int32_t fx_step_a(fx *x, core_step k, core_facts *out, const char *const *args, int32_t n) {
    facts_init(out, &x->a);
    if (!fx_ok(x)) return -1;
    if (!x->life->host.step || (x->life->native & NATIVE_STEP)) return CORE_E_STATE;
    return x->life->host.step(x->life->host.ctx, k, args, n, out);
}

/* runPrepareHelper(args, timeout:): what it printed and its error output ("" when it didn't start) */
typedef struct { const char *out, *err; } helper_out;
static helper_out w_helper(fx *x, const char *timeout, const char *const *args, int32_t n) {
    const char *a[40];
    int32_t k = 0;
    a[k++] = timeout;
    for (int32_t i = 0; i < n && k < 40; i++) a[k++] = args[i];
    core_facts r;
    helper_out o = {"", ""};
    if (fx_step_a(x, CORE_STEP_WATCH_HELPER, &r, a, k) != 0) return o;
    o.out = fact_str(&r, FACT_STEP_TEXT);
    o.err = fact_str(&r, FACT_STEP_TEXT2);
    return o;
}

/* each line of s (split at "\n", empty ones dropped, as Swift's split): fn(line, n, ctx) */
static void each_line(const char *s, void (*fn)(const char *, size_t, void *), void *ctx) {
    while (*s) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n) fn(s, n, ctx);
        s += n + (e ? 1 : 0);
    }
}

typedef struct { fx *x; prep_estimate e; int oom; } est_ctx;
static void est_line(const char *l, size_t n, void *p) {
    est_ctx *c = p;
    if (prep_estimate_line(&c->x->a, &c->e, l, n) < 0) c->oom = 1;
}
/* parseEstimate: 1 when it isn't nil */
static int w_parse_estimate(fx *x, const char *out, prep_estimate *e) {
    est_ctx c = {x, {0}, 0};
    prep_estimate_init(&c.e);
    each_line(out, est_line, &c);
    *e = c.e;
    return !c.oom && prep_estimate_ok(&c.e);
}

typedef struct { fx *x; const char *id; const char *name; int found; } pl_ctx;
static void pl_line(const char *l, size_t n, void *p) {
    pl_ctx *c = p;
    prep_playlist pl;
    if (c->found || prep_playlist_parse(&c->x->a, l, n, &pl) != 1) return;
    if (pl.id.n == strlen(c->id) && !memcmp(pl.id.s, c->id, pl.id.n)) {
        c->name = pl.name.s;
        c->found = 1;
    }
}

/* kept.noticed (JSONEncoder's [String: [String]]): what the last notifications covered, a set's
 * tracks that weren't ready. A file that isn't one such object (JSONDecoder's failure) is none. */
typedef struct { const char *name; pstr *files; int32_t n; } noticed_set;
static int32_t w_noticed_parse(fx *x, const char *text, size_t len, noticed_set *out, int32_t cap) {
    jval root;
    if (!len || json_parse_lim(&x->a, text, len, &kept_lim, &root) != CORE_OK || root.type != J_OBJ) return 0;
    int32_t k = 0;
    for (size_t i = 0; i < root.n; i++) {
        const jval *v = &root.items[i];
        if (v->type != J_ARR) return 0;
        for (size_t j = 0; j < v->n; j++)
            if (v->items[j].type != J_STR) return 0;
    }
    for (size_t i = 0; i < root.n && k < cap; i++) {
        const jval *v = &root.items[i];
        pstr *f = arena_alloc(&x->a, (v->n ? v->n : 1) * sizeof *f);
        if (!f) return 0;
        for (size_t j = 0; j < v->n; j++) f[j] = (pstr){v->items[j].s, v->items[j].len};
        out[k++] = (noticed_set){root.keys[i], f, (int32_t)v->n};
    }
    return k;
}
static const noticed_set *noticed_of(const noticed_set *v, int32_t n, const char *name) {
    for (int32_t i = 0; i < n; i++)
        if (!strcmp(v[i].name, name)) return &v[i];
    return NULL;
}

/* a JSON string, escaped as JSONEncoder escapes one (a NUL-free C string) */
static void json_str(tbuf *b, const char *s, size_t n) {
    tb_puts(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        char e[8];
        if (ch == '"' || ch == '\\') { e[0] = '\\'; e[1] = (char)ch; tb_put(b, e, 2); }
        else if (ch == '\n') tb_puts(b, "\\n");
        else if (ch == '\r') tb_puts(b, "\\r");
        else if (ch == '\t') tb_puts(b, "\\t");
        else if (ch < 0x20) { snprintf(e, sizeof e, "\\u%04x", ch); tb_puts(b, e); }
        else tb_put(b, (const char *)&s[i], 1);
    }
    tb_puts(b, "\"");
}

/* the noticing map as kept.noticed's text */
static void w_noticed_text(tbuf *b, const noticed_set *v, int32_t n) {
    tb_puts(b, "{");
    for (int32_t i = 0, first = 1; i < n; i++) {
        if (!v[i].files || !v[i].n) continue;   /* nil: not written */
        if (!first) tb_puts(b, ",");
        first = 0;
        json_str(b, v[i].name, strlen(v[i].name));
        tb_puts(b, ":[");
        for (int32_t j = 0; j < v[i].n; j++) {
            if (j) tb_puts(b, ",");
            json_str(b, v[i].files[j].s, v[i].files[j].n);
        }
        tb_puts(b, "]");
    }
    tb_puts(b, "}");
}

/* prepareFacts(measured: false), prepareProblems and prepareBaseArgs: on main, from the step's facts */
typedef struct { const core_facts *f; const char **argv; int32_t argc; char *problems; size_t cap; } kept_base;
static int q_kept_base(core *c, void *arg) {
    kept_base *k = arg;
    k->argc = prep_watch_args(c, k->f, k->argv, k->problems, k->cap);
    return 1;
}

typedef struct { fx *x; const char **v; int32_t n; } names_ctx;
static void name_line(const char *l, size_t n, void *p) {
    names_ctx *c = p;
    const char *t = memchr(l, '\t', n);
    if (!t || t == l || c->n >= KEPT_SETS_MAX || !memchr(t + 1, '\t', n - (size_t)(t + 1 - l))) return;   /* the Status tab's rule */
    char *s = arena_alloc(&c->x->a, (size_t)(t - l) + 1);
    if (!s) return;
    memcpy(s, l, (size_t)(t - l));
    s[t - l] = 0;
    c->v[c->n++] = s;
}

static void w_kept_check(watch_ctx *w) {
    fx *x = w->x;
    core_facts s;
    if (fx_step(x, CORE_STEP_WATCH_KEPT, &s, 1, "session") != 0) return;
    const char *pid = fact_str(&s, FACT_STEP_TEXT), *last = fact_str(&s, FACT_STEP_TEXT2);
    if (!*pid || !strcmp(last, pid) || !strcmp(w->kept_pid, pid)) return;
    /* this session is seen even with nothing prepared: a set made during it is the user's own doing */
    snprintf(w->kept_pid, sizeof w->kept_pid, "%s", pid);
    fx_do(x, CORE_STEP_WATCH_KEPT, 2, "seen", pid);
    core_facts ks;
    if (fx_step(x, CORE_STEP_WATCH_KEPT, &ks, 1, "sets") != 0) return;
    const char *names[KEPT_SETS_MAX];
    names_ctx nc = {x, names, 0};
    each_line(fact_str(&ks, FACT_KEPT_SETS), name_line, &nc);
    if (!nc.n) return;
    core_facts pf;
    if (fx_step(x, CORE_STEP_WATCH_KEPT, &pf, 1, "facts") != 0) return;
    const char *base[24];
    char *problems = arena_alloc(&x->a, 8192);
    if (!problems) return;
    kept_base kb = {&pf, base, 0, problems, 8192};
    if (!fx_ok(x) || !flow_query(x->life, q_kept_base, &kb, 0)) return;
    if (problems[0]) {   /* prepareProblems joined by " " */
        for (char *q = problems; *q; q++)
            if (*q == '\n') *q = ' ';
        LOG(x, "prepared playlists not checked: ", problems);
        return;
    }
    const char *pl_args[1] = {"--playlists"};
    helper_out pls = w_helper(x, "30", pl_args, 1);
    core_facts nf;
    const char *nt = "";
    size_t nlen = 0;
    if (fx_step(x, CORE_STEP_WATCH_KEPT, &nf, 1, "noticed") == 0) {
        nt = fact_str(&nf, FACT_STEP_TEXT);
        nlen = nf.set[FACT_STEP_TEXT] ? nf.len[FACT_STEP_TEXT] : 0;
    }
    noticed_set noticed[KEPT_SETS_MAX], noticing[KEPT_SETS_MAX];
    int32_t nn = w_noticed_parse(x, nt, nlen, noticed, KEPT_SETS_MAX);
    for (int32_t i = 0; i < nc.n && fx_ok(x); i++) {
        const char *set = names[i];
        const noticed_set *was = noticed_of(noticed, nn, set);
        noticing[i] = (noticed_set){set, was ? was->files : NULL, was ? was->n : 0};
        /* being prepared now: the tracks it hasn't reached aren't news */
        core_facts rn;
        if (fx_step(x, CORE_STEP_WATCH_KEPT, &rn, 2, "running", set) == 0 && fact_flag(&rn, FACT_STEP_FLAG)) {
            LOG(x, "prepared playlist ", set, ": being prepared, not checked");
            continue;
        }
        /* checkSet: the playlist as it is now, else the set's own files */
        const char *args[32];
        int32_t k = 0;
        for (int32_t j = 0; j < kb.argc && k < 24; j++) args[k++] = base[j];
        args[k++] = "--quick";
        args[k++] = "--keep";
        args[k++] = set;
        args[k] = "--playlist";
        args[k + 1] = set;
        helper_out r = w_helper(x, "60", args, k + 2);
        prep_estimate e;
        if (!w_parse_estimate(x, r.out, &e)) r = w_helper(x, "60", args, k);
        pl_ctx pc = {x, set, NULL, 0};
        each_line(pls.out, pl_line, &pc);
        const char *name = pc.found ? (pc.name ? pc.name : "") : TX1(x, T_PREP_PLAYLIST_ID, SL(name, set));
        if (!w_parse_estimate(x, r.out, &e)) {
            const char *why;
            COMPOSE_FIT(x, why, w_no_paths(B, r.err));
            LOG(x, "prepared playlist ", set, ": couldn't be checked (", why ? why : "", ")");
            continue;
        }
        const char *state;
        COMPOSE(x, state, w_set_state(B, &e));
        LOG(x, "prepared playlist ", set, ": ", state);
        pstr *now = arena_alloc(&x->a, (size_t)(e.ntracks ? e.ntracks : 1) * sizeof *now);
        if (!now) continue;
        int32_t nnow = w_kept_not_ready(&e, now, e.ntracks < KEPT_FILES_MAX ? e.ntracks : KEPT_FILES_MAX);
        noticing[i] = (noticed_set){set, nnow ? now : NULL, nnow};
        /* was's files aren't written any more (noticing[i] is now): sorted in place for the lookup */
        if (was) w_kept_sort(was->files, was->n);
        if (w_kept_newly(now, nnow, was ? was->files : NULL, was ? was->n : 0)) {
            const char *body = NULL;
            int has = 0;
            COMPOSE(x, body, has = w_kept_notice(B, &e));
            if (has) fx_do(x, CORE_STEP_WATCH_KEPT, 3, "notify", TX1(x, T_RUN_PARTIAL, SL(playlist, name)), body);
        }
    }
    if (!fx_ok(x)) return;
    /* written whole at any size; one that can't be built leaves the old file (1.3.0 wrote only
     * what JSONEncoder made); "{}" is written: it clears what no set covers any more */
    const char *text;
    COMPOSE_FIT(x, text, w_noticed_text(B, noticing, nc.n));
    if (text) fx_do(x, CORE_STEP_WATCH_KEPT, 2, "notice", text);
}
static void w_session(void *p) { w_kept_check(p); }

/* unsupportedKey(key): key + " unsupported with " + the lists joined by "," */
static void w_unsupported(tbuf *b, const pstr *k, const char *rb_lines, const char *engine_lines, const char *ort_prefix) {
    const char *rb[64], *en[64];
    char rbuf[2048], ebuf[2048];
    int32_t nrb = 0, nen = 0;
    snprintf(rbuf, sizeof rbuf, "%s", rb_lines);
    snprintf(ebuf, sizeof ebuf, "%s", engine_lines);
    for (char *s = rbuf, *t; *s && nrb < 64; s = t) {
        t = strchr(s, '\n');
        if (t) *t++ = 0; else t = s + strlen(s);
        if (*s) rb[nrb++] = s;
    }
    for (char *s = ebuf, *t; *s && nen < 64; s = t) {
        t = strchr(s, '\n');
        if (t) *t++ = 0; else t = s + strlen(s);
        if (*s) en[nen++] = s;
    }
    watcher_unsupported_key(b, k->s, k->n, rb, nrb, en, nen, ort_prefix);
}

/* dismissed's new text, have and add together (remember: sorted, one a line), written whole */
static void w_remember(watch_ctx *w, const keyset *have, const pstr *add, int32_t n) {
    const char *text;
    COMPOSE(w->x, text, keyset_union_text(B, have, add, n));
    if (fx_do(w->x, CORE_STEP_WATCH_REMEMBER, 1, text) != 0) LOG(w->x, "couldn't remember the answer");
}

static const char *w_join(fx *x, const pstr *k, int32_t n) {   /* "a, b" */
    const char *s = "";
    for (int32_t i = 0; i < n; i++) {
        const char *one = arena_strdup(&x->a, k[i].s, k[i].n);
        s = CAT(x, s, i ? ", " : "", one ? one : "");
    }
    return s;
}

/* watcherCheck: 1 done (a change since may be looked at), 0 stop (busy, an uninstall left
 * half-done, the app opened to reinstall) */
static int32_t w_check(void *p) {
    watch_ctx *w = p;
    fx *x = w->x;
    watcher_settle_ops so = {w, w_updating, w_busy, w_open, w_pause, w_half, w_session, w_log};
    if (!watcher_settle(&so)) return 0;
    /* what this check sees starts here: a start before now is covered by it, one after it leaves
     * the again-mark anew */
    fx_do(x, CORE_STEP_WATCH_LOCK, 1, "clear_again");
    core_facts f;
    if (fx_step(x, CORE_STEP_WATCH_FACTS, &f, 0) != 0) {
        LOG(x, "not checking: what is installed couldn't be read");
        return 1;
    }
    int cm = fact_flag(&f, FACT_CHOSEN_MODEL), cc = fact_flag(&f, FACT_CHOSEN_CACHE);
    int plus = fact_flag(&f, FACT_STEMS_PLUS_PRESENT), cache = fact_flag(&f, FACT_STEMS_CACHE_PRESENT);
    const char *rb = fact_str(&f, FACT_REKORDBOX_VERSION), *engine = fact_str(&f, FACT_ENGINE);
    LOG(x, "run: RB Stems Plus Watcher ", fact_str(&f, FACT_STEP_TEXT2), "; rekordbox ", rb, ", engine ", engine, ", Demucs v4 ",
        plus ? "on" : "off", ", Stems Cache ", cache ? "on" : "off", "; chosen: model=", cm ? "true" : "false", " cache=", cc ? "true" : "false");
    const char *dis = fact_str(&f, FACT_STEP_TEXT);
    keyset dismissed = {0}, put = {0};
    pstr keys[2], kept[2];
    int32_t nk = -1, nkept = 0;
    if (keyset_parse(&x->a, dis, strlen(dis), &dismissed) == 0 && keyset_parse(&x->a, w->put_off, strlen(w->put_off), &put) == 0)
        nk = state_missing_keys(&x->a, cm, cc, cache, plus, rb, engine, &dismissed, keys);
    if (nk < 0) {
        LOG(x, "not checking: out of memory");
        return 1;
    }
    /* less what the user was already told can't be reinstalled yet */
    for (int32_t i = 0; i < nk; i++) {
        const char *u;
        COMPOSE(x, u, w_unsupported(B, &keys[i], fact_str(&f, FACT_STEP_PATH), fact_str(&f, FACT_STEP_URL), fact_str(&f, FACT_ORT_PREFIX)));
        if (!keyset_has(&dismissed, u, strlen(u))) kept[nkept++] = keys[i];
    }
    const char *list = w_join(x, kept, nkept);
    int32_t q = watcher_ask(kept, nkept, &put);
    if (q == WATCH_NOTHING) { LOG(x, "nothing to ask"); return 1; }
    if (q == WATCH_PUT_OFF) { LOG(x, "still missing (", list, "), put off a moment ago: not asking again now"); return 1; }
    LOG(x, "asking: ", list);
    int model_off = 0, cache_off = 0;
    for (int32_t i = 0; i < nkept; i++) {
        if (kept[i].n >= 6 && !memcmp(kept[i].s, "model:", 6)) model_off = 1;
        if (kept[i].n >= 6 && !memcmp(kept[i].s, "cache:", 6)) cache_off = 1;
    }
    const char *short_rb, *msg;
    COMPOSE(x, short_rb, w_short_version(B, rb));
    watcher_dialog wd;
    COMPOSE(x, msg, watcher_dialog_build(B, &wd, model_off, cache_off, engine, short_rb, fact_flag(&f, FACT_ENGINE_SUPPORTED),
                                         fact_flag(&f, FACT_REKORDBOX_SUPPORTED), cm && plus));
    core_dialog d;
    dlg_init(&d, wd.title, TX(x, wd.title), msg, 1);
    for (int32_t i = 0; i < wd.nbuttons; i++) dlg_button(x, &d, TX(x, wd.buttons[i]));
    int b = ask_d(x, &d);
    LOG(x, "user chose: ", b >= 0 && b < d.nbuttons ? d.buttons[b] : "");
    switch (watcher_answer(&wd, b)) {
    case WANSWER_REINSTALL:
        /* the app does the reinstall: it reads the request at launch, or on the reopen event macOS
         * sends when it is already open; it is opened once this process has exited (and let go
         * of the lock): its reinstall changes what the watcher watches */
        fx_do(x, CORE_STEP_WATCH_REINSTALL, 0);
        return 0;
    case WANSWER_REMEMBER:
        w_remember(w, &dismissed, kept, nkept);
        return 1;
    case WANSWER_REMEMBER_UNSUPPORTED: {
        /* "not supported yet": nothing to do until a newer version list arrives, so don't ask
         * after every rekordbox session (the settings folder changes each time) */
        pstr add[2];
        for (int32_t i = 0; i < nkept; i++) {
            const char *u;
            COMPOSE(x, u, w_unsupported(B, &kept[i], fact_str(&f, FACT_STEP_PATH), fact_str(&f, FACT_STEP_URL), fact_str(&f, FACT_ORT_PREFIX)));
            add[i] = (pstr){u, strlen(u)};
        }
        w_remember(w, &dismissed, add, nkept);
        return 1;
    }
    default: {   /* Remind Me Later: not asked again until the next start */
        const char *t;
        COMPOSE(x, t, keyset_union_text(B, &put, kept, nkept));
        snprintf(w->put_off, sizeof w->put_off, "%s", t);
        return 1;
    }
    }
}

static void watch_flow(core_life *life, void *arg) {
    (void)arg;
    fx x;
    memset(&x, 0, sizeof x);
    x.life = life;
    x.os = life->os;
    watch_ctx w;
    memset(&w, 0, sizeof w);
    w.x = &x;
    watcher_round_ops ro = {&w, w_check, w_release, w_again, w_retake, w_log};
    (void)watcher_rounds(WATCH_MAX_ROUNDS, &ro);
    arena_free(&x.a);
}

static void no_free(void *a) { (void)a; }

int flows_watch_start(core *c) {
    if (c->role != CORE_ROLE_WATCHER || c->flow_running) return CORE_E_STATE;
    return flow_start_ex(c, CORE_WORK_FLOW, watch_flow, NULL, no_free, 0);
}

/* ---------------------------------------------------------------- main's entries */

void flows_init(core *c) {
    memset(&c->fl, 0, sizeof c->fl);
    c->fl.del_text = c->fl.del_text_n = -1;
}

void flows_free(core *c) {
    flows_state *fl = &c->fl;
    free(fl->update_version);
    free(fl->admin_status);
    free(fl->dl_what);
    free(fl->del_drive);
    memset(fl, 0, sizeof *fl);
}

static const struct { const char *id; flow_kind kind; } click_ids[] = {
    {"install_plus", F_INSTALL_PLUS},     {"install_cache", F_INSTALL_CACHE},     {"reinstall", F_REINSTALL},
    {"reinstall_plus", F_REINSTALL_PLUS}, {"reinstall_cache", F_REINSTALL_CACHE}, {"uninstall_plus", F_UNINSTALL_PLUS},
    {"uninstall_cache", F_UNINSTALL_CACHE}, {"uninstall_all", F_UNINSTALL_ALL},   {"settings", F_SETTINGS},
    {"update", F_UPDATE},                 {"create_report", F_REPORT},
#ifdef CORE_WIN_POLICY
    {"restore_pioneer", F_WIN_RESTORE},   /* Windows: R1's button (W-core) */
#endif
};

int flows_event(core *c, const char *id, size_t n) {
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN && win_flows_event(c, id, n)) return 1;   /* Settings > Apps' Uninstall */
#endif
    for (size_t i = 0; i < sizeof click_ids / sizeof *click_ids; i++) {
        if (strlen(click_ids[i].id) != n || memcmp(click_ids[i].id, id, n)) continue;
#ifdef CORE_WIN_POLICY
        if (click_ids[i].kind == F_WIN_RESTORE && c->os != CORE_OS_WIN) return 0;   /* Windows' button only */
#endif
        flows_state *fl = &c->fl;
        if (c->action_running) {   /* the buttons are off. M4: the button's own enabled state */
            core_log(c->life, "core: a click while an action runs is ignored");
            return 1;
        }
        if (c->flow_running) {
            /* F4: delayed only while the running flow hasn't asked anything or turned busy on (1.3.0
             * delivered it after main-thread work without a modal); dropped once it has, as 1.3.0's
             * modal session and disabled buttons dropped it */
            int open;
            CALLBACK(c, open = c->life->host.modal_open(c->life->host.ctx));
            if (!open && !atomic_load(&c->life->asked) && fl->nqueued < FLOWS_QUEUE_MAX) {
                fl->queued[fl->nqueued++] = (int8_t)i;
                core_log(c->life, "core: a click while a flow runs is queued");
            } else core_log(c->life, "core: a click while an action runs is ignored");
            return 1;
        }
        if (start_flow(c, click_ids[i].kind)) core_log(c->life, "core: the action couldn't start");
        return 1;
    }
    return 0;
}

/* a flow's end, on main (flow.c), F4/F5: when the flow asked something or turned busy on, the
 * queued clicks go (1.3.0 dropped them); else the first one that still can is replayed, and the
 * rest stay queued behind its flow. A pending cache offer (Q9) waits for an empty queue. */
void flows_flow_ended(core *c) {
    flows_state *fl = &c->fl;
    if (c->flow_running) return;
    if (fl->prep_reserved) {   /* SO1: the flow that held Prepare back has ended */
        fl->prep_reserved = 0;
        prep_update(c);
    }
    if (atomic_load(&c->life->asked)) {
        for (int i = 0; i < fl->nqueued; i++) core_log(c->life, "core: a click while an action runs is ignored");
        fl->nqueued = 0;
    }
    while (fl->nqueued && !c->flow_running) {
        int8_t k = fl->queued[0];
        memmove(fl->queued, fl->queued + 1, (size_t)(fl->nqueued - 1) * sizeof *fl->queued);
        fl->nqueued--;
        if (c->action_running) {
            core_log(c->life, "core: a click while an action runs is ignored");
            continue;
        }
        if (start_flow(c, click_ids[k].kind)) core_log(c->life, "core: the action couldn't start");
    }
#ifdef CORE_WIN_POLICY
    if (c->os == CORE_OS_WIN && !c->flow_running) win_flow_ended(c);   /* the update or uninstall asked for meanwhile */
#endif
    if (fl->cache_offer_pending && !c->flow_running && !fl->nqueued) offer_cache_update_now(c);
}

void flows_shown(core *c) {
    if (c->fl.shown_done) {
        core_log(c->life, "core: shown again: ignored");
        return;
    }
    c->fl.shown_done = 1;
    if (start_flow(c, F_STARTUP)) {   /* out of memory: asked again at the next activation (OOM sweep) */
        core_log(c->life, "core: the start-up checks couldn't start");
        c->fl.startup_retry = 1;
    }
}

void flows_check_request(core *c) {
    flows_state *fl = &c->fl;
    if (fl->startup_retry && !c->flow_running && !c->action_running) {   /* the start-up first, as at "shown" */
        if (!start_flow(c, F_STARTUP)) fl->startup_retry = 0;
        return;
    }
    if (!fl->startup_done || c->action_running || fl->request_waiting || c->flow_running) return;
    start_flow(c, F_REQUEST);
}

void flows_progress(core *c, int32_t stage, int64_t done, int64_t total) {
    flows_state *fl = &c->fl;
    if (!c->flow_running) {   /* F20: a late progress after the flow ended breaks the host contract */
#ifdef CORE_DEBUG
        if (!fl->warned_late) core_log(c->life, "core: violation: a progress after its flow ended is ignored");
#endif
        fl->warned_late = 1;
        return;
    }
    char line[2048];
    line[0] = 0;
    core_slot s[2];
    char n[24];
    tbuf b;
    tb_init(&b, c->os, line, sizeof line);
    switch (stage) {
    case CORE_PROGRESS_ADMIN_ATTEMPT:   /* the password accepted: busy(status) */
        if (!fl->admin_status) return;
        c->action_running = 1;
        model_set_str(&c->m, F_STATUS_LINE, fl->admin_status, strlen(fl->admin_status));
        model_set_i64(&c->m, F_SPINNER, 1);
        /* OM2: the flow's end turns it off if EF_IDLE never comes. Every admin run today is inside
         * an action, where OM3's flow_marked latch already turns busy off at the flow's end; this
         * guards a future admin run outside an action (no act_id, so no latch) */
        fl->busy_shown = 1;
        status_update_buttons(c);   /* Stop Keeping… off */
        core_mark_changed(c);
        return;
    case CORE_PROGRESS_DOWNLOAD: {
        if (!fl->dl_what) return;
        s[0] = SLOT_S(name, fl->dl_what);
        if (done == -1) tb_text(&b, T_CHECKING_WHAT, s, 1);
        else if (done >= 0 && total > 0) {
            int64_t pct = done * 100 / total;
            snprintf(n, sizeof n, "%lld", (long long)(pct < 99 ? pct : 99));
            s[1] = SLOT_S(n, n);
            tb_text(&b, T_DOWNLOADING_WHAT_PCT, s, 2);
        } else tb_text(&b, T_DOWNLOADING_WHAT, s, 1);
        break;
    }
#ifdef CORE_WIN_POLICY
    case CORE_PROGRESS_STILL_CLOSING:   /* the worker waits for rekordbox's lingering process */
        if (done == 1) {   /* ... and it has closed: the attempt's own line again (review C11) */
            const char *own = fl->admin_status ? fl->admin_status : "";
            model_set_str(&c->m, F_STATUS_LINE, own, strlen(own));
            core_mark_changed(c);
            return;
        }
        TB_TEXT0(&b, T_WIN_STILL_CLOSING);
        break;
#endif
    case CORE_PROGRESS_DELETE: {
        if (fl->del_text_n < 0 || !fl->del_drive) return;
        snprintf(n, sizeof n, "%lld", (long long)done);
        s[0] = SLOT_S(drive, fl->del_drive);
        s[1] = SLOT_S(n, n);
#ifdef CORE_WIN_POLICY
        if (TB_NAMELESS(&b, fl->del_drive)) {   /* del_text_n is the nameless row (delete_texts, clear_location): only {n} */
            tb_text(&b, (core_text_id)fl->del_text_n, &s[1], 1);
            break;
        }
#endif
        tb_text(&b, (core_text_id)fl->del_text_n, s, 2);
        break;
    }
    default:
        core_mark_changed(c);
        return;
    }
    if (b.err) return;
    model_set_str(&c->m, F_STATUS_LINE, line, strlen(line));
    core_mark_changed(c);
}

void core_admin_note(core *c, int32_t note) {
#ifdef CORE_WIN_POLICY
    if (!c || (note != CORE_ADMIN_NOTE_NOT_PERMITTED && !(c->os == CORE_OS_WIN && note == CORE_ADMIN_NOTE_RESTART_PENDING))) return;
#else
    if (!c || note != CORE_ADMIN_NOTE_NOT_PERMITTED) return;
#endif
    core_life *l = c->life;
    if (life_on_main(l)) {   /* flow only, inside admin */
        CORE_VIOLATION("core_admin_note on main");
        return;
    }
    if (!life_alive(l)) return;
    atomic_fetch_or(&l->admin_note, note);
}

/* ---------------------------------------------------------------- Prepare's dialogs (M4, PrepareView.swift) */

/* What the dialog is about, copied on main (the flow never reads the core's state). e's numbers
 * only: its tracks, refusal and folders are left out (refused carries the refusal). */
typedef struct {
    int kind;
    char *id, *name, *refused, *folder;   /* folder: "doesn't fit"'s free space; the set's name for Stop Keeping */
    prep_estimate e;
    uint64_t seq;
} prep_ask_arg;

static void prep_ask_free(void *p) {
    prep_ask_arg *a = p;
    if (!a) return;
    free(a->id);
    free(a->name);
    free(a->refused);
    free(a->folder);
    free(a);
}

typedef struct { uint64_t seq; int64_t first; double raised; char id[]; } prep_go_msg;

static void prep_go_job(core *c, void *payload) {
    prep_go_msg *m = payload;
    prep_ask_go(c, m->seq, m->id, m->first, m->raised);
    core_mark_changed(c);
}

static void stopped_keeping_job(core *c, void *payload) {
    status_stopped_keeping(c, (const char *)payload);
    core_mark_changed(c);
}

/* the answer to main: a run (prep_go_job) or the Status tab's reload (stopped_keeping_job) */
static void post_named(fx *x, job_fn fn, const char *id, uint64_t seq, int64_t first, double raised) {
    if (!fx_ok(x)) return;
    size_t n = strlen(id);
    if (fn == prep_go_job) {
        prep_go_msg *m = malloc(sizeof *m + n + 1);
        if (!m) return;   /* lost: the click did nothing, as a cancelled dialog */
        m->seq = seq;
        m->first = first;
        m->raised = raised;
        memcpy(m->id, id, n + 1);
        core_post_main(x->life, fn, m, NULL, 0, 0);
        return;
    }
    /* Stop Keeping's reload comes after the removal. Lost under out of memory (here or in
     * core_post_main): accepted, as other lost posts are. The Status row stays until the next
     * reload (activation, a tab switch); a second click asks again and then reloads. */
    char *s = malloc(n + 1);
    if (!s) return;
    memcpy(s, id, n + 1);
    core_post_main(x->life, fn, s, NULL, 0, 0);
}

/* prepareAction's "doesn't fit" (PrepareView.swift:765): the first tracks that fit, or the limit
 * raised (writeConfig with readConfig().maxDays). 1: prepare (*first the tracks, -1 all; *raised
 * the limit written, 0 none) */
static int doesnt_fit(fx *x, const prep_ask_arg *a, int64_t *first, double *raised) {
    core_facts sp;
    double free_gb = 0;   /* max(0, (freeBytes(folder) ?? 0) - 1 GiB) in GiB */
    if (fx_step(x, CORE_STEP_FREE_SPACE, &sp, 1, a->folder) == 0) {
        double g = ((double)fact_i64(&sp, FACT_STEP_CODE, 0) - 1073741824.0) / 1073741824.0;
        free_gb = g > 0 ? g : 0;
    }
    if (!fx_ok(x)) return 0;
    fit_choices fc;
    const char *msg;
    COMPOSE(x, msg, w_doesnt_fit(B, &a->e, free_gb, &fc));
    core_dialog d;
    dlg_init(&d, T_PREP_DOESNT_FIT, TX1(x, T_PREP_DOESNT_FIT, SL(playlist, a->name)), msg, 0);
    for (int32_t i = 0; i < fc.nbuttons; i++) {
        const char *t;
        COMPOSE(x, t, w_fit_button(B, &fc, i));
        dlg_button(x, &d, t);
    }
    int b = ask_d(x, &d);
    if (b < 0) return 0;
    if (fc.first_n > 0 && b-- == 0) {   /* Prepare the First N Tracks */
        *first = fc.first_n;
        return 1;
    }
    if (!fc.has_raise || b != 0) return 0;   /* Cancel */
    char r[32];
    fmt_config_number(fc.raise_to, r);
    core_facts f, w;
    const char *days = fx_facts(x, &f) ? fact_str_or_null(&f, FACT_CONFIG_MAX_DAYS) : NULL;
    if (days && fx_step(x, CORE_STEP_CONFIG_WRITE, &w, 4, "max_gb", r, "max_days", days) == 0) {
        LOG(x, "settings: max_gb=", r, " (raised to prepare playlist ", a->id, ")");
        *raised = fc.raise_to;
        return 1;
    }
    if (fx_ok(x)) alert_ok(x, T_LIMIT_NOT_RAISED, TX(x, T_LIMIT_NOT_RAISED_MSG));
    return 0;
}

static void prep_ask_flow(core_life *life, void *arg) {
    prep_ask_arg *a = arg;
    fx x;
    memset(&x, 0, sizeof x);
    x.life = life;
    x.os = life->os;
    int64_t first = -1;
    double raised = 0;
    int go = 0;
    switch (a->kind) {
    case PREP_ASK_REFUSED:   /* "Can't prepare “x”" with the refusal [OK] */
        (void)ask_t(&x, T_PREP_CANT_TITLE, TX1(&x, T_PREP_CANT_TITLE, SL(playlist, a->name)), a->refused, 1, TX(&x, T_OK));
        break;
    case PREP_ASK_FIT: go = doesnt_fit(&x, a, &first, &raised); break;
    case PREP_ASK_SPACE: {   /* "Not enough free space" [Prepare] [Cancel] */
        const char *size;
        COMPOSE(&x, size, w_size(B, a->e.need_gb));
        go = ASK(&x, T_NO_SPACE_TITLE, TX1(&x, T_NO_SPACE_MSG, SL(size, size)), 2, TX(&x, T_BTN_PREPARE), TX(&x, T_CANCEL)) == 0;
        break;
    }
    default: break;
    }
    if (go) post_named(&x, prep_go_job, a->id, a->seq, first, raised);
    prep_ask_free(a);   /* Q24: flow_work hands the arg over */
    fx_release(&x);
}

static char *dup_or_null(const char *s, int *bad) {
    if (!s) return NULL;
    char *d = strdup(s);
    if (!d) *bad = 1;
    return d;
}

int flows_prepare_ask(core *c, int kind, const char *id, const char *name, const prep_estimate *e, const char *folder,
                      uint64_t seq) {
    prep_ask_arg *a = calloc(1, sizeof *a);
    if (!a) return CORE_E_NOMEM;
    int bad = 0;
    a->kind = kind;
    a->seq = seq;
    a->id = dup_or_null(id ? id : "", &bad);
    a->name = dup_or_null(name ? name : "", &bad);
    a->folder = dup_or_null(folder ? folder : "", &bad);
    if (e) {
        a->e = *e;
        a->e.tracks = NULL;
        a->e.ntracks = a->e.captracks = 0;
        a->e.folders = NULL;
        a->e.nfolders = 0;
        a->e.refused = (pstr){0};
        if (kind == PREP_ASK_REFUSED) {
            a->refused = malloc(e->refused.n + 1);
            if (a->refused) {
                memcpy(a->refused, e->refused.s ? e->refused.s : "", e->refused.n);
                a->refused[e->refused.n] = 0;
            } else bad = 1;
        }
    }
    if (bad) {
        prep_ask_free(a);
        return CORE_E_NOMEM;
    }
    return flow_start_ex(c, CORE_WORK_FLOW, prep_ask_flow, a, prep_ask_free, 0);
}

static void stop_keeping_flow(core_life *life, void *arg) {
    prep_ask_arg *a = arg;   /* name: the playlist's; folder: the set's */
    fx x;
    memset(&x, 0, sizeof x);
    x.life = life;
    x.os = life->os;
    /* confirm("Stop keeping “x”", "Stop Keeping", info): "Stop keeping “x”?" [Stop Keeping] [Cancel].
     * The set confirmed is the set removed, whatever the tab shows meanwhile (1.3.0's `s`). */
    const char *title = TX1(&x, T_CONFIRM, SL(action, TX1(&x, T_STOP_KEEPING_TITLE, SL(playlist, a->name))));
    if (ask_t(&x, T_STOP_KEEPING_TITLE, title, TX(&x, T_STOP_KEEPING_MSG), 2, TX(&x, T_STOP_KEEPING), TX(&x, T_CANCEL)) == 0) {
        core_facts out;
        int ok = fx_step(&x, CORE_STEP_STOP_KEEPING, &out, 1, a->folder) == 0;
        LOG(&x, "prepare: stopped keeping playlist ", a->folder, ": ", ok ? "ok" : "FAILED");
        post_named(&x, stopped_keeping_job, a->folder, 0, 0, 0);
    }
    prep_ask_free(a);
    fx_release(&x);
}

int flows_stop_keeping(core *c, const char *set, const char *name) {
    prep_ask_arg *a = calloc(1, sizeof *a);
    if (!a) return CORE_E_NOMEM;
    int bad = 0;
    a->folder = dup_or_null(set ? set : "", &bad);
    a->name = dup_or_null(name ? name : "", &bad);
    if (bad) {
        prep_ask_free(a);
        return CORE_E_NOMEM;
    }
    return flow_start_ex(c, CORE_WORK_FLOW, stop_keeping_flow, a, prep_ask_free, 0);
}

#ifdef CORE_WIN_POLICY
#include "flows_win.c"   /* the Windows flow variants (W-core): flows.c's statics, never in the Mac objects */
#endif
