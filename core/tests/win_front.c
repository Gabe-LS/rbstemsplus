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

void win_front_tests(int *passed, int *failed) {
    g_passed = passed;
    g_failed = failed;
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
