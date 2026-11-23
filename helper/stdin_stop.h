/* The Windows helper's Stop (helper.md §1, T15). Windows has no signals between processes: the app
 * stops a run by closing the helper's stdin (EOF), or by a line "stop", and the run then ends as
 * SIGTERM ends it on the Mac: after the chunk it is on, its kept set and manifest written, its
 * last line printed. The watcher is started only on the run path, where prepare.c installs its
 * signal handlers; any other call (an estimate, --playlists, --tracks) is ended by the app
 * terminating it (TerminateJobObject, app.md §5.6), as the Mac's SIGTERM ends it.
 *
 * Pause/Resume is the app suspending and resuming the whole process (NtSuspendProcess /
 * NtResumeProcess), as SIGSTOP/SIGCONT do. A suspended helper can't see the EOF, so the app's stop
 * of a paused run is: close stdin, resume it once, then wait for the final lines and the exit
 * before its Job may close (win/c/helper_proc.c's helper_ops). So a stdin that is NUL, or a pipe
 * whose other end is closed, stops a run at once: the app always passes a pipe it keeps open.
 *
 * The line rule (stop_scan_feed) is pure and compiled on both OSes, so the Mac tests it; the
 * watcher is Windows only, and the Mac helper links neither (the Mac app starts it with stdin =
 * /dev/null, and SIGTERM stops it). */
#ifndef RBSP_STDIN_STOP_H
#define RBSP_STDIN_STOP_H
#include <stddef.h>

typedef struct {
    unsigned char line[8];              /* the line so far (only its first bytes matter) */
    size_t len;
    int over;                           /* longer than line[]: not "stop" */
} stop_scan;

/* n bytes of stdin into *s (zeroed to start); 1 once a line that is exactly "stop" (a CR before
 * its LF allowed) has ended, else 0. Other lines are ignored. */
int stop_scan_feed(stop_scan *s, const void *buf, size_t n);

#if defined(_WIN32)
/* starts the thread that calls on_stop once: at stdin's EOF, a read error or a "stop" line. 0, or
 * -1 when there is no stdin to watch (the process got no handle: the end of the process is then
 * the only stop) or no thread could start */
int stop_watch_start(void (*on_stop)(void));
#endif

#endif
