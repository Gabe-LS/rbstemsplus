/* notify_win.c: the app's notification (CORE_STEP_NOTIFY: notify(title:body:), a notification and
 * never a dialog), as the watcher shows its own (watch_win.c, app.md §5.7): Shell_NotifyIconW with
 * NIF_INFO, which Windows 11 shows as a notification. The core DLL has no window of its own, so the
 * first call starts one thread for the process that owns a message-only window and pumps it; each
 * notification is posted to it. The icon goes when the notification times out, is closed or is
 * clicked. SHELL32 and USER32 only (the DLL's allowlist). Unverified until the VM run. */
#ifndef RBSP_WIN_NOTIFY_H
#define RBSP_WIN_NOTIFY_H

/* title and body are UTF-8 (cut to the notification's 63 and 255 characters). Any thread. 0 when
 * posted to the window, -1 when it couldn't be (no window, no memory) */
int notify_show(const char *title, const char *body);

#endif
