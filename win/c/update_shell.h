/* update_shell.h: the WPF shell's entries into win/c, exported by the core DLL beside core.h's
 * (W3b and W6b; app.md §5.4, §5.8, §5.9). What the shell needs before the core exists (the
 * update's start and --swap), after its first render, and from its window (device events), plus
 * the quit the flow's UPDATE_SWAP ends with. Each is core.h's ABI: cdecl, strings in UTF-8,
 * int32_t answers; the names start with core_win_ (the DLL's export gate takes core_* only).
 *
 * Defined in update_win.c for the core DLL build only (CORE_NATIVE_WIN), except where noted. The paths
 * come from the shell: `support` is %LOCALAPPDATA%\rbstemsplus, `app` the version folder the
 * running exe is in (its parent is the apps folder), `own` the app's version (the start config's
 * "version"). None is rooted in Program Files (PF3). `log` (may be NULL) gets the updater's lines
 * for the app's log, on the calling thread. */
#ifndef RBSP_WIN_UPDATE_SHELL_H
#define RBSP_WIN_UPDATE_SHELL_H

#include "core.h"

#include <stddef.h>
#include <stdint.h>

typedef void(CORE_CALL *core_win_log_fn)(void *ctx, const char *line, size_t len);
typedef void(CORE_CALL *core_win_quit_fn)(void *ctx);

/* core_win_swap_wait's answers */
enum { CORE_WIN_SWAP_READY = 0, CORE_WIN_SWAP_NONE = 1, CORE_WIN_SWAP_OLD_RUNS = 2 };

/* --swap's first call (update_swap_wait): `pending` names this app as the new one and the old app
 * has exited, waited for at most ms. READY; NONE (no update names this app); OLD_RUNS (the old app
 * still runs after ms); -1 bad arguments. Main thread, before core_new. */
CORE_API int32_t CORE_CALL core_win_swap_wait(const char *support, const char *app, const char *own, uint32_t ms, core_win_log_fn log,
                                              void *ctx);

/* --swap's checks failed (update_swap_failed with UPDATE_FAIL_SWAP: core_selftest or the core's
 * other start checks, the minimum, the off-screen WPF start): `update\result` says "failed swap"
 * and the old app is started again through the stub. 0, or an errno (-1 bad arguments). */
CORE_API int32_t CORE_CALL core_win_swap_failed(const char *support, const char *app, const char *own, core_win_log_fn log, void *ctx);

/* At every start, before the one-copy mutex and the first render (update_at_start, its wait
 * included): UPDATE_BUSY (-2: an update still runs; this start exits with no UI), UPDATE_FAIL_NONE
 * (-1) or the failed update's key, which is also kept for the flow's UPDATE_RESULT
 * (update_result_keep). Bad arguments: UPDATE_FAIL_NONE. Main thread, before core_new. */
CORE_API int32_t CORE_CALL core_win_update_at_start(const char *support, const char *app, const char *own, core_win_log_fn log, void *ctx);

/* After the first full snapshot is on screen (update_after_render): `started` and the pointers
 * for a pending update whose new app this is; else the leftovers. 0, or an errno (-1 bad
 * arguments). A worker thread, never the UI thread (it hashes, deletes folders, stops the old
 * watcher). */
CORE_API int32_t CORE_CALL core_win_update_after_render(const char *support, const char *app, const char *own, core_win_log_fn log,
                                                        void *ctx);

/* About's version as of this call (update_shown_now in update.c). Returns its byte length,
 * excluding the NUL, or -1 for bad arguments. out has room for at least PAYLOAD_VERSION_MAX. */
CORE_API int32_t CORE_CALL core_win_update_shown(const char *support, const char *own, char *out, size_t cap);

/* The minimum (app.md §5.9, lifecycle.h's LIFE_MIN_*): Windows' build by RtlGetVersion and .NET
 * Framework's Release. 0 ok (LIFE_OS_OK), 1 Windows too old (LIFE_OS_WINDOWS_OLD), 2 .NET too old
 * (LIFE_OS_NET_OLD). Any thread. */
CORE_API int32_t CORE_CALL core_win_os_check(void);

/* SetCurrentProcessExplicitAppUserModelID(RBSP_AUMID) (aumid.h), before the first window: the
 * window groups with the pin after every update. 0, or -1. */
CORE_API int32_t CORE_CALL core_win_set_aumid(void);

/* The shell's quit (main thread, before core_new): quit(ctx), from any thread, posts the app's
 * quit at once, asking nothing and never blocking (the flow's UPDATE_SWAP; SELF_DELETE's "the app
 * quits" too). NULL: none. The ctx it gets is the one given here, except for SELF_DELETE's quit,
 * which passes CORE_WIN_QUIT_UNINSTALL: the support folder is gone, so the shell writes no window
 * place and no log file line from then on (review UN5). */
#define CORE_WIN_QUIT_UNINSTALL ((void *)1)
CORE_API void CORE_CALL core_win_set_quit(core_win_quit_fn quit, void *ctx);

/* The complete uninstall's after-exit (afterexit.h, release.md T11): called by the stub's temp copy
 * started with --after-exit, from the copy of this DLL beside it, never by the app. args: the
 * copy's arguments after its own name ("--after-exit <handle> \"<LOCALAPPDATA>\""). ax_run's
 * outcome: AX_DONE 0, AX_BAD_ARGS 1, AX_PARENT_RUNS 2, AX_LEFT 3. Defined in afterexit_win.c. */
CORE_API int32_t CORE_CALL core_win_after_exit(const char *args);

/* WM_DEVICECHANGE (volumes.h's vol_device_events): the core's "mount"/"unmount" events for one
 * device change, each a JSON object followed by "\n", into out (n bytes). event is wParam,
 * devtype the DEV_BROADCAST_HDR's dbch_devicetype, unitmask a volume's dbcv_unitmask, iface a
 * device interface's dbcc_name in UTF-8 (NULL: none). How many events, 0 for any other message,
 * -1 when out is too small. Any thread. */
CORE_API int32_t CORE_CALL core_win_device_events(uint32_t event, uint32_t devtype, uint32_t unitmask, const char *iface, char *out,
                                                  size_t n);

/* For the core DLL's native steps (core_native_steps.c), not exported: whether the shell set a
 * quit, and the quit itself (ctx unused), for UPDATE_SWAP's update_step_swap:
 *     update_step_swap(&ue, update_shell_can_quit() ? update_shell_quit : NULL, NULL) */
int update_shell_can_quit(void);
void update_shell_quit(void *ctx);
/* SELF_DELETE's quit: the registered quit called with CORE_WIN_QUIT_UNINSTALL as its ctx */
void update_shell_quit_uninstall(void);

#ifdef CORE_TEST_HOOKS
/* TEST builds only (CORE_TEST_HOOKS: core/build.sh dll --test-hooks; the release DLL has no
 * core_test_ export, its gate's rule): the releases' base URL the updater and the flows' downloads
 * use from now on instead of UPDATE_BASE, the TEST build's own server (release.md §3.1, T12; the
 * TEST shell sets it from its build's config before anything downloads). https:// only, as
 * update_url requires; NULL or "" goes back to UPDATE_BASE. 0, or -1 (refused: not https://, or
 * longer than 255 bytes). update_test_base: the URL set, or NULL (not exported). */
CORE_API int32_t CORE_CALL core_test_update_base(const char *url);
const char *update_test_base(void);
#endif

#endif
