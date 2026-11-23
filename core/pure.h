/* The core's pure functions (M5, mac-core §3.2, §3.9): texts built from texts.def, the lights and
 * the gating, the helper's line grammar, the words of the Prepare and Status tabs, the state
 * files' formats and decisions, the watcher's decisions. No core, no host, no globals: each takes
 * its facts as arguments, so it is thread-safe (parsing runs on the helper's reader thread) and
 * tested against today's Swift (core/tests/swift: the vectors). Not part of the API.
 *
 * Each function names the Swift it ports (file and function); it behaves as that does, byte for
 * byte, unless its comment says otherwise. Texts come out in a tbuf, in the OS column asked for. */
#ifndef RBSP_CORE_PURE_H
#define RBSP_CORE_PURE_H

#include "core.h"

/* included by internal.h after its arena type */

/* ------------------------------------------------------------------ text builder (text.c) */

/* A text being built into a caller's buffer. An overflow sets err (len counts the piece that
 * overflowed); any other failure (a bad slot, a column not written yet) sets err too, and the
 * caller drops the result. Once err is set, tb_text adds nothing more. The buffer is always
 * NUL-terminated when cap > 0. */
typedef struct { char *p; size_t cap, len; int32_t err; core_os os; } tbuf;
void tb_init(tbuf *b, core_os os, char *p, size_t cap);
void tb_put(tbuf *b, const char *s, size_t n);
void tb_puts(tbuf *b, const char *s);
void tb_i64(tbuf *b, int64_t v);
/* texts.def's text `id` in b's OS column, its slots filled */
void tb_text(tbuf *b, core_text_id id, const core_slot *slots, int32_t nslots);
/* what a slot may take from t's buffer (SLOT_N(name, buf, tb_len(&t))): its text, 0 after an
 * error, never past the buffer */
size_t tb_len(const tbuf *t);
#define TB_TEXT0(b, id) tb_text((b), (id), NULL, 0)
#ifdef CORE_WIN_POLICY
/* Windows: a drive with no name ("": unplugged, no label remembered) takes its text's nameless
 * form, nameless_text(id), which has no {drive} (review NR2) */
#define TB_NAMELESS(b, drive) ((b)->os == CORE_OS_WIN && (drive) && !(drive)[0])
core_text_id nameless_text(core_text_id named);
/* Settings' choice (owner, 2026-11-15; review NT1): on Windows a picked folder whose drive would
 * be named "" (neither a label nor a letter: folder-mounted, or its letter removed) is refused
 * with T_WIN_REFUSE_NAMELESS; 1 refused. Asked for a probe that accepted the folder, and for its read-only and
 * file-system refusals (review N21) */
int location_refuse_nameless(core_os os, const char *drive);
#endif
/* one slot from a NUL-terminated value */
#define SLOT_S(name, v) ((core_slot){CORE_SLOT_##name, 0, (v), CORE_NTS})
#define SLOT_N(name, v, n) ((core_slot){CORE_SLOT_##name, 0, (v), (n)})

/* ------------------------------------------------------------------ numbers as Swift writes them (words.c) */

/* Swift's String(Double) (the shortest digits that read back, "1e-05", "20.5"): out has 32 bytes */
void fmt_swift_double(double x, char out[32]);
/* Config.swift configNumber: "20" for a whole number, else Swift's String(x) */
void fmt_config_number(double x, char out[32]);
/* "%.1f" / "%.2f" / "%.3f" style, as String(format:) gives them: out has 64 bytes */
void fmt_fixed(double x, int decimals, char out[64]);

/* ------------------------------------------------------------------ the helper's lines (helper_lines.c)
 *
 * helper/prepare.c's output, one JSON object per line, read as Prepare.swift reads it:
 * JSONSerialization's grammar (duplicate keys: the first wins; a trailing comma is allowed; at most
 * 512 levels), its number classes (int64, uint64, double; NSNumber's intValue, doubleValue,
 * boolValue and stringValue), and invalid UTF-8 replaced first as String(decoding:as:) does.
 * Known differences, all outside what the helper prints: integers beyond the uint64 range or
 * with over 19 digits and a fraction (NSDecimalNumber's own conversions), a NUL inside a string
 * (kept here; the texts stop at it). The line is untrusted (the helper runs Pioneer's code): every
 * input is bounded, nothing recurses, fuzzed (core/tests/fuzz.c). */

