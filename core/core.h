/* Frozen: API v1, 2026-11-09, at commit f3c8759 (after the review and re-review 1; see
 * core/API-V1-NOTES.md). Signatures, struct layouts, the hand-written enums' values and the rules
 * below don't change; new entry points and .def rows are only added (core_ids_hash checks the
 * generated ids).
 *
 * RB Stems Plus: the shared C core, API v1.
 *
 * One C11 core under two thin shells: AppKit (Swift; this header is the bridging header and the
 * core is linked statically, it exports nothing outside the app) and WPF on .NET Framework 4.8.1
 * (P/Invoke into rbsp-core.dll, x64 and arm64). The design: mac-core.md §3.
 *
 * ABI rules, for both shells (so the same declarations serve Swift and P/Invoke):
 * - 64-bit targets only (arm64, x86_64). Calling convention cdecl (the only one on x64 and arm64).
 * - Only scalars, opaque handles and pointers cross the boundary. No struct is passed or returned
 *   by value, no varargs, no C bool: a truth value is an int32_t, 0 or 1 (C#: int, never bool).
 * - size_t is C#'s UIntPtr (nuint) everywhere: parameters, callback parameters, out parameters.
 *   Never int or uint (the high bits would be garbage on x64 and arm64).
 * - Enums are C ints (C#: int). Their values are generated from the .def tables, per build: ids
 *   are positions, so the C# ids must come from the same build's generator (core/tools/gen_ids.c,
 *   which also emits the anonymous constants below). Each shell checks core_ids_hash() and
 *   core_api_version() at start and refuses a mismatch. Dumps and traces use names, never numbers.
 * - Strings are UTF-8. In: pointer + byte length, where a length of CORE_NTS means "NUL-terminated"
 *   (so C# can pass LPUTF8Str, or byte* + length, the G1.10 fallback). Callbacks receive
 *   NUL-terminated strings that also come with their length where the table says so.
 *   Out: a pointer to NUL-terminated UTF-8 plus its byte length (an out parameter, may be NULL).
 *   C# declares every returned string as IntPtr/byte*, never as a MarshalAs return (the marshaler
 *   would free the core's memory), and reads it with Encoding.UTF8.GetString(byte*, int).
 *   Each "out" string says how long it lives.
 * - Buffer-out functions follow one pattern: they write at most cap bytes, always NUL-terminate
 *   when cap > 0 (on errors too: then the buffer holds ""), store the full length (without the
 *   NUL) in *len and return CORE_OK, or CORE_E_SPACE when the result was cut short.
 * - No exception or unwind may leave a callback. Every C# delegate is a try/catch wrapper that
 *   returns its entry's safe value (below, "on failure"); Swift callbacks can't throw across C.
 *
 * Threads (mac-core §3.2). "main": the AppKit main thread / the WPF dispatcher thread, the thread
 * core_new ran on. Every function says where it may be called: main, flow, worker, any. A
 * main-only entry called from another thread aborts in CORE_DEBUG builds and, in release builds,
 * returns CORE_E_THREAD (or its documented safe value). core_event and render never block and
 * never show a dialog.
 *
 * Inside a callback on main (render, front_facts, modal_open, facts, close_reply, post_main, log)
 * the host may call only: the snapshot accessors, core_snap_retain/_release, core_fact_*,
 * core_facts_parts, core_menu_state/_count/_at, the pure functions (core_text, core_mask, core_api_version,
 * core_ids_hash, core_selftest), and core_job_drop (inside post_main/post_work, when the host's
 * queue is closed). Never core_event, core_commit, core_run, core_snap_current or core_free.
 * core_close_asked is allowed there in one case only: it answers CORE_CLOSE_NOW for a `what` the
 * core has already approved with close_reply(what, 1). Anything else is a violation: CORE_DEBUG
 * builds abort; release builds refuse a nested core_event (CORE_E_STATE, logged once), ignore a
 * nested core_commit/core_snap_current (NULL), queue a nested core_run of a main job (a nested
 * message pump) and run it, in order, when the outermost core call returns, and defer a nested
 * core_free to that point too.
 * Inside a callback on a worker (dialog, pick_folder, admin, step, slow_fact) the host may call
 * core_fact_*, core_facts_parts, core_reply_*, core_dlg_*, core_flow_progress, core_admin_note (inside admin only;
 * added after the freeze, M6) and the pure functions.
 * Inside helper_spawn and helper_signal (any thread), the host may call only the pure functions.
 *
 * Ownership (mac-core §3.4, §3.10): every handle the core gives out is either borrowed for the
 * duration of a callback, or a counted reference that the receiver releases exactly once. Each is
 * marked below. No manual frees otherwise: memory the core gives out is the core's.
 *
 * Lifetime: `ctx` and every callback in the host table stay valid until every job the core posted
 * has been run or dropped and every flow and helper run has ended. In practice the shells keep
 * them for the process lifetime.
 */
#ifndef RBSP_CORE_H
#define RBSP_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define CORE_CALL __cdecl
#  if defined(CORE_BUILD_DLL)
#    define CORE_API __declspec(dllexport)
#  else
#    define CORE_API
#  endif
#else
#  define CORE_CALL
#  define CORE_API      /* the Mac builds with -fvisibility=hidden: linked into the app, exported by nothing */
#endif

/* ------------------------------------------------------------------------------------------------
 * Version, errors, small enums
 */

#define CORE_API_VERSION 1u
#define CORE_NTS ((size_t)-1)   /* a length meaning "the string is NUL-terminated" */

enum {
    CORE_OK = 0,
    CORE_E_ARG = -1,       /* a bad argument: unknown id, out of range, NULL where not allowed, a bad event member */
    CORE_E_THREAD = -2,    /* called from the wrong thread (release builds; debug builds abort) */
    CORE_E_PARSE = -3,     /* malformed input (event JSON, config JSON) */
    CORE_E_UNKNOWN = -4,   /* well-formed, but an unknown event, key or value */
    CORE_E_SPACE = -5,     /* the buffer was too small: *len holds the full length */
    CORE_E_STATE = -6,     /* not now: inside a callback, shutting down, or the role doesn't have it */
    CORE_E_NOMEM = -7,
    CORE_E_NOTYET = -8     /* declared in API v1, implemented by a later work item */
};

typedef enum core_os { CORE_OS_MAC = 0, CORE_OS_WIN = 1 } core_os;

/* The process role a core is made for. The watcher (the same binary on the Mac) gets no
 * snapshots, no tables and no render: only its rounds and its one dialog (mac-core §3.11). */
typedef enum core_role { CORE_ROLE_APP = 0, CORE_ROLE_WATCHER = 1 } core_role;

/* CORE_API_VERSION as compiled into the core. The shell refuses a mismatch. Any thread. */
CORE_API uint32_t CORE_CALL core_api_version(void);

/* An FNV-1a hash over every .def row whose id crosses the boundary (sections, tables, fields,
 * columns, slots, texts, menus, facts; events cross by name and are left out), with each row's
 * table, name, kind, OS tag, owner and position, as this build has them. core/tools/gen_ids.c writes the same value into the shell's generated ids; the shell
 * refuses a mismatch at start (a stale DLL or ids file). Any thread. */
CORE_API uint64_t CORE_CALL core_ids_hash(void);

/* ------------------------------------------------------------------------------------------------
 * Generated ids (X-macros over the .def tables, mac-core §3.1)
 *
 * A row is tagged all, mac or win. A win row exists only when CORE_WIN_POLICY is defined (the
 * Windows builds and the core's host test build, never scripts/build-app.sh's Mac build), so a
 * Windows-only row never reaches the Mac objects, ids included. The tags are pasted, never
 * expanded, so no other header's macros can change them.
 */
#define CORE_KEEP_all(...) __VA_ARGS__
#define CORE_KEEP_mac(...) __VA_ARGS__
#ifdef CORE_WIN_POLICY
#  define CORE_KEEP_win(...) __VA_ARGS__
#else
#  define CORE_KEEP_win(...)
#endif

/* Snapshot sections (schema.def SECTION rows). Each has its own revision. */
typedef enum core_section {
#define SECTION(id, doc) id,
#include "schema.def"
    CORE_SECTION_COUNT
} core_section;

/* Tables: rows with a list_gen and a per-row rev (schema.def TABLE rows). */
typedef enum core_table {
#define TABLE(id, section, os, doc) CORE_KEEP_##os(id,)
#include "schema.def"
    CORE_TABLE_COUNT
} core_table;

/* Snapshot fields, read with core_str / core_flag / core_i64 / core_f64 (schema.def FIELD rows). */
typedef enum core_field {
#define FIELD(id, section, kind, os, doc) CORE_KEEP_##os(id,)
#include "schema.def"
    CORE_FIELD_COUNT
} core_field;

/* Row columns, read with core_cell_* (schema.def COLUMN rows). A column belongs to one table. */
typedef enum core_column {
#define COLUMN(id, table, kind, os, doc) CORE_KEEP_##os(id,)
#include "schema.def"
    CORE_COLUMN_COUNT
} core_column;

/* User-facing texts (texts.def), and the named slots they may use ({version}, {drive}, ...).
 * Slots are OS-tagged like every other row. The includer's own TEXT (windows.h's, when it comes
 * first) is set aside and restored around the table. */
#pragma push_macro("TEXT")
#undef TEXT
typedef enum core_text_id {
#define TEXT(id, os, mac, win) CORE_KEEP_##os(id,)
#include "texts.def"
    CORE_TEXT_COUNT
} core_text_id;

typedef enum core_slot_id {
#define SLOT(name, os) CORE_KEEP_##os(CORE_SLOT_##name,)
#include "texts.def"
    CORE_SLOT_COUNT
} core_slot_id;
#pragma pop_macro("TEXT")

/* Menu items (menus.def). */
typedef enum core_menu_id {
#define MENU(id, os, menu, kind, title, key_mac, key_win, mods_mac, mods_win, event) CORE_KEEP_##os(id,)
#include "menus.def"
    CORE_MENU_COUNT
} core_menu_id;

/* Facts the host reports: the cheap ones on every refresh, the slow ones from workers, and the
 * results of steps (facts.def). */
typedef enum core_fact {
#define FACT(id, kind, os, doc) CORE_KEEP_##os(id,)
#include "facts.def"
    CORE_FACT_COUNT
} core_fact;

/* ------------------------------------------------------------------------------------------------
 * Handles
 */
typedef struct core core;                 /* one per process role; main thread only (except where marked) */
typedef struct core_snap core_snap;       /* an immutable, counted snapshot */
typedef struct core_delta core_delta;     /* what moved since the previous render; owned by its snapshot */
typedef struct core_job core_job;         /* a unit of work the host runs once: core_run, or core_job_drop */
typedef struct core_facts core_facts;     /* a fact sheet the host fills with core_fact_* (borrowed) */
typedef struct core_dialog core_dialog;   /* a dialog to show (borrowed for the dialog callback) */
typedef struct core_reply core_reply;     /* the dialog's answer, filled by the host (borrowed) */

