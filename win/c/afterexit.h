/* afterexit.c, afterexit_win.c: the complete uninstall's last part on Windows, the app's own files
 * deleted after it has quit (release.md §4 "The uninstaller runs from a temp copy", T11; app.md
 * §5.9 "Self-delete", I21, I78, R4.I82). The Mac's afterExit of selfDeleteScript.
 *
 * The flow's SELF_DELETE (core_native_steps.c), once the uninstall has removed Stems Cache, the
 * model and the support folder, and only when the app itself goes (APP_FATE's apps folder):
 *  1. afterexit_launch: a new, randomly named folder in %TEMP% (rbsp-uninstall-<16 hex>) gets a
 *     copy of the start stub of the machine's architecture and the core DLL of the same
 *     architecture, from the app's own version folder (stub\<arch>\, <arch>\): C only, no CLR,
 *     nothing in it is the app's folder. The copy is started with "--after-exit", an inherited
 *     handle on this process (SYNCHRONIZE only: no pid that could be reused), and
 *     %LOCALAPPDATA% (this account's own, plat_home: anything else is refused, review UN11); its
 *     working folder is %TEMP%, never the app's;
 *  2. the app quits (the shell's uninstall quit, asking nothing).
 * The copy (stub.c's --after-exit; refused unless its folder's name is AX_DIR_PREFIX and 16
 * lowercase hex digits and no `current` is beside it) loads the core DLL beside it by full path and
 * calls core_win_after_exit, which runs ax_run. Before anything, even the temp folder's cleanup
 * (review UN1, UN11): this copy's folder must be a plain folder (no reparse point) named as above
 * directly in GetTempPathW's folder (both long paths, case ignored), and the %LOCALAPPDATA% given
 * must be plat_home()'s; else AX_BAD_ARGS. Then:
 *  a. waits for the app to exit, on the inherited handle, at most AX_PARENT_WAIT_MS; still
 *     running: nothing is deleted or removed;
 *  b. the Run value and its StartupApproved value go first (a logon meanwhile starts nothing);
 *     then it stops every watcher running from a version folder (update_os's stop_watchers: its
 *     stop event, then its process handle, found by full path, never by image name), then waits
 *     while any process runs from an exe under the apps folder (by full path: another copy of the
 *     app, a --swap), at most AX_OTHERS_WAIT_MS; a process list that can't be read is waited on as
 *     one that runs (review UN4);
 *  c. only once nothing runs from the apps folder (confirmed): deletes the CLR's UsageLogs entry
 *     of our exe (%LOCALAPPDATA%\Microsoft\CLR_v4.0\UsageLogs\RB Stems Plus.exe.log, and
 *     CLR_v4.0_32's): each folder opened from %LOCALAPPDATA%'s handle, never through a link, the
 *     entry a plain file only (review UN3); not confirmed: kept, with d. and e.,
 *     AX_LEFT, and ax_os.kept_dialog tells the user to restart Windows and uninstall again (the
 *     program's path in the log only; owner 2026-11-16), before f.;
 *  d. deletes the apps folder %LOCALAPPDATA%\Programs\RB Stems Plus (the stub, `current`, every
 *     version folder) by handle, never following a link (plat_remove_tree): after the 60 s, what
 *     can go goes (app.md, as on the Mac); the apps folder is never handed to cmd or RunOnce,
 *     which could delete a later reinstall;
 *  e. the other records last (a watcher's toast meanwhile can't make them again): the Apps &
 *     features entry, the Start menu shortcut, the notification settings of our AppUserModelID
 *     (lifecycle.h); kept when d. failed (not ENOENT) and the app still starts (the stub,
 *     `current` and current's <version>\RB Stems Plus.exe there, through a fresh apps handle), so
 *     the uninstall can be run again from Apps & features: AX_LEFT (review GH6);
 *  f. its own temp folder can't be deleted while its exe runs: a cmd.exe from System32 started
 *     with no window, its working folder %TEMP%, deletes it once this process has ended (it tries
 *     for about a minute): it sees only the folder's relative name, deletes the two files by name
 *     and the folder without /s, and calls PING.EXE by its System32 path (reviews UN1, UN6, UN10,
 *     UN14). If cmd can't be started, an HKCU RunOnce value deletes it at the next logon instead,
 *     its registration read back and at most 259 characters (the fallback the trace audit lists
 *     as a disclosed record; review UN12). No MOVEFILE_DELAY_UNTIL_REBOOT (an unelevated process
 *     can't register it). A %TEMP% path holding " % ! ^ gets no RunOnce fallback (its cmd line
 *     would read them): the folder then stays, logged.
 * The folder to delete is always <LOCALAPPDATA>\Programs\RB Stems Plus, from the %LOCALAPPDATA%
 * given, which must be absolute, plain and this account's own (plat_home).
 *
 * afterexit.c is the pure part (the command line, the paths, the order of ax_run over an ops
 * table), tested on the Mac (tests/afterexit_test.c); afterexit_win.c is Windows' (the launch, the
 * ops, the DLL's export). */
