/* update.c: the Windows app's updater, the C part (app.md §5.4, W3a; release.md §1.4, §2, §7).
 * The Windows copy of Update.swift's rules, under one version number for both platforms.
 *
 * What it does, in order:
 *  - The check (at most once a day; the schedule is the core's): payload.json and its signature,
 *    from `releases/latest` only (no beta channel, release.md §2). A payload without the `windows`
 *    block is no news. One with it is saved with payload_accept (the floor raised first).
 *  - What a verified payload means (update_seen): the version shown never steps back (ShownVersion,
 *    mac-core §6.3); a payload that names this very app (windows.app.version == own) also becomes
 *    the `installed-release` record and moves the version shown, with no download (a release
 *    whose Windows files are unchanged); a newer windows.app.version is an update.
 *  - The update (update_stage): the zip downloaded (WinHTTP, HTTPS only, Range resume), its
 *    SHA-256 checked against the verified payload, unpacked (unzip.c) into a staging folder, its
 *    contents checked (the files the zip must hold, the worker's and both stubs' SHA-256, the
 *    app's ProductVersion, no link or reparse point), renamed to `<version>\`; then `update\pending`
 *    is written and the new app is started with `--swap`, and the old one quits.
 *  - The new app (update_after_render, after its first full snapshot): `update\started`, then the
 *    pointers move: `current` (the commit point), `installed-release`, the shown version and the
 *    Apps & features DisplayVersion; a changed stub is written as `RB Stems Plus.exe.new`, checked
 *    against the payload's stub_sha256 and swapped in by rename (the old one kept as `.old` until the
 *    next successful start, I83); the old watcher stopped by its named event (waiting on its
 *    process, found by full path) and the new one started through the stub; the old folder deleted.
 *  - At every start (update_at_start): a `pending` record whose new app never wrote `started`, or
 *    whose `current` never moved, is a failed update: the pointers stay (they never moved), the new
 *    folder goes, and `update\result` holds the failure ("failed <key>"), shown once.
 *
 * Files (app.md §5.10): the support folder %LOCALAPPDATA%\rbstemsplus\ holds payload\ (the saved
 * payload pair), payload-version (the floor), shown-version, installed-release\ (payload.json and
 * payload.json.sig: the verified payload that installed this app) and update\ (pending, started,
 * result, the payload pair being installed, the zip and its .part). The app folder
 * %LOCALAPPDATA%\Programs\RB Stems Plus\ holds the stub `RB Stems Plus.exe`, `current` and the
 * version folders `<version>\`.
 *
 * update_rules.c holds the pure rules (no file, no network); update.c the updater itself, in pure
 * C11 over common/plat.h's file layer, testable on the Mac; what only Windows does (WinHTTP,
 * processes, the watcher's event, the registry, deleting a folder tree) goes through update_net and
 * update_os, which update_win.c implements and the tests fake. Nothing here runs on the UI thread:
 * every call that touches files or the network is for a worker thread (app.md §2 rule 1). */
#ifndef RBSP_WIN_UPDATE_H
#define RBSP_WIN_UPDATE_H

#include "payload.h"
#include "plat.h"

#include <stddef.h>
#include <stdint.h>

enum {
    UPDATE_PATH_MAX = 1024,
    UPDATE_URL_MAX = 512,
    UPDATE_ZIP_MAX = 256 * 1024 * 1024,      /* the app zip's size cap */
    UPDATE_RECORD_MAX = 256,                 /* pending, started, current, shown-version, result */
    UPDATE_SWAP_WAIT_MS = 60000,             /* how long --swap waits for the old app (update_swap_wait) */
};

/* -------------------------------------------------------------- the release's addresses
 * (a contract with release.md §2: one tag per release, v<payload_version>) */

#define UPDATE_BASE "https://github.com/Gabe-LS/rbstemsplus/releases"
#define UPDATE_USER_AGENT "RB Stems Plus"

/* The URL of a release file: tag NULL: `<base>/latest/download/<name>`; else
 * `<base>/download/v<tag>/<name>`. base NULL: UPDATE_BASE. name must be a plain file name (letters,
 * digits, '.', '-', '_'; not "." or ".."), tag a version. 0, or -1 when either isn't or it doesn't
 * fit. */