typedef struct { const char *s; size_t n; } pstr;   /* NUL-terminated too; may hold a NUL (\u0000) */

/* String(decoding:as: UTF8.self): each maximal ill-formed subsequence becomes U+FFFD. The text
 * itself when it is valid; NULL when out of memory. */
const char *utf8_repair(arena *a, const char *s, size_t n, size_t *out_n);

/* Prepare.swift TrackLine */
typedef struct {
    pstr file, result, why, access, folder;
    int64_t entries, written, already, missing, refused;
    double seconds, gb;
} prep_track;

/* Prepare.swift RunLine */
enum { RUN_NONE = 0, RUN_PROGRESS = 1, RUN_TRACK = 2, RUN_ROOM = 3, RUN_DONE = 4 };
typedef struct {
    int32_t kind;
    int64_t at, files, chunk, chunks;                 /* RUN_PROGRESS */
    prep_track track;                                 /* RUN_TRACK */
    int64_t removed; double cache_gb;                 /* RUN_ROOM */
    int64_t done; int32_t has_stopped; pstr stopped;  /* RUN_DONE */
    double kept_gb; int32_t has_error; pstr error;
} prep_run_line;

/* Prepare.swift Playlist (--playlists) */
typedef struct { pstr id, parent, name, kind; int64_t tracks; } prep_playlist;

/* Prepare.swift Estimate: built line by line (prep_estimate_line), then prep_estimate_ended. */
typedef struct {
    prep_track *tracks; int32_t ntracks, captracks;
    int32_t has_refused; pstr refused;
    int64_t ready, prepare, skipped, unchecked, asking, fit_files;
    double time_s, need_gb, kept_gb, share_gb, max_gb, max_gb_needed;
    int32_t fits, disk_ok;
    pstr *folders; int32_t nfolders;
    int32_t ended;        /* an "estimate" line came */
    int32_t done;         /* a "refused" line came: later lines change nothing */
} prep_estimate;

/* Parse one line (no newline) into out, its strings in a. Each returns 1 when the line is one of
 * its kind, 0 when it isn't (Swift's nil), -1 when out of memory. */
int prep_run_line_parse(arena *a, const char *line, size_t n, prep_run_line *out);          /* parseRunLine */
int prep_playlist_parse(arena *a, const char *line, size_t n, prep_playlist *out);          /* parsePlaylists, per line */
int prep_track_list_parse(arena *a, const char *line, size_t n, pstr *file);                /* parseTrackList, per line */
/* Estimate(): every count 0, fits and disk_ok true (an estimate line then sets them as bool()) */
void prep_estimate_init(prep_estimate *e);
/* parseEstimate's loop body for one line; 0, or -1 when out of memory */
int prep_estimate_line(arena *a, prep_estimate *e, const char *line, size_t n);
/* parseEstimate's result: 1 when it isn't nil (an estimate line came, or a refusal) */
int prep_estimate_ok(const prep_estimate *e);

/* NSString.lastPathComponent (TrackLine.name). Points into path; NUL-terminated only when path
 * has no trailing slash. */
pstr prep_last_path_component(pstr path);

/* ------------------------------------------------------------------ words (words.c)
 * The Prepare and Status tabs' texts, sizes and durations. */

void w_duration(tbuf *b, double seconds);              /* Prepare.swift durationText */
void w_size(tbuf *b, double gb);                       /* Prepare.swift sizeText: "850 MB", "2.4 GB" */
void w_about_size(tbuf *b, int64_t bytes);             /* Location.swift aboutSize: " (about 1.2 GB)", "" */
enum { STEMS_MEASURING = 0, STEMS_MEASURED = 1, STEMS_FAILED = 2, STEMS_NOT_CONNECTED = 3 };
void w_stems_size(tbuf *b, int32_t kind, int64_t bytes);   /* Location.swift StemsSize.text */
void w_size_words(tbuf *b, int64_t bytes);             /* LocationActions.swift sizeWords: "18.2 GB", "35 MB" */
void w_estimate_summary(tbuf *b, const prep_estimate *e);  /* Prepare.swift estimateSummary */

/* Prepare.swift doesntFitChoices: the message into msg; the buttons, in order: "Prepare the First
 * N Tracks" when first_n > 0, "Raise the Limit to X GB" when has_raise, then Cancel. */
