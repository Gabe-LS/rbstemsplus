/* inventory.c: every installed rekordbox on Windows, one walk shared by the app, the watcher and
 * the worker (multi-version contract, .orchestration/mv-spec.md §2.1, §2.2, §2.5, §2.6; decisions
 * D1, D3, D13). compat_find_rekordbox and its cached form become thin views over it (inv_current),
 * so every caller of today's "current rekordbox" keeps its answer; Stems Cache uses the rows.
 *
 *  - The rows: both registry views' uninstall entries, deduplicated by folder (case-blind), plus
 *    the "rekordbox X.Y.Z" folders under root that no entry names (cleanup evidence only).
 *  - complete: an incomplete inventory never authorizes a new swap or a shared-file deletion (the
 *    worker enforces it); the app shows the rows it has with a "checking" line.
 *  - The target rule (inv_targets) and the row rule (inv_rows) are pure, so the app and the worker
 *    compute the same sets from the same facts.
 *  - D13 (Windows never shipped): there is no legacy record or legacy backup to adopt. A bridge
 *    without a matching receipt is never "done" and never adopted by install: its row needs repair
 *    (WHY_NOT_RECORDED), and Put Back Pioneer's File restores the original the signed binding
 *    vouches for (inv_original_bound).
 *
 * L0 contract: the bodies are stubs that answer "not implemented" (-1, or nothing); lane A writes
 * them. An inv_list is large (INV_MAX rows with their exe's facts): allocate it, never on a stack. */
#ifndef RBSP_WIN_INVENTORY_H
#define RBSP_WIN_INVENTORY_H

#include "admin_proto.h"
#include "compat.h"
#include "payload.h"
#include "plat.h"

#include <stddef.h>
#include <stdint.h>

enum { INV_MAX = COMPAT_ENTRIES_MAX };
enum { INV_VIEW_64 = COMPAT_VIEW_64, INV_VIEW_32 = COMPAT_VIEW_32 };

enum inv_trust {
    INV_VERIFIED,    /* registered; a real folder (no reparse); rekordbox.exe AlphaTheta-pinned; the folder's,
                      * the entry's DisplayVersion and the exe's ProductVersion agree */
    INV_UNVERIFIED,  /* registered, exe present: pin NO, the versions disagree, or conflicting live entries
                      * name this folder */
    INV_NO_ANSWER,   /* the exe or the folder couldn't be looked at, pin ERROR, changed while pinned */
    INV_SETTLING,    /* registered, the folder exists, rekordbox.exe missing (an installer mid-copy) */
    INV_STALE,       /* registered, the folder missing (ENOENT/ENOTDIR): ignored for display and install */
    INV_DISK_ONLY,   /* "rekordbox X.Y.Z" under root that no entry names: cleanup evidence only, never install */
};

typedef struct {
    char folder[COMPAT_PATH_MAX];             /* root\rekordbox X.Y.Z, spelled as the entry or the listing has it */
    char name[COMPAT_PATH_MAX];               /* "rekordbox X.Y.Z" */
    char version[COMPAT_VERSION_MAX];         /* the folder's X.Y.Z */
    char display_version[COMPAT_VERSION_MAX]; /* the entry's, "" for disk-only */
    int views;                                /* INV_VIEW_64 | INV_VIEW_32: which views named it (diagnostics only) */
    int trust;                                /* enum inv_trust */
    plat_id dir_id;                           /* the folder's identity, not followed */
    compat_file exe;                          /* its rekordbox.exe, pinned (COMPAT_PIN); the worker reads COMPAT_FRESH */
} inv_install;

typedef struct {
    inv_install it[INV_MAX];   /* newest first (version order), duplicates collapsed case-blind */
    int n;
    int registry_complete;     /* both views enumerated whole, under COMPAT_ENTRIES_MAX, no allocation failure */
    int disk_complete;         /* root listed whole (or absent: ENOENT) */
    int complete;              /* registry_complete && disk_complete && no INV_NO_ANSWER or INV_SETTLING row */
    uint64_t gen;              /* compat_uninstall_generation() read before the walk (0: none) */
} inv_list;

