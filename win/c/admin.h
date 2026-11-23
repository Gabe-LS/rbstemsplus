/* admin.c: the app's side of the elevated worker (app.md §5.6, W4), in the core DLL. Pure C11
 * over common/plat.h's file layer; what only Windows does (runas on a short-lived STA thread,
 * the pipe server, waiting for the worker) goes through admin_elevate_fn, which admin_win.c
 * implements and the tests fake.
 *
 *  - Which worker (I76, J23): the app's own is the bin\ worker named by its own verified
 *    installed-release record (windows.app.admin_sha256), never one a newer fetched payload
 *    names. The app uses its own when it is in bin\; when it isn't, the newest bin\ worker whose
 *    protocol matches runs uninstall, restore and remove-root-folder, and install first puts the
 *    app's own worker into bin\ through a bin\ worker's install-worker (never refused by the
 *    protocol), then runs it ("run again"). The app folder's admin\RB Stems Plus.exe is elevated
 *    only when bin\ holds no worker at all (I75). When no bin\ worker can serve the app (none
 *    matches its protocol and its own can't be installed), or the floor refuses its payload, the
 *    outcome is "update the app first" (one line, the core's).
 *  - The source folder (--source): the app's verified installed-release pair, the bridge it
 *    verified, and its own worker, copied into one folder of the account's (admin_proto.h's
 *    layout). The worker re-verifies all of it from its own staging copy.
 *  - The worker's lines go to the log; its records (@still-closing, @closed, @restart, @bridge, @floor,
 *    @target, @root-kept) into the result, for the core's texts and the app's own state, written
 *    after the exit code. */
#ifndef RBSP_WIN_ADMIN_H
#define RBSP_WIN_ADMIN_H

#include "admin_proto.h"
#include "payload.h"

#include <stddef.h>
#include <stdint.h>

enum { ADMIN_BIN_MAX = 32 };

/* one bin\<sha12>\ worker as the app sees it (Users may read bin\) */
typedef struct {
    char sha12[13];
    char sha256[65];                 /* of its RB Stems Plus.exe, "" when it couldn't be read */
    int proto;                       /* from its marker; -1 when it has none */
    char minimum[ADMIN_VERSION_MAX]; /* its compiled minimum, from the marker */
    int64_t mtime;
} admin_bin_worker;

/* bin\'s workers under root. *present: how many entries are there at all, by name and kind only
 * (a folder named exactly 12 lowercase hex digits holding something named exactly RB Stems
 * Plus.exe, or one that can't be looked into; a *.del isn't one, J7). Written to out: those that
 * verified (a regular file whose sha256 starts with its folder's name). How many were written (at
 * most max), or -1 when bin\ couldn't be listed. A missing root or bin\ is 0 with *present 0: the
 * only case in which bin\ holds no worker (I75). */
int admin_list_bin(const char *root, admin_bin_worker *out, int max, int *present);

enum {
    ADMIN_PICK_APP_COPY,      /* bin\ holds no worker: elevate the app folder's admin\RB Stems Plus.exe */
    ADMIN_PICK_OWN,           /* the app's own bin\ worker */
    ADMIN_PICK_MATCHING,      /* another bin\ worker of this protocol (uninstall, restore, remove-root-folder) */
    ADMIN_PICK_INSTALL_OWN,   /* a bin\ worker's install-worker first, then the app's own */
    ADMIN_PICK_UPDATE_FIRST,  /* none can serve this app: update the app first */
};
/* The choice (pure). own_sha256: the installed-release payload's windows.app.admin_sha256 ("" when
 * the app has no verified record); proto: the app's (ADMIN_PROTO). *index: the worker to start
 * (for INSTALL_OWN, the one that runs install-worker), -1 for APP_COPY and UPDATE_FIRST. */
int admin_choose(const admin_bin_worker *w, int n, const char *own_sha256, int proto, int verb, int *index);

/* Copies the source folder's files into dir (made when missing, emptied first): payload.json and
 * payload.json.sig from payload_dir, the bridge at bridge_path as bridge_name (the payload's
 * windows.bridge.file; NULL: none, for the verbs that don't install), the worker at worker_path
 * as admin\RB Stems Plus.exe (NULL: none). Each is a regular file read without following a link,
 * written and flushed. 0, or an errno. */
int admin_source_prepare(const char *dir, const char *payload_dir, const char *bridge_path, const char *bridge_name, const char *worker_path);

/* One @target record (protocol 2, mv-spec §2.2): a target's version, its exit code (-1 for
 * "-": not started) and what happened (ADMIN_TARGET_WHAT_*) */
typedef struct {
    char version[ADMIN_VERSION_MAX];
    int code;
    char what[ADMIN_TARGET_WHAT_MAX];
} admin_target_result;