#ifndef RBSP_WIN_AFTEREXIT_H
#define RBSP_WIN_AFTEREXIT_H

#include <stddef.h>
#include <stdint.h>

#define AX_ARG "--after-exit"
#define AX_DIR_PREFIX "rbsp-uninstall-"
#define AX_APPS_REL "Programs" AX_SEP "RB Stems Plus"
#define AX_EXE "RB Stems Plus.exe"
#define AX_DLL "rbsp-core.dll"
#define AX_RUNONCE_NAME "RB Stems Plus cleanup"   /* a prefix: each value is "<it> <16 hex>" (ax_runonce_name) */
#ifdef _WIN32
#  define AX_SEP "\\"
#else
#  define AX_SEP "/"
#endif

enum {
    AX_PATH_MAX = 1024,
    AX_LINE_MAX = 4096,
    AX_PARENT_WAIT_MS = 120000,   /* the app's quit: CloseGate waits up to 60 s for CriticalWork */
    AX_OTHERS_WAIT_MS = 60000,    /* other processes from the apps folder, after the watchers stopped */
    AX_POLL_MS = 250,
};

/* ax_run's outcomes */
enum {
    AX_DONE = 0,          /* everything deleted (or wasn't there) */
    AX_BAD_ARGS = 1,      /* nothing done */
    AX_PARENT_RUNS = 2,   /* the app didn't exit: nothing deleted */
    AX_LEFT = 3,          /* deleted what could be; something stayed (a process still ran, a file was held) */
};

typedef struct {
    uint64_t parent;            /* the app's process handle, inherited (its value) */
    char local[AX_PATH_MAX];    /* %LOCALAPPDATA% */
    char apps[AX_PATH_MAX];     /* local SEP AX_APPS_REL: what goes */
} ax_args;

/* The copy's command line: "\"<exe>\" --after-exit <parent, hex> \"<local>\"". 0, or -1 (a '"'
 * in a path, local not absolute, too long). */
int ax_cmdline(const char *exe, uint64_t parent, const char *local, char *out, size_t n);
/* The arguments after the program's name ("--after-exit <hex> \"<local>\"", spaces around them
 * allowed), into *out with apps made from local. 0, or -1: anything else, a handle of 0, a local
 * that isn't absolute (a drive letter and SEP, or \\server\share on Windows; '/' on the Mac) or
 * holds a '..' part, too long. */
int ax_parse(const char *args, ax_args *out);
/* %LOCALAPPDATA% from the apps folder (<local> SEP Programs SEP RB Stems Plus, the last two parts'
 * ASCII case ignored, SEP or '/' read as one): 0, or -1 when apps isn't that */
int ax_local_of(const char *apps, char *out, size_t n);
/* whether path (a process's exe, as QueryFullProcessImageName gives it; a "\\?\" prefix ignored)
 * is under dir, at any depth: ASCII case ignored, '/' read as '\', dir's own trailing separators
 * ignored; dir itself or "<dir>x" never is */
int ax_under(const char *path, const char *dir);
/* whether two folders are the same by name: ASCII case ignored, '/' read as '\', trailing
 * separators ignored (the %LOCALAPPDATA% given against plat_home's) */
int ax_same_dir(const char *a, const char *b);
/* the temp folder's name: AX_DIR_PREFIX and 16 lowercase hex digits of rnd. 0, or -1 */
int ax_temp_name(const unsigned char rnd[8], char *out, size_t n);
/* whether name is such a name: AX_DIR_PREFIX and exactly 16 lowercase hex digits, nothing more */
int ax_is_temp_name(const char *name);
/* cmd.exe's arguments that delete the temp folder `name` (relative: cmd's working folder is its
 * parent, %TEMP%) once this process has ended, sys being GetSystemDirectory's folder:
 * "/d /e:on /v:off /q /s /c \"for /l %i in (1,1,60) do @(del /f /q \"<name>\\RB Stems Plus.exe\"
 * \"<name>\\rbsp-core.dll\" 2>nul & rd /q \"<name>\" 2>nul & (if not exist \"<name>\\\" exit /b 0) &
 * \"<sys>\\PING.EXE\" -n 2 -w 1000 127.0.0.1 >nul)\"". The IF is grouped: cmd reads `if C A & B` as
 * the IF's own A & B, so ungrouped the PING would run only once the folder is gone (review UQ1).
 * /e:on: `for /l` even with command extensions
 * off in the account's settings; /v:off: no '!' expansion. 0, or -1 when name isn't
 * ax_is_temp_name's, sys holds a character cmd would read in it (" % ! ^, a control character)
 * or isn't absolute, or it doesn't fit */
int ax_cleanup_args(const char *sys, const char *name, char *out, size_t n);
/* whether a RunOnce value's data (UTF-8) runs at all: at most 259 UTF-16 units (Windows runs no
 * longer command; review UN12, UQ5). 1, or 0 (too long, or not strict UTF-8: no overlong form, no
 * surrogate, nothing past U+10FFFF, as MultiByteToWideChar reads it; review U52) */
