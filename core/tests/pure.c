/* The pure functions (M5) whose Swift can't be called on its own (Controller.refresh's buttons and
 * status line, validateMenuItem, the reasons of Compat.swift that read the Mac's files, State.swift's
 * file functions, the watcher's question): cases written from that Swift, line by line (each block
 * names it). Also the text builder's own rules. vectors.c covers the rest against the Swift itself. */
#include "../internal.h"
#include "../native.h"
#include <stdio.h>

static int *pp, *pf;
#define CHECK(cond) do { if (cond) (*pp)++; else { (*pf)++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define TB(name, cap) char name##_buf[cap]; tbuf name; tb_init(&name, CORE_OS_MAC, name##_buf, sizeof name##_buf)

static void t_builder(void) {
    char small[8];
    tbuf b;
    tb_init(&b, CORE_OS_MAC, small, sizeof small);
    tb_puts(&b, "abc");
    CHECK(!b.err && b.len == 3 && !strcmp(small, "abc"));
    TB_TEXT0(&b, T_CHECKING);   /* "Checking…": cut short, counted whole */
    CHECK(b.err == CORE_E_SPACE && b.len == 3 + strlen("Checking…") && strlen(small) == 7);
    tb_init(&b, CORE_OS_MAC, small, sizeof small);
    core_slot bad = SLOT_S(drive, "x");
    tb_text(&b, T_OK, &bad, 1);   /* a slot the text doesn't use */
    CHECK(b.err == CORE_E_ARG && small[0] == 0);
#ifdef CORE_WIN_POLICY
    tb_init(&b, CORE_OS_WIN, small, sizeof small);
    TB_TEXT0(&b, T_PW_TITLE);
    CHECK(b.err == CORE_E_NOTYET);   /* a Mac-only text: no Windows words (core/tests/win_texts.c) */
#endif
    tb_init(&b, CORE_OS_MAC, NULL, 0);
    tb_i64(&b, INT64_MIN);
    CHECK(b.len == 20 && b.err == CORE_E_SPACE);
    TB(n, 32);
    tb_i64(&n, -42);
    tb_i64(&n, 0);
    CHECK(!strcmp(n.p, "-420"));
}

