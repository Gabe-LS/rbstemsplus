/* The view model (mac-core §3, M4): its state on main and the entries core.c and flows.c call.
 * Private to the core. view.c holds Controller.swift's refresh, busy and prepareChanged; the
 * tabs (PrepareView.swift) are prepare_tab.c and status_tab.c.
 *
 * Every decision here is today's Swift's (Controller.swift, PrepareView.swift, main.swift), named
 * at each function; the facts it reads are host->facts' cheap ones (facts.def), read live on
 * every refresh and never cached, except the model's sha256, memoised by path, size and mtime as
 * currentModelSHA memoises it. */
#ifndef RBSP_CORE_VIEW_H
#define RBSP_CORE_VIEW_H

#include <stdint.h>

struct core;
struct core_facts;
/* needs pure.h's types (tbuf, prep_track, prep_estimate) and the arena: internal.h includes it */

/* who asked for the model's sha256 first, and is told once it is known (currentModelSHA's done:
 * only the call that started the work gets it, as today) */
enum { SHA_WAIT_NONE = 0, SHA_WAIT_REFRESH = 1, SHA_WAIT_PREP_UPDATE = 2, SHA_WAIT_PREP_CHECK = 3 };

typedef struct view_sha_memo {
    char *path;              /* the key: path, size, mtime (currentModelSHA's) */
    int64_t size, mtime;
    char sha[72];            /* "" while unknown */
    int known;
    /* the work under way, if any, and who started it */
    char *work_path;
    int64_t work_size, work_mtime;
    int working;
    int waiter;
} view_sha_memo;

/* the Manage tab's buttons as they are shown (the menus read them, as validateMenuItem reads the
 * buttons' isEnabled) */
typedef struct view_buttons {
    int32_t install_plus, install_cache, reinstall, reinstall_plus, reinstall_cache, settings;
    int32_t uninstall_plus, uninstall_cache, uninstall_all;
} view_buttons;

/* LocationMonitor's snapshot (CORE_SLOW_LOCATION's facts), as last measured */
enum { LOC_FACTS = 11 };
typedef struct view_loc_memo {
    int known;                   /* a measurement came (the host measures: its facts win over the cheap sheet's) */
    int working, again;          /* one measurement at a time; another asked meanwhile */
    int size_asked, again_size;  /* the one under way, the one asked meanwhile: with the size (update(size: true)) */
    uint8_t set[LOC_FACTS];
    int64_t i[LOC_FACTS];
    char *s[LOC_FACTS];
    /* the stems' size (StemsSize: 0 measuring, 1 measured, 2 failed, 3 not connected), of size_folder,
     * walked at size_time (mono ns, 0 never); size_gen: the walk that may publish */
    int64_t size_kind, size_bytes;
    char *size_folder;
    uint64_t size_time, size_gen;   /* size_gen moves only on LOCATION_REMEASURE (a walk from before Clear is dropped) */
    int walk_busy, walk_again;   /* one walk at a time (LocationMonitor's serial size queue); another asked meanwhile */
    struct plat_timer *every;    /* the 10-minute update(size: true) */
    void *every_ctx;
#ifdef CORE_WIN_POLICY
    uintptr_t every_gen;
    struct plat_timer *retry;    /* review LN1: one measure again after a not now, 30 s doubling to 10 minutes */
    void *retry_ctx;
    uintptr_t retry_gen;         /* the armed retry's; a job from a retry cancelled after it fired finds another */
    int32_t retry_ms, retry_armed_ms;   /* the next retry's delay; the armed one's */
#endif
    int test_fail_copies;        /* CORE_TEST_HOOKS: view_loc_query's next copies fail (MR1) */
} view_loc_memo;

/* the memo as a flow reads it (flow_query): known 0 when the host keeps its own snapshot. The drive
 * and the folder in use ("" when the memo has none) are copies in the query's arena, whole at any
 * length (a Windows path or drive name has no 256- or 1024-byte bound: MR11) */
