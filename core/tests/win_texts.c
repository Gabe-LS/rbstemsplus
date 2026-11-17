/* The Windows column of texts.def (W-core, app.md §9; mac-core §3.9), under CORE_WIN_POLICY:
 * every text Windows can show is written; "" only for the Mac-only paths listed here; each
 * Windows text takes exactly its Mac text's slots (one composer serves both); no Mac word and no
 * tip phrase in a Windows text (the dialogs rule, app.md §2.2); the composers' Windows variants.
 * In the Mac build (as shipped) the Windows column isn't there at all (unit.c checks core_text's
 * answer). Linked into core-test (unit.c's main). */
#include "../internal.h"
#include "win_texts.h"
#include <stdio.h>

static int *g_passed, *g_failed;
#define CHECK(cond) do { if (cond) (*g_passed)++; else { (*g_failed)++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKS(cond, ctx) do { if (cond) (*g_passed)++; else { (*g_failed)++; printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond, (ctx)); } } while (0)

#ifdef CORE_WIN_POLICY

/* Windows never shows these (texts.def's header): "" in the Windows column, and nothing else is */
static const core_text_id mac_only[] = {
    T_PW_TITLE, T_PW_WRONG_TITLE, T_PW_MSG, T_PW_WRONG_MSG, T_PW_FIELD,   /* the password sheet */
    T_PERM_TITLE, T_PERM_MSG, T_TRYING_AGAIN,                             /* App Management's re-sign loop */
    T_REFUSE_TIME_MACHINE, T_PREP_NEEDS_ARM, T_PREP_ASKING,   /* never composed on Windows: the host contract (pure.h, core.h) */
    T_FATE_OTHER_ACCOUNT, T_UPDATE_R_SIGN,
    T_FATE_OTHERS_PASTE,                                                  /* other accounts keep their app on Windows (C17) */
    T_MENU_HIDE, T_MENU_WINDOW, T_MENU_CLOSE, T_MENU_MINIMIZE,            /* the Mac's own menus */
};

static int is_mac_only(int id) {
    for (size_t i = 0; i < sizeof mac_only / sizeof *mac_only; i++)
        if ((int)mac_only[i] == id) return 1;
    return 0;
}

/* the slots a template uses, as a bit set */
static uint64_t slots_of(const char *t) {
    uint64_t u = 0;
    for (const char *p = t; (p = strchr(p, '{')); p++) {
        const char *e = strchr(p, '}');
        for (int k = 0; e && k < CORE_SLOT_COUNT; k++)
            if (strlen(core_slot_names[k]) == (size_t)(e - p - 1) && !memcmp(core_slot_names[k], p + 1, (size_t)(e - p - 1))) u |= 1ull << k;
    }
    return u;
}

