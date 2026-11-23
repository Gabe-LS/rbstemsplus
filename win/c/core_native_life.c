/* core_win.h's entry points: win/c/lifecycle.h's one copy and process start time, and shell.h's
 * process-wide CriticalWork, exported from the core DLL under core_* names (the export gate allows
 * nothing else). Thin: lifecycle.c's decision and shell.c's CriticalWork are host-tested
 * (tests/app_test.c), lifecycle_win.c's calls are the VM's (W13a). */
#include "core_win.h"

#include "lifecycle.h"
#include "shell.h"

#include <errno.h>
#include <stddef.h>

_Static_assert(CORE_WIN_TOKEN_OK == LIFE_TOKEN_OK && CORE_WIN_TOKEN_ELEVATED == LIFE_TOKEN_ELEVATED && CORE_WIN_TOKEN_UAC_OFF == LIFE_TOKEN_UAC_OFF
                   && CORE_WIN_TOKEN_SERVICE == LIFE_TOKEN_SERVICE && CORE_WIN_TOKEN_UNKNOWN == LIFE_TOKEN_UNKNOWN,
               "core_win.h's token answers are lifecycle.h's");
_Static_assert(CORE_WIN_ONE_HELD == LIFE_ONE_HELD && CORE_WIN_ONE_HANDED == LIFE_ONE_HANDED && CORE_WIN_ONE_BUSY == LIFE_ONE_BUSY
                   && CORE_WIN_ONE_FAILED == LIFE_ONE_FAILED,
               "core_win.h's one-copy answers are lifecycle.h's");

/* core_win_one is lifecycle.c's life_one, never looked into here */
CORE_API int32_t CORE_CALL core_win_one_take(const char *name, uint32_t wait_ms, core_win_one **out) {
    if (!out) return CORE_WIN_ONE_FAILED;
    life_one *o = NULL;
    int r = life_one_take(name ? name : LIFE_ONE_NAME, wait_ms, &o);
    *out = r == LIFE_ONE_HELD ? (core_win_one *)o : NULL;
    return r;
}

CORE_API int32_t CORE_CALL core_win_one_window(core_win_one *one, core_win_one_signal on_signal, void *ctx) {
    if (!one || !on_signal) return EINVAL;
    return life_one_window((life_one *)one, on_signal, ctx);
}

CORE_API void CORE_CALL core_win_one_window_close(core_win_one *one) {
    if (one) life_one_window_close((life_one *)one);
}

CORE_API void CORE_CALL core_win_one_close(core_win_one *one) {
    if (one) life_one_close((life_one *)one);
}

CORE_API int64_t CORE_CALL core_win_process_start(uint32_t pid) { return life_process_start(pid); }

/* the shell's start refusal (app.md §5.9, I35): life_token_check, LIFE_TOKEN_* */
CORE_API int32_t CORE_CALL core_win_token_check(void) { return life_token_check(); }

CORE_API int32_t CORE_CALL core_win_critical_running(void) { return critical_running(critical_process()); }

CORE_API int32_t CORE_CALL core_win_critical_wait(uint32_t ms) { return critical_wait_idle(critical_process(), ms); }

CORE_API void CORE_CALL core_win_critical_close(void) { critical_close(critical_process()); }
