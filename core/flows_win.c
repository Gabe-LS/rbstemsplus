/* The Windows flow variants (W-core, app.md §9; mac-core §3.1 "Windows-only C code"). Included at
 * the end of flows.c under CORE_WIN_POLICY, so it uses flows.c's own helpers; it is not compiled on
 * its own (core/build.sh doesn't list it) and never reaches the Mac objects. Each variant runs only
 * for a core started with "os":"win"; flows.c calls in through its CORE_WIN_POLICY hooks.
 *
 *   - admin: one call is one elevation (UAC); no "Permission needed" re-sign loop and no password to
 *     forget; UAC declined (ERROR_CANCELLED, 1223) is the user stopping, with no alert (app.md
 *     §2.2); the worker's "run again" (13) runs the attempt once more; its protocol mismatch (12)
 *     and a payload below its floor (14) offer to update the app first (I76, J23), and the update
 *     starts once the flow has ended.
 *   - rekordbox still closing (PLAN R1, app.md §5.5): rekordbox 7.2.19's process lingers about 75 s
 *     after its window closes; every "is rekordbox open" check waits for it with "rekordbox is
 *     still closing…" (the worker's own wait shows the same line, CORE_PROGRESS_STILL_CLOSING).
 *   - R1's "Put Back Pioneer's File" (click "restore_pioneer"): the worker's restore verb.
 *   - Settings > Apps' Uninstall: the Apps & features entry starts the app with its uninstall
 *     argument, and the shell sends {"ev":"click","id":"uninstall_from_settings"}: the complete
 *     uninstall, as the menu's, once the start-up flow is done.
 *   - The complete uninstall: no Dock step; the restart-pending line when the worker left its own
 *     file for the next restart (I20, only when it applies).
 */

enum { WIN_CLOSING_POLL_MS = 500, WIN_CLOSING_MAX_MS = 90000 };   /* rekordbox's linger is about 75 s */

/* after host->admin returned r: UAC declined is the user stopping; App Management doesn't exist */
static int32_t win_after_admin(fx *x, int32_t r, int *not_permitted) {
    *not_permitted = 0;
    if (atomic_load(&x->life->admin_note) & CORE_ADMIN_NOTE_RESTART_PENDING) x->win_restart = 1;
    if (r == ROOT_WIN_USER_STOPPED) {
        LOG(x, "  UAC declined, or the app is quitting: stopped");   /* review MC5: a closed admission answers 1223 too */
        return -1;
    }
    return r;
}

/* main: the status line as shown now, for the wait to put back */
typedef struct { char s[1024]; } status_copy;
static int q_status_line(core *c, void *arg) {
    status_copy *o = arg;
    const field_meta *fm = &core_field_meta[F_STATUS_LINE];
    const cell *v = &c->m.sec[fm->section]->cells[fm->index];
    size_t n = v->len < sizeof o->s ? v->len : sizeof o->s - 1;
    if (n) memcpy(o->s, v->v.s, n);
    o->s[n] = 0;
    return 1;
}

/* the wait's line comes down: busy, the action's own line again; else none (review C11) */
static void win_closing_done(fx *x, const char *before) { fx_status(x, x->busy ? before : ""); }

/* one rekordbox check: 0 done (open, closing filled), -1 couldn't be checked */
static int win_rb_check(fx *x, int *updating, int *open, int *closing) {
    core_facts f;
    int32_t rc = fx_step(x, CORE_STEP_REKORDBOX, &f, 0);
    if (updating) *updating = fact_flag(&f, FACT_PIONEER_UPDATING);
    *open = fact_flag(&f, FACT_REKORDBOX_OPEN);
    *closing = *open && fact_flag(&f, FACT_REKORDBOX_CLOSING);
    if (rc != 0) {
        LOG(x, "  rekordbox check failed: stopped");
        return -1;
    }
    return 0;
}

/* rekordboxOpen() on Windows: 1 open, 0 closed, -1 couldn't be checked. A rekordbox whose window
 * is gone but whose process lingers is waited for, with "rekordbox is still closing…", until one
 * monotonic deadline per flow (WIN_CLOSING_MAX_MS from its first wait; review C12, C13); then it
 * counts as open, and the "rekordbox open" texts say it is still closing (C14). The line comes
 * down on every way out, the failed check and core_free included (C11). */