/* Compat.swift stemsPlusBlocked, stemsCacheBlocked; Controller.swift refresh() */
static void t_gating(void) {
    plus_facts pf0 = {1, "0002", 1};
    CHECK(plus_blocked(&pf0) == FEAT_OK);
    pf0.engine_supported = 0;
    CHECK(plus_blocked(&pf0) == FEAT_ENGINE_UNLISTED && feature_why_checks_again(FEAT_ENGINE_UNLISTED));
    pf0.engine = "";
    CHECK(plus_blocked(&pf0) == FEAT_NO_ENGINE);
    pf0.rekordbox_installed = 0;
    CHECK(plus_blocked(&pf0) == FEAT_RB_NOT_INSTALLED);

    cache_facts c = {1, 1, "7.2.19.0342", 1, 0, 0, 0, 1, 1, "1.18.0", "1.18"};
    CHECK(cache_blocked(&c) == FEAT_OK);
    c.ort_version = NULL;   /* dylibVersion nil: "unknown", never the prefix */
    CHECK(cache_blocked(&c) == FEAT_ORT_UNLISTED);
    TB(w, 256);
    w_feature_why(&w, FEAT_ORT_UNLISTED, NULL, NULL, NULL);
    CHECK(!strcmp(w.p, "Stems Cache doesn't support ONNX Runtime unknown yet. RB Stems Plus checks again each time you open it."));
    c.ort_version = "1.18.0";
    c.lib_signed = 0;
    CHECK(cache_blocked(&c) == FEAT_ORT_NOT_ORIGINAL && !feature_why_checks_again(FEAT_ORT_NOT_ORIGINAL));
    c.lib_exists = 0;
    CHECK(cache_blocked(&c) == FEAT_NO_ORT);
    c.cache_present = 1; c.prototype_exists = 1;
    CHECK(cache_blocked(&c) == FEAT_TEST_VERSION);
    c.root_ort_exists = 1;
    CHECK(cache_blocked(&c) == FEAT_NO_ORT);
    c.rekordbox_supported = 0;
    CHECK(cache_blocked(&c) == FEAT_RB_UNLISTED);
    tb_init(&w, CORE_OS_MAC, w_buf, sizeof w_buf);
    w_feature_why(&w, FEAT_RB_UNLISTED, NULL, "7.2.19.0342", NULL);
    CHECK(!strcmp(w.p, "Stems Cache doesn't support rekordbox 7.2.19 yet. RB Stems Plus checks again each time you open it."));
    c.rekordbox_installed = 0;
    CHECK(cache_blocked(&c) == FEAT_RB_NOT_INSTALLED);
    c.user_is_admin = 0;
    CHECK(cache_blocked(&c) == FEAT_NEEDS_ADMIN);
#ifdef CORE_WIN_POLICY
    /* Windows: Pioneer's ORT by sha256 against the verified list, never the version prefix */
    cache_facts cw = {1, 1, "7.2.19.0342", 1, 0, 0, 0, 1, 1, "1.18.0", "9.99"};
    CHECK(cache_blocked_win(&cw, 1, 0) == FEAT_OK);   /* the prefix doesn't match: not read */
    CHECK(cache_blocked_win(&cw, 0, 0) == FEAT_ORT_UNLISTED && feature_why_checks_again(FEAT_ORT_UNLISTED));
    cw.ort_version = NULL;
    CHECK(cache_blocked_win(&cw, 1, 0) == FEAT_OK);   /* no version resource: still listed by its sha256 */
    cw.lib_signed = 0;
    CHECK(cache_blocked_win(&cw, 1, 0) == FEAT_ORT_NOT_ORIGINAL);
    cw.lib_exists = 0;
    CHECK(cache_blocked_win(&cw, 1, 0) == FEAT_NO_ORT);
    /* HF7: no answer for it is not "can't find it", and not "checks again" */
    CHECK(cache_blocked_win(&cw, 1, 1) == FEAT_NOT_CHECKED && !feature_why_checks_again(FEAT_NOT_CHECKED));
    {
        char nb[256];
        tbuf n;
        tb_init(&n, CORE_OS_WIN, nb, sizeof nb);
        w_feature_why(&n, FEAT_NOT_CHECKED, NULL, NULL, NULL);
        CHECK(!strcmp(n.p, "Stems Cache couldn't check rekordbox just now."));
    }
    cw.rekordbox_supported = 0;   /* no verified payload: nothing listed */
    CHECK(cache_blocked_win(&cw, 0, 0) == FEAT_RB_UNLISTED);
    cw.rekordbox_installed = 0;
    CHECK(cache_blocked_win(&cw, 1, 0) == FEAT_RB_NOT_INSTALLED);
    cw.user_is_admin = 0;
    CHECK(cache_blocked_win(&cw, 1, 0) == FEAT_NEEDS_ADMIN);

    /* blockedByVersionList from the native BLOCKED facts (native.h), the core's ordered rules: an
     * administrator, rekordbox installed and listed, the engine listed, Pioneer's ORT listed */
    native_blocked_facts nb = {1, 1, "0002", 1, 1, 1, "7.2.19.0342", 1, 0, 1, 1, 1};
    CHECK(!native_blocked_by_list(&nb));
    nb.ort_listed = 0;
    CHECK(native_blocked_by_list(&nb));   /* the ORT unlisted: checks again */
    nb.ort_listed = 1;
    nb.engine_supported = 0;
    CHECK(native_blocked_by_list(&nb));   /* the engine unlisted */
    /* rekordbox absent (no current folder: not installed): nothing counts, whatever the rest says
     * (a leftover demucs3_ver.txt, nothing listed) */
    native_blocked_facts gone = {0, 1, "0002", 0, 1, 1, "", 0, 0, 0, 0, 0};
    CHECK(!native_blocked_by_list(&gone));
    gone.admin = 0;
    CHECK(!native_blocked_by_list(&gone));
    /* a standard account: the cache part is the admin line, never "checks again" */
    native_blocked_facts std = {1, 1, "0002", 1, 1, 0, "7.2.20.0001", 0, 0, 1, 1, 0};
    CHECK(!native_blocked_by_list(&std));   /* rekordbox unlisted: 0 */
    std.engine_supported = 0;
    CHECK(native_blocked_by_list(&std));    /* the engine unlisted: 1 */
    /* no engine yet ("turn on STEMS once"): not the list's */
    std.engine = "";
    CHECK(!native_blocked_by_list(&std));
    /* the cache's facts unanswered (the ORT unreadable): the engine part still counts (S23) */
    native_blocked_facts un = {1, 1, "0002", 0, 0, 1, "7.2.19.0342", 0, 0, 0, 0, 0};
    CHECK(native_blocked_by_list(&un));
    un.engine_supported = 1;
    CHECK(!native_blocked_by_list(&un));
    un.plus_answered = 0;
    un.cache_answered = 1;
    un.rekordbox_supported = 0;
    CHECK(native_blocked_by_list(&un));     /* and the reverse: the cache part alone */
    /* no verified payload: nothing listed, so it is due (API-V1-NOTES) */
    native_blocked_facts none = {1, 1, "0002", 0, 1, 1, "7.2.19.0342", 0, 0, 0, 0, 0};
    CHECK(native_blocked_by_list(&none));
    none.admin = 0;
    CHECK(native_blocked_by_list(&none));   /* the engine alone, on a standard account */
#endif

    /* nothing installed, an administrator, everything allowed */
    gate_in in;
    memset(&in, 0, sizeof in);
    in.rekordbox_installed = in.user_is_admin = in.own_bundle = 1;
    gate_out g;
    gate_buttons(&in, &g);
    CHECK(g.install_plus && g.install_cache && !g.reinstall && !g.reinstall_plus && !g.reinstall_cache && g.settings);
    CHECK(!g.uninstall_plus && !g.uninstall_cache && g.uninstall_all);
    TB(t, 256);
    w_reinstall_title(&t, &g);
    CHECK(!strcmp(t.p, "Reinstall Demucs v4"));   /* names(c.model || !c.cache, c.cache) */
    TB(s, 1024);
    w_status_lines(&s, &in);
    CHECK(s.len == 0);
    /* Stems Cache from another account: Install turns it on here, no administrator needed */
    in.cache = 1; in.cache_traces = 1;
    in.cache_why = FEAT_NEEDS_ADMIN; in.user_is_admin = 0;
    gate_buttons(&in, &g);
    CHECK(g.install_cache && !g.uninstall_cache && !g.uninstall_all);
    tb_init(&s, CORE_OS_MAC, s_buf, sizeof s_buf);
    w_status_lines(&s, &in);
    /* cacheWhy isn't said (cache && !c.cache); the standard line is */
    CHECK(!strcmp(s.p, "Removing Stems Cache needs an administrator account."));
    /* chosen here, a standard account: the standard line replaces needsAdminLine */
    in.chosen_cache = 1;
    gate_buttons(&in, &g);
    CHECK(!g.install_cache && !g.reinstall && !g.reinstall_cache);
    tb_init(&s, CORE_OS_MAC, s_buf, sizeof s_buf);
    w_status_lines(&s, &in);
    CHECK(!strcmp(s.p, "Reinstalling or removing Stems Cache needs an administrator account."));
    tb_init(&t, CORE_OS_MAC, t_buf, sizeof t_buf);
    w_reinstall_title(&t, &g);
    CHECK(!strcmp(t.p, "Reinstall Stems Cache"));
    /* rekordbox missing: one line for both features (lines.contains(why)) */
    memset(&in, 0, sizeof in);
    in.user_is_admin = 1;
    in.plus_why = in.cache_why = FEAT_RB_NOT_INSTALLED;
    in.chosen_model = 1;
    tb_init(&s, CORE_OS_MAC, s_buf, sizeof s_buf);
    w_status_lines(&s, &in);
    CHECK(!strcmp(s.p, "Move RB Stems Plus to the Applications folder, then open it from there.\nrekordbox 7 isn't installed."));
    gate_buttons(&in, &g);
    CHECK(!g.install_plus && !g.install_cache && !g.reinstall);
    tb_init(&t, CORE_OS_MAC, t_buf, sizeof t_buf);
    w_reinstall_title(&t, &g);
    CHECK(!strcmp(t.p, "Reinstall Demucs v4"));
    /* every other line, in refresh()'s order */
    memset(&in, 0, sizeof in);
    in.own_bundle = 1; in.user_is_admin = 1; in.plus = 1; in.chosen_model = 1; in.chosen_cache = 1;
    in.plus_why = FEAT_ENGINE_UNLISTED; in.engine = "0003";
    in.cache_why = FEAT_ORT_UNLISTED; in.ort_version = "1.19.1";
    in.cache_is_link = in.watcher_off = 1;
    in.location_notice = "L"; in.model_notice = "M"; in.notice = "N";
    tb_init(&s, CORE_OS_MAC, s_buf, sizeof s_buf);
    w_status_lines(&s, &in);
    CHECK(!strcmp(s.p, "Demucs v4 isn't available for STEMS Engine 0003 yet. RB Stems Plus checks again each time you open it.\n"
                       "Stems Cache doesn't support ONNX Runtime 1.19.1 yet. RB Stems Plus checks again each time you open it.\n"
                       "~/Library/Caches/rbstemsplus is a link, so RB Stems Plus leaves it and what it points to alone.\nL\n"
                       "Reinstall reminders are off. Turn on RB Stems Plus Watcher in System Settings › General › Login Items & Extensions.\nM\nN"));
    gate_buttons(&in, &g);
    CHECK(g.uninstall_plus && !g.reinstall_plus && !g.reinstall && !g.install_plus);
    tb_init(&t, CORE_OS_MAC, t_buf, sizeof t_buf);
    w_reinstall_title(&t, &g);
    CHECK(!strcmp(t.p, "Reinstall Demucs v4 + Stems Cache"));
    /* plusWhy only when it isn't installed or this account chose it */
    in.chosen_model = 0;
    in.cache_why = FEAT_OK; in.cache_is_link = in.watcher_off = 0; in.location_notice = in.model_notice = in.notice = NULL;
    tb_init(&s, CORE_OS_MAC, s_buf, sizeof s_buf);
    w_status_lines(&s, &in);
    CHECK(s.len == 0);
}

