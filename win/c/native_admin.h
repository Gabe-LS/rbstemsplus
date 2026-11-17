/* native_admin.c: the core DLL's native admin entry as pure rules (API-V1-NOTES §11, core.h's
 * admin comment, app.md §5.5-5.6); core_native_admin.c does it on Windows with admin.c's client.
 * Pure C11: the Mac tests run every rule here.
 *
 *  - The core's admin kinds (core.h's core_admin_kind; plain ints here, core_native_admin.c checks
 *    them against core.h) as the worker's verbs: the install and the reinstall are install, the
 *    uninstall, remove-root-folder and restore their own verbs. FORGET (the Mac's sudo -k) is
 *    nothing on Windows: 0 at once.
 *  - The client's result as admin's return: the worker's exit code (RootExit, 12 protocol, 14
 *    floor), 1223 UAC declined (the user stopped). A worker that couldn't be started, or whose run
 *    couldn't be read, is 1 ("a step failed before rekordbox was changed": the core says the
 *    action failed), never a silent stop: core.h's "< 0" is a cancelled password, which Windows
 *    doesn't have.
 *  - What the pipe's lines mean for the core, live: the first line from the worker is
 *    core_flow_progress(ADMIN_ATTEMPT, 1) (the worker runs; the Mac's password accepted);
 *    @still-closing is STILL_CLOSING 0 and @closed (rekordbox's process gone) STILL_CLOSING 1;
 *    every other line keeps the state (the worker's "waiting for it" follows @still-closing at
 *    once). The run's end is the backstop (exit 5 after the linger, a crash): a closing line still
 *    up comes down. @restart is core_admin_note(RESTART_PENDING). A run that ended with no line at
 *    all still counts as an attempt when the worker ran. */
#ifndef RBSP_WIN_NATIVE_ADMIN_H
#define RBSP_WIN_NATIVE_ADMIN_H

#include "admin.h"

/* core.h's core_admin_kind values */
enum { NA_KIND_INSTALL = 0, NA_KIND_REINSTALL = 1, NA_KIND_UNINSTALL = 2, NA_KIND_REMOVE_ROOT = 3, NA_KIND_FORGET = 4, NA_KIND_RESTORE = 5 };
enum { NA_FORGET = -2 };   /* native_admin_verb: nothing to run, admin returns 0 */

/* the worker's verb (enum admin_verb) for a core admin kind; NA_FORGET; -1 for an unknown kind */
int native_admin_verb(int kind);
/* whether the verb needs the flow's checked bridge in the source folder (the install) */
int native_admin_needs_bridge(int verb);
/* admin's return for a client run's result */
int native_admin_code(const admin_result *r);
/* The run's result line, as core.h asks of the host (the Mac's Admin.swift): "  ok: <verb>", or
 * "  FAILED: <verb> [exit <code>]" with the raw code before native_admin_code maps it (a negative
 * one, or an NTSTATUS a crashed worker exits with, as 0x%08X), into out. */
void native_admin_result_line(int verb, int code, char *out, size_t n);

/* Whether the install may stage the flow's checked bridge (review NA2): the app's verified
 * installed-release record m (NULL: none) names exactly that bridge, its windows.bridge.sha256 and
 * windows.bridge.file. Otherwise the install is "update the app first" (12), decided before any
 * UAC prompt: staging a newer release's bridge beside this app's record mixes releases (I76). */
int native_admin_bridge_ok(const payload_manifest *m, const char *bridge_sha, const char *bridge_name);
/* What a source folder that couldn't be prepared means (NA6): with a verified record, a check
 * failed (4: an update wouldn't help); without one, the client's "update the app first" for the
 * verbs that need it (-1: run on with no source folder). */
int native_admin_source_failed(int has_record);

/* what to tell the core, in this order (NA_ATTEMPT first) */
enum { NA_ATTEMPT = 1, NA_CLOSING = 2, NA_CLOSED = 4, NA_RESTART = 8 };
typedef struct {
    int attempt;   /* ADMIN_ATTEMPT was sent */
    int closing;   /* STILL_CLOSING 0 was sent, 1 not yet */
} native_admin_track;
void native_admin_track_init(native_admin_track *t);
/* one pipe line of kind (admin_line_kind's ADMIN_LINE_*): the NA_* to send now */
unsigned native_admin_line(native_admin_track *t, int kind);
/* one elevation ended; ran: the worker was started (UAC accepted). The NA_* still to send */
unsigned native_admin_end(native_admin_track *t, int ran);

/* One admin call's elevations, seen on the way (review NA7): an admin_elevate_fn
 * (native_admin_elevate, with this as admin_client's ctx) around the real one (inner: admin_win.c's;
 * the tests' fake). Each pipe line goes through native_admin_line to tell, then on to admin.c's
 * line callback; each elevation's end through native_admin_end; an elevation that couldn't start
 * the worker (-1) calls not_started first; critical brackets the worker's run (below). ctx: the
 * caller's (core_native_admin.c: its nw_admin). */
typedef struct {
    admin_elevate_fn inner;
    void *inner_ctx;
    void (*tell)(void *ctx, unsigned what);    /* NA_* bits for the core, in order */
    void (*not_started)(void *ctx);            /* may be NULL */
    void *ctx;
    native_admin_track t;
    void (*line)(void *lctx, const char *s, size_t n);
    void *lctx;
    /* may be NULL (set after init): CriticalWork's step, with ctx. 1 from the app's read of the
     * worker's first pipe line (UAC's prompt is over; the worker's root open and leftover cleanup
     * may come before that read, and both are safe to stop: the next run cleans them again, WC1),
     * 0 when its elevation returns; once per elevation that ran, never for one declined, not
     * started or ended before any line (the Mac's root script, never its password prompt) */
    void (*critical)(void *ctx, int on);
    int in_critical;
    /* may be NULL (set after init): 0 once the quit is decided (CriticalWork's admission,
     * shell.h's critical_admit): the elevation is then not tried at all, no UAC prompt, and
     * answers ADMIN_EXIT_USER_STOPPED (no dialog), started called as on every way out (WC1) */
    int (*admit)(void *ctx);
} native_admin_run;
void native_admin_run_init(native_admin_run *r, admin_elevate_fn inner, void *inner_ctx, void (*tell)(void *ctx, unsigned what),
                           void (*not_started)(void *ctx), void *ctx);
int native_admin_elevate(void *ctx, const char *exe, const char *params, const char *pipe, void (*line)(void *lctx, const char *s, size_t n),
                         void *lctx, void (*started)(void *sctx), void *sctx, int *code);

#endif
