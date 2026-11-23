/* The Windows flow variants (W-core, core/flows_win.c) on flows_test.c's fake host, with a core
 * started "os":"win". Included at the end of flows_test.c under CORE_WIN_POLICY (its fake host is
 * static there); not compiled on its own. The Mac flows run in the same build too (flows_test.c's
 * own tests, "os":"mac"), so a Windows variant that leaked into the Mac's path fails there. */

static const char *const CONFIG_WIN = "{\"version\":\"1.4.0\",\"os\":\"win\",\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";

#include "../native.h"

static void ffw_init(ff *f) {
    ff_config = CONFIG_WIN;
    ff_init(f);
    ff_config = NULL;
}

/* a frame whose status line starts with piece */
static int frame_starts(const ff *f, const char *piece) {
    size_t n = strlen(piece);
    for (const char *p = f->frames; *p;) {
        if (!strncmp(p, piece, n)) return 1;
        const char *e = strchr(p, '\n');
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* one elevation per attempt: no "Permission needed", no password to forget, no bundle id */
static void t_win_install_cache(void) {
    ff f;
    ffw_init(&f);
    CHECK(f.c && f.c->os == CORE_OS_WIN);
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "Install Stems Cache?"));
    CHECK(in_order(&f, 4, "log admin run (install), attempt 1",
                   "admin 0 7.2.10:1111111111111111 - 2222222222222222222222222222222222222222222222222222222222222222 ortsha 0", "step SET_CHOSEN 0 1",
                   "step LAUNCH_AGENT_LOAD"));
    CHECK(count_of(&f, "\nadmin 4 ") == 0 && count_of(&f, "\nadmin ") == 1);
    CHECK(frame(&f, "Installing Stems Cache (this takes about a minute)…|1|2") && f.chosen_cache);
    ended_clean(&f, "win install cache");
    ff_end(&f);
    /* "Operation not permitted" has no meaning here: one attempt, the plain failure */
    ffw_init(&f);
    f.nadmin_rc = 2;
    f.admin_rc[0] = 4 | FAKE_NP;
    f.admin_rc[1] = 0;
    answers(&f, 2, "Install", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.admin_attempts == 1 && count_of(&f, "Permission needed") == 0);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_title[1], "Install failed") && !strcmp(f.dlg_msg[1], "Stems Cache couldn't be installed. See the log in this window."));
    ended_clean(&f, "win install fails");
    ff_end(&f);

    ffw_init(&f);
    f.nadmin_rc = 1;
    f.admin_rc[0] = ROOT_WIN_START_TIMEOUT;
    answers(&f, 2, "Install", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.admin_attempts == 1 && !f.chosen_cache);
    CHECK(!frame_starts(&f, "Installing Stems Cache (this takes about a minute)…"));
    CHECKS(f.ndialogs == 2 && !strcmp(f.dlg_title[1], "Administrator step didn't start"), f.dlg_title[1]);
    CHECKS(!strcmp(f.dlg_msg[1], "Windows didn't start the administrator step in time. Try again."), f.dlg_msg[1]);
    ended_clean(&f, "admin worker timed out before connecting");
    ff_end(&f);
}

/* Stems Cache already in rekordbox (owner, 2026-11-17): turned on with no elevation only when the
 * live DLL is the payload's bridge and the worker's receipt binds it; else the
 * worker runs again (one UAC prompt: an interrupted install, or an older bridge) */
static void t_win_cache_opt_in(void) {
    static const char *const OPTED = "log Stems Cache: already in rekordbox (installed from another account): turned on for this account";
    ff f;
    /* the record and the payload's bridge: turned on, no admin run, nothing downloaded */
    ffw_init(&f);
    f.cache = f.traces = f.cache_record_ok = 1;
    f.bridge_installed = f.bridge_sha;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && has(&f, OPTED) && f.chosen_cache);
    CHECK(count_of(&f, "\nadmin ") == 0 && f.admin_attempts == 0 && count_of(&f, "step DOWNLOAD") == 0);
    CHECK(in_order(&f, 3, "step WIN_SCOPE install", OPTED, "step SET_CHOSEN 0 1"));
    ended_clean(&f, "win opt-in");
    ff_end(&f);
    /* the bridge without the record (an interrupted install): the worker runs, with the bridge */
    ffw_init(&f);
    f.cache = f.traces = 1;
    f.bridge_installed = f.bridge_sha;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(!has(&f, OPTED) && f.admin_attempts == 1 && f.chosen_cache);
    CHECK(in_order(&f, 3, "step WIN_SCOPE install", "step DOWNLOAD bridge",
                   "admin 0 7.2.10:1111111111111111 - 2222222222222222222222222222222222222222222222222222222222222222 ortsha 0"));
    ended_clean(&f, "win opt-in, no record");
    ff_end(&f);
    /* the record, but another bridge than the payload's (an older one): the worker runs */
    ffw_init(&f);
    f.cache = f.traces = f.cache_record_ok = 1;
    f.bridge_installed = "3333333333333333333333333333333333333333333333333333333333333333";
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(!has(&f, OPTED) && f.admin_attempts == 1 && f.chosen_cache);
    CHECK(in_order(&f, 2, "step WIN_SCOPE install",
                   "admin 0 7.2.10:1111111111111111 - 2222222222222222222222222222222222222222222222222222222222222222 ortsha 0"));
    ended_clean(&f, "win opt-in, other bridge");
    ff_end(&f);
    /* the live DLL unreadable (BRIDGE_SHA ""): the worker runs */
    ffw_init(&f);
    f.cache = f.traces = f.cache_record_ok = 1;
    f.bridge_installed = "";
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(!has(&f, OPTED) && f.admin_attempts == 1 && f.chosen_cache);
    ff_end(&f);
    /* Reinstall (force) never opts in, record or not */
    ffw_init(&f);
    f.cache = f.traces = f.cache_record_ok = f.chosen_cache = 1;
    f.bridge_installed = f.bridge_sha;
    answers(&f, 1, "Reinstall");
    ev(&f, "{\"ev\":\"click\",\"id\":\"reinstall_cache\"}");
    settle(&f);
    CHECK(!has(&f, OPTED) && !has(&f, "step BRIDGE_SHA") && f.admin_attempts == 1);
    ff_end(&f);
    /* the record gone during the payload step (another account's worker swapped a bridge in and
     * was cut off): read again before BRIDGE_SHA, so not turned on as it is (review OR4) */
    ffw_init(&f);
    f.cache = f.traces = f.cache_record_ok = f.record_lost_at_payload = 1;
    f.bridge_installed = f.bridge_sha;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(!has(&f, OPTED) && f.admin_attempts == 1 && f.chosen_cache);
    CHECK(in_order(&f, 2, "step PAYLOAD latest", "step WIN_SCOPE install"));
    ended_clean(&f, "win opt-in, record gone meanwhile");
    ff_end(&f);
    /* a standard account, no record: refused, and the worker isn't for it (FEAT_NEEDS_ADMIN):
     * said in an alert, no admin run, the choice unchanged (review OR5) */
    ffw_init(&f);
    f.cache = f.traces = 1;
    f.admin_user = 0;
    f.bridge_installed = f.bridge_sha;
    answers(&f, 2, "Install", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(!has(&f, OPTED) && f.admin_attempts == 0 && count_of(&f, "\nadmin ") == 0 && !f.chosen_cache && !has(&f, "step SET_CHOSEN"));
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_title[1], "Install failed") && !strcmp(f.dlg_msg[1], "Stems Cache needs an administrator account."));
    CHECK(in_order(&f, 2, "step WIN_SCOPE install",
                   "log Stems Cache not installed: Stems Cache needs an administrator account."));
    ended_clean(&f, "win opt-in, standard account");
    ff_end(&f);
    /* the Mac's way in the same build: the bridge in rekordbox is enough, no record asked */
    ff_init(&f);
    f.cache = f.traces = 1;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(has(&f, OPTED) && f.admin_attempts == 0 && f.chosen_cache && !has(&f, "step BRIDGE_SHA"));
    ff_end(&f);
}

/* UAC declined (ERROR_CANCELLED): the user stopped, so no alert (app.md §2.2) */
static void t_win_uac_declined(void) {
    ff f;
    ffw_init(&f);
    f.nadmin_rc = 1;
    f.admin_rc[0] = 1223;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && has(&f, "log   UAC declined, or the app is quitting: stopped") && !f.chosen_cache);
    ended_clean(&f, "uac declined");
    ff_end(&f);
    /* in the complete uninstall: it stops there, with no alert either */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = f.rb_folder = 1;
    f.nadmin_rc = 1;
    f.admin_rc[0] = 1223;
    answers(&f, 1, "Uninstall");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && has(&f, "log Stems Cache could not be removed: stopping, nothing else deleted") && !has(&f, "step REMOVE_OWN_FOLDERS 0"));
    ended_clean(&f, "uac declined, uninstall");
    ff_end(&f);
    /* the Mac's way in the same build: a cancelled password is the user stopping too, and the
     * re-sign loop is still there */
    ff_init(&f);
    f.nadmin_rc = 2;
    f.admin_rc[0] = 4 | FAKE_NP;
    f.admin_rc[1] = 0;
    answers(&f, 2, "Install", "Try Again");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Permission needed") && f.admin_attempts == 2 && has(&f, "admin 4 - - - - 0"));
    ff_end(&f);
}

/* the worker's "run again" (13): once more; a second 13 is a failure */
static void t_win_run_again(void) {
    ff f;
    ffw_init(&f);
    f.nadmin_rc = 2;
    f.admin_rc[0] = ROOT_WIN_RUN_AGAIN;
    f.admin_rc[1] = 0;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.admin_attempts == 2 && in_order(&f, 2, "log admin run (install), attempt 1", "log admin run (install), attempt 2"));
    CHECK(f.ndialogs == 1 && f.chosen_cache);
    ended_clean(&f, "run again");
    ff_end(&f);
    ffw_init(&f);
    f.nadmin_rc = 1;
    f.admin_rc[0] = ROOT_WIN_RUN_AGAIN;
    answers(&f, 2, "Install", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.admin_attempts == 2 && f.ndialogs == 2 && !strcmp(f.dlg_title[1], "Install failed"));
    ff_end(&f);
}

/* C2: the worker says rekordbox is open (5) while the core sees it closed (the DLL in use): one
 * elevation per Try Again, Cancel stops with no alert; after the fourth, the failure */
static void t_win_rb_open_for_worker(void) {
    ff f;
    ffw_init(&f);
    f.nadmin_rc = 4;
    for (int i = 0; i < 4; i++) f.admin_rc[i] = ROOT_REKORDBOX_OPEN;
    answers(&f, 4, "Install", "Try Again", "Try Again", "Cancel");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.admin_attempts == 3 && f.ndialogs == 4);
    for (int d = 1; d < 4; d++)
        CHECKS(!strcmp(f.dlg_title[d], "rekordbox open") && !strcmp(f.dlg_buttons[d], "Try Again|Cancel") &&
               !strcmp(f.dlg_msg[d], "Close rekordbox (also in other accounts on this PC), then click Try Again."), "the ask");
    CHECK(in_order(&f, 4, "admin 0 7.2.10:1111111111111111 - 2222222222222222222222222222222222222222222222222222222222222222 ortsha 0",
                   "dialog rekordbox open [Try Again|Cancel] -> Try Again", "log admin run (install), attempt 2",
                   "dialog rekordbox open [Try Again|Cancel] -> Cancel"));
    CHECK(count_of(&f, "Install failed") == 0 && !f.chosen_cache);
    ended_clean(&f, "worker: rekordbox open");
    ff_end(&f);
    ffw_init(&f);
    f.nadmin_rc = 4;
    for (int i = 0; i < 4; i++) f.admin_rc[i] = ROOT_REKORDBOX_OPEN;
    answers(&f, 5, "Install", "Try Again", "Try Again", "Try Again", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.admin_attempts == 4 && f.ndialogs == 5 && !strcmp(f.dlg_title[4], "Install failed"));
    ended_clean(&f, "worker: rekordbox open 4 times");
    ff_end(&f);
}

/* the worker speaks another protocol (12) or its floor refuses this app (14): one offer to update
 * the app first, and the update after this flow */