/* main.swift validateMenuItem */
static void t_menu_rules(void) {
    menu_facts f;
    memset(&f, 0, sizeof f);
    int32_t title;
    CHECK(menu_rule(M_VIEW_PREPARE, &f, &title) == CORE_MENU_ENABLED && title == T_MENU_PREPARE);
    CHECK(menu_rule(M_PREP_CHECK, &f, NULL) == 0);
    f.prep_selected = 1;
    CHECK(menu_rule(M_PREP_CHECK, &f, NULL) == CORE_MENU_ENABLED);
    f.preparing = 1;
    CHECK(menu_rule(M_PREP_CHECK, &f, NULL) == 0 && menu_rule(M_PREP_STOP, &f, NULL) == CORE_MENU_ENABLED);
    CHECK(menu_rule(M_PREP_PAUSE, &f, &title) == CORE_MENU_ENABLED && title == T_MENU_PAUSE);
    f.paused = 1;
    CHECK(menu_rule(M_PREP_PAUSE, &f, &title) == CORE_MENU_ENABLED && title == T_MENU_RESUME);
    /* each feature's items as their buttons */
    CHECK(menu_rule(M_MANAGE_PLUS_INSTALL, &f, NULL) == 0 && menu_rule(M_APP_UNINSTALL, &f, NULL) == 0);
    f.buttons.install_plus = f.buttons.uninstall_all = f.buttons.reinstall_cache = 1;
    CHECK(menu_rule(M_MANAGE_PLUS_INSTALL, &f, NULL) == CORE_MENU_ENABLED && menu_rule(M_APP_UNINSTALL, &f, NULL) == CORE_MENU_ENABLED);
    CHECK(menu_rule(M_MANAGE_CACHE_REINSTALL, &f, NULL) == CORE_MENU_ENABLED && menu_rule(M_MANAGE_CACHE_INSTALL, &f, NULL) == 0);
    CHECK(menu_rule(M_APP_UPDATE, &f, NULL) == (CORE_MENU_ENABLED | CORE_MENU_HIDDEN));
    f.update_visible = 1;
    CHECK(menu_rule(M_APP_UPDATE, &f, NULL) == CORE_MENU_ENABLED);
    /* an action runs: the controller's items go off; the views, the log, Help's links, the OS's own stay */
    f.action_running = 1;
    CHECK(menu_rule(M_MANAGE_PLUS_INSTALL, &f, NULL) == 0 && menu_rule(M_APP_SETTINGS, &f, NULL) == 0 && menu_rule(M_HELP_REPORT, &f, NULL) == 0);
    CHECK(menu_rule(M_PREP_PLAYLIST, &f, NULL) == 0 && menu_rule(M_APP_UPDATE, &f, NULL) == 0);
    CHECK(menu_rule(M_VIEW_STATUS, &f, NULL) == CORE_MENU_ENABLED && menu_rule(M_MANAGE_SHOW_LOG, &f, NULL) == CORE_MENU_ENABLED);
    CHECK(menu_rule(M_HELP_HOME, &f, NULL) == CORE_MENU_ENABLED && menu_rule(M_HELP_ISSUES, &f, NULL) == CORE_MENU_ENABLED);
    CHECK(menu_rule(M_MANAGE_PLUS, &f, NULL) == CORE_MENU_ENABLED && menu_rule(M_MANAGE, &f, NULL) == CORE_MENU_ENABLED);
    CHECK(menu_rule(M_APP_QUIT, &f, NULL) == CORE_MENU_ENABLED && menu_rule(M_PREP_STOP, &f, NULL) == CORE_MENU_ENABLED);
    CHECK(menu_rule(CORE_MENU_COUNT, &f, NULL) == 0);
    /* the table: every item has a title and today's keys */
    for (int32_t i = 0; i < CORE_MENU_COUNT; i++) {
        const menu_row *r = &core_menu_rows[i];
        CHECK(r->kind == CORE_MENU_SEPARATOR || r->title >= 0);
    }
    CHECK(!strcmp(core_menu_rows[M_VIEW_MANAGE].key_mac, "3") && !strcmp(core_menu_rows[M_PREP_STOP].key_mac, "."));
    CHECK(core_menu_rows[M_MANAGE_CACHE_UNINSTALL].parent == M_MANAGE_CACHE && core_menu_rows[M_MANAGE_CACHE].parent == M_MANAGE);
}

