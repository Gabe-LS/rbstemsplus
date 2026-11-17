/* helper_proc_win.c: the prepare helper's process on Windows (helper_proc.h's rules). For the
 * host's helper_spawn and helper_signal (core.h). */
#ifndef RBSP_WIN_HELPER_PROC_WIN_H
#define RBSP_WIN_HELPER_PROC_WIN_H

#include "helper_proc.h"

typedef struct helper_proc helper_proc;
typedef void (*helper_line_fn)(void *ctx, const char *line, size_t n);
/* code: its exit code; err, err_len: the tail of its stderr (helper_tail: at most 4 KiB, "" none) */
typedef void (*helper_ended_fn)(void *ctx, int code, const char *err, size_t err_len);
typedef void (*helper_log_fn)(void *ctx, const char *line);

/* Starts exe (UTF-8, absolute) with argv[0..argc-1] (the core's; exe isn't among them), run: a
 * run (stopped by stdin's EOF) or another call (terminated). line, log and ended come from its
 * reader thread: every line (helper_lines: whole, up to HELPER_LINE_MAX bytes), then ended
 * exactly once, after stdout's end, stderr's end and the process's exit; log (may be NULL) once
 * when a line longer than that was discarded. stderr is its own pipe, drained by its own thread.
 * If that thread can't start, the helper is terminated with its Job and the spawn fails. 0 with *out,
 * or -1 (not started: no ended). */
int helper_proc_spawn(const char *exe, const char *const *argv, int argc, int run, helper_line_fn line, helper_ended_fn ended, helper_log_fn log,
                      void *ctx, helper_proc **out);
/* HELPER_SIG_PAUSE, _RESUME or _STOP, at once (any thread) */
void helper_proc_signal(helper_proc *p, int sig);
/* After ended, or to give up: a helper that hasn't ended is terminated with its Job first; then
 * the reader is waited for (ended fires) and the Job closed */
void helper_proc_close(helper_proc *p);

#endif