static void t_win_update_first(void) {
    ff f;
    ffw_init(&f);
    f.nadmin_rc = 1;
    f.admin_rc[0] = ROOT_WIN_PROTOCOL;
    f.app_version = "1.5.0";
    answers(&f, 3, "Install", "Update", "Cancel");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 3 && !strcmp(f.dlg_title[1], "Update RB Stems Plus first") && !strcmp(f.dlg_buttons[1], "Update|Cancel"));
    CHECK(!strcmp(f.dlg_msg[1], "Stems Cache on this PC was installed by a newer RB Stems Plus: update RB Stems Plus, then try again."));
    CHECK(in_order(&f, 4, "dialog Update RB Stems Plus first [Update|Cancel] -> Update", "step BUSY_MARKER", "step PAYLOAD saved",
                   "dialog Update RB Stems Plus to 1.5.0? [Update|Cancel] -> Cancel"));
    CHECK(count_of(&f, "Install failed") == 0 && !f.chosen_cache);
    ended_clean(&f, "update first");
    ff_end(&f);
    /* below the floor, the offer declined: nothing more */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = f.rb_folder = 1;
    f.nadmin_rc = 1;
    f.admin_rc[0] = ROOT_WIN_OLDER;
    f.app_version = "1.5.0";
    answers(&f, 2, "Uninstall", "Cancel");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_title[1], "Update RB Stems Plus first") && count_of(&f, "step PAYLOAD saved") == 1);
    CHECK(!has(&f, "step PAYLOAD latest"));   /* the saved payload.json named the newer app */
    ended_clean(&f, "update first declined");
    ff_end(&f);
    /* C1: no newer app in the saved payload.json, and the latest one names this version, or can't
     * be downloaded: one alert that says so, no offer, no update flow */
    for (int fetch_fails = 0; fetch_fails < 2; fetch_fails++) {
        ffw_init(&f);
        f.cache = f.traces = f.chosen_cache = f.rb_folder = 1;
        f.nadmin_rc = 1;
        f.admin_rc[0] = ROOT_WIN_PROTOCOL;   /* app_version 1.4.0: this app's own */
        f.payload_fetch = fetch_fails ? 2 : 0;
        answers(&f, 2, "Uninstall", "OK");
        ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
        settle(&f);
        const char *ctx = fetch_fails ? "no newer: fetch fails" : "no newer: same version";
        CHECKS(f.ndialogs == 2 && !strcmp(f.dlg_title[1], "Update RB Stems Plus first") && !strcmp(f.dlg_buttons[1], "OK"), ctx);
        CHECKS(!strcmp(f.dlg_msg[1], "Stems Cache on this PC was installed by a newer RB Stems Plus, and no newer version can be downloaded now. Try again later."), ctx);
        CHECKS(in_order(&f, 3, "step PAYLOAD saved", "step PAYLOAD latest", "dialog Update RB Stems Plus first [OK] -> OK"), ctx);
        CHECKS(has(&f, fetch_fails ? "log   the latest payload.json couldn't be downloaded" : "log   the latest payload.json names no newer RB Stems Plus"), ctx);
        CHECKS(!f.c->fl.win_update_after && count_of(&f, "step UPDATE_CHECK") == 0, ctx);
        ended_clean(&f, ctx);
        ff_end(&f);
    }
}

/* rekordbox's process lingers after its window closed (PLAN R1): waited for, and said */
static void t_win_still_closing(void) {
    ff f;
    ffw_init(&f);
    f.rb_closing_n = 2;
    f.admin_still_closing = 1;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(in_order(&f, 4, "log   rekordbox is still closing: waiting", "log   rekordbox has closed", "dialog Install Stems Cache? [Install|Cancel] -> Install",
                   "log admin run (install), attempt 1"));
    CHECK(frame_starts(&f, "rekordbox is still closing…|0|") && frame(&f, "rekordbox is still closing…|1|2"));   /* ours, then the worker's */
    CHECK(f.ndialogs == 1 && f.chosen_cache && count_of(&f, "rekordbox open") == 0);
    ended_clean(&f, "still closing");
    ff_end(&f);
    /* still open (a window): asked to close it, as on the Mac, in Windows words */
    ffw_init(&f);
    f.rb_open_n = 1;
    f.rb_open_from = 1;
    answers(&f, 2, "Install", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "rekordbox open") && !strcmp(f.dlg_msg[0], "Close rekordbox (also in other accounts on this PC), then try again."));
    ff_end(&f);
}

/* frames (whole lines) in this order, others between them allowed */
static int frames_in_order(const ff *f, int n, ...) {
    va_list ap;
    va_start(ap, n);
    const char *p = f->frames;
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        char want[700];
        snprintf(want, sizeof want, "%s\n", va_arg(ap, const char *));
        const char *q = p;
        while ((q = strstr(q, want)) && q != f->frames && q[-1] != '\n') q++;
        if (!q) {
            printf("  missing frame (in order): %s", want);
            ok = 0;
        } else p = q + strlen(want);
    }
    va_end(ap);
    return ok;
}

static const char *status_now(const ff *f) {
    size_t n;
    return f->held ? core_str(f->held, F_STATUS_LINE, &n) : "?";
}

/* between actions the status line is the refresh's (M4: why a feature can't be installed, with
 * this fake's facts), never the wait's line */
#define IDLE_LINE "This copy of RB Stems Plus isn't the installed one: open RB Stems Plus from the Start menu.\n" \
                  "Open rekordbox and turn on STEMS once, so it downloads its STEMS Engine.\n" \
                  "Stems Cache doesn't support rekordbox 7.2.10 yet. RB Stems Plus checks again each time you open it."
static int status_idle(const ff *f) { return !strcmp(status_now(f), IDLE_LINE); }

/* C11-C14: the wait's line comes down on every way out, the worker's "closed" puts the attempt's
 * line back, the wait has one deadline per flow, and after it the texts say rekordbox is closing */
static void t_win_closing_lines(void) {
    ff f;
    /* closing -> closed while an action runs: its own line again */
    ffw_init(&f);
    f.rb_closing_n = 2;
    f.rb_closing_from = 2;   /* preflight's check sees it closed; run_root's, closing */
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(frames_in_order(&f, 3, "Downloading Stems Cache… 50%|1|2", "rekordbox is still closing…|1|2", "Downloading Stems Cache… 50%|1|2"));
    CHECK(has(&f, "log   rekordbox has closed") && f.chosen_cache && status_idle(&f));
    ended_clean(&f, "closing -> closed");
    ff_end(&f);
    /* closing -> the deadline (1 s here): rekordbox counts as open, the alert says it is closing,
     * and the line is gone */
    ffw_init(&f);
    f.c->life->win_closing_max_ms = 1000;
    f.rb_closing_n = 1000;
    answers(&f, 2, "Install", "OK");
    uint64_t t0 = plat_now_mono_ns();
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    uint64_t ms = (plat_now_mono_ns() - t0) / 1000000u;
    CHECK(ms >= 1000 && ms < 3000);   /* the deadline holds (C13) */
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "rekordbox open") && !strcmp(f.dlg_msg[0], "rekordbox hasn't finished closing. Try again in a moment."));
    CHECK(has(&f, "log   rekordbox didn't finish closing") && !strcmp(status_now(&f), ""));
    ended_clean(&f, "closing -> timeout");
    ff_end(&f);
    /* closing -> the check fails: stopped, and the line is gone too */
    ffw_init(&f);
    f.rb_closing_n = 1;
    f.fail_step_at = 2;
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(in_order(&f, 2, "log   rekordbox is still closing: waiting", "log   rekordbox check failed: stopped"));
    CHECK(f.ndialogs == 0 && !strcmp(status_now(&f), "") && frame_starts(&f, "rekordbox is still closing…"));
    ended_clean(&f, "closing -> check fails");
    ff_end(&f);
    /* core_free in the middle of the wait: nothing more asked of the host */
    ffw_init(&f);
    core_life *l = life_retain(f.c->life);
    f.rb_closing_n = 1000;
    f.free_at = 2;
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    for (int i = 0; i < f.nthreads; i++) pthread_join(f.threads[i], NULL);
    f.nthreads = 0;
    CHECK(f.freed && f.calls_after_free == 0 && atomic_load(&l->flows_out) == 0);
    ff_end(&f);
    life_release(l);
    /* the worker's own wait: "still closing", then "closed" puts the attempt's line back */
    ffw_init(&f);
    f.admin_still_closing = 2;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(frames_in_order(&f, 3, "Installing Stems Cache (this takes about a minute)…|1|2", "rekordbox is still closing…|1|2",
                          "Installing Stems Cache (this takes about a minute)…|1|2"));
    ended_clean(&f, "worker closed");
    ff_end(&f);
    /* a resumed action (readyForQueuedAction): the "updating" reads don't wait, one deadline for the
     * flow, and the Try Again ask says rekordbox is still closing (C12, C14) */
    ffw_init(&f);
    f.c->life->win_closing_max_ms = 500;
    f.rb_closing_n = 1000;
    f.leftover_id = "install-cache";
    f.leftover_label = "Install Stems Cache";
    answers(&f, 3, "Continue", "Try Again", "Cancel");
    t0 = plat_now_mono_ns();
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    ms = (plat_now_mono_ns() - t0) / 1000000u;
    CHECK(count_of(&f, "log   rekordbox is still closing: waiting") == 1 && ms < 2500);
    CHECK(f.ndialogs == 3 && !strcmp(f.dlg_msg[1], "rekordbox hasn't finished closing. Click Try Again in a moment.") &&
          !strcmp(f.dlg_msg[2], "rekordbox hasn't finished closing. Click Try Again in a moment."));
    CHECK(has(&f, "log   rekordbox is open, the user cancelled: not started") && status_idle(&f));
    ff_end(&f);
}

static void t_win_restore_empty_scope(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.win_scope = f.win_scope_versions = "";
    answers(&f, 1, "OK");
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}") == CORE_OK);
    settle(&f);
    CHECKS(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "Put back failed") &&
           !strcmp(f.dlg_msg[0], "Pioneer's file couldn't be put back.") &&
           !strcmp(f.dlg_buttons[0], "OK"), f.trace);
    CHECK(!f.admin_attempts);
    ended_clean(&f, "restore, empty scope");
    ff_end(&f);
}

/* R1's "Put Back Pioneer's File": the worker's restore verb for the current rekordbox */
static void t_win_restore(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    answers(&f, 1, "Put Back Pioneer's File");
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}") == CORE_OK);
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "Put back Pioneer's file?") && !strcmp(f.dlg_buttons[0], "Put Back Pioneer's File|Cancel"));
    CHECK(!strcmp(f.dlg_msg[0], "Puts rekordbox's own onnxruntime.dll back. Stems Cache stays off until you reinstall it."));
    CHECK(in_order(&f, 5, "step REKORDBOX", "log restore: putting Pioneer's onnxruntime.dll back", "log admin run (restore), attempt 1",
                   "admin 5 7.2.10:1111111111111111 - - - 0", "log restore: done"));
    CHECK(frame(&f, "Putting back Pioneer's file…|1|2") && count_of(&f, "\nadmin ") == 1);
    /* C3: the busy marker names the restore while the worker changes rekordbox */
    CHECK(in_order(&f, 3, "step BUSY_MARKER restore Put Back Pioneer's File", "admin 5 7.2.10:1111111111111111 - - - 0", "log restore: done"));
    CHECK(count_of(&f, "step BUSY_MARKER restore Put Back Pioneer's File") == 1);   /* fx_start's busy() */
    ff_end(&f);
    /* C3: a restore the last run left half-done: the marker goes with no dialog (R1's check offers it again) */
    ffw_init(&f);
    f.leftover_id = "restore";
    f.leftover_label = "Put Back Pioneer's File";
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(count_of(&f, "Action interrupted") == 0);
    CHECK(in_order(&f, 3, "step LEFTOVER", "log the last run quit during: Put Back Pioneer's File (restore): R1's check offers it again", "step BUSY_MARKER"));
    CHECK(count_of(&f, "\nadmin ") == 0);
    ended_clean(&f, "restore leftover");
    ff_end(&f);
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    answers(&f, 1, "Put Back Pioneer's File");
    ev(&f, "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}");
    settle(&f);
    ended_clean(&f, "restore");
    ff_end(&f);
    /* it fails after a rollback, and rekordbox missing (Windows on D:: the folder its Program Files names): the alerts */
    struct { int32_t rc; const char *title, *msg; } cases[] = {
        {ROOT_ROLLED_BACK, "Put back failed", "Pioneer's file couldn't be put back. rekordbox has its original files again. See the log in this window."},
        {1, "Put back failed", "Pioneer's file couldn't be put back. See the log in this window."},
        {ROOT_REKORDBOX_MISSING, "rekordbox missing", "rekordbox isn't in D:\\Program Files\\rekordbox. Install rekordbox from rekordbox.com, then try again."},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ffw_init(&f);
        snprintf(f.c->win_programs, sizeof f.c->win_programs, "D:\\Program Files");
        f.nadmin_rc = 1;
        f.admin_rc[0] = cases[i].rc;
        answers(&f, 2, "Put Back Pioneer's File", "OK");
        ev(&f, "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}");
        settle(&f);
        CHECKS(f.ndialogs == 2 && !strcmp(f.dlg_title[1], cases[i].title) && !strcmp(f.dlg_msg[1], cases[i].msg), cases[i].title);
        ended_clean(&f, cases[i].title);
        ff_end(&f);
    }
    /* rekordbox gone, Windows on E:: the root folder alone, the question naming E:'s rekordbox folder */
    ffw_init(&f);
    snprintf(f.c->win_programs, sizeof f.c->win_programs, "E:\\Program Files");
    f.cache = 0;
    f.chosen_cache = f.traces = 1;
    f.rb_folder = 0;
    f.root_folder = 1;
    answers(&f, 2, "Uninstall", "Remove");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_title[1], "rekordbox missing")
          && !strcmp(f.dlg_msg[1], "rekordbox isn't in E:\\Program Files\\rekordbox. RB Stems Plus can remove the copy of rekordbox's files it kept; "
                                   "Pioneer's installer can always reinstall rekordbox."));
    ended_clean(&f, "root folder (E:)");
    ff_end(&f);
    /* cancelled: nothing asked of the worker */
    ffw_init(&f);
    answers(&f, 1, "Cancel");
    ev(&f, "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}");
    settle(&f);
    CHECK(count_of(&f, "\nadmin ") == 0 && !has(&f, "log restore: putting Pioneer's onnxruntime.dll back"));
    ended_clean(&f, "restore cancelled");
    ff_end(&f);
    /* the Mac's core has no such button */
    ff_init(&f);
    ev(&f, "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}");
    settle(&f);
    CHECK(f.ndialogs == 0 && count_of(&f, "step ") == 0);
    ff_end(&f);
}

