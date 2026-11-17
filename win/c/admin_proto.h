/* admin_proto.c: what the app (admin.c, W4) and the elevated worker (win/admin/, W5) agree on
 * (app.md §5.5, §5.6). Pure C11, no file and no Windows call: the Mac tests and the fuzz driver
 * run every rule here.
 *
 *  - The command line: `install|uninstall|restore|install-worker|remove-root-folder --proto N
 *    --version V --source DIR --force 0|1 --pipe NAME`, every option exactly once, in any order.
 *    `--version` is rekordbox's version as the app checked it (the Mac's $VERSION) and must match
 *    ^\d+\.\d+\.\d+$ before any version agreement is looked at (each part at most 9 digits here);
 *    it is empty only for remove-root-folder and install-worker. `--source` is the folder of the
 *    payload the app verified: absolute, needed by install and install-worker, empty otherwise.
 *    A protocol that isn't ADMIN_PROTO is refused with ADMIN_EXIT_PROTOCOL, except for
 *    install-worker, which no protocol version refuses (I75).
 *  - The exit codes: Scripts.swift's RootExit table, unchanged, plus three appended (W-core adds
 *    their texts): the protocol mismatch, "run again" after install-worker, and a payload older
 *    than the worker's floor (J23). 1223 (ERROR_CANCELLED) is the app's own: UAC declined.
 *  - The pipe: its name, and the lines the worker writes on it (plain lines for the log; lines
 *    starting with '@' are records the app acts on).
 *  - The worker's names (I78): `bin\<sha12>\RB Stems Plus.exe`, <sha12> exactly 12 lowercase hex
 *    digits (J7), and the marker text every worker build carries, from which the app reads a
 *    `bin\` worker's protocol without running it.
 *  - The source folder's layout: payload.json, payload.json.sig, the bridge under the payload's
 *    windows.bridge.file, and admin\RB Stems Plus.exe. */
#ifndef RBSP_WIN_ADMIN_PROTO_H
#define RBSP_WIN_ADMIN_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* The protocol this app and this worker speak. Raised with any change to the command line, the
 * exit codes' meanings or the pipe records. */
enum { ADMIN_PROTO = 1 };

enum {
    ADMIN_PATH_MAX = 1024,       /* --source, and every path the worker builds */
    ADMIN_VERSION_MAX = 32,      /* "999999999.999999999.999999999" fits */
    ADMIN_PIPE_MAX = 64,
    ADMIN_LINE_MAX = 1024,       /* one pipe line, its end included; longer ones are cut */
};

/* the bridge's cap, both sides' (bridge.md §2, Scripts.swift:310-311): the app's download
 * (native_steps.c), its copy into the source folder (admin.c) and the worker's stage (worker.h) */
#define ADMIN_BRIDGE_MAX ((int64_t)32 * 1024 * 1024)

enum admin_verb {
    ADMIN_INSTALL = 0,
    ADMIN_UNINSTALL,
    ADMIN_RESTORE,
    ADMIN_INSTALL_WORKER,
    ADMIN_REMOVE_ROOT_FOLDER,
    ADMIN_VERB_COUNT
};

/* Exit codes. 3-11 are Scripts.swift's RootExit, with the same meanings; any other code but 0 and
 * those below: a step failed before rekordbox was changed (as on the Mac). */
enum {
    ADMIN_EXIT_OK = 0,
    ADMIN_EXIT_FAILED = 1,               /* a step failed before rekordbox was changed */
    ADMIN_EXIT_NO_ORIGINALS = 3,         /* no complete saved set for this rekordbox: nothing changed */
    ADMIN_EXIT_CHECK_FAILED = 4,         /* a check failed: nothing changed */
    ADMIN_EXIT_REKORDBOX_OPEN = 5,       /* nothing changed */
    ADMIN_EXIT_ROLLED_BACK = 6,          /* failed after rekordbox was changed; Pioneer's file is back */
    ADMIN_EXIT_ALREADY_RUNNING = 7,      /* another run holds the lock: nothing changed */
    ADMIN_EXIT_INSTALLER_RUNNING = 8,    /* Pioneer's installer, updater or uninstaller runs: nothing changed */
    ADMIN_EXIT_REKORDBOX_MISSING = 9,    /* rekordbox isn't installed: nothing changed */
    ADMIN_EXIT_NOT_PIONEERS = 10,        /* rekordbox isn't as Pioneer's installer leaves it: nothing changed */
    ADMIN_EXIT_ROLLBACK_FAILED = 11,     /* failed after rekordbox was changed, and so did putting it back */
    /* appended for Windows (W-core gives them texts) */
    ADMIN_EXIT_PROTOCOL = 12,            /* this worker speaks another protocol: update the app first */
    ADMIN_EXIT_RUN_AGAIN = 13,           /* install-worker put the app's own worker in bin\: start it */
    ADMIN_EXIT_OLDER = 14,               /* the app's payload is older than the worker's floor: update first */
    ADMIN_EXIT_USER_STOPPED = 1223,      /* the app's own: UAC declined (ERROR_CANCELLED), never the worker's */
};

typedef struct {
    int verb;                             /* enum admin_verb */
    int proto;
    char version[ADMIN_VERSION_MAX];      /* rekordbox's version as the app checked it, or "" */
    char source[ADMIN_PATH_MAX];          /* the verified payload's folder, or "" */
    int force;                            /* 0/1: install again over our bridge (Reinstall) */
    char pipe[ADMIN_PIPE_MAX];            /* \\.\pipe\rbstemsplus-admin-<32 hex> */
} admin_args;