int update_url(const char *base, const char *tag, const char *name, char *out, size_t n);

/* -------------------------------------------------------------- versions (pure) */

/* a release's version: x.y.z (payload_is_release_version) */
int update_is_version(const char *s);

/* ShownVersion's record: the shown version, then the app's own version, one per line. */
typedef struct {
    char shown[PAYLOAD_VERSION_MAX];
    char app[PAYLOAD_VERSION_MAX];
} update_shown;
/* ShownVersion(data): 1 with *out when it is one (exactly "S\nA\n", under 64 bytes, both versions,
 * S not older than A), else 0 */
int update_shown_parse(const void *data, size_t n, update_shown *out);
/* the record's bytes ("S\nA\n"), NUL-terminated; their length */
size_t update_shown_format(const update_shown *r, char *out, size_t n);
/* nextShownVersion: the record after this app (own version `own`) has read a verified payload
 * that says `seen_payload` (its payload_version) and `seen_app` (its windows.app.version; either
 * NULL: none seen), from the record before (NULL: none). 1 with *out, 0: no record. As
 * Update.swift's rule. */
int update_next_shown(const char *own, const update_shown *before, const char *seen_payload, const char *seen_app, update_shown *out);
/* the version shown by an app whose own version is own, with this record (NULL: none) */
const char *update_shown_version(const char *own, const update_shown *record);

/* Whether m offers an update: it has the windows block and its windows.app.version is newer than
 * own (semver; never payload_version: a carried app is no update) */
int update_newer(const payload_manifest *m, const char *own);

/* -------------------------------------------------------------- the records (pure) */

/* `pending` (written by the old app before it starts the new one with --swap): the old app's own
 * version, the new one's, the old app's pid and its start time (plat_process_start), one per line. */
typedef struct {
    char from[PAYLOAD_VERSION_MAX];
    char to[PAYLOAD_VERSION_MAX];
    uint32_t pid;
    int64_t started;
} update_pending;
int update_pending_parse(const void *data, size_t n, update_pending *out);   /* 1 or 0 */
size_t update_pending_format(const update_pending *p, char *out, size_t n);

/* `current` (the stub reads it, stub.h): one version, then a line end. 1 with the version, or 0. */
int update_current_parse(const void *data, size_t n, char out[PAYLOAD_VERSION_MAX]);

/* `update\result`'s failure keys (Update.swift's, minus "sign": the Windows app isn't signed) */
enum {
    UPDATE_BUSY = -2,      /* update_at_start: the new app still runs (its --swap); this start exits, no UI */
    UPDATE_FAIL_NONE = -1,
    UPDATE_FAIL_GENERIC,   /* "failed": a pending update whose new app never rendered (no key) */
    UPDATE_FAIL_APP,       /* "failed app": the app moved or changed while it was updating */
    UPDATE_FAIL_DOWNLOAD,  /* "failed download": the download isn't a valid copy of the app */
    UPDATE_FAIL_STAGING,   /* "failed staging": something else is in the way of the new folder */
    UPDATE_FAIL_COPY,      /* "failed copy": the new version couldn't be unpacked */
    UPDATE_FAIL_COPY_CHECK,/* "failed copy-check": the unpacked new version didn't check out */
    UPDATE_FAIL_SWAP,      /* "failed swap": the new version couldn't take this one's place (--swap's checks) */
};
/* the result line for a key ("failed copy-check"); NULL for UPDATE_FAIL_NONE */
const char *update_fail_line(int key);
/* updateFailureReason's parse: the key of a result file's text, UPDATE_FAIL_GENERIC for "failed"
 * alone or a line that isn't one of ours (Update.swift then says no reason), UPDATE_FAIL_NONE for
 * no text at all */
int update_fail_parse(const void *data, size_t n);

/* The watcher's stop event, per watcher process (W7 creates it at its start and exits when it is
 * set): "Local\io.github.rbstemsplus.watcher.stop.<pid>". Its length, or 0 when it doesn't fit. */
size_t update_watcher_event_name(uint32_t pid, char *out, size_t n);

