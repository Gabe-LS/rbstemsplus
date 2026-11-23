/* The Windows in-front policy (core/win.c, W-core; mac-core §3.6, app.md §4.3), under
 * CORE_WIN_POLICY: every combination of front_facts' bits, the tab shown, a dialog up and the held
 * click's check. In the Mac build (as shipped) win.c has no code. Linked into core-test. */
#include "../internal.h"
#include "win_texts.h"
#include <stdio.h>

static int *g_passed, *g_failed;
#define CHECK(cond) do { if (cond) (*g_passed)++; else { (*g_failed)++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#ifdef CORE_WIN_POLICY

static uint32_t tab_bits(int32_t tab) { return (uint32_t)tab << CORE_FRONT_TAB_SHIFT; }

static const char *field_str(const model *m, core_field f) {
    const field_meta *fm = &core_field_meta[f];
    const cell *x = &m->sec[fm->section]->cells[fm->index];
    return x->len ? x->v.s : "";
}

/* R1's schema fields (W-core's rows): the line, the button shown and, with no action, enabled;
 * Don't Ask Again for This Version (N10) the same while the version isn't dismissed */
static void t_r1_fields(void) {
    model m;
    CHECK(model_init(&m) == CORE_OK);
    CHECK(core_field_meta[F_WIN_R1_LINE].section == S_APP && core_field_meta[F_WIN_RESTORE_VISIBLE].section == S_APP);
    CHECK(win_r1_fields(&m, R1_LIVE_MISSING, 0, 0) == CORE_OK);
    CHECK(!strncmp(field_str(&m, F_WIN_R1_LINE), "rekordbox's onnxruntime.dll is missing", 38));
    CHECK(model_get_i64(&m, F_WIN_RESTORE_VISIBLE) == 1 && model_get_i64(&m, F_WIN_RESTORE_ENABLED) == 1);
    CHECK(model_get_i64(&m, F_WIN_R1_DISMISS_VISIBLE) == 1 && model_get_i64(&m, F_WIN_R1_DISMISS_ENABLED) == 1);
    CHECK(win_r1_fields(&m, R1_ORT_MISSING, 0, 1) == CORE_OK);   /* an action runs: shown, off */
    CHECK(!strncmp(field_str(&m, F_WIN_R1_LINE), "Stems Cache's copy", 18));
    CHECK(model_get_i64(&m, F_WIN_RESTORE_VISIBLE) == 1 && model_get_i64(&m, F_WIN_RESTORE_ENABLED) == 0);
    CHECK(model_get_i64(&m, F_WIN_R1_DISMISS_VISIBLE) == 1 && model_get_i64(&m, F_WIN_R1_DISMISS_ENABLED) == 0);
    CHECK(win_r1_fields(&m, R1_LIVE_FOREIGN, 1, 0) == CORE_OK);   /* dismissed: the line and Put Back stay */
    CHECK(model_get_i64(&m, F_WIN_RESTORE_VISIBLE) == 1 && model_get_i64(&m, F_WIN_RESTORE_ENABLED) == 1 && *field_str(&m, F_WIN_R1_LINE));
    CHECK(model_get_i64(&m, F_WIN_R1_DISMISS_VISIBLE) == 0 && model_get_i64(&m, F_WIN_R1_DISMISS_ENABLED) == 0);
    CHECK(win_r1_fields(&m, R1_OK, 0, 0) == CORE_OK);
    CHECK(!*field_str(&m, F_WIN_R1_LINE) && model_get_i64(&m, F_WIN_RESTORE_VISIBLE) == 0 && model_get_i64(&m, F_WIN_RESTORE_ENABLED) == 0);
    CHECK(model_get_i64(&m, F_WIN_R1_DISMISS_VISIBLE) == 0 && model_get_i64(&m, F_WIN_R1_DISMISS_ENABLED) == 0);
    model_free(&m);
    /* C19: Stems Cache's light: a non-zero R1 state is "not working", with R1's line as its tip */
    cache_light_in in;
    memset(&in, 0, sizeof in);
    in.installed = in.plus_on = 1;
    char tb_buf[512];
    tbuf tip;
    tb_init(&tip, CORE_OS_WIN, tb_buf, sizeof tb_buf);
    CHECK(light_cache(&tip, &in) == LOOK_ON && !strcmp(tip.p, "Stems Cache is on."));
    for (int64_t s = R1_LIVE_MISSING; s <= R1_ORT_MISSING; s++) {
        in.win_r1 = s;
        tb_init(&tip, CORE_OS_WIN, tb_buf, sizeof tb_buf);
        char want[512];
        tbuf w;
        tb_init(&w, CORE_OS_WIN, want, sizeof want);
        w_r1_line(&w, s);
        CHECK(light_cache(&tip, &in) == LOOK_LIMITED && !tip.err && *tip.p && !strcmp(tip.p, want));
    }
    in.installed = 0;   /* R1 holds even when Stems Cache reads as not installed (the live DLL is gone) */
    in.win_r1 = R1_LIVE_MISSING;
    tb_init(&tip, CORE_OS_WIN, tb_buf, sizeof tb_buf);
    CHECK(light_cache(&tip, &in) == LOOK_LIMITED && strstr(tip.p, "onnxruntime.dll is missing"));
}

