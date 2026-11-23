/* The action flows (mac-core §3.7, M6): their state on main and the entries core.c calls. Private
 * to the core. The flows themselves are in flows.c. */
#ifndef RBSP_CORE_FLOWS_H
#define RBSP_CORE_FLOWS_H

#include <stdint.h>
/* included by internal.h after pure.h (prep_estimate) */

struct core;
struct core_life;

#define FLOWS_QUEUE_MAX 8

/* The flows' state on main (today's Controller flags). Only main reads or writes it; a flow
 * changes it through jobs it posts to main. */
typedef struct flows_state {
    int startup_done;        /* the start-up sheets are done: the watcher's request may be asked */
    int request_waiting;     /* the watcher's request waits for rekordbox to be closed */
    int cache_update_asked;  /* this run asked "Stems Cache update" */
    int action_marked;       /* an action that changes things is running (today's currentAction) */
    int busy_shown;          /* EF_BUSY shown and no EF_IDLE since: the flow's end turns it off (CF1, CF2) */
    char *update_version;    /* a newer app's version (the notice and Update RB Stems Plus…), or NULL */
    char *admin_status;      /* what the next admin attempt's progress shows */
    char *dl_what;           /* the file a download step is fetching, for its status lines */
    int32_t del_text, del_text_n;   /* a deletion's status texts (core_text_id), -1 none */
    char *del_drive;
    int model_notice;        /* 1.0's saved copy of Pioneer's model was ours: T_NOTICE_NO_SAVED_MODEL this run (M4 shows it) */
    int cache_offer_pending; /* a payload.json came while a flow ran: the cache offer once it ends (Q9) */
    int8_t queued[FLOWS_QUEUE_MAX];   /* clicks delayed while a flow ran without a dialog (Q11), in order */
    int nqueued;
    int shown_done;          /* "shown" came: the start-up flow ran (F15) */
    int startup_running;     /* the start-up flow runs: its end marks it done (OM1) */
    int startup_retry;       /* the start-up flow couldn't start (out of memory): the next activation starts it */
    int warned_late;         /* a late progress was logged once (F20) */
    int prep_reserved;       /* SO1: a flow's preflight found no run (or stopped it): no Prepare may start
                              * until that flow ends (1.3.0 ran preflight and busy back to back on main) */
#ifdef CORE_WIN_POLICY
    int win_update_after;    /* Windows: "Update RB Stems Plus first" was chosen: the update once this flow ends */
    int win_uninstall_asked; /* Windows: Settings > Apps asked to uninstall while a flow ran: once it ends */
    char win_r1_shown[256];  /* Windows: the versions R1's Don't Ask Again showed at its click ({"ev":"r1_dismiss"}),
                              * comma-separated, for the next Don't Ask Again flow (it takes and clears them) */
#endif
} flows_state;

/* main */
void flows_init(struct core *c);
void flows_free(struct core *c);
/* {"ev":"click"} and {"ev":"menu"}: 1 when id names a flow (started, or refused because another
 * flow runs), 0 when it isn't a flow's */
int flows_event(struct core *c, const char *id, size_t n);
#ifdef CORE_WIN_POLICY
/* {"ev":"r1_dismiss","versions":"7.2.20,7.2.19"} (Windows): R1's Don't Ask Again with the versions
 * its button showed when clicked (F_WIN_R1_DISMISS_VERSIONS then), as the click "r1_dismiss" */
void flows_win_r1_dismiss(struct core *c, const char *versions, size_t n);
void flows_win_inventory_changed(struct core *c);
#endif
/* {"ev":"shown"}: welcome, the update result, resume, the watcher's request, the cache offer,
 * the update check */
void flows_shown(struct core *c);
/* {"ev":"reopened"} and {"ev":"check_request"}: the watcher's request (checkRequest) */
void flows_check_request(struct core *c);
/* a flow's end, on main (any kind): the delayed clicks, then a pending cache offer */
void flows_flow_ended(struct core *c);
/* a normal flow's end, on main, first: busy(nil) when the flow left busy on (its EF_IDLE lost to
 * an allocation, or never posted by a flow that stopped), unless it ended with the app quitting */