typedef struct { int64_t first_n; int32_t has_raise; double raise_to; int32_t nbuttons; } fit_choices;
void w_doesnt_fit(tbuf *msg, const prep_estimate *e, double free_gb, fit_choices *out);
void w_fit_button(tbuf *b, const fit_choices *c, int32_t i);   /* button i's title */

void w_result_text(tbuf *b, const prep_track *t, int32_t checking);   /* Prepare.swift resultText */
void w_prepared_note(tbuf *b, const prep_track *t);                    /* Prepare.swift preparedNote */
void w_track_note(tbuf *b, const prep_track *t);       /* PrepareView.swift: the Note column */
void w_set_state(tbuf *b, const prep_estimate *e);     /* PrepareView.swift setState (e NULL: nil) */
/* PrepareView.swift lastRunLine (stopped, error: NULL for nil) */
void w_last_run_line(tbuf *b, int64_t files, int64_t skipped, const pstr *stopped, int32_t has_error);
/* PrepareView.swift runEndTitle */
void w_run_end_title(tbuf *b, pstr name, int32_t status, int32_t got_done, const pstr *stopped, int64_t finished,
                     int64_t skipped, int32_t has_error, int32_t partial);
/* PrepareView.swift runSummary while a run is under way: "Preparing “x”: 12%" */
void w_run_summary(tbuf *b, pstr name, int32_t paused, double fraction);
/* PrepareView.swift runLine(.done)'s phase line */
void w_run_done_line(tbuf *b, int64_t done, int64_t skipped, double gb, const pstr *stopped, const pstr *error);
/* Watcher.swift keptSetNotice: 0 (nil) when the set is ready */
int w_kept_notice(tbuf *b, const prep_estimate *e);
/* Watcher.swift notReadyTracks: the files to prepare or skipped, in order; how many (at most cap) */
int32_t w_kept_not_ready(const prep_estimate *e, pstr *out, int32_t cap);
/* Watcher.swift newlyNotReady: 1 when a file of now isn't among noticed */
void w_kept_sort(pstr *v, int32_t m);
int w_kept_newly(const pstr *now, int32_t n, const pstr *noticed, int32_t m);   /* noticed sorted by w_kept_sort */
/* Prepare.swift noPaths: every path in s becomes <path> (a log line never holds one) */
void w_no_paths(tbuf *b, const char *s);

/* State.swift: the free-space floor's texts (drive NULL: this Mac) */
int64_t cache_floor_gb(int64_t floor);                                 /* cacheFloorGB */
void w_cache_paused_tip(tbuf *b, int64_t floor, const char *drive);    /* cachePausedTip */
void w_cache_paused_at_install(tbuf *b, int64_t floor, const char *drive);   /* cachePausedAtInstall */

/* ------------------------------------------------------------------ lights and gating (lights.c) */

/* Controller.swift LightLook, and lightDrawing's VoiceOver text */
enum { LOOK_ON = 0, LOOK_LIMITED = 1, LOOK_OFF = 2 };
core_text_id light_ax_text(int32_t look);
int32_t light_plus(tbuf *tip, int32_t installed);      /* plusLight: the look; the tip into tip */

/* cacheLight's inputs */
typedef struct {
    int32_t installed, plus_on, chosen, rekordbox_model;
    int32_t has_bridge_models;                 /* bridgeModels isn't nil */
    const char *const *bridge_models; int32_t n_bridge_models;
    const char *model;                         /* the model's sha256, NULL while unknown */
    const char *engine;                        /* "" when rekordbox has none */
    int32_t has_paused_floor; int64_t paused_floor;
    const char *location;                      /* the location light's text, NULL when fine */
    const char *drive;                         /* the chosen drive's name, NULL on this Mac */
#ifdef CORE_WIN_POLICY
    int64_t win_r1;                            /* Windows: FACT_R1_STATE (R1_*), 0 nothing to put back */
#endif
} cache_light_in;
int32_t light_cache(tbuf *tip, const cache_light_in *in);   /* cacheLight */