/* State.swift chosen, setChosen, dismissed, remember, missingKeys, setBusyMarker */
static void t_state_files(void) {
    int32_t m, c;
    state_chosen("model=1\ncache=0\n", 16, &m, &c);
    CHECK(m == 1 && c == 0);
    state_chosen("xcache=1model=10", 16, &m, &c);
    CHECK(m == 1 && c == 1);   /* s.contains(...), anywhere */
    state_chosen("model=1\xff", 8, &m, &c);
    CHECK(m == 0 && c == 0);   /* String(contentsOfFile:encoding:.utf8) can't read it */
    state_chosen(NULL, 0, &m, &c);
    CHECK(m == 0 && c == 0);
    char b[32];
    state_chosen_text(b, sizeof b, 1, 0);
    CHECK(!strcmp(b, "model=1\ncache=0\n"));

    arena a = {0};
    keyset k;
    const char *txt = "model:0003\ncache:7.2.19\n\ncache:7.2.19\r\nx\nmodel:0003";
    CHECK(keyset_parse(&a, txt, strlen(txt), &k) == 0 && k.n == 3);
    CHECK(keyset_has(&k, "model:0003", 10) && keyset_has(&k, "cache:7.2.19", 12) && !keyset_has(&k, "x", 1));
    CHECK(keyset_has(&k, "cache:7.2.19\r\nx", 15));   /* "\r\n" is one Character: no split there */
    pstr add[2] = {{"b", 1}, {"model:0003", 10}};
    TB(u, 256);
    keyset_union_text(&u, &k, add, 2);
    CHECK(!strcmp(u.p, "b\ncache:7.2.19\ncache:7.2.19\r\nx\nmodel:0003\n"));
    pstr out[2];
    CHECK(state_missing_keys(&a, 1, 1, 0, 0, "7.2.19.0342", "0003", &k, out) == 1 && out[0].n == 17 && !strcmp(out[0].s, "cache:7.2.19.0342"));
    CHECK(state_missing_keys(&a, 1, 1, 1, 1, "7", "1", NULL, out) == 0);
    CHECK(state_missing_keys(&a, 1, 0, 1, 0, "7", "1", NULL, out) == 1 && !strcmp(out[0].s, "model:1"));
    TB(bm, 256);
    busy_marker_text(&bm, 42, 1700000000, "install-cache", "Install Stems Cache");
    CHECK(!strcmp(bm.p, "42\n1700000000\ninstall-cache\nInstall Stems Cache\n"));
    busy_marker mk;
    CHECK(busy_marker_parse(&a, bm.p, bm.len, &mk) == 1 && mk.pid == 42 && mk.has_start && mk.start == 1700000000 && mk.has_action);
    CHECK(busy_marker_live(&mk, 1700000000, 0, 0) && !busy_marker_live(&mk, 1700000001, 1, 1800000000) && !busy_marker_live(&mk, -1, 1, 0));
    pstr p = {"/a/b//", 6}, n = prep_last_path_component(p);
    CHECK(n.n == 1 && n.s[0] == 'b');
    arena_free(&a);
}

