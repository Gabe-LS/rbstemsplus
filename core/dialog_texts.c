/* The action dialogs' texts that Swift composes (Actions.swift, LocationActions.swift,
 * Location.swift, Update.swift, Admin.swift): the flows (M6) show them; the order of the dialogs
 * is the flows'. */
#include "internal.h"
#ifdef CORE_WIN_POLICY
#include "native.h"
#endif

#include <stdio.h>

static void sub_text(tbuf *b, char *buf, size_t cap, tbuf *t) { tb_init(t, b->os, buf, cap); }
static void sub_done(tbuf *b, const tbuf *t) { if (t->err) b->err = t->err; }

void w_install_note(tbuf *b, int32_t plus, int32_t cache, int32_t reinstall, int32_t has_paused, int64_t floor, const char *drive) {
    if (reinstall) {
        char fb[128];
        tbuf f;
        sub_text(b, fb, sizeof fb, &f);
        TB_TEXT0(&f, plus && cache ? T_FEATURES_AND : cache ? T_FEATURE_CACHE : T_FEATURE_PLUS);
        sub_done(b, &f);
        core_slot sl = SLOT_N(features, fb, tb_len(&f));
        tb_text(b, T_INSTALL_NOTE_REINSTALL, &sl, 1);
    } else TB_TEXT0(b, plus && cache ? T_INSTALL_NOTE_BOTH : cache ? T_INSTALL_NOTE_CACHE : T_INSTALL_NOTE_PLUS);
    if (cache && has_paused) {
        tb_puts(b, " ");
        w_cache_paused_at_install(b, floor, drive);
    }
}

void w_uninstall_note(tbuf *b, int32_t cache) { TB_TEXT0(b, cache ? T_UNINSTALL_NOTE_CACHE : T_UNINSTALL_NOTE_PLUS); }