typedef struct view_loc_now {
    int known;
    int64_t floor, size_kind, size_bytes;
    const char *drive, *in_use;
#ifdef CORE_WIN_POLICY
    int drive_set;          /* the memo has the drive fact (drive "" then is a drive with no name) */
    int64_t kind, probe;    /* the memo's LOC_* and PROBE_*, -1 unset (review NT3) */
#endif
} view_loc_now;

typedef struct view_state {
    int changed_during_action;   /* prepareChanged came during an action: its end checks */
    int quiet_during_action;     /* only coming back to the app came during a wait: its end checks quietly */
    int watcher_off;             /* watcherIsOff (Actions.swift updateWatcherState) */
    char *seen_location;         /* Controller.seenLocation: the location's key the tabs last checked against */
    char *status_key;            /* what a confirmation describes, as last seen (DOM_STATUS moves when it changes) */
    view_buttons shown;
    view_sha_memo sha;
    view_loc_memo loc;
#ifdef CORE_WIN_POLICY
    int64_t win_r1;              /* Windows: R1's state (FACT_R1_STATE), R1_OK until an answer says otherwise */
    uint64_t r1_gen;             /* the latest R1 ask: an older answer is stale */
    int r1_after_action;         /* an action ended and its R1 answer hasn't come: Put Back stays off (CW3) */
    int win_r1_dismissed;        /* Windows: R1's answer says this version is dismissed (FACT_R1_DISMISSED, N10) */
    win_row win_r1_bridge_repair; /* the repair row identifying our bridge, for the cache light */
    char win_r1_dismiss_versions[256];   /* F_WIN_R1_DISMISS_VERSIONS as last set: what Don't Ask Again covers now */
#endif
} view_state;

/* main */
void view_init(struct core *c);
void view_free(struct core *c);
/* Controller.refresh(): the lights, then (between actions) the buttons and the status line */
void view_refresh(struct core *c);
/* busy(msg) with a message: every window button off (the status line and spinner: flows.c) */
void view_busy_on(struct core *c);
/* busy(nil) (Controller.swift:290): the buttons back, then what the action changed checked.
 * marked: the action that ended changed things (currentAction) */
void view_busy_off(struct core *c, int marked);
/* prepareChanged(quiet:) (Controller.swift:320) */
void view_prepare_changed(struct core *c, int quiet);
/* LocationMonitor.onChange (Controller.swift:143): refresh, then a quiet check when what the
 * tabs show about the location moved */
void view_location_changed(struct core *c);
/* LocationMonitor.update(): the cheap state measured again off main (CORE_SLOW_LOCATION); when
 * it moved, view_location_changed */
void view_location_measure(struct core *c);
void view_location_update(struct core *c, int size);   /* ... update(size:) */
/* LOCATION_REMEASURE: invalidateSize(), then update(size: true) */
void view_location_remeasure(struct core *c);
/* the memo's location facts over a cheap sheet (when the memo is known) */
void view_loc_overlay(struct core *c, struct core_facts *f);
/* flow_query: the memo now, into q's view_loc_now, its strings copied into q's arena (the flow's,
 * which waits meanwhile); out of memory: 0, and the flow stops (never the host's path: a memo
 * host keeps no snapshot) */
typedef struct view_loc_q {
    view_loc_now *n;
    arena *a;
} view_loc_q;
int view_loc_query(struct core *c, void *q);
/* updateWatcherState's main half (Actions.swift:210): the status line's reminders line */
void view_watcher_off(struct core *c, int off);
/* checkWatcherOn (Actions.swift:199), on activation: CORE_STEP_WATCHER_STATE on the flow queue (M72) */
void view_check_watcher_on(struct core *c);
/* the start state (Controller.init): the first tab, then the first refresh */
void view_start(struct core *c);
/* the model's sha256 now (currentModelSHA): NULL while unknown; the work is started then, and
 * `waiter` is told once it is known if this call started it */
const char *view_model_sha(struct core *c, const struct core_facts *f, int waiter);
/* the menus' facts (validateMenuItem) */
void view_menu_facts(struct core *c, void *menu_facts_out);
#ifdef CORE_WIN_POLICY
/* A Windows host without "prepare_tabs":true has only Manage. The start tab, menu rows, clicks
 * and {"ev":"tab"} cannot reach Status or Prepare. */