void flows_flow_end_busy(struct core *c);
/* core_flow_progress's job */
void flows_progress(struct core *c, int32_t stage, int64_t done, int64_t total);

/* The watcher's prompt (watcherPrompt, Watcher.swift:247), shown from the watcher's round flow
 * (M5): what went missing, judged against the last payload.json's version lists. Returns 0
 * Reinstall Now, 1 Remind Me Later, 2 Don't Ask Again; -1 when nothing can be reinstalled (the
 * "OK" form) or no button was pressed. *dont_ask: "Don't Ask Again for This Version" was pressed
 * (both forms). Flow thread. */
/* CORE_ROLE_WATCHER's {"ev":"start"}: the watcher's rounds (watcherRounds over watcherCheck) as a
 * flow; its end is close_reply(CORE_CLOSE_WATCHER_DONE, 1) */
int flows_watch_start(struct core *c);
int flows_watcher_prompt(struct core_life *life, int model_off, int cache_off, const char *engine, const char *rekordbox,
                         int engine_ok, int rekordbox_ok, int plus_works, int *dont_ask);

/* prepareAction's dialogs (PrepareView.swift:750, M4), as a flow that doesn't turn the busy state
 * on: "Can't prepare" (PREP_ASK_REFUSED, e->refused its text), "doesn't fit" (PREP_ASK_FIT: the
 * free space of `folder`, the Stems Cache folder, measured on the flow thread; Prepare the First N
 * Tracks, or the limit raised in config.ini) and "Not enough free space" (PREP_ASK_SPACE). An
 * answer that prepares reaches main as prep_ask_go(seq, id, first). id and name: the playlist's.
 * Main; 0 started, else not (another flow runs, or out of memory). */
enum { PREP_ASK_REFUSED = 0, PREP_ASK_FIT = 1, PREP_ASK_SPACE = 2 };
int flows_prepare_ask(struct core *c, int kind, const char *id, const char *name, const prep_estimate *e,
                      const char *folder, uint64_t seq);
/* Stop Keeping…'s confirmation (StatusTab.stopKeepingSet, PrepareView.swift:256), as a flow that
 * doesn't turn the busy state on: "Stop keeping “name”?" [Stop Keeping] [Cancel]; confirmed, the
 * set is removed (CORE_STEP_STOP_KEEPING) and logged, then main forgets its state and reloads the
 * Status tab (status_stopped_keeping). set: the kept set's name; name: its playlist's name now.
 * Main; 0 started, else not. */
int flows_stop_keeping(struct core *c, const char *set, const char *name);

/* Pure decisions, tested on their own (Actions.swift, Update.swift). */
int flows_version_newer(const char *a, const char *b);   /* String.compare(.numeric) == .orderedDescending */
int flows_is_sha256(const char *s);
int flows_cache_from_saved_payload(int unverified, int have_age, int64_t age, int bridge_verified);
/* cacheUpdateDue (Actions.swift:662): `blocked` (it runs codesign) asked last, only when the rest says due */
int flows_cache_update_due(int present, int chosen, const char *installed, const char *latest, int app_update_pending,
                           int (*blocked)(void *ctx), void *ctx);
/* queuedStart (Actions.swift:724): 0 start, 1 updating, 2 cancelled, -1 a check failed (a callback's -1) */
int flows_queued_start(int (*updating)(void *ctx), int (*rekordbox_open)(void *ctx), int (*try_again)(void *ctx), void *ctx);
void flows_missing_parts(int chosen_model, int chosen_cache, int plus_present, int cache_present, int *model, int *cache);
int flows_drive_deletion_allowed(const char *confirmed_dir, const char *confirmed_id, const char *fresh_dir, const char *fresh_id);

#ifdef CORE_TEST_HOOKS
/* Test builds only: called (with the host's ctx) by the background payload fetch between its
 * job's start (core_run's liveness check) and its step, so flows_test.c can hold it there until
 * a core_free has landed in that window, the window a scheduler hits only now and then (NULL:
 * none). */
extern void (*_Atomic flows_test_fetch_hold)(void *ctx);
#endif

#endif