/* ------------------------------------------------------------------------------------------------
 * Host callbacks (mac-core §3.5)
 *
 * The shell fills one table and passes it to core_new, which copies it (the table itself needn't
 * outlive the call; C# keeps its delegates in static fields for the process lifetime). Every entry
 * gets the table's ctx first.
 * Required for CORE_ROLE_APP: render, front_facts, modal_open, facts, post_main, post_work,
 * dialog, close_reply, log. For CORE_ROLE_WATCHER: post_main, post_work, dialog, close_reply,
 * facts, step, log. NULL never means "native": it only means "not provided", and a flow that
 * needs a missing entry fails cleanly. Under CORE_WIN_POLICY only, the start config's "native"
 * list hands post_work, slow_fact, step, admin and helper (helper_spawn and helper_signal) to the
 * DLL's own Windows code; a listed entry must then be NULL in the table. Windows-only entries are
 * appended at the end under CORE_WIN_POLICY, so the Mac layout never changes.
 *
 * Host queues: while the core lives, the host queues every job it is given. post_main and post_work
 * never drop a job of any kind. A host that can't queue one then (out of memory) fails fast. C#:
 * log, then Environment.FailFast. Only once the host's loop has stopped (shutdown) do post_main
 * and post_work drop every pending and later job with core_job_drop, synchronously, inside the
 * callback. WPF flow and worker threads are background threads.
 *
 * Dialogs: the host presents dialog and pick_folder calls one at a time, in call order (the quit
 * flow, the normal flow and the watcher share one modal count). Once the host is shutting down,
 * dialog returns nonzero.
 *
 * Close and quit (core_close_asked): close_reply always comes from its own main job, never from
 * inside core_close_asked. The shell never closes the window or quits synchronously inside
 * close_reply: it posts the action (BeginInvoke / DispatchQueue.main.async; the Mac's
 * reply(toApplicationShouldTerminate:) is already asynchronous). A repeated ask while a decision
 * for that `what` is pending returns LATER and gets the one reply already coming. QUIT supersedes
 * a pending WINDOW, and the core answers both, with QUIT's decision. While modal_open() is 1 the
 * quit flow waits until no dialog is up before it asks; Quit stays disabled under an app-modal
 * dialog, as today. On the Mac, the shell's CriticalWork wait still follows a NOW or a proceed.
 * Windows logoff (WM_QUERYENDSESSION) is app.md §5.9's own bounded path, outside these answers.
 */

/* post_work kinds: the host maps each to its own queue and QoS (the Mac: GCD; Windows: thread
 * pool). An unknown kind runs as CORE_WORK_FACT. */
typedef enum core_work {
    CORE_WORK_FLOW = 0,     /* THE flow thread: serial, FIFO, one at a time (today's `work` queue) */
    CORE_WORK_QUIT = 1,     /* a short-lived flow beside the flow thread (quit or close during a Prepare run, §3.7) */
    CORE_WORK_FACT = 2,     /* slow facts (model sha, drive probe, AlphaTheta check), short steps: user-initiated QoS */
    CORE_WORK_WALK = 3,     /* the stems size walk: utility QoS */
    CORE_WORK_BACKGROUND = 4
} core_work;

/* front_facts bits: the raw facts, read live at the call. The core decides "in front" (§3.6). */
enum {
    CORE_FRONT_ACTIVE = 1u << 0,         /* NSApp.isActive / an activated app */
    CORE_FRONT_WINDOW_LOADED = 1u << 1,  /* the main window's views exist */
    CORE_FRONT_MINIATURIZED = 1u << 2,   /* the main window is minimized */
#ifdef CORE_WIN_POLICY
    CORE_FRONT_ELEVATING = 1u << 3,      /* Windows only (W-core): a UAC elevation is in flight, from
                                            ShellExecuteEx until the worker's exit is handled (app.md §4.3) */
#endif
    CORE_FRONT_TAB_SHIFT = 8,            /* bits 8-15: the tab the window shows now (live), a core_tab.
                                            F_TAB is the tab the core asks for. The two differ until
                                            that render is applied, and the in-front decision reads
                                            this one. */
    CORE_FRONT_TAB_MASK = 0xFFu << 8
};
typedef enum core_tab { CORE_TAB_STATUS = 0, CORE_TAB_PREPARE = 1, CORE_TAB_MANAGE = 2 } core_tab;

/* Slow facts the core asks a worker for. Results come back as facts in a core_facts sheet. */
typedef enum core_slow {
    CORE_SLOW_MODEL_SHA = 0,    /* key: the model's path. Memoised by path, size, mtime (§3.5) */
    CORE_SLOW_BRIDGE_SHA = 1,   /* key: the bridge's path. Memoised the same way */
    CORE_SLOW_DRIVE = 2,        /* key: the location. Drive state and free space, never memoised */
    CORE_SLOW_SIZE_WALK = 3,    /* key: the cache root. Its own policy (§3.5) */
    CORE_SLOW_ALPHATHETA = 4    /* key: the path checked. SecStaticCode on the Mac. Memoised */
#ifdef CORE_WIN_POLICY
    /* Windows only (W-core): R1's check (app.md §5.2), key: the current rekordbox version folder,
     * or "" (the app's ask, review WR1): the host finds the current one itself (the core DLL's
     * discovery, as BRIDGE_SHA and STATE_LINE do); no folder is no answer (-1).
     * FACT_R1_STATE (facts.def): what the live onnxruntime.dll and ort\'s copy are. Asked at start,
     * at activation, on the watcher's notifications and when an action ends; the answer is never
     * cached (computed on each ask) from compat.c's file facts, memoised by path, size, mtime and
     * identity as the watcher reads them (compat.h: for R1's offer only; the restore worker reads
     * fresh). With a non-zero state, FACT_R1_DISMISSED (N10): that folder's version is in
     * dismissed (R1's "Don't Ask Again for This Version", CORE_STEP_WIN_R1_DISMISS) */
    , CORE_SLOW_R1 = 5
#endif
    /* added (M4): LocationMonitor's cheap state, measured now (measureCheapState: config.ini, the
     * chosen folder's probe, rekordbox running and the bridge's state for it, the folder in use's
     * floor), key "". The facts.def rows the cheap sheet carried from the host's snapshot:
     * FACT_LOC_MEASURED (1), FACT_LOC_KIND, FACT_LOC_PROBE, FACT_LOC_STATUS_DRIVE, FACT_LOC_DRIVE,
     * FACT_LOC_BRIDGE_STATE, _REASON, _DRIVE, FACT_REKORDBOX_RUNNING, FACT_LOC_PAUSED_FLOOR,
     * FACT_LOC_IN_USE. The core keeps the memo, measures again at start, on activation, on a
     * mount, unmount, rename or bridge-state event and after an action, and checks the tabs when
     * it moved (LocationMonitor.onChange). The stems' size is the core's too: CORE_SLOW_SIZE_WALK of
     * FACT_LOC_IN_USE on CORE_WORK_WALK when the folder in use changed, when asked (start, every 10
     * minutes, LOCATION_REMEASURE) or when the last walk is 10 minutes old. Nonzero: the host keeps
     * its own snapshot in the cheap facts and sends {"ev":"location"}. CORE_SLOW_NOT_NOW (Windows):
     * not measured this time; the core keeps its memo, its size and its 10-minute timer, and the
     * snapshot stands until a measure answers; since the DLL drops a late answer, the core asks
     * again after 30 s, doubling to 10 minutes */
    , CORE_SLOW_LOCATION = 6
} core_slow;

/* CORE_SLOW_LOCATION's answer "not measured this time" (Windows; review NW1): see above */
#define CORE_SLOW_NOT_NOW 2

/* Platform steps the core asks the host for (§3.5 step). Arguments are strings and results are
 * facts, documented per kind by M6 before the shells implement them. Additive: new kinds are
 * appended. Open URL, notify, reveal and the manifest download run as short worker jobs outside
 * the one-flow lock (the Help menu, notifications and the update check work during an action). */
typedef enum core_step {
    CORE_STEP_LAUNCH_AGENT_LOAD = 0,
    CORE_STEP_LAUNCH_AGENT_UNLOAD = 1,
    CORE_STEP_DOCK = 2,
    CORE_STEP_SELF_DELETE = 3,
    CORE_STEP_MODEL_COPY = 4,
    CORE_STEP_DOWNLOAD = 5,
    CORE_STEP_REPORT_ZIP = 6,
    CORE_STEP_OPEN_URL = 7,
    CORE_STEP_NOTIFY = 8,      /* a notification (Notification Center / a toast), never a dialog */
    CORE_STEP_REVEAL = 9,      /* show a file in Finder / Explorer */
    /* Added after the freeze (M6, additive). Each kind's arguments and results: "The flows'
     * steps" below. Flags are "0"/"1"; a step's result facts are cleared before the call. */
    CORE_STEP_REKORDBOX = 10,
    CORE_STEP_PAYLOAD = 11,
    CORE_STEP_BLOCKED = 12,
    CORE_STEP_SPACE = 13,
    CORE_STEP_PRUNE_PAYLOADS = 14,
    CORE_STEP_CACHE_PREPARE = 15,
    CORE_STEP_MODEL_RESTORE = 16,
    CORE_STEP_MODELS_INFO = 17,
    CORE_STEP_SET_CHOSEN = 18,
    CORE_STEP_CONFIG_WRITE = 19,
    CORE_STEP_CONFIG_SETS = 20,
    CORE_STEP_BUSY_MARKER = 21,
    CORE_STEP_STATE_LINE = 22,
    CORE_STEP_CACHE_FOLDERS = 23,
    CORE_STEP_LOCATION_NOW = 24,
    CORE_STEP_REMOVE_LOCATION = 25,
    CORE_STEP_REMOVE_OWN_FOLDERS = 26,
    CORE_STEP_APP_FATE = 27,
    CORE_STEP_LEFTOVER = 28,
    CORE_STEP_REQUEST = 29,
    CORE_STEP_BRIDGE_SHA = 30,
    CORE_STEP_UPDATE_CHECK = 31,
    CORE_STEP_UPDATE_SWAP = 32,
    CORE_STEP_UPDATE_RESULT = 33,
    CORE_STEP_WELCOME = 34,
    CORE_STEP_PAUSED = 35,
    CORE_STEP_SETTINGS = 36,
    CORE_STEP_CLEAR_THIS_MAC = 37,
    CORE_STEP_LOCATION_REMEASURE = 38,
    CORE_STEP_LOCATION_PROBE = 39,
    CORE_STEP_LOCATION_DISCOVER = 40,
    CORE_STEP_LOCATION_SIZE = 41,
    CORE_STEP_LOCATION_WRITABLE = 42,
    CORE_STEP_LOCATION_CREATE = 43,
    CORE_STEP_LOCATION_WRITE = 44,
    CORE_STEP_THIS_MAC_DELETE = 45,
    CORE_STEP_WATCHER_CHECK = 46,
    CORE_STEP_MIGRATE_LEGACY = 47,
    CORE_STEP_REMOVE_LEFTOVERS = 48,
    /* added (M4): Prepare's dialogs */
    CORE_STEP_FREE_SPACE = 49,
    CORE_STEP_STOP_KEEPING = 50,
    /* added (M4): the host's date style */
    CORE_STEP_DATE_TEXT = 51,
    /* added (the M7 review's M72): the reminders line's measurement on activation */
    CORE_STEP_WATCHER_STATE = 52,
    /* added (the watcher role over the core, M7's switch-over): CORE_ROLE_WATCHER's steps */
    CORE_STEP_WATCH_LOCK = 53,
    CORE_STEP_WATCH_PAUSE = 54,
    CORE_STEP_WATCH_BUSY = 55,
    CORE_STEP_WATCH_FACTS = 56,
    CORE_STEP_WATCH_REMEMBER = 57,
    CORE_STEP_WATCH_REINSTALL = 58,
    CORE_STEP_WATCH_KEPT = 59,
    CORE_STEP_WATCH_HELPER = 60,
    CORE_STEP_REPORT_FACTS = 61
#ifdef CORE_WIN_POLICY
    /* Windows only (N10, review WCM3): R1's "Don't Ask Again for This Version" (below) */
    , CORE_STEP_WIN_R1_DISMISS = 62
    /* Windows only (multi-version rekordbox, .orchestration/mv-spec.md §2.4): an action's scope
     * over every installed rekordbox, and the last admin run's per-version outcome (below) */
    , CORE_STEP_WIN_SCOPE = 63
    , CORE_STEP_WIN_ADMIN_TARGETS = 64
#endif
} core_step;

