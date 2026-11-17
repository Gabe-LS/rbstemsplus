/* lifecycle.c, lifecycle_win.c: the app's own start-up and install records on Windows (app.md
 * §5.7, §5.9, W6a), what main.swift, Safety.swift, State.swift and Actions.swift do on the Mac:
 *  - one copy: the Local\ mutex and a findable message-only window; a second start passes the
 *    foreground rights to the first copy and signals it (§4.3, I88);
 *  - the minimum: Windows 11 22H2 (build 22621, read with RtlGetVersion, I73) and .NET Framework
 *    4.8.1 (NDP\v4\Full Release >= 533320);
 *  - startup refusals: an over-elevated start (a full token with a linked one), an administrator
 *    with UAC off (an elevated default token, I35), a service account (as the Mac refuses root);
 *  - the busy marker's start time of another process (processStartTime, State.swift:168);
 *  - the watcher's Run value and StartupApproved's "off" (Reminders are off, §5.7);
 *  - the Apps & features entry (HKCU\...\Uninstall\RBStemsPlus, release.md §4);
 *  - the Start menu shortcut with the AppUserModelID, and this process's AppUserModelID.
 *
 * Texts are the core's: every verdict here is a code. The decisions are pure C (lifecycle.c), run
 * on the Mac by tests/app_test.c; lifecycle_win.c asks Windows. The self-delete (--after-exit)
 * is release T11's. */
#ifndef RBSP_WIN_LIFECYCLE_H
#define RBSP_WIN_LIFECYCLE_H

#include "aumid.h"

#include <stddef.h>
#include <stdint.h>

#define LIFE_ONE_NAME "Local\\io.github.rbstemsplus.app"   /* the app's one-copy mutex */
#define LIFE_AUMID RBSP_AUMID_UTF8                         /* the AppUserModelID (aumid.h): the stub, the app, the shortcut */
#define LIFE_RUN_NAME "RB Stems Plus Watcher"              /* the Run value's name (§5.7) */
#define LIFE_ENTRY_KEY "RBStemsPlus"                       /* HKCU\...\Uninstall\<this> */
#define LIFE_DISPLAY_NAME "RB Stems Plus"
#define LIFE_SHORTCUT_NAME "RB Stems Plus.lnk"             /* in the Start menu's Programs folder */

enum { LIFE_PATH_MAX = 1024 };

/* ------------------------------------------------------------------ one copy */

enum { LIFE_ONE_HELD, LIFE_ONE_HANDED, LIFE_ONE_BUSY, LIFE_ONE_FAILED };
enum { LIFE_MUTEX_OWNED, LIFE_MUTEX_EXISTS, LIFE_MUTEX_TIMEOUT, LIFE_MUTEX_FAILED };
/* What the one-copy decision asks the OS (lifecycle_win.c; the tests' fakes). */
typedef struct {
    void *ctx;
    int (*create)(void *ctx);                  /* the mutex: OWNED (made, ours), EXISTS or FAILED */
    int (*wait)(void *ctx, uint32_t ms);       /* the mutex: OWNED (released or abandoned to us), TIMEOUT or FAILED */
    uint32_t (*find)(void *ctx);               /* the first copy's pid, from its window; 0 when none */
    int (*hand_off)(void *ctx, uint32_t pid);  /* foreground rights to pid, then the signal: 0 when sent */
    uint64_t (*now_ms)(void *ctx);
} life_one_ops;
/* One copy at a time (main.swift's lock, I88): HELD (this one runs; the mutex is held), HANDED (a
 * first copy was signalled and comes forward: exit quietly), BUSY (a copy holds the mutex and
 * shows no window for wait_ms: "Already open"), FAILED ("can't open"). A copy that is quitting
 * closes its window first, so the next start waits for its mutex rather than signal it. */
int life_one_copy(const life_one_ops *ops, uint32_t wait_ms);

/* The hand-off acknowledged (review Q3, WC5): the second start sends its signal with
 * SendMessageTimeoutW (SMTO_ABORTIFHUNG | SMTO_BLOCK, LIFE_ONE_SEND_MS), and the first copy's
 * window answers LIFE_ONE_ACK ('RBSP') only while it is open: life_one_window_close clears that
 * before DestroyWindow, so a copy that is quitting never counts as handed to. Anything else (the
 * send failed or timed out, a window that accepted the message but didn't acknowledge it) is no
 * hand-off: the start goes on waiting for the mutex. */
#define LIFE_ONE_ACK 0x52425350u   /* 'RBSP' */
enum { LIFE_ONE_SEND_MS = 2000 };
/* sent: SendMessageTimeoutW returned nonzero; answer: its result. 1: handed. */
int life_one_acked(int sent, uintptr_t answer);

/* ------------------------------------------------------------------ the minimum */

enum { LIFE_MIN_BUILD = 22621, LIFE_MIN_NET_RELEASE = 533320 };
enum { LIFE_OS_OK, LIFE_OS_WINDOWS_OLD, LIFE_OS_NET_OLD };
/* major, build: RtlGetVersion's (0 when unknown: refused); net: whether .NET counts (the app,
 * --swap and the watcher: 1; the stub: 0); release: NDP\v4\Full's Release, -1 when missing */
int life_os_verdict(uint32_t major, uint32_t build, int net, int64_t release);

/* ------------------------------------------------------------------ startup refusals */