/* What stop_watchers does with a watcher it found by full path (review UN7). Its stop event is
 * Local\ to its own session, so only a watcher in this process's session can be asked to stop
 * (and ended if it doesn't). UPDATE_WATCHER_STOP when both sessions are known (ProcessIdToSessionId
 * succeeded for each) and equal, else UPDATE_WATCHER_LEAVE: it is left running, never ended. */
enum { UPDATE_WATCHER_LEAVE = 0, UPDATE_WATCHER_STOP = 1 };
int update_watcher_action(int mine_known, uint32_t mine, int theirs_known, uint32_t theirs);
/* stop_watchers' result from its scan: -1 when the process list couldn't be read (listed 0) or a
 * watcher was left running (its files stay), else 1 when one was stopped, else 0 */
int update_stop_result(int listed, int stopped, int left);
/* running_under's and watchers_in's scan, its pure parts (review UQ's GPT re-run, G2, GH1). A
 * listed process whose path can't be read is unknown only when its image name (the snapshot's,
 * UTF-8) is one an apps folder runs: UPDATE_IMAGE_APP "RB Stems Plus.exe" (the stub, the shell,
 * the admin helper) or UPDATE_IMAGE_WATCHER an rbsp-watch exe, ASCII case ignored, their 8.3 forms
 * too (a ShortPath launch shortens the name: RBSTEM~ and RBSP-W~ followed by anything, review
 * K2F3; the hashed form after four collisions isn't covered); else UPDATE_IMAGE_OTHER. Its path can't be read when QueryFullProcessImageNameW fails on the opened
 * handle, or when OpenProcess is refused with an error update_open_skips doesn't skip. */
enum { UPDATE_IMAGE_OTHER = 0, UPDATE_IMAGE_APP = 1, UPDATE_IMAGE_WATCHER = 2 };
int update_image_kind(const char *name);
/* a refused OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) that isn't unknown by its name alone:
 * 1 for ERROR_INVALID_PARAMETER (the pid gone) and ERROR_ACCESS_DENIED (another account's or the
 * system's, or this account's in another logon session: one in this logon session always opens,
 * elevated too; other accounts' copies never count, app.md 5.9), else 0 (Win32's codes;
 * update_win.c asserts them). watchers_in's rule (this session's watchers only, GR1);
 * running_under and stop_watchers read a refused one's path too (update_listed_verdict, K2) */
enum { UPDATE_ERR_ACCESS_DENIED = 5, UPDATE_ERR_INVALID_PARAMETER = 87 };
int update_open_skips(unsigned long err);
/* One listed process in running_under's and stop_watchers' scans (review K2). One of this account's
 * in another logon session (a password-logon task in session 0, a second logon) refuses
 * OpenProcess with ERROR_ACCESS_DENIED as another account's does, so a refused one's image path is
 * read from the system's process list instead (SystemProcessIdInformation: no access to the
 * process needed, any session, any account) and matched against the folder's NT forms.
 *   opened: OpenProcess gave a handle; err: its error when not;
 *   path: 1 the image path was read (QueryFullProcessImageNameW on the handle; the system's list
 *     when refused), 0 the process is gone, -1 it couldn't be read or compared;
 *   match: the path is the folder's (running_under: under it; stop_watchers: a watcher in it);
 *   kind: update_image_kind of its snapshot name; watcher_only: only UPDATE_IMAGE_WATCHER is ours.
 * UPDATE_LISTED_FOUND when the path was read and matches, whichever session and account (it runs
 * from the folder, which is in this account's own profile); UPDATE_LISTED_UNKNOWN when that can't
 * be told and its name is ours: a path that can't be read (opened or refused), or a refusal other
 * than ERROR_ACCESS_DENIED whose path isn't the folder's (as before); else UPDATE_LISTED_SKIP:
 * gone (ERROR_INVALID_PARAMETER, path 0), opened and not from the folder, or ERROR_ACCESS_DENIED
 * with its path elsewhere (another account's own copy or the system's: GH1 holds). */
