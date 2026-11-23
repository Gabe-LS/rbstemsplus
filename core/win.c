/* win.c: the core's Windows-only pure decisions (W-core, app.md §9). Compiled into the Windows
 * builds and the host test builds, never into the Mac archive: core/build.sh lib builds $SRCS
 * only, and this file is in $WIN_SRCS. Under CORE_WIN_POLICY only, so even a Mac test build
 * compiles nothing of it. */
#include "internal.h"
#include <stdio.h>

#ifdef CORE_WIN_POLICY

/* The Windows in-front policy (mac-core §3.6, app.md §4.1, §4.3), from front_facts' raw bits read
 * live at the decision: the Mac's inFront() (PrepareView.swift:293: the tab's view loaded, the
 * window not minimized, the app active) with WPF's facts: "active" is an activated app, as the
 * activation aggregator sees it (Application.Activated: any of our windows, dialogs included);
 * the tab asked about is the one the window shows now (the first release creates only Manage, so
 * WINDOW_LOADED is the tab's content); and nothing is in front while a UAC elevation is in flight
 * (CORE_FRONT_ELEVATING: the secure desktop is up or the worker's exit isn't handled yet). Windows
 * has no privacy prompt to keep off rekordbox, but the same rule keeps a playlist's check from
 * reading a drive while the DJ is in rekordbox. held: the held click's check (PrepareView.swift:670)
 * also needs no dialog up (modal_open). 1 in front, 0 not. */
int win_in_front(uint32_t front, int32_t modal, int32_t tab, int held) {
    if (!(front & CORE_FRONT_ACTIVE) || !(front & CORE_FRONT_WINDOW_LOADED)) return 0;
    if (front & (CORE_FRONT_MINIATURIZED | CORE_FRONT_ELEVATING)) return 0;
    if ((int32_t)((front & CORE_FRONT_TAB_MASK) >> CORE_FRONT_TAB_SHIFT) != tab) return 0;
    return !(held && modal);
}

/* R1's fields (app.md §5.2, W-core's schema rows), for M4's refresh to call with CORE_SLOW_R1's
 * FACT_R1_STATE and FACT_R1_DISMISSED: the line, and the button shown when there is something to
 * put back, enabled while no action runs; beside it "Don't Ask Again for This Version" (N10), the
 * same while this version isn't dismissed (the line and Put Back stay once it is: the watcher only
 * stops opening the app for it, as the Mac's Don't Ask Again stops only the watcher's question).
 * 0, or the first model error. */
int win_r1_fields(model *m, int64_t r1_state, int dismissed, int action_running) {
    char line[512];
    tbuf b;
    tb_init(&b, CORE_OS_WIN, line, sizeof line);
    w_r1_line(&b, r1_state);
    size_t n = tb_len(&b);
    int show = n > 0, ask = show && !dismissed, r;
    if ((r = model_set_str(m, F_WIN_R1_LINE, line, n))) return r;
    if ((r = model_set_i64(m, F_WIN_RESTORE_VISIBLE, show))) return r;
    if ((r = model_set_i64(m, F_WIN_RESTORE_ENABLED, show && !action_running))) return r;
    if ((r = model_set_i64(m, F_WIN_R1_DISMISS_VISIBLE, ask))) return r;
    return model_set_i64(m, F_WIN_R1_DISMISS_ENABLED, ask && !action_running);
}

/* Folder versions contain exactly three numeric components. A display version belongs to
 * the registry entry, so unverified rows may carry a different build string. */
int win_version_ok(const char *s, size_t n) {
    int dots = 0, digits = 0;
    if (n >= WIN_VERSION_CAP) return 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] >= '0' && s[i] <= '9') digits = 1;
        else if (s[i] == '.' && digits) { dots++; digits = 0; }
        else return 0;
    }
    return dots == 2 && digits;
}

static int row_number(const char *s, size_t n, int max) {
    if (!n || n > 2 || (n > 1 && s[0] == '0')) return -1;
    int value = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        value = value * 10 + s[i] - '0';
    }
    return value <= max ? value : -1;
}

