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
 *   - R1's "Put Back Pioneer's File" (click "restore_pioneer"): the worker's restore verb; beside
 *     it "Don't Ask Again for This Version" (click "r1_dismiss", N10): the watcher doesn't open
 *     the app for R1 in that version any more.
 *   - Settings > Apps' Uninstall: the Apps & features entry starts the app with its uninstall
 *     argument, and the shell sends {"ev":"click","id":"uninstall_from_settings"}: the complete
 *     uninstall, as the menu's, once the start-up flow is done.
 *   - Stems Cache already covers every eligible version: the fresh scope is empty and the
 *     account can opt in without elevation.
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
    x->win_rb_open_versions = rc == 0 && *open ? fact_str(&f, FACT_WIN_RB_OPEN_VERSIONS) : NULL;
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

static const char *win_join_versions(fx *x, const char *const *versions, int n) {
    const char *joined;
    COMPOSE(x, joined, w_win_version_list(B, versions, n));
    return joined;
}

static int win_read_versions(fx *x, const char *list, const char **joined) {
    const char *versions[WIN_ROWS_MAX];
    int n = 0;
    *joined = "";
    for (const char *s = list; s && *s;) {
        const char *end = strchr(s, '\n');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        if (n == WIN_ROWS_MAX || !win_version_ok(s, len)) return 0;
        const char *v = arena_strdup(&x->a, s, len);
        if (!v) return 0;
        versions[n++] = v;
        s = end ? end + 1 : NULL;
    }
    if (n) *joined = win_join_versions(x, versions, n);
    return **joined ? n : 0;
}

/* After the wait, the lingering process gets the closing message. Known versions also
 * identify the system-wide gate when a process is still open. */
static const char *win_rb_open_text(fx *x, core_text_id id) {
    if (x->os != CORE_OS_WIN) return TX(x, id);
    if (x->win_rb_closing) {
        if (id == T_RB_OPEN_TRY_AGAIN) return TX(x, T_WIN_STILL_CLOSING_TRY);
        if (id == T_RB_OPEN_MSG) return TX(x, T_WIN_STILL_CLOSING_MSG);
    }
    const char *versions;
    if (win_read_versions(x, x->win_rb_open_versions, &versions)) {
        if (id == T_RB_OPEN_TRY_AGAIN) return TX1(x, T_WIN_RB_OPEN_VERSION_TRY, SL(rekordbox, versions));
        if (id == T_RB_OPEN_MSG) return TX1(x, T_WIN_RB_OPEN_VERSION_MSG, SL(rekordbox, versions));
    }
    return TX(x, id);
}

static int win_scope_read(fx *x, const char *verb, core_text_id title, win_scope *scope) {
    core_facts f;
    int32_t rc = fx_step(x, CORE_STEP_WIN_SCOPE, &f, 1, verb);
    if (!fx_ok(x)) return 0;
    if (rc || fact_i64(&f, FACT_STEP_CODE, 1) || !f.set[FACT_WIN_SCOPE]) {
        alert_ok(x, title, TX(x, T_WIN_CACHE_CHECKING));
        return 0;
    }
    *scope = (win_scope){fact_str(&f, FACT_WIN_SCOPE), fact_str(&f, FACT_WIN_SCOPE_VERSIONS), fact_str(&f, FACT_WIN_SCOPE_AFFECTED)};
    return 1;
}

static int win_cache_opt_in(fx *x, const core_facts *f, const win_scope *scope) {
    if (scope->token[0] || !fact_flag(f, FACT_STEMS_CACHE_PRESENT)) return 0;
    if (fact_flag(f, FACT_CACHE_RECORD_OK)) return 1;
    LOG(x, "Stems Cache: full coverage couldn't be verified");
    return 0;
}

