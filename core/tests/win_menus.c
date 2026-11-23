/* menus.def's Windows rows (W-core, app.md §4.5; mac-core §3.8), under CORE_WIN_POLICY: the menu
 * bar's order (the first menu with Exit, Edit drawn from the table, About in Help, no Hide and no
 * Window menu), the shortcuts (Ctrl for ⌘, F1, Alt+F4), one access key per item and menu level
 * (WPF's "_" in texts.def's Windows column), and the rows a Manage-only host hides.
 * In the Mac build (as shipped) there are no Windows rows. Linked into core-test. */
#include "../internal.h"
#include "win_texts.h"
#include <ctype.h>
#include <stdio.h>

static int *g_passed, *g_failed;
#define CHECK(cond) do { if (cond) (*g_passed)++; else { (*g_failed)++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKS(cond, ctx) do { if (cond) (*g_passed)++; else { (*g_failed)++; printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond, (ctx)); } } while (0)

#ifdef CORE_WIN_POLICY

static int item_at(core_os os, int32_t id, core_menu_item *out) {
    for (int32_t i = 0; i < core_menu_count(os); i++)
        if (core_menu_at(os, i, out) == CORE_OK && out->id == id) return i;
    return -1;
}

/* the children of menu (or the top-level menus, NONE = -1), in order */
static int children(core_os os, int32_t menu, int32_t *ids, int cap) {
    int n = 0;
    core_menu_item it;
    for (int32_t i = 0; i < core_menu_count(os); i++)
        if (core_menu_at(os, i, &it) == CORE_OK && it.menu == menu && n < cap) ids[n++] = it.id;
    return n;
}

