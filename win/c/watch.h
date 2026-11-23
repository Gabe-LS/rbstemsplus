/* watch.c: rbsp-watch.exe's decisions, the resident Windows watcher (app.md §5.7, W7), in portable
 * C over small operation tables, so the Mac tests run every rule on fakes; watch_win.c is the thin
 * Win32 side (ReadDirectoryChangesW, TaskDialogIndirect, Shell_NotifyIconW, RtlGetVersion, the
 * stop event). Three parts:
 *
 *  - The folders (watch_engine), each with its subtree, so a handle is held only on the top folder
 *    that may be renamed or removed, never below it (NTFS refuses to rename a folder while a handle
 *    is open anywhere below it, whatever the handle's share modes): rekordbox's folder
 *    `<Program Files>\rekordbox` (Windows' own Program Files on whichever drive:
 *    plat_program_files, the worker's rule; a new version folder is an update, and the current version
 *    folder is watched through its parent, so no handle is ever held on a folder rekordbox's
 *    updater or uninstaller removes), `%APPDATA%\Pioneer` (rekordbox's settings folder
 *    `rekordbox6\`: rekordbox opening; the model folder `rekordbox6\models\demucs3_model\`; so
 *    `Pioneer\` or `rekordbox6\` can be renamed while the watcher runs), and our admin-only root
 *    (only `ort\` counts there: its removal and re-creation included). A folder that is gone,
 *    refused, replaced or renamed away is closed at once and its nearest existing ancestor is
 *    watched instead, until the next component appears; then it is opened again.
 *  - The schedule (watch_sched): notifications add what they ask for (WATCH_ROUND, WATCH_R1); a
 *    set of rounds starts at most every 10 s (the Mac LaunchAgent's ThrottleInterval); what comes
 *    in during a set is its next round, at most watcherMaxRounds (3) per set.
 *  - The check (watch_check): Watcher.swift's watcherCheck through the core's own decisions
 *    (core/watcher.c: watcher_rounds, watcher_settle, watcher_ask, watcher_dialog_build,
 *    watcher_answer; core/state.c: the missing keys and the dismissed set), plus R1's check (I72):
 *    on a notification in the current version folder or in `ort\` only, never in the opening
 *    round, the live onnxruntime.dll and `ort\`'s copy are looked at (compat_r1_facts) and, when
 *    Pioneer's file should be put back, the watcher opens RB Stems Plus, whose own check at start
 *    (or at activation, when it is open) makes R1's offer: one offer, the app's, and no request
 *    record (a lane choice: the watcher asks nothing first).
 *
 * Texts come from the core in its Windows column (texts.def); one not written yet (W-core) means
 * the question isn't asked, with a log line, never a Mac text. Log lines are the watcher log's
 * own, the Mac's words where the Mac has the line. */
#ifndef RBSP_WIN_WATCH_H
#define RBSP_WIN_WATCH_H

#include "compat.h"
#include "dismissed.h"
#include "inventory.h"
#include "payload.h"

#include <stddef.h>
#include <stdint.h>

enum { WATCH_PATH_MAX = COMPAT_PATH_MAX, WATCH_MAX_ROUNDS = 3, WATCH_THROTTLE_MS = 10000, WATCH_TICK_MS = 60000 };

/* -------------------------------------------------------------------------------- the folders */

/* the watched folders (a folder's slot is its kind; the ancestor watched while it is missing has
 * slot kind + WD_COUNT): rekordbox's folder, `%APPDATA%\Pioneer`, our root */
enum { WD_REKORDBOX, WD_PIONEER, WD_ROOT, WD_COUNT, WATCH_SLOTS = 2 * WD_COUNT };

/* below WD_PIONEER's folder: rekordbox's settings folder and the model folder */
#define WATCH_SETTINGS_BELOW "rekordbox6"
#define WATCH_MODELS_BELOW "rekordbox6\\models\\demucs3_model"

/* watch_ops.reinstall's failures */
enum { WATCH_NO_REQUEST = 1, WATCH_NO_APP = 2 };

/* what a notification asks for */
enum { WATCH_ROUND = 1, WATCH_R1 = 2, WATCH_REGISTRY = 4 };

#define WATCH_REFRESH_EVENT L"Local\\io.github.rbstemsplus.rekordbox-changed"
#define WATCH_REPAIR_EVENT L"Local\\io.github.rbstemsplus.rekordbox-repair"

/* a notification's action: FILE_ACTION_*'s values */
enum { WATCH_ADDED = 1, WATCH_REMOVED = 2, WATCH_MODIFIED = 3, WATCH_RENAMED_OLD = 4, WATCH_RENAMED_NEW = 5 };

/* what a watch reports (FILE_NOTIFY_CHANGE_*): names (file and folder names), writes (last write
 * and size too) */