static int win_rows_at_least(const core_facts *f, int state, int count) {
    win_row rows[WIN_ROWS_MAX];
    int n = win_rows_parse(fact_str(f, FACT_WIN_RB_ROWS), f->len[FACT_WIN_RB_ROWS], rows, WIN_ROWS_MAX);
    for (int i = 0; i < n; i++)
        if (rows[i].state == state && --count == 0) return 1;
    return 0;
}

static int win_install_confirm(fx *x, const action *a, const win_scope *scope, int retry) {
    const char *versions, *note;
    int count = win_read_versions(x, scope->versions, &versions);
    int64_t floor = a->win_confirm ? a->win_floor : -1;
    if (count >= 2 || (retry && count == 1)) {
        note = a->again ? TX2(x, T_WIN_REINSTALL_NOTE_VERSIONS, SL(features, TX(x, T_FEATURE_CACHE)), SL(rekordbox, versions))
                        : TX1(x, T_WIN_INSTALL_NOTE_CACHE_VERSIONS, SL(rekordbox, versions));
        if (a->model && !retry) note = CAT(x, install_note(x, 1, 0, a->again, -1, NULL), " ", note);
        if (floor >= 0) {
            const char *paused;
            COMPOSE(x, paused, w_cache_paused_at_install(B, floor, a->win_drive));
            note = CAT(x, note, " ", paused);
        }
    } else note = install_note(x, a->model && !retry, 1, a->again, floor, a->win_drive);
    return confirm(x, a->label, TX(x, a->again ? T_REINSTALL : T_INSTALL), note);
}

static const char *win_uninstall_note(fx *x, const win_scope *scope, const char *fallback) {
    const char *versions;
    return win_read_versions(x, scope->affected, &versions) >= 2 ?
        TX1(x, T_WIN_UNINSTALL_NOTE_CACHE_VERSIONS, SL(rekordbox, versions)) : fallback;
}

typedef struct {
    const char *done, *failed;
    int installed, first_code, root_kept, answered;
} win_batch_result;

static int win_target_code(const char *s, const char *end) {
    if (s == end || end - s > 2) return -1;
    int code = 0;
    for (; s < end; s++) {
        if (*s < '0' || *s > '9') return -1;
        code = code * 10 + *s - '0';
    }
    return code;
}

static win_batch_result win_batch_read(fx *x, core_text_id partial) {
    win_batch_result r = {.done = "", .failed = ""};
    core_facts f;
    if (fx_step(x, CORE_STEP_WIN_ADMIN_TARGETS, &f, 0) || !fx_ok(x)) return r;
    r.root_kept = fact_flag(&f, FACT_STEP_FLAG);
    r.answered = f.set[FACT_STEP_FLAG];
    const char *done[WIN_ROWS_MAX], *failed[WIN_ROWS_MAX];
    int nd = 0, nf = 0;
    for (const char *s = fact_str(&f, FACT_STEP_TEXT); *s && nd + nf < WIN_ROWS_MAX;) {
        const char *end = strchr(s, '\n');
        if (!end) break;
        const char *t = memchr(s, '\t', (size_t)(end - s));
        const char *c = t ? memchr(t + 1, '\t', (size_t)(end - t - 1)) : NULL;
        const char *start = s;
        s = end + 1;
        if (!c || !win_version_ok(start, (size_t)(t - start))) continue;
        int code = win_target_code(t + 1, c);
        const char *what = c + 1;
        size_t len = (size_t)(end - what);
        int installed = len == 9 && !memcmp(what, "installed", len);
        int already = len == 7 && !memcmp(what, "already", len);
        int put_back = len == 8 && !memcmp(what, "put-back", len);
        int good = partial == T_WIN_CACHE_PARTIAL_INSTALLED ? installed || already : put_back || already;
        const char *version = arena_strdup(&x->a, start, (size_t)(t - start));
        if (!version) break;
        if (code == 0 && good) {
            done[nd++] = version;
            r.installed |= installed;
        } else {
            failed[nf++] = version;
            if (code > 0 && !r.first_code) r.first_code = code;
        }
    }
    r.done = win_join_versions(x, done, nd);
    r.failed = win_join_versions(x, failed, nf);
    return r;
}