/* the complete uninstall on Windows: Windows words, no Dock, the restart line only when it applies */
static void t_win_uninstall_all(void) {
    for (int restart = 0; restart < 2; restart++) {
        ff f;
        ffw_init(&f);
        f.cache = f.traces = f.chosen_cache = f.rb_folder = 1;
        f.admin_restart = restart;
        f.traces_go = 1;
        f.loc_dir = "\\\\?\\Volume{0b1c}\\RB Stems Plus Cache";
        f.loc_drive = f.loc_name = "GLS";
        f.loc_shown = "E:\\RB Stems Plus Cache";
        f.loc_size_kind = 1;
        f.loc_size = 1288490189;
        f.loc_gate_id = "ABC";
        f.app_fate = 0;
        f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
        answers(&f, 2, "Uninstall", "Done");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        CHECK(!strcmp(f.dlg_msg[0], "rekordbox goes back to exactly how it was. Your music and rekordbox library aren't touched.\n\n"
                                    "RB Stems Plus will be deleted, including all stems saved in its folder on \"GLS\" (about 1.2 GB), also any saved there by other computers or accounts."));
        CHECK(in_order(&f, 4, "admin 2 * - - - 0", "step LAUNCH_AGENT_UNLOAD", "step REMOVE_OWN_FOLDERS 0",
                       "step SELF_DELETE C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus 0"));
        CHECK(!has(&f, "step DOCK C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus") && count_of(&f, "\nadmin 4 ") == 0);
        CHECK(!strcmp(f.dlg_title[1], "RB Stems Plus is uninstalled"));
        CHECK(!strcmp(f.dlg_msg[1], restart ? "rekordbox is back to how it was. One file of RB Stems Plus is removed when Windows restarts." : "rekordbox is back to how it was."));
        CHECK(f.quit_steps == 1);
        if (f.quit_steps != 1) DUMP(&f);
        ff_end(&f);
    }
    {   /* the cleanup's stub couldn't start: the app stays open, idle, and says so in the Windows words */
        ff f;
        ffw_init(&f);
        f.cache = f.traces = f.chosen_cache = f.rb_folder = 1;
        f.traces_go = 1;
        f.app_fate = 0;
        f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
        f.self_delete_rc = 1;
        answers(&f, 3, "Uninstall", "Done", "OK");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        CHECK(in_order(&f, 4, "step SELF_DELETE C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus 0",
                       "log self-delete couldn't start: RB Stems Plus stays open", "step BUSY_MARKER",
                       "dialog RB Stems Plus is uninstalled [OK] -> OK"));
        CHECK(f.ndialogs == 3 && !strcmp(f.dlg_buttons[2], "OK")
              && !strcmp(f.dlg_msg[2], "RB Stems Plus's own folder couldn't be removed: delete it in File Explorer."));
        CHECK(f.quit_steps == 0 && f.held && core_flag(f.held, F_SPINNER) == 0);
        if (f.ndialogs != 3) DUMP(&f);
        ended_clean(&f, "win self-delete failed");
        ff_end(&f);
    }
    /* C17: another account's Stems Cache kept: the app is per account on Windows, so nothing is
     * said about other accounts installing it again */
    ff f;
    ffw_init(&f);
    f.cache = f.traces = 1;
    f.chosen_cache = 0;
    f.app_fate = 0;
    f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
    answers(&f, 3, "Uninstall", "Keep", "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(!strcmp(f.dlg_title[1], "Stems Cache from another account") && !strcmp(f.dlg_buttons[1], "Remove|Keep"));
    CHECK(!strcmp(f.dlg_title[2], "RB Stems Plus is uninstalled") &&
          !strcmp(f.dlg_msg[2], "rekordbox has its own stems model again. Stems Cache stays for the other account."));
    CHECK(f.quit_steps == 1);
    ff_end(&f);
}

/* reviews NR2, N3R16: with the drive unplugged and nameless, the confirmation and done alert say
 * "Stems Cache's drive"; the folder in the done alert is relative to that drive */
static void t_win_uninstall_nameless_drive(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.traces = 1;
    f.chosen_cache = 0;
    f.loc_dir = "\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0}\\Music\\RB Stems Plus Cache.noindex";
    f.loc_shown = "Music\\RB Stems Plus Cache.noindex";
    f.loc_drive = f.loc_name = "";
    f.app_fate = 0;
    f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
    answers(&f, 3, "Uninstall", "Keep", "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECKS(f.ndialogs == 3 && strstr(f.dlg_msg[0], "RB Stems Plus will be deleted. Its stems on Stems Cache's drive, which isn't connected, stay: "
                                                   "connect it first to delete them too.") && !strstr(f.dlg_msg[0], "\"\""), f.dlg_msg[0]);
    CHECKS(!strcmp(f.dlg_msg[2], "rekordbox has its own stems model again. Stems Cache stays for the other account. The stems saved on "
                                 "Stems Cache's drive stay. The last known folder was \"Music\\RB Stems Plus Cache.noindex\"."), f.dlg_msg[2]);
    ff_end(&f);
}

/* review N3R16: the named-drive texts use the path for File Explorer; the deletion gate still
 * uses the volume GUID and the marker id the confirmation named */
static void t_win_uninstall_drive_path(void) {
    static const char dir[] = "\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0}\\Music\\RB Stems Plus Cache.noindex";
    static const char mounted[] = "E:\\Music\\RB Stems Plus Cache.noindex", relative[] = "Music\\RB Stems Plus Cache.noindex";
    static const struct {
        const char *first_id, *last_id, *shown, *text, *last_identity;
        int tries_delete, remove_code;
    } cases[] = {
        {"ABC", "abc", mounted, "Stems Cache cleanup on \"GLS\" couldn't be completed. The last known folder was", NULL, 1, 7},
        {"ABC", "abc", mounted, "The stems saved on \"GLS\" stay. The last known folder was", NULL, 1, 6},
        {NULL, NULL, relative, "The stems saved on \"GLS\" stay. The last known folder was", NULL, 0, 0},
        {"ABC", "ABD", mounted, "The stems saved on \"GLS\" stay. The last known folder was", NULL, 0, 0},
        {"ABC", "abc", mounted, "The stems saved on \"GLS\" stay. The last known folder was",
         "000000000000000100000000000000020000000000000004", 0, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ff f;
        ffw_init(&f);
        f.loc_dir = dir;
        f.loc_shown = cases[i].shown;
        f.loc_drive = f.loc_name = "GLS";
        f.loc_gate_id = cases[i].first_id;
        f.loc2_from = 2;
        f.loc2_gate_id = cases[i].last_id;
        f.loc2_identity = cases[i].last_identity;
        f.remove_location_rc = 1;
        f.remove_location_code = cases[i].remove_code;
        f.app_fate = 0;
        answers(&f, 2, "Uninstall", "Done");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        char expected[256], remove[160];
        snprintf(expected, sizeof expected, "%s \"%s\".", cases[i].text, cases[i].shown);
        snprintf(remove, sizeof remove, "step REMOVE_LOCATION %s abc 1 " TEST_LOCATION_IDENTITY, dir);
        CHECKS(f.ndialogs == 2 && strstr(f.dlg_msg[1], expected)
               && count_of(&f, "step REMOVE_LOCATION") == cases[i].tries_delete
               && (!cases[i].tries_delete || has(&f, remove)), f.dlg_msg[1]);
        ff_end(&f);
    }
}

/* Finding 6: Windows native result codes 1-6 (pre-deletion refusal) route to the kept-stems wording,
 * while code 7 (cleanup failure) uses the incomplete-cleanup wording; Mac stays unchanged */
static void t_win_remove_location_codes(void) {
    for (int named = 0; named < 2; named++) {
        for (int code = 1; code <= 7; code++) {
            ff f;
            ffw_init(&f);
            f.loc_dir = "\\\\?\\Volume{0b1c}\\RB Stems Plus Cache";
            f.loc_drive = f.loc_name = named ? "GLS" : "";
            f.loc_shown = "E:\\RB Stems Plus Cache";
            f.loc_gate_id = "ABC";
            f.remove_location_rc = 1;
            f.remove_location_code = code;
            f.app_fate = 0;
            answers(&f, 2, "Uninstall", "Done");
            ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
            settle(&f);
            const char *expected = code <= 6
                ? (named ? "The stems saved on \"GLS\" stay. The last known folder was \"E:\\RB Stems Plus Cache\"."
                         : "The stems saved on Stems Cache's drive stay. The last known folder was \"E:\\RB Stems Plus Cache\".")
                : (named ? "Stems Cache cleanup on \"GLS\" couldn't be completed. The last known folder was \"E:\\RB Stems Plus Cache\"."
                         : "Stems Cache cleanup on its drive couldn't be completed. The last known folder was \"E:\\RB Stems Plus Cache\".");
            CHECKS(f.ndialogs == 2 && strstr(f.dlg_msg[1], expected) != NULL, f.dlg_msg[1]);
            ff_end(&f);
        }
    }
    /* Mac keeps its Finder wording even with code 4 */
    ff f;
    ff_init(&f);
    f.loc_dir = f.loc_shown = "/Volumes/GLS/RB Stems Plus Cache";
    f.loc_drive = f.loc_name = "GLS";
    f.loc_gate_id = "ABC";
    f.remove_location_rc = 1;
    f.remove_location_code = 4;
    f.app_fate = 0;
    answers(&f, 2, "Uninstall", "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECKS(f.ndialogs == 2 && strstr(f.dlg_msg[1],
           "Some of the stems saved on \"GLS\" couldn't be deleted: delete the folder \"/Volumes/GLS/RB Stems Plus Cache\" in Finder.") != NULL,
           f.dlg_msg[1]);
    ff_end(&f);
}

/* no `""` in any dialog, frame or label line: a drive with no name is never shown as `drive ""` */
static int no_empty_quotes(const ff *f) {
    for (int i = 0; i < f->ndialogs && i < 32; i++)
        if (strstr(f->dlg_title[i], "\"\"") || strstr(f->dlg_msg[i], "\"\"") || strstr(f->dlg_buttons[i], "\"\"")) {
            printf("  dialog %d: %s / %s\n", i, f->dlg_title[i], f->dlg_msg[i]);
            return 0;
        }
    if (strstr(f->frames, "\"\"") || strstr(f->trace, "label Stems Cache location: \n")) {
        DUMP(f);
        return 0;
    }
    return 1;
}

/* N3R20: a measured drive uses the existing texts. An unchecked drive stays without a gate. */
static void t_win_uninstall_drive_measurement(void) {
    static const struct {
        int measured;
        const char *drive, *gate_id, *confirmation, *completion;
    } cases[] = {
        {1, "STICK", "ABC",
         "RB Stems Plus will be deleted, including all stems saved in its folder on \"STICK\", also any saved there by other computers or accounts.",
         "Stems Cache cleanup on \"STICK\" couldn't be completed. The last known folder was \"E:\\rbstemsplus\"."},
        {1, "STICK", NULL,
         "RB Stems Plus will be deleted. Its stems on \"STICK\", which isn't connected, stay: connect it first to delete them too.",
         "The stems saved on \"STICK\" stay. The last known folder was \"E:\\rbstemsplus\"."},
        {0, "STICK", NULL,
         "RB Stems Plus will be deleted. The stems saved on \"STICK\" will stay. The last known folder was \"E:\\rbstemsplus\".",
         "The stems saved on \"STICK\" stay. The last known folder was \"E:\\rbstemsplus\"."},
        {0, "", NULL,
         "RB Stems Plus will be deleted. The saved stems on Stems Cache's drive will stay. The last known folder was \"E:\\rbstemsplus\".",
         "The stems saved on Stems Cache's drive stay. The last known folder was \"E:\\rbstemsplus\"."},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ff f;
        ffw_init(&f);
        f.loc_shown = "E:\\rbstemsplus";
        f.loc_dir = cases[i].gate_id ? "\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0}\\rbstemsplus" : f.loc_shown;
        f.loc_drive = f.loc_name = cases[i].drive;
        f.loc_measured = cases[i].measured;
        f.loc_gate_id = cases[i].gate_id;
        f.remove_location_rc = 1;
        f.remove_location_code = 7;
        f.app_fate = 0;
        f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
        answers(&f, 2, "Uninstall", "Done");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        const char *paragraph = strstr(f.dlg_msg[0], "\n\n");
        CHECKS(f.ndialogs == 2 && paragraph && !strcmp(paragraph + 2, cases[i].confirmation), f.dlg_msg[0]);
        CHECKS(strstr(f.dlg_msg[1], cases[i].completion) != NULL, f.dlg_msg[1]);
        CHECK(count_of(&f, "step REMOVE_LOCATION") == (cases[i].gate_id != NULL));
        CHECK(no_empty_quotes(&f));
        ff_end(&f);
    }
}