/* The flows' steps (M6): arguments, results, and what each one does today (the Swift function
 * it runs). Steps return codes, numbers and raw facts; the core writes every text from them.
 * "flow" marks the steps that use the flow's payload.json and bridge bytes: those of the last
 * PAYLOAD "latest"/"saved" and the last CACHE_PREPARE on the flow thread (the host keeps them;
 * "check" and "fetch" never change them; a new flow replaces them at its first PAYLOAD). One
 * normal flow runs at a time.
 *
 *   OPEN_URL         args[0] the URL (NSWorkspace.open; openHelp)
 *   LAUNCH_AGENT_LOAD    installWatcher(): the LaunchAgent written and registered (it logs itself).
 *                    Added (M72): FACT_STEP_FLAG the reminders' state afterwards, as WATCHER_STATE
 *                    measures it; not set: the core leaves the reminders line as it is
 *   LAUNCH_AGENT_UNLOAD  removeWatcher(); FACT_STEP_FLAG as LOAD's (M72)
 *   DOCK             args[0] the app's path: removeFromDock. FACT_STEP_FLAG: it was in the Dock
 *   SELF_DELETE      args[0] the app to delete ("" none), args[1] "1" the Dock restarts: afterExit
 *                    of ownPrefsDeleteScript (+ selfDeleteScript, + dockRestartScript), then the app
 *                    quits at once without asking anything (today's ownQuit + terminate, posted).
 *                    0: the cleanup is set to run and the app quits; nonzero: it couldn't be
 *                    started (Windows: the after-exit stub didn't launch): the host doesn't quit,
 *                    and the core logs one line, leaves the app idle and, when args[0] names the
 *                    app, says so in a one-button alert (T_SELF_DELETE_FAILED).
 *   MODEL_COPY      flow: Models.installOurs from the payload's model. 0 done; else FACT_STEP_CODE
 *                    a core_model_outcome, FACT_STEP_TEXT the OS error where the outcome has one
 *   DOWNLOAD         flow: args[0] "model" | "bridge": fetchAsset. 0 ok, 1 offline, 2 the disk is
 *                    full, 3 damaged. Progress: core_flow_progress(DOWNLOAD, done, total) where
 *                    fetchAsset calls status: done -1 "Checking …", done -2 "Downloading …"
 *                    (no percent yet), else done bytes of total (the percent)
 *   REPORT_FACTS     what the report's lights are measured from, now (reportFacts' measurement,
 *                    which REPORT_ZIP then uses as it is: the location with its size, the model's
 *                    sha256): FACT_STEMS_PLUS_PRESENT, FACT_STEMS_CACHE_PRESENT, FACT_CHOSEN_CACHE,
 *                    FACT_CONFIG_ENABLED, FACT_CONFIG_RB_MODEL, FACT_ENGINE, FACT_BRIDGE_MODELS
 *                    (Stems Cache installed and its bridge lists them; unset: an older bridge),
 *                    FACT_STEP_TEXT the model's sha256 ("" none or not one), the location's facts as
 *                    the cheap sheet carries them (FACT_LOC_MEASURED, _KIND, _PROBE, _STATUS_DRIVE,
 *                    _BRIDGE_*, _DRIVE, FACT_REKORDBOX_RUNNING) with FACT_LOC_PAUSED_FLOOR the floor
 *                    the folder in use is under now (-1 none). Nonzero: not measured (no report)
 *   WIN_R1_DISMISS   (Windows only, N10) R1's "Don't Ask Again for This Version" / "for These
 *                    Versions" (app.md §5.2, §5.7): args[0] the versions its button showed at the
 *                    click (F_WIN_R1_DISMISS_VERSIONS, comma-separated). For each one whose folder
 *                    R1 still holds now, the line "r1:<version>" added to dismissed, its other
 *                    lines kept (the reinstall question's writer, under the answer lock); a version
 *                    not named is never written. 0 written; 1 nothing to dismiss (none named, or
 *                    nothing to put back now in those); 2 not written (FACT_STEP_CODE the errno).
 *                    FACT_STEP_TEXT the versions written, ", "-joined ("" none). Any worker
 *   WIN_SCOPE        (Windows only, multi-version rekordbox) args[0] "install" | "reinstall" |
 *                    "uninstall" | "restore": the rows that action covers now, by inventory.c's
 *                    target rule over a fresh inventory (the worker computes the same set again).
 *                    FACT_WIN_SCOPE the scope token for host->admin's version, FACT_WIN_SCOPE_VERSIONS
 *                    its versions, FACT_WIN_SCOPE_AFFECTED (uninstall, restore) the versions holding
 *                    our bridge or needing repair. FACT_STEP_CODE 0 ok, 1 the inventory is
 *                    incomplete (nothing may be changed now; for uninstall only when no folder
 *                    holds our bridge with its receipt: those are put back, the root stays, D3).
 *                    Nonzero: couldn't be checked. Flow
 *   WIN_ADMIN_TARGETS (Windows only, multi-version rekordbox) the last admin run's @target records:
 *                    FACT_STEP_TEXT one line per target, version, code ("-" not started) and what
 *                    (installed, already, put-back, kept, not-started, refused), tab-separated, each
 *                    ending in "\n"; FACT_STEP_FLAG the shared root was kept (@root-kept). Flow
 *   REPORT_ZIP       buildReport, with the lights the core decided from REPORT_FACTS: args[0] the
 *                    Demucs v4 light's look ("0" on, "1" limited, "2" off), args[1] its text, args[2]
 *                    and args[3] the Stems Cache light's (on Windows the core measures R1 itself,
 *                    CORE_SLOW_R1, so the light matches the window's), args[4] "1" when the Stems Cache light's
 *                    text is the location's (the issue's link names the location's state instead:
 *                    it names a drive). 0 made: FACT_STEP_PATH the zip, FACT_STEP_TEXT the zip as
 *                    the log and the dialog show it (redact), FACT_STEP_URL the issue's URL ("" none);
 *                    nonzero: not made
 *   REVEAL           args[0] a path: activateFileViewerSelecting
 *   NOTIFY           (M4) args[0] the title, args[1] the body: notify(title:body:) (a notification,
 *                    asking macOS for permission the first time; never a dialog). Any worker
 *   REKORDBOX        FACT_REKORDBOX_OPEN rekordboxOpen(), FACT_PIONEER_UPDATING pioneerUpdating().
 *                    Nonzero: couldn't be checked (the core stops the action)
 *   PAYLOAD          args[0] "latest": fetchManifestChecked, falling back to savedManifest() (the
 *                    one found becomes the flow's); "saved": savedManifest() (becomes the flow's);
 *                    "check" (the launch's checkForUpdate, any worker): savedManifest() and
 *                    noteShownVersion, FACT_STEP_FLAG manifestDue; "fetch" (its background
 *                    download, any worker): fetchManifest, not the flow's. Windows "refresh-compat"
 *                    is the same background fetch, only for an untried unsupported version/list pair.
 *                    Every payload.json found
 *                    is recorded by manifestSeen (the version shown). FACT_PAYLOAD_FETCH (0
 *                    downloaded and verified, 1 not verified, 2 offline or not downloaded),
 *                    FACT_PAYLOAD_FOUND, FACT_PAYLOAD_AGE (manifestAge in whole seconds; not set
 *                    when unknown), FACT_PAYLOAD_VERSION, FACT_PAYLOAD_SIGNED_BY,
 *                    FACT_PAYLOAD_APP_VERSION, FACT_PAYLOAD_BRIDGE_SHA, FACT_PAYLOAD_BRIDGE_HERE
 *                    (verified(bridgePath)), FACT_PAYLOAD_MODEL_SHA; FACT_STEP_PATH rekordbox's
 *                    model file (modelDir + "/hdemucs.onnx"), the key of CORE_SLOW_MODEL_SHA
 *   BLOCKED          args[0] "plus": stemsPlusBlocked()'s facts: FACT_ENGINE (engineVersion(), ""
 *                    none), FACT_ENGINE_SUPPORTED. "cache": stemsCacheBlocked()'s, in its order
 *                    (the host may stop at the first that fails, so codesign runs only when Swift's
 *                    would): FACT_REKORDBOX_SUPPORTED, FACT_ROOT_ORT_EXISTS, FACT_PROTOTYPE_EXISTS,
 *                    FACT_LIB_EXISTS, FACT_LIB_SIGNED (signedByPioneer), FACT_ORT_VERSION
 *                    (dylibVersion; not set when unreadable), FACT_ORT_PREFIX. Windows:
 *                    FACT_ORT_LISTED in place of FACT_ORT_PREFIX (Pioneer's ORT's sha256 in the
 *                    verified payload's windows.compatible.ort_sha256), decided by
 *                    cache_blocked_win. FACT_REKORDBOX_VERSION is the version checked in this read,
 *                    FACT_STEP_TEXT is its X.Y.Z folder identity for elevation. The core decides (plus_blocked / cache_blocked) with the
 *                    cheap facts
 *   SPACE            flow: args[0] "1" the model is copied, args[1] "1" Stems Cache: spaceShort's
 *                    measure. home's need (the model: its missing download, its copy and Pioneer's
 *                    saved model) and /Library's need (Stems Cache) are summed when both are on one
 *                    volume, else checked separately, home first. The first volume whose free bytes
 *                    are below need + 200 MiB (200 << 20) gives FACT_STEP_CODE = need + 200 MiB. A
 *                    volume with unknown free space, or with need 0, is skipped. 0 when there's
 *                    room. The core writes "about N GB" (rounded up to 0.1)
 *   PRUNE_PAYLOADS   flow: when the payload's model is here (modelPath exists), prunePayloads(keep:)
 *   CACHE_PREPARE    flow: Pioneer's ONNX Runtime's sha256 (rootOrt once the bridge is in, else
 *                    rbLib; logged) and the payload's bridge read once and checked; the admin
 *                    install of this flow uses exactly those bytes. 0 ok: FACT_STEP_TEXT ORT's sha
 *                    ("unreadable" when it can't be read); 1: the bridge changed after its check
 *   MODEL_RESTORE    installedModels.restore(). 0 put back; else FACT_STEP_CODE a
 *                    core_model_outcome, FACT_STEP_TEXT the OS error where it has one
 *   MODELS_INFO      args[0] "saved": only FACT_ORIGINALS_SAVED (savedOriginals non-empty or one unknown, no
 *                    hashing). No args: FACT_ORIGINALS_SAVED (likewise), FACT_ORIGINALS_VERIFIED
 *                    (originalsKept: a copy verifies or can't be checked, the folder stays), FACT_STEP_CODE what modelLeftBehind finds (0 no
 *                    model, 1 unreadable, 2 Demucs v4, 3 one RB Stems Plus doesn't know),
 *                    FACT_STEP_PATH the originals folder with the home as "~", FACT_STEP_URL
 *                    modelHelpURL
 *   SET_CHOSEN       args[0] model, args[1] cache: setChosen
 *   CONFIG_WRITE     args key, value, ... (pairs: "rekordbox_model" "1"; "max_gb" "1.5" "max_days"
 *                    "30", the numbers as the user typed them with "." for a decimal mark):
 *                    writeConfigValues (writeConfig, configNumber). 0 written: FACT_STEP_TEXT the
 *                    pairs as written ("max_gb=1.5 max_days=30"); else FACT_STEP_PATH configFile
 *   CONFIG_SETS      args[0] a key: configSets. FACT_STEP_FLAG
 *   BUSY_MARKER      args[0] the action's id, args[1] its label; no args: removed (setBusyMarker)
 *   STATE_LINE       FACT_STEP_TEXT state()
 *   CACHE_FOLDERS    FACT_STEP_FLAG rekordbox's folder is there, FACT_STEP_FLAG2 the root folder
 *   LOCATION_NOW     LocationMonitor.current() (a drive probe now) beside its background snapshot:
 *                    FACT_LOC_DIR status.dir ("" this Mac), FACT_LOC_DRIVE its .drive,
 *                    FACT_STEP_TEXT2 driveName(dir), FACT_STEP_TEXT redact(dir), FACT_LOC_GATE_ID
 *                    (the deletable gate's marker id, "" none); when the snapshot's size is of the
 *                    same folder, FACT_LOC_SIZE_KIND (StemsSize: 0 measuring, 1 measured, 2 failed,
 *                    3 not connected; -1 or not set: none) and FACT_LOC_SIZE its bytes. Added (M4):
 *                    with the core's location memo (CORE_SLOW_LOCATION), args[0] is the memo's
 *                    folder in use ("" none) and "the same folder" is that one: FACT_LOC_SIZE_KIND
 *                    4 then means "the core's size" (its own walk), which the core fills in
 *                    Windows also sets FACT_LOC_MEASURED (0 when there was no drive measure)
 *                    and FACT_STEP_PATH with the opened folder's identity when giving a gate.
 *   REMOVE_LOCATION  args[0] dir, args[1] id, args[2] "1" the whole folder: removeInLocation. 0 ok;
 *                    else FACT_STEP_CODE why (1 not connected, 2 the drive can't be opened, 3 it
 *                    can't be read, 4 an entry couldn't be opened: FACT_STEP_TEXT2 its name,
 *                    FACT_STEP_TEXT the OS error, 5 an entry is on another drive: FACT_STEP_TEXT2,
 *                    6 the marker or folder identity isn't the one expected, 7 some files couldn't be deleted).
 *                    Windows requires args[3], the identity returned with the confirmed gate.
 *                    Progress: core_flow_progress(DELETE, files, 0)
 *   REMOVE_OWN_FOLDERS  args[0] "1" keep the originals: removeOwnFolders. 0 all went
 *   APP_FATE         FACT_STEP_CODE 0 deleted (FACT_STEP_PATH the app), 1 another account's,
 *                    2 can't delete (ownBundle, ownedByThisAccount, access). A Windows host never
 *                    answers 1 (the app is installed per account), so T_FATE_OTHER_ACCOUNT, Mac-only,
 *                    is never composed there
 *   LEFTOVER         leftoverAction(); when there is one, also `sudo -k`. FACT_STEP_TEXT its id
 *                    ("" none), FACT_STEP_TEXT2 its label
 *   REQUEST          args[0] "take": takeReinstallRequest; else reinstallRequestAge. FACT_STEP_CODE
 *                    the age in seconds, -1 none. On read, FACT_STEP_TEXT is the request's line
 *                    ("model\n", "cache\n" or "model cache\n"; omitted, empty or "reinstall\n" is legacy).
 *   BRIDGE_SHA       FACT_STEP_TEXT sha256(rbLib), "" unreadable. Windows "outdated" and the
 *                    payload's bridge digest return a differing receipt-backed bridge digest or "".
 *   UPDATE_CHECK     flow ("saved"): ownBundle and its folder writable. FACT_STEP_CODE 0 ok, 1 not
 *                    in its own place, 2 the folder isn't writable
 *   UPDATE_SWAP      flow: the app's zip downloaded (progress as DOWNLOAD), unpacked and checked,
 *                    then updateScript after exit and the app quits (as SELF_DELETE). Returns only
 *                    on failure: 1 offline, 2 the disk is full, 3 anything else (damaged, not the
 *                    expected app)
 *   UPDATE_RESULT    update/result read and the update folder removed (checkUpdateResult).
 *                    FACT_STEP_FLAG an update failed, FACT_STEP_TEXT its "failed <key>" line
 *   WELCOME          args[0] "check": FACT_STEP_FLAG the welcome was shown before; "done": the
 *                    marker file written
 *   PAUSED           the background snapshot's (LocationMonitor) pausedFloor and drive: FACT_STEP_CODE
 *                    the floor in bytes (-1 not paused), FACT_STEP_TEXT the chosen drive ("" this Mac).
 *                    Not asked once the core keeps the memo (M4)
 *   SETTINGS         args[0] "open": readConfigState(log) (it logs config.ini's ignored lines), else
 *                    without the log; the Settings sheet's facts now (the snapshot, settingsStems):
 *                    FACT_SET_GB and FACT_SET_DAYS (configNumber of config.ini's values),
 *                    FACT_SET_UNREADABLE, FACT_SET_CAN_CHOOSE (!installed || bridgeSaysLocation),
 *                    FACT_STEP_FLAG (Windows: this build disables the drive chooser),
 *                    FACT_SET_SIZE_KIND (as FACT_LOC_SIZE_KIND, 4 included; -1 none) and FACT_SET_SIZE (bytes),
 *                    FACT_SET_CLEAR (0 nothing, 1 this Mac's folder, 2 this Mac without its folder,
 *                    3 the chosen folder: FACT_LOC_DIR and FACT_LOC_GATE_ID its gate, FACT_LOC_DRIVE
 *                    the snapshot's drive or its name, FACT_STEP_PATH redact of it,
 *                    on Windows FACT_STEP_TEXT2 the opened folder's identity),
 *                    FACT_SET_LOCATION (0 This Mac, 1 a chosen folder, 2 config.ini unreadable, 3
 *                    set wrongly) with FACT_SET_LABEL_PATH for 1 (the folders between the volume's
 *                    mount point and the chosen folder's parent, '/'-separated, "" none; the core
 *                    joins the drive and them with " › " as locationLabel does), FACT_STEP_TEXT
 *                    driveName of config.ini's chosen folder ("" none)
 *                    Windows kind 4 binds the memo's size to the measured FACT_LOC_IN_USE.
 *                    Windows "config" (review N3R18): only FACT_SET_LOCATION, FACT_SET_LABEL_PATH
 *                    and FACT_STEP_TEXT (the remembered drive name), without a measure.
 *   CLEAR_THIS_MAC   safeRemove(defaultCacheDir, whole). 0 ok; FACT_STEP_FLAG cacheIsLink()
 *   LOCATION_REMEASURE  LocationMonitor.invalidateSize(), then update(size: true) (the core's memo,
 *                    when it keeps one, does the same itself)
 *   LOCATION_PROBE   args[0] the folder chosen: realPath, volumeFacts, locationRefusalFor.
 *                    FACT_STEP_CODE 0 may be used, else the refusal (1 internal disk, 2 network,
 *                    3 read-only, 4 Time Machine, 5 inside our folder, 6 a link, 7 file system,
 *                    8 path, 9 unreadable); FACT_STEP_TEXT the real path, FACT_STEP_TEXT2 redact of
 *                    the folder chosen, FACT_LOC_DRIVE the volume's name (else the folder's last
 *                    component; on Windows, for a volume that was read, native_location_drive_name's:
 *                    its label, else its letter ("E:"), else "", never the last component, so the
 *                    core refuses it), FACT_STEP_PATH its mount point
 *   LOCATION_DISCOVER  args[0] the real path, args[1] the mount point: discover() with config.ini's
 *                    location (readConfigState(log)) and the hint. FACT_STEP_CODE 0 none, 1 not
 *                    ours, 2 found (FACT_LOC_DIR, FACT_LOC_GATE_ID); FACT_STEP_TEXT the current
 *                    chosen folder ("" This Mac), FACT_STEP_TEXT2 its id, FACT_LOC_DRIVE its
 *                    drive's name ("" none), FACT_STEP_PATH redact of the folder found
 *   LOCATION_SIZE    args[0] a folder: FACT_STEP_CODE entriesSize (0 unknown); "this_mac": of
 *                    defaultCacheDir
 *   LOCATION_WRITABLE  args[0] a folder: FACT_STEP_FLAG locationWritable
 *   LOCATION_CREATE  args[0] the real path: createLocationFolder, then excludeFromBackup. 0 made:
 *                    FACT_LOC_DIR, FACT_LOC_GATE_ID; else FACT_STEP_CODE 1 a folder appeared
 *                    meanwhile, 2 mkdir failed (FACT_STEP_TEXT the OS error), 3 the marker couldn't
 *                    be written (FACT_STEP_TEXT the OS error); FACT_STEP_PATH redact of the real path
 *   LOCATION_WRITE   args dir, id, mount, created ("1"); dir "" for This Mac: writeCacheLocation,
 *                    then the hint (a drive) or the pending-deletion flag removed (This Mac). On a
 *                    failed write a folder this flow created is removed. 0 written: FACT_STEP_TEXT
 *                    redact(dir); else FACT_STEP_PATH configFile
 *   THIS_MAC_DELETE  args[0] "check": FACT_STEP_FLAG thisMacDeletionNow; "delete": removeThisMacStems,
 *                    on failure the pending-deletion flag is written (nonzero); "defer": the flag
 *                    written; "unflag": safeRemove(pendingDeletionFile); "facts" (the launch retry,
 *                    retryThisMacDeletion's inputs): FACT_STEP_FLAG the pending-deletion flag exists,
 *                    FACT_STEP_FLAG2 bridgeSaysLocation(), FACT_STEP_CODE 1 when
 *                    LocationMonitor.current()'s status is .chosen(_, _, .available) (else 0),
 *                    FACT_STEP_TEXT its status code (status?.code ?? "?"), FACT_THIS_MAC_MODEL_FOLDERS
 *                    thisMacHasModelFolders(). The core decides (this_mac_needs_cleanup,
 *                    may_retry_this_mac_deletion) and logs
 *   WATCHER_CHECK    checkWatcher() then updateWatcherState() (they log themselves). Added (M4):
 *                    FACT_STEP_FLAG updateWatcherState's `off` (watcherOff; 0 before macOS 13). The
 *                    core logs its line and shows the reminders line when it changes
 *   MIGRATE_LEGACY   installedModels.migrateLegacy(): FACT_STEP_FLAG 1.0's saved copy turned out to
 *                    be ours and no saved copy is there or can't be checked (the core shows
 *                    T_NOTICE_NO_SAVED_MODEL for this run)
 *   REMOVE_LEFTOVERS removeLeftovers() (it logs itself)
 *   FREE_SPACE       (M4) args[0] a folder: FACT_STEP_CODE freeBytes(folder), the bytes free on its
 *                    volume. Nonzero: unknown (the core counts 0, as `?? 0` does)
 *   STOP_KEEPING     (M4) args[0] a kept set's name: stopKeeping(name) (its .kept and .manifest
 *                    removed from the kept folder). 0: its .kept is gone; nonzero: it is still there
 *   DATE_TEXT        (M4) args[0] a local date, YYYY-MM-DD: FACT_STEP_TEXT that date as the OS
 *                    writes a date in its medium style (the Mac: a DateFormatter with dateStyle
 *                    .medium and timeStyle .none in the current locale, PrepareView's dateOnly).
 *                    Nonzero or no text: the core writes YYYY-MM-DD
 *   WATCHER_STATE    (M72) updateWatcherState's measurement alone, never checkWatcher's repair:
 *                    FACT_STEP_FLAG watcherOff (0 before macOS 13). Asked on activation as a short
 *                    job on the flow queue, behind any running flow (checkWatcherOn's work.async);
 *                    the core logs the line and shows the reminders line when it changes.
 *                    Nonzero: nothing changes (a host without it)
 * The watcher's (CORE_ROLE_WATCHER, Watcher.swift's watcherCheck and watcherRounds; the host took
 * the watcher's lock and left its again-mark before core_new, as takeWatcherLock does):
 *   WATCH_LOCK       args[0] "release" (safeRemove(watcherLock)), "again" (FACT_STEP_FLAG: the
 *                    again-mark is there), "retake" (FACT_STEP_FLAG: mkdir(watcherLock) worked),
 *                    "clear_again" (safeRemove(watcherAgainFile): what this check sees starts here)
 *   WATCH_PAUSE      args[0] seconds: sleep them, then touch the lock (its age says it is alive)
 *   WATCH_BUSY       FACT_STEP_FLAG appBusy(); FACT_STEP_TEXT the busy marker's action id
 *                    (markedAction), "" none
 *   WATCH_FACTS      what one check reads: FACT_CHOSEN_MODEL, FACT_CHOSEN_CACHE,
 *                    FACT_STEMS_PLUS_PRESENT, FACT_STEMS_CACHE_PRESENT, FACT_REKORDBOX_VERSION
 *                    (rekordboxVersion()), FACT_ENGINE (engineVersion()), FACT_ENGINE_SUPPORTED,
 *                    FACT_REKORDBOX_SUPPORTED (the compatible lists), FACT_ORT_PREFIX, FACT_STEP_PATH
 *                    the lists' rekordbox versions and FACT_STEP_URL their STEMS Engine versions
 *                    (one a line, as unsupportedKey joins them), FACT_STEP_TEXT dismissed's text,
 *                    FACT_STEP_TEXT2 the run line's version (versionText(shown:)). Nonzero: not read
 *   WATCH_REMEMBER   args[0] dismissed's new text, written whole (remember). Nonzero: not written
 *   WATCH_REINSTALL  "Reinstall Now": args[0] the offered features ("model", "cache" or "model cache").
 *                    Write the request, then open the app once this process has exited (afterExit).
 *   WATCH_KEPT       the prepared playlists' check as rekordbox's session gets under way
 *                    (watcherSettle's inSession; Watcher.swift's checkKeptSets until M7's switch-over:
 *                    the core decides, the host reads and writes). args[0]:
 *                    "session": FACT_STEP_TEXT rekordbox's process id now ("" none), FACT_STEP_TEXT2
 *                      the last checked session's (kept.checked's text, "" none);
 *                    "seen" args[1] the id: kept.checked written with it;
 *                    "sets": FACT_KEPT_SETS keptSets() as the facts sheet carries it;
 *                    "facts": prepareFacts' reads for the watcher (it doesn't measure where the stems
 *                      are saved: no FACT_LOC_*, and no FACT_MODEL_SIZE: the model's sha256 isn't
 *                      worked out here): FACT_APPLE_SILICON, FACT_HELPER_THERE, the rekordbox facts
 *                      (_INSTALLED, _VERSION, _SUPPORTED), FACT_STEMS_CACHE_PRESENT,
 *                      FACT_STEMS_PLUS_PRESENT, FACT_CHOSEN_CACHE, FACT_ENGINE, FACT_BRIDGE_KEEPS,
 *                      FACT_ROOT_ORT_EXISTS, config.ini's (FACT_CONFIG_*), FACT_BRIDGE_MODELS (with
 *                      rekordbox's own model), FACT_STEMS_QUALITY, FACT_OUR_MODELS;
 *                    "noticed": FACT_STEP_TEXT kept.noticed's bytes ("" none);
 *                    "notice" args[1] kept.noticed's new text, written whole;
 *                    "running" args[1] a set's name: FACT_STEP_FLAG a helper preparing it runs now;
 *                    "notify" args[1] the title, args[2] the text: a notification (never a dialog
 *                      over rekordbox).
 *                    Nonzero: not done (a failed "session" or "sets": no check this time)
 *   WATCH_HELPER     args[0] the seconds it may take, args[1...] the prepare helper's arguments:
 *                    runPrepareHelper, waited for. FACT_STEP_CODE its exit status, FACT_STEP_TEXT
 *                    what it printed, FACT_STEP_TEXT2 its error output. Nonzero: it didn't start */