static const char *win_batch_message(fx *x, const win_batch_result *r, core_text_id id, const char *fallback) {
    const char *why;
    COMPOSE(x, why, w_win_batch_why(B, r->first_code, CORE_PROGRAMS(&x->life->c)));
    if (r->root_kept) why = r->first_code ? CAT(x, why, " ", TX(x, T_ROOT_FOLDER_FAILURE)) : TX(x, T_ROOT_FOLDER_FAILURE);
    if (r->done[0] && r->failed[0])
        return tx_(x, id, (const core_slot[]){SL(done, r->done), SL(failed, r->failed), SL(why, why)}, 3);
    if (!fallback || (r->root_kept && !r->failed[0])) return why;
    return TX2(x, T_FAILURE_WHY, SL(message, fallback), SL(why, why));
}

static int win_install_root(fx *x, const action *a, root_args *ra) {
    core_text_id title = a->again ? T_REINSTALL_FAILED : T_INSTALL_FAILED;
    core_text_id failed = a->again ? T_CACHE_NOT_REINSTALLED : T_CACHE_NOT_INSTALLED;
    int kind = a->again ? CORE_ADMIN_CACHE_REINSTALL : CORE_ADMIN_CACHE_INSTALL;
    for (int attempt = 0; attempt < 2; attempt++) {
        root_outcome o = run_root(x, kind, ra, TX(x, failed), 1);
        if (!fx_ok(x)) return 0;
        if (o.code == ROOT_WIN_SCOPE_CHANGED) {
            if (attempt) {
                alert_ok(x, title, TX(x, T_WIN_SCOPE_CHANGED_MSG));
                return 0;
            }
            win_scope scope;
            if (!win_scope_read(x, a->force ? "reinstall" : "install", title, &scope)) return 0;
            if (!scope.token[0]) {
                core_facts f;
                if (!a->force && fx_facts(x, &f) && win_cache_opt_in(x, &f, &scope)) {
                    if (!win_install_confirm(x, a, &scope, 1) || !fx_facts(x, &f)) return 0;
                    if (win_cache_opt_in(x, &f, &scope)) return 1;
                }
                if (fx_ok(x)) alert_ok(x, title, TX(x, T_WIN_CACHE_NO_TARGETS));
                return 0;
            }
            if (!win_install_confirm(x, a, &scope, 1)) return 0;
            ra->win_scope = scope.token;
            continue;
        }
        if (o.code == ROOT_WIN_PARTIAL) {
            win_batch_result batch = win_batch_read(x, T_WIN_CACHE_PARTIAL_INSTALLED);
            alert_ok(x, title, win_batch_message(x, &batch, T_WIN_CACHE_PARTIAL_INSTALLED, TX(x, failed)));
            return batch.installed;
        }
        core_facts f;
        return o.kind == ROOT_DONE && fx_facts(x, &f) && fact_flag(&f, FACT_STEMS_CACHE_PRESENT);
    }
    return 0;
}