int view_tabs_hidden(const struct core *c);
#endif
/* a track line's strings: their size (each with its NUL), and dst made from src with its strings
 * copied into w (core.c) */
size_t view_track_size(const prep_track *t);
void view_track_put(prep_track *dst, const prep_track *src, char *w);
/* a fact holding one item a line ("" lines dropped), as an array in a */
int32_t view_split_lines(arena *a, const char *s, const char ***out);
/* the cheap facts now (host->facts, on main), the sheet in the scratch */
/* the cheap facts now: the CORE_FACTS_* parts the caller's decision reads (M71) */
void view_read_facts(struct core *c, struct core_facts *f, uint32_t parts);
/* LocationSnapshot.lightTip: NULL when the snapshot has no status yet or the location is fine */
const char *view_location_tip(struct core *c, const struct core_facts *f);
/* cacheLight with today's inputs; *tip in the scratch */
int32_t view_cache_light(struct core *c, const struct core_facts *f, int waiter, const char **tip);
/* inFront(tab) (PrepareView.swift:293), read live */
int view_in_front(struct core *c, int tab);
int view_in_front_held(struct core *c, int tab);   /* ... and no dialog or sheet up (the held click) */
/* a tab comes to the front (viewWillAppear), and the core's own tab switch (showTab) */
void view_tab_shown(struct core *c, int tab);
void view_show_tab(struct core *c, int tab);
/* an action's click (by its button as shown) or menu choice (validateMenuItem): 1 it may start */
int view_click_enabled(struct core *c, const char *id, size_t n, int menu);
#ifdef CORE_WIN_POLICY
/* Windows: R1's check (CORE_SLOW_R1) asked on a worker; its answer refreshes only when it changed */
void view_r1_check(struct core *c);
#endif
/* a click or menu choice that isn't an action flow's */
void view_event(struct core *c, const char *id, size_t n, int menu);

/* ---------------------------------------------------------------- the Prepare tab (prepare_tab.c) */

typedef struct prep_pl { char *id, *parent, *name, *kind; int64_t tracks; } prep_pl;   /* Playlist, owned */
typedef struct prep_est { arena a; prep_estimate e; } prep_est;                          /* an Estimate, owned */

typedef struct prep_state {
    prep_pl *pls; int32_t npl;     /* the playlists, as --playlists listed them */
    int32_t *row_pl;               /* the outline's row: its playlist (the tree in pre-order) */
    int32_t *pl_row, *pl_parent;   /* a playlist: its row, its parent in the tree (-1: a root) */
    char *load_error;              /* NULL: the library was read */
    char **closed; int32_t nclosed, capclosed;   /* closedFolders, by id */
    int has_sel; prep_pl sel;      /* selected */
    prep_est *est;                 /* estimate, NULL nil */
    uint64_t est_seq;              /* moves when the estimate changes by value (and on nil): a dialog's `estimate == e` */
    int estimating, refreshing, held, stale;     /* held: prepareWhenChecked */
    uint64_t est_run;              /* job: the estimate under way, stopped when the gen moves */
    int32_t next_row;
    prep_track **lines; int32_t nlines, caplines;
    char *problems; int32_t nproblems;           /* one a line */
    /* the tracks' cells: the state they were last drawn with */
    int cells_checking, cells_running; int32_t cells_run_row;
    /* the run */
    uint64_t run; int running, paused;
    int has_run_pl; prep_pl run_pl;
    int64_t run_done, run_skipped, run_files; double run_fraction;
    int32_t run_row;               /* -1 nil */
    char *run_phase, *last_run, *run_short, *run_error, *run_stopped;
    int run_got_done, run_partial;
    /* the "Keep the stems until" date: YYYY-MM-DD, local time */
    char until[16];
    char until_text[200], until_text_for[16];   /* until in the host's medium style (CORE_STEP_DATE_TEXT), for that date */
    int64_t test_now;              /* CORE_TEST_HOOKS: the clock's seconds since 1970, 0 the real one */
} prep_state;