/* The rekordbox stems model steps' outcomes (MODEL_COPY, MODEL_RESTORE: Models.swift's messages,
 * M5's T_MODEL_* texts). The *_WHY ones carry the OS error in FACT_STEP_TEXT. */
typedef enum core_model_outcome {
    CORE_MODEL_OK = 0,
    CORE_MODEL_UNKNOWN = 1,          /* unknownModelMessage */
    CORE_MODEL_UNREADABLE = 2,
    CORE_MODEL_NOT_A_FILE = 3,
    CORE_MODEL_NO_SPACE_SAVE = 4,
    CORE_MODEL_NOT_SAVED = 5,
    CORE_MODEL_NO_SPACE_COPY = 6,
    CORE_MODEL_NOT_COPIED_WHY = 7,
    CORE_MODEL_COPY_BAD = 8,
    CORE_MODEL_CHANGED = 9,
    CORE_MODEL_NOT_PLACED_WHY = 10,
    CORE_MODEL_NO_SAVED_COPY = 11,
    CORE_MODEL_NO_SPACE_BACK = 12,   /* restore's noSpace */
    CORE_MODEL_NOT_BACK = 13,
    CORE_MODEL_NOT_BACK_WHY = 14,
    CORE_MODEL_NOT_VERIFIED = 15,
    CORE_MODEL_SAVED_DAMAGED = 16
} core_model_outcome;

