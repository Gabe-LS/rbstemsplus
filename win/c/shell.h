/* shell.c: CriticalWork (Shell.swift:38-69) on Windows, over common/plat.h's mutex and condition
 * (app.md §1's shell.c, §5.9). The steps quitting waits for: an admin action running, or our
 * model being copied into or renamed in rekordbox's folder. The quit flow waits off the UI thread
 * (core.h's quit flow, ≤60 s); the logoff's WM_ENDSESSION waits on the UI thread, bounded. The
 * clean child environment of §1's shell.c is W4's (admin.c's helper spawn). */
#ifndef RBSP_WIN_SHELL_H
#define RBSP_WIN_SHELL_H

#include "plat.h"

#include <stdint.h>

typedef struct {
    plat_mutex lock;
    plat_cond idle;
    int count;
    int closed;   /* the quit is decided: no new step is admitted (critical_close; one-way) */
} critical_work;

/* before any use; never destroyed (one per process, as the Mac's `critical`) */
void critical_init(critical_work *w);
/* a step starts / ends (nested and concurrent steps count) */
void critical_begin(critical_work *w);
void critical_end(critical_work *w);
/* whether a step runs now */
int critical_running(critical_work *w);
/* Admission (review WC1/WC2, with the model steps): the close or the quit is decided (the shell's
 * CloseGate, before it lets the window go), so no new step may start; one already running still
 * counts, and so does a plain critical_begin (the elevated worker past its prompt). One-way. */
void critical_close(critical_work *w);
/* 1: a new step may start (not closed); 0: closed */
int critical_admit(critical_work *w);
/* a new step starts unless closed: 1 begun (critical_end it), 0 refused (closed; nothing begun) */
int critical_try_begin(critical_work *w);
/* whenIdle: returns once no step runs (at once if none does): 1; or after about timeout_ms at the
 * latest: 0, so a logoff or a shutdown is never held up longer. Any thread but a step's own. */
int critical_wait_idle(critical_work *w, uint32_t timeout_ms);

/* The process's one CriticalWork (the Mac's `critical`), initialized at its first use, any thread:
 * the core DLL's steps count in it (the elevated worker's run: native_admin.h's critical), and
 * core_win.h's core_win_critical_running and core_win_critical_wait read it for the shell. */
critical_work *critical_process(void);

#endif
