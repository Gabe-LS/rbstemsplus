/* admin_win.c: admin.h's elevate and random on Windows (the core DLL). */
#ifndef RBSP_WIN_ADMIN_WIN_H
#define RBSP_WIN_ADMIN_WIN_H

#include "admin.h"

typedef struct {
    void *owner;              /* the app's main window (an HWND): UAC's prompt in front of it */
    unsigned long last_err;   /* Win32 startup error when one is available, else 0 */
} admin_win;

/* admin_elevate_fn; ctx: an admin_win */
int admin_win_elevate(void *ctx, const char *exe, const char *params, const char *pipe, void (*line)(void *lctx, const char *s, size_t n), void *lctx,
                      void (*started)(void *sctx), void *sctx, int *code);
/* admin_client's random: 16 bytes from RtlGenRandom */
void admin_win_random(void *ctx, unsigned char out[16]);

#endif