enum { UPDATE_LISTED_SKIP = 0, UPDATE_LISTED_FOUND = 1, UPDATE_LISTED_UNKNOWN = 2 };
int update_listed_verdict(int opened, unsigned long err, int path, int match, int kind, int watcher_only);
/* whether a process's NT image path is the folder's (review K2's undefined1, undefined2), for
 * update_listed_verdict's path and match: 1 it is, 0 it is elsewhere, -1 that can't be told.
 *   read: the image path was read; matched: it matched one of the folder's NT forms, or its
 *     alias did (below);
 *   canonical: the folder's final path by handle was had (links resolved), or the folder doesn't
 *     exist (nothing runs from it). Without either, the folder's only form keeps the junctions and
 *     subst drives of its spelling while an image path has them resolved: a mismatch proves
 *     nothing;
 *   alias_resolved: 0 the image has no 8.3 component ('~'), 1 it had one and its final path by
 *     handle was compared too, -1 it had one that couldn't be resolved (the image keeps the
 *     launcher's 8.3 spelling: a mismatch proves nothing).
 * -1 without a read; else 1 on a match; else 0 only with a canonical form (or a missing folder)
 * and no unresolved alias, -1 otherwise */
int update_nt_path(int read, int matched, int canonical, int alias_resolved);
#if defined(_WIN32) && defined(UPDATE_WIN_TEST_HOOKS)
/* update_win.c's NT path match for its Windows test (tests/gate_update.c, review K2's undefined4):
 * pid's image path from the system's list into out (UPDATE_WIN_NT_MAX): 1, 0 gone, -1 unreadable;
 * image (NT form) against dir's NT forms: nt_matches' 1, 0 or -1 (-2 no memory), by_form[i] whether
 * form i alone matches it */
enum { UPDATE_WIN_NT_MAX = 32768 };
int update_win_test_listed_image(unsigned long pid, wchar_t *out);
int update_win_test_nt_matches(const wchar_t *dir, const wchar_t *image, int watcher_only, int by_form[2]);
#endif
/* the scan's result: 1 when one was found, else -1 when one was unknown or the list didn't end
 * with ERROR_NO_MORE_FILES (ended 0), else 0. Its callers wait on -1 as on a program that runs. */
int update_scan_result(int found, int unknown, int ended);

/* A command line argument quoted as CommandLineToArgvW reads it back (UTF-8 here; the Windows
 * layer converts the whole line): appended to out (n bytes, NUL-terminated). 0, or -1 when it
 * doesn't fit. */
int update_quote_arg(const char *arg, char *out, size_t n);

/* The shell's entries (update_shell.h), their pure parts:
 * the apps folder for the version folder `app` the running exe is in: its parent, a trailing '\'
 * trimmed first. 0, or -1 (empty, no parent, too long). */
int update_apps_of(const char *app, char *out, size_t n);
/* the stub's and the watcher's architecture for this PC's native machine (IMAGE_FILE_MACHINE_*,
 * compat_os's native_machine): "arm64" for ARM64, "x64" for AMD64; anything else (unknown) the
 * build's own (built_arm64) */
const char *update_arch_of(uint16_t native_machine, int built_arm64);
/* update_swap_wait's answer as core_win_swap_wait's: 0 ready, 1 no such update, 2 the old app still runs */
int update_swap_answer(int r);

/* -------------------------------------------------------------- what only Windows does */

/* Where a download's bytes go: begin(at, total) says the body starts at byte `at` of the file (0:
 * from scratch, so whatever was there goes; else the offset asked for), total the whole file's
 * size when the server said it (-1 unknown); write takes the next bytes. Each returns 0, or an
 * errno that stops the download (ENOSPC: the disk is full). cancelled: nonzero stops it. */
typedef struct {
    int (*begin)(void *ctx, int64_t at, int64_t total);
    int (*write)(void *ctx, const void *p, size_t n);
    int (*cancelled)(void *ctx);
    void *ctx;
    uint64_t deadline_ns;   /* plat_now_mono_ns() past which cancelled says so (0: none): the net
                             * bounds its own timeouts by the time left */
} update_sink;