/* Location.swift LocationStatus and LocationProbe, and the bridge's state (BridgeState) */
enum { LOC_THIS_MAC = 0, LOC_CHOSEN = 1, LOC_BRIDGE_TOO_OLD = 2, LOC_CONFIG_UNREADABLE = 3, LOC_CONFIG_MALFORMED = 4 };
enum { PROBE_AVAILABLE = 0, PROBE_NOT_CONNECTED, PROBE_FOLDER_MISSING, PROBE_MARKER_MISSING_OR_WRONG, PROBE_LINK,
       PROBE_NETWORK, PROBE_READ_ONLY, PROBE_NO_PERMISSION };
typedef struct {
    int32_t kind, probe;     /* probe: LOC_CHOSEN only */
    const char *drive;       /* driveName(dir) for LOC_CHOSEN and LOC_BRIDGE_TOO_OLD */
} loc_status;
typedef struct {
    const char *state, *reason;   /* pending | available | unavailable; the bridge's reason */
    const char *drive;            /* driveName of its location, NULL when the location is "" (this Mac) */
} loc_bridge;
/* locationLight: 1 and the text, or 0 (nil: the location is fine). bridge NULL: none live. */
int location_light(tbuf *b, const loc_status *s, const loc_bridge *bridge, int32_t rekordbox_running);

/* Compat.swift: why a feature can't be installed (FEAT_OK: it can) */
enum {
    FEAT_OK = 0, FEAT_RB_NOT_INSTALLED, FEAT_NO_ENGINE, FEAT_ENGINE_UNLISTED, FEAT_NEEDS_ADMIN, FEAT_RB_UNLISTED,
    FEAT_TEST_VERSION, FEAT_NO_ORT, FEAT_ORT_NOT_ORIGINAL, FEAT_ORT_UNLISTED,
    FEAT_NOT_CHECKED   /* Windows: Pioneer's ORT couldn't be checked (FACT_LIB_UNCHECKED); not "checks again" */
};
typedef struct { int32_t rekordbox_installed; const char *engine; int32_t engine_supported; } plus_facts;
int32_t plus_blocked(const plus_facts *f);                                    /* stemsPlusBlocked */
typedef struct {
    int32_t user_is_admin, rekordbox_installed;
    const char *rekordbox_version; int32_t rekordbox_supported;
    int32_t cache_present, root_ort_exists, prototype_exists;
    int32_t lib_exists, lib_signed;           /* of rootOrt when the cache is present, else rekordbox's */
    const char *ort_version;                  /* dylibVersion, NULL when unreadable */
    const char *ort_prefix;                   /* the list's ortVersionPrefix */
} cache_facts;
int32_t cache_blocked(const cache_facts *f);                                  /* stemsCacheBlocked */
#ifdef CORE_WIN_POLICY
/* Windows' stemsCacheBlocked: the same checks in the same order, but Pioneer's ORT is matched by
 * sha256 against the verified payload's list (ort_listed, FACT_ORT_LISTED), not by a version
 * prefix; ort_prefix isn't read. No verified payload: nothing is listed (Windows has no built-in
 * list), so the rekordbox version is unlisted: "checks again" */
/* lib_unchecked: FACT_LIB_UNCHECKED (Pioneer's ORT couldn't be checked: FEAT_NOT_CHECKED) */
int32_t cache_blocked_win(const cache_facts *f, int32_t ort_listed, int32_t lib_unchecked);
#endif
/* the reason's line; engine, rekordbox_version, ort_version as in the facts */
void w_feature_why(tbuf *b, int32_t why, const char *engine, const char *rekordbox_version, const char *ort_version);
int32_t feature_why_checks_again(int32_t why);       /* blockedByVersionList: ends with checksAgain */
void w_short_version(tbuf *b, const char *v);         /* shortVersion: "7.2.19.0342" → "7.2.19" */
int32_t version_listed(const char *v, const char *const *list, int32_t n);   /* listed */
/* standardAccountLine: 0 (nil), or 1 with the line */
int w_standard_account_line(tbuf *b, int32_t admin, int32_t cache_traces, int32_t cache_chosen);

