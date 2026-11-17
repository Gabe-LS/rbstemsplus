/* The stable start stub (app.md §5.4, §3; release.md §4 "A stable start stub"; I73, I83, I88):
 * %LOCALAPPDATA%\Programs\RB Stems Plus\RB Stems Plus.exe, the one file the Start menu shortcut,
 * the taskbar pin, the AppUserModelID and the Run value point at. It reads the `current` record
 * next to it, starts that version's app (or, with --watcher, its watcher) with the arguments it
 * was given, passes the child its foreground rights (AllowSetForegroundWindow) and exits.
 *
 *  - No version resource and nothing derived from VERSION, so its bytes change only with its code
 *    (I83, J18); a changed stub is swapped in by the updater (update.c, `.new` and rename).
 *  - The Windows build is read with RtlGetVersion (ntdll, found at run time), never from the
 *    manifest (I73): below 22621 it refuses in one line (silently with --watcher).
 *  - The AppUserModelID (aumid.h): the shortcut carries it, and Windows passes it to the stub in its
 *    startup info (STARTF_TITLEISAPPID); the stub hands it to the app, and passes the app's own ID
 *    when it got none (the Run value), so the app's window groups with the pin. The app also sets it
 *    on itself. The stub has no window and calls no SHELL32 (J21's allowlist: KERNEL32, USER32 for
 *    AllowSetForegroundWindow and the refusal, the UCRT).
 *
 *  - --after-exit (release.md T11, afterexit.h): run from its temp copy only (its folder named
 *    rbsp-uninstall-<16 hex>, no `current` beside it), it loads the core DLL copied beside it by
 *    full path and calls core_win_after_exit with its arguments; silent, its exit code the outcome.
 *
 * stub_rules.c is the pure part (UTF-16, tested on the Mac with wchar_t), stub.c the program. */
#ifndef RBSP_WIN_STUB_H
#define RBSP_WIN_STUB_H

#include <stddef.h>
#include <wchar.h>

enum { STUB_APP, STUB_WATCHER, STUB_AFTER_EXIT };
enum { STUB_VERSION_MAX = 48, STUB_MIN_BUILD = 22621 };

/* the version shape the updater writes: x.y.z (each 1-9 digits); the same as payload.c's
 * payload_is_release_version */
int stub_version_ok(const char *s);
/* `current`'s bytes: one version, then "\n" or "\r\n" or nothing (update_current_parse's rule).
 * 1 with the version (wide) in out, 0 otherwise. */
int stub_current(const char *data, size_t n, wchar_t out[STUB_VERSION_MAX]);
/* the arguments after the program's name in a command line (argv[0] as CommandLineToArgvW reads
 * it: a quoted name runs to the next quote; else to the first space or tab), spaces skipped */
const wchar_t *stub_args(const wchar_t *cmdline);
/* STUB_WATCHER when the arguments are exactly "--watcher" (spaces around allowed); STUB_AFTER_EXIT
 * when they start with "--after-exit" and a space or tab (the uninstall's temp copy, afterexit.h:
 * the rest is the core DLL's to check); else STUB_APP */
int stub_mode(const wchar_t *args);
/* whether name (the stub's own folder's) is the after-exit's temp folder's: "rbsp-uninstall-" and
 * exactly 16 lowercase hex digits (afterexit.h's AX_DIR_PREFIX, ax_is_temp_name; review UN1) */
int stub_after_exit_dir(const wchar_t *name);
/* dir\version\<the app, or rbsp-watch-<arch>.exe>: 0, or -1 when it doesn't fit */
int stub_target(const wchar_t *dir, const wchar_t *version, int mode, const wchar_t *arch, wchar_t *out, size_t cap);
/* the child's command line: the target quoted, then " " and args when there are any (as given:
 * the app parses them itself). 0, or -1 when it doesn't fit. */
int stub_cmdline(const wchar_t *target, const wchar_t *args, wchar_t *out, size_t cap);

#endif
