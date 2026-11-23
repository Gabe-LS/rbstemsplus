/* core_win.h: the core DLL's Windows-only entry points beside core.h (API-V1-NOTES §11, additive:
 * never in the Mac's library, which doesn't build win/c). They hand the WPF shell win/c's own
 * code where the shell needs it before or beside a core: the one-copy mutex and window
 * (win/c/lifecycle.h's life_one_*, app.md §4.3, I88), another process's start time (the busy
 * marker's clock) and CriticalWork (win/c/shell.h, app.md §5.9). cdecl, no struct by value, as
 * core.h. */
#ifndef RBSP_CORE_WIN_H
#define RBSP_CORE_WIN_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- one copy
 *
 * core_win_one_take: one copy at a time (main.swift's lock). name: the mutex, NULL for the app's
 * own ("Local\io.github.rbstemsplus.app"); its window class is named after it. HELD: this copy
 * runs, *out holds the mutex until core_win_one_close (or the process ends); HANDED: a first copy
 * was given the foreground rights and signalled, so exit quietly; BUSY: a copy holds the mutex and
 * shows no window for wait_ms ("Already open"); FAILED: it can't be told ("can't open"). On any
 * answer but HELD, *out is NULL.
 *
 * The thread rule: a mutex belongs to the thread that took it and is abandoned when that thread
 * ends. So take, window, window_close and close all run on one thread that lives as long as the
 * app: the shell's UI thread. A close or a window close from another thread does nothing and
 * returns at once: the window and the mutex stay until that thread or the process ends (so never
 * close from a finalizer: keep *out as an IntPtr, not a SafeHandle).
 *
 * core_win_one_window: the findable message-only window, made on the calling thread (its
 * dispatcher pumps it). on_signal(ctx) runs there when a second start signals: the shell brings
 * its window forward and sends "activated" and "check_request". 0, or an errno.
 * core_win_one_window_close: the window goes as quitting starts (a later start then waits for the
 * mutex rather than signal a copy that is going); the mutex stays until core_win_one_close. */
enum { CORE_WIN_ONE_HELD = 0, CORE_WIN_ONE_HANDED = 1, CORE_WIN_ONE_BUSY = 2, CORE_WIN_ONE_FAILED = 3 };
typedef struct core_win_one core_win_one;
typedef void(CORE_CALL *core_win_one_signal)(void *ctx);
CORE_API int32_t CORE_CALL core_win_one_take(const char *name, uint32_t wait_ms, core_win_one **out);
CORE_API int32_t CORE_CALL core_win_one_window(core_win_one *one, core_win_one_signal on_signal, void *ctx);
CORE_API void CORE_CALL core_win_one_window_close(core_win_one *one);
CORE_API void CORE_CALL core_win_one_close(core_win_one *one);

/* ---------------------------------------------------------------- another process
 *
 * When process pid started, in whole seconds since 1970 (the busy marker's second line, as
 * plat_process_start writes it): -1 when there is no such process (or it has exited), -2 when it
 * couldn't be asked. Another account's process is found through the system's process list. */
CORE_API int64_t CORE_CALL core_win_process_start(uint32_t pid);

/* ---------------------------------------------------------------- the start's token
 *
 * core_win_token_check: this process's token, as the shell's start refuses it (app.md §5.9, I35):
 * OK (a standard user, or an admin's limited token), ELEVATED (Run as administrator), UAC_OFF (an
 * elevated default token), SERVICE (LocalSystem, LocalService, NetworkService), UNKNOWN (not
 * readable). Only OK starts. win/c/lifecycle.h's LIFE_TOKEN_*, asserted. */
enum { CORE_WIN_TOKEN_OK = 0, CORE_WIN_TOKEN_ELEVATED = 1, CORE_WIN_TOKEN_UAC_OFF = 2, CORE_WIN_TOKEN_SERVICE = 3, CORE_WIN_TOKEN_UNKNOWN = 4 };
CORE_API int32_t CORE_CALL core_win_token_check(void);

/* ---------------------------------------------------------------- CriticalWork
 *
 * The steps quitting waits for (win/c/shell.h's process-wide critical_work, the Mac's
 * CriticalWork): the elevated worker, MODEL_COPY, MODEL_RESTORE. The worker is counted from the
 * app's read of its first pipe line to its end (never UAC's prompt, as the Mac never counts the
 * password's; the worker's root open and leftover cleanup may precede that read, and both are
 * safe to stop); the model steps only the copy through the rename (models.c's; the save and the
 * prompts stay outside, as Models.swift's). Any thread.
 * core_win_critical_running: 1 while such a step runs, else 0.
 * core_win_critical_wait: whenIdle, blocking: 1 once no step runs (at once if none does), 0 after
 * about ms. Never on the UI thread for long, never from a step's own thread.
 * core_win_critical_close: closed when the quit is decided (the shell's CloseGate, on the UI
 * thread: Proceed, WM_ENDSESSION(TRUE), WPF's session-end Closing, the veto's quit): from then on
 * no new step is admitted (a new elevation is "stopped" with no UAC prompt; a refused model
 * copy or restore leaves rekordbox's folder untouched, and a copy may have saved the original
 * in the support folder). An admitted step still counts. One-way. */
CORE_API int32_t CORE_CALL core_win_critical_running(void);
CORE_API int32_t CORE_CALL core_win_critical_wait(uint32_t ms);
CORE_API void CORE_CALL core_win_critical_close(void);

/* Atomically replace name in the shell's support folder (a child of parent). 0 or an errno. */
CORE_API int32_t CORE_CALL core_win_window_save(const char *parent, const char *support, const char *name, const void *text, size_t n);
/* Append to Logs/app.log under the same verified support folder. 0 or an errno. */
CORE_API int32_t CORE_CALL core_win_log_append(const char *parent, const char *support, const void *text, size_t n);

/* The WPF shell's host->facts (win/c/host_facts.c), from the start config's paths of c. Main,
 * inside facts only. Writes FACT_KEPT_SETS when CORE_FACTS_KEPT is asked, CORE_FACTS_REFRESH's
 * cheap rows when REFRESH is asked, and CORE_FACTS_PREPARE's rows only together with REFRESH.
 * Returns 1 if FACT_LIB_UNCHECKED is set (the shell then warms again on activation, app.md §2
 * rule 1), 0 if it isn't, and -1 if no REFRESH part was read. On -1 the shell keeps its previous
 * LibUnchecked and the sheet may still hold the kept rows. */
CORE_API int32_t CORE_CALL core_win_cheap_facts(core *c, core_facts *out);
/* The same reads on a worker, keeping nothing but compat.c's memo of rekordbox's files (their
 * sha256 and AlphaTheta pin), so the UI thread's first reads find them (app.md §2 rule 1). Any
 * thread but main, after core_new. c must outlive the warm: the shell never frees c while its
 * warm task runs (the start's: it waits for it, 10 s at most from core_new, before
 * {"ev":"start"}, and then lets it finish; and again on "activated" after an unchecked sheet, one
 * at a time with the start's (Host.WarmOnActivation), never waited for; a future core_free must
 * wait for any warm task first, not free after a bounded wait). */
CORE_API void CORE_CALL core_win_warm_facts(core *c);
/* The app's own rekordbox refresh (multi-version rekordbox, .orchestration/mv-spec.md §2.5): a
 * count that moves whenever either Uninstall view changes (compat.h's
 * compat_uninstall_generation), polled by the shell's 2 s timer while the window is open; 0 when
 * it can't be watched. Any thread, cheap (no registry walk). win/c/core_win_refresh.c */
CORE_API uint64_t CORE_CALL core_win_rekordbox_generation(void);

#ifdef CORE_TEST_HOOKS
/* ---------------------------------------------------------------- test-hooks DLLs only
 *
 * The harness's fake helper (tests/harness/fake-prepare-win.c, probe.conf's helper=): every later
 * helper call spawns path instead of paths.helper (NULL or "": paths.helper again). The TEST
 * shell calls it from its probe; a release DLL has no such entry (core/build.sh's gate refuses
 * any core_test_ export), so a release can't start a fake helper. Any thread. CORE_OK, CORE_E_ARG
 * (1,024 bytes or more) or CORE_E_NOMEM. */
CORE_API int32_t CORE_CALL core_test_win_fake_helper(const char *path);
#endif

#ifdef __cplusplus
}
#endif

#endif