/* Controller.swift refresh(): the buttons and the status line between actions */
typedef struct {
    int32_t plus, cache;                 /* stemsPlusPresent, stemsCachePresent */
    int32_t chosen_model, chosen_cache;
    int32_t plus_why, cache_why;         /* plus_blocked, cache_blocked */
    const char *engine, *rekordbox_version, *ort_version;   /* for the reasons' texts */
    int32_t cache_traces, rekordbox_installed, user_is_admin;
    int32_t own_bundle, cache_is_link, watcher_off;
    const char *location_notice, *model_notice, *notice;   /* NULL: none */
#ifdef CORE_WIN_POLICY
    const char *win_cache_problem;   /* version-specific status line when available */
#endif
} gate_in;
typedef struct {
    int32_t install_plus, install_cache, reinstall, reinstall_plus, reinstall_cache, settings;
    int32_t uninstall_plus, uninstall_cache, uninstall_all;
    int32_t reinstall_features;          /* the Reinstall button's title: T_FEATURE_PLUS, _CACHE or T_FEATURES_BOTH */
} gate_out;
void gate_buttons(const gate_in *in, gate_out *out);
void w_reinstall_title(tbuf *b, const gate_out *g);   /* "Reinstall Demucs v4 + Stems Cache" */
void w_status_lines(tbuf *b, const gate_in *in);      /* the lines, joined by "\n" */

/* Prepare.swift prepareProblems: the lines joined by "\n"; returns how many */
typedef struct {
    int32_t apple_silicon, helper_there, rekordbox_installed, rekordbox_ok;   /* apple_silicon: a Windows host always passes 1
                                                                                (its helper runs on x64 and arm64), so T_PREP_NEEDS_ARM,
                                                                                Mac-only, is never composed there */
    const char *rekordbox_version;
    int32_t cache_installed, bridge_keeps, ort_there;
    const char *config_unreadable;       /* NULL: readable */
    int32_t enabled;
    int32_t cache_look; const char *cache_tip;
    int32_t quality;                     /* -1: unknown */
    int32_t plus_on;
    const char *model_sha;               /* NULL: still being worked out */
    const char *const *our_models; int32_t n_our_models;
    int32_t cache_folder;                /* a folder the stems go to now */
    const char *programs;                /* Windows' own Program Files (T_PREP_NO_REKORDBOX's {dir}); NULL on the Mac */
} prep_facts;
int32_t w_prepare_problems(tbuf *b, const prep_facts *f);

/* ------------------------------------------------------------------ the state files (state.c, State.swift, Location.swift)
 * Formats and decisions; the host reads and writes the files. A file String(contentsOfFile:
 * encoding: .utf8) can't read (invalid UTF-8) is passed as NULL. Lines split as Swift's
 * split(separator: "\n") splits a String: a "\r\n" is one Character and doesn't split. */

/* chosen(): "model=1", "cache=1" anywhere in installed's text (NULL: neither) */
void state_chosen(const char *text, size_t n, int32_t *model, int32_t *cache);
/* setChosen's text, into b (at least 24 bytes) */
void state_chosen_text(char *b, size_t cap, int32_t model, int32_t cache);

/* the dismissed keys: a set of the file's lines (empty ones dropped), byte-sorted, no repeats.
 * keyset_parse and keyset_has work on one arena-held list. */
typedef struct { pstr *keys; int32_t n; } keyset;
int keyset_parse(arena *a, const char *text, size_t n, keyset *out);   /* 0, or -1 out of memory */
int32_t keyset_has(const keyset *k, const char *key, size_t n);
/* remember(): the file's new text (the union, sorted, one per line, then "\n") */
void keyset_union_text(tbuf *b, const keyset *have, const pstr *add, int32_t nadd);

/* missingKeys(): the prompt keys of what went missing, less the dismissed ones, into out (2) */
int32_t state_missing_keys(arena *a, int32_t chosen_model, int32_t chosen_cache, int32_t cache_present, int32_t plus_present,
                           const char *rekordbox_version, const char *engine, const keyset *dismissed, pstr out[2]);

/* the free-space floor (cacheFreeFloor, cachePausedFloor); disk, free: -1 when unknown */
int64_t state_free_floor(int64_t disk);
int32_t state_paused_floor(int64_t free_bytes, int64_t disk, int64_t *floor);   /* 1: paused under *floor */

/* the busy marker (readBusyMarker, markerLive, setBusyMarker's text) */
typedef struct {
    int32_t pid;
    int32_t has_start; int64_t start;
    int32_t has_action; pstr id, label;
} busy_marker;
int busy_marker_parse(arena *a, const char *text, size_t n, busy_marker *out);   /* 1, 0 (nil), -1 out of memory */
/* started: the pid's start time (-1: no such process); written: the file's mtime in seconds
 * since 1970 rounded down, has_written 0 when unknown */