/* the verb's word ("install", ...), NULL for none */
const char *admin_verb_name(int verb);
/* the verb of a word, -1 when it isn't one */
int admin_verb_of(const char *word);

/* ^\d+\.\d+\.\d+$, each part 1 to 9 ASCII digits */
int admin_version_ok(const char *v);
/* exactly \\.\pipe\rbstemsplus-admin- and 32 lowercase hex digits: a local pipe, never a remote
 * one (\\server\pipe\...) */
int admin_pipe_name_ok(const char *name);
/* exactly 12 lowercase hex digits (a bin\ worker folder's name, J7) */
int admin_sha12_ok(const char *name);
/* An absolute path the worker may open: Windows "X:\..." (no "\\?\", UNC or device path); the
 * Mac tests "/..."; no empty, "." or ".." component, no control character, no '"', and on
 * Windows none of <>|?* and no ':' past the drive; shorter than ADMIN_PATH_MAX. */
int admin_abs_path_ok(const char *p);
/* A plain file name from the payload (windows.bridge.file): one component, printable ASCII
 * without separators, ':' or the characters Windows refuses, not "." or "..", no trailing dot or
 * space, at most 128 bytes. */
int admin_plain_name_ok(const char *name);

/* Parses argv[1..argc-1] (UTF-8; argv[0] is the program). ADMIN_EXIT_OK with *out filled;
 * ADMIN_EXIT_CHECK_FAILED for anything malformed (why, n bytes, says what); ADMIN_EXIT_PROTOCOL
 * when everything else is well-formed but --proto isn't ADMIN_PROTO (install-worker excepted:
 * then ADMIN_EXIT_OK, out->proto as given). */
int admin_parse_args(int argc, const char *const *argv, admin_args *out, char *why, size_t n);

/* The command line ShellExecuteEx gets as lpParameters (the verb and options, not the program),
 * quoted for the CRT's (and CommandLineToArgvW's) parsing: every value in double quotes, the
 * backslashes before a quote doubled. The length written (NUL not counted), or -1 when a value
 * is invalid (admin_parse_args would refuse it, with this protocol) or it doesn't fit. */
long admin_build_params(const admin_args *a, char *out, size_t cap);

/* the pipe's name for 16 random bytes */
void admin_pipe_name(const unsigned char rnd[16], char out[ADMIN_PIPE_MAX]);

/* -------------------------------------------------------------- the worker's names (I78) */

#define ADMIN_WORKER_NAME "RB Stems Plus.exe"   /* every copy Windows can elevate */
#define ADMIN_APP_COPY_DIR "admin"              /* the zip's and the source folder's admin\ */
#define ADMIN_MARKER_PREFIX "rbsp-admin worker: "

/* The marker every worker build carries in its bytes: ADMIN_MARKER_PREFIX "proto=N minimum=V\n".
 * Parses the first one found in buf[0..n): 1 with *proto and the compiled minimum in min, or 0. */
int admin_marker_find(const void *buf, size_t n, int *proto, char min[ADMIN_VERSION_MAX]);

/* Whether self is root\bin\<sha12>\RB Stems Plus.exe (case-blind on Windows for root and "bin",
 * exact for the folder name's hex and the file name); 1 with sha12 (13 bytes) filled, or 0. */
int admin_self_in_bin(const char *root, const char *self, char sha12[13]);

/* -------------------------------------------------------------- the pipe's lines */

/* Records (a line starting with '@'). Every other line is text for the log. */
enum {
    ADMIN_LINE_TEXT = 0,
    ADMIN_LINE_STILL_CLOSING,   /* "@still-closing": rekordbox's process lingers with no window (PLAN R1) */
    ADMIN_LINE_RESTART,         /* "@restart": the root's last file is removed at the next restart (I20) */
    ADMIN_LINE_BRIDGE,          /* "@bridge <sha256>": the bridge now in rekordbox (for the app's own state) */
    ADMIN_LINE_FLOOR,           /* "@floor <version>": the worker's floor now */
    ADMIN_LINE_CLOSED,          /* "@closed": the rekordbox that was still closing has closed (after @still-closing) */
    ADMIN_LINE_UNKNOWN,         /* '@' and anything else: ignored by the app (a newer worker) */
};
/* A line (without its end) as a record: its kind, and its value (after one space) in val. */
int admin_line_kind(const char *line, size_t n, char *val, size_t cap);

/* Splits the pipe's bytes into lines: feed what was read, take lines while there are. A line
 * longer than ADMIN_LINE_MAX - 1 is cut there (the rest, to its end, dropped); a NUL or a byte
 * that isn't printable ASCII, a tab or UTF-8 is replaced by '?'. */
typedef struct {
    char buf[ADMIN_LINE_MAX];
    size_t n;
    int dropping;               /* inside a line that was cut */
} admin_lines;
void admin_lines_init(admin_lines *l);
/* feeds data[0..n); calls fn(ctx, line, len) for every whole line; at_end: the pipe closed, a
 * last line without its end counts too. Lines end at "\n", a "\r" before it dropped. */
void admin_lines_feed(admin_lines *l, const void *data, size_t n, int at_end, void (*fn)(void *ctx, const char *line, size_t len),
                      void *ctx);

#endif