/* admin kinds (host->admin's kind, M6). One call is one attempt: the password sheet when sudo
 * doesn't remember one (asked until right or cancelled), then the root script; the core runs the
 * attempts (rekordbox checked before each, "Permission needed" between them) and forgets the
 * password with CORE_ADMIN_FORGET at the end. On acceptance of the password (or when sudo
 * remembers it) the host calls core_flow_progress(ADMIN_ATTEMPT, 1, 0). The host logs the run's
 * result line ("  ok: …" / "  FAILED: … [exit n]") before it returns, and calls core_admin_note
 * when the run's output says "Operation not permitted". admin returns the plain RootExit code, as
 * frozen.
 * version: rekordbox's version as the root script checks it ($VERSION); bundle_id: rekordbox's
 * bundle id (NULL on Windows). The core passes rekordboxVersion() and "com.pioneerdj.rekordboxdj"
 * for the install, reinstall and uninstall, "" and "" for REMOVE_ROOT_FOLDER (bundle_id NULL on
 * Windows), NULL and NULL for FORGET.
 * Windows (W-core, app.md §5.5): one call is one elevation of the worker (UAC; no password, no
 * "Permission needed", no FORGET). The host calls core_flow_progress(ADMIN_ATTEMPT, 1, 0) once
 * the worker runs, core_flow_progress(STILL_CLOSING, 0, 0) on its @still-closing record and
 * core_flow_progress(STILL_CLOSING, 1, 0) once that rekordbox has closed, and
 * core_admin_note(RESTART_PENDING) on its @restart record. Besides the RootExit table, admin
 * returns the worker's appended codes: 12 it speaks another protocol, 14 the app's payload is
 * older than its floor (both: the core offers to update the app first), 13 install-worker put the
 * app's own worker in bin\ (the core runs the attempt once more, unless the host already did);
 * 1223 (ERROR_CANCELLED, UAC declined) or < 0: the user stopped, no alert. A worker that
 * couldn't be started (ShellExecuteEx failed for another reason) or crashed returns 1, never
 * < 0: the action failed, and the host's log has its result line with the raw code.
 * Multi-version rekordbox (Windows, .orchestration/mv-spec.md §2.2, §2.4): version carries
 * CORE_STEP_WIN_SCOPE's token (FACT_WIN_SCOPE), not one rekordbox's version (no ABI change), and
 * admin also returns 15 the worker's fresh scope differs (nothing changed: the core scopes again
 * and asks once more), 16 some targets done, one failed (CORE_STEP_WIN_ADMIN_TARGETS says
 * which) and 17 an install refused whose undo couldn't restore everything in the root (the core
 * says so, never "nothing changed"): pure.h's ROOT_WIN_SCOPE_CHANGED, ROOT_WIN_PARTIAL,
 * ROOT_WIN_UNDO_INCOMPLETE. */