static void t_win_unchecked_uninstall_skips_probe(void) {
    ff f;
    ffw_init(&f);
    f.loc_dir = "\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0}\\rbstemsplus";
    f.loc_shown = "E:\\rbstemsplus";
    f.loc_drive = f.loc_name = "STICK";
    f.loc_measured = 0;
    f.loc2_from = 2;
    f.loc2_delay_ms = 300;
    f.loc2_dir = "F:\\changed";
    f.loc2_gate_id = "ABC";
    f.app_fate = 0;
    answers(&f, 2, "Uninstall", "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECKS(f.loc_now_calls == 1, "t_win_unchecked_uninstall_skips_probe: second drive call would block");
    CHECK(f.ndialogs == 2 && strstr(f.dlg_msg[0], f.loc_shown));
    CHECKS(strstr(f.dlg_msg[1], f.loc_shown) && !strstr(f.dlg_msg[1], f.loc_dir) && !strstr(f.dlg_msg[1], f.loc2_dir)
           && !count_of(&f, "step REMOVE_LOCATION"), f.dlg_msg[1]);
    ff_end(&f);
}

/* a chosen drive mounted in a folder with neither a label nor a letter: the connected-only texts'
 * nameless forms (review NT1), with the folder mount for File Explorer (review N3R16) */
static void win_nameless_connected(ff *f) {
    f->loc_dir = "\\\\?\\Volume{0b1c}\\RB Stems Plus Cache";
    f->loc_drive = f->loc_name = "";
    f->loc_shown = "C:\\mnt\\gls\\RB Stems Plus Cache";
    f->loc_size_kind = 1;
    f->loc_size = 1288490189;
    f->loc_gate_id = "ABC";
}

static void t_win_nameless_connected(void) {
    for (int fail = 0; fail < 2; fail++) {   /* Uninstall All: the confirmation, the deletion's lines, what's left */
        ff f;
        ffw_init(&f);
        f.cache = f.traces = f.chosen_cache = f.rb_folder = 1;
        f.traces_go = 1;
        win_nameless_connected(&f);
        f.remove_location_rc = fail;
        f.remove_location_code = 7;
        f.app_fate = 0;
        f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
        answers(&f, 2, "Uninstall", "Done");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        CHECKS(f.ndialogs == 2 && !strcmp(f.dlg_msg[0], "rekordbox goes back to exactly how it was. Your music and rekordbox library aren't touched.\n\n"
                                          "RB Stems Plus will be deleted, including all stems saved in its folder on Stems Cache's drive (about 1.2 GB), "
                                          "also any saved there by other computers or accounts."), f.dlg_msg[0]);
        CHECK(has(&f, "step REMOVE_LOCATION \\\\?\\Volume{0b1c}\\RB Stems Plus Cache ABC 1 " TEST_LOCATION_IDENTITY));
        CHECK(frame_starts(&f, "Deleting the stems saved on Stems Cache's drive…|") && frame_starts(&f, "Deleting the stems saved on Stems Cache's drive… 7 files|"));
        if (fail) CHECKS(strstr(f.dlg_msg[1], "Stems Cache cleanup on its drive couldn't be completed. The last known folder was "
                                              "\"C:\\mnt\\gls\\RB Stems Plus Cache\".") != NULL, f.dlg_msg[1]);
        else CHECKS(!strcmp(f.dlg_msg[1], "rekordbox is back to how it was."), f.dlg_msg[1]);
        CHECK(no_empty_quotes(&f));
        ff_end(&f);
    }
    {   /* the confirmation didn't name the folder found before the deletion: it stays */
        ff f;
        ffw_init(&f);
        f.cache = f.traces = f.chosen_cache = f.rb_folder = 1;
        f.traces_go = 1;
        win_nameless_connected(&f);
        f.loc2_from = 2;
        f.loc2_gate_id = "ABD";
        f.app_fate = 0;
        f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
        answers(&f, 2, "Uninstall", "Done");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        CHECK(count_of(&f, "step REMOVE_LOCATION") == 0);
        CHECKS(f.ndialogs == 2 && strstr(f.dlg_msg[1], "The stems saved on Stems Cache's drive stay. The last known folder was "
                                                       "\"C:\\mnt\\gls\\RB Stems Plus Cache\".") != NULL, f.dlg_msg[1]);
        CHECK(no_empty_quotes(&f));
        ff_end(&f);
    }
    {   /* Remove Anyway's removal note */
        ff f;
        ffw_init(&f);
        f.plus = f.chosen_model = 1;
        f.restore_rc = 1;
        f.restore_code = CORE_MODEL_UNKNOWN;
        f.originals_saved = f.originals_verified = 1;
        f.left_code = 3;
        f.originals_dir = "C:\\Users\\x\\AppData\\Local\\rbstemsplus\\originals";
        f.help_url = "https://help/#remove-anyway";
        win_nameless_connected(&f);
        answers(&f, 2, "Uninstall", "Cancel");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        CHECKS(f.ndialogs == 2 && strstr(f.dlg_msg[1], "Remove RB Stems Plus Anyway deletes only its own files, including all stems saved in its folder on "
                                                       "Stems Cache's drive (about 1.2 GB), also any saved there by other computers or accounts, and keeps") != NULL,
               f.dlg_msg[1]);
        CHECK(no_empty_quotes(&f));
        ff_end(&f);
    }
    {   /* Settings: the location row (NT2), then Clear on the drive, which fails */
        ff f;
        ffw_init(&f);
        f.set_gb = "20";
        f.set_days = "30";
        f.set_location = 1;
        f.set_old_drive = "";
        f.set_label_path = "DJ";
        f.set_clear = 3;
        win_nameless_connected(&f);
        f.remove_location_rc = 1;
        answers(&f, 3, "Clear Saved Stems", "Clear", "OK");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
        settle(&f);
        CHECK(has(&f, "label Stems Cache location: Stems Cache's drive › DJ"));
        CHECKS(f.ndialogs == 3 && !strcmp(f.dlg_msg[1], "Deletes all stems saved in this folder on Stems Cache's drive, including any saved there by other computers or accounts."),
               f.dlg_msg[1]);
        CHECK(frame(&f, "Clearing saved stems on Stems Cache's drive…|1|2") && frame(&f, "Clearing saved stems on Stems Cache's drive… 7 files|1|2"));
        CHECK(in_order(&f, 2, "step REMOVE_LOCATION \\\\?\\Volume{0b1c}\\RB Stems Plus Cache ABC 0 " TEST_LOCATION_IDENTITY,
                       "dialog Saved stems not cleared [OK] -> OK"));
        CHECKS(!strcmp(f.dlg_msg[2], "Some saved stems on Stems Cache's drive couldn't be deleted (Demucs: Resource busy)."), f.dlg_msg[2]);
        CHECK(no_empty_quotes(&f));
        ended_clean(&f, "win clear nameless");
        ff_end(&f);
    }
    /* the location row at the drive's root, and a named drive on Windows as before */
    for (int named = 0; named < 2; named++) {
        ff f;
        ffw_init(&f);
        f.set_location = 1;
        f.set_old_drive = named ? "GLS" : "";
        f.set_label_path = "";
        ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
        settle(&f);
        CHECK(has(&f, named ? "label Stems Cache location: GLS" : "label Stems Cache location: Stems Cache's drive"));
        ff_end(&f);
    }
}

/* review NT4: Use This PC from a chosen drive with no name keeps the previous-drive sentence */
static void t_win_use_this_pc_nameless(void) {
    for (int named = 0; named < 2; named++) {
        ff f;
        ffw_init(&f);
        f.set_gb = "20";
        f.set_days = "30";
        f.set_location = 1;
        f.set_label_path = "DJ";
        f.set_old_drive = named ? "GLS" : "";
        f.row_answer[0] = 1;
        answers(&f, 3, "", "Use This PC", "Cancel");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
        settle(&f);
        CHECKS(!strcmp(f.dlg_title[1], "Use This PC?") &&
               !strcmp(f.dlg_msg[1], named ? "Stems Cache will save stems on this PC. The stems saved on \"GLS\" stay there."
                                           : "Stems Cache will save stems on this PC. The stems saved on the previous drive stay there."), f.dlg_msg[1]);
        /* review N3R18: only the sheet opens with a location measure */
        CHECK(count_of(&f, "step SETTINGS open") == 1
              && in_order(&f, 4, "step SETTINGS open", "step SETTINGS config",
                          "dialog Use This PC? [Use This PC|Cancel] -> Use This PC", "step SETTINGS again"));
        CHECK(no_empty_quotes(&f));
        ff_end(&f);
    }
    /* already on this PC: no sentence */
    ff f;
    ffw_init(&f);
    f.set_location = 0;
    f.set_old_drive = "";
    f.row_answer[0] = 1;
    answers(&f, 3, "", "Use This PC", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECKS(f.ndialogs < 2 || !strcmp(f.dlg_msg[1], "Stems Cache will save stems on this PC."), f.dlg_msg[1]);
    ff_end(&f);
}

static void t_win_settings_open_read_timeout(void) {
    ff f;
    ffw_init(&f);
    f.set_timeout_at = "open";
    answers(&f, 1, "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && has(&f, "step SETTINGS open"));
    CHECKS(!strcmp(f.dlg_msg[0], "Windows didn't read the Stems Cache settings in time. Try again."), f.dlg_msg[0]);
    CHECK(!has(&f, "step LOCATION_WRITE"));
    ff_end(&f);
}

static void t_win_use_this_pc_read_timeout(void) {
    ff f;
    ffw_init(&f);
    f.set_timeout_at = "config";
    f.set_location = 1;
    f.row_answer[0] = 1;
    answers(&f, 3, "", "OK", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(f.ndialogs == 3 && in_order(&f, 3, "step SETTINGS config", "dialog Settings [OK] -> OK", "step SETTINGS again"));
    CHECKS(!strcmp(f.dlg_msg[1], "Windows didn't read the Stems Cache settings in time. Try again."), f.dlg_msg[1]);
    CHECK(!has(&f, "step LOCATION_WRITE"));
    ff_end(&f);
}

/* owner, 2026-11-15 (review NT1): Settings refuses a picked folder whose drive has neither a label
 * nor a letter, also when the probe refuses it as read-only (3) or wrong file system (7) (review
 * N21); a named drive goes on to the Use question, or keeps its named refusal */
static void t_win_refuse_nameless(void) {
    static const struct { const char *drive; int64_t why; } cases[] = {
        {"", 0}, {"", 3}, {"", 7}, {"STICK", 0}, {"STICK", 3},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int named = cases[i].drive[0] != 0;
        ff f;
        ffw_init(&f);
        f.set_gb = "20";
        f.set_days = "30";
        f.row_answer[0] = 0;
        f.pick = "C:\\Mount\\Stick";
        f.probe_real = "C:\\Mount\\Stick";
        f.probe_drive = cases[i].drive;
        f.probe_why = cases[i].why;
        f.probe_mount = "\\\\?\\Volume{0b1c}\\";
        f.discover = 0;
        answers(&f, 3, "", named && cases[i].why == 0 ? "Cancel" : "OK", "Cancel");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
        settle(&f);
        if (named && cases[i].why == 0) {
            CHECKS(!strcmp(f.dlg_title[1], "Use \"STICK\"?"), f.dlg_title[1]);
        } else if (named) {
            CHECKS(!strcmp(f.dlg_title[1], "Can't save stems there") &&
                   !strcmp(f.dlg_msg[1], "\"STICK\" is read-only."), f.dlg_msg[1]);
            CHECK(has(&f, "log settings: ~/picked refused (readOnly)") && !has(&f, "step LOCATION_DISCOVER"));
        } else {
            CHECKS(!strcmp(f.dlg_title[1], "Can't save stems there") &&
                   !strcmp(f.dlg_msg[1], "That folder's drive has no name and no drive letter. Choose another folder."), f.dlg_msg[1]);
            CHECK(has(&f, "log settings: ~/picked refused (nameless)") && !has(&f, "step LOCATION_DISCOVER"));
        }
        CHECK(no_empty_quotes(&f));
        ff_end(&f);
    }
}

static void t_win_picker_check_timeout_refuses(void) {
    const core_step checks[] = {CORE_STEP_LOCATION_DISCOVER, CORE_STEP_LOCATION_WRITABLE};
    for (size_t i = 0; i < sizeof checks / sizeof *checks; i++) {
        ff f;
        ffw_init(&f);
        f.set_gb = "20";
        f.set_days = "30";
        f.row_answer[0] = 0;
        f.pick = f.probe_real = "E:\\DJ";
        f.probe_drive = "STICK";
        f.probe_mount = "E:\\";
        f.discover = checks[i] == CORE_STEP_LOCATION_WRITABLE ? 2 : 0;
        f.disc_dir = "E:\\DJ\\RB Stems Plus Cache.noindex";
        f.disc_id = "ABC";
        f.blocked_picker_check = checks[i];
        answers(&f, 3, "", "Use \"STICK\"", "Cancel");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
        settle(&f);
        CHECKS(!strcmp(f.dlg_title[1], "Can't save stems there") &&
               !strcmp(f.dlg_msg[1], "RB Stems Plus can't read that folder's drive. Choose another folder.") &&
               !has(&f, "step LOCATION_CREATE") && !has(&f, "step LOCATION_WRITE"),
               "t_win_picker_check_timeout_refuses: discovery and write-access checks must finish before the Use prompt");
        ff_end(&f);
    }
}

/* HF1: an administrator's Uninstall All with Stems Cache's traces removes Stems Cache first; its
 * removal failing stops everything: nothing of ours deleted, rekordbox's model not touched */
static void t_win_uninstall_all_cache_fails(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = f.rb_folder = 1;
    f.admin_user = 1;
    f.nadmin_rc = 1;
    f.admin_rc[0] = 1;   /* the worker failed */
    f.app_fate = 0;
    f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
    answers(&f, 2, "Uninstall", "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(count_of(&f, "\nadmin 2 ") == 1);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_title[1], "Uninstall stopped"));
    CHECK(has(&f, "log Stems Cache could not be removed: stopping, nothing else deleted"));
    CHECK(!has(&f, "step MODEL_RESTORE") && !has(&f, "step LAUNCH_AGENT_UNLOAD") && !has(&f, "step REMOVE_OWN_FOLDERS") && !has(&f, "step SELF_DELETE"));
    CHECK(f.quit_steps == 0);
    if (f.ndialogs != 2) DUMP(&f);
    ended_clean(&f, "win uninstall all, cache removal failed");
    ff_end(&f);
}

static void t_win_failed_old_cache_cleanup_preserves_choice(void) {
    for (int all = 0; all < 2; all++) {
        ff f;
        ffw_init(&f);
        f.traces = f.chosen_cache = f.rb_folder = f.config_sets = 1;
        f.cache = 0;   /* the current folder is stock; the failing bridge is in an older folder */
        f.nadmin_rc = 1;
        f.admin_rc[0] = ROOT_NO_ORIGINALS;
        answers(&f, 2, "Uninstall", "OK");
        ev(&f, all ? "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}" : "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
        settle(&f);
        CHECK(f.admin_attempts == 1 && f.chosen_cache && !has(&f, "step SET_CHOSEN") && !has(&f, "step CONFIG_WRITE rekordbox_model 0"));
        CHECK(!all || !has(&f, "step MODEL_RESTORE"));
        ff_end(&f);
    }
}

static void t_win_install_keeps_compatibility_target(void) {
    for (int reinstall = 0; reinstall < 2; reinstall++) {
        ff f;
        ffw_init(&f);
        f.cache = f.chosen_cache = reinstall;
        f.win_rb_after_download = "7.2.20.0342";
        answers(&f, 1, reinstall ? "Reinstall" : "Install");
        ev(&f, reinstall ? "{\"ev\":\"click\",\"id\":\"reinstall_cache\"}" : "{\"ev\":\"click\",\"id\":\"install_cache\"}");
        settle(&f);
        CHECKS(f.admin_attempts == 1 && f.win_rb_version && !strcmp(f.win_rb_version, "7.2.20.0342") &&
               strstr(f.trace, reinstall ? "admin 1 7.2.10:1111111111111111 " : "admin 0 7.2.10:1111111111111111 "), f.trace);
        ff_end(&f);
    }
}

static const char batch_scope[] = "7.2.19:1111111111111111,7.2.20:2222222222222222";

static void t_win_install_two_versions(void) {
    ff f;
    ffw_init(&f);
    f.win_scope = batch_scope;
    f.win_scope_versions = "7.2.19\n7.2.20\n";
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && strstr(f.dlg_msg[0], "rekordbox 7.2.19 and 7.2.20 separates"));
    CHECK(f.admin_attempts == 1 && f.chosen_cache && strstr(f.trace, batch_scope));
    ff_end(&f);
}

static void t_win_watcher_install_two_versions(void) {
    ff f;
    ffw_init(&f);
    f.chosen_cache = 1;
    f.request_age = 0;
    f.win_scope = batch_scope;
    f.win_scope_versions = "7.2.19\n7.2.20\n";
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(has(&f, "step REQUEST take") && f.ndialogs == 0);
    CHECK(f.admin_attempts == 1 && strstr(f.trace, batch_scope));
    ff_end(&f);
}

static void t_win_install_noop_full_coverage(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.traces = f.cache_record_ok = 1;
    f.win_scope = "";
    f.win_scope_versions = "";
    /* The newest candidate is unsupported. Its DLL is irrelevant to the covered rows. */
    f.bridge_installed = "";
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && !f.admin_attempts && f.chosen_cache);
    CHECK(!has(&f, "step DOWNLOAD bridge"));
    ff_end(&f);
}

static void t_win_install_empty_scope(void) {
    for (int retry = 0; retry < 2; retry++) {
        ff f;
        ffw_init(&f);
        f.cache = f.traces = 1;
        f.win_rows = "7.2.10\t3\t11\t0\t2\t0\t7.2.10.0342\n";
        if (retry) {
            f.win_retry_scope = f.win_retry_versions = "";
            f.nadmin_rc = 1;
            f.admin_rc[0] = ROOT_WIN_SCOPE_CHANGED;
        } else f.win_scope = f.win_scope_versions = "";
        answers(&f, 3, "Install", "Install", "OK");
        ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
        settle(&f);
        CHECKS(f.ndialogs == 1 + retry && !strcmp(f.dlg_title[retry], "Install failed") &&
               !strcmp(f.dlg_msg[retry], "No installed version of rekordbox is ready for this change."), f.trace);
        CHECK(f.admin_attempts == retry && !f.chosen_cache && has(&f, "step DOWNLOAD bridge") == retry);
        ended_clean(&f, "win install, empty scope");
        ff_end(&f);
    }
}

static void t_win_reinstall_empty_cache_scope(void) {
    ff f;
    ffw_init(&f);
    f.plus = f.chosen_model = f.chosen_cache = 1;
    f.win_scope = f.win_scope_versions = "";
    answers(&f, 2, "Cancel", "Cancel");
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"reinstall\"}") == CORE_OK);
    settle(&f);
    CHECKS(f.ndialogs == 1 && !strcmp(f.dlg_title[0], "Reinstall Demucs v4?") &&
           !strcmp(f.dlg_msg[0], "Installs the latest version of Demucs v4 again."), f.trace);
    ended_clean(&f, "win combined reinstall, empty cache scope");
    ff_end(&f);
}

static void t_win_install_scope_changed_once(void) {
    for (int second_change = 0; second_change < 2; second_change++) {
        ff f;
        ffw_init(&f);
        f.win_scope = batch_scope;
        f.win_scope_versions = "7.2.19\n7.2.20\n";
        f.win_retry_scope = "7.2.19:1111111111111111,7.2.21:3333333333333333";
        f.win_retry_versions = "7.2.19\n7.2.21\n";
        f.nadmin_rc = 2;
        f.admin_rc[0] = ROOT_WIN_SCOPE_CHANGED;
        f.admin_rc[1] = second_change ? ROOT_WIN_SCOPE_CHANGED : 0;
        answers(&f, 3, "Install", "Install", "OK");
        ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
        settle(&f);
        CHECK(f.admin_attempts == 2 && f.ndialogs == 2 + second_change);
        CHECK(strstr(f.dlg_msg[0], "7.2.19 and 7.2.20") && strstr(f.dlg_msg[1], "7.2.19 and 7.2.21"));
        CHECK(strstr(f.trace, batch_scope) && strstr(f.trace, f.win_retry_scope) && f.chosen_cache == !second_change);
        if (second_change) {
            CHECK(strstr(f.dlg_msg[2], "The installed versions of rekordbox changed"));
            CHECK(count_of(&f, "step SET_CHOSEN ") == 0);
        }
        ff_end(&f);
    }
}

/* Exit 17 keeps the account's choice. A first Install with a stock DLL points to the action
 * still available. An Install for an opted-in account and a Reinstall keep the existing advice. */
static void t_win_install_undo_incomplete(void) {
    for (int scenario = 0; scenario < 3; scenario++) {
        ff f;
        ffw_init(&f);
        int chosen = scenario != 0, reinstall = scenario == 2;
        f.chosen_cache = chosen;
        if (reinstall) f.plus = f.chosen_model = f.cache = f.traces = 1;
        if (!chosen) {
            f.win_rows = "7.2.20\t1\t0\t0\t0\t0\t7.2.20.0354\n7.2.19\t1\t0\t0\t0\t0\t7.2.19.0342\n";
            f.win_scope = batch_scope;
            f.win_scope_versions = "7.2.19\n7.2.20\n";
        }
        f.nadmin_rc = 1;
        f.admin_rc[0] = ROOT_WIN_UNDO_INCOMPLETE;
        answers(&f, 2, reinstall ? "Reinstall" : "Install", "OK");
        ev(&f, reinstall ? "{\"ev\":\"click\",\"id\":\"reinstall\"}" : "{\"ev\":\"click\",\"id\":\"install_cache\"}");
        settle(&f);
        const char *message = chosen ? "Stems Cache couldn't finish undoing its changes. Click Reinstall Stems Cache, or Put Back Pioneer's File."
                                     : "Stems Cache couldn't finish undoing its changes. Click Install Stems Cache to try again.";
        CHECKS(f.admin_attempts == 1 && f.ndialogs == 2 && !strcmp(f.dlg_msg[1], message), f.dlg_msg[1]);
        CHECK(f.chosen_cache == chosen && (!reinstall || f.chosen_model));
        if (!chosen) CHECK(model_get_i64(&f.c->m, F_INSTALL_CACHE_ENABLED) && !model_get_i64(&f.c->m, F_REINSTALL_CACHE_ENABLED)
                           && !model_get_i64(&f.c->m, F_WIN_RESTORE_VISIBLE));
        ff_end(&f);
    }
}

static void t_win_install_partial(void) {
    for (int installed = 0; installed < 2; installed++) {
        ff f;
        ffw_init(&f);
        f.win_scope = "7.2.19:1111111111111111,7.2.20:2222222222222222,7.2.21:3333333333333333";
        f.win_scope_versions = "7.2.19\n7.2.20\n7.2.21\n";
        f.nadmin_rc = 1;
        f.admin_rc[0] = ROOT_WIN_PARTIAL;
        f.win_admin_targets = installed ? "7.2.19\t0\tinstalled\n7.2.20\t5\trefused\n7.2.21\t-\tnot-started\n"
                                       : "7.2.19\t0\talready\n7.2.20\t5\trefused\n7.2.21\t-\tnot-started\n";
        answers(&f, 2, "Install", "OK");
        ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
        settle(&f);
        CHECK(f.admin_attempts == 1 && f.chosen_cache == installed && f.ndialogs == 2);
        CHECK(count_of(&f, "step SET_CHOSEN ") == installed);
        CHECK(!strcmp(f.dlg_msg[1], "Stems Cache was installed in rekordbox 7.2.19, but not in rekordbox 7.2.20 and 7.2.21. Close rekordbox (also in other accounts on this PC), then try again."));
        ff_end(&f);
    }
}

static void t_win_uninstall_partial_keeps_choice(void) {
    for (int all = 0; all < 2; all++) {
        ff f;
        ffw_init(&f);
        f.cache = f.traces = f.chosen_cache = f.rb_folder = f.config_sets = 1;
        f.win_scope_affected = "7.2.19\n7.2.20\n";
        f.nadmin_rc = 1;
        f.admin_rc[0] = all ? 0 : ROOT_WIN_PARTIAL;
        f.win_root_kept = all;
        f.traces_go = 1;
        f.win_admin_targets = all ? "7.2.19\t0\tput-back\n7.2.20\t0\tput-back\n" :
            "7.2.19\t0\tput-back\n7.2.20\t4\tkept\n";
        answers(&f, 2, "Uninstall", "OK");
        ev(&f, all ? "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}" : "{\"ev\":\"click\",\"id\":\"uninstall_cache\"}");
        settle(&f);
        CHECK(f.admin_attempts == 1 && strstr(f.trace, "admin 2 * ") && f.chosen_cache && count_of(&f, "step SET_CHOSEN ") == 0);
        CHECK(f.ndialogs == 2 && strstr(f.dlg_msg[0], "rekordbox 7.2.19 and 7.2.20"));
        CHECK(strstr(f.dlg_msg[1], all ? "RB Stems Plus couldn't remove its copy of rekordbox's files." :
                     "Stems Cache was removed from rekordbox 7.2.19"));
        CHECK(!all || (strstr(f.dlg_msg[1], "nothing else was deleted") && !has(&f, "step MODEL_RESTORE")));
        ff_end(&f);
    }
}

static void t_win_uninstall_all_partial_nothing_removed(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = f.rb_folder = f.config_sets = 1;
    f.win_scope_affected = "7.2.19\n7.2.20\n";
    f.nadmin_rc = 1;
    f.admin_rc[0] = ROOT_WIN_PARTIAL;
    f.win_admin_targets = "7.2.19\t5\tkept\n7.2.20\t-\tnot-started\n";
    answers(&f, 2, "Uninstall", "OK");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_msg[1],
          "Stems Cache couldn't be removed, so nothing else was deleted. Close rekordbox (also in other accounts on this PC), then try again."));
    CHECK(f.admin_attempts == 1 && f.chosen_cache && !has(&f, "step SET_CHOSEN") && !has(&f, "step REMOVE_OWN_FOLDERS"));
    ff_end(&f);
}

