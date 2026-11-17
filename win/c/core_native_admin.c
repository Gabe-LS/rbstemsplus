/* The core DLL's native admin (core_native_admin.h). One host->admin call:
 *
 *  - The kind as the worker's verb (native_admin.c); FORGET is nothing (no password on Windows).
 *  - The worker: admin.c's client picks it (the app's own bin\ worker, named by its verified
 *    installed-release record's windows.app.admin_sha256, I76, J23) and runs it with admin_win.c's
 *    elevation (runas on a short-lived STA thread, the pipe server), install-worker first when the
 *    app's own is missing (two UAC prompts, I75).
 *  - The source folder (support\admin-source, the account's own): the installed-release pair, the
 *    app folder's worker, and for the install the flow's checked bridge (CACHE_PREPARE's): exactly
 *    that file, its copy hashed again against the sha256 the flow checked. The worker verifies all
 *    of it again from its own staging copy.
 *  - UAC's prompt is owned by the app's window: the first visible top-level window of the core's
 *    main thread (the WPF UI thread), found at the call.
 *  - The pipe's lines go to the app's log; its records become the core's live progress
 *    (native_admin.c): ADMIN_ATTEMPT, STILL_CLOSING 0 and 1, RESTART_PENDING. The whole call is
 *    the in-flight elevation of front_facts' CORE_FRONT_ELEVATING (native.c).
 *  - The worker's run, from its first pipe line to its elevation's end, counts in the process's
 *    CriticalWork (shell.h's critical_process): quitting and a logoff wait for it (app.md §5.9).
 *  - The root is the worker's own, <Program Files>\RB Stems Plus (win/admin/main.c): Windows' own
 *    Program Files as core_new read it (native_core_programs: plat_program_files, the worker's
 *    rule), on whichever drive. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "plat.h"
#include "plat_win.h"
#include "win_rules.h"

#include "core_native_admin.h"

#include "admin_win.h"
#include "native_admin.h"
#include "native_facts.h"
#include "shell.h"
#include "update.h"
#include "core_win.h"   /* core_win_warm_facts (host_facts.c) */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(NA_KIND_INSTALL == CORE_ADMIN_CACHE_INSTALL && NA_KIND_REINSTALL == CORE_ADMIN_CACHE_REINSTALL &&
                   NA_KIND_UNINSTALL == CORE_ADMIN_CACHE_UNINSTALL && NA_KIND_REMOVE_ROOT == CORE_ADMIN_REMOVE_ROOT_FOLDER &&
                   NA_KIND_FORGET == CORE_ADMIN_FORGET && NA_KIND_RESTORE == CORE_ADMIN_RESTORE,
               "native_admin.h's kinds are core.h's");
_Static_assert(ADMIN_EXIT_USER_STOPPED == 1223, "UAC declined is the core's 1223");

enum { NA_KEYS_MAX = 4, NA_ROOT_MAX = 512 };

struct nw_admin {
    core *c;
    DWORD main_tid;
    char *support, *app, *own;
    char root[NA_ROOT_MAX];   /* <Program Files>\RB Stems Plus, the worker's (win/admin/main.c) */
    sig_pubkey keys[NA_KEYS_MAX];
    size_t nkeys;
    SRWLOCK lock;   /* the flow's bridge */
    char *bridge_path, *bridge_name;
};

static char *dup_or_null(const char *s) { return s && s[0] ? _strdup(s) : NULL; }

nw_admin *nw_admin_open(core *c) {
    nw_admin *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->c = c;
    a->main_tid = GetCurrentThreadId();   /* open runs on main */
    InitializeSRWLock(&a->lock);
    const char *s = native_core_path(c, "support"), *p = native_core_path(c, "app"), *v = native_core_version(c);
    a->support = dup_or_null(s);
    a->app = dup_or_null(p);
    a->own = dup_or_null(v);
    const char *pf = native_core_programs(c);
    if ((s && s[0] && !a->support) || (p && p[0] && !a->app) || (v && v[0] && !a->own) || !pf
        || wr_program_path(pf, WR_PF_ROOT, a->root, sizeof a->root) < 0) {
        nw_admin_close(a);
        return NULL;
    }
    for (int i = 0; i < native_n_key_texts && a->nkeys < NA_KEYS_MAX; i++)
        if (!sig_key_from_text(&native_key_texts[i], &a->keys[a->nkeys])) a->nkeys++;
    return a;
}

void nw_admin_close(nw_admin *a) {
    if (!a) return;
    free(a->support);
    free(a->app);
    free(a->own);
    free(a->bridge_path);
    free(a->bridge_name);
    free(a);
}

int nw_admin_set_bridge(nw_admin *a, const char *path, const char *name) {
    char *p = path ? _strdup(path) : NULL, *n = path && name ? _strdup(name) : NULL;
    if (path && (!p || !n)) {
        free(p);
        free(n);
        return ENOMEM;
    }
    AcquireSRWLockExclusive(&a->lock);
    free(a->bridge_path);
    free(a->bridge_name);
    a->bridge_path = p;
    a->bridge_name = n;
    ReleaseSRWLockExclusive(&a->lock);
    return 0;
}