enum {
    UPDATE_NET_OK = 0,
    UPDATE_NET_OFFLINE,     /* no connection, a timeout, TLS refused, winhttp.dll missing */
    UPDATE_NET_HTTP,        /* an HTTP status that isn't a success (*status), a redirect to http included:
                             * it isn't followed, so its own 3xx is the answer */
    UPDATE_NET_RANGE,       /* the server answered a range request with a range it wasn't asked for */
    UPDATE_NET_TOO_BIG,     /* more than max bytes */
    UPDATE_NET_SINK,        /* the sink stopped it (its errno in *err) */
    UPDATE_NET_CANCELLED,
};
typedef struct {
    /* GET url (https only) into sink, asking for bytes from `from` on (Range: bytes=from-) when
     * from > 0; at most max bytes in all. UPDATE_NET_*; *status the HTTP status; *err the sink's
     * errno. Redirects are followed to https only. */
    int (*get)(void *ctx, const char *url, int64_t from, int64_t max, const update_sink *sink, int *status, int *err);
    void *ctx;
} update_net;

/* How a range request's answer is read (pure; update_win.c applies it): the offset the body starts
 * at in the file, or -1 when it can't be used. status 200: the whole file (0). 206 with
 * "bytes A-B/T": A, which must be `from`. Anything else: -1. *total: T, or the 200's length
 * (content_length, -1 unknown), else -1. *end: the offset just past the body's last byte (B + 1;
 * the 200's length), -1 unknown: the body must end exactly there. */
int64_t update_range_start(int64_t from, int status, const char *content_range, int64_t content_length, int64_t *total, int64_t *end);

typedef struct {
    /* this exe's full path (the running app) */
    int (*self_path)(void *ctx, char *out, size_t n);
    /* starts exe with these arguments (NULL-terminated), passing it the foreground rights
     * (AllowSetForegroundWindow), not waiting: 0 or an errno */
    int (*spawn)(void *ctx, const char *exe, const char *const *args);
    /* waits until process pid, started at `started` (seconds since 1970, 0: any), has exited, at most
     * ms: 0 (exited, or no such process), ETIMEDOUT */
    int (*wait_exit)(void *ctx, uint32_t pid, int64_t started, uint32_t ms);
    /* stops every watcher running from the folder `dir` (found by full path, never by image name):
     * sets its stop event (update_watcher_event_name) and waits on its process handle, then
     * terminates one still running after that. 1 when one was running, 0 none, -1 when the
     * process list couldn't be read or was cut short, one that may be a watcher couldn't be
     * inspected (watchers_in's rule, review GH4), or a watcher runs in another session (its stop
     * event is session-local: it is left running, never ended, and its files stay; review UN7),
     * also one OpenProcess refuses but whose path from the system's list is a watcher in `dir`
     * (this account's in another logon session; update_listed_verdict, review K2).
     * On -1, or while running_under(old) isn't 0, the commit keeps the old folder; the sweep
     * takes it once nothing runs from it */
    int (*stop_watchers)(void *ctx, const char *dir);
    /* whether a process runs from an exe under the folder `dir` (any depth; by full path, as
     * stop_watchers finds them), stopping nothing: 1 yes, 0 no, -1 the list couldn't be read or
     * one that may be ours couldn't be inspected (update_scan_result). Any session: one
     * OpenProcess refuses counts by its path from the system's list (this account's in another
     * logon session; another account's own copy never matches; update_listed_verdict, K2) */
    int (*running_under)(void *ctx, const char *dir);
    /* whether a watcher of this session (one stop_watchers would stop: an rbsp-watch exe directly
     * in `dir`, by full path) runs, stopping nothing: 1 yes, 0 no, -1 the list couldn't be read, a
     * watcher's couldn't be inspected, or a matched watcher's session can't be read
     * (update_scan_result) */
    int (*watchers_in)(void *ctx, const char *dir);
    /* deletes the folder tree at path, never following a link or reparse point (one is deleted
     * itself, never what it points to): 0, ENOENT, or an errno (some of it may stay). Not used by
     * update.c (remove_tree_at below): afterexit's whole-apps delete */
    int (*remove_tree)(void *ctx, const char *path);
    /* the Apps & features entry's DisplayVersion (HKCU\...\Uninstall\RBStemsPlus), only when the
     * entry exists: 0 or an errno */
    int (*set_display_version)(void *ctx, const char *version);
    /* deletes `name` in the folder d and its tree, never following a link or reparse point (one is
     * deleted itself): 0, ENOENT, or an errno (some of it may stay). update.c's every delete of a
     * folder goes through it, d its one checked handle on the apps folder (plat_remove_tree_at) */
    int (*remove_tree_at)(void *ctx, plat_dir d, const char *name);
    void *ctx;
} update_os;