/* Watcher.swift watcherCheck's question and answer, unsupportedKey */
static void t_watcher_question(void) {
    arena a = {0};
    keyset put_off;
    CHECK(keyset_parse(&a, "cache:7.2.20\nmodel:0003", 23, &put_off) == 0);
    pstr keys[2] = {{"cache:7.2.20", 12}, {"model:0003", 10}};
    CHECK(watcher_ask(keys, 0, &put_off) == WATCH_NOTHING);
    CHECK(watcher_ask(keys, 2, &put_off) == WATCH_PUT_OFF);
    CHECK(watcher_ask(keys, 2, NULL) == WATCH_ASK);
    pstr other = {"model:0004", 10};
    CHECK(watcher_ask(&other, 1, &put_off) == WATCH_ASK);
    watcher_dialog d;
    TB(m, 1024);
    watcher_dialog_build(&m, &d, 1, 1, "0003", "7.3.0", 1, 1, 0);
    CHECK(d.nbuttons == 3 && watcher_answer(&d, 0) == WANSWER_REINSTALL && watcher_answer(&d, 1) == WANSWER_PUT_OFF);
    CHECK(watcher_answer(&d, 2) == WANSWER_REMEMBER && watcher_answer(&d, -1) == WANSWER_PUT_OFF && watcher_answer(&d, 3) == WANSWER_PUT_OFF);
    tb_init(&m, CORE_OS_MAC, m_buf, sizeof m_buf);
    watcher_dialog_build(&m, &d, 1, 0, "0003", "7.3.0", 0, 1, 0);
    CHECK(d.blocked && d.nbuttons == 2 && watcher_answer(&d, 0) == WANSWER_REMEMBER_UNSUPPORTED && watcher_answer(&d, 1) == WANSWER_REMEMBER);
    static const char *const rb[] = {"7.2.19", "7.2.20"}, *const en[] = {"0002"};
    TB(k, 256);
    watcher_unsupported_key(&k, "cache:7.3.0", 11, rb, 2, en, 1, "1.18");
    CHECK(!strcmp(k.p, "cache:7.3.0 unsupported with 7.2.19,7.2.20,0002,1.18"));
    prep_estimate e;
    prep_estimate_init(&e);
    CHECK(e.fits && e.disk_ok && !watcher_newly_not_ready(&e, NULL));
    const char *line = "{\"file\":\"/m/a\",\"result\":\"skipped\"}";
    CHECK(prep_estimate_line(&a, &e, line, strlen(line)) == 0 && e.ntracks == 1);
    keyset noticed;
    CHECK(keyset_parse(&a, "/m/a", 4, &noticed) == 0);
    CHECK(watcher_newly_not_ready(&e, NULL) && !watcher_newly_not_ready(&e, &noticed));
    arena_free(&a);
}