typedef enum core_admin_kind {
    CORE_ADMIN_CACHE_INSTALL = 0,      /* cacheInstallScript with CACHE_PREPARE's bridge, worded "install" */
    CORE_ADMIN_CACHE_REINSTALL = 1,    /* the same, worded "reinstall" */
    CORE_ADMIN_CACHE_UNINSTALL = 2,    /* cacheUninstallScript */
    CORE_ADMIN_REMOVE_ROOT_FOLDER = 3, /* removeRootFolderScript */
    CORE_ADMIN_FORGET = 4              /* sudo -k: no password, no script; returns 0 */
#ifdef CORE_WIN_POLICY
    /* Windows only (W-core): the worker's restore verb (R1's "put Pioneer's file back", app.md
     * §5.5), version as for the uninstall. The core never asks FORGET on Windows (no password) */
    , CORE_ADMIN_RESTORE = 5
#endif
} core_admin_kind;

/* helper_signal values: portable, not raw signal numbers (Windows: NtSuspendProcess, stdin EOF). */
typedef enum core_helper_sig { CORE_HELPER_PAUSE = 0, CORE_HELPER_RESUME = 1, CORE_HELPER_STOP = 2 } core_helper_sig;

/* close_reply's `what`, and core_close_asked's. */
typedef enum core_close {
    CORE_CLOSE_WINDOW = 0,         /* windowShouldClose / Window.Closing */
    CORE_CLOSE_QUIT = 1,           /* applicationShouldTerminate / app exit asked */
    CORE_CLOSE_WATCHER_DONE = 2    /* the watcher's rounds are over: the process may exit */
} core_close;

typedef struct core_host {
    uint32_t size;      /* sizeof(core_host) as the shell compiled it; core_new refuses a smaller one */
    uint32_t reserved;  /* must be 0: core_new refuses anything else */
    void *ctx;          /* passed back as the first argument of every entry */

    /* main. A new snapshot and what moved since the previous render. The shell OWNS one reference
     * to snap (release it with core_snap_release, typically after the next render is applied);
     * delta lives as long as snap. Apply every render, in order, in one pass. On failure: release
     * snap, log, and reload everything from the next render (a shell that skips a render does the
     * same). Never blocks. */
    void (CORE_CALL *render)(void *ctx, core_snap *snap, const core_delta *delta);

    /* main, live: CORE_FRONT_* bits read at the moment of the call, never cached. On failure: 0. */
    uint32_t (CORE_CALL *front_facts)(void *ctx);

    /* main, live: 1 when a modal dialog or a sheet is up (NSApp.modalWindow / attachedSheet;
     * WPF: our modal count), else 0. On failure: 1. */
    int32_t (CORE_CALL *modal_open)(void *ctx);

    /* main, on every refresh: the cheap facts, written into out with core_fact_*: at least the
     * parts core_facts_parts(out) asks for (added after the freeze, M71). Never memoised.
     * On failure: return with the sheet as filled, and log. */
    void (CORE_CALL *facts)(void *ctx, core_facts *out);

    /* any thread: run job on main, later, in order (DispatchQueue.main.async / the dispatcher
     * queue). The host calls core_run(job) exactly once, or, once its loop has stopped,
     * core_job_drop(job). One render per drain. The Mac's drain is GCD's: the core's own commit
     * job renders and ends the turn. On Windows the commit job only wakes the drain. The WPF drain
     * runs jobs for at most 4-8 ms, then calls core_commit (the one render), then re-posts itself
     * if its queue isn't empty. */
    void (CORE_CALL *post_main)(void *ctx, core_job *job);

    /* any thread: run job on a worker of `kind` (core_work) with core_run, once; or, once the
     * host's loop has stopped, core_job_drop(job).
     * CORE_WORK_FLOW must be one serial FIFO thread or queue. */
    void (CORE_CALL *post_work)(void *ctx, core_work kind, core_job *job);

    /* flow (normal or quit) only, blocks until answered: show d, fill r with core_reply_*, return
     * 0. Presented through the app-modal presenter (§3.5: a scheduled runModal plus a run loop
     * wake-up on the Mac; ShowDialog from a separate dispatcher operation on Windows), never from
     * inside a core callback on main. Nonzero (also on failure and at shutdown): Cancel. */
    int32_t (CORE_CALL *dialog)(void *ctx, const core_dialog *d, core_reply *r);

    /* flow only, blocks: the folder picker (NSOpenPanel / IFileOpenDialog) starting at `start`
     * (may be empty), with its message and button texts from core_text (T_PICK_FOLDER_MESSAGE,
     * T_PICK_FOLDER_CHOOSE). The chosen path goes in with core_reply_field(r, 0, ...). Return 0
     * chosen, 1 cancelled, negative (also on failure): couldn't be shown. */
    int32_t (CORE_CALL *pick_folder)(void *ctx, const char *start, size_t start_len, core_reply *r);

    /* any core worker (any core_work kind, possibly concurrently: thread-safe): a slow fact for key
     * (core_slow kind). The result goes into out with core_fact_*. Return 0, or nonzero (also on
     * failure) when it couldn't be measured. */
    int32_t (CORE_CALL *slow_fact)(void *ctx, core_slow kind, const char *key, size_t key_len, core_facts *out);

    /* any thread (thread-safe): start the helper for run `run` with argv[0..argc-1] (argv[0] is the
     * first argument, not the program: the host knows its own helper's path). Return 0 when
     * started: exactly one core_helper_ended follows, after the run's last core_helper_line, both
     * from the host's reader thread. Nonzero: not started, and no core_helper_ended follows. Runs
     * may overlap. */
    int32_t (CORE_CALL *helper_spawn)(void *ctx, uint64_t run, const char *const *argv, int32_t argc);

    /* any thread (thread-safe): pause, resume or stop the helper of run `run` (core_helper_sig). The
     * core sends STOP from its own timers for today's per-operation deadlines. */
    void (CORE_CALL *helper_signal)(void *ctx, uint64_t run, core_helper_sig sig);

    /* flow only, blocks: one admin action with typed values only (the core never passes script
     * text): kind (M6's admin kinds), the release version, the bundle id (NULL on Windows), the
     * bridge's and ORT's sha256 (hex, or NULL), force (0/1). The password sheet and "Permission
     * needed" go through the asynchronous presenter. Progress goes through core_flow_progress.
     * Returns the RootExit outcome (>= 0), or < 0: cancelled at the password (also on failure). */
    int32_t (CORE_CALL *admin)(void *ctx, int32_t kind, const char *version, const char *bundle_id,
                               const char *bridge_sha, const char *ort_sha, int32_t force);

    /* any core worker (thread-safe; blocks that worker): a platform step (core_step) with string
     * arguments. Results, if the step has any, go into out with core_fact_*. Returns 0 on success,
     * else the step's own code (nonzero on failure). */
    int32_t (CORE_CALL *step)(void *ctx, core_step kind, const char *const *args, int32_t nargs, core_facts *out);

    /* main, from its own main job: the answer to a CORE_CLOSE_LATER (proceed 1: close the window
     * or terminate, posted out of this callback; 0: stay), or CORE_CLOSE_WATCHER_DONE (the
     * watcher's last callback: the host exits, no core_free needed). */
    void (CORE_CALL *close_reply)(void *ctx, core_close what, int32_t proceed);

    /* any thread: one line for the app's log, the one path for every core line. The shell writes
     * it, with its own lines, through one function to the log file and the log view (today's
     * Controller.log). Never holds the library key (§3.10). */
    void (CORE_CALL *log)(void *ctx, const char *line, size_t len);

#ifdef CORE_WIN_POLICY
    /* Windows-only entries (W-core) are appended here; they never change the Mac layout. */
#endif
} core_host;

/* ------------------------------------------------------------------------------------------------
 * Life cycle
 *
 * Start-up, the same in both shells: core_new; core_event {"ev":"start"} (reads the cheap facts
 * synchronously and sets the start state); core_commit (the first render, a full reload: the
 * shell builds its views from it); show the window; core_event {"ev":"shown"} (starts welcome,
 * the update result, resume, the request and the cache offer).
 * The watcher: core_new(CORE_ROLE_WATCHER); {"ev":"start"}; rounds, run by the core's own timer and
 * by {"ev":"watch"} from the host's change notification (Windows: ReadDirectoryChangesW); the last
 * callback is close_reply(CORE_CLOSE_WATCHER_DONE, 1), after which the host exits. The host runs a
 * main loop so post_main works. Rounds run as flows and read facts by posting to main.
 */

/* main. Makes the core for this process. host is copied. config is a small JSON object (the only
 * JSON at start), required, with "os" required:
 *   {"os":"mac","version":"1.4.0","paths":{"home":...,"support":...}}
 * paths: home, support, app, helper, cache_default, log, tmp, watcher (each optional; M4 says which
 * it needs); added (M4): model (rekordbox's model file, modelDir + "/hdemucs.onnx": the model's
 * sha256 key and the prepare helper's --model), root_ort (rootOrt), rekordbox (rbApp; on Windows: the current rekordbox version folder) and kept
 * (keptDir), the prepare helper's arguments; cache_default is defaultCacheDir. Under CORE_WIN_POLICY only: "os":"win" and "native":["post_work","slow_fact","step",
 * "admin","helper"]; added (W8b), with "os":"win" only: "prepare_tabs":true when the shell shows
 * the Status and Prepare tabs (the 1.5.0 release, a TEST build's prepare scenarios); false or
 * absent (a Manage-only host): core_menu_state answers CORE_MENU_HIDDEN, not enabled, for
 * View > Status, View > Prepare, the Prepare menu and its items, so their shortcuts and
 * {"ev":"menu"} events do nothing. Unknown keys are refused. Returns NULL on a bad host table or config (the
 * reason goes to host->log). The calling thread becomes the core's main thread. */
