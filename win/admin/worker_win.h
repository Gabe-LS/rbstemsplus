/* worker_win.c: admin_os on Windows, and the worker's start-up checks (main.c). Windows only. */
#ifndef RBSP_WIN_ADMIN_WORKER_WIN_H
#define RBSP_WIN_ADMIN_WORKER_WIN_H

#include "worker.h"

typedef struct {
    admin_os os;
    void *pipe;   /* the app's pipe (a HANDLE), NULL when gone */
} worker_win;

/* fills ww->os with the Windows calls (its ctx is ww) */
void worker_win_init(worker_win *ww);
/* Opens the app's pipe for writing, identification only (SECURITY_SQOS_PRESENT |
 * SECURITY_IDENTIFICATION), and checks with GetNamedPipeServerProcessId that its server is this
 * process's parent (no re-exec chain: the app that ran it). 0, or -1 (then nothing is said). */
int worker_win_connect(worker_win *ww, const char *pipe);
/* whether this process's token is elevated */
int worker_win_elevated(void);
/* the token's default owner set to Administrators, so whatever the worker makes is owned by
 * them whatever the policy and the elevation type (bridge.md §2's saved set): 0 or -1 */
int worker_win_owner_admins(void);

#endif