enum { LIFE_TOKEN_OK, LIFE_TOKEN_ELEVATED, LIFE_TOKEN_UAC_OFF, LIFE_TOKEN_SERVICE, LIFE_TOKEN_UNKNOWN };
/* TOKEN_ELEVATION_TYPE (1 default, 2 full, 3 limited; 0 unknown), TokenElevation (1, 0; -1
 * unknown), service: the token's user is LocalSystem, LocalService or NetworkService. A standard
 * user (default, not elevated) and a limited admin token run; UNKNOWN is refused too. */
int life_token_verdict(int elevation_type, int elevated, int service);

/* ------------------------------------------------------------------ the Run value, StartupApproved */

/* the Run value's data: "\"<stub>\" --watcher". 0, or -1 (stub has a '"', is empty, too long) */
int life_run_value(const char *stub, char *out, size_t n);
/* whether a Run value runs stub with --watcher and nothing else (the program quoted or not; the
 * path's ASCII letters' case ignored, '/' read as '\') */
int life_run_names(const char *value, const char *stub);
/* whether a StartupApproved\Run value says off: its first byte odd (0x03 "disabled" as Task
 * Manager and Settings > Apps > Startup write it; 0x02 and 0x06 are on) */
int life_approved_off(const unsigned char *b, size_t n);
typedef struct {
    int present;   /* the Run value is there */
    int ours;      /* ... and runs this install's stub with --watcher */
    int off;       /* StartupApproved says off (Reminders are off), whether ours or not */
} life_run;

/* ------------------------------------------------------------------ the Apps & features entry */

typedef struct {
    const char *stub;          /* the stub's path: DisplayIcon and UninstallString's program */
    const char *location;      /* InstallLocation (%LOCALAPPDATA%\Programs\RB Stems Plus) */
    const char *version;       /* DisplayVersion: the release version the app shows */
    const char *publisher;     /* Publisher; NULL or "": none written */
    uint64_t size_bytes;       /* EstimatedSize (written in KB, rounded up) */
} life_entry;
/* "\"<stub>\" --uninstall"; 0, or -1 */
int life_uninstall_string(const char *stub, char *out, size_t n);
/* bytes as EstimatedSize's KB, rounded up, at most 0xFFFFFFFF */
uint32_t life_size_kb(uint64_t bytes);

/* ------------------------------------------------------------------ from Windows (lifecycle_win.c) */

/* One copy, for real: the mutex `name` (LIFE_ONE_NAME; the watcher may pass its own), its window
 * class named after it. On HELD, *out holds the mutex until life_one_close (or the process ends);
 * on any other answer *out is NULL.
 * The thread contract: a mutex is owned by the thread that took it, and is released (abandoned)
 * when that thread ends. So life_one_take, life_one_window, life_one_window_close and
 * life_one_close all run on one thread that lives as long as the app: the shell's UI thread. A
 * life_one_close or a window close from any other thread does nothing and returns at once: no
 * handle closed, nothing freed; the window and the mutex stay until the owning thread or the
 * process ends. */
typedef struct life_one life_one;
int life_one_take(const char *name, uint32_t wait_ms, life_one **out);
/* the findable window, made on the calling thread (the shell's UI thread: its dispatcher pumps
 * the window's messages). on_signal(ctx) runs there when a second start signals: the shell
 * brings its window forward and sends "activated" and "check_request" (main.swift:169-172).
 * 0, or an errno. */
int life_one_window(life_one *o, void (*on_signal)(void *ctx), void *ctx);
/* the window goes (quitting starts: later starts wait for the mutex); the mutex stays */
void life_one_window_close(life_one *o);
void life_one_close(life_one *o);

/* RtlGetVersion's major, minor, build: 0, or -1 */
int life_os_version(uint32_t *major, uint32_t *minor, uint32_t *build);
/* HKLM\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full Release, -1 when missing */
int64_t life_net_release(void);
/* life_os_verdict on this PC */
int life_os_check(int net);
/* life_token_verdict on this process's token */
int life_token_check(void);

/* When process pid started, in whole seconds since 1970, plat_process_start's clock (the
 * marker's second line): -1 when there is no such process (or it has exited), -2 when it
 * couldn't be asked. Another account's process is found through the system's process list. */
int64_t life_process_start(uint32_t pid);

/* The Run value and StartupApproved for stub (HKCU): 0 with *out, or an errno */
int life_run_read(const char *stub, life_run *out);
/* writes the Run value for stub (the repair); StartupApproved is left as the user set it */
int life_run_write(const char *stub);
/* removes the Run value and its StartupApproved value (uninstall); 0 when gone, or an errno */
int life_run_remove(void);

/* writes the Apps & features entry; removes it. 0, or an errno */
int life_entry_write(const life_entry *e);
int life_entry_remove(void);

/* this process's AppUserModelID (the WPF app's first lines; the stub sets its own): 0 or -1 */
int life_set_aumid(const char *aumid);
/* the Start menu shortcut's path: FOLDERID_Programs\LIFE_SHORTCUT_NAME. 0, or -1 */
int life_shortcut_path(char *out, size_t n);
/* writes a shortcut to target carrying aumid, under a temp name next to lnk, then renamed over
 * it; 0, or an errno (EIO: a COM call failed) */
int life_shortcut_write(const char *lnk, const char *target, const char *aumid);
/* removes the shortcut; 0 when gone, or an errno */
int life_shortcut_remove(const char *lnk);
/* removes the records Windows made for aumid in HKCU (Notifications\Settings\<aumid>); 0 when
 * gone, or an errno */
int life_records_remove(const char *aumid);

#endif