static cache_removal win_run_uninstall(fx *x, const char *failure, const char *root_folder_failure) {
    win_scope *scope = &x->win_uninstall_scope;
    if (!scope->token && !win_scope_read(x, "uninstall", T_UNINSTALL_FAILED, scope))
        return (cache_removal){CR_FAILED, RX_CANCELLED};
    if (strcmp(scope->token, "*")) {
        alert_ok(x, T_UNINSTALL_FAILED, TX(x, T_WIN_CACHE_CHECKING));
        return (cache_removal){CR_FAILED, RX_CANCELLED};
    }
    int kind = CORE_ADMIN_CACHE_UNINSTALL;
    if (!scope->affected[0]) {
        core_facts f;
        if (fx_step(x, CORE_STEP_CACHE_FOLDERS, &f, 0) == 0 &&
            !fact_flag(&f, FACT_STEP_FLAG) && fact_flag(&f, FACT_STEP_FLAG2)) {
            if (!offer_root_folder_cleanup(x)) return (cache_removal){CR_KEPT_ROOT_FOLDER, 0};
            kind = CORE_ADMIN_REMOVE_ROOT_FOLDER;
        }
    }
    root_args ra = {.win_scope = scope->token};
    root_outcome o = run_root(x, kind, &ra, kind == CORE_ADMIN_CACHE_UNINSTALL ? failure : root_folder_failure, 1);
    if (o.code == ROOT_WIN_SCOPE_CHANGED) {
        alert_ok(x, T_UNINSTALL_FAILED, TX(x, T_WIN_SCOPE_CHANGED_MSG));
        return (cache_removal){CR_FAILED, RX_CANCELLED};
    }
    if (o.kind == ROOT_DONE || o.code == ROOT_WIN_PARTIAL) {
        win_batch_result batch = win_batch_read(x, T_WIN_CACHE_PARTIAL_REMOVED);
        if (o.code == ROOT_WIN_PARTIAL || batch.root_kept || !batch.answered) {
            /* Complete uninstall already supplies the failure sentence. */
            x->win_removal_partial = win_batch_message(x, &batch, T_WIN_CACHE_PARTIAL_REMOVED, failure);
            if (failure) alert_ok(x, T_UNINSTALL_FAILED, x->win_removal_partial);
            return (cache_removal){CR_FAILED, ROOT_WIN_PARTIAL};
        }
        return (cache_removal){CR_REMOVED, 0};
    }
    return (cache_removal){CR_FAILED, o.code};
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
static int win_confirm_restore(fx *x, const win_scope *scope, int retry) {
    if (!scope->token[0]) return 0;
    const char *versions;
    int n = win_read_versions(x, scope->versions, &versions);
    if (!n) n = win_read_versions(x, scope->affected, &versions);
    const char *msg = n >= 2 ? TX1(x, T_WIN_RESTORE_MSG_VERSIONS, SL(rekordbox, versions)) :
        n == 1 && retry ? TX1(x, T_WIN_RESTORE_MSG_VERSION, SL(rekordbox, versions)) : TX(x, T_WIN_RESTORE_MSG);
    if (!x->snapped && !guard_snap(x, NULL)) return 0;
    guard_arm(x);
    int b = ASK(x, T_WIN_RESTORE_Q, msg, 2, TX(x, T_WIN_BTN_RESTORE), TX(x, T_CANCEL));
    return disarm_guard(x, b) == 0;
}

static void win_restore_flow(fx *x) {
    win_scope scope;
    if (!preflight(x) || !guard_snap(x, NULL) || !win_scope_read(x, "restore", T_WIN_RESTORE_FAILED, &scope)) return;
    if (!scope.token[0]) {
        LOG(x, "restore: no version to put back");
        alert_ok(x, T_WIN_RESTORE_FAILED, TX(x, T_WIN_RESTORE_NOT_DONE));
        return;
    }
    if (!win_confirm_restore(x, &scope, 0)) return;
    LOG(x, "restore: putting Pioneer's onnxruntime.dll back");
    fx_start(x, "restore", TX(x, T_WIN_BTN_RESTORE), T_WIN_RESTORING);
    root_args ra = {.win_scope = scope.token};
    root_outcome o = run_root(x, CORE_ADMIN_RESTORE, &ra, TX(x, T_WIN_RESTORE_NOT_DONE), 1);
    if (o.code == ROOT_WIN_SCOPE_CHANGED && win_scope_read(x, "restore", T_WIN_RESTORE_FAILED, &scope) &&
        win_confirm_restore(x, &scope, 1)) {
        ra.win_scope = scope.token;
        o = run_root(x, CORE_ADMIN_RESTORE, &ra, TX(x, T_WIN_RESTORE_NOT_DONE), 1);
        if (o.code == ROOT_WIN_SCOPE_CHANGED) alert_ok(x, T_WIN_RESTORE_FAILED, TX(x, T_WIN_SCOPE_CHANGED_MSG));
    }
    if (!fx_ok(x)) return;
    if (o.code == ROOT_WIN_PARTIAL) {
        win_batch_result batch = win_batch_read(x, T_WIN_RESTORE_PARTIAL);
        alert_ok(x, T_WIN_RESTORE_FAILED, win_batch_message(x, &batch, T_WIN_RESTORE_PARTIAL, TX(x, T_WIN_RESTORE_NOT_DONE)));
    }
    LOG(x, "restore: ", o.kind == ROOT_DONE ? "done" : "not done");
    log_state(x, "state: ");
    fx_idle(x);
}

/* main: R1 asked again once Don't Ask Again has been written (its button goes) */
static void win_r1_recheck_job(core *c, void *payload) {
    (void)payload;
    view_r1_check(c);
}

/* R1's "Don't Ask Again for This Version" / "for These Versions" (app.md §5.2, §5.7; review
 * WCM3, W14b 6; the grouped ruling): the Mac watcher's T_W_DONT_ASK answer for R1. No question
 * first, as the Mac's button is itself the answer. The host adds "r1:<version>" to dismissed for
 * each version the button showed at the click (x->win_r1_shown) that still needs repair now,
 * keeping its other lines, so the watcher doesn't open RB Stems Plus for R1 in those versions. A
 * version the button didn't show is never written. Not an action: rekordbox's files don't change,
 * the line and Put Back stay. A write that fails is logged, as the watcher's "couldn't remember
 * the answer"; the button then stays. */
static void win_r1_dismiss_flow(fx *x) {
    core_facts f;
    LOG(x, "user chose: ", TX(x, strchr(x->win_r1_shown, ',') ? T_WIN_BTN_R1_DISMISS_MANY : T_W_DONT_ASK), " (R1)");
    if (!x->win_r1_shown[0]) {
        LOG(x, "  the versions shown aren't the ones it covers now: not remembered");
        core_post_main(x->life, win_r1_recheck_job, NULL, NULL, 0, 0);
        return;
    }
    int32_t rc = fx_step(x, CORE_STEP_WIN_R1_DISMISS, &f, 1, x->win_r1_shown);
    if (!fx_ok(x)) return;
    if (rc == 0) LOG(x, "  remembered: the watcher won't open RB Stems Plus for R1 in rekordbox ", fact_str(&f, FACT_STEP_TEXT));
    else if (rc == 1) LOG(x, "  nothing to put back now: not remembered");
    else LOG(x, "  couldn't remember the answer");
    core_post_main(x->life, win_r1_recheck_job, NULL, NULL, 0, 0);
}

/* 1 when the comma-separated list has the version v[0..n) */
static int win_list_has(const char *list, const char *v, size_t n) {
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == n && !memcmp(p, v, n)) return 1;
        p += k + (e != NULL);
    }
    return 0;
}

/* main: {"ev":"r1_dismiss"}: the versions the button showed at the click, then the click. Only
 * those the button covers now too go to the flow (shown and current: a row that appeared since,
 * or one fixed since, isn't remembered) */
void flows_win_r1_dismiss(core *c, const char *versions, size_t n) {
    flows_state *fl = &c->fl;
    size_t at = 0;
    fl->win_r1_shown[0] = 0;
    for (size_t i = 0; i < n;) {
        size_t k = 0;
        while (i + k < n && versions[i + k] != ',') k++;
        if (k && win_list_has(c->v.win_r1_dismiss_versions, versions + i, k) && !win_list_has(fl->win_r1_shown, versions + i, k)
            && at + k + 2 <= sizeof fl->win_r1_shown) {
            if (at) fl->win_r1_shown[at++] = ',';
            memcpy(fl->win_r1_shown + at, versions + i, k);
            fl->win_r1_shown[at += k] = 0;
        }
        i += k + 1;
    }
    flows_event(c, "r1_dismiss", sizeof "r1_dismiss" - 1);
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