#ifdef CORE_WIN_POLICY
static void uninstall_all_message(tbuf *b, int32_t others_cache, int32_t no_saved_model, const char *stems, const char *drive,
                                  int32_t drive_connected, const char *unchecked_dir) {
#else
void w_uninstall_all_message(tbuf *b, int32_t others_cache, int32_t no_saved_model, const char *stems, const char *drive, int32_t drive_connected) {
#endif
    char ab[2048], db[2048];   /* a drive's name: up to 255 UTF-16 units, 765 bytes */
    tbuf about, del;
    sub_text(b, ab, sizeof ab, &about);
    if (!others_cache && !no_saved_model) TB_TEXT0(&about, T_UNINSTALL_ALL_EXACT);
    else {
        TB_TEXT0(&about, no_saved_model ? T_UNINSTALL_ALL_NO_SAVED : T_UNINSTALL_ALL_OWN_MODEL);
        if (others_cache) TB_TEXT0(&about, T_UNINSTALL_ALL_OTHERS);
    }
    sub_text(b, db, sizeof db, &del);
    core_slot sl[2] = {SLOT_S(drive, drive ? drive : ""), SLOT_S(size, stems ? stems : "")};
#ifdef CORE_WIN_POLICY
    if (unchecked_dir) {
        core_slot loc[2] = {sl[0], SLOT_S(dir, unchecked_dir)};
        if (TB_NAMELESS(&del, drive)) tb_text(&del, nameless_text(T_WIN_UNINSTALL_ALL_DRIVE_UNMEASURED), &loc[1], 1);
        else tb_text(&del, T_WIN_UNINSTALL_ALL_DRIVE_UNMEASURED, loc, 2);
    } else
#endif
    if (drive) {
#ifdef CORE_WIN_POLICY
        if (drive_connected && TB_NAMELESS(&del, drive)) tb_text(&del, nameless_text(T_UNINSTALL_ALL_DRIVE), &sl[1], 1);
        else
#endif
        if (drive_connected) tb_text(&del, T_UNINSTALL_ALL_DRIVE, sl, 2);
#ifdef CORE_WIN_POLICY
        else if (TB_NAMELESS(&del, drive)) TB_TEXT0(&del, nameless_text(T_UNINSTALL_ALL_DRIVE_GONE));
#endif
        else tb_text(&del, T_UNINSTALL_ALL_DRIVE_GONE, sl, 1);
    } else tb_text(&del, T_UNINSTALL_ALL_THIS_MAC, &sl[1], 1);
    sub_done(b, &about);
    sub_done(b, &del);
    core_slot m[2] = {SLOT_N(message, ab, tb_len(&about)), SLOT_N(why, db, tb_len(&del))};
    tb_text(b, T_UNINSTALL_ALL_MSG, m, 2);
}

#ifdef CORE_WIN_POLICY
void w_uninstall_all_message(tbuf *b, int32_t others_cache, int32_t no_saved_model, const char *stems, const char *drive, int32_t drive_connected) {
    uninstall_all_message(b, others_cache, no_saved_model, stems, drive, drive_connected, NULL);
}

void w_win_uninstall_all_unmeasured(tbuf *b, int32_t others_cache, int32_t no_saved_model, const char *drive, const char *dir) {
    uninstall_all_message(b, others_cache, no_saved_model, NULL, drive, 0, dir);
}
#endif

void w_own_files_removed(tbuf *b, const char *about, int32_t fate, int32_t others_cache, int32_t cache_link, const char *const *left, int32_t nleft) {
    tb_puts(b, about ? about : "");
#ifdef CORE_WIN_POLICY
    if (b->os == CORE_OS_WIN) others_cache = 0;   /* the app is per account on Windows: other accounts keep theirs (review C17) */
#endif
    if (fate == FATE_DELETED) { if (others_cache) TB_TEXT0(b, T_FATE_OTHERS_PASTE); }
    else TB_TEXT0(b, fate == FATE_OTHER_ACCOUNT ? T_FATE_OTHER_ACCOUNT : T_FATE_CANT_DELETE);
    if (cache_link) { tb_puts(b, " "); TB_TEXT0(b, T_CACHE_LINK); }
    for (int32_t i = 0; i < nleft; i++) { tb_puts(b, " "); tb_puts(b, left[i] ? left[i] : ""); }
}

void w_location_removal_note(tbuf *b, const char *drive, int32_t connected, const char *size) {
    if (!drive) return;
    core_slot sl[2] = {SLOT_S(drive, drive), SLOT_S(size, size ? size : "")};
#ifdef CORE_WIN_POLICY
    if (connected && TB_NAMELESS(b, drive)) tb_text(b, nameless_text(T_REMOVAL_NOTE_CONNECTED), &sl[1], 1);
    else
#endif
    if (connected) tb_text(b, T_REMOVAL_NOTE_CONNECTED, sl, 2);
#ifdef CORE_WIN_POLICY
    else if (TB_NAMELESS(b, drive)) TB_TEXT0(b, nameless_text(T_REMOVAL_NOTE_NOT_CONNECTED));
#endif
    else tb_text(b, T_REMOVAL_NOTE_NOT_CONNECTED, sl, 1);
}

static void old_drive_stays(tbuf *b, const char *old_drive) {
    if (!old_drive) return;
    tb_puts(b, " ");
#ifdef CORE_WIN_POLICY
    if (TB_NAMELESS(b, old_drive)) { TB_TEXT0(b, nameless_text(T_OLD_DRIVE_STAYS)); return; }
#endif
    core_slot sl = SLOT_S(drive, old_drive);
    tb_text(b, T_OLD_DRIVE_STAYS, &sl, 1);
}

void w_switch_message(tbuf *b, const char *drive, int32_t has_found, int64_t found, int64_t this_mac, const char *old_drive) {
    core_slot d = SLOT_S(drive, drive ? drive : "");
    if (has_found && found < 0) tb_text(b, T_SWITCH_FOUND_UNSIZED, &d, 1);
    else if (has_found) {
        char sb[64];
        tbuf s;
        sub_text(b, sb, sizeof sb, &s);
        w_size_words(&s, found);
        sub_done(b, &s);
        core_slot sl[2] = {SLOT_N(size, sb, tb_len(&s)), d};
        tb_text(b, T_SWITCH_FOUND, sl, 2);
    } else tb_text(b, T_SWITCH_NEW, &d, 1);
    if (this_mac < 0) TB_TEXT0(b, T_SWITCH_LOCAL_DELETED_UNSIZED);
    else if (this_mac > 0) {
        char sb[64];
        tbuf s;
        sub_text(b, sb, sizeof sb, &s);
        w_size_words(&s, this_mac);
        sub_done(b, &s);
        core_slot sl = SLOT_N(size, sb, tb_len(&s));
        tb_text(b, T_SWITCH_THIS_MAC_DELETED, &sl, 1);
    }
    old_drive_stays(b, old_drive);
}

void w_use_this_mac_message(tbuf *b, const char *old_drive) {
    TB_TEXT0(b, T_USE_THIS_MAC_MSG);
    old_drive_stays(b, old_drive);
}

void w_clear_location_message(tbuf *b, const char *drive) {
#ifdef CORE_WIN_POLICY
    if (TB_NAMELESS(b, drive)) { TB_TEXT0(b, nameless_text(T_CLEAR_LOCATION_MSG)); return; }
#endif
    core_slot sl = SLOT_S(drive, drive ? drive : "");
    tb_text(b, T_CLEAR_LOCATION_MSG, &sl, 1);
}

void w_location_refusal(tbuf *b, int32_t why, const char *drive) {
    static const core_text_id ids[] = {T_REFUSE_INTERNAL, T_REFUSE_NETWORK, T_REFUSE_READ_ONLY, T_REFUSE_TIME_MACHINE, T_REFUSE_INSIDE,
                                       T_REFUSE_LINK, T_REFUSE_FILE_SYSTEM, T_REFUSE_PATH, T_REFUSE_UNREADABLE};
    if (why < 0 || why >= (int32_t)(sizeof ids / sizeof *ids)) return;
    core_slot sl = SLOT_S(drive, drive ? drive : "");
    int uses = why == REFUSE_READ_ONLY || why == REFUSE_TIME_MACHINE || why == REFUSE_FILE_SYSTEM;
    tb_text(b, ids[why], uses ? &sl : NULL, uses ? 1 : 0);
}

/* Character.isWhitespace at p: Unicode's White_Space, as UTF-8; its length, or 0 */
static size_t white(const unsigned char *p, size_t n) {
    if (p[0] == ' ' || (p[0] >= 0x09 && p[0] <= 0x0D)) return 1;
    if (n >= 2 && p[0] == 0xC2 && (p[1] == 0x85 || p[1] == 0xA0)) return 2;
    if (n >= 3 && p[0] == 0xE1 && p[1] == 0x9A && p[2] == 0x80) return 3;
    if (n >= 3 && p[0] == 0xE2 && p[1] == 0x80 && ((p[2] >= 0x80 && p[2] <= 0x8A) || p[2] == 0xA8 || p[2] == 0xA9 || p[2] == 0xAF)) return 3;
    if (n >= 3 && p[0] == 0xE2 && p[1] == 0x81 && p[2] == 0x9F) return 3;
    if (n >= 3 && p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) return 3;
    return 0;
}

int32_t update_failure_reason(const char *result, size_t n) {
    static const char *const keys[] = {"app", "download", "staging", "copy", "copy-check", "sign", "swap"};
    const unsigned char *p = (const unsigned char *)result, *e = p + (result ? n : 0);
    const unsigned char *w[3];
    size_t wn[3];
    int words = 0;
    while (p < e) {
        size_t k = white(p, (size_t)(e - p));
        if (k) { p += k; continue; }
        const unsigned char *s = p;
        while (p < e && !white(p, (size_t)(e - p))) p++;
        if (words < 3) { w[words] = s; wn[words] = (size_t)(p - s); }
        words++;
    }
    if (words != 2 || wn[0] != 6 || memcmp(w[0], "failed", 6)) return UPDATE_R_NONE;
    for (int i = 0; i < 7; i++)
        if (wn[1] == strlen(keys[i]) && !memcmp(w[1], keys[i], wn[1])) return i + 1;
    return UPDATE_R_NONE;
}

void w_update_failed_message(tbuf *b, int32_t reason) {
    static const core_text_id ids[] = {T_UPDATE_R_APP, T_UPDATE_R_DOWNLOAD, T_UPDATE_R_STAGING, T_UPDATE_R_COPY, T_UPDATE_R_COPY_CHECK,
                                       T_UPDATE_R_SIGN, T_UPDATE_R_SWAP};
    char rb[2048];
    tbuf r;
    sub_text(b, rb, sizeof rb, &r);
#ifdef CORE_WIN_POLICY
    /* Windows (W-core): no re-sign step, and the staging text names no file (the new version's
     * own folder is in the way, update.c) */
    if (b->os == CORE_OS_WIN && reason == UPDATE_R_SIGN) reason = UPDATE_R_NONE;
    if (b->os == CORE_OS_WIN && reason == UPDATE_R_STAGING) TB_TEXT0(&r, T_UPDATE_R_STAGING);
    else
#endif
    if (reason >= 1 && reason <= 7) {
        core_slot sl = SLOT_S(name, ".RB Stems Plus.update.app");   /* Paths.swift updateStagingName */
        tb_text(&r, ids[reason - 1], reason == UPDATE_R_STAGING ? &sl : NULL, reason == UPDATE_R_STAGING ? 1 : 0);
    }
    sub_done(b, &r);
    core_slot sl = SLOT_N(why, rb, tb_len(&r));
    tb_text(b, T_UPDATE_FAILED_MSG, &sl, 1);
}

int32_t root_problem(int32_t code, core_text_id *title, core_text_id *body) {
    switch (code) {
#ifdef CORE_WIN_POLICY
    case ROOT_WIN_START_TIMEOUT: *title = T_WIN_ADMIN_START_FAILED; *body = T_WIN_ADMIN_START_TIMEOUT; return 1;
#endif
    case ROOT_ALREADY_RUNNING: *title = T_ROOT_ALREADY_TITLE; *body = T_ROOT_ALREADY_MSG; return 1;
    case ROOT_INSTALLER_RUNNING: *title = T_ROOT_UPDATING_TITLE; *body = T_ROOT_UPDATING_MSG; return 1;
    case ROOT_REKORDBOX_MISSING: *title = T_ROOT_MISSING_TITLE; *body = T_ROOT_MISSING_MSG; return 1;
    case ROOT_NOT_PIONEERS: *title = T_ROOT_REINSTALL_TITLE; *body = T_ROOT_NOT_PIONEERS_MSG; return 1;
    case ROOT_NO_ORIGINALS: *title = T_ROOT_REINSTALL_TITLE; *body = T_ROOT_NO_ORIGINALS_MSG; return 1;
    case ROOT_ROLLBACK_FAILED: *title = T_ROOT_REINSTALL_TITLE; *body = T_ROOT_ROLLBACK_FAILED_MSG; return 1;
    default: return 0;
    }
}

void root_run_texts(const char *name, core_text_id *status, core_text_id *failed_title) {
#ifdef CORE_WIN_POLICY
    if (name && !strcmp(name, "restore")) { *status = T_WIN_RESTORING; *failed_title = T_WIN_RESTORE_FAILED; return; }   /* R1 (W-core) */
#endif
    if (name && !strcmp(name, "install")) { *status = T_RUN_INSTALLING_CACHE; *failed_title = T_INSTALL_FAILED; }
    else if (name && !strcmp(name, "reinstall")) { *status = T_RUN_REINSTALLING_CACHE; *failed_title = T_REINSTALL_FAILED; }
    else if (name && !strcmp(name, "remove-root-folder")) { *status = T_RUN_REMOVING_ROOT; *failed_title = T_UNINSTALL_FAILED; }
    else { *status = T_RUN_UNINSTALLING_CACHE; *failed_title = T_UNINSTALL_FAILED; }
}

int32_t w_rb_dir_slot(core_os os, core_text_id id, const char *programs, char *buf, size_t cap, core_slot *sl) {
    if (os != CORE_OS_WIN || (id != T_PREP_NO_REKORDBOX && id != T_ROOT_MISSING_MSG && id != T_ROOT_FOLDER_CLEANUP_MSG)) return 0;
    snprintf(buf, cap, "%s\\rekordbox", programs ? programs : "");
    *sl = SLOT_S(dir, buf);
    return 1;
}

void w_rb_text(tbuf *b, core_text_id id, const char *programs) {
    char d[CORE_WIN_PROGRAMS_MAX + 16];
    core_slot sl;
    int32_t k = w_rb_dir_slot(b->os, id, programs, d, sizeof d, &sl);
    tb_text(b, id, k ? &sl : NULL, k);
}

void w_cache_failure_reason(tbuf *b, int32_t failed, int32_t code, const char *programs) {
    core_text_id title, body;
    if (failed && root_problem(code, &title, &body)) { w_rb_text(b, body, programs); return; }
    TB_TEXT0(b, failed && code == ROOT_ROLLED_BACK ? T_ROOT_ROLLED_BACK : T_SEE_LOG);
}

#ifdef CORE_WIN_POLICY
void w_win_batch_why(tbuf *b, int code, const char *programs) {
    if (code == ROOT_REKORDBOX_OPEN) { TB_TEXT0(b, T_RB_OPEN_MSG); return; }
    w_cache_failure_reason(b, 1, code, programs);
}

/* R1's line (W-core, app.md §5.2): what is wrong with rekordbox's ONNX Runtime and the button that
 * puts Pioneer's file back; nothing when there is nothing to put back */
void w_r1_line(tbuf *b, int64_t state) {
    if (state == R1_LIVE_MISSING) TB_TEXT0(b, T_WIN_R1_LIVE_MISSING);
    else if (state == R1_LIVE_FOREIGN) TB_TEXT0(b, T_WIN_R1_LIVE_FOREIGN);
    else if (state == R1_ORT_MISSING) TB_TEXT0(b, T_WIN_R1_ORT_MISSING);
}
#endif