int32_t busy_marker_live(const busy_marker *m, int64_t started, int32_t has_written, int64_t written);
void busy_marker_text(tbuf *b, int32_t pid, int64_t start, const char *id, const char *label);

/* the bridge's state file (parseBridgeState, liveBridgeState); text NULL-safe, at most 8192 bytes
 * (readBridgeState: bigger is nil); invalid UTF-8 is repaired first (String(decoding:)) */
typedef struct { int32_t pid; int64_t start; pstr location, state, reason; } bridge_state;
int bridge_state_parse(arena *a, const char *text, size_t n, bridge_state *out);   /* 1, 0 (nil), -1 */
int32_t bridge_state_live(const bridge_state *s, const char *dir, int64_t started);

/* ------------------------------------------------------------------ the watcher (watcher.c, Watcher.swift, Actions.swift) */

/* watcherRounds: the checks of one start, the host's operations as callbacks. Returns the rounds run. */
typedef struct {
    void *ctx;
    int32_t (*check)(void *ctx);     /* 1: done, a change since may be looked at; 0: stop */
    void (*release)(void *ctx);
    int32_t (*again)(void *ctx);
    int32_t (*retake)(void *ctx);
    void (*log)(void *ctx, const char *line);
} watcher_round_ops;
int32_t watcher_rounds(int32_t max, const watcher_round_ops *ops);

/* watcherSettle: 1 when nothing changes rekordbox and it is closed; 0: don't ask */
typedef struct {
    void *ctx;
    int32_t (*updating)(void *ctx);
    int32_t (*app_busy)(void *ctx);
    int32_t (*rekordbox_open)(void *ctx);
    void (*pause)(void *ctx, int32_t seconds);
    int32_t (*half_uninstalled)(void *ctx, char *id, size_t cap);   /* 1 and the action's id: an uninstall left half-done */
    void (*in_session)(void *ctx);
    void (*log)(void *ctx, const char *line);
} watcher_settle_ops;
int32_t watcher_settle(const watcher_settle_ops *ops);

/* watcherCheck's question: what to do with the missing keys (less the unsupported ones the user
 * was told of, and those put off a moment ago). */
enum { WATCH_NOTHING = 0, WATCH_PUT_OFF = 1, WATCH_ASK = 2 };
int32_t watcher_ask(const pstr *keys, int32_t nkeys, const keyset *put_off);
/* and the answer (the button's index in watcherDialog's buttons) */
enum { WANSWER_REINSTALL = 1, WANSWER_REMEMBER = 2, WANSWER_REMEMBER_UNSUPPORTED = 3, WANSWER_PUT_OFF = 4 };
/* unsupportedKey: key + " unsupported with " + the lists joined by "," */
void watcher_unsupported_key(tbuf *b, const char *key, size_t n, const char *const *rekordbox, int32_t nrb,
                             const char *const *engines, int32_t nengines, const char *ort_prefix);

/* watcherDialog */
typedef struct {
    core_text_id title;
    int32_t nbuttons; core_text_id buttons[3];
    int32_t blocked;                 /* the OK / Don't Ask Again form */
    const char *reinstall;           /* the features offered by Reinstall Now */
} watcher_dialog;
void watcher_dialog_build(tbuf *message, watcher_dialog *d, int32_t model_off, int32_t cache_off, const char *engine,
                          const char *rekordbox_short, int32_t engine_ok, int32_t rekordbox_ok, int32_t plus_works);
int32_t watcher_answer(const watcher_dialog *d, int32_t button);   /* WANSWER_* (Cancel, -1: put off) */

void watcher_set_run_pattern(tbuf *b, const char *name);     /* setRunPattern */
/* notReadyTracks then newlyNotReady: whether e has a track that isn't ready that `noticed` doesn't cover */
int32_t watcher_newly_not_ready(const prep_estimate *e, const keyset *noticed);
int32_t watcher_launchd_disabled(const char *output, size_t n, const char *label);   /* launchdDisabled */
int32_t watcher_off(int32_t plist_there, int32_t loaded, int32_t disabled);          /* watcherOff */