enum { WATCH_F_NAMES = 1, WATCH_F_WRITES = 2 };

/* how a read completed: notes, an overflow (ERROR_NOTIFY_ENUM_DIR, or a read with no notes: what
 * changed is unknown), the folder gone (ERROR_ACCESS_DENIED, delete pending, the handle no longer
 * valid), or another error */
enum { WATCH_READ_OK, WATCH_READ_OVERFLOW, WATCH_READ_GONE, WATCH_READ_ERROR };

typedef struct {
    int action;          /* WATCH_ADDED... */
    const char *name;    /* relative to the watched folder, UTF-8, "\" between components */
} watch_note;

typedef struct {
    void *ctx;
    /* opens path for watching in slot, with every share mode, and arms its first read: 0, or an
     * errno (ENOENT: nothing there or not a folder; another: refused). A slot is opened only
     * while closed. */
    int (*open)(void *ctx, int slot, const char *path, int subtree, int filter);
    void (*close)(void *ctx, int slot);
    /* whether slot's handle is still the folder at path (not removed, renamed away or replaced) */
    int (*same)(void *ctx, int slot, const char *path);
    void (*log)(void *ctx, const char *line);
} watch_fs;

typedef struct {
    const watch_fs *fs;
    char path[WD_COUNT][WATCH_PATH_MAX];
    int open[WATCH_SLOTS];
    char anchor[WD_COUNT][WATCH_PATH_MAX];   /* the ancestor watched while the folder is missing */
    char current[WATCH_PATH_MAX];            /* the current version folder's name, "" unknown */
    int seen[WD_COUNT];                      /* looked at by a tick before */
} watch_engine;

/* What a notification in kind's folder asks for (pure): WATCH_* flags. */
int watch_classify(int kind, const char *current, int action, const char *name);
/* what kind's folder appearing, going or overflowing asks for */
int watch_kind_flags(int kind);
/* how kind's folder is opened */
int watch_subtree(int kind);
int watch_filter(int kind);

/* paths[kind]: the folders, absolute */
void watch_engine_init(watch_engine *e, const watch_fs *fs, const char *const paths[WD_COUNT]);
/* opens every closed folder, or its nearest existing ancestor; checks every open one is still the
 * folder at its path. At start and every WATCH_TICK_MS. Returns WATCH_* flags (a folder that
 * appeared or went). */
int watch_engine_tick(watch_engine *e);
/* a completed read on slot: what it asks for. After it, the host arms the slot's next read only
 * while watch_engine_slot_open(slot). */
int watch_engine_read(watch_engine *e, int slot, int status, const watch_note *notes, int n);
int watch_engine_slot_open(const watch_engine *e, int slot);
/* the current version folder (its full path or its name; NULL or "": unknown) */
void watch_engine_set_current(watch_engine *e, const char *folder);

/* -------------------------------------------------------------------------------- the schedule */

typedef struct {
    int pending;         /* WATCH_* flags not yet taken by a round */
    int running;         /* a set of rounds runs */
    int started;         /* a set has started before (last_start is set) */
    int64_t last_start;  /* ms, the host's monotonic clock */
    uint64_t registry_gen;
} watch_sched;

void watch_sched_note(watch_sched *s, int flags);
/* A new Uninstall generation, or a periodic check for changes without a registry notification. */
void watch_sched_registry(watch_sched *s, uint64_t generation, int periodic);
/* 1: start a set of rounds now (running, last_start = now); 0: not now, and *wait_ms is how long
 * until it may (-1: nothing pending, or a set runs) */
int watch_sched_poll(watch_sched *s, int64_t now_ms, int64_t *wait_ms);
/* the pending flags, cleared (a round's own, after it settled) */
int watch_sched_take(watch_sched *s);
void watch_sched_done(watch_sched *s);

/* -------------------------------------------------------------------------------- the check */

/* What a round reads (watch_facts.c on Windows; the tests fill it themselves). */
typedef struct {
    inv_row_view rows[INV_MAX];
    int nrows, inv_complete;
    int rb_state;                          /* COMPAT_RB_* */
    int rb_unsettled;                      /* registry entry waiting for its EXE */
    char rekordbox_version[COMPAT_VERSION_MAX];   /* the current folder's "7.2.19", "" none */
    char folder[WATCH_PATH_MAX];           /* the current version folder (COMPAT_RB_OK), "" none */
    char watch_name[WATCH_PATH_MAX];       /* the version folder to watch: the candidate's, "" none */
    char engine[64];                       /* demucs3_ver.txt, trimmed; "" none */
    int chosen_model, chosen_cache;        /* installed */
    int plus_present;                      /* Demucs v4 in rekordbox's model folder */
    int cache_present;                     /* the live DLL is the bridge (its own text) */
    int r1_known;                          /* r1 was read (a current rekordbox) */
    compat_r1 r1;
    int have_lists;                        /* a verified payload.json gave the lists below */
    const payload_manifest *m;             /* its compatible lists (NULL: none) */
    const char *dismissed;                 /* dismissed's text (NULL: none or not UTF-8) */
    size_t dismissed_n;
    const char *version;                   /* this watcher's version, for the run line */
} watch_facts;

