/* admin_proto.c: what the app (admin.c, W4) and the elevated worker (win/admin/, W5) agree on
 * (app.md §5.5, §5.6). Pure C11, no file and no Windows call: the Mac tests and the fuzz driver
 * run every rule here.
 *
 *  - The command line (protocol 2, multi-version rekordbox, .orchestration/mv-spec.md §2.2):
 *    `install|uninstall|restore|install-worker|remove-root-folder --proto N --targets T
 *    --manifest M --source DIR --force 0|1 --pipe NAME`, every option exactly once, in any order.
 *    `--targets` is the scope the app checked: `*` for uninstall, remove-root-folder and
 *    install-worker (they sweep and name none), else 1 to ADMIN_TARGETS_MAX comma-separated
 *    `X.Y.Z:<16 lowercase hex>` items (a version folder's X.Y.Z, ^\d+\.\d+\.\d+$ with each part
 *    at most 9 digits, and the first 16 hex digits of its rekordbox.exe's sha256), sorted by
 *    version, unique. `--manifest` is the sha256 of the payload.json the app verified and copied
 *    into the source folder (install, install-worker), `-` for the verbs without a source.
 *    `--source` is that folder: absolute, needed by install and install-worker, empty otherwise.
 *    A line whose --proto is a well-formed number other than ADMIN_PROTO is refused with
 *    ADMIN_EXIT_PROTOCOL whatever its other options (another protocol's line), except for
 *    install-worker, which no protocol version refuses (I75) once its line is well-formed: its
 *    shape (`--targets *`, a manifest) is kept from protocol 2 on. Protocol 2 is a plain version
 *    check (decision D13: Windows never shipped, so there is no protocol-1 adoption).
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
 * exit codes' meanings or the pipe records. Protocol 2 (.orchestration/mv-spec.md §2.2, decision
 * D13) replaced protocol 1's --version with --targets T and added --manifest M; it is a plain
 * version check (a mismatch refuses, and the app reinstalls the worker): Windows never shipped,
 * so there is no protocol-1 adoption or migration path. ADMIN_PROTO_TEXT is the same number as
 * text, for the worker's marker (win/admin/main.c). */
enum { ADMIN_PROTO = 2 };
#define ADMIN_PROTO_TEXT "2"
enum { ADMIN_PROTO_TARGETS = 2 };   /* the first protocol that speaks --targets and --manifest */

enum {
    ADMIN_PATH_MAX = 1024,       /* --source, and every path the worker builds */
    ADMIN_VERSION_MAX = 32,      /* "999999999.999999999.999999999" fits */
    ADMIN_PIPE_MAX = 64,
    ADMIN_LINE_MAX = 1024,       /* one pipe line, its end included; longer ones are cut */
    ADMIN_TARGETS_MAX = 16,      /* --targets' items */
    ADMIN_TARGETS_ARG_MAX = 16 * 32,   /* --targets' text, its NUL included */
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
    /* appended for multi-version rekordbox (protocol 2, mv-spec §2.2) */
    ADMIN_EXIT_SCOPE_CHANGED = 15,       /* the worker's fresh target set isn't --targets, or the staged payload.json's
                                          * sha256 isn't --manifest: nothing changed; the app refreshes and asks again */
    ADMIN_EXIT_PARTIAL = 16,             /* some targets done, one failed (its @target says how), the rest not started */
    ADMIN_EXIT_UNDO_INCOMPLETE = 17,     /* an Install refused before any rekordbox folder changed, and its undo couldn't
                                          * restore everything in the root: the staging folder with the saved copies stays
                                          * (the "rollback incomplete" line names it) */
    ADMIN_EXIT_USER_STOPPED = 1223,      /* the app's own: UAC declined (ERROR_CANCELLED), never the worker's */
};

/* One --targets item, "X.Y.Z:<16 lowercase hex>": a version folder's X.Y.Z (admin_version_ok) and
 * the first 16 hex digits of its rekordbox.exe's sha256, as the app checked them (protocol 2). */
typedef struct {
    char version[ADMIN_VERSION_MAX];
    char exe16[17];
} admin_target;

typedef struct {
    int verb;                             /* enum admin_verb */
    int proto;
    /* --targets and --manifest (protocol 2, mv-spec §2.2) */
    int all;                              /* --targets *: uninstall, remove-root-folder and install-worker
                                           * sweep and name none */
    admin_target t[ADMIN_TARGETS_MAX];    /* else 1 to ADMIN_TARGETS_MAX items, sorted by version, unique */
    int nt;
    char manifest[65];                    /* the sha256 of the payload.json the app verified and copied
                                           * into --source; "" for "-" (the verbs without a source) */
    char source[ADMIN_PATH_MAX];          /* the verified payload's folder, or "" */
    int force;                            /* 0/1: install again over our bridge (Reinstall) */
    char pipe[ADMIN_PIPE_MAX];            /* \\.\pipe\rbstemsplus-admin-<32 hex> */
} admin_args;