/* -1 rejects the whole sheet. The caller uses no rows from a rejected sheet. */
int win_rows_parse(const char *s, size_t len, win_row *out, int max) {
    int n = 0;
    while (len) {
        const char *end = memchr(s, '\n', len);
        if (!end || n >= max) return -1;
        const char *field[7];
        size_t bytes[7];
        for (int i = 0; i < 7; i++) {
            const char *stop = i == 6 ? end : memchr(s, '\t', (size_t)(end - s));
            if (!stop) return -1;
            field[i] = s;
            bytes[i] = (size_t)(stop - s);
            s = stop + 1;
        }
        if (!win_version_ok(field[0], bytes[0]) || bytes[6] >= WIN_VERSION_CAP ||
            !utf8_valid(field[6], bytes[6])) return -1;
        for (size_t i = 0; i < bytes[6]; i++)
            if ((unsigned char)field[6][i] < ' ' || field[6][i] == 127) return -1;
        win_row r = {0};
        memcpy(r.version, field[0], bytes[0]);
        memcpy(r.display_version, field[6], bytes[6]);
        for (int i = 0; i < n; i++)
            if (!strcmp(out[i].version, r.version)) return -1;
        r.state = row_number(field[1], bytes[1], WIN_ROW_UNVERIFIED);
        r.why = row_number(field[2], bytes[2], WIN_WHY_NOT_RECORDED);
        r.running = row_number(field[3], bytes[3], 1);
        r.r1 = row_number(field[4], bytes[4], R1_ORT_MISSING);
        r.r1_dismissed = row_number(field[5], bytes[5], 1);
        if (r.state < 0 || r.why < 0 || r.running < 0 || r.r1 < 0 || r.r1_dismissed < 0) return -1;
        out[n++] = r;
        len -= (size_t)(s - field[0]);
    }
    return n;
}

void w_win_version_list(tbuf *b, const char *const *versions, int n) {
    for (int i = 0; i < n; i++) {
        if (i) tb_puts(b, i == n - 1 ? " and " : ", ");
        tb_puts(b, versions[i]);
    }
}

void w_win_row_text(tbuf *b, const win_row *r, int full_build) {
    static const core_text_id state[] = {
        T_WIN_ROW_ON, T_WIN_ROW_READY, T_WIN_ROW_UNSUPPORTED, T_WIN_ROW_REPAIR, T_WIN_ROW_UNVERIFIED
    };
    core_slot version = SLOT_S(rekordbox, full_build && r->display_version[0] ? r->display_version : r->version);
    tb_text(b, state[r->state], &version, 1);
    if (r->running) TB_TEXT0(b, T_WIN_ROW_RUNNING);
}

void w_win_row_reason(tbuf *b, const win_row *r) {
    static const core_text_id why[] = {
        T_WIN_ROW_READY_TIP, T_CACHE_RB_UNLISTED, T_WIN_ROW_BUILD_UNLISTED, T_WIN_ROW_ORT_UNLISTED,
        T_CACHE_ORT_NOT_ORIGINAL, T_WIN_ROW_R1_MISSING, T_WIN_ROW_R1_FOREIGN, T_WIN_ROW_R1_ORT_MISSING,
        T_WIN_ROW_DELISTED, T_WIN_ROW_NOT_VERIFIED, T_WIN_ROW_NO_ANSWER, T_WIN_ROW_NOT_RECORDED
    };
    if (r->state == WIN_ROW_ON) return;
    core_slot version = SLOT_S(rekordbox, r->version);
    tb_text(b, why[r->why], &version, 1);
}

void w_win_cache_summary(tbuf *b, int on, int total) {
    char n[16], m[16];
    snprintf(n, sizeof n, "%d", on);
    snprintf(m, sizeof m, "%d", total);
    core_slot slots[] = {SLOT_S(n, n), SLOT_S(m, m)};
    tb_text(b, T_WIN_CACHE_SUMMARY, slots, 2);
}

#else
typedef int core_win_c_is_windows_only;   /* ISO C: a translation unit isn't empty */
#endif