typedef struct {
    void *ctx;
    int text_os;   /* the texts' column (core_os): CORE_OS_WIN in the exe; the Mac tests also run
                    * the questions on the Mac column's words */
    /* the facts, read now: 0, or nonzero when they couldn't be had (the reader logged why) */
    int (*facts)(void *ctx, watch_facts *f);
    /* watcherSettle's: an update or uninstall of rekordbox runs, RB Stems Plus is busy, rekordbox
     * runs (any session), an uninstall of ours left half-done (its id), wait, rekordbox's session
     * under way (the prepared-playlists check) */
    int32_t (*updating)(void *ctx);
    int32_t (*app_busy)(void *ctx);
    int32_t (*rekordbox_open)(void *ctx);
    int32_t (*half_uninstalled)(void *ctx, char *id, size_t cap);
    void (*pause)(void *ctx, int32_t seconds);
    void (*in_session)(void *ctx);
    /* the notifications' flags since the last take, cleared (watch_sched_take under the lock) */
    int (*take)(void *ctx);
    /* flags taken but left for a later set (a set that ends at its third round): noted again
     * (watch_sched_note under the lock), so the next set looks at them */
    void (*give_back)(void *ctx, int flags);
    /* the dialog (the first button the default): index of the button pressed, or -1 (closed, or it
     * couldn't be shown) */
    int (*ask)(void *ctx, const char *title, const char *message, const char *const *buttons, int n);
    /* dismissed's new text: in the exe its lines are added to the file as it is now, under the
     * answer lock (dismissed_add_locked), so a line the app wrote meanwhile (R1's Don't Ask Again,
     * N10) is kept: 0 or an errno */
    int (*remember)(void *ctx, const char *text, size_t n);
    /* Write "reinstall" or "cache" and start the app under one hold of the answer lock
     * (the stop event waits for it): 0,
     * WATCH_NO_REQUEST, or WATCH_NO_APP (written, but the app didn't start) */
    int (*request)(void *ctx, const char *name);
    /* RB Stems Plus started (through the start stub): 0 or an errno */
    int (*open_app)(void *ctx);
    /* the current version folder's name for diagnostics */
    void (*set_current)(void *ctx, const char *name);
    void (*log)(void *ctx, const char *line);
    void (*refresh_app)(void *ctx); /* quiet notification to an app that is already open */
} watch_ops;

/* What the watcher remembers while it runs: R1's openings of the app, one per state of the live
 * DLL and `ort\`'s copy per version folder (the app's own offer decides from there; a declined
 * offer isn't made again until that state changes or the watcher starts again, at the next logon
 * or update). */
typedef struct {
    unsigned char rows_digest[32], row_digests[INV_MAX][32], list_digest[32];
    int nrows, inv_complete;
    char r1_opened[INV_MAX * (COMPAT_VERSION_MAX + 32)];   /* keys, one per line */
    char observed_folder[WATCH_PATH_MAX];
    char observed_version[COMPAT_VERSION_MAX];
    int observed, rb_state, cache_present, supported, live, ort_copy, offer_restore;
} watch_state;

/* One set of rounds: the questions put off ("Remind Me Later"), asked again from the next set on
 * (on the Mac, the next start of the watcher). */
typedef struct {
    watch_state *st;
    char put_off[INV_MAX * (COMPAT_VERSION_MAX + 80)];   /* keys, one per line */
    int stash;            /* flags taken between rounds, for the next one */
} watch_set;

/* watcherCheck, one round of set s. flags: WATCH_* it was started for; more are taken once it has
 * settled. 1: done, a change since may be looked at; 0: stop this set (busy, an uninstall left
 * half-done, the app opened). */
int watch_check(const watch_ops *o, watch_set *s, int flags);
/* runWatcher's rounds (watcher_rounds, at most WATCH_MAX_ROUNDS): one set. The rounds run. */
int watch_rounds(const watch_ops *o, watch_state *st);

/* R1's key for the version folder's facts ("r1:7.2.19 live=missing ort=0"); its length */
size_t watch_r1_key(const char *version, const compat_r1 *r, char *out, size_t n);
/* watch_r1_dismissed_key, the line R1's Don't Ask Again writes ("r1:7.2.19"): dismissed.h, shared
 * with the core DLL's writer (N10) */