/* --targets' value into out->all, out->t and out->nt: "*", or 1 to ADMIN_TARGETS_MAX
 * comma-separated items sorted by version (payload_version_cmp's order: numeric, part by part),
 * unique, each version admin_version_ok and each hex exactly 16 lowercase digits, the whole
 * shorter than ADMIN_TARGETS_ARG_MAX. 0, or -1 (out's targets cleared). */
int admin_targets_parse(const char *s, admin_args *out);
/* n targets as --targets' value ("*" for n 0 is the caller's): the length written (NUL not
 * counted), or -1 when an item is invalid, they aren't sorted and unique, n isn't 1 to
 * ADMIN_TARGETS_MAX, or it doesn't fit (out "" then). */
long admin_targets_format(const admin_target *t, int n, char *out, size_t cap);
/* the order of two admin_version_ok versions, numeric part by part: -1, 0 or 1 */
int admin_version_cmp(const char *a, const char *b);

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

/* Parses argv[1..argc-1] (UTF-8; argv[0] is the program; ADMIN_ARGC of them for this protocol,
 * any count for another's). ADMIN_EXIT_OK with *out filled;
 * ADMIN_EXIT_CHECK_FAILED for anything malformed (why, n bytes, says what); ADMIN_EXIT_PROTOCOL
 * when the verb is known and --proto is a well-formed number other than ADMIN_PROTO, whatever the
 * rest and the argument count (another protocol's line; out->pipe its --pipe when that is a valid
 * one, so the worker can say why). install-worker in this protocol's shape is the exception
 * (I75): parsed as this protocol's and, well-formed, ADMIN_EXIT_OK with out->proto as given. */
enum { ADMIN_ARGC = 14 };   /* the program, the verb and six options with their values */
int admin_parse_args(int argc, const char *const *argv, admin_args *out, char *why, size_t n);
/* The worker's start (main.c): argv (any count; an argument that couldn't be read is NULL)
 * parsed, then the app's pipe (*out's --pipe) connected and the marker line said on it, and a
 * refusal's reason. ADMIN_EXIT_OK with *out: go on. Else the exit code: another protocol's
 * recognised line is ADMIN_EXIT_PROTOCOL whether or not its pipe could be used (re-review
 * finding 9); without the app's pipe anything else is ADMIN_EXIT_CHECK_FAILED, nothing said. */
int admin_start(int argc, const char *const *argv, const char *marker, int (*connect)(void *ctx, const char *pipe), void (*say)(void *ctx, const char *line),
                void *ctx, admin_args *out);

/* The command line ShellExecuteEx gets as lpParameters (the verb and options, not the program),
 * quoted for the CRT's (and CommandLineToArgvW's) parsing: every value in double quotes, the
 * backslashes before a quote doubled. a->all gives `--targets *`, else a->t[0..nt); an empty
 * a->manifest gives `-`. The length written (NUL not counted), or -1 when a value is invalid
 * (admin_parse_args would refuse it, whatever a->proto) or it doesn't fit. */
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
    /* protocol 2 (mv-spec §2.2), after UNKNOWN so no value above moves; an older app reads them
     * as ADMIN_LINE_UNKNOWN */
    ADMIN_LINE_TARGET,          /* "@target <X.Y.Z> <code> <what>": one target's outcome; code the exit code it
                                 * would have alone ("-" for not-started), what one of ADMIN_TARGET_WHAT_* */
    ADMIN_LINE_ROOT_KEPT,       /* "@root-kept <why>": the inventory was incomplete or a bridge remains: the shared root stays */
};
/* @target's <what> words */
#define ADMIN_TARGET_WHAT_INSTALLED "installed"
#define ADMIN_TARGET_WHAT_ALREADY "already"
#define ADMIN_TARGET_WHAT_PUT_BACK "put-back"
#define ADMIN_TARGET_WHAT_KEPT "kept"
#define ADMIN_TARGET_WHAT_NOT_STARTED "not-started"
#define ADMIN_TARGET_WHAT_REFUSED "refused"
enum { ADMIN_TARGET_WHAT_MAX = 16 };   /* the longest word and its NUL fit */
/* A line (without its end) as a record: its kind, and its value (after one space) in val. */
int admin_line_kind(const char *line, size_t n, char *val, size_t cap);
/* @target's value "<X.Y.Z> <code> <what>": 0 with the version (ADMIN_VERSION_MAX bytes), the code
 * (-1 for "-"; else 0 to 9999) and the word (one of ADMIN_TARGET_WHAT_*), or -1 */
int admin_target_value(const char *val, char *version, int *code, char *what);

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