/* The walk: 0 with *out filled, -1 (no memory). flags: COMPAT_FRESH reads every exe again (the
 * worker), else compat.c's memo serves. */
int inv_read(const compat_os *os, const char *rb_root, int flags, inv_list *out);

/* inv_read's answer kept while gen (the caller's compat_uninstall_generation(), read just before)
 * hasn't moved and isn't 0, the root is the same and every row's folder and exe stat as they did
 * (compat_find_rekordbox_cached's rules). A SETTLING or NO_ANSWER row, or a partial enumeration,
 * is never kept. One call at a time per cache. The members past lock are inventory.c's own. */
typedef struct {
    plat_mutex lock;
    int valid;
    uint64_t gen;
    char root[COMPAT_PATH_MAX];
    inv_list *list;            /* malloc'd */
    void *stamps;              /* inventory.c's per-row stat stamps, malloc'd */
    int walks;                 /* how many times it walked (tests) */
} inv_cache;
#define INV_CACHE_INIT {.lock = PLAT_MUTEX_INIT}
int inv_read_cached(inv_cache *c, const compat_os *os, uint64_t gen, const char *rb_root, inv_list *out);
void inv_cache_clear(inv_cache *c);

/* Today's selection, unchanged: the newest row holding an exe (VERIFIED, UNVERIFIED or NO_ANSWER)
 * decides, with no fallback; a partial registry with a newer unlisted disk folder is NO_ANSWER
 * (review RE3). Fills compat_rekordbox[] for the old callers: compat_find_rekordbox's return,
 * *current and *state (COMPAT_RB_*), exactly. */
int inv_current(const inv_list *l, compat_rekordbox *out, int max, int *current, int *state);

/* -------------------------------------------------------------- the original ORT (mv-spec §2.3) */

/* Whether original (the live onnxruntime.dll when it is Pioneer's, else the complete saved set's)
 * is the one the payload binds for rekordbox of exe_product_version: its sha256 ==
 * payload_win_ort_for(m, exe_product_version), its code_sha256 == m->win_ort_code_sha256, its pin
 * OK, and it isn't a bridge. 1 or 0. */
int inv_original_bound(const payload_manifest *m, const char *exe_product_version, const compat_file *original);

/* -------------------------------------------------------------- the target rule (mv-spec §2.2) */

enum { INV_VERB_INSTALL, INV_VERB_REINSTALL, INV_VERB_RESTORE, INV_VERB_UNINSTALL };

/* Per row of an inv_list, filled by the caller's assessment (the app: memoised facts; the worker:
 * FRESH, through held handles). */
typedef struct {
    int live;                    /* COMPAT_LIVE_* of the live onnxruntime.dll */
    payload_sha live_sha;        /* the assessed live file's SHA-256, if readable */
    int saved_complete;          /* pioneer\<version>\ holds a complete saved set */
    payload_sha saved_sha;       /* its onnxruntime.dll's sha256, "" none */
    payload_sha original_sha;    /* the live DLL's when Pioneer's, else the saved set's */
    payload_sha original_code;   /* pe_code_sha256 of that original ("" not a PE the digest takes) */
    int original_bound;          /* inv_original_bound() of that original */
    int listed;                  /* payload_win_rekordbox_supported(m, the exe's ProductVersion) */
    int receipt_ok;              /* receipt_matches() (receipt.h) */
    int r1;                      /* NF_R1_* for this folder */
    int running;                 /* a rekordbox.exe runs from this folder (the app's and the watcher's rows) */
} inv_assess_row;