/* Location.swift thisMacNeedsCleanup and mayRetryThisMacDeletion (the launch retry of This Mac's
 * deletion, M6): chosen_available is status == .chosen(_, _, .available) */
int32_t this_mac_needs_cleanup(int32_t chosen_available, int32_t bridge_location, int32_t model_folders);
int32_t may_retry_this_mac_deletion(int32_t flag, int32_t chosen_available, int32_t bridge_location, int32_t rekordbox_running);

/* ------------------------------------------------------------------ the action dialogs' texts (dialog_texts.c) */

/* Actions.swift installNote: plus, cache as the button's title names them; floor when paused */
void w_install_note(tbuf *b, int32_t plus, int32_t cache, int32_t reinstall, int32_t has_paused, int64_t floor, const char *drive);
void w_uninstall_note(tbuf *b, int32_t cache);                       /* uninstallNote */
/* uninstallEverythingMessage: stems the size text (" (1.2 GB)" or ""), drive NULL on this Mac */
void w_uninstall_all_message(tbuf *b, int32_t others_cache, int32_t no_saved_model, const char *stems, const char *drive, int32_t drive_connected);
#ifdef CORE_WIN_POLICY
void w_win_uninstall_all_unmeasured(tbuf *b, int32_t others_cache, int32_t no_saved_model, const char *drive, const char *dir);
#endif
enum { FATE_DELETED = 0, FATE_OTHER_ACCOUNT = 1, FATE_CANT_DELETE = 2 };     /* AppFate */
void w_own_files_removed(tbuf *b, const char *about, int32_t fate, int32_t others_cache, int32_t cache_link, const char *const *left, int32_t nleft);
void w_location_removal_note(tbuf *b, const char *drive, int32_t connected, const char *size);   /* locationRemovalNote */
/* LocationActions.swift switchMessage, useThisMacMessage, clearLocationMessage (old_drive NULL: none) */
void w_switch_message(tbuf *b, const char *drive, int32_t has_found, int64_t found, int64_t this_mac, const char *old_drive);
void w_use_this_mac_message(tbuf *b, const char *old_drive);
void w_clear_location_message(tbuf *b, const char *drive);
/* Location.swift LocationRefusal and locationRefusal */
enum { REFUSE_INTERNAL = 0, REFUSE_NETWORK, REFUSE_READ_ONLY, REFUSE_TIME_MACHINE, REFUSE_INSIDE, REFUSE_LINK, REFUSE_FILE_SYSTEM,
       REFUSE_PATH, REFUSE_UNREADABLE };
void w_location_refusal(tbuf *b, int32_t why, const char *drive);
/* Update.swift updateFailureReason ("failed <key>" in update/result) and updateFailedMessage */
enum { UPDATE_R_NONE = 0, UPDATE_R_APP, UPDATE_R_DOWNLOAD, UPDATE_R_STAGING, UPDATE_R_COPY, UPDATE_R_COPY_CHECK, UPDATE_R_SIGN, UPDATE_R_SWAP };
int32_t update_failure_reason(const char *result, size_t n);
void w_update_failed_message(tbuf *b, int32_t reason);
/* Scripts.swift RootExit; Admin.swift rootProblem (0: nil), rootRunTexts; Actions.swift cacheFailureReason */
enum { ROOT_NO_ORIGINALS = 3, ROOT_CHECK_FAILED = 4, ROOT_REKORDBOX_OPEN = 5, ROOT_ROLLED_BACK = 6, ROOT_ALREADY_RUNNING = 7,
       ROOT_INSTALLER_RUNNING = 8, ROOT_REKORDBOX_MISSING = 9, ROOT_NOT_PIONEERS = 10, ROOT_ROLLBACK_FAILED = 11 };
#ifdef CORE_WIN_POLICY
/* Windows (W-core): the worker's appended exit codes (app.md §5.5) and UAC declined */
enum { ROOT_WIN_PROTOCOL = 12, ROOT_WIN_RUN_AGAIN = 13, ROOT_WIN_OLDER = 14, ROOT_WIN_USER_STOPPED = 1223 };
/* multi-version rekordbox (mv-spec §2.2): the worker's fresh scope differs from the app's (nothing
 * changed), and some targets done while one failed */