static void t_win_uninstall_all_names_cache_versions(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.win_scope_affected = "7.2.18\n7.2.19\n7.2.20\n";
    answers(&f, 1, "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && strstr(f.dlg_msg[0],
          "\n\nStems Cache is removed from rekordbox 7.2.18, 7.2.19 and 7.2.20."));
    CHECK(f.admin_attempts == 0 && f.chosen_cache);
    ff_end(&f);
}

static void t_win_restore_versions(void) {
    ff f;
    ffw_init(&f);
    f.chosen_cache = 1;
    f.win_scope = batch_scope;
    f.win_scope_versions = f.win_scope_affected = "7.2.19\n7.2.20\n";
    answers(&f, 1, "Put Back Pioneer's File");
    ev(&f, "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && strstr(f.dlg_msg[0], "back in rekordbox 7.2.19 and 7.2.20."));
    CHECK(f.admin_attempts == 1 && strstr(f.trace, batch_scope) && f.chosen_cache && count_of(&f, "step SET_CHOSEN ") == 0);
    ff_end(&f);
}

static void t_win_rb_open_names_version(void) {
    ff f;
    ffw_init(&f);
    f.rb_open_n = 4;
    f.rb_open_versions = "7.2.19\n";
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && strstr(f.dlg_msg[0], "Close rekordbox 7.2.19 (also in other accounts on this PC)"));
    CHECK(!f.admin_attempts);
    ff_end(&f);
}