/* the word w in t, not inside a longer word ("Mac" in "Macs" is found; "Machine" isn't) */
static int has_word(const char *t, const char *w) {
    size_t n = strlen(w);
    for (const char *p = t; (p = strstr(p, w)); p += n) {
        int before = p > t && ((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z'));
        char a = p[n];
        int after = (a >= 'a' && a <= 'z' && !(a == 's' && !((p[n + 1] >= 'a' && p[n + 1] <= 'z')))) || (a >= 'A' && a <= 'Z');
        if (!before && !after) return 1;
    }
    return 0;
}

static void t_column(void) {
    int unwritten = 0, mac_only_seen = 0, win_rows = 0, slots_ok = 1, format_ok = 1;
    for (int id = 0; id < CORE_TEXT_COUNT; id++) {
        const char *mac = core_text_template(CORE_OS_MAC, id), *win = core_text_template(CORE_OS_WIN, id);
        char ctx[32];
        snprintf(ctx, sizeof ctx, "text %d", id);
        CHECKS(mac && win, ctx);
        if (!mac || !win) continue;
        if (!*mac) {   /* a Windows-only row */
            win_rows++;
            CHECKS(*win, ctx);
        } else if (is_mac_only(id)) {
            mac_only_seen++;
            CHECKS(!*win, ctx);
        } else {
            if (!*win) { unwritten++; printf("  not written for Windows: text %d (%s)\n", id, mac); }
            /* the exceptions: the staging text names no file on Windows (its composer's variant), and
             * the three naming rekordbox's folder take {dir} there (w_rb_dir_slot: Windows' own
             * Program Files, on whichever drive) */
            int rb_dir = id == T_PREP_NO_REKORDBOX || id == T_ROOT_MISSING_MSG || id == T_ROOT_FOLDER_CLEANUP_MSG;
            if (rb_dir) CHECKS(slots_of(win) == (slots_of(mac) | 1ull << CORE_SLOT_dir) && !strstr(win, "Program Files"), ctx);
            else if (*win && slots_of(win) != slots_of(mac) && id != T_UPDATE_R_STAGING) { slots_ok = 0; printf("  other slots on Windows: %s\n", win); }
        }
        if (!*win) continue;
        /* it formats with exactly its own slots */
        core_slot s[CORE_SLOT_COUNT];
        int n = 0;
        uint64_t u = slots_of(win);
        for (int k = 0; k < CORE_SLOT_COUNT; k++)
            if (u >> k & 1) s[n++] = (core_slot){k, 0, "<v>", CORE_NTS};
        char b[2048];
        size_t len;
        if (core_text(CORE_OS_WIN, (core_text_id)id, s, n, b, sizeof b, &len) != CORE_OK || strlen(b) != len) {
            format_ok = 0;
            printf("  doesn't format on Windows: %s\n", win);
        }
    }
    CHECK(unwritten == 0);
    CHECK(mac_only_seen == (int)(sizeof mac_only / sizeof *mac_only));
    CHECK(win_rows >= 17);
    CHECK(slots_ok && format_ok);
    CHECK(core_text_check_all() == 0);
}

/* no Mac word, no tip phrase (app.md §2.2: no UAC, SmartScreen or "keep the drive connected"
 * tips): the one text naming User Account Control is the refusal of a PC where it is off */
static void t_words(void) {
    static const char *const mac_words[] = {"Mac", "macOS", "Finder", "Trash", "System Settings", "Disk Utility", "Applications",
        "Time Machine", "APFS", "Mac OS Extended", "Login Items", "Dock", "password", "Quit", "quit", "Library", "Apple"};
    static const char *const tips[] = {"SmartScreen", "keep the drive connected", "Keep the drive connected", "UAC"};
    int clean = 1;
    for (int id = 0; id < CORE_TEXT_COUNT; id++) {
        const char *w = core_text_template(CORE_OS_WIN, id);
        if (!w || !*w) continue;
        for (size_t i = 0; i < sizeof mac_words / sizeof *mac_words; i++)
            if (has_word(w, mac_words[i])) { clean = 0; printf("  a Mac word (%s) on Windows: %s\n", mac_words[i], w); }
        for (size_t i = 0; i < sizeof tips / sizeof *tips; i++)
            if (strstr(w, tips[i])) { clean = 0; printf("  a tip (%s) on Windows: %s\n", tips[i], w); }
        if (strstr(w, "User Account Control") && id != T_WIN_START_UAC_OFF) { clean = 0; printf("  UAC named: %s\n", w); }
        static const char *const paths[] = {"~/", "/Users", "/Volumes", "/Applications", "/Library", ".app"};
        for (size_t i = 0; i < sizeof paths / sizeof *paths; i++)
            if (strstr(w, paths[i])) { clean = 0; printf("  a Mac path (%s) on Windows: %s\n", paths[i], w); }
    }
    CHECK(clean);
    CHECK(!has_word("Machine", "Mac") && has_word("other Macs or", "Mac") && has_word("this Mac.", "Mac"));
}

#define TBW(name, cap) char name##_buf[cap]; tbuf name; tb_init(&name, CORE_OS_WIN, name##_buf, sizeof name##_buf)

/* the composers, in Windows words */
static void t_composers(void) {
    TBW(b, 2048);
    w_update_failed_message(&b, UPDATE_R_STAGING);
    CHECK(!b.err && !strcmp(b.p, "RB Stems Plus wasn't changed. Something else is in the way of the new version's folder. Try again later."));
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    w_update_failed_message(&b, UPDATE_R_SIGN);   /* no re-sign on Windows: the plain failure */
    CHECK(!b.err && !strcmp(b.p, "RB Stems Plus wasn't changed. Try again later."));
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    w_update_failed_message(&b, update_failure_reason("failed swap", 11));
    CHECK(!b.err && !strcmp(b.p, "RB Stems Plus wasn't changed. The new version couldn't take this one's place. Try again later."));
    tb_init(&b, CORE_OS_MAC, b_buf, sizeof b_buf);   /* the Mac's words unchanged */
    w_update_failed_message(&b, UPDATE_R_STAGING);
    CHECK(!b.err && strstr(b.p, "(.RB Stems Plus.update.app)") && strstr(b.p, "If macOS said"));
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    w_location_refusal(&b, REFUSE_FILE_SYSTEM, "USB");
    CHECK(!b.err && !strcmp(b.p, "\"USB\" isn't formatted as NTFS, exFAT or FAT32."));
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    w_location_refusal(&b, REFUSE_INTERNAL, NULL);
    CHECK(!b.err && !strcmp(b.p, "Choose a folder on an external drive. To save stems on this PC, use This PC."));
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    w_uninstall_all_message(&b, 1, 0, " (about 1.2 GB)", "GLS", 1);
    CHECK(!b.err && !strcmp(b.p, "rekordbox gets its own stems model back. Stems Cache was installed from another account on this PC. Your music and "
                                  "rekordbox library aren't touched.\n\nRB Stems Plus will be deleted, including all stems saved in its folder on \"GLS\" "
                                  "(about 1.2 GB), also any saved there by other computers or accounts."));
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    const char *left[] = {"The stems saved on \"GLS\" stay: it isn't connected. To delete them, delete the folder \"E:\\RB Stems Plus Cache\" in File Explorer."};
    w_own_files_removed(&b, "rekordbox is back to how it was.", FATE_CANT_DELETE, 0, 1, left, 1);
    CHECK(!b.err && !strcmp(b.p, "rekordbox is back to how it was. RB Stems Plus couldn't delete all of its files: delete the folder "
                                  "%LOCALAPPDATA%\\Programs\\RB Stems Plus in File Explorer. %LOCALAPPDATA%\\rbstemsplus\\Cache is a link, so RB Stems "
                                  "Plus leaves it and what it points to alone. The stems saved on \"GLS\" stay: it isn't connected. To delete them, "
                                  "delete the folder \"E:\\RB Stems Plus Cache\" in File Explorer."));
}

/* R1's line: one per state, none when there is nothing to put back */
static void t_r1_line(void) {
    static const struct { int64_t state; const char *line; } cases[] = {
        {R1_OK, ""},
        {R1_LIVE_MISSING, "rekordbox's onnxruntime.dll is missing, so rekordbox can't separate stems. Click Put Back Pioneer's File."},
        {R1_LIVE_FOREIGN, "rekordbox's onnxruntime.dll isn't Pioneer's or RB Stems Plus's, so rekordbox may not separate stems. Click Put Back Pioneer's File."},
        {R1_ORT_MISSING, "Stems Cache's copy of rekordbox's ONNX Runtime is missing, so rekordbox can't separate stems. Click Put Back Pioneer's File."},
        {7, ""},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        TBW(b, 512);
        w_r1_line(&b, cases[i].state);
        CHECKS(!b.err && !strcmp(b.p, cases[i].line), cases[i].line);
    }
    char t[64];
    size_t len;   /* the line's button is the restore flow's: the same words */
    CHECK(core_text(CORE_OS_WIN, T_WIN_BTN_RESTORE, NULL, 0, t, sizeof t, &len) == CORE_OK && strstr(cases[1].line, t));
}

/* C6: the shared composers that reach a text the Mac words differently, composed for Windows */
static void t_shared_composers(void) {
    TBW(b, 4096);
    gate_in g;
    memset(&g, 0, sizeof g);
    g.rekordbox_installed = g.user_is_admin = 1;
    g.own_bundle = 0;   /* not the installed copy */
    w_status_lines(&b, &g);
    CHECK(!b.err && strstr(b.p, "This copy of RB Stems Plus isn't the installed one: open RB Stems Plus from the Start menu."));
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    prep_facts pf;
    memset(&pf, 0, sizeof pf);
    pf.apple_silicon = 1;   /* the Windows host's contract (pure.h) */
    pf.quality = -1;
    pf.programs = "D:\\Program Files";   /* Windows on a second drive */
    CHECK(w_prepare_problems(&b, &pf) > 0 && !b.err && !strstr(b.p, "Apple"));
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    pf.helper_there = 1;
    CHECK(w_prepare_problems(&b, &pf) == 1 && !b.err && !strcmp(b.p, "rekordbox 7 isn't in D:\\Program Files\\rekordbox."));
    /* the texts naming rekordbox's folder: {dir} on Windows only, from the folder given */
    static const core_text_id rb[] = {T_PREP_NO_REKORDBOX, T_ROOT_MISSING_MSG, T_ROOT_FOLDER_CLEANUP_MSG};
    for (size_t i = 0; i < sizeof rb / sizeof *rb; i++) {
        tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
        w_rb_text(&b, rb[i], "C:\\Program Files");
        CHECK(!b.err && strstr(b.p, " isn't in C:\\Program Files\\rekordbox.") && !strstr(b.p, "{"));
        tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
        w_rb_text(&b, rb[i], "E:\\Windows\\Program Files");
        CHECK(!b.err && strstr(b.p, "isn't in E:\\Windows\\Program Files\\rekordbox."));
        char d[64];
        core_slot sl;
        CHECK(w_rb_dir_slot(CORE_OS_MAC, rb[i], "C:\\Program Files", d, sizeof d, &sl) == 0);
    }
    char d[64];
    core_slot sl;
    CHECK(w_rb_dir_slot(CORE_OS_WIN, T_ROOT_MISSING_TITLE, "C:\\Program Files", d, sizeof d, &sl) == 0);
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    w_cache_failure_reason(&b, 1, ROOT_REKORDBOX_MISSING, "D:\\Program Files");
    CHECK(!b.err && !strcmp(b.p, "rekordbox isn't in D:\\Program Files\\rekordbox. Install rekordbox from rekordbox.com, then try again."));
    tb_init(&b, CORE_OS_WIN, b_buf, sizeof b_buf);
    w_own_files_removed(&b, "rekordbox is back to how it was.", FATE_DELETED, 0, 0, NULL, 0);
    CHECK(!b.err && !strcmp(b.p, "rekordbox is back to how it was."));
}

void win_texts_tests(int *passed, int *failed) {
    g_passed = passed;
    g_failed = failed;
    t_shared_composers();
    t_r1_line();
    t_column();
    t_words();
    t_composers();
}

#else

/* the Mac build (as shipped): no Windows row, no Windows column */
void win_texts_tests(int *passed, int *failed) {
    g_passed = passed;
    g_failed = failed;
    char b[64];
    size_t len;
    CHECK(core_text(CORE_OS_WIN, T_APP_NAME, NULL, 0, b, sizeof b, &len) == CORE_E_ARG);
    int empty_mac = 0;
    for (int id = 0; id < CORE_TEXT_COUNT; id++) empty_mac += !*core_text_template(CORE_OS_MAC, id);
    CHECK(empty_mac == 0);   /* every row here is the Mac's */
}

#endif