void win_front_tests(int *passed, int *failed) {
    g_passed = passed;
    g_failed = failed;
    t_r1_fields();
    const uint32_t up = CORE_FRONT_ACTIVE | CORE_FRONT_WINDOW_LOADED;
    /* the one way to be in front, for each tab, with and without the held click's check */
    for (int32_t tab = CORE_TAB_STATUS; tab <= CORE_TAB_MANAGE; tab++) {
        CHECK(win_in_front(up | tab_bits(tab), 0, tab, 0) == 1);
        CHECK(win_in_front(up | tab_bits(tab), 0, tab, 1) == 1);
        CHECK(win_in_front(up | tab_bits(tab), 1, tab, 0) == 1);   /* a dialog up: a check may run under it (I2) */
        CHECK(win_in_front(up | tab_bits(tab), 1, tab, 1) == 0);   /* ... but a held click waits for it */
    }
    /* every other bit combination with the Prepare tab shown and asked about */
    int wrong = 0;
    for (uint32_t bits = 0; bits < 16; bits++) {
        int want = bits == up;
        for (int held = 0; held < 2; held++)
            if (win_in_front(bits | tab_bits(CORE_TAB_PREPARE), 0, CORE_TAB_PREPARE, held) != want) wrong++;
    }
    CHECK(wrong == 0);
    /* another tab shown: not in front for this one */
    CHECK(win_in_front(up | tab_bits(CORE_TAB_MANAGE), 0, CORE_TAB_PREPARE, 0) == 0);
    CHECK(win_in_front(up | tab_bits(CORE_TAB_STATUS), 0, CORE_TAB_PREPARE, 0) == 0);
    /* UAC's secure desktop: the app reads as active again before the worker's exit is handled */
    CHECK(win_in_front(up | CORE_FRONT_ELEVATING | tab_bits(CORE_TAB_PREPARE), 0, CORE_TAB_PREPARE, 0) == 0);
    /* minimized from the taskbar, still active: not in front */
    CHECK(win_in_front(up | CORE_FRONT_MINIATURIZED | tab_bits(CORE_TAB_PREPARE), 0, CORE_TAB_PREPARE, 0) == 0);
    /* bits above the tab are ignored */
    CHECK(win_in_front(up | tab_bits(CORE_TAB_PREPARE) | (1u << 20), 0, CORE_TAB_PREPARE, 0) == 1);
}

#else

void win_front_tests(int *passed, int *failed) {
    g_passed = passed;
    g_failed = failed;
    CHECK(1);   /* the Mac build: no Windows policy compiled (os-rows.sh: the Mac objects) */
}

#endif