CORE_API core *CORE_CALL core_new(const core_host *host, core_role role, const char *config, size_t config_len);

/* main, outside callbacks (inside one it is deferred until the outermost core call returns).
 * Optional at process exit: the production shells don't call it on quit. Tears the model down at
 * once. After it: queued main jobs and worker jobs that haven't started are dropped (their
 * cleanup runs, not their work); a running flow, or a worker job that has started, carries on and
 * may still make the host call it was about to make (revalidate answers 0; the host's
 * dialog answers nonzero once it is shutting down); core_helper_line, core_helper_ended and
 * core_flow_progress stay legal for runs and flows that began before it, and do nothing. Never
 * pass `c` to a main-thread entry again. Snapshots the shell still holds stay valid until
 * released. */
CORE_API void CORE_CALL core_free(core *c);

/* Runs a job the core posted (post_main's on main, post_work's on its worker) and frees it. Once.
 * A main job run off main is re-posted through post_main. */
CORE_API void CORE_CALL core_run(core_job *job);

/* Frees a job without running its work (its cleanup still runs): the host's queue is closed. Any
 * thread. Once. */
CORE_API void CORE_CALL core_job_drop(core_job *job);

/* main, outside callbacks: if anything changed since the last render, freeze the model and call
 * render now, synchronously; otherwise nothing. The core still posts its own commit job on the
 * first change of a turn, so a turn always ends. On the Mac that job renders, and it is a no-op
 * after a core_commit. On Windows it renders nothing, and the drain's core_commit does. */
CORE_API void CORE_CALL core_commit(core *c);

/* Pure C self-test for `--swap` (G1.4): the tables, texts, the event parser, the arenas and a
 * snapshot round trip (about 30 us on an M-series Mac), with no core and no host. 0: passed; else
 * the number of the first failed check. Any thread. */
CORE_API int32_t CORE_CALL core_selftest(void);

/* ------------------------------------------------------------------------------------------------
 * Events in (mac-core §3.3): main, outside callbacks, never blocks, never shows a dialog.
 */

/* One low-rate event as a JSON object: {"ev":"activated"}, {"ev":"click","id":"install_cache"},
 * {"ev":"select","table":"T_PREP_OUTLINE","row":12,"gen":41}, ... (the names and members:
 * core/events.def). Every event that names a row carries "gen": the table's list_gen in the
 * snapshot the shell rendered; an event whose gen isn't the table's current list_gen is ignored
 * (logged quietly, no dialog) and returns CORE_OK. Returns CORE_OK, CORE_E_PARSE, CORE_E_UNKNOWN
 * (an unknown event or member) or CORE_E_ARG (a member of the wrong type or out of range); the
 * refused event is logged and ignored. CORE_E_STATE inside a callback, CORE_E_THREAD off main. */
CORE_API int32_t CORE_CALL core_event(core *c, const char *json, size_t len);

/* Window close or quit asked (main). Returns CORE_CLOSE_NOW (go ahead now), CORE_CLOSE_LATER
 * (return false / terminateLater: the core answers through host->close_reply, from its own main
 * job), or CORE_CLOSE_REFUSE (stay; also off main). Bypasses the flow queue (§3.7). The rules:
 * "Close and quit" above. */
enum { CORE_CLOSE_NOW = 0, CORE_CLOSE_LATER = 1, CORE_CLOSE_REFUSE = 2 };
CORE_API int32_t CORE_CALL core_close_asked(core *c, core_close what);

/* One line of the helper's output for run `run`, without its newline. Any thread (the host's
 * reader thread): parsed there, once, by the pure parser; the result goes to main as a typed job.
 * Whether a run is current is decided per run (or per purpose), never core-wide. */
CORE_API void CORE_CALL core_helper_line(core *c, uint64_t run, const char *line, size_t len);

/* The helper of run `run` exited with status, and err holds its stderr's tail. Any thread, once per
 * run started, after its last core_helper_line. */
CORE_API void CORE_CALL core_helper_ended(core *c, uint64_t run, int32_t status, const char *err, size_t err_len);

/* worker (inside admin or step): progress of the running action, posted to main. stage: a
 * core_progress value. */
typedef enum core_progress { CORE_PROGRESS_NONE = 0, CORE_PROGRESS_ADMIN_ATTEMPT = 1, CORE_PROGRESS_DOWNLOAD = 2, CORE_PROGRESS_COPY = 3,
                             CORE_PROGRESS_DELETE = 4 /* added (M6): done files deleted so far */
#ifdef CORE_WIN_POLICY
                             /* Windows only (W-core), inside admin: the worker waits for rekordbox's
                              * process, which lingers about 75 s after its window closes (the
                              * worker's @still-closing record, PLAN R1): done 0 "rekordbox is still
                              * closing…", done 1 it has closed (the attempt's own line again) */
                             , CORE_PROGRESS_STILL_CLOSING = 5
#endif
                             } core_progress;
CORE_API void CORE_CALL core_flow_progress(core *c, core_progress stage, int64_t done, int64_t total);

/* Added after the freeze (M6). flow only, inside admin: what the admin run said beside its exit
 * code. CORE_ADMIN_NOTE_NOT_PERMITTED: the run's output said "Operation not permitted" (App
 * Management refused it). The core clears the note before each attempt and reads it after admin
 * returns. Any other value is ignored. After core_free it does nothing. */
enum { CORE_ADMIN_NOTE_NOT_PERMITTED = 1 };
#ifdef CORE_WIN_POLICY
/* Windows only (W-core): the run removed the root but its own last file goes at the next restart
 * (the worker's @restart record, I20); the complete uninstall's last message says so */
enum { CORE_ADMIN_NOTE_RESTART_PENDING = 2 };
#endif
CORE_API void CORE_CALL core_admin_note(core *c, int32_t note);

/* ------------------------------------------------------------------------------------------------
 * Snapshots (mac-core §3.4)
 *
 * Immutable and counted. retain/release from any thread. Every pointer read from a snapshot
 * (strings, delta arrays) stays valid while the caller holds a reference to that snapshot.
 * Reading a field with the wrong accessor, or a row out of range, gives "" / 0 (and aborts in
 * CORE_DEBUG builds). One-shot view commands (scroll a row into view, focus the outline, show the
 * log) are a value field plus an i64 *_SEQ field: the shell acts once per new seq.
 */

/* main, outside callbacks: the current state as a snapshot, +1 (for the probe and tests; the shell
 * takes the one render gives it). */
CORE_API core_snap *CORE_CALL core_snap_current(core *c);
CORE_API void CORE_CALL core_snap_retain(core_snap *s);
CORE_API void CORE_CALL core_snap_release(core_snap *s);

/* A section's revision. The shell redraws only the sections whose revision moved. A change to one
 * of a section's tables moves the section's revision too. */
CORE_API uint64_t CORE_CALL core_snap_rev(const core_snap *s, core_section section);

/* Typed field accessors (ids from schema.def). No JSON on this path. */
CORE_API const char *CORE_CALL core_str(const core_snap *s, core_field f, size_t *len);
CORE_API int32_t CORE_CALL core_flag(const core_snap *s, core_field f);
CORE_API int64_t CORE_CALL core_i64(const core_snap *s, core_field f);
CORE_API double CORE_CALL core_f64(const core_snap *s, core_field f);

/* Tables. A new list_gen means: reload everything. Within one list_gen rows never move and never
 * go away: rows only change in place (their rev moves) or are appended at the end. */
CORE_API int32_t CORE_CALL core_rows(const core_snap *s, core_table t);
CORE_API uint64_t CORE_CALL core_list_gen(const core_snap *s, core_table t);
CORE_API uint64_t CORE_CALL core_row_rev(const core_snap *s, core_table t, int32_t row);

/* Cells: the column names its table. */
CORE_API const char *CORE_CALL core_cell_str(const core_snap *s, core_column col, int32_t row, size_t *len);
CORE_API int32_t CORE_CALL core_cell_flag(const core_snap *s, core_column col, int32_t row);
CORE_API int64_t CORE_CALL core_cell_i64(const core_snap *s, core_column col, int32_t row);
CORE_API double CORE_CALL core_cell_f64(const core_snap *s, core_column col, int32_t row);

/* The delta given with a render: relative to the snapshot of the previous render (the first
 * render reloads everything). */
CORE_API uint32_t CORE_CALL core_delta_sections(const core_delta *d);   /* bit (1 << section): its rev moved */
CORE_API int32_t CORE_CALL core_delta_reload(const core_delta *d, core_table t);   /* 1: list_gen moved, reload the table */
CORE_API int32_t CORE_CALL core_delta_old_rows(const core_delta *d, core_table t); /* rows before (0 on reload); [old_rows, core_rows) are appended */
/* The changed rows below old_rows, ascending, no repeats; NULL with *n = 0 when none, or when the
 * table reloads. */
CORE_API const int32_t *CORE_CALL core_delta_changed(const core_delta *d, core_table t, int32_t *n);

/* The snapshot as JSON, by names (tests, traces, the report; never the hot path). Buffer-out. */
CORE_API int32_t CORE_CALL core_snap_dump(const core_snap *s, char *buf, size_t cap, size_t *len);

/* ------------------------------------------------------------------------------------------------
 * Facts in (host → core): written by the host into a sheet the core lends it, during the facts,
 * slow_fact or step callback only. Strings are copied. A fact of the wrong kind is ignored (and
 * aborts in CORE_DEBUG builds). Any thread the callback runs on.
 */
CORE_API void CORE_CALL core_fact_flag(core_facts *out, core_fact f, int32_t v);
CORE_API void CORE_CALL core_fact_i64(core_facts *out, core_fact f, int64_t v);
CORE_API void CORE_CALL core_fact_str(core_facts *out, core_fact f, const char *v, size_t len);

/* Added after the freeze (the M7 review's M71, additive): the parts of the cheap sheet host->facts
 * is asked for, CORE_FACTS_* bits. The core asks each read for the parts its decision reads, as
 * 1.3.0 read them: Controller.refresh's rows (REFRESH), prepareFacts' own files besides them
 * (PREPARE), the Status tab's kept sets (KEPT). host->facts writes at least the parts asked for and
 * may write more (a host that writes every row stays correct). facts.def lists each part's rows (by
 * group comment, not by position).
 * 0 for a slow_fact or step sheet. Inside the facts callback, any thread. */
enum { CORE_FACTS_REFRESH = 1, CORE_FACTS_PREPARE = 2, CORE_FACTS_KEPT = 4 };
CORE_API uint32_t CORE_CALL core_facts_parts(const core_facts *f);