/* update_win.c's: the real ones (Windows builds only); winhttp.dll is loaded from System32 at the
 * first download */
const update_net *update_net_windows(void);
const update_os *update_os_windows(void);

/* -------------------------------------------------------------- the updater */

typedef struct {
    const char *support;          /* %LOCALAPPDATA%\rbstemsplus */
    const char *apps;             /* %LOCALAPPDATA%\Programs\RB Stems Plus */
    const char *own;              /* this app's own version: windows.app.version of the release that built it */
    const char *arch;             /* "x64" or "arm64": the machine's, the stub's build */
    const char *base;             /* the releases' base URL; NULL: UPDATE_BASE (a test build's override) */
    const sig_pubkey *keys;
    size_t nkeys;
    const update_net *net;
    const update_os *os;
    void (*log)(void *ctx, const char *line);                     /* may be NULL */
    void (*progress)(void *ctx, int64_t done, int64_t total);     /* the download's; may be NULL */
    int (*cancelled)(void *ctx);                                  /* may be NULL */
    uint32_t busy_wait_ms;        /* update_at_start's wait for a new app still running; 0: UPDATE_SWAP_WAIT_MS */
    uint32_t small_fetch_ms;      /* payload.json's and its signature's whole download, each (curl's
                                   * --max-time 60): past it, offline. 0: 60 s (tests set less) */
    void *ctx;
} update_env;

/* the payload store this app uses (payload.h): payload\ and the floor in the support folder, the
 * minimum its own version. The strings live in *buf. */
typedef struct {
    payload_files files;
    char payload_dir[UPDATE_PATH_MAX];
} update_store;
void update_store_init(const update_env *e, update_store *out);

/* fetchAsset (Payload.swift): dir SEP name holds the release asset `name` of tag (payload_version)
 * with this sha256: kept when it is there already, else downloaded (at most max bytes) into
 * name.<sha12>.part (a partial one of the same file resumed), verified and renamed; once more from
 * scratch when the bytes don't match. dir is made when missing. e->progress gets the bytes. */
enum { UPDATE_FETCH_OK = 0, UPDATE_FETCH_OFFLINE = 1, UPDATE_FETCH_NO_SPACE = 2, UPDATE_FETCH_DAMAGED = 3 };
int update_fetch_asset(const update_env *e, const char *tag, const char *dir, const char *name, const char *sha, int64_t max);

/* The installed-release record: its payload checked again (signature and rules, no floor: it
 * may be older than one), and that it names this app. PAYLOAD_OK with *out; PAYLOAD_NONE
 * when there is none or it names another app; else the refusal. When it doesn't count, the record
 * it replaced (installed-release\prev\, kept by each write) is read instead: a write stopped
 * between the signature and payload.json loses nothing. */
int update_installed(const update_env *e, payload_manifest *out);
/* The same, and the folder whose pair verified into dir (n bytes): installed-release, or its
 * prev\ when that is the one that counts; "" when none did. What stages the record (the native
 * admin's source folder) copies from there, the pair that verified (review NA5). */
int update_installed_at(const update_env *e, payload_manifest *out, char *dir, size_t n);

/* The check's outcome */
enum {
    UPDATE_CHECK_OK,          /* *out: the payload taken and saved (it has the windows block) */
    UPDATE_CHECK_NO_NEWS,     /* the payload read was valid but has no windows block */
    UPDATE_CHECK_OFFLINE,     /* nothing could be downloaded (or saved) */
    UPDATE_CHECK_UNVERIFIED,  /* what was downloaded didn't verify */
    UPDATE_CHECK_OLDER,       /* valid, but older than the floor allows (the Mac's .older: its
                               * caller treats it as unverified) */
};
/* Downloads latest's payload.json and its signature and, when it has the windows block, saves it
 * (payload_accept). The caller then passes *out to update_seen. */
