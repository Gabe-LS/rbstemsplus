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
                   "admin 0 7.2.10.0342 - 2222222222222222222222222222222222222222222222222222222222222222 ortsha 0", "step SET_CHOSEN 0 1",
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
    CHECK(in_order(&f, 4, "admin 0 7.2.10.0342 - 2222222222222222222222222222222222222222222222222222222222222222 ortsha 0",
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
                   "admin 5 7.2.10.0342 - - - 0", "log restore: done"));
    CHECK(frame(&f, "Putting back Pioneer's file…|1|2") && count_of(&f, "\nadmin ") == 1);
    /* C3: the busy marker names the restore while the worker changes rekordbox */
    CHECK(in_order(&f, 3, "step BUSY_MARKER restore Put Back Pioneer's File", "admin 5 7.2.10.0342 - - - 0", "log restore: done"));
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
        CHECK(in_order(&f, 4, "admin 2 7.2.10.0342 - - - 0", "step LAUNCH_AGENT_UNLOAD", "step REMOVE_OWN_FOLDERS 0",
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

/* review NR2: the complete uninstall with Stems Cache's drive unplugged and nameless (""): the
 * confirmation and the done alert say "Stems Cache's drive", never `drive ""` */
static void t_win_uninstall_nameless_drive(void) {
    ff f;
    ffw_init(&f);
    f.cache = f.traces = 1;
    f.chosen_cache = 0;
    f.loc_dir = "E:\\rbstemsplus";
    f.loc_drive = f.loc_name = "";
    f.app_fate = 0;
    f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
    answers(&f, 3, "Uninstall", "Keep", "Done");
    ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
    settle(&f);
    CHECKS(f.ndialogs == 3 && strstr(f.dlg_msg[0], "RB Stems Plus will be deleted. Its stems on Stems Cache's drive, which isn't connected, stay: "
                                                   "connect it first to delete them too.") && !strstr(f.dlg_msg[0], "\"\""), f.dlg_msg[0]);
    CHECKS(!strcmp(f.dlg_msg[2], "rekordbox has its own stems model again. Stems Cache stays for the other account. The stems saved on "
                                 "Stems Cache's drive stay: it isn't connected. To delete them, delete the folder \"E:\\rbstemsplus\" in File Explorer."), f.dlg_msg[2]);
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

/* a chosen drive that is connected but has neither a label nor a letter (its folder is the volume
 * GUID path): the connected-only texts' nameless forms (review NT1) */
static void win_nameless_connected(ff *f) {
    f->loc_dir = "\\\\?\\Volume{0b1c}\\RB Stems Plus Cache";
    f->loc_drive = f->loc_name = "";
    f->loc_shown = "\\\\?\\Volume{0b1c}\\RB Stems Plus Cache";
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
        f.app_fate = 0;
        f.app_path = "C:\\Users\\x\\AppData\\Local\\Programs\\RB Stems Plus";
        answers(&f, 2, "Uninstall", "Done");
        ev(&f, "{\"ev\":\"menu\",\"id\":\"uninstall_all\"}");
        settle(&f);
        CHECKS(f.ndialogs == 2 && !strcmp(f.dlg_msg[0], "rekordbox goes back to exactly how it was. Your music and rekordbox library aren't touched.\n\n"
                                          "RB Stems Plus will be deleted, including all stems saved in its folder on Stems Cache's drive (about 1.2 GB), "
                                          "also any saved there by other computers or accounts."), f.dlg_msg[0]);
        CHECK(has(&f, "step REMOVE_LOCATION \\\\?\\Volume{0b1c}\\RB Stems Plus Cache ABC 1"));
        CHECK(frame_starts(&f, "Deleting the stems saved on Stems Cache's drive…|") && frame_starts(&f, "Deleting the stems saved on Stems Cache's drive… 7 files|"));
        if (fail) CHECKS(strstr(f.dlg_msg[1], "Some of the stems saved on Stems Cache's drive couldn't be deleted: delete the folder "
                                              "\"\\\\?\\Volume{0b1c}\\RB Stems Plus Cache\" in File Explorer.") != NULL, f.dlg_msg[1]);
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
        CHECKS(f.ndialogs == 2 && strstr(f.dlg_msg[1], "The stems saved on Stems Cache's drive stay. To delete them, delete the folder "
                                                       "\"\\\\?\\Volume{0b1c}\\RB Stems Plus Cache\" in File Explorer.") != NULL, f.dlg_msg[1]);
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
        CHECK(in_order(&f, 2, "step REMOVE_LOCATION \\\\?\\Volume{0b1c}\\RB Stems Plus Cache ABC 0", "dialog Saved stems not cleared [OK] -> OK"));
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

static void flows_win_tests(void) {
    RUN(t_win_ort_listed);
    RUN(t_win_native_admin);
    RUN(t_win_install_cache);
    RUN(t_win_uac_declined);
    RUN(t_win_run_again);
    RUN(t_win_rb_open_for_worker);
    RUN(t_win_update_first);
    RUN(t_win_still_closing);
    RUN(t_win_closing_lines);
    RUN(t_win_restore);
    RUN(t_win_uninstall_all);
    RUN(t_win_uninstall_all_cache_fails);
    RUN(t_win_uninstall_nameless_drive);
    RUN(t_win_nameless_connected);
    RUN(t_win_use_this_pc_nameless);
    RUN(t_win_refuse_nameless);
    RUN(t_win_uninstall_from_settings);
    RUN(t_win_matrix);
}
