/* helper_proc.c: the app's control of the prepare helper (app.md §5.6, W4; helper.md's contract):
 * the clean environment, and what Pause, Resume and Stop do to it, as pure rules; helper_proc_win.c
 * does it (CreateProcessW in a Job object that kills on close, stdin, stdout and stderr pipes,
 * NtSuspendProcess/NtResumeProcess, TerminateJobObject). The core builds the argv and parses the
 * lines (host->helper_spawn, host->helper_signal).
 *
 *  - Pause and Resume: NtSuspendProcess and NtResumeProcess, at once, as SIGSTOP and SIGCONT do.
 *  - Stop of a run: its stdin closed (EOF is the stop; it writes the kept set, as SIGTERM does),
 *    then, if it is paused, NtResumeProcess once (a suspended helper can't see EOF); then its
 *    last lines and its exit are waited for, before the Job may close.
 *  - Stop of any other call (an estimate...; I63): TerminateJobObject, at once, as the Mac's
 *    SIGTERM does. */
#ifndef RBSP_WIN_HELPER_PROC_H
#define RBSP_WIN_HELPER_PROC_H

#include <stddef.h>
#include <stdint.h>

/* core.h's core_helper_sig values */
enum { HELPER_SIG_PAUSE = 0, HELPER_SIG_RESUME = 1, HELPER_SIG_STOP = 2 };

/* what to do, in this order */
enum { HELPER_OP_SUSPEND = 1, HELPER_OP_CLOSE_STDIN = 2, HELPER_OP_RESUME = 4, HELPER_OP_TERMINATE = 8 };

typedef struct {
    int run;          /* a run (stdin's EOF stops it), or another call (terminated) */
    int paused;       /* suspended by us */
    int stdin_open;
    int ended;        /* its exit was seen: nothing more is done to it */
} helper_state;

void helper_state_init(helper_state *s, int run);
/* the ops a signal takes now (HELPER_OP_* bits, applied in the order of their values), the state
 * updated: Pause suspends a helper not paused; Resume resumes a paused one; Stop closes a run's
 * stdin (once) and resumes it if paused, and terminates any other call */
unsigned helper_ops(helper_state *s, int sig);

/* The helper's environment block: SystemRoot, TEMP and TMP only (Shell.swift's clean
 * environment; TEMP and TMP = LocalAppData\Temp), sorted as Windows wants it, UTF-16, each
 * "NAME=value" NUL-terminated and one more NUL at the end. system_root and temp: UTF-8. The
 * units written (the last NUL included), or -1 (invalid UTF-8, '=' or a NUL in a value, or it
 * doesn't fit in cap units). */
long helper_env_block(const char *system_root, const char *temp, uint16_t *out, size_t cap);

/* The helper's paths under Program Files, the core DLL's own whatever the core's argv says (review
 * PF3: the shell never computes a Program Files path, so the config names none): argv's argc
 * entries into out (argc slots), walked with prepare.c's grammar: an option that takes a value
 * has it copied as it is; the value of "--ort" is replaced by ort (our root's ort\onnxruntime.dll)
 * and that of "--rekordbox" by rekordbox (the current rekordbox version folder; NULL: ""); the
 * first argument that isn't an option ends the options, and it and the rest are copied as they
 * are. *has_rb (may be NULL): "--rekordbox" was given as an option. 0; -1 when "--ort" or
 * "--rekordbox" is argv's last entry (no value; out is then incomplete). */
int helper_program_args(const char *const *argv, int argc, const char *ort, const char *rekordbox, const char **out, int *has_rb);
/* Whether the call is a run (Prepare's run: its stdin's EOF stops it), by the same walk: "--keep"
 * given as an option, and neither "--estimate" nor "--quick". */
int helper_is_run(const char *const *argv, int argc);
/* Whether a call isn't started (review PF4): it names "--rekordbox" (has_rb), no verified current
 * rekordbox folder is known (rekordbox ""), and it is a run (is_run). An estimate is started with
 * "" (it loads nothing, PF2) */
int helper_run_refused(int has_rb, const char *rekordbox, int is_run);

/* The helper's stdout split into lines, as the Mac's reader splits them: raw bytes (the core
 * repairs UTF-8), split at '\n', one '\r' before it dropped, the last unterminated line given at
 * the end. A line of any length up to max bytes (HELPER_LINE_MAX: a track's line with a long or
 * non-Latin path is about 1-2 KB; the cap only bounds a runaway) is given whole; the buffer
 * doubles as it grows. A longer line is discarded up to its '\n', and too_long is called once per
 * splitter (the first such line). */
enum { HELPER_LINE_MAX = 4 << 20 };
typedef void (*helper_line_out)(void *ctx, const char *line, size_t len);
typedef struct {
    char *buf;
    size_t len, cap, max;
    int discarding;   /* the line now coming passed max: dropped up to its '\n' */
    int warned;       /* too_long was called */
    int oom;          /* the buffer couldn't grow: the line is discarded as a too-long one */
} helper_lines;
/* max 0: HELPER_LINE_MAX */
void helper_lines_init(helper_lines *l, size_t max);
/* n bytes of stdout (at_end: none follow); each whole line to out, a discarded one to too_long
 * (once; may be NULL) */
void helper_lines_feed(helper_lines *l, const char *p, size_t n, int at_end, helper_line_out out, void (*too_long)(void *ctx), void *ctx);
void helper_lines_free(helper_lines *l);

/* The tail of the helper's stderr (core_helper_ended's err: runPrepareHelper's, where the helper
 * writes why it failed): its last HELPER_ERR_TAIL bytes. When earlier bytes were dropped, the tail
 * starts at the first byte that isn't a UTF-8 continuation byte. */
enum { HELPER_ERR_TAIL = 4096 };
typedef struct {
    char buf[HELPER_ERR_TAIL];
    size_t len;
    int cut;   /* bytes before buf were dropped */
} helper_tail;
void helper_tail_init(helper_tail *t);
void helper_tail_feed(helper_tail *t, const char *p, size_t n);
const char *helper_tail_get(const helper_tail *t, size_t *len);

#endif