int ax_runonce_fits(const char *utf8);
/* whether cmd.exe can take parent (UTF-8) as its working folder: 0 for a UNC path ("\\..." or
 * "//...": cmd would run in %SystemRoot% instead, review UQ4, UQ5), an empty one or NULL; else 1 */
int ax_cmd_cwd_ok(const char *parent8);
/* the RunOnce value's name for the temp folder `name` (ax_is_temp_name's): AX_RUNONCE_NAME, a
 * space and the folder's 16 hex digits, so a second uninstall before the next logon adds its own
 * value instead of replacing a pending one (review UN13). 0, or -1 */
int ax_runonce_name(const char *name, char *out, size_t n);
/* the RunOnce value's data: "\"<cmd>\" /d /q /s /c \"rd /s /q \"<dir>\"\"". 0, or -1 when dir holds a
 * character cmd would read in it (as above) or isn't absolute, cmd holds a '"', or it doesn't fit */
int ax_runonce_data(const char *cmd, const char *dir, char *out, size_t n);

/* What ax_run asks the OS (afterexit_win.c; the tests' fakes) */
typedef struct {
    /* whether this process runs from its own temp folder: a plain folder (no reparse point) named
     * ax_is_temp_name, directly in GetTempPathW's folder: 1, else 0 */
    int (*self_ok)(void *ctx);
    /* this account's %LOCALAPPDATA% (plat_home): 0, or -1 */
    int (*home)(void *ctx, char *out, size_t n);
    /* waits for the app (the inherited handle) to exit, at most ms: 0 exited, else nonzero */
    int (*wait_parent)(void *ctx, uint64_t parent, uint32_t ms);
    /* update_os's: stops the watchers running from dir (1 one was, 0 none, -1 the list couldn't be read) */
    int (*stop_watchers)(void *ctx, const char *dir);
    /* update_os's: whether a process runs from an exe under dir (1, 0, -1 the list couldn't be read
     * or one with one of our names couldn't be inspected: a refused open counts only for an error
     * other than ERROR_ACCESS_DENIED or ERROR_INVALID_PARAMETER, update_open_skips) */
    int (*running_under)(void *ctx, const char *dir);
    /* update_os's: deletes the tree at path, never following a link: 0, ENOENT, or an errno */
    int (*remove_tree)(void *ctx, const char *path);
    /* the temp folder deleted after this process: 0 a cmd.exe waits for it, 1 a RunOnce value will
     * at the next logon (read back), -1 neither */
    int (*clean_later)(void *ctx);
    /* the Run value and its StartupApproved value (lifecycle.h's life_run_remove), first: 0, or an errno */
    int (*remove_run)(void *ctx);
    /* the other Windows records of the install (lifecycle.h: the Apps & features entry, the Start
     * menu shortcut, the notification settings of our AppUserModelID), last: 0 all gone, else how
     * many couldn't be removed */
    int (*remove_records)(void *ctx);
    uint64_t (*now_ms)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    void (*log)(void *ctx, const char *line);   /* may be NULL */
    /* the temp folder set to go at the next logon (HKCU RunOnce, its own value, read back), just
     * before kept_dialog, so a restart with the dialog still open still removes it: 1 registered,
     * else not. The value stays: if clean_later's cmd.exe deletes the folder first, its rd of a
     * missing folder does nothing at the logon. May be NULL */
    int (*clean_at_logon)(void *ctx);
    /* the one dialog the after-exit shows (T_WIN_UNINSTALL_KEPT: restart Windows, then uninstall
     * again; no program named), in the user's session, only when b.'s wait kept the app files and
     * the Apps & features entry (a program still ran from the apps folder, or the list stayed
     * unknown); returns once it is closed. May be NULL (owner 2026-11-16) */
    void (*kept_dialog)(void *ctx);
    void *ctx;
} ax_os;

/* the after-exit's order (afterexit.h's checks and a-f): AX_DONE, AX_PARENT_RUNS, AX_LEFT or
 * AX_BAD_ARGS. The UsageLogs files are deleted through common/plat.h under a->local. */
int ax_run(const ax_os *os, const ax_args *a);

/* ------------------------------------------------------------------ Windows (afterexit_win.c) */

/* SELF_DELETE's launch (step 2): the temp folder, the stub and the core DLL of arch ("x64",
 * "arm64") copied from apps SEP own (stub\<arch>\RB Stems Plus.exe, <arch>\rbsp-core.dll), the copy
 * started with this process's handle; the folder removed again when it couldn't start. 0, or an
 * errno: EINVAL when apps isn't <plat_home>\Programs\RB Stems Plus (review UN11). */
int afterexit_launch(const char *apps, const char *own, const char *arch);

/* afterexit_self_win.c (KERNEL32 only; also in the Windows test exe): whether self_dir, the copy's
 * folder without a trailing '\', is a plain folder (no reparse point) whose parent (everything up to
 * its last '\', cut here, review U53) is GetTempPathW's folder (both long paths, case ignored). The
 * folder's name is ax_is_temp_name's to check. 1, or 0 */
#ifdef _WIN32
#  include <wchar.h>
int ax_win_dir_ok(const wchar_t *self_dir);
#endif

#endif