/* Q2: a long drive name through the composers (255 emoji, 1,020 bytes, past a real name's 765;
 * then 2,000, past every sub-buffer): the whole text, or the error path, and never a read past a
 * buffer (ASan) */
static int has_drive(const char *s, const char *drive) { return strstr(s, drive) != NULL; }
static void t_long_drive(void) {
    static char drive[8004], longer[8004];
    for (int i = 0; i < 255; i++) memcpy(drive + 4 * i, "\xF0\x9F\x8E\xA7", 4);
    drive[1020] = 0;
    for (int i = 0; i < 2000; i++) memcpy(longer + 4 * i, "\xF0\x9F\x8E\xA7", 4);
    longer[8000] = 0;
    static char big[16384];
    tbuf b;
    tb_init(&b, CORE_OS_MAC, big, sizeof big);
    w_uninstall_all_message(&b, 1, 0, " (about 1.2 GB)", drive, 1);
    CHECK(!b.err && has_drive(big, drive) && strstr(big, "also any saved there by other Macs or accounts.") && strlen(big) == b.len);
    tb_init(&b, CORE_OS_MAC, big, sizeof big);
    w_switch_message(&b, drive, 1, 2000000000, 50000000, drive);
    CHECK(!b.err && strstr(big, "The stems saved on \"") && has_drive(big, drive) && strlen(big) == b.len);
    tb_init(&b, CORE_OS_MAC, big, sizeof big);
    w_location_removal_note(&b, drive, 1, " (about 1.0 GB)");
    CHECK(!b.err && has_drive(big, drive) && strlen(big) == b.len);
    /* a sub-buffer overflows: the error, nothing composed past it */
    tb_init(&b, CORE_OS_MAC, big, sizeof big);
    w_uninstall_all_message(&b, 0, 0, "", longer, 1);
    CHECK(b.err == CORE_E_SPACE && strlen(big) < sizeof big);
    tb_init(&b, CORE_OS_MAC, big, sizeof big);
    w_update_failed_message(&b, UPDATE_R_STAGING);
    CHECK(!b.err);
    /* the caller's buffer is small: the error, cut short, NUL-terminated */
    char small[64];
    tb_init(&b, CORE_OS_MAC, small, sizeof small);
    w_uninstall_all_message(&b, 0, 0, "", drive, 1);
    CHECK(b.err == CORE_E_SPACE && strlen(small) == sizeof small - 1);
    tb_init(&b, CORE_OS_MAC, small, sizeof small);
    w_switch_message(&b, drive, 0, 0, 0, NULL);
    CHECK(b.err == CORE_E_SPACE && strlen(small) == sizeof small - 1);
    tb_init(&b, CORE_OS_MAC, small, sizeof small);
    w_location_removal_note(&b, longer, 0, "");
    CHECK(b.err == CORE_E_SPACE && strlen(small) == sizeof small - 1);
    /* tb_len: what a slot may take */
    tb_init(&b, CORE_OS_MAC, small, 8);
    tb_puts(&b, "abcdefghij");
    CHECK(tb_len(&b) == 0);
    tb_init(&b, CORE_OS_MAC, small, 8);
    tb_puts(&b, "abc");
    CHECK(tb_len(&b) == 3);
}