static void t_win_request_adds_uncovered_version(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.chosen_cache = f.traces = 1;
    f.request_age = 12;
    f.win_rows = "7.2.20\t1\t0\t0\t0\t0\t7.2.20.0354\n7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n";
    f.win_scope = "7.2.20:2222222222222222";
    f.win_scope_versions = "7.2.20\n";
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(f.admin_attempts == 1 && has(&f, "step WIN_SCOPE install") &&
          has(&f, "admin 0 7.2.20:2222222222222222 - 2222222222222222222222222222222222222222222222222222222222222222 ortsha 0"));
    ff_end(&f);

    ffw_init(&f);
    f.chosen_model = f.cache = f.chosen_cache = f.traces = 1;
    f.request_age = 12;
    f.request_features = "model\n";
    f.win_rows = "7.2.20\t1\t0\t0\t0\t0\t7.2.20.0354\n7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n";
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECKS(has(&f, "step MODEL_COPY") && f.admin_attempts == 0 && !has(&f, "step WIN_SCOPE install"), f.trace);
    ended_clean(&f, "model offer with an eligible cache row");
    ff_end(&f);
}

static void t_win_settings_multiple_versions(void) {
    ff f;
    ffw_init(&f);
    f.win_drive_chooser_disabled = 1;
    f.win_rows = "7.2.20\t0\t0\t0\t0\t0\t7.2.20.0354\n7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n";
    answers(&f, 1, "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && !strcmp(f.dlg_msg[0], "Stems Cache uses these settings the next time each version of rekordbox opens."));
    ff_end(&f);
}

static void t_win_settings_omits_reinstall_when_build_disables_drive_choice(void) {
    ff f;
    ffw_init(&f);
    f.win_drive_chooser_disabled = 1;
    f.set_clear = 1;
    answers(&f, 1, "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECK(f.ndialogs == 1 && f.dlg_enabled[0] == 7 && !f.dlg_first_change_enabled &&
          !strcmp(f.dlg_msg[0], "Stems Cache uses these settings the next time rekordbox opens."));
    ff_end(&f);
}

/* Settings > Apps' Uninstall: the complete uninstall, after the start-up flow */
static void t_win_uninstall_from_settings(void) {
    ff f;
    ffw_init(&f);
    CHECK(ev(&f, "{\"ev\":\"shown\"}") == CORE_OK);
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_from_settings\"}") == CORE_OK);   /* the start-up flow runs: waits */
    CHECK(f.c->fl.win_uninstall_asked == 1);
    answers(&f, 1, "Cancel");
    settle(&f);
    CHECK(f.c->fl.win_uninstall_asked == 0 && f.ndialogs == 1 && !strcmp(f.dlg_title[0], "Uninstall RB Stems Plus?"));
    /* review N3R18: startup's read doesn't compete with the location memo */
    CHECK(in_order(&f, 3, "step THIS_MAC_DELETE facts", "step SETTINGS config", "step LOCATION_NOW")
          && !has(&f, "step SETTINGS open"));
    CHECK(in_order(&f, 3, "log core: the uninstall from Settings > Apps waits for the flow running now", "step LOCATION_NOW",
                   "dialog Uninstall RB Stems Plus? [Uninstall|Cancel] -> Cancel"));
    ended_clean(&f, "uninstall from settings");
    /* asked again with nothing running: at once */
    answers(&f, 1, "Cancel");
    ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_from_settings\"}");
    settle(&f);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_title[1], "Uninstall RB Stems Plus?"));
    ff_end(&f);
    /* the shell's --uninstall (LaunchArgs.cs) on a never-opened install, the click gate on as
     * shipped: the click right after the first "shown", Welcome first, then the uninstall's
     * question (review UN2: the menu's uninstall_all was dropped here) */
    ffw_init(&f);
    f.c->click_gate_off = 0;
    f.welcomed = 0;
    answers(&f, 2, "Continue", "Cancel");
    CHECK(ev(&f, "{\"ev\":\"shown\"}") == CORE_OK);
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"uninstall_from_settings\"}") == CORE_OK);
    settle(&f);
    CHECK(f.ndialogs == 2 && !strcmp(f.dlg_title[0], "Welcome to RB Stems Plus") && !strcmp(f.dlg_title[1], "Uninstall RB Stems Plus?"));
    CHECK(in_order(&f, 3, "log core: the uninstall from Settings > Apps waits for the flow running now", "step WELCOME done",
                   "dialog Uninstall RB Stems Plus? [Uninstall|Cancel] -> Cancel"));
    CHECK(f.c->fl.win_uninstall_asked == 0);
    ended_clean(&f, "uninstall from settings, welcome first");
    ff_end(&f);
}

/* t_matrix's runs (every step failing, every dialog cancelled, core_free at every host call) for
 * the Windows variants, with a core started "os":"win" */
static void scw_install(ff *f) {
    f->nadmin_rc = 2;
    f->admin_rc[0] = ROOT_WIN_RUN_AGAIN;
    f->admin_rc[1] = 0;
    answers(f, 1, "Install");
}
static void scw_update_first(ff *f) {
    f->nadmin_rc = 1;
    f->admin_rc[0] = ROOT_WIN_PROTOCOL;
    f->app_version = "1.5.0";
    f->update_swap_rc = 3;   /* the update fails (it would quit the app): its alert, then idle */
    answers(f, 4, "Install", "Update", "Update", "OK");
}
static void scw_restore(ff *f) {
    f->cache = f->traces = f->chosen_cache = 1;
    f->admin_restart = 1;
    answers(f, 1, "Put Back Pioneer's File");
}
static void scw_uninstall_all(ff *f) {
    f->cache = f->traces = f->chosen_cache = f->rb_folder = 1;
    f->traces_go = 1;
    f->admin_restart = 1;
    f->app_fate = 0;
    f->app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
    answers(f, 2, "Uninstall", "Done");
}

static const scenario win_scenarios[] = {
    {"win install, run again", "{\"ev\":\"click\",\"id\":\"install_cache\"}", scw_install},
    {"win update first", "{\"ev\":\"click\",\"id\":\"install_cache\"}", scw_update_first},
    {"win restore", "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}", scw_restore},
    {"win uninstall all", "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}", scw_uninstall_all},
};

static void t_win_matrix(void) {
    ff_config = CONFIG_WIN;
    for (size_t i = 0; i < sizeof win_scenarios / sizeof *win_scenarios; i++) {
        int ns = 0, nd = 0, nc = 0;
        run_one(&win_scenarios[i], 0, 0, &ns, &nd, &nc);
        CHECKS(ns > 0 && nc > 0, win_scenarios[i].name);
        for (int k = 1; k <= ns; k++) run_one(&win_scenarios[i], 1, k, NULL, NULL, NULL);
        for (int k = 0; k < nd; k++) run_one(&win_scenarios[i], 2, k, NULL, NULL, NULL);
        for (int k = 1; k <= nc; k++) run_one(&win_scenarios[i], 3, k, NULL, NULL, NULL);
    }
    ff_config = NULL;
}

/* the native admin (API-V1-NOTES §11, review NW8): the install's admin call reaches the DLL's
 * backend on the flow thread; while it blocks (UAC up), main's front_facts carries
 * CORE_FRONT_ELEVATING and the core isn't in front; the user stops it (< 0): no alert, the bit
 * gone, the flow ended cleanly, the shell's admin never asked */
static pthread_mutex_t nat_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t nat_cv = PTHREAD_COND_INITIALIZER;
static int nat_in, nat_release, nat_kind = -1;
static void *nat_open(core *c) { return c; }
static int32_t nat_admin(void *st, int32_t kind, const char *version, const char *bundle_id, const char *bridge_sha, const char *ort_sha, int32_t force) {
    (void)st; (void)version; (void)bundle_id; (void)bridge_sha; (void)ort_sha; (void)force;
    pthread_mutex_lock(&nat_mu);
    nat_in = 1;
    nat_kind = kind;
    pthread_cond_broadcast(&nat_cv);
    while (!nat_release) pthread_cond_wait(&nat_cv, &nat_mu);
    pthread_mutex_unlock(&nat_mu);
    return -1;
}
static const native_backend nat_backend = {.open = nat_open, .admin = nat_admin};
static int nat_entered(void) {
    pthread_mutex_lock(&nat_mu);
    int in = nat_in;
    pthread_mutex_unlock(&nat_mu);
    return in;
}

static void t_win_native_admin(void) {
    ff f;
    ff_config = "{\"version\":\"1.4.0\",\"os\":\"win\",\"native\":[\"admin\"],\"paths\":{\"home\":\"C:\\\\Users\\\\x\"}}";
    ff_no_admin = 1;
    ff_init(&f);
    ff_config = NULL;
    ff_no_admin = 0;
    CHECK(f.c && core_test_native(f.c, &nat_backend) == CORE_OK);
    nat_in = nat_release = 0;
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    for (int i = 0; i < 5000 && !nat_entered(); i++) {   /* at most ~5 s */
        if (!pump(&f)) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
    }
    CHECK(nat_entered() && nat_kind == CORE_ADMIN_CACHE_INSTALL);
    uint32_t during = f.c->life->host.front_facts(f.c->life->host.ctx);
    CHECK((during & CORE_FRONT_ELEVATING) && !win_in_front(during, 0, CORE_TAB_MANAGE, 0));
    pthread_mutex_lock(&nat_mu);
    nat_release = 1;
    pthread_cond_broadcast(&nat_cv);
    pthread_mutex_unlock(&nat_mu);
    settle(&f);
    uint32_t after = f.c->life->host.front_facts(f.c->life->host.ctx);
    CHECK(!(after & CORE_FRONT_ELEVATING) && win_in_front(after, 0, CORE_TAB_MANAGE, 0));
    CHECK(f.admin_attempts == 0 && f.ndialogs == 1 && !f.chosen_cache);
    ended_clean(&f, "native admin, stopped");
    ff_end(&f);
}

/* BLOCKED on Windows: Pioneer's ORT by sha256 (FACT_ORT_LISTED), whatever its version prefix */
static void t_win_ort_listed(void) {
    ff f;
    ffw_init(&f);
    f.ort_unlisted = 1;
    answers(&f, 2, "Install", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.admin_attempts == 0
          && has(&f, "log Stems Cache not installed: Stems Cache doesn't support ONNX Runtime 1.17.3 yet. RB Stems Plus checks again each time you open it."));
    ended_clean(&f, "ort unlisted");
    ff_end(&f);
    /* listed by its sha256: installed */
    ffw_init(&f);
    answers(&f, 1, "Install");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECK(f.admin_attempts == 1 && f.chosen_cache);
    ended_clean(&f, "ort listed");
    ff_end(&f);
}

/* R1 in the app (review WR1): CORE_SLOW_R1 asked with no key at start, at activation, on the
 * watcher's notification and when an action ends; its answer drives Stems Cache's light, R1's line
 * and Put Back Pioneer's File; busy turns the button off; an older answer is dropped; no answer is
 * nothing to put back */
static int64_t snap_i64(const ff *f, core_field fl) { return f->held ? core_i64(f->held, fl) : -1; }
static int snap_flag(const ff *f, core_field fl) { return f->held ? core_flag(f->held, fl) : -1; }
static const char *snap_str(const ff *f, core_field fl) {
    size_t n;
    const char *s = f->held ? core_str(f->held, fl, &n) : NULL;
    return s ? s : "";
}
static int r1_calls(ff *f) {
    pthread_mutex_lock(&f->mu);
    int n = f->r1_calls;
    pthread_mutex_unlock(&f->mu);
    return n;
}

static void t_win_watcher_refreshes_manage_quietly(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    CHECK(ev(&f, "{\"ev\":\"start\"}") == CORE_OK);
    settle(&f);
    int64_t before = snap_i64(&f, F_LIGHT_CACHE_LOOK);
    f.cache = 0;
    f.win_rb_version = "7.2.20.0342";
    f.request_age = 0;
    CHECK(ev(&f, "{\"ev\":\"rekordbox_changed\"}") == CORE_OK);
    settle(&f);
    CHECK(snap_i64(&f, F_LIGHT_CACHE_LOOK) != before && r1_calls(&f) == 2);
    CHECK(!has(&f, "step REQUEST") && f.admin_attempts == 0 && f.ndialogs == 0);
    ff_end(&f);
}