#ifdef WATCH_TEST_HOOKS
/* Test builds only (watch.sh): the question's text with this id (a core_text_id) reads as not
 * written in its column, as a Windows "" does; -1: none */
extern int watch_test_unwritten_text;
#endif

/* -------------------------------------------------------------------------------- the facts
 * watch_facts.c: the facts a round reads, on Windows, over compat.c, payload_store.c and
 * common/plat.h's file layer (the Mac tests run it on a scratch tree and a fake compat_os). */

typedef struct {
    const compat_os *os;
    const char *rekordbox;   /* <Program Files>\rekordbox */
    const char *root;        /* <Program Files>\RB Stems Plus */
    const char *models;      /* %APPDATA%\Pioneer\rekordbox6\models\demucs3_model */
    const char *support;     /* %LOCALAPPDATA%\rbstemsplus */
    const sig_pubkey *keys;  /* the compiled-in keys payload.json is checked with */
    size_t nkeys;
    const char *version;     /* this install's version (the oldest payload.json accepted) */
    /* a process's start time as the busy marker writes it (seconds since 1970), -1 when there is
     * no such process */
    int64_t (*process_start)(void *ctx, int32_t pid);
    void (*log)(void *ctx, const char *line);
    void *ctx;
} watch_env;

/* what the reader keeps between rounds; zeroed before the first, watch_reader_free after the last */
typedef struct {
    payload_manifest saved, installed;   /* payload\'s and installed-release\'s, checked again */
    char *dismissed;
    size_t dismissed_n;
} watch_reader;

/* the facts, read now (watch_ops.facts); 0, or nonzero when they couldn't be had (logged) */
int watch_read_facts(const watch_env *env, watch_reader *r, watch_facts *f);
void watch_reader_free(watch_reader *r);
/* appBusy: the busy marker's writer still runs */
int32_t watch_app_busy(const watch_env *env);
/* markedAction's id when it starts with "uninstall" (1, into id), else 0 */
int32_t watch_half_uninstalled(const watch_env *env, char *id, size_t cap);
/* support's file name, replaced whole with text (a temp name, flushed, renamed over): 0 or an errno */
int watch_write_support(const watch_env *env, const char *name, const char *text, size_t n);
/* the Mac's ourModelSize: Demucs v4's hdemucs.onnx (models.c's, when W2 writes it) */
enum { WATCH_OUR_MODEL_SIZE = 308572524 };

/* -------------------------------------------------------------------------------- watch_win.c */

/* The compiled-in keys payload.json is checked with, written by the build as build-app.sh writes
 * Keys.swift (watch.sh writes the release and backup keys for its gate builds; the release build
 * is W12's). */
extern const sig_key_text watch_compiled_keys[];
extern const int watch_n_compiled_keys;
/* A notification (Shell_NotifyIconW, NIF_INFO; Windows 11 shows it as a toast), from any thread:
 * clicking it opens RB Stems Plus. For the prepared playlists' check, which comes with Prepare
 * (Watcher.swift checkKeptSets); the watcher's test build shows one with --notify-test. 0, or -1
 * when it couldn't be posted. */
int watch_notify(const char *title, const char *body);

/* -------------------------------------------------------------------------------- the minimum */

/* app.md §5.9: Windows 11 22H2 (build 22621) and .NET Framework 4.8.1 (Release 533320) */
enum { WATCH_MIN_BUILD = 22621, WATCH_MIN_NET_RELEASE = 533320 };
/* NULL when build (RtlGetVersion's; 0 unknown) and release (the NDP v4 Full Release value; 0
 * none) are enough, else the one log line saying why the watcher doesn't run */
const char *watch_os_refusal(uint32_t build, uint32_t release);

/* -------------------------------------------------------------------------------- small rules */

/* Shell.swift appendLog's line: "yyyy-MM-dd HH:mm:ss  " + line + "\n", for a local time given as
 * parts; the length written (out has at least 24 bytes more than line) */
size_t watch_log_line(char *out, size_t cap, int y, int mo, int d, int h, int mi, int s, const char *line);
/* the stop event's name (app.md §5.4 "As built", update_win.c's side): "Local\io.github.
 * rbstemsplus.watcher.stop.<pid>"; the length, 0 when it doesn't fit */
size_t watch_stop_event_name(uint32_t pid, char *out, size_t n);
/* the start stub of the install this watcher belongs to: exe is
 * <Programs>\RB Stems Plus\<version>\rbsp-watch-<arch>.exe, the stub <Programs>\RB Stems Plus\RB
 * Stems Plus.exe. 1 with the path in out, or 0. */
int watch_stub_path(const char *exe, char *out, size_t n);

#endif