#ifdef CORE_WIN_POLICY
/* review NR2: on Windows a drive with no name ("": unplugged, no label remembered) gets each
 * {drive} text's nameless form, never `drive ""`; a named drive, and the Mac, keep the named one */
static char nb[2048];
static tbuf nbuf(core_os os) { tbuf b; tb_init(&b, os, nb, sizeof nb); return b; }
static int is(const tbuf *b, const char *want) {
    if (!b->err && !strcmp(nb, want)) return 1;
    printf("  got %s (err %d), want %s\n", nb, b->err, want);
    return 0;
}
static void t_nameless_win(void) {
    static const struct { int32_t kind, probe; const char *want; } lights[] = {
        {LOC_BRIDGE_TOO_OLD, 0, "Stems Cache needs an update to save stems on its drive: it's saving them on this PC for now. Click Reinstall Stems Cache."},
        {LOC_CHOSEN, PROBE_NOT_CONNECTED, "Stems Cache's drive isn't connected: rekordbox separates tracks as usual. Connect it, then open rekordbox."},
        {LOC_CHOSEN, PROBE_FOLDER_MISSING, "Stems Cache's folder on its drive is missing or was replaced."},
        {LOC_CHOSEN, PROBE_MARKER_MISSING_OR_WRONG, "Stems Cache's folder on its drive is missing or was replaced."},
        {LOC_CHOSEN, PROBE_LINK, "Stems Cache isn't saving: a folder on the way to Stems Cache's folder on its drive is a link."},
        {LOC_CHOSEN, PROBE_NETWORK, "Stems Cache isn't saving: Stems Cache's drive is a network drive. Choose a drive connected to this PC in Settings."},
        {LOC_CHOSEN, PROBE_READ_ONLY, "Stems Cache's drive is read-only; Windows may need to repair it (the drive's Properties › Tools › Check in File Explorer)."},
        {LOC_CHOSEN, PROBE_NO_PERMISSION, "RB Stems Plus isn't allowed to use Stems Cache's drive. Check the folder's permissions (Properties › Security in File Explorer)."},
    };
    tbuf b;
    for (size_t i = 0; i < sizeof lights / sizeof *lights; i++) {
        loc_status st = {lights[i].kind, lights[i].probe, ""};
        b = nbuf(CORE_OS_WIN);
        CHECK(location_light(&b, &st, NULL, 0) == 1 && is(&b, lights[i].want));
    }
    static const struct { const char *state, *reason; int32_t probe; const char *want; } bridged[] = {
        {"pending", NULL, PROBE_AVAILABLE, "Stems Cache is still checking its drive."},
        {"unavailable", "not-connected", PROBE_AVAILABLE, "Stems Cache will use its drive again in a few seconds."},
        {"unavailable", "read-only", PROBE_AVAILABLE, "Stems Cache isn't using its drive right now: the drive is read-only; Windows may need to repair it (the drive's Properties › Tools › Check in File Explorer)."},
    };
    for (size_t i = 0; i < sizeof bridged / sizeof *bridged; i++) {
        loc_status st = {LOC_CHOSEN, bridged[i].probe, ""};
        loc_bridge br = {bridged[i].state, bridged[i].reason, ""};
        b = nbuf(CORE_OS_WIN);
        CHECK(location_light(&b, &st, &br, 0) == 1 && is(&b, bridged[i].want));
    }
    /* a named drive on Windows and "" on the Mac: the named form, as before */
    loc_status named = {LOC_CHOSEN, PROBE_FOLDER_MISSING, "D:"}, mac = {LOC_CHOSEN, PROBE_FOLDER_MISSING, ""};
    b = nbuf(CORE_OS_WIN);
    CHECK(location_light(&b, &named, NULL, 0) == 1 && is(&b, "Stems Cache's folder on \"D:\" is missing or was replaced."));
    b = nbuf(CORE_OS_MAC);
    CHECK(location_light(&b, &mac, NULL, 0) == 1 && is(&b, "Stems Cache's folder on \"\" is missing or was replaced."));
    /* the paused tip and the install note's paused line */
    b = nbuf(CORE_OS_WIN);
    w_cache_paused_tip(&b, 50000000000LL, "");
    CHECK(is(&b, "Stems Cache stopped saving stems: its drive has less than 50 GB free."));
    b = nbuf(CORE_OS_WIN);
    w_cache_paused_at_install(&b, 50000000000LL, "");
    CHECK(is(&b, "Stems Cache will stop saving stems while its drive has less than 50 GB free."));
    b = nbuf(CORE_OS_WIN);
    w_cache_paused_tip(&b, 50000000000LL, "STICK");
    CHECK(is(&b, "Stems Cache stopped saving stems: \"STICK\" has less than 50 GB free."));
    /* the not-connected flow texts */
    b = nbuf(CORE_OS_WIN);
    w_location_removal_note(&b, "", 0, "");
    CHECK(is(&b, " (its stems on Stems Cache's drive, which isn't connected, stay)"));
    b = nbuf(CORE_OS_WIN);
    w_uninstall_all_message(&b, 0, 0, "", "", 0);
    CHECK(!b.err && strstr(nb, "Its stems on Stems Cache's drive, which isn't connected, stay: connect it first to delete them too.") && !strstr(nb, "\"\""));
    b = nbuf(CORE_OS_WIN);
    w_switch_message(&b, "D:", 0, 0, 0, "");
    CHECK(is(&b, "Stems Cache will save stems on \"D:\". The stems saved on the previous drive stay there."));
    /* review NT1: the connected-only flow texts, for a connected drive with neither a label nor a letter */
    b = nbuf(CORE_OS_WIN);
    w_location_removal_note(&b, "", 1, " (about 1.2 GB)");
    CHECK(is(&b, ", including all stems saved in its folder on Stems Cache's drive (about 1.2 GB), also any saved there by other computers or accounts"));
    b = nbuf(CORE_OS_WIN);
    w_uninstall_all_message(&b, 0, 0, " (about 1.2 GB)", "", 1);
    CHECK(!b.err && strstr(nb, "RB Stems Plus will be deleted, including all stems saved in its folder on Stems Cache's drive (about 1.2 GB), "
                               "also any saved there by other computers or accounts.") && !strstr(nb, "\"\""));
    b = nbuf(CORE_OS_WIN);
    w_clear_location_message(&b, "");
    CHECK(is(&b, "Deletes all stems saved in this folder on Stems Cache's drive, including any saved there by other computers or accounts."));
    b = nbuf(CORE_OS_WIN);
    w_clear_location_message(&b, "D:");
    CHECK(is(&b, "Deletes all stems saved in this folder on \"D:\", including any saved there by other computers or accounts."));
    b = nbuf(CORE_OS_MAC);
    w_location_removal_note(&b, "", 1, "");
    CHECK(is(&b, ", including all stems saved in its folder on \"\", also any saved there by other Macs or accounts"));
    /* the choose rule: only Windows refuses a drive with no name */
    CHECK(location_refuse_nameless(CORE_OS_WIN, "") && location_refuse_nameless(CORE_OS_WIN, NULL));
    CHECK(!location_refuse_nameless(CORE_OS_WIN, "STICK") && !location_refuse_nameless(CORE_OS_MAC, ""));
    /* every nameless form names no drive slot, and texts with no nameless form map to themselves */
    CHECK(nameless_text(T_LEFT_DRIVE_NOT_CONNECTED) == T_WIN_LEFT_DRIVE_NOT_CONNECTED_NAMELESS);
    CHECK(nameless_text(T_CLEARING_DRIVE_N) == T_WIN_CLEARING_DRIVE_N_NAMELESS && nameless_text(T_LOCATION_LABEL) == T_WIN_LOCATION_LABEL_NAMELESS);
    CHECK(nameless_text(T_USE_DRIVE_Q) == T_USE_DRIVE_Q);
    for (int id = T_WIN_LOC_CHECKING_NAMELESS; id <= T_WIN_REFUSE_NAMELESS; id++) {
        const char *t = core_text_template(CORE_OS_WIN, (core_text_id)id);
        CHECK(t && *t && !strstr(t, "{drive}") && !strstr(t, "this Mac"));
    }
}
#endif

void pure_tests(int *passed, int *failed);
void pure_tests(int *passed, int *failed) {
    pp = passed;
    pf = failed;
    t_builder();
    t_gating();
    t_menu_rules();
    t_state_files();
    t_watcher_question();
    t_long_drive();
#ifdef CORE_WIN_POLICY
    t_nameless_win();
#endif
}
