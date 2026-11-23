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
 * The masking is the core's (core_mask, through report_env's mask); this file only drops the event
 * XML's account SID and computer name. The zip is report_build.c's own (stored entries). */
#ifndef RBSP_WIN_REPORT_H
#define RBSP_WIN_REPORT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

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

/* -- the report itself (report_build.c: Report.swift's buildReport, issueURL, lastProblem; pure
 * or over common/plat.h, so the Mac tests build a whole report on a scratch tree) */

enum { REPORT_TAIL_MAX = 2000000, REPORT_PROBLEM_TAIL = 500000, REPORT_URL_MAX = 7000, REPORT_PROBLEM_WINDOW_S = 3 * 3600 };
#define REPORT_ISSUES_URL "https://github.com/Gabe-LS/rbstemsplus/issues/new"   /* Paths.swift's issuesURL */

/* The zip, in memory: entries stored (method 0, no compression: Windows has no deflate in C, and
 * the report's text stays far below an issue's attachment limit), names ASCII, one disk, no ZIP64.
 * Explorer and every unzip read it. */
typedef struct {
    unsigned char *b, *cd;   /* the local entries, then the central directory */
    size_t n, cap, cdn, cdcap;
    int count, err;          /* err: an entry didn't fit (ENOMEM, EFBIG over 2 GB, EINVAL a name) */
} report_zip;
void report_zip_init(report_zip *z);
/* name: printable ASCII, '/'-separated, relative; when: the entry's local date and time (DOS) */
int report_zip_add(report_zip *z, const char *name, const void *p, size_t n, const struct tm *when);
/* the central directory and its end appended: 0 with the zip in z->b (z->n bytes), else z->err */
int report_zip_end(report_zip *z);
void report_zip_free(report_zip *z);
uint32_t report_crc32(uint32_t crc, const void *p, size_t n);

/* capBytes: s in at most bytes bytes of UTF-8, ending in "…" when cut (a code point kept whole).
 * malloc'd; NULL when out of memory */
char *report_cap_bytes(const char *s, size_t bytes);
/* knownChecksums: the 64- and 40-hex-digit runs in s (not inside a longer hex run), lower case,
 * each once, appended to *list (*n of them, malloc'd strings; grown with realloc). 0, or -1 out of
 * memory. report_strings_free frees such a list. */
int report_known_checksums(const char *s, size_t n, char ***list, int *count);
void report_strings_free(char **list, int n);
/* issueDetails: the last problem (at most 800 bytes), a blank line, then the facts one a line (each
 * at most 300 bytes, newlines as spaces). malloc'd */
char *report_issue_details(const char *problem, const char *const *facts, int n);
/* issueURL: REPORT_ISSUES_URL with title (at most 120 bytes) and the issue's body (issueBody: the
 * user's own words first, the details at the bottom), every byte but A-Z a-z 0-9 - . _ ~
 * percent-encoded; whole lines left out from the end of the details until it is at most
 * REPORT_URL_MAX bytes. malloc'd; NULL when out of memory */
char *report_issue_url(const char *title, const char *details);
/* lastProblem (Report.swift, line for line): the problems of app.log, then bridge.log's ERROR
 * lines, of the REPORT_PROBLEM_WINDOW_S before now (a line's "yyyy-MM-dd HH:mm:ss" is local time;
 * now: the local time as seconds since 1970 as if it were UTC, report_local_s). *title: "" when
 * there is none, else at most 60 characters; *text: "" or "Last problem:\n..." Both malloc'd (NULL
 * on no memory: then -1). Not masked: the caller masks both. */
int report_last_problem(const char *app, size_t an, const char *bridge, size_t bn, int64_t now, char **title, char **text);
/* local time t as seconds since 1970 read as UTC (the log's stamps compared with it) */
int64_t report_local_s(int64_t t);
/* maskLocation for the issue's link and title: the chosen folder (dir, "" none) as "<stems folder>",
 * then every path on another drive than the system's (sysdrive: 'C') as "<drive>:\<path>" (a path
 * up to a quote, a parenthesis or the end of the line), every name in names[] in quotes as
 * "\"<drive>\"". malloc'd */