int update_check(const update_env *e, payload_manifest *out);

/* What a verified payload (signed_by set; any other counts as none) means for this app
 * (manifestSeen): the shown version moves by the rule
 * (and DisplayVersion with it); one that names this very app and is newer than the
 * installed-release record replaces that record (its pair read from the saved payload\, checked
 * again first). m NULL: only the record's own rule (at start, with nothing saved). Returns 1 when m
 * offers an update (update_newer), 0 otherwise; *shown (may be NULL) the version shown now. */
int update_seen(const update_env *e, const payload_manifest *m, char shown[PAYLOAD_VERSION_MAX]);
/* the version shown now, read only (the watcher, the report, About): the record's, or own */
void update_shown_now(const update_env *e, char out[PAYLOAD_VERSION_MAX]);

/* The running exe's id, taken once at start (nw_steps_open, shell_env_init; review UH1): what the
 * apps folder's every check compares <own>\RB Stems Plus.exe with (one per update_os, the first
 * call for it wins; a regular file only). 0, or an errno (nothing taken: every later check of the
 * apps folder fails closed) */
int update_self_capture(const update_env *e);
/* UPDATE_CHECK: whether `<apps>\<own>\RB Stems Plus.exe`, through a handle on the apps folder
 * opened without following a link at it, is the running exe (the id taken at start) and `current`,
 * read through that handle, names own. 0 yes; 1 not in its own place (or no id was taken). */
int update_in_place(const update_env *e);
/* UPDATE_CHECK's code: 0 in its own place (update_in_place) and the apps folder writable; 1 not
 * in its own place (apps or own NULL included); 2 the apps folder can't be opened or written */
int update_where(const update_env *e);
/* APP_FATE's code: 0 the app can delete itself (update_where 0: the apps folder is
 * FACT_STEP_PATH, what SELF_DELETE removes), else 2. Never 1: the app is installed per account
 * (core.h) */
int update_app_fate(const update_env *e);

/* update_stage's outcome, the core's UPDATE_SWAP codes (core.h) */
enum { UPDATE_STAGED = 0, UPDATE_STAGE_OFFLINE = 1, UPDATE_STAGE_NO_SPACE = 2, UPDATE_STAGE_FAILED = 3 };
/* The old app's half (Update RB Stems Plus): the saved payload is checked again and must offer an
 * update; its zip is downloaded into update\ (a .part resumed; reused when already verified),
 * checked, unpacked into a staging folder next to the version folders, its contents checked, and
 * renamed to `<apps>\<version>\`; the payload pair is copied into update\ and `pending` written.
 * UPDATE_STAGED with the new version in `to`: the caller then calls update_launch and quits. On a
 * failure nothing the running app needs changed; *key (may be NULL) gets the UPDATE_FAIL_* the
 * log names. */
int update_stage(const update_env *e, char to[PAYLOAD_VERSION_MAX], int *key);
/* starts `<apps>\<to>\RB Stems Plus.exe --swap`: 0 or an errno (then pending is removed; the staged
 * folder is swept at the next start, and a retry downloads again) */
int update_launch(const update_env *e, const char *to);

/* --swap's first step, in the new app: `pending` must name this app as its new one; waits (at most
 * ms) for the old app to exit. 0 ready; 1 no such update (pending missing or for another app);
 * ETIMEDOUT the old app is still running. */
int update_swap_wait(const update_env *e, uint32_t ms);
/* --swap's checks failed in the new app (W3b: core_selftest, the off-screen WPF start, the OS or
 * .NET minimum): `update\result` gets the key; only once it is written do `pending` and the payload
 * pair go (else the old app reads the pending record as a failed update anyway); the old app is
 * started again through the stub (it removes this folder at its start). 0 or an errno. */
int update_swap_failed(const update_env *e, int key);