static int win_rb_open(fx *x, int *updating) {
    status_copy before = {{0}};
    int shown = 0, open, closing;
    uint32_t max_ms = x->life->win_closing_max_ms ? x->life->win_closing_max_ms : WIN_CLOSING_MAX_MS;
    for (;;) {
        int rc = win_rb_check(x, updating, &open, &closing);
        x->win_rb_closing = rc == 0 && closing;
        uint64_t now = plat_now_mono_ns();
        if (rc == 0 && closing && !x->win_deadline) x->win_deadline = now + (uint64_t)max_ms * 1000000u;
        if (rc != 0 || !closing || now >= x->win_deadline || !fx_ok(x)) {
            if (shown) {
                if (rc == 0) LOG(x, open ? "  rekordbox didn't finish closing" : "  rekordbox has closed");
                win_closing_done(x, before.s);
            }
            return rc != 0 ? -1 : open;
        }
        if (!shown) {
            if (!flow_query(x->life, q_status_line, &before, 0)) before.s[0] = 0;
            LOG(x, "  rekordbox is still closing: waiting");
            fx_status(x, TX(x, T_WIN_STILL_CLOSING));
            shown = 1;
        }
        uint64_t left_ms = (x->win_deadline - now) / 1000000u, nap = left_ms < WIN_CLOSING_POLL_MS ? left_ms : WIN_CLOSING_POLL_MS;
        for (uint64_t t = 0; t < nap && fx_ok(x); t += 50) plat_sleep_ms((uint32_t)(nap - t < 50 ? nap - t : 50));
    }
}

/* the checks that only read "updating", or only whether rekordbox runs: one check, no wait (C12) */
static int win_rb_updating(fx *x, int *updating) {
    if (x->os != CORE_OS_WIN) return rb_open(x, updating);
    int open, closing;
    if (win_rb_check(x, updating, &open, &closing)) return -1;
    return open;
}

/* "rekordbox open"'s message: after the wait for a rekordbox still closing, that it is (C14) */
static const char *win_rb_open_text(fx *x, core_text_id id) {
    if (x->os == CORE_OS_WIN && x->win_rb_closing) {
        if (id == T_RB_OPEN_TRY_AGAIN) return TX(x, T_WIN_STILL_CLOSING_TRY);
        if (id == T_RB_OPEN_MSG) return TX(x, T_WIN_STILL_CLOSING_MSG);
    }
    return TX(x, id);
}

/* main: the update once the flow that offered it has ended (win_flow_ended) */
static void win_update_after_job(core *c, void *payload) {
    (void)payload;
    c->fl.win_update_after = 1;
}

/* a newer RB Stems Plus than this one: the saved payload.json's, else the latest one, downloaded
 * and verified now (as install_body fetches it); NULL when none is known or the download failed */
static const char *win_newer_app(fx *x) {
    core_facts p;
    fx_step(x, CORE_STEP_PAYLOAD, &p, 1, "saved");
    const char *v = fact_str(&p, FACT_PAYLOAD_APP_VERSION);
    if (fact_flag(&p, FACT_PAYLOAD_FOUND) && flows_version_newer(v, x->own)) return dup_s(x, v);
    if (!fx_ok(x)) return NULL;
    fx_status(x, TX(x, T_CHECKING_LATEST));
    fx_step(x, CORE_STEP_PAYLOAD, &p, 1, "latest");
    if (!fx_ok(x)) return NULL;
    if (!x->busy) fx_status(x, "");   /* busy: the action's own end clears it */
    v = fact_str(&p, FACT_PAYLOAD_APP_VERSION);
    int fresh = fact_i64(&p, FACT_PAYLOAD_FETCH, 2) == 0 && fact_flag(&p, FACT_PAYLOAD_FOUND);
    if (!fresh || !flows_version_newer(v, x->own)) {
        LOG(x, fresh ? "  the latest payload.json names no newer RB Stems Plus" : "  the latest payload.json couldn't be downloaded");
        return NULL;
    }
    fx_effect(x, EF_UPDATE_FOUND, 0, v);   /* the notice and the menu item, as install_body's */
    return dup_s(x, v);
}