void prep_init(struct core *c);
void prep_free(struct core *c);
void prep_update(struct core *c);                       /* update() */
void prep_set_stale(struct core *c);                    /* stale = true */
/* prepareChanged's part when the tab is in front: reloadList(quiet: quiet && !stale); stale = false */
void prep_reload_from_change(struct core *c, int quiet);
void prep_will_appear(struct core *c);                  /* viewWillAppear */
void prep_sha_known(struct core *c, int check);         /* the model's sha256 came: update(), or check(results: inFront) */
int prep_has_selection(struct core *c);
int prep_running(struct core *c);
/* {"ev":"select"} on the outline, {"ev":"folder"}, {"ev":"until"} (CORE_OK or CORE_E_ARG) */
void prep_user_select(struct core *c, int32_t row);
void prep_user_folder(struct core *c, int32_t row, int open);
int prep_until(struct core *c, const char *date);
/* a click or menu choice the Prepare tab handles: 1 handled, 0 not its */
int prep_event(struct core *c, const char *id, size_t n, int menu);
/* select(id:) (Status' Prepare Again) */
void prep_select_id(struct core *c, const char *id);
/* loadPlaylists() with no follow-up (the Status tab reads the names) */
void prep_load_playlists(struct core *c);
/* playlistName(id): its name, or "Playlist <id>" (into b) */
void prep_playlist_name(struct core *c, const char *id, tbuf *b);
const char *prep_running_set(struct core *c);           /* runningSet: the run's playlist id, NULL none */
void prep_run_summary(struct core *c, tbuf *b);         /* runSummary */
/* stopAndWait's first half (the flow then waits for prep_running to go 0): stop the run */
void prep_stop(struct core *c);
/* prepareAction after its dialog (flows_prepare_ask's answer, main): seq the estimate it was
 * asked on, id its playlist, first the tracks of "Prepare the First N Tracks" (-1: all), raised
 * the limit "Raise the Limit" wrote (0: none) */
void prep_ask_go(struct core *c, uint64_t seq, const char *id, int64_t first, double raised);
/* the run's playlist's name ("" none), for "“x” is being prepared" */
const char *prep_run_name(struct core *c);
/* prepareFacts() now, then prepareBaseArgs into argv (14 slots; strings in the scratch): their
 * count; *problems: prepareProblems' line count */
int32_t prep_base_args_now(struct core *c, const char **argv, int32_t *problems);
/* the watcher's: prepareFacts(measured: false) from f, prepareProblems into problems ("" none),
 * prepareBaseArgs into argv (main) */
int32_t prep_watch_args(struct core *c, const struct core_facts *f, const char **argv, char *problems, size_t cap);

/* ---------------------------------------------------------------- the Status tab (status_tab.c) */

typedef struct status_kv { char *k, *v; } status_kv;
typedef struct status_dict { status_kv *a; int32_t n, cap; } status_dict;   /* a few names: linear */
typedef struct status_set { char *name, *until; int64_t files; } status_set;   /* KeptSet as the host read it */
typedef struct status_state {
    status_set *sets; int32_t nsets;
    status_dict states;            /* what each set's check found, by name */
    status_dict checking;          /* the sets being checked now (values unused) */
    status_dict counts;            /* each set's tracks as its check found them, decimal */
    uint64_t *jobs; int32_t njobs, capjobs;   /* the checks under way, stopped when the gen moves */
} status_state;
void status_init(struct core *c);
void status_free(struct core *c);
void status_forget(struct core *c);                     /* forget() */
void status_reload_if_showing(struct core *c);          /* reloadIfShowing() */
void status_will_appear(struct core *c);                /* viewWillAppear: reload() */
void status_update_buttons(struct core *c);             /* updateButtons() */
void status_names_changed(struct core *c);              /* statusTab.table.reloadData() after the playlists came */
void status_set_forget(struct core *c, const char *id); /* states[id] = nil */
int status_event(struct core *c, const char *id, size_t n, int menu);
/* Stop Keeping… confirmed and the set removed (flows_stop_keeping's end, main): states[set] and
 * counts[set] = nil, then reload() */
void status_stopped_keeping(struct core *c, const char *set);   /* Prepare Again, Stop Keeping…: 1 handled */

#endif