static int same(const int32_t *a, int n, const int32_t *b, int m) {
    if (n != m) return 0;
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static void t_order(void) {
    int32_t ids[32];
    int n = children(CORE_OS_WIN, -1, ids, 32);
    const int32_t top[] = {M_APP, M_WIN_EDIT, M_VIEW, M_PREPARE, M_MANAGE, M_HELP};
    CHECK(same(ids, n, top, 6));   /* no Window menu, Edit drawn from the table */
    n = children(CORE_OS_WIN, M_APP, ids, 32);
    const int32_t app[] = {M_APP_UPDATE, M_APP_SETTINGS, M_APP_UNINSTALL, M_APP_SEP1, M_WIN_APP_EXIT};
    CHECK(same(ids, n, app, 5));   /* no About (in Help), no Hide */
    n = children(CORE_OS_WIN, M_WIN_EDIT, ids, 32);
    const int32_t edit[] = {M_WIN_EDIT_CUT, M_WIN_EDIT_COPY, M_WIN_EDIT_PASTE, M_WIN_EDIT_SEP, M_WIN_EDIT_ALL};
    CHECK(same(ids, n, edit, 5));
    n = children(CORE_OS_WIN, M_HELP, ids, 32);
    const int32_t help[] = {M_HELP_HOME, M_HELP_TROUBLESHOOTING, M_HELP_ISSUES, M_HELP_REPORT, M_WIN_HELP_SEP, M_WIN_HELP_ABOUT};
    CHECK(same(ids, n, help, 6));
    /* the Mac's own rows and the Windows rows stay apart */
    n = children(CORE_OS_MAC, -1, ids, 32);
    const int32_t mac_top[] = {M_APP, M_EDIT, M_VIEW, M_PREPARE, M_MANAGE, M_WINDOW, M_HELP};
    CHECK(same(ids, n, mac_top, 7));
    core_menu_item it;
    CHECK(item_at(CORE_OS_MAC, M_WIN_APP_EXIT, &it) < 0 && item_at(CORE_OS_MAC, M_WIN_HELP_ABOUT, &it) < 0);
    CHECK(item_at(CORE_OS_WIN, M_APP_QUIT, &it) < 0 && item_at(CORE_OS_WIN, M_APP_HIDE, &it) < 0 && item_at(CORE_OS_WIN, M_APP_ABOUT, &it) < 0);
    /* the first menu's title has its access key on Windows, the app's name on the Mac */
    CHECK(item_at(CORE_OS_WIN, M_APP, &it) >= 0 && it.title == T_WIN_MENU_APP);
    CHECK(item_at(CORE_OS_MAC, M_APP, &it) >= 0 && it.title == T_APP_NAME);
}

static void t_keys(void) {
    core_menu_item it;
    CHECK(item_at(CORE_OS_WIN, M_WIN_APP_EXIT, &it) >= 0 && it.kind == CORE_MENU_NATIVE && !strcmp(it.key, "F4") && it.mods == CORE_MOD_ALT && !*it.event);
    CHECK(item_at(CORE_OS_WIN, M_WIN_EDIT_CUT, &it) >= 0 && it.kind == CORE_MENU_NATIVE && !strcmp(it.key, "x") && it.mods == CORE_MOD_CMD_OR_CTRL);
    CHECK(item_at(CORE_OS_WIN, M_WIN_EDIT_ALL, &it) >= 0 && !strcmp(it.key, "a") && it.mods == CORE_MOD_CMD_OR_CTRL);
    CHECK(item_at(CORE_OS_WIN, M_HELP_HOME, &it) >= 0 && !strcmp(it.key, "F1") && it.mods == 0);
    CHECK(item_at(CORE_OS_WIN, M_WIN_HELP_ABOUT, &it) >= 0 && it.kind == CORE_MENU_NATIVE && !*it.key);
    CHECK(item_at(CORE_OS_WIN, M_PREP_STOP, &it) >= 0 && !strcmp(it.key, ".") && it.mods == CORE_MOD_CMD_OR_CTRL);
    /* no shortcut twice */
    int clash = 0;
    core_menu_item a, b;
    for (int32_t i = 0; i < core_menu_count(CORE_OS_WIN); i++)
        for (int32_t j = i + 1; j < core_menu_count(CORE_OS_WIN); j++)
            if (core_menu_at(CORE_OS_WIN, i, &a) == CORE_OK && core_menu_at(CORE_OS_WIN, j, &b) == CORE_OK && *a.key && !strcmp(a.key, b.key) && a.mods == b.mods) {
                clash = 1;
                printf("  the same shortcut twice: %d and %d\n", a.id, b.id);
            }
    CHECK(!clash);
}

/* one "_" before a letter in each Windows title, its letter unique in its menu; the words without
 * it are the Mac's, but Exit's */
static void t_access_keys(void) {
    int ok = 1;
    core_menu_item it;
    for (int32_t i = 0; i < core_menu_count(CORE_OS_WIN); i++) {
        if (core_menu_at(CORE_OS_WIN, i, &it) != CORE_OK || it.kind == CORE_MENU_SEPARATOR || it.title < 0) continue;
        const char *w = core_text_template(CORE_OS_WIN, it.title), *u = strchr(w, '_');
        if (!u || strchr(u + 1, '_') || !isalpha((unsigned char)u[1])) { ok = 0; printf("  no single access key: %s\n", w); continue; }
        char plain[128];
        snprintf(plain, sizeof plain, "%.*s%s", (int)(u - w), w, u + 1);
        const char *mac = core_text_template(CORE_OS_MAC, it.title);
        if (*mac && strcmp(plain, mac) && it.title != T_MENU_QUIT) { ok = 0; printf("  other words on Windows: %s / %s\n", w, mac); }
        if (it.title == T_WIN_MENU_APP && strcmp(plain, core_text_template(CORE_OS_WIN, T_APP_NAME))) ok = 0;
        core_menu_item o;
        for (int32_t j = 0; j < i; j++) {   /* an earlier item of the same menu with the same letter */
            if (core_menu_at(CORE_OS_WIN, j, &o) != CORE_OK || o.menu != it.menu || o.kind == CORE_MENU_SEPARATOR || o.title < 0) continue;
            const char *ou = strchr(core_text_template(CORE_OS_WIN, o.title), '_');
            if (ou && tolower((unsigned char)ou[1]) == tolower((unsigned char)u[1])) { ok = 0; printf("  the same access key twice: %s, %s\n", w, core_text_template(CORE_OS_WIN, o.title)); }
        }
    }
    CHECK(ok);
    char b[64];
    size_t len;
    CHECK(core_text(CORE_OS_WIN, T_MENU_QUIT, NULL, 0, b, sizeof b, &len) == CORE_OK && !strcmp(b, "E_xit"));
    CHECK(core_text(CORE_OS_MAC, T_MENU_QUIT, NULL, 0, b, sizeof b, &len) == CORE_OK && !strcmp(b, "Quit RB Stems Plus"));
}

/* a host that only lets a core exist: menu states are read on main, nothing else runs */
static void h_render(void *x, core_snap *s, const core_delta *d) { (void)x; (void)d; core_snap_release(s); }
static uint32_t h_front(void *x) { (void)x; return 0; }
static int32_t h_modal(void *x) { (void)x; return 0; }
static int g_plus;   /* h_facts reports Demucs v4 installed (review WR2: start would pick Status) */
static void h_facts(void *x, core_facts *o) { (void)x; if (g_plus) core_fact_flag(o, FACT_STEMS_PLUS_PRESENT, 1); }
static void h_post(void *x, core_job *j) { (void)x; core_job_drop(j); }
static void h_work(void *x, core_work k, core_job *j) { (void)x; (void)k; core_job_drop(j); }
static int32_t h_dialog(void *x, const core_dialog *d, core_reply *r) { (void)x; (void)d; (void)r; return 1; }
static void h_close(void *x, core_close w, int32_t p) { (void)x; (void)w; (void)p; }
static void h_log(void *x, const char *l, size_t n) { (void)x; (void)l; (void)n; }

static core *new_core(const char *config) {
    core_host h;
    memset(&h, 0, sizeof h);
    h.size = sizeof h;
    h.render = h_render;
    h.front_facts = h_front;
    h.modal_open = h_modal;
    h.facts = h_facts;
    h.post_main = h_post;
    h.post_work = h_work;
    h.dialog = h_dialog;
    h.close_reply = h_close;
    h.log = h_log;
    return core_new(&h, CORE_ROLE_APP, config, CORE_NTS);
}

/* Status and Prepare are shown with the release shell's explicit "prepare_tabs":true and hidden
 * when the key is absent or false. Manage's rows are always shown. */
static void t_without_prepare(void) {
    static const int32_t gone[] = {M_VIEW_STATUS, M_VIEW_PREPARE, M_PREPARE, M_PREP_PLAYLIST, M_PREP_CHECK, M_PREP_PAUSE, M_PREP_STOP};
    static const char *const configs[] = {"{\"os\":\"win\"}", "{\"os\":\"win\",\"prepare_tabs\":false}", "{\"os\":\"win\",\"prepare_tabs\":true}"};
    for (int k = 0; k < 3; k++) {
        const char *config = configs[k];
        int shown = k == 2;
        core *c = new_core(config);
        CHECKS(c != NULL, config);
        if (!c) continue;
        c->preparing = 1;   /* even a run (there can't be one) shows nothing */
        int hidden = 0;
        for (size_t i = 0; i < sizeof gone / sizeof *gone; i++) hidden += (core_menu_state(c, (core_menu_id)gone[i], NULL, NULL) & CORE_MENU_HIDDEN) != 0;
        CHECKS(hidden == (shown ? 0 : (int)(sizeof gone / sizeof *gone)), config);
        CHECKS(!(core_menu_state(c, M_PREP_STOP, NULL, NULL) & CORE_MENU_ENABLED) == !shown, config);   /* its Ctrl+. does nothing */
        CHECKS(!(core_menu_state(c, M_VIEW_MANAGE, NULL, NULL) & CORE_MENU_HIDDEN) && (core_menu_state(c, M_VIEW_MANAGE, NULL, NULL) & CORE_MENU_ENABLED), config);
        CHECKS(!(core_menu_state(c, M_MANAGE_SHOW_LOG, NULL, NULL) & CORE_MENU_HIDDEN), config);
        c->preparing = 0;
        core_free(c);
    }
    /* review WR2/WR3: with a feature installed, start picks Status only when the tabs are shown;
     * without them no menu choice or click reaches the Status or Prepare tab (prepare_playlist's
     * focus included); with them, the same events still switch */
    g_plus = 1;
    static const char *const evs[] = {
        "{\"ev\":\"menu\",\"id\":\"tab_status\"}", "{\"ev\":\"menu\",\"id\":\"tab_prepare\"}", "{\"ev\":\"menu\",\"id\":\"prepare_playlist\"}",
        "{\"ev\":\"click\",\"id\":\"tab_status\"}", "{\"ev\":\"click\",\"id\":\"tab_prepare\"}", "{\"ev\":\"click\",\"id\":\"prepare_playlist\"}",
    };
    for (int k = 0; k < 2; k++) {
        const char *config = k ? configs[2] : configs[0];
        core *t = new_core(config);
        CHECKS(t != NULL, config);
        if (!t) continue;
        CHECKS(core_event(t, "{\"ev\":\"start\"}", CORE_NTS) == CORE_OK, config);
        CHECKS(model_get_i64(&t->m, F_TAB) == (k ? CORE_TAB_STATUS : CORE_TAB_MANAGE), config);
        for (size_t i = 0; i < sizeof evs / sizeof *evs; i++) {
            CHECKS(core_event(t, "{\"ev\":\"menu\",\"id\":\"tab_manage\"}", CORE_NTS) == CORE_OK && model_get_i64(&t->m, F_TAB) == CORE_TAB_MANAGE, evs[i]);
            int64_t focus = model_get_i64(&t->m, F_PREP_FOCUS_SEQ);
            CHECKS(core_event(t, evs[i], CORE_NTS) == CORE_OK, evs[i]);
            int moved = model_get_i64(&t->m, F_TAB) != CORE_TAB_MANAGE;
            CHECKS(moved == k, evs[i]);
            if (!k) CHECKS(model_get_i64(&t->m, F_PREP_FOCUS_SEQ) == focus, evs[i]);
        }
        core_free(t);
    }
    g_plus = 0;
    /* Pause/Resume's title, pulled, in Windows words (shown: a hidden row keeps its first title) */
    core *c = new_core(configs[2]);
    const char *t;
    size_t tl;
    core_menu_state(c, M_PREP_PAUSE, &t, &tl);
    CHECK(!strcmp(t, "Pa_use"));
    c->preparing = c->paused = 1;
    core_menu_state(c, M_PREP_PAUSE, &t, &tl);
    CHECK(!strcmp(t, "_Resume") && tl == 7);
    c->preparing = c->paused = 0;
    /* C15: every Windows item's pulled title is its enumerated one (the first menu's access key too) */
    int same_titles = 1;
    core_menu_item it;
    for (int32_t i = 0; i < core_menu_count(CORE_OS_WIN); i++) {
        if (core_menu_at(CORE_OS_WIN, i, &it) != CORE_OK || it.kind == CORE_MENU_SEPARATOR || it.title < 0) continue;
        char want[128];
        size_t wl;
        core_menu_state(c, (core_menu_id)it.id, &t, &tl);
        if (core_text(CORE_OS_WIN, (core_text_id)it.title, NULL, 0, want, sizeof want, &wl) != CORE_OK || strcmp(t, want)) {
            same_titles = 0;
            printf("  pulled %s, enumerated %s\n", t, want);
        }
    }
    CHECK(same_titles);
    core_menu_state(c, M_APP, &t, &tl);
    CHECK(!strcmp(t, "_RB Stems Plus"));
    core_free(c);
}

void win_menus_tests(int *passed, int *failed) {
    g_passed = passed;
    g_failed = failed;
    t_order();
    t_keys();
    t_access_keys();
    t_without_prepare();
}

#else

void win_menus_tests(int *passed, int *failed) {
    g_passed = passed;
    g_failed = failed;
    CHECK(core_menu_count(CORE_OS_WIN) == 0 && core_menu_count(CORE_OS_MAC) == CORE_MENU_COUNT);   /* no Windows row here */
}

#endif