/* ------------------------------------------------------------------------------------------------
 * Dialogs (mac-core §3.7): read during the dialog callback; strings live until it returns.
 * One dialog form covers today's Sheet: title, message, labelled fields, rows of small buttons,
 * then the buttons (stacked, the first is the default and Return; or inline). Every button is
 * enabled unless the dialog says otherwise.
 */
typedef enum core_dlg_what {
    CORE_DLG_TITLE = 0,          /* text */
    CORE_DLG_MESSAGE = 1,        /* text, may be empty */
    CORE_DLG_BUTTON = 2,         /* text of button i */
    CORE_DLG_FIELD_LABEL = 3,    /* text of field i */
    CORE_DLG_FIELD_VALUE = 4,    /* initial text of field i */
    CORE_DLG_ROW_LABEL = 5,      /* text of row i */
    CORE_DLG_ROW_BUTTON = 6,     /* text of row i's button j */
    CORE_DLG_N_BUTTONS = 7,      /* count */
    CORE_DLG_N_FIELDS = 8,       /* count */
    CORE_DLG_N_ROWS = 9,         /* count */
    CORE_DLG_N_ROW_BUTTONS = 10, /* count of row i's buttons */
    CORE_DLG_DEFAULT = 11,       /* index of the default button (Return) */
    CORE_DLG_CANCEL = 12,        /* index of the Cancel button (Esc), or -1 */
    CORE_DLG_INLINE = 13,        /* 1: buttons in a row, the first on the right */
    CORE_DLG_ID = 14,            /* the core_text_id of the title (probe and lint) */
    CORE_DLG_BUTTON_ENABLED = 15,      /* button i: 1 enabled, 0 disabled */
    CORE_DLG_ROW_BUTTON_ENABLED = 16,  /* row i's button j: 1 enabled, 0 disabled */
    CORE_DLG_BUTTON_LEFT = 17          /* button i: 1 when an inline dialog places it on the left ("Don't Ask Again…") */
} core_dlg_what;
/* i and j index what `what` names; pass 0 when unused. Out of range: "" / 0. */
CORE_API const char *CORE_CALL core_dlg_text(const core_dialog *d, core_dlg_what what, int32_t i, int32_t j, size_t *len);
CORE_API int32_t CORE_CALL core_dlg_int(const core_dialog *d, core_dlg_what what, int32_t i, int32_t j);

/* The answer, set before the dialog callback returns (on any thread whose writes happen before
 * that return, e.g. main before it signals the flow thread). Not set: Cancel. */
CORE_API void CORE_CALL core_reply_button(core_reply *r, int32_t i);
CORE_API void CORE_CALL core_reply_row_button(core_reply *r, int32_t row, int32_t j);
CORE_API void CORE_CALL core_reply_field(core_reply *r, int32_t i, const char *v, size_t len);

/* ------------------------------------------------------------------------------------------------
 * Menus (mac-core §3.8): the table is static; states are pulled live.
 */
enum { CORE_MENU_ITEM = 0, CORE_MENU_SEPARATOR = 1, CORE_MENU_NATIVE = 2 };  /* native: the OS's own item, left to the shell */
enum { CORE_MOD_CMD_OR_CTRL = 1u, CORE_MOD_SHIFT = 2u, CORE_MOD_ALT = 4u };
enum { CORE_MENU_ENABLED = 1u, CORE_MENU_HIDDEN = 2u, CORE_MENU_CHECKED = 4u };

/* Static description of one menu item, for building the menus once. Its strings are static. */
typedef struct core_menu_item {
    int32_t id;          /* core_menu_id */
    int32_t menu;        /* core_menu_id of its parent (an item, at any depth), or -1 for a top-level menu */
    int32_t kind;        /* CORE_MENU_ITEM, _SEPARATOR, _NATIVE */
    int32_t title;       /* core_text_id, or -1 */
    const char *key;     /* key equivalent for this OS, UTF-8, "" for none */
    uint32_t mods;       /* CORE_MOD_*, for this OS */
    uint32_t reserved;
    const char *event;   /* the "id" of the {"ev":"menu"} event it sends, "" for none */
} core_menu_item;

/* Number of items for os, and item i of them in menu order (parents before children). Any thread. */
CORE_API int32_t CORE_CALL core_menu_count(core_os os);
CORE_API int32_t CORE_CALL core_menu_at(core_os os, int32_t i, core_menu_item *out);

/* main, live (validateMenuItem / SubmenuOpened / CanExecute; allowed inside callbacks): CORE_MENU_*
 * bits now, 0 off main. *title (may be NULL) gets the title now (Pause/Resume), valid until the
 * next core call on main. */
CORE_API uint32_t CORE_CALL core_menu_state(core *c, core_menu_id id, const char **title, size_t *title_len);

/* ------------------------------------------------------------------------------------------------
 * Pure functions: no core, no state, thread-safe (mac-core §3.2)
 */

/* A text for os with its slots filled. slots[i].id is a core_slot_id; a slot the text uses but
 * isn't given, or one given that the text doesn't use, is CORE_E_ARG. Slot values are inserted
 * as they are, never read as templates. Buffer-out. */
typedef struct core_slot { int32_t id; int32_t reserved; const char *value; size_t len; } core_slot;   /* reserved: 0, else CORE_E_ARG */
CORE_API int32_t CORE_CALL core_text(core_os os, core_text_id id, const core_slot *slots, int32_t nslots,
                                     char *buf, size_t cap, size_t *len);

/* Masking for Create Report (Report.swift's redact), with every input explicit: the context is
 * read during the call only. Buffer-out. The body is M5's. */
typedef struct core_mask_ctx {
    uint32_t size; uint32_t reserved;   /* sizeof(core_mask_ctx) as compiled; must be 0, else CORE_E_ARG */
    const char *home;                   /* NUL-terminated, may be NULL */
    const char *const *names;           /* the full name, then the account name (redact's order; each NUL-terminated) */
    int32_t n_names; int32_t n_allowed;
    const char *const *allowed;         /* checksums (hex) kept readable in this report */
} core_mask_ctx;                        /* 40 bytes, blittable */
CORE_API int32_t CORE_CALL core_mask(const core_mask_ctx *mc, const char *in, size_t in_len,
                                     char *buf, size_t cap, size_t *len);

/* ------------------------------------------------------------------------------------------------
 * Test hooks: test builds only (CORE_TEST_HOOKS, a separately named archive; never in a release
 * build). They drive the real snapshot, delta and commit machinery for the unit tests and the G1
 * API spike before the view model (M4) exists. main only, outside callbacks; each marks its
 * section or table dirty and schedules the turn's one commit, as a real change does.
 */
#ifdef CORE_TEST_HOOKS
CORE_API int32_t CORE_CALL core_test_set_str(core *c, core_field f, const char *v, size_t len);
CORE_API int32_t CORE_CALL core_test_set_i64(core *c, core_field f, int64_t v);   /* flag, i64 */
CORE_API int32_t CORE_CALL core_test_set_f64(core *c, core_field f, double v);
CORE_API int32_t CORE_CALL core_test_table_reset(core *c, core_table t, int32_t rows); /* new list_gen, rows empty cells */
CORE_API int32_t CORE_CALL core_test_table_append(core *c, core_table t, int32_t rows);
CORE_API int32_t CORE_CALL core_test_cell_str(core *c, core_column col, int32_t row, const char *v, size_t len);
CORE_API int32_t CORE_CALL core_test_cell_i64(core *c, core_column col, int32_t row, int64_t v);
/* Starts a test flow on the flow thread. CORE_TEST_FLOW_DIALOG: it shows a two-button dialog,
 * sends revalidate, and logs "test flow: button <i>, revalidate <0|1>". CORE_TEST_FLOW_STEP: it
 * calls host->step(CORE_STEP_OPEN_URL) and logs "test flow: step <rc>". */
enum { CORE_TEST_FLOW_DIALOG = 0, CORE_TEST_FLOW_STEP = 1 };
CORE_API int32_t CORE_CALL core_test_flow(core *c, int32_t mode);
/* A Prepare run in progress or not (core state on main), for the quit path. */
CORE_API int32_t CORE_CALL core_test_set_preparing(core *c, int32_t preparing);
/* The next n job allocations fail (allocation-failure tests). */
CORE_API int32_t CORE_CALL core_test_fail_allocs(core *c, int32_t n);
/* Added after the freeze (additive): after `skip` more job allocations succeed, the next `count`
 * (at least 1) fail, once (the "fail the Nth allocation" sweeps). A later call replaces it;
 * skip -1: none. */
CORE_API int32_t CORE_CALL core_test_fail_alloc_after(core *c, int32_t skip, int32_t count);
/* Starts a helper run through host->helper_spawn (argv: "--test"); its run id, or 0 when the host
 * refused it. Purpose 0..3: an independent "current" per purpose. */
CORE_API uint64_t CORE_CALL core_test_helper_start(core *c, int32_t purpose);
/* The number of lines applied on main for runs of `purpose` that were current. */
CORE_API int64_t CORE_CALL core_test_helper_lines(core *c, int32_t purpose);
/* Added after the freeze (additive; the G1 API spike, mac-core §9 G1.7). on 1 / off 0: a stub
 * parse on the reader thread turns each helper line into one typed main job that writes the
 * Prepare tracks table, as M5's parser will: {"file":P} appends a row (C_TRK_NAME: P's name,
 * C_TRK_RESULT: Checking…), {"file":P,"result":R,...} sets the next row's C_TRK_RESULT (each
 * purpose's rows count from 0 again at core_test_helper_start); anything else is only counted.
 * While it is on, an outline {"ev":"select"} ends the current run of purpose 2 (the spike's
 * estimate): its later lines are dropped and host->helper_signal gets CORE_HELPER_STOP. A spike
 * stand-in, not M5's parser; never a release path. Any thread. */
CORE_API int32_t CORE_CALL core_test_helper_stub(core *c, int32_t on);
/* Added after the freeze (additive, M4): the view model's wall clock, in seconds since 1970 (the
 * Prepare tab's "until" date and its argument); 0: the real clock again. main only. */
CORE_API int32_t CORE_CALL core_test_set_now(core *c, int64_t unix_seconds);
/* Added after the freeze (additive): the TEST builds' probe timing points the core decides. The
 * core calls mark(ctx, name) on main, from inside its own work, at each point: name
 * "trigger.actionEnd" when an action's busy state ends (busy(nil): view_busy_off), the point
 * today's Controller.busy(nil) marks. NULL: none. Not a core_host entry: core_host's frozen
 * 136-byte layout (the C# shell's, W0) stays as it is, and a release build has no trace of it.
 * main only. */
typedef void (CORE_CALL *core_test_mark_fn)(void *ctx, const char *name);
CORE_API int32_t CORE_CALL core_test_set_mark(core *c, core_test_mark_fn mark, void *ctx);
#endif

#ifdef __cplusplus
}
#endif
#endif /* RBSP_CORE_H */