char *report_mask_location(const char *s, const char *dir, char sysdrive, const char *const *names, int nnames);

/* summary.txt's facts: one line each, most important first; link: also in the issue's link (the
 * long ones only in summary.txt) */
typedef struct {
    char *line;
    int link;
} report_fact;
typedef struct {
    report_fact *v;
    int n, cap, err;
} report_facts;
/* a line (printf-style) at the end; err set and nothing added when out of memory */
void report_fact_add(report_facts *f, int link, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
void report_facts_free(report_facts *f);
/* the light's colour as the window shows it: "0" green, "1" yellow, else red */
const char *report_light_colour(const char *look);
/* watcherLine's last check: "N min ago", "N h ago", "N days ago", "never" (age < 0) */
void report_ago(int64_t age, char *out, size_t n);
/* "N.N GB" (10^9 bytes, one decimal), "unknown" for < 0 */
void report_gb(int64_t bytes, char *out, size_t n);

typedef struct {
    void *ctx;              /* mask's and events' */
    /* redact: the home folder as "~", the account's names as "<user>", secrets as "<token>" but the
     * checksums in keep. malloc'd (*len bytes, NUL-terminated); NULL out of memory */
    char *(*mask)(void *ctx, const char *s, size_t n, const char *const *keep, int nkeep, size_t *len);
    /* the Application log's wanted events (report_event_log): 0 with *out (malloc'd), or an errno.
     * NULL: none read */
    int (*events)(void *ctx, char **out, size_t *len);
    void (*log)(void *log_ctx, const char *line);   /* may be NULL */
    void *log_ctx;
    const char *support;    /* installed, dismissed, request, busy, config.ini, payload\payload.json */
    const char *logs;       /* app.log, watcher.log, bridge.log */
    const char *const *wer_roots;
    int nwer;
    const char *desktop;    /* where the zip goes; NULL or "" none */
    const char *fallback;   /* else here (made when missing): support\Logs\reports */
    const report_facts *facts;
    const char *location_dir;   /* config.ini's chosen folder, for the link's masking ("" none) */
    char sysdrive;              /* the system drive's letter (the link's masking), 0 none */
    const char *const *drive_names;
    int ndrive_names;
    int64_t now;            /* seconds since 1970 */
} report_env;
typedef struct {
    char home[REPORT_PATH_MAX], short_home[REPORT_PATH_MAX], full[260], user[260], folder_name[260];
} report_mask_names;
/* report_env's mask over core_mask, with the account's names and profile paths */
char *report_mask_with(void *ctx, const char *in, size_t n, const char *const *keep, int nkeep, size_t *len);
typedef struct {
    char path[REPORT_PATH_MAX];   /* the zip */
    char *title, *details;        /* the issue's, masked (malloc'd) */
} report_result;
/* buildReport: summary.txt (the facts), the logs' last REPORT_TAIL_MAX bytes and the small state
 * files, the event log's errors, the newest REPORT_WER_MAX Report.wer of the last REPORT_WER_DAYS
 * days, each masked, in "RB Stems Plus report yyyy-MM-dd HHmmss/" of one zip, written whole on the
 * Desktop, else in fallback. Nothing else is read: no kept sets, no library, no helper output but
 * what app.log holds. 0 with *out (report_result_free after); else an errno (nothing written). */
int report_build(const report_env *e, report_result *out);
void report_result_free(report_result *r);

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
/* the account's profile folder (FOLDERID_Profile: C:\Users\<name>, redact's home): 0, or -1 */
int report_profile(char *out, size_t n);
/* a distinct 8.3 alias for the profile path: 0, or -1 when there is none */
int report_short_profile(const char *home, char *out, size_t n);
/* the account's name (GetUserNameW, redact's NSUserName): 0, or -1 */
int report_user_name(char *out, size_t n);
/* the account's display name (redact's NSFullUserName): 0, or -1 */
int report_full_name(const char *user, char *out, size_t n);
/* the Windows folder's drive letter ('C'), 0 when unknown */
char report_system_drive(void);
/* the PC's physical memory in bytes, 0 unknown; the user's locale name ("en-US"), "" unknown */
uint64_t report_memory(void);
void report_locale(char *out, size_t n);

#endif