static void t_win_r1(void) {
    static const char *const MISSING = "rekordbox's onnxruntime.dll is missing, so rekordbox can't separate stems. Click Put Back Pioneer's File.";
    ff f;
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_state = R1_LIVE_MISSING;
    f.r1_after_restore = R1_OK;
    CHECK(ev(&f, "{\"ev\":\"start\"}") == CORE_OK);
    settle(&f);
    CHECK(r1_calls(&f) == 1 && f.r1_keyed == 0);   /* asked once at start, the folder the host's own */
    CHECK(snap_i64(&f, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED && !strcmp(snap_str(&f, F_LIGHT_CACHE_TIP), MISSING));
    CHECK(!strcmp(snap_str(&f, F_WIN_R1_LINE), MISSING) && snap_flag(&f, F_WIN_RESTORE_VISIBLE) == 1 && snap_flag(&f, F_WIN_RESTORE_ENABLED) == 1);
    /* the same answer again (activation, the watcher's notification): asked, nothing else moves */
    CHECK(ev(&f, "{\"ev\":\"check_request\"}") == CORE_OK);
    settle(&f);
    CHECK(r1_calls(&f) == 2 && !strcmp(snap_str(&f, F_WIN_R1_LINE), MISSING));
    /* Put Back Pioneer's File: off while the action runs, and R1 clear after it */
    answers(&f, 1, "Put Back Pioneer's File");
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}") == CORE_OK);
    settle(&f);
    CHECK(count_of(&f, "\nadmin 5 ") == 1 && f.r1_busy_frames > 0 && f.r1_busy_bad == 0);
    CHECK(f.r1_reenabled == 0);   /* CW3: Put Back stays off from the restore until R1's answer clears it */
    CHECK(r1_calls(&f) >= 3);   /* the action's end asked */
    CHECK(!strcmp(snap_str(&f, F_WIN_R1_LINE), "") && snap_flag(&f, F_WIN_RESTORE_VISIBLE) == 0 && snap_flag(&f, F_WIN_RESTORE_ENABLED) == 0);
    CHECK(snap_i64(&f, F_LIGHT_CACHE_LOOK) != LOOK_LIMITED || strcmp(snap_str(&f, F_LIGHT_CACHE_TIP), MISSING));
    ended_clean(&f, "r1 restore");
    ff_end(&f);

    /* a stale answer: the start's ask (missing) comes back after activation's (fine): dropped */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_state = R1_LIVE_MISSING;
    f.r1_hold = 1;
    CHECK(ev(&f, "{\"ev\":\"start\"}") == CORE_OK);
    for (int i = 0; i < 5000 && r1_calls(&f) < 1; i++) { pump(&f); struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    pthread_mutex_lock(&f.mu);
    f.r1_state = R1_OK;
    pthread_mutex_unlock(&f.mu);
    CHECK(ev(&f, "{\"ev\":\"activated\"}") == CORE_OK);
    for (int i = 0; i < 5000 && r1_calls(&f) < 2; i++) { pump(&f); struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    pthread_mutex_lock(&f.mu);
    f.r1_released = 1;
    pthread_cond_broadcast(&f.cv);
    pthread_mutex_unlock(&f.mu);
    settle(&f);
    CHECK(r1_calls(&f) == 2 && f.c->v.r1_gen == 2);
    CHECK(!strcmp(snap_str(&f, F_WIN_R1_LINE), "") && snap_flag(&f, F_WIN_RESTORE_VISIBLE) == 0 && f.c->v.win_r1 == R1_OK);
    ff_end(&f);

    /* no answer (no rekordbox version folder, unreadable): nothing to put back */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_none = 1;
    f.r1_state = R1_LIVE_MISSING;
    CHECK(ev(&f, "{\"ev\":\"start\"}") == CORE_OK);
    settle(&f);
    CHECK(r1_calls(&f) == 1 && !strcmp(snap_str(&f, F_WIN_R1_LINE), "") && snap_flag(&f, F_WIN_RESTORE_VISIBLE) == 0);
    CHECK(snap_i64(&f, F_LIGHT_CACHE_LOOK) != LOOK_LIMITED || strcmp(snap_str(&f, F_LIGHT_CACHE_TIP), MISSING));
    ff_end(&f);

    /* Create Report's Stems Cache light is R1's, as the window's (WCM1) */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_state = R1_ORT_MISSING;
    answers(&f, 1, "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"create_report\"}");
    settle(&f);
    CHECK(strstr(f.trace, "step REPORT_ZIP 2 Demucs v4 is off. 1 Stems Cache's copy") != NULL && !strstr(f.trace, "Stems Cache is on."));
    ff_end(&f);

    /* a Mac core asks nothing */
    ff_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    ev(&f, "{\"ev\":\"start\"}");
    ev(&f, "{\"ev\":\"activated\"}");
    settle(&f);
    CHECK(r1_calls(&f) == 0);
    ff_end(&f);
}

static uint32_t r1_prepare_front(void *ctx) {
    (void)ctx;
    return CORE_FRONT_ACTIVE | CORE_FRONT_WINDOW_LOADED | (CORE_TAB_PREPARE << CORE_FRONT_TAB_SHIFT);
}

/* R1's "Don't Ask Again for This Version" (N10, review WCM3): beside Put Back while this version
 * isn't dismissed; a click asks nothing, the host's step adds the version's line to dismissed, R1
 * is asked again and the button goes, the line and Put Back staying; off under an action; a write
 * that fails or has nothing to dismiss is logged and the button stays; a Mac core has no such click */
static void t_win_r1_dismiss(void) {
    static const char *const MISSING = "rekordbox's onnxruntime.dll is missing, so rekordbox can't separate stems. Click Put Back Pioneer's File.";
    ff f;
    ffw_init(&f);
    f.c->prepare_tabs = 1;
    f.c->life->host.front_facts = r1_prepare_front;
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_state = R1_LIVE_MISSING;
    CHECK(ev(&f, "{\"ev\":\"start\"}") == CORE_OK);
    settle(&f);
    CHECK(snap_flag(&f, F_WIN_R1_DISMISS_VISIBLE) == 1 && snap_flag(&f, F_WIN_R1_DISMISS_ENABLED) == 1 && snap_flag(&f, F_WIN_RESTORE_VISIBLE) == 1);
    uint64_t prep_gen = f.c->gen[DOM_PREPARE];
    CHECK(!strcmp(snap_str(&f, F_WIN_R1_DISMISS_VERSIONS), "7.2.10"));   /* the current version folder's: no rows */
    CHECK(ev(&f, "{\"ev\":\"click\",\"id\":\"r1_dismiss\"}") == CORE_E_ARG);   /* it names the versions shown */
    CHECK(ev(&f, "{\"ev\":\"r1_dismiss\",\"versions\":\"7.2.10\"}") == CORE_OK);
    settle(&f);
    CHECK(f.ndialogs == 0 && count_of(&f, "step WIN_R1_DISMISS 7.2.10") == 1 && count_of(&f, "\nadmin ") == 0);
    CHECK(in_order(&f, 3, "log user chose: Don't Ask Again for This Version (R1)", "step WIN_R1_DISMISS 7.2.10",
                   "log   remembered: the watcher won't open RB Stems Plus for R1 in rekordbox 7.2.10"));
    CHECK(r1_calls(&f) == 2);   /* asked again: its answer says dismissed */
    CHECK(prep_gen > 0 && f.c->gen[DOM_PREPARE] == prep_gen);   /* the first R1 answer rechecked Prepare; only the dismissal changed now */
    CHECK(snap_flag(&f, F_WIN_R1_DISMISS_VISIBLE) == 0 && snap_flag(&f, F_WIN_R1_DISMISS_ENABLED) == 0);
    CHECK(!strcmp(snap_str(&f, F_WIN_R1_LINE), MISSING) && snap_flag(&f, F_WIN_RESTORE_VISIBLE) == 1 && snap_flag(&f, F_WIN_RESTORE_ENABLED) == 1);
    CHECK(snap_i64(&f, F_LIGHT_CACHE_LOOK) == LOOK_LIMITED && f.c->v.win_r1_dismissed == 1);
    ended_clean(&f, "r1 dismiss");
    /* activation: still dismissed, so no button; once R1 is gone, nothing is shown */
    CHECK(ev(&f, "{\"ev\":\"activated\"}") == CORE_OK);
    settle(&f);
    CHECK(snap_flag(&f, F_WIN_R1_DISMISS_VISIBLE) == 0 && snap_flag(&f, F_WIN_RESTORE_VISIBLE) == 1);
    pthread_mutex_lock(&f.mu);
    f.r1_state = R1_OK;
    pthread_mutex_unlock(&f.mu);
    CHECK(ev(&f, "{\"ev\":\"check_request\"}") == CORE_OK);
    settle(&f);
    CHECK(snap_flag(&f, F_WIN_R1_DISMISS_VISIBLE) == 0 && snap_flag(&f, F_WIN_RESTORE_VISIBLE) == 0 && f.c->v.win_r1_dismissed == 0);
    ff_end(&f);

    /* already dismissed in the first R1 answer: Put Back alone */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_state = R1_LIVE_MISSING;
    f.r1_dismissed = 1;
    CHECK(ev(&f, "{\"ev\":\"start\"}") == CORE_OK);
    settle(&f);
    CHECK(snap_flag(&f, F_WIN_R1_DISMISS_VISIBLE) == 0 && snap_flag(&f, F_WIN_RESTORE_VISIBLE) == 1);
    ff_end(&f);

    /* under an action (Put Back's restore) it is off with Put Back (the frames' check), and back after */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_state = R1_LIVE_FOREIGN;
    CHECK(ev(&f, "{\"ev\":\"start\"}") == CORE_OK);
    settle(&f);
    f.nadmin_rc = 1;
    f.admin_rc[0] = 1;
    answers(&f, 2, "Put Back Pioneer's File", "OK");
    ev(&f, "{\"ev\":\"click\",\"id\":\"restore_pioneer\"}");
    settle(&f);
    CHECK(f.r1_busy_frames > 0 && f.r1_busy_bad == 0);
    CHECK(snap_flag(&f, F_WIN_R1_DISMISS_VISIBLE) == 1 && snap_flag(&f, F_WIN_R1_DISMISS_ENABLED) == 1);   /* the restore failed: R1 still holds */
    ff_end(&f);

    /* the write fails, or there is nothing to dismiss now: logged, the button stays */
    struct { int rc; const char *line; } cases[] = {
        {2, "log   couldn't remember the answer"},
        {1, "log   nothing to put back now: not remembered"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ffw_init(&f);
        f.cache = f.traces = f.chosen_cache = 1;
        f.r1_state = R1_ORT_MISSING;
        f.r1_dismiss_rc = cases[i].rc;
        ev(&f, "{\"ev\":\"start\"}");
        settle(&f);
        ev(&f, "{\"ev\":\"r1_dismiss\",\"versions\":\"7.2.10\"}");
        settle(&f);
        CHECKS(f.ndialogs == 0 && has(&f, cases[i].line) && !has(&f, "log   remembered: the watcher won't open RB Stems Plus for R1 in rekordbox 7.2.10"), cases[i].line);
        CHECKS(r1_calls(&f) == 2 && snap_flag(&f, F_WIN_R1_DISMISS_VISIBLE) == 1 && snap_flag(&f, F_WIN_R1_DISMISS_ENABLED) == 1, cases[i].line);
        ended_clean(&f, cases[i].line);
        ff_end(&f);
    }

    /* no R1: no button */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    ev(&f, "{\"ev\":\"start\"}");
    settle(&f);
    CHECK(snap_flag(&f, F_WIN_R1_DISMISS_VISIBLE) == 0 && snap_flag(&f, F_WIN_R1_DISMISS_ENABLED) == 0);
    ff_end(&f);

    /* the Mac's core has no such button */
    ff_init(&f);
    ev(&f, "{\"ev\":\"click\",\"id\":\"r1_dismiss\"}");
    ev(&f, "{\"ev\":\"r1_dismiss\",\"versions\":\"7.2.10\"}");
    settle(&f);
    CHECK(f.ndialogs == 0 && count_of(&f, "step ") == 0);
    ff_end(&f);
}

/* the grouped ruling: with several versions to repair, "Don't Ask Again for These Versions" covers
 * every one shown and the click names them; a version the button didn't show at the click (a row
 * that appeared since, or one the shell never showed) is never written */
static void t_win_r1_dismiss_grouped(void) {
    static const char *const TWO = "7.2.21\t0\t0\t0\t0\t0\t7.2.21.0400\n7.2.20\t3\t5\t0\t1\t0\t7.2.20.0354\n7.2.19\t3\t6\t0\t2\t0\t7.2.19.0342\n";
    static const char *const ONE = "7.2.21\t0\t0\t0\t0\t0\t7.2.21.0400\n7.2.20\t0\t0\t0\t0\t0\t7.2.20.0354\n7.2.19\t3\t6\t0\t2\t0\t7.2.19.0342\n";
    ff f;
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_state = R1_LIVE_MISSING;
    f.win_rows = TWO;
    CHECK(ev(&f, "{\"ev\":\"start\"}") == CORE_OK);
    settle(&f);
    CHECK(snap_flag(&f, F_WIN_R1_DISMISS_VISIBLE) == 1 && snap_flag(&f, F_WIN_R1_DISMISS_ENABLED) == 1
          && !strcmp(snap_str(&f, F_WIN_R1_DISMISS_VERSIONS), "7.2.20,7.2.19"));
    CHECK(ev(&f, "{\"ev\":\"r1_dismiss\",\"versions\":\"7.2.20,7.2.19\"}") == CORE_OK);
    settle(&f);
    CHECK(count_of(&f, "step WIN_R1_DISMISS 7.2.20,7.2.19") == 1
          && in_order(&f, 2, "log user chose: Don't Ask Again for These Versions (R1)",
                      "log   remembered: the watcher won't open RB Stems Plus for R1 in rekordbox 7.2.20,7.2.19"));
    ended_clean(&f, "r1 dismiss grouped");
    ff_end(&f);

    /* the button showed 7.2.19 alone; 7.2.20 needs repair too since, unseen: only 7.2.19 */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_state = R1_LIVE_MISSING;
    f.win_rows = ONE;
    ev(&f, "{\"ev\":\"start\"}");
    settle(&f);
    CHECK(!strcmp(snap_str(&f, F_WIN_R1_DISMISS_VERSIONS), "7.2.19"));
    pthread_mutex_lock(&f.mu);
    f.win_rows = TWO;   /* the next refresh would show both */
    pthread_mutex_unlock(&f.mu);
    CHECK(ev(&f, "{\"ev\":\"r1_dismiss\",\"versions\":\"7.2.19\"}") == CORE_OK);
    settle(&f);
    CHECK(count_of(&f, "step WIN_R1_DISMISS 7.2.19\n") == 1 && count_of(&f, "step WIN_R1_DISMISS") == 1);
    CHECK(!strcmp(snap_str(&f, F_WIN_R1_DISMISS_VERSIONS), "7.2.20,7.2.19"));   /* R1 asked again: both now */
    ff_end(&f);

    /* a click naming a version the button doesn't cover (the shell's list, stale or wrong): only
     * the ones it covers go; none of them: nothing is written */
    ffw_init(&f);
    f.cache = f.traces = f.chosen_cache = 1;
    f.r1_state = R1_LIVE_MISSING;
    f.win_rows = ONE;
    ev(&f, "{\"ev\":\"start\"}");
    settle(&f);
    CHECK(ev(&f, "{\"ev\":\"r1_dismiss\",\"versions\":\"7.2.21,7.2.20,7.2.19\"}") == CORE_OK);
    settle(&f);
    CHECK(count_of(&f, "step WIN_R1_DISMISS 7.2.19\n") == 1 && count_of(&f, "step WIN_R1_DISMISS") == 1);
    CHECK(ev(&f, "{\"ev\":\"r1_dismiss\",\"versions\":\"7.2.20\"}") == CORE_OK);
    settle(&f);
    CHECK(count_of(&f, "step WIN_R1_DISMISS") == 1 && has(&f, "log   the versions shown aren't the ones it covers now: not remembered"));
    CHECK(ev(&f, "{\"ev\":\"r1_dismiss\"}") == CORE_E_ARG);
    ff_end(&f);
}

/* Lane C */
static void t_win_switch_unmeasured_this_pc(void) {
    for (int found = 0; found <= 2; found += 2) {
        for (int reply = 0; reply <= 2; reply++) {
            ff f;
            ffw_init(&f);
            f.set_gb = "20";
            f.set_days = "30";
            f.row_answer[0] = 0;
            f.pick = f.probe_real = "E:\\DJ";
            f.probe_drive = "STICK";
            f.probe_mount = "E:\\";
            f.discover = found;
            f.disc_dir = "E:\\DJ\\RB Stems Plus Cache";
            f.disc_id = "ABC";
            f.writable = 1;
            f.size_this_mac_reply = reply;
            f.size_this_mac = reply ? 3LL << 30 : 0;   /* zero when Cache was absent at the probe */
            f.size_dir = 1LL << 30;
            f.pc_identity = TEST_LOCATION_IDENTITY;
            f.this_mac_now = 1;
            answers(&f, 3, "", "Use \"STICK\"", "Cancel");
            ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
            settle(&f);
            CHECKS(!strcmp(f.dlg_msg[1], found
                       ? "Stems Cache will use the 1.0 GB already saved on \"STICK\". The stems saved on this PC will be deleted: rekordbox separates those tracks again the next time you load them."
                       : "Stems Cache will save stems on \"STICK\". The stems saved on this PC will be deleted: rekordbox separates those tracks again the next time you load them."), f.dlg_msg[1]);
            CHECK(in_order(&f, 3, "step LOCATION_SIZE this_mac", "dialog Use \"STICK\"? [Use \"STICK\"|Cancel] -> Use \"STICK\"",
                           "step THIS_MAC_DELETE delete " TEST_LOCATION_IDENTITY));
            ff_end(&f);
        }
    }
}

static void t_win_switch_unmeasured_found(void) {
    ff f;
    ffw_init(&f);
    f.set_gb = "20";
    f.set_days = "30";
    f.row_answer[0] = 0;
    f.pick = f.probe_real = "E:\\DJ";
    f.probe_drive = "STICK";
    f.probe_mount = "E:\\";
    f.discover = 2;
    f.disc_dir = "E:\\DJ\\RB Stems Plus Cache";
    f.disc_id = "ABC";
    f.writable = 1;
    f.size_dir_unavailable = 1;
    answers(&f, 3, "", "Use \"STICK\"", "Cancel");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
    settle(&f);
    CHECKS(!strcmp(f.dlg_msg[1], "Stems Cache will use the stems already saved on \"STICK\"."), f.dlg_msg[1]);
    CHECK(has_suffix_line(&f, "step LOCATION_WRITE E:", " 0 <found>"));
    ff_end(&f);
}

static void t_win_create_marker_failure(void) {
    for (int left = 0; left <= 1; left++) {
        ff f;
        ffw_init(&f);
        f.set_gb = "20";
        f.set_days = "30";
        f.row_answer[0] = 0;
        f.pick = f.probe_real = "E:\\DJ";
        f.probe_drive = "STICK";
        f.probe_mount = "E:\\";
        f.create_rc = 1;
        f.create_code = 3;
        f.create_err = "Access is denied (5)";
        f.create_left_folder = left;
        answers(&f, 4, "", "Use \"STICK\"", "OK", "Cancel");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
        settle(&f);
        CHECKS(!strcmp(f.dlg_msg[2], left
                       ? "RB Stems Plus left an unfinished cache folder. Choose another folder."
                       : "RB Stems Plus couldn't set up its cache folder. Try again."), f.dlg_msg[2]);
        ff_end(&f);
    }
}

static void t_win_confirmed_location_timeout(void) {
    for (int step = 0; step < 5; step++) {
        ff f;
        ffw_init(&f);
        f.set_gb = "20";
        f.set_days = "30";
        f.row_answer[0] = 0;
        f.pick = f.probe_real = "E:\\DJ";
        f.probe_drive = "STICK";
        f.probe_mount = "E:\\";
        int promised_deletion = step != 4;
        int unchecked = step >= 3;
        f.pc_identity = promised_deletion ? TEST_LOCATION_IDENTITY : NULL;
        f.this_mac_now = 1;
        f.create_rc = step == 0;
        f.create_code = WIN_LOCATION_TIMED_OUT;
        f.write_rc = step == 1 || step == 2;
        f.write_code = unchecked ? WIN_LOCATION_SAVED_UNCHECKED : WIN_LOCATION_TIMED_OUT;
        f.write_left_folder = step == 2;
        answers(&f, 4, "", "Use \"STICK\"", "OK", "Cancel");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"settings\"}");
        settle(&f);
        CHECKS((strstr(f.dlg_msg[1], "will be deleted") != NULL) == promised_deletion, f.dlg_msg[1]);
        CHECKS(!strcmp(f.dlg_msg[2], step == 4
                    ? "The new location is saved, but its drive didn't respond in time to be checked." : step == 3
                    ? "The new location is saved, but its drive didn't respond in time to be checked. The stems on this PC weren't deleted." : step == 2
                    ? "The drive didn't respond in time, and RB Stems Plus may have left an unfinished cache folder there. Stems are still saved where they were. Choose another folder."
                    : "The drive didn't respond in time. Stems are still saved where they were. Try again."), f.dlg_msg[2]);
        CHECK(!count_of(&f, "step THIS_MAC_DELETE delete ") && !count_of(&f, "step THIS_MAC_DELETE defer ") &&
              (count_of(&f, "log settings: stems are saved in ") != 0) == unchecked);
        ff_end(&f);
    }
}

static void t_win_install_ignores_stale_floor(void) {
    ff f;
    ffw_init(&f);
    f.location_slow_reply = 1;
    view_location_measure(f.c);
    settle(&f);
    CHECK(f.c->v.loc.known);
    f.location_slow_reply = 2;
    view_location_measure(f.c);
    settle(&f);
    f.paused_floor = 7LL << 30;   /* a stale memo also must not fall back to the host's old floor */
    answers(&f, 1, "Cancel");
    ev(&f, "{\"ev\":\"click\",\"id\":\"install_cache\"}");
    settle(&f);
    CHECKS(f.ndialogs == 1 && !strcmp(f.dlg_msg[0],
           "Saves the stems rekordbox separates, with either stems model, so they load much faster the next time."), f.dlg_msg[0]);
    ff_end(&f);
}

static void t_win_uninstall_passes_pc_identity(void) {
    static const struct { int external, anyway; } cases[] = {{1, 0}, {1, 1}, {0, 1}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ff f;
        ffw_init(&f);
        if (cases[i].external) {
            f.loc_dir = "\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0}\\rbstemsplus";
            f.loc_shown = "E:\\rbstemsplus";
            f.loc_drive = f.loc_name = "STICK";
            f.loc_gate_id = "ABC";
        }
        f.loc_measured = 1;
        f.pc_identity = TEST_LOCATION_IDENTITY;
        f.app_fate = 0;
        if (cases[i].anyway) {
            f.plus = f.chosen_model = 1;
            f.restore_rc = 1;
            f.restore_code = CORE_MODEL_UNKNOWN;
            answers(&f, 3, "Uninstall", "Remove RB Stems Plus Anyway", "Done");
        } else answers(&f, 2, "Uninstall", "Done");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        CHECK(f.ndialogs == 2 + cases[i].anyway);
        CHECK(count_of(&f, "step THIS_MAC_DELETE target") == 1);
        CHECK(in_order(&f, 3, "step THIS_MAC_DELETE target", "dialog Uninstall RB Stems Plus? [Uninstall|Cancel] -> Uninstall",
                       "step REMOVE_OWN_FOLDERS 0 " TEST_LOCATION_IDENTITY));
        ff_end(&f);
    }
}

static void t_win_startup_cache_offer(void) {
    static const char old[] = "3333333333333333333333333333333333333333333333333333333333333333";
    const char *rows[] = {
        "7.2.20\t2\t1\t0\t0\t0\t7.2.20.0354\n7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n",
        "7.2.20\t1\t0\t0\t0\t0\t7.2.20.0354\n7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n",
    };
    for (int i = 0; i < 3; i++) {
        ff f;
        ffw_init(&f);
        f.cache = f.chosen_cache = f.payload_due = 1;
        f.win_rows = rows[i == 0 ? 0 : 1];
        f.bridge_installed = old;  /* the newest folder's stock DLL */
        f.win_covered_bridge = i == 2 ? old : "";
        answers(&f, 1, "Not Now");
        ev(&f, "{\"ev\":\"shown\"}");
        settle(&f);
        CHECK(count_of(&f, "dialog Stems Cache update") == (i == 2));
        CHECK(count_of(&f, "step PAYLOAD fetch") == 1);
        if (i == 2) CHECK(in_order(&f, 2, "step PAYLOAD check", "dialog Stems Cache update [Update|Not Now] -> Not Now"));
        ff_end(&f);
    }
}

static void t_win_inventory_refresh(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.chosen_cache = 1;
    f.win_rows = "7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n";
    ev(&f, "{\"ev\":\"shown\"}");
    settle(&f);
    CHECK(count_of(&f, "step PAYLOAD refresh-compat") == 1 && !has(&f, "step PAYLOAD fetch"));
    f.win_covered_bridge = "3333333333333333333333333333333333333333333333333333333333333333";
    f.win_rows = "7.2.20\t2\t1\t0\t0\t0\t7.2.20.0354\n7.2.19\t0\t0\t0\t0\t0\t7.2.19.0342\n";
    ev(&f, "{\"ev\":\"rekordbox_changed\"}");
    settle(&f);
    CHECK(count_of(&f, "step PAYLOAD refresh-compat") == 2 && !f.ndialogs && !f.off_main_violations);
    ev(&f, "{\"ev\":\"activated\"}");
    settle(&f);
    CHECK(count_of(&f, "step PAYLOAD refresh-compat") == 3 && !f.ndialogs);
    ff_end(&f);
}

static void flows_win_tests(void) {
    RUN(t_win_inventory_refresh);
    RUN(t_win_startup_cache_offer);
    RUN(t_win_ort_listed);
    RUN(t_win_native_admin);
    RUN(t_win_install_two_versions);
    RUN(t_win_watcher_install_two_versions);
    RUN(t_win_install_noop_full_coverage);
    RUN(t_win_install_empty_scope);
    RUN(t_win_reinstall_empty_cache_scope);
    RUN(t_win_install_scope_changed_once);
    RUN(t_win_install_undo_incomplete);
    RUN(t_win_install_partial);
    RUN(t_win_uninstall_partial_keeps_choice);
    RUN(t_win_uninstall_all_partial_nothing_removed);
    RUN(t_win_uninstall_all_names_cache_versions);
    RUN(t_win_restore_versions);
    RUN(t_win_rb_open_names_version);
    RUN(t_win_request_adds_uncovered_version);
    RUN(t_win_settings_multiple_versions);
    RUN(t_win_install_cache);
    RUN(t_win_install_keeps_compatibility_target);
    RUN(t_win_cache_opt_in);
    RUN(t_win_uac_declined);
    RUN(t_win_run_again);
    RUN(t_win_rb_open_for_worker);
    RUN(t_win_update_first);
    RUN(t_win_still_closing);
    RUN(t_win_closing_lines);
    RUN(t_win_restore);
    RUN(t_win_restore_empty_scope);
    RUN(t_win_r1);
    RUN(t_win_watcher_refreshes_manage_quietly);
    RUN(t_win_r1_dismiss);
    RUN(t_win_r1_dismiss_grouped);
    RUN(t_win_uninstall_all);
    RUN(t_win_uninstall_all_cache_fails);
    RUN(t_win_failed_old_cache_cleanup_preserves_choice);
    RUN(t_win_settings_omits_reinstall_when_build_disables_drive_choice);
    RUN(t_win_uninstall_nameless_drive);
    RUN(t_win_uninstall_drive_measurement);
    RUN(t_win_unchecked_uninstall_skips_probe);
    RUN(t_win_uninstall_drive_path);
    RUN(t_win_remove_location_codes);
    RUN(t_win_nameless_connected);
    RUN(t_win_use_this_pc_nameless);
    RUN(t_win_settings_open_read_timeout);
    RUN(t_win_use_this_pc_read_timeout);
    RUN(t_win_refuse_nameless);
    RUN(t_win_picker_check_timeout_refuses);
    RUN(t_win_uninstall_from_settings);
    RUN(t_win_matrix);
    RUN(t_win_switch_unmeasured_this_pc);
    RUN(t_win_switch_unmeasured_found);
    RUN(t_win_create_marker_failure);
    RUN(t_win_confirmed_location_timeout);
    RUN(t_win_install_ignores_stale_floor);
    RUN(t_win_uninstall_passes_pc_identity);
}