/* ---------------------------------------------------------------- R1 */

int nw_admin_r1(nw_admin *a, const char *folder) {
    return native_r1_read(compat_os_windows(), a->support, a->own, a->keys, a->nkeys, folder, a->root);
}

/* ---------------------------------------------------------------- the account */

int native_user_is_admin(void) {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &t)) return -1;
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD got = 0;
    HANDLE check = NULL;
    if (GetTokenInformation(t, TokenElevationType, &type, sizeof type, &got) && type == TokenElevationTypeLimited) {
        /* an administrator's normal start: the filtered token has the group deny-only */
        TOKEN_LINKED_TOKEN linked;
        if (GetTokenInformation(t, TokenLinkedToken, &linked, sizeof linked, &got)) check = linked.LinkedToken;
    } else if (!DuplicateToken(t, SecurityIdentification, &check))
        check = NULL;   /* CheckTokenMembership takes an impersonation token */
    CloseHandle(t);
    if (!check) return -1;
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD n = sizeof sid;
    BOOL member = FALSE;
    int r = CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sid, &n) && CheckTokenMembership(check, sid, &member) ? member != 0 : -1;
    CloseHandle(check);
    return r;
}

/* ---------------------------------------------------------------- one run */

static void say(nw_admin *a, const char *line) { native_core_log(a->c, line); }

/* native_admin_run's: the core told, live (the flow thread: core_flow_progress is worker-only) */
static void tell(void *ctx, unsigned what) {
    nw_admin *a = ctx;
    core *c = a->c;
    if (what & NA_ATTEMPT) core_flow_progress(c, CORE_PROGRESS_ADMIN_ATTEMPT, 1, 0);
    if (what & NA_CLOSING) core_flow_progress(c, CORE_PROGRESS_STILL_CLOSING, 0, 0);
    if (what & NA_CLOSED) core_flow_progress(c, CORE_PROGRESS_STILL_CLOSING, 1, 0);
    if (what & NA_RESTART) core_admin_note(c, CORE_ADMIN_NOTE_RESTART_PENDING);
}

/* one call's state: the elevation's (admin_win) and the glue's (native_admin_run, admin_client's ctx) */
typedef struct {
    native_admin_run run;   /* first: admin_client's ctx is this struct */
    nw_admin *a;
    admin_win aw;
} run_ctx;

static void not_started(void *ctx) {
    run_ctx *r = ctx;
    char b[96];
    snprintf(b, sizeof b, "  admin: the worker couldn't be started (error %lu)", r->aw.last_err);
    say(r->a, b);
}

static void run_tell(void *ctx, unsigned what) { tell(((run_ctx *)ctx)->a, what); }

/* the elevated worker running counts in the process's CriticalWork (the Mac's root script) */
static void run_critical(void *ctx, int on) {
    (void)ctx;
    if (on) critical_begin(critical_process());
    else critical_end(critical_process());
}

/* no new elevation once the quit is decided (WC1: CriticalWork's admission) */
static int run_admit(void *ctx) {
    (void)ctx;
    return critical_admit(critical_process());
}

static void run_log(void *ctx, const char *s, size_t n) {
    run_ctx *r = ctx;
    char b[ADMIN_LINE_MAX + 8];
    snprintf(b, sizeof b, "  %.*s", (int)(n < ADMIN_LINE_MAX ? n : ADMIN_LINE_MAX), s);
    say(r->a, b);
}

/* admin_client's hold (review NT1): a bin\ worker and its folders up to Program Files (<sha12>,
 * bin, RB Stems Plus, Program Files) owned and writable only by Administrators, SYSTEM or
 * TrustedInstaller, with no reparse point (the bridge's rule), held open sharing READ only until
 * run_release (plat_owner_hold) */
static int run_hold(void *ctx, const char *exe, char *bad, size_t n, void **held) {
    (void)ctx;
    plat_owner_held *h = NULL;
    int r = plat_owner_hold(exe, 4, 0, bad, n, &h);
    *held = h;
    return r != PLAT_OWNER_OK;
}

static void run_release(void *ctx, void *held) {
    (void)ctx;
    plat_owner_release(held);
}

static void run_random(void *ctx, unsigned char out[16]) {
    (void)ctx;
    admin_win_random(NULL, out);
}

/* the app's window, for UAC's prompt: the main thread's first visible window that has no owner */
static BOOL WINAPI first_window(HWND w, LPARAM lp) {
    if (IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        *(HWND *)lp = w;
        return FALSE;
    }
    return TRUE;
}

static int join2(char *out, size_t n, const char *a, const char *b) {
    int k = snprintf(out, n, "%s\\%s", a, b);
    return k < 0 || (size_t)k >= n;
}