/* the update-first offer, in one line, when a newer RB Stems Plus is known: the update starts once
 * this flow has ended (its own confirmation, as the menu's Update RB Stems Plus…). With none known,
 * a one-button alert: never a silent end (review C1) */
static void win_offer_update_first(fx *x) {
    LOG(x, "  this app's worker can't make the change: offering to update RB Stems Plus first");
    const char *newer = win_newer_app(x);
    if (!fx_ok(x)) return;
    if (!newer) {
        alert_ok(x, T_WIN_UPDATE_FIRST_TITLE, TX(x, T_WIN_NO_NEWER_MSG));
        return;
    }
    LOG(x, "  RB Stems Plus ", newer, " is available");
    core_dialog d;
    dlg_init(&d, T_WIN_UPDATE_FIRST_TITLE, TX(x, T_WIN_UPDATE_FIRST_TITLE), TX(x, T_WIN_UPDATE_FIRST_MSG), 0);
    dlg_button(x, &d, TX(x, T_UPDATE));
    dlg_button(x, &d, TX(x, T_CANCEL));
    int b = ask_d(x, &d);
    LOG(x, "  user chose: ", chose(&d, b));
    if (b == 0 && fx_ok(x)) core_post_main(x->life, win_update_after_job, NULL, NULL, 0, 0);
}

/* R1's "Put Back Pioneer's File" (app.md §5.2, §5.5 restore): the worker puts rekordbox's own
 * onnxruntime.dll back and keeps the root; Stems Cache is off until it is reinstalled */
static void win_restore_flow(fx *x) {
    const char *label = TX(x, T_WIN_BTN_RESTORE);
    if (!preflight(x) || !guard_snap(x, NULL)) return;
    guard_arm(x);
    int b = ASK(x, T_WIN_RESTORE_Q, TX(x, T_WIN_RESTORE_MSG), 2, label, TX(x, T_CANCEL));
    if (disarm_guard(x, b) != 0) return;
    LOG(x, "restore: putting Pioneer's onnxruntime.dll back");
    fx_start(x, "restore", label, T_WIN_RESTORING);   /* the busy marker names it while rekordbox changes (review C3) */
    root_outcome o = run_root(x, CORE_ADMIN_RESTORE, NULL, TX(x, T_WIN_RESTORE_NOT_DONE), 1);
    if (!fx_ok(x)) return;
    LOG(x, "restore: ", o.kind == ROOT_DONE ? "done" : "not done");
    log_state(x, "state: ");
    fx_idle(x);
}

/* main: "uninstall_from_settings" (Settings > Apps' Uninstall); 1 when it was that */
static int win_flows_event(core *c, const char *id, size_t n) {
    static const char name[] = "uninstall_from_settings";
    if (n != sizeof name - 1 || memcmp(id, name, n)) return 0;
    flows_state *fl = &c->fl;
    if (c->flow_running || c->action_running || !fl->startup_done) {
        fl->win_uninstall_asked = 1;   /* after the start-up flow, or the flow running now */
        core_log(c->life, "core: the uninstall from Settings > Apps waits for the flow running now");
        return 1;
    }
    if (start_flow(c, F_UNINSTALL_ALL)) core_log(c->life, "core: the action couldn't start");
    return 1;
}

/* main, a flow's end: the update chosen in the update-first offer, or the uninstall Settings >
 * Apps asked for meanwhile */
static void win_flow_ended(core *c) {
    flows_state *fl = &c->fl;
    if (c->action_running) return;
    if (fl->win_update_after) {
        fl->win_update_after = 0;
        if (start_flow(c, F_UPDATE)) core_log(c->life, "core: the update couldn't start");
    } else if (fl->win_uninstall_asked && fl->startup_done) {
        fl->win_uninstall_asked = 0;
        if (start_flow(c, F_UNINSTALL_ALL)) core_log(c->life, "core: the action couldn't start");
    }
}
