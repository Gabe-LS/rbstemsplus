/* report.c, report_win.c: what Create Report collects on Windows beyond our own files (app.md
 * §5.10, W6a), Report.swift's buildReport twins:
 *  - the Application event log's errors about rekordbox and our programs, the last 30 minutes
 *    (macOS's `log show --last 30m` of rekordbox and the app), through wevtapi.dll, loaded from
 *    System32 at the first call (not a static import, app.md §7);
 *  - from WER's ReportArchive (reports sent) and ReportQueue (reports waiting to be sent) only
 *    the text Report.wer of rekordbox's and our programs' crashes and hangs, the last 7 days, the
 *    newest 5 of both together (the Mac's DiagnosticReports rule); never a crash dump: only files
 *    named exactly Report.wer are opened;
 *  - the Desktop folder (FOLDERID_Desktop, often OneDrive's), where the zip goes.
 * The masking is the core's; this file only drops the event XML's account SID and computer name.
 * The zip is the shell's (System.IO.Compression). */
#ifndef RBSP_WIN_REPORT_H
#define RBSP_WIN_REPORT_H

#include <stddef.h>
#include <stdint.h>

enum { REPORT_PATH_MAX = 1024, REPORT_EVENT_MINUTES = 30, REPORT_EVENT_LINES = 3000, REPORT_WER_DAYS = 7, REPORT_WER_MAX = 5,
       REPORT_TEXT_MAX = 2000000, REPORT_WER_ROOTS = 4 };

/* -- pure (report.c) */

/* the event query's XPath: errors and criticals (Level 1, 2) of the last `minutes` minutes. 0,
 * or -1 when it doesn't fit */
int report_event_query(uint32_t minutes, char *out, size_t n);
/* whether an event's XML (cleaned first: report_event_clean) is about rekordbox or our programs:
 * an image name (rekordbox.exe, RB Stems Plus.exe, rbsp-<name>.exe or .dll; case ignored, not
 * inside a longer word) inside its EventData or its Provider's Name. A path that only mentions
 * rekordbox (a folder, a library file) or a PC named so doesn't count. */
int report_event_wanted(const char *text, size_t n);
/* drops what names the account and the PC from an event's XML (n bytes, a NUL after them), in
 * place: every UserID attribute (the Security element's SID) and the Computer element's text.
 * Returns the new length; the text stays NUL-terminated. */
size_t report_event_clean(char *xml, size_t n);
/* whether a WER report folder is one Create Report takes: "AppCrash_" or "AppHang_", then
 * rekordbox's or our programs' name ("rekordbox", "RB Stems Plus", "rbsp-"), case ignored */
int report_wer_wanted(const char *folder);
/* WER's folders under each base (%LOCALAPPDATA%, then %ProgramData%; NULL or "" skipped):
 * <base>\Microsoft\Windows\WER\ReportArchive, then ...\ReportQueue, into roots (at most cap).
 * How many were written. */
int report_wer_root_paths(const char *const *bases, int nbases, char roots[][REPORT_PATH_MAX], int cap);
/* Text bytes as valid UTF-8: UTF-16LE when it starts with FF FE (or has a NUL in every odd byte
 * of its first 64), else UTF-8 (a BOM dropped); invalid sequences and unpaired surrogates as
 * U+FFFD, NULs dropped. malloc'd, NUL-terminated; NULL when out of memory. */
char *report_text_utf8(const unsigned char *b, size_t n, size_t *len);

/* -- over common/plat.h (report.c; plat_win.c on Windows, plat_mac.c in the Mac tests) */

typedef struct {
    char path[REPORT_PATH_MAX];   /* <root>\<folder>\Report.wer */
    int64_t mtime;                /* Report.wer's, seconds since 1970 */
} report_wer;
/* Every wanted folder's Report.wer under the roots (missing roots skipped), modified at or
 * after `since` (seconds since 1970); the newest max in out, newest first. Returns how many are
 * in out; *found: how many there were. */
int report_wer_find(const char *const *roots, int nroots, int64_t since, report_wer *out, int max, int *found);
/* A Report.wer's text (report_text_utf8 of its first max bytes), opened in its folder without
 * following a link, a regular file only. 0 with *out (malloc'd) and *len; an errno otherwise. */
int report_wer_text(const char *path, size_t max, char **out, size_t *len);

/* -- from Windows (report_win.c) */

/* WER's ReportArchive and ReportQueue folders, the account's (%LOCALAPPDATA%) and the machine's
 * (%ProgramData%), report_wer_root_paths's order; how many were found (0-4) */
int report_wer_roots(char roots[REPORT_WER_ROOTS][REPORT_PATH_MAX]);
/* the Desktop folder (FOLDERID_Desktop): 0, or -1 */
int report_desktop(char *out, size_t n);
/* The Application log's wanted events of the last `minutes` minutes, oldest first, one cleaned
 * XML event per line, at most `lines` events and max bytes (the newest kept). 0 with *out
 * (malloc'd, "" when none) and *len; an errno otherwise (ENOENT: wevtapi.dll couldn't be loaded). */
int report_event_log(uint32_t minutes, int lines, size_t max, char **out, size_t *len);

#endif
