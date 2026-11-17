/* The app's AppUserModelID, defined once (app.md §5.4): the Start menu shortcut carries it (W6a),
 * the WPF app sets it on itself before its first window (SetCurrentProcessExplicitAppUserModelID,
 * W6a/W3b), and every start our code makes passes it in the startup info (STARTF_TITLEISAPPID):
 * the stub (stub.c) when Windows didn't hand it one, and the updater's starts (update_win.c). So
 * the app's window groups with the taskbar pin after every update. */
#ifndef RBSP_WIN_AUMID_H
#define RBSP_WIN_AUMID_H

/* UTF-8 for the C code that takes UTF-8 (lifecycle.h's shortcut and records), wide for the Win32
 * calls: one spelling, so they can't drift */
#define RBSP_AUMID_UTF8 "io.github.rbstemsplus.app"
#define RBSP_AUMID L"" RBSP_AUMID_UTF8

#endif