enum { ROOT_WIN_SCOPE_CHANGED = 15, ROOT_WIN_PARTIAL = 16 };
/* an Install refused before any rekordbox folder changed whose undo couldn't restore everything
 * (worker exit 17): never "nothing changed" */
enum { ROOT_WIN_UNDO_INCOMPLETE = 17 };
/* FACT_WIN_RB_ROWS' state and why codes: win/c/inventory.h's INV_ROW_* and WHY_*, the same values */
enum { WIN_ROW_ON = 0, WIN_ROW_READY = 1, WIN_ROW_UNSUPPORTED = 2, WIN_ROW_REPAIR = 3, WIN_ROW_UNVERIFIED = 4 };
enum { WIN_WHY_NONE = 0, WIN_WHY_RB_UNLISTED = 1, WIN_WHY_BUILD_UNLISTED = 2, WIN_WHY_ORT_UNLISTED = 3, WIN_WHY_ORT_NOT_ORIGINAL = 4,
       WIN_WHY_R1_MISSING = 5, WIN_WHY_R1_FOREIGN = 6, WIN_WHY_R1_ORT_MISSING = 7, WIN_WHY_DELISTED_BRIDGE = 8, WIN_WHY_NOT_VERIFIED = 9,
       WIN_WHY_NO_ANSWER = 10, WIN_WHY_NOT_RECORDED = 11 };
/* R1's state (FACT_R1_STATE) and its line for the status line and the light's tip: nothing for 0 */
enum { R1_OK = 0, R1_LIVE_MISSING = 1, R1_LIVE_FOREIGN = 2, R1_ORT_MISSING = 3 };
enum { WIN_ROWS_MAX = 32, WIN_VERSION_CAP = 64 };
typedef struct {
    char version[WIN_VERSION_CAP], display_version[WIN_VERSION_CAP];
    int state, why, running, r1, r1_dismissed;
} win_row;
int win_version_ok(const char *s, size_t n);
int win_rows_parse(const char *s, size_t len, win_row *out, int max);
void w_win_version_list(tbuf *b, const char *const *versions, int n);
void w_win_row_text(tbuf *b, const win_row *r, int full_build);
void w_win_row_reason(tbuf *b, const win_row *r);
void w_win_cache_summary(tbuf *b, int on, int total);
void w_win_batch_why(tbuf *b, int code, const char *programs);
void w_r1_line(tbuf *b, int64_t state);
/* the Windows in-front policy (win.c, mac-core §3.6): front_facts' bits, modal_open, the tab asked
 * about (core_tab), held: the held click's check. 1 in front */
int win_in_front(uint32_t front, int32_t modal, int32_t tab, int held);
#endif
int32_t root_problem(int32_t code, core_text_id *title, core_text_id *body);
void root_run_texts(const char *name, core_text_id *status, core_text_id *failed_title);
void w_cache_failure_reason(tbuf *b, int32_t failed, int32_t code, const char *programs);   /* failed 0: .removed or .keptRootFolder; programs: w_rb_text's */
/* The texts naming rekordbox's folder (T_PREP_NO_REKORDBOX, T_ROOT_MISSING_MSG,
 * T_ROOT_FOLDER_CLEANUP_MSG): on Windows they take {dir}, <programs>\rekordbox, programs being
 * Windows' own Program Files as the core read it (core's win_programs), on whichever drive; the
 * Mac's words take nothing. w_rb_dir_slot: 1 with *sl set (its value in buf, cap bytes), or 0 for
 * any other text or os (no slot). w_rb_text: id into b, with its slot when it takes one. */
int32_t w_rb_dir_slot(core_os os, core_text_id id, const char *programs, char *buf, size_t cap, core_slot *sl);
void w_rb_text(tbuf *b, core_text_id id, const char *programs);

/* ------------------------------------------------------------------ menus (menus.c) */

/* What main.swift's validateMenuItem reads */
typedef struct {
    int32_t action_running, preparing, paused, prep_selected, update_visible;
    gate_out buttons;                    /* the buttons' enabled states (off while an action runs) */
#ifdef CORE_WIN_POLICY
    int32_t tabs_hidden;                 /* Windows without "prepare_tabs": the Status and Prepare rows hidden */
#endif
} menu_facts;
/* CORE_MENU_* bits for item id; *title the title now (Pause/Resume) */
uint32_t menu_rule(int32_t id, const menu_facts *f, int32_t *title);

#endif