/* The scope of a verb, sorted by version, unique: how many targets written to out (at most max),
 * or -1 when l isn't complete (install, reinstall).
 *  - install: VERIFIED, listed and bound rows whose live DLL is PIONEER, OURS or OURS_UNLISTED,
 *    or MISSING with a complete saved set that is the bound original, and not done (done: live
 *    OURS with the payload's bridge sha256 and receipt_ok). An empty scope is a no-op (no UAC).
 *  - reinstall (force): every VERIFIED, listed and bound row, done or not.
 *  - restore: the rows with r1 != NF_R1_NOTHING, plus our bridges in versions now unlisted or
 *    unbound (no listing or verification needed beyond the folder's own checks).
 *  - uninstall: the rows the worker's disk sweep ("*") changes, its affected versions. With a
 *    complete inventory: every row holding our bridge or needing repair (DISK_ONLY rows too, and
 *    rows whose rekordbox.exe couldn't be hashed: exe16 "" then). With an incomplete one (D3):
 *    restore-only, the folders whose live DLL is our bridge with receipt_ok; the worker keeps the
 *    shared root (@root-kept). Never -1.
 * D13: a live bridge (OURS, OURS_UNLISTED) without receipt_ok is never in an install or reinstall
 * scope; its row needs repair. Each target's exe16 is the first 16 hex digits of its
 * rekordbox.exe's sha256. */
int inv_targets(int verb, const inv_list *l, const inv_assess_row *a, const payload_manifest *m, admin_target *out, int max);

/* -------------------------------------------------------------- the UI rows (mv-spec §2.5, §2.6) */

/* Row states and why codes: FACT_WIN_RB_ROWS carries them as numbers; core/pure.h mirrors them
 * (WIN_ROW_*, WIN_WHY_*) with the same values */
enum { INV_ROW_ON = 0, INV_ROW_READY = 1, INV_ROW_UNSUPPORTED = 2, INV_ROW_REPAIR = 3, INV_ROW_UNVERIFIED = 4 };
enum {
    WHY_NONE = 0,
    WHY_RB_UNLISTED = 1,        /* Unsupported: the version isn't in the signed list */
    WHY_BUILD_UNLISTED = 2,     /* Unsupported: listed, its original isn't its binding, the code digest matches */
    WHY_ORT_UNLISTED = 3,       /* Unsupported: listed, its original's code digest isn't the payload's */
    WHY_ORT_NOT_ORIGINAL = 4,   /* Unsupported: the original isn't AlphaTheta's */
    WHY_R1_MISSING = 5,         /* Needs repair: the live onnxruntime.dll is missing */
    WHY_R1_FOREIGN = 6,         /* Needs repair: it is neither Pioneer's nor ours */
    WHY_R1_ORT_MISSING = 7,     /* Needs repair: ort\'s copy is missing */
    WHY_DELISTED_BRIDGE = 8,    /* Needs repair: our bridge in a version now unlisted or unbound */
    WHY_NOT_VERIFIED = 9,       /* Couldn't verify: UNVERIFIED */
    WHY_NO_ANSWER = 10,         /* Couldn't verify: NO_ANSWER */
    WHY_NOT_RECORDED = 11,      /* Needs repair: our bridge without a matching receipt (D13: never adopted) */
};

typedef struct {
    char version[COMPAT_VERSION_MAX];          /* X.Y.Z */
    char name[COMPAT_PATH_MAX];                /* "rekordbox X.Y.Z" */
    char display_version[COMPAT_VERSION_MAX];  /* the entry's full build, "" none */
    int state, why, r1, running;               /* INV_ROW_*, WHY_*, NF_R1_*, 0/1 */
} inv_row_view;

/* The rows the UI and the watcher show, newest first: STALE rows never, DISK_ONLY rows only when
 * they hold our bridge (as Needs repair). How many written (at most max). */
int inv_rows(const inv_list *l, const inv_assess_row *a, const payload_manifest *m, inv_row_view *out, int max);

/* A verified, receipt-backed bridge eligible for replacement, or an empty string. */
const char *inv_outdated_bridge(const inv_list *l, const inv_assess_row *a, const char *latest);

enum { INV_STATE_TEXT_MAX = INV_MAX * (COMPAT_VERSION_MAX + 32) + 1 };
void inv_state_text(const inv_row_view *rows, int count, char *out, size_t cap);
int inv_list_digest(const payload_manifest *m, unsigned char out[32]);
enum { INV_REFRESH_KEYS_MAX = INV_MAX * (65 + COMPAT_VERSION_MAX) + 1 };
/* Unseen version/list pairs to record before requesting an early refresh. */
size_t inv_refresh_keys(const inv_row_view *rows, int count, const payload_manifest *m, const char *seen,
                        char out[INV_REFRESH_KEYS_MAX]);

#endif