int32_t nw_admin_run(nw_admin *a, int32_t kind, const char *version, const char *bridge_sha, int32_t force) {
    int verb = native_admin_verb(kind);
    if (verb == NA_FORGET) return 0;
    if (verb < 0) {
        say(a, "  admin: an unknown action: not run");
        return ADMIN_EXIT_FAILED;
    }
    if (!a->support || !a->app || !a->own) {
        say(a, "  admin: the app's folders or version aren't known: not run");
        return ADMIN_EXIT_FAILED;
    }
    char app_copy[ADMIN_PATH_MAX], source[ADMIN_PATH_MAX], payload_dir[ADMIN_PATH_MAX], copy[ADMIN_PATH_MAX];
    if (join2(app_copy, sizeof app_copy, a->app, ADMIN_APP_COPY_DIR "\\" ADMIN_WORKER_NAME) || join2(source, sizeof source, a->support, "admin-source")
        || join2(payload_dir, sizeof payload_dir, a->support, "installed-release")) {
        say(a, "  admin: a path is too long: not run");
        return ADMIN_EXIT_FAILED;
    }
    /* the app's own worker: the one its verified installed-release record names; the record is
     * kept for the install's bridge check */
    char own_sha[65] = "";
    int has_record = 0;
    payload_manifest *m = malloc(sizeof *m);
    if (m) {
        update_env ue;
        memset(&ue, 0, sizeof ue);
        ue.support = a->support;
        ue.own = a->own;
        ue.keys = a->keys;
        ue.nkeys = a->nkeys;
        char at[ADMIN_PATH_MAX];
        if (update_installed_at(&ue, m, at, sizeof at) == PAYLOAD_OK && m->has_windows) {
            has_record = 1;
            memcpy(own_sha, m->win_app.admin_sha256, sizeof own_sha);
            memcpy(payload_dir, at, strlen(at) + 1);   /* the pair that verified: installed-release or its prev\ (NA5) */
        }
    }
    /* the flow's checked bridge, for the install */
    char *bpath = NULL, *bname = NULL;
    AcquireSRWLockExclusive(&a->lock);
    if (a->bridge_path) {
        bpath = _strdup(a->bridge_path);
        bname = _strdup(a->bridge_name);
    }
    ReleaseSRWLockExclusive(&a->lock);
    int32_t rc = -1;
    const int with_bridge = native_admin_needs_bridge(verb);
    if (with_bridge && (!bpath || !bname)) {
        say(a, "  admin: no checked bridge for this install: not run");
        rc = ADMIN_EXIT_CHECK_FAILED;
        goto out;
    }
    /* the install stages only this app's release's bridge (NA2): else update first, before UAC */
    if (with_bridge && !native_admin_bridge_ok(has_record ? m : NULL, bridge_sha, bname)) {
        say(a, "  admin: the checked bridge isn't this app's release's: update first");
        rc = ADMIN_EXIT_PROTOCOL;
        goto out;
    }
    int e = admin_source_prepare(source, payload_dir, with_bridge ? bpath : NULL, with_bridge ? bname : NULL, app_copy);
    if (e) {
        char b[160];
        snprintf(b, sizeof b, "  admin: the source folder couldn't be prepared (%s)", strerror(e));
        say(a, b);
        /* NA6: with a verified record, a check failed (an update wouldn't help); without one, the
         * verbs that need the folder (install, install-worker) are "update the app first" */
        if ((rc = native_admin_source_failed(has_record)) >= 0) goto out;
        source[0] = 0;
    } else if (with_bridge && bridge_sha) {
        /* exactly the bytes the flow checked. This hash is never memoised: it reads the copy about
         * to be elevated, so nw_file_sha256 is sha_memo.c's uncached sha_file, never sha_memo_file */
        char got[65];
        if (join2(copy, sizeof copy, source, bname) || nw_file_sha256(copy, got) || strcmp(got, bridge_sha)) {
            say(a, "  admin: the bridge changed after its check: not run");
            rc = ADMIN_EXIT_CHECK_FAILED;
            goto out;
        }
    }
    run_ctx r;
    memset(&r, 0, sizeof r);
    r.a = a;
    native_admin_run_init(&r.run, admin_win_elevate, &r.aw, run_tell, not_started, &r);
    r.run.critical = run_critical;
    r.run.admit = run_admit;
    HWND owner = NULL;
    EnumThreadWindows(a->main_tid, first_window, (LPARAM)&owner);
    r.aw.owner = owner;
    admin_client cl;
    memset(&cl, 0, sizeof cl);
    cl.root = a->root;
    cl.app_copy = app_copy;
    cl.source = source;
    cl.own_sha256 = own_sha;
    cl.elevate = native_admin_elevate;
    cl.ctx = &r;
    cl.log = run_log;
    cl.random = run_random;
    cl.hold = run_hold;
    cl.release = run_release;
    admin_result res;
    admin_client_run(&cl, verb, version ? version : "", force, &res);
    core_win_warm_facts(a->c);   /* the files it changed, read and pinned again here, not on the UI thread (HF2) */
    char line[96];
    native_admin_result_line(verb, res.code, line, sizeof line);   /* the raw code, before the mapping */
    say(a, line);
    rc = native_admin_code(&res);
out:
    free(m);
    free(bpath);
    free(bname);
    return rc;
}