/* At every start, before the first render (the core's UPDATE_RESULT step): a pending update whose
 * new app is this one is left to update_after_render; one from this app to another is a failed
 * update: the new folder is removed, the records go, and the failure is returned (the result's key,
 * or UPDATE_FAIL_GENERIC when the new app left none); a stale one (neither app) goes. Then the
 * result file is read and removed. Returns UPDATE_FAIL_NONE when there is nothing to say.
 * While the old app that started the update still runs, or a process still runs from the new
 * app's folder (its --swap, started before this one) past busy_wait_ms, it gives UPDATE_BUSY and
 * nothing is touched: W3b's contract is that this start then exits with no UI. `current` is read
 * again after every probe: once it names the new app, nothing is undone (UPDATE_FAIL_NONE); a
 * process list that can't be read changes nothing either (UPDATE_FAIL_NONE). W3b calls
 * update_at_start, wait included, before taking the one-copy mutex. */
int update_at_start(const update_env *e);

/* After the first full snapshot on screen (W3b calls it, on a worker thread): when this app is a
 * pending update's new one, `started` is written and the pointers move (above). Otherwise, the
 * old stub kept by the last stub swap and leftover version or staging folders (neither own nor
 * current) are removed, and a stub that isn't the one installed-release names is swapped again
 * (a swap that failed in the commit, as a stub image running at that instant). 0, or an errno when
 * the commit couldn't finish, for example when the replacement watcher isn't confirmed running.
 * The records stay, and the next start finishes it. */
int update_after_render(const update_env *e);

/* -------------------------------------------------------------- the steps that need the shell (W3b)
 * The core's UPDATE_SWAP and UPDATE_RESULT (core.h "The flows' steps"), for the core DLL's native
 * step entry. UPDATE_SWAP ends with the app quitting, which only the shell can do; UPDATE_RESULT's
 * file is consumed by update_at_start, which the shell calls before the core exists. */

/* UPDATE_SWAP (updateApp's download and swap): update_stage, then update_launch, then quit(qctx):
 * the shell's quit (core_win_set_quit), the app quitting at once without asking anything, as after
 * SELF_DELETE. The step's code: 0 the new app runs its --swap and this one quits; 1 offline; 2 the
 * disk is full; 3 anything else (the download refused, the new app not started). quit NULL:
 * nothing is downloaded or started, 3 (nothing would quit this app, and the new one would wait for
 * it in vain). */
int update_step_swap(const update_env *e, void (*quit)(void *ctx), void *qctx);

/* The start's update_at_start answer, kept for the flow's UPDATE_RESULT (any thread; the shell
 * keeps it before core_new). UPDATE_BUSY and UPDATE_FAIL_NONE keep nothing to say. */
void update_result_keep(int key);
/* UPDATE_RESULT's facts: 1 with *line the result's "failed <key>" line (FACT_STEP_FLAG,
 * FACT_STEP_TEXT) when the update this start found had failed, said once (later calls give 0);
 * 0 nothing to say (*line ""). e (may be NULL): support\update\result, which update_at_start
 * leaves for this, is removed now (a start that ends before it says the result next time). */
int update_result_take(const update_env *e, const char **line);

#ifdef UPDATE_TEST_HOOKS
/* test builds only: called before each of the commit's writes; a nonzero errno stops the commit
 * there, as that write's failure would (a stop in a power cut leaves the same files) */
enum { UPDATE_STEP_STUB_NEW = 1, UPDATE_STEP_STUB_OLD, UPDATE_STEP_STUB_REPLACE, UPDATE_STEP_INSTALLED, UPDATE_STEP_WATCHER,
       UPDATE_STEP_INSTALLED_HALF, UPDATE_STEP_MARKER, UPDATE_STEP_SWEEP,
       UPDATE_STEP_APPS_OPEN,   /* apps_open's check passed (a swap after it) */
       UPDATE_STEP_STAGED };    /* update_stage: the staging folder checked, before its rename */
extern int (*update_test_step)(int step);
/* after a test's longjmp out of a step hook (the process "killed"): the apps folder's lock is new */
void update_test_killed(void);
/* the running exe's ids taken by update_self_capture forgotten (a test's new world) */
void update_test_self_reset(void);
#endif

#endif