/* What a run told the app */
typedef struct {
    int code;                /* the worker's exit code (admin_proto.h), ADMIN_EXIT_USER_STOPPED, or -1: not started */
    int update_first;        /* the "update the app first" offer applies (12, 14, or no worker can serve) */
    int no_source;           /* the selected verb needed a source folder before elevation */
    int still_closing;       /* the worker waited for rekordbox to close (PLAN R1) */
    int restart_pending;     /* the root's last file is removed at the next restart (I20) */
    char bridge[65];         /* @bridge: the bridge now in rekordbox */
    char floor[PAYLOAD_VERSION_MAX];
    int elevations;          /* how many times UAC was asked (the I75 case: 2) */
    /* protocol 2 (mv-spec §2.2): the @target records in the order written (at most
     * ADMIN_TARGETS_MAX; a malformed one is ignored), and @root-kept */
    admin_target_result targets[ADMIN_TARGETS_MAX];
    int ntargets;
    int root_kept;
} admin_result;

/* Starts exe elevated (verb runas) with params, serving the pipe named pipe for its lines (each
 * passed to line, from the calling thread), waits for it to end. 0 with *code its exit code;
 * ADMIN_EXIT_USER_STOPPED when UAC was declined (ERROR_CANCELLED); -1 when it couldn't be started
 * or its pipe client wasn't it. started (may be NULL) is called once, with sctx, as soon as
 * ShellExecuteExW has returned (the worker started, or it failed, or UAC was declined), before
 * the pipe is waited on, and on every earlier way out: admin.c releases the worker's hold there
 * (review NT1), never at the session's end, since the worker's install-worker and uninstall
 * rename and delete under the held folders. */
typedef int (*admin_elevate_fn)(void *ctx, const char *exe, const char *params, const char *pipe, void (*line)(void *lctx, const char *s, size_t n),
                                void *lctx, void (*started)(void *sctx), void *sctx, int *code);

typedef struct {
    const char *root;          /* <Program Files>\RB Stems Plus (Windows' own: plat_program_files) */
    const char *app_copy;      /* the app folder's admin\RB Stems Plus.exe */
    const char *source;        /* the source folder (admin_source_prepare), prepared for every verb:
                                * any of them may need install-worker first (J23). Without one, an
                                * install or an install-worker is "update the app first" */
    const char *own_sha256;    /* the installed-release payload's windows.app.admin_sha256, or "" */
    const char *manifest;      /* --manifest: the sha256 of the source folder's payload.json (the pair the
                                * app verified, copied there); "" or NULL with no source folder, when
                                * install and install-worker are "update the app first" */
    admin_elevate_fn elevate;
    void *ctx;
    void (*log)(void *ctx, const char *line, size_t n);   /* the core's log path (host->log) */
    void (*random)(void *ctx, unsigned char out[16]);     /* the pipe's name */
    /* Before every elevation of a bin\ worker (never the app folder's copy, which is checked
     * against own_sha256), held through ShellExecuteExW (reviews NA4, NT1): hold checks that only
     * an administrator can change the exe and its parent folders up to Program Files and keeps
     * them so (core_native_admin.c: plat_owner_hold, 4 levels); 0 held, with *held what release
     * takes; nonzero refused, nothing held, the path that failed in bad. Under the hold the exe
     * is hashed again (uncached) against the sha256 it was picked by. A refusal or a different
     * hash is "a check failed" (4), with no fallback to the app folder's copy. release is called
     * exactly once per hold: from the elevation's started, or on the way out. NULL hold: no
     * check, in a test build (ADMIN_TEST_HOOKS) only; elsewhere a bin\ worker is then refused */
    int (*hold)(void *ctx, const char *exe, char *bad, size_t n, void **held);
    void (*release)(void *ctx, void *held);
} admin_client;

/* One admin action: the worker chosen, run (install-worker first when the app's own is missing,
 * then its own, at most once: two UAC prompts, I75), its lines logged, its records and exit code
 * into *res. Returns res->code. targets: --targets (admin_proto.h, protocol 2), the scope the app
 * checked for install and restore ("X.Y.Z:<16 hex>,..."); the other verbs send "*" whatever it
 * is. Runs on the flow thread; blocks. */
int admin_client_run(const admin_client *c, int verb, const char *targets, int force, admin_result *res);

/* The quoted command line of a program and its arguments, as CreateProcessW reads it back (the
 * helper's spawn): every argument in double quotes, backslashes before a quote doubled, a quote
 * inside escaped. The length, or -1 when it doesn't fit. */
long admin_quote_argv(const char *const *argv, int argc, char *out, size_t cap);

#endif
