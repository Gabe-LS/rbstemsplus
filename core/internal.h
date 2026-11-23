/* The core's private declarations: metadata tables, arenas, the live model and snapshots, jobs,
 * the core's life. Not part of the API. C11. */
#ifndef RBSP_CORE_INTERNAL_H
#define RBSP_CORE_INTERNAL_H

#include "core.h"
#include "plat_shim.h"
#ifdef _WIN32
/* windows.h's CALLBACK (__stdcall, a no-op on x64 and arm64) and TEXT (a string literal's TCHAR
 * form) clash with this file's CALLBACK(c, call) and the .def tables' TEXT X-macro. The core's own
 * files never use the Windows ones. Here, after the platform include, so it holds whether that is
 * plat_shim.h or common/plat.h. */
#  undef CALLBACK
#  undef TEXT
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(void *) == 8, "the core is built for 64-bit targets only");
_Static_assert(sizeof(int) == 4, "C enums cross the ABI as 32-bit ints");

/* Checks. Debug and test builds (CORE_DEBUG) abort with the expression and the function; release
 * builds carry none (no __FILE__ or __LINE__ in release objects, mac-core §3.1). */
#ifdef CORE_DEBUG
_Noreturn void core_fail_(const char *what, const char *func);
#  define CORE_CHECK(cond) ((cond) ? (void)0 : core_fail_(#cond, __func__))
#  define CORE_VIOLATION(what) core_fail_(what, __func__)
/* every counted object the core allocates (sections, rows, tables, snapshots, jobs, lives): the
 * tests' leak check. The one mutable global, and only in CORE_DEBUG builds. */
extern atomic_long core_debug_live;
#  define LIVE_INC() atomic_fetch_add_explicit(&core_debug_live, 1, memory_order_relaxed)
#  define LIVE_DEC() atomic_fetch_sub_explicit(&core_debug_live, 1, memory_order_relaxed)
#else
#  define CORE_CHECK(cond) ((void)0)
#  define CORE_VIOLATION(what) ((void)0)
#  define LIVE_INC() ((void)0)
#  define LIVE_DEC() ((void)0)
#endif
long core_debug_live_objects(void);   /* CORE_DEBUG: the count above; otherwise -1 */

/* ------------------------------------------------------------------ metadata (schema.c) */
enum { K_STR = 0, K_FLAG = 1, K_I64 = 2, K_F64 = 3 };
#define CORE_KIND_str K_STR
#define CORE_KIND_flag K_FLAG
#define CORE_KIND_i64 K_I64
#define CORE_KIND_f64 K_F64

typedef struct { const char *name; uint8_t section; uint8_t kind; uint16_t index; } field_meta;
typedef struct { const char *name; uint8_t table; uint8_t kind; uint16_t index; } column_meta;
typedef struct { const char *name; uint8_t section; uint16_t ncols; } table_meta;
typedef struct { const char *name; uint16_t nfields; } section_meta;
typedef struct { const char *name; uint8_t kind; } fact_meta;
typedef struct { const char *name; const char *keys; } event_meta;

extern const field_meta core_field_meta[CORE_FIELD_COUNT > 0 ? CORE_FIELD_COUNT : 1];
extern const column_meta core_column_meta[CORE_COLUMN_COUNT > 0 ? CORE_COLUMN_COUNT : 1];
extern const table_meta core_table_meta[CORE_TABLE_COUNT > 0 ? CORE_TABLE_COUNT : 1];
extern const section_meta core_section_meta[CORE_SECTION_COUNT];
extern const fact_meta core_fact_meta[CORE_FACT_COUNT > 0 ? CORE_FACT_COUNT : 1];

/* events (events.c) */
enum core_event_id {
#define EVENT(id, name, os, keys) CORE_KEEP_##os(id,)
#include "events.def"
    CORE_EVENT_COUNT
};
extern const event_meta core_event_meta[CORE_EVENT_COUNT];
int core_event_lookup(const char *name, size_t n);   /* the event's id, or -1 */

/* texts (text.c) */
_Static_assert(CORE_SLOT_COUNT <= 64, "core_text keeps the slots given and used in 64-bit sets");
const char *core_text_template(core_os os, int32_t id);   /* NULL: no such text for os */
extern const char *const core_slot_names[CORE_SLOT_COUNT > 0 ? CORE_SLOT_COUNT : 1];
int core_text_check_all(void);   /* 0, or the first text whose slots are wrong, +1 */

/* menus (menus.c) */
typedef struct {
    int16_t id, parent, kind, title;
    const char *key_mac, *key_win;
    uint32_t mods_mac, mods_win;
    const char *event;
    uint8_t os;                     /* bit 1: mac, bit 2: win */
} menu_row;
extern const menu_row core_menu_rows[];
extern const int32_t core_menu_rows_n;

/* ------------------------------------------------------------------ arenas (arena.c)
 * Per-event scratch and a section's own strings (mac-core §3.10). Chunked bump allocation; freed
 * all at once. */
typedef struct arena_chunk arena_chunk;
typedef struct { arena_chunk *head; size_t used_total; } arena;
void *arena_alloc(arena *a, size_t n);                  /* 16-byte aligned; NULL only when out of memory */
char *arena_strdup(arena *a, const char *s, size_t n);  /* NUL-terminated copy */
void arena_reset(arena *a);                             /* frees every chunk but the first, rewinds it */
void arena_free(arena *a);

/* ------------------------------------------------------------------ JSON (json.c)
 * A strict RFC 8259 reader for the low-rate events and the start config: UTF-8 checked, depth
 * limited, "\u0000" refused (strings are C strings), duplicate keys refused. Into an arena. */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct jval jval;
struct jval {
    int type;
    int b;                 /* J_BOOL */
    double num;            /* J_NUM */
    int is_int;            /* J_NUM: an integer that fits int64 */
    int64_t i;
    const char *s;         /* J_STR: NUL-terminated, valid UTF-8, no NUL inside */
    size_t len;
    size_t n;              /* J_ARR, J_OBJ: element count */
    const char **keys;     /* J_OBJ */
    size_t *key_lens;
    jval *items;
#ifdef CORE_TEST_HOOKS
    int scale_steps;       /* J_NUM: steps the scaling took (R7: the tests assert a bound) */
#endif
};
int json_parse(arena *a, const char *p, size_t n, jval *out);   /* CORE_OK or CORE_E_PARSE / CORE_E_NOMEM */
/* json_parse with another input limit and array limit (an object's members stay at most 256):
 * a file the core reads (kept.noticed), never an event */
typedef struct { size_t bytes, arr_items, values; int depth; } json_lim;
int json_parse_lim(arena *a, const char *p, size_t n, const json_lim *lim, jval *out);
const jval *json_get(const jval *obj, const char *key);
int utf8_valid(const char *p, size_t n);

/* the pure functions (M5): before the view model's state, which holds their types */
#include "pure.h"

/* ------------------------------------------------------------------ cells, sections, rows, tables, snapshots (model.c) */
typedef struct {
    uint8_t kind;
    uint32_t len;          /* K_STR */
    union { int64_t i; double f; const char *s; } v;
} cell;

typedef struct section_obj {
    atomic_int refs;
    uint8_t section;
    size_t garbage;        /* bytes of replaced strings still in strs (compacted past a limit) */
    arena strs;            /* the section's own strings */
    cell cells[];          /* core_section_meta[section].nfields */
} section_obj;

typedef struct row_obj {
    atomic_int refs;
    uint64_t rev;
    uint16_t ncells;
    cell cells[];          /* then the strings, packed */
} row_obj;

typedef struct table_obj {
    atomic_int refs;
    uint8_t table;
    uint64_t list_gen;
    int32_t n, cap;
    row_obj **rows;
} table_obj;

#define NTAB (CORE_TABLE_COUNT > 0 ? CORE_TABLE_COUNT : 1)

struct core_delta {
    uint32_t sections;
    uint8_t reload[NTAB];
    int32_t old_rows[NTAB];
    int32_t nchanged[NTAB];
    int32_t *changed[NTAB];
};

struct core_snap {
    atomic_int refs;
    uint64_t rev[CORE_SECTION_COUNT];
    section_obj *sec[CORE_SECTION_COUNT];
    table_obj *tab[NTAB];
    core_delta delta;
};

/* The live model, on main: private or shared sections and tables, written copy-on-write, and what
 * moved since the last render. */
typedef struct {
    uint64_t counter;      /* revisions, row revs and list_gens come from one counter */
    uint64_t rev[CORE_SECTION_COUNT];      /* each section's revision now */
    section_obj *sec[CORE_SECTION_COUNT];
    table_obj *tab[NTAB];
    uint32_t dirty_sections;              /* written since the last freeze: rev to bump */
    /* since the last render: */
    uint64_t last_rev[CORE_SECTION_COUNT];
    uint64_t last_gen[NTAB];
    int32_t last_rows[NTAB];
    int32_t *changed[NTAB];               /* dirty row indices < last_rows, unsorted */
    int32_t nchanged[NTAB], capchanged[NTAB];
    uint8_t *seen[NTAB];                  /* one bit per row: already in changed */
    int32_t seencap[NTAB];
    int rendered;          /* a render happened: deltas are relative to it */
} model;

int model_init(model *m);
void model_free(model *m);
int model_set_str(model *m, core_field f, const char *v, size_t n);
int model_set_i64(model *m, core_field f, int64_t v);     /* K_FLAG and K_I64 */
int model_set_f64(model *m, core_field f, double v);
int64_t model_get_i64(const model *m, core_field f);
/* f's section rendered again though nothing in it changed (a refused value redrawn: SO3) */
void model_touch(model *m, core_field f);
int model_table_reset(model *m, core_table t, int32_t rows);
int model_table_append(model *m, core_table t, int32_t rows);
int model_cell_str(model *m, core_column col, int32_t row, const char *v, size_t n);
int model_cell_i64(model *m, core_column col, int32_t row, int64_t v);
int model_cell_f64(model *m, core_column col, int32_t row, double v);
#ifdef CORE_WIN_POLICY
/* R1's snapshot fields from FACT_R1_STATE and FACT_R1_DISMISSED (win.c), for M4's refresh; here,
 * after the model's type (pure.h comes before it) */
int win_r1_fields(model *m, int64_t r1_state, int dismissed, int action_running);
/* FACT_STEP_CODE for confirmed Windows LOCATION_CREATE / LOCATION_WRITE and SETTINGS timeouts. */
enum { WIN_LOCATION_TIMED_OUT = 6, WIN_LOCATION_SAVED_UNCHECKED = 7 };
#endif
/* 1 when anything moved since the last render (or there was none yet): a commit renders. */
int model_needs_render(const model *m);
/* A snapshot of the model now, +1. With for_render, it carries the delta since the last render,
 * and the model's "since the last render" state restarts from it. NULL when out of memory. */
core_snap *model_freeze(model *m, int for_render);

/* ------------------------------------------------------------------ the core (core.c, flow.c) */

/* the gens of today's independent domains: a main job carries (domain, gen) and is dropped when
 * that domain's gen moved. DOM_STATUS: what a confirmation describes (M6's guard; M4 moves it when
 * the installed features, the location or rekordbox's version change); DOM_PREPARE: PrepareTab.gen;
 * DOM_LIST: PrepareTab.listGen; DOM_SIZE: the size walk; DOM_SETS: StatusTab.gen (M4). */
enum { DOM_STATUS, DOM_PREPARE, DOM_LIST, DOM_SIZE, DOM_SETS, DOM_COUNT };

#define CORE_WIN_PROGRAMS_MAX 256   /* win_programs' bytes (win_rules.h's WR_PF_MAX and the NUL fit) */
#ifdef CORE_WIN_POLICY
#define CORE_PROGRAMS(c) ((const char *)(c)->win_programs)
#else
#define CORE_PROGRAMS(c) ((const char *)NULL)
#endif

/* the start config's paths (core.h: core_new), by index */
enum { P_HOME, P_SUPPORT, P_APP, P_HELPER, P_CACHE_DEFAULT, P_LOG, P_TMP, P_WATCHER,
       P_MODEL, P_ROOT_ORT, P_REKORDBOX, P_KEPT, P_COUNT };

/* host entries the start config may hand to the DLL's Windows code (CORE_WIN_POLICY only) */
enum { NATIVE_POST_WORK = 1, NATIVE_SLOW_FACT = 2, NATIVE_STEP = 4, NATIVE_ADMIN = 8, NATIVE_HELPER = 16 };

typedef struct core_life core_life;
typedef void (*job_fn)(core *c, void *payload);              /* main jobs */
typedef void (*work_fn)(core_life *life, void *payload);
struct core_job {
    core_life *life;
    struct core_job *next;           /* the core's deferred queue */
    int on_main;
    job_fn main_fn;
    work_fn work_fn;
    void (*drop_fn)(void *payload);  /* the cleanup when the job is dropped, or the core is gone; may be NULL */
    int domain;                      /* main jobs: dropped when gen != 0 and the domain's gen moved */
    uint64_t gen;
    void *payload;                   /* free()d after the job */
};

enum { RUNS_MAX = 32, PURPOSES = 4 };
typedef struct hrun hrun;
typedef struct { uint64_t id; int32_t purpose; struct core_job *end_job; hrun *h; } run_slot;   /* end_job: reserved at the start (E2) */

/* M4's helper runs: what a run's lines become, parsed on the host's reader thread by M5's parser
 * (mac-core §3.2: never on main). The starter fills kind, gen and the handlers on main before the
 * spawn; the reader thread owns the accumulator (a, pls, files, e, la) until the run's end job
 * hands it back to main, after the run's last line. */
enum { HK_PLAYLISTS = 1, HK_TRACKS = 2, HK_ESTIMATE = 3, HK_RUN = 4 };
struct hrun {
    int kind;
    int domain; uint64_t gen;            /* the per-line jobs are dropped once this gen moved (0: never) */
    /* reader side, then main's at the end */
    arena a;                             /* the results' strings */
    arena la;                            /* one line's scratch */
    prep_playlist *pls; int32_t npl, cappl;           /* HK_PLAYLISTS: parsePlaylists */
    pstr *files; int32_t nfiles, capfiles;            /* HK_TRACKS: parseTrackList */
    prep_estimate e;                                  /* HK_ESTIMATE: parseEstimate */
    int oom;                             /* a line was lost to an allocation failure */
    int32_t status;                      /* the exit status (core_helper_ended), -1 not started */
    int32_t deadline_ms;                 /* runPrepareHelper's timeout: the helper is stopped after it (0 none) */
    char *err;                           /* stderr's tail, whitespace trimmed ("" none) */
    /* main */
    void *ctx;                           /* the starter's own, freed with ctx_free (NULL: free) */
    void (*ctx_free)(void *);
    void (*on_track)(struct core *c, hrun *h, const prep_track *t);      /* HK_ESTIMATE: a track's line (parseRunLine's .track) */
    void (*on_line)(struct core *c, hrun *h, const prep_run_line *l);    /* HK_RUN: each line */
    void (*on_end)(struct core *c, hrun *h);                             /* always once, on main (also when not started) */
};
hrun *hrun_new(int kind);
void hrun_free(hrun *h);

#include "flows.h"
#include "view.h"

struct core {
    core_life *life;
    plat_thread_id main_thread;
    int32_t role;
    core_os os;
    uint32_t native;       /* NATIVE_* */
#ifdef CORE_WIN_POLICY
    int prepare_tabs;      /* the start config's "prepare_tabs": the shell shows the Status and Prepare tabs */
#endif
    char *version;
    char *paths[P_COUNT];
    model m;
    arena scratch;         /* per event / per job, reset when the outermost core call returns */
    uint64_t gen[DOM_COUNT];
    /* entries and callbacks on main (D2) */
    int entry;             /* nesting of core calls on main */
    int cb_depth;          /* inside a host callback on main */
    int free_pending;
    int warned_nested;     /* "a nested call": logged once (main only) */
    atomic_int warned_thread;   /* "the wrong thread": logged once (any thread) */
    core_job *deferred, *deferred_tail;
    /* the turn's one commit */
    int commit_posted;
    /* busy state (actionRunning) and the Prepare run: core state on main */
    int flow_running;      /* the normal flow (CORE_WORK_FLOW) */
    int action_running;
    int preparing, paused;
    /* close and quit (D2) */
    int close_pending[2];  /* CORE_CLOSE_WINDOW, CORE_CLOSE_QUIT: a decision is coming */
    int close_approved[2]; /* close_reply(what, 1) was sent: the re-ask answers NOW */
    int quit_flow_running;
    /* helper runs (D3): per run, per purpose */
    uint64_t next_run;
    run_slot runs[RUNS_MAX];
    uint64_t current_run[PURPOSES];
    int64_t run_lines[PURPOSES];
#ifdef CORE_TEST_HOOKS
    int32_t deadline_test_ms;     /* the tests: every helper deadline this long instead (0: today's) */
    int32_t size_every_test_ms;   /* the tests: the size's timer this often instead (0: 10 minutes) */
#ifdef CORE_WIN_POLICY
    int32_t size_retry_test_ms;   /* the tests: the location's retry this long instead (0: retry_ms) */
#endif
    uint64_t size_test_now;       /* the tests: the size's monotonic clock (0: the real one) */
    struct { uint64_t run; int32_t ms; } deadline_seen[64];   /* the tests: each M4 run's own deadline, as armed */
    int32_t ndeadline_seen;
    int click_gate_off;           /* the flows' tests: an action's click or menu choice as its button or item on */
    int32_t stub_row[PURPOSES];   /* the G1 spike's stub parser: the next estimate row, per purpose */
    core_test_mark_fn test_mark;  /* the probe's timing points (core_test_set_mark), NULL: none */
    void *test_mark_ctx;
#endif
    char menu_title[256];  /* core_menu_state's title, valid until the next call */
#ifdef CORE_WIN_POLICY
    /* Windows' own Program Files folder (no trailing '\'): read once by core_new in the DLL
     * (native_win_programs: plat_program_files, the worker's, the bridge's and the watcher's
     * rule), so the admin root, R1 and the texts naming rekordbox's folder can't disagree; ""
     * elsewhere (the host tests set it) */
    char win_programs[CORE_WIN_PROGRAMS_MAX];
#endif
    flows_state fl;        /* the action flows' state (flows.c) */
    view_state v;          /* the view model's app section (view.c) */
    prep_state p;          /* the Prepare tab (prepare_tab.c) */
    status_state st;       /* the Status tab (status_tab.c) */
};

/* What outlives core_free: the host table, the alive mark, and the core header itself. */
struct core_life {
    atomic_int refs;
    atomic_int alive;      /* cleared by core_free: queued work is dropped, late calls do nothing */
    atomic_int flows_out;  /* flows of any kind not finished yet */
    atomic_int admin_note; /* core_admin_note's latch for the running admin attempt (M6) */
#ifdef CORE_WIN_POLICY
    uint32_t win_closing_max_ms;   /* W-core: rekordbox's linger is waited for this long (0: 90 s; tests set it) */
    /* the "native" dispatch (native.c): with entries listed, host holds trampolines with ctx = the
     * life, which call the shell's own table (shell) or the DLL's backend (nb, its state nst) */
    uint32_t native_listed;        /* NATIVE_* the start config listed */
    atomic_int native_elevating;   /* native admin calls in flight: front_facts adds CORE_FRONT_ELEVATING */
    core_host shell;
    const struct native_backend *nb;
    void *nst;
#endif
    atomic_int asked;      /* the running normal flow showed a dialog or turned busy on (M6, F4) */
    atomic_int flow_quits; /* the running normal flow ended with the app quitting: busy stays on (CF1) */
    atomic_int flow_marked; /* the normal flow stopped busy during an action: its end checks again (OM3) */
    core_host host;
    core_os os;
    uint32_t native;
    plat_thread_id main_thread;
#ifdef CORE_TEST_HOOKS
    atomic_int fail_allocs;
    atomic_int fail_after;   /* core_test_fail_alloc_after: allocations still to succeed before one fails, -1 none */
    atomic_int fail_after_more;   /* ... and how many more fail right after it */
    atomic_int stub_parse;   /* core_test_helper_stub: the G1 spike's stub parser is on */
#endif
    core c;
};

#ifdef CORE_WIN_POLICY
/* native.c: the entries "native" lists, wired to the DLL's backend (the shell's table kept); 0, or
 * CORE_E_NOMEM when the backend's state can't be made. native_release: its state closed, from
 * life_release's last reference. */
int native_install(core_life *life, uint32_t listed);
void native_release(core_life *life);
#endif

int core_on_main(const core *c);
int life_on_main(const core_life *l);
#define CORE_MAIN(c) CORE_CHECK(core_on_main(c))

core_life *life_retain(core_life *l);
void life_release(core_life *l);
int life_alive(const core_life *l);
/* Posts fn(c, payload) to main through the host. payload is owned by the job (free()d after);
 * drop_fn is its cleanup when dropped. 0, or CORE_E_NOMEM / CORE_E_STATE when it couldn't be posted
 * (then drop_fn has run and payload is freed). */
int core_post_main(core_life *life, job_fn fn, void *payload, void (*drop_fn)(void *), int domain, uint64_t gen);
int core_post_work(core_life *life, core_work kind, work_fn fn, void *payload, void (*drop_fn)(void *));
void core_mark_changed(core *c);   /* schedule the turn's one commit */
/* E2: a job that ends a state the core waits on is reserved when the state begins: job_reserve makes a
 * main job (NULL when out of memory: the caller still owns payload); job_post posts it and can't fail. */
core_job *job_reserve(core_life *life, job_fn fn, void *payload, void (*drop_fn)(void *));
void job_post(core_life *life, core_job *j);
/* an entry frame on main (E3): c->entry++ before, core_leave after (deferred jobs, scratch, free) */
void core_leave(core *c);
void core_log(core_life *life, const char *line);
/* a host callback on main: counted, so the core sees re-entry */
#define CALLBACK(c, call) do { (c)->cb_depth++; call; (c)->cb_depth--; } while (0)

/* helper runs: starts one through host->helper_spawn for `purpose` (0..3); its run id, or 0 when
 * not started. main (the test hooks and the G1 spike). */
uint64_t helper_start(core *c, int32_t purpose, const char *const *argv, int32_t argc);
/* M4's runs: starts h's run. Its run id, or 0 when not started: then h->on_end is posted to run
 * later on main (status -1, err ""), or, when an allocation fails (and always for HK_RUN), h and
 * its ctx are freed without it. Either way h belongs to the core from here: the caller never
 * touches h or its ctx after the call. main. */
uint64_t helper_start_run(core *c, hrun *h, const char *const *argv, int32_t argc);

/* flows (flow.c) */
typedef void (*flow_fn)(core_life *life, void *arg);
/* arg_free: how arg is released when the flow can't start or its job is dropped (NULL: free()) */
int flow_start(core *c, core_work kind, flow_fn fn, void *arg, void (*arg_free)(void *));
/* the same; busy 0: the busy state (action_running) stays as it is, for a flow whose start is
 * what was a main-thread dialog in 1.3.0 (its busy state comes from the flow's own busy()) */
int flow_start_ex(core *c, core_work kind, flow_fn fn, void *arg, void (*arg_free)(void *), int busy);
int flow_revalidate(core_life *life, int domain, uint64_t gen);   /* 1: the state the dialog was about still holds */
/* runs q(c, arg) on main and waits for its answer; if_gone when the core is gone or the job is dropped */
int flow_query(core_life *life, int (*q)(core *c, void *arg), void *arg, int if_gone);
/* the quit flow beside the flow thread (core_close_asked during a Prepare run) */
int quit_flow_start(core *c);
/* flow: the Prepare run's whole playlist name now ("" none), the flow's own copy to free(); NULL
 * when it couldn't be asked (out of memory, or the core gone); and stopAndWait (1 once no run
 * goes) (M4) */
char *flow_run_name(core_life *life);
int flow_stop_and_wait(core_life *life);

/* the dialog under construction (dialog.c) */
#define DLG_MAX_BUTTONS 8
#define DLG_MAX_FIELDS 4
#define DLG_MAX_ROWS 4
struct core_dialog {
    int32_t id;
    const char *title, *message;
    int32_t nbuttons, nfields, nrows, def, cancel, inline_;
    const char *buttons[DLG_MAX_BUTTONS];
    const char *field_label[DLG_MAX_FIELDS], *field_value[DLG_MAX_FIELDS];
    const char *row_label[DLG_MAX_ROWS];
    int32_t row_nbuttons[DLG_MAX_ROWS];
    const char *row_buttons[DLG_MAX_ROWS][DLG_MAX_BUTTONS];
    /* masks, so a zeroed dialog is fully enabled and nothing sits on the left */
    uint32_t disabled_buttons;
    uint32_t disabled_row_buttons[DLG_MAX_ROWS];
    uint32_t left_buttons;
};
struct core_reply {
    int32_t button, row, row_button;    /* -1: not set */
    arena *a;
    const char *fields[DLG_MAX_FIELDS];
    size_t field_lens[DLG_MAX_FIELDS];
};
void reply_init(core_reply *r, arena *a);
/* Settings' enable rules, as today (Settings.swift:29-32), on a dialog laid out as Settings is:
 * buttons Save, Clear Saved Stems, Cancel; row 0's buttons Change…, Use This Mac. */
void dlg_settings_enable(core_dialog *d, int readable, int clear_target, int can_choose, int this_mac);

/* fact sheets (facts.c) */
struct core_facts {
    arena *a;
    uint32_t parts;   /* CORE_FACTS_* asked of host->facts (M71); 0 for slow_fact and step sheets */
    uint8_t set[CORE_FACT_COUNT > 0 ? CORE_FACT_COUNT : 1];
    int64_t i[CORE_FACT_COUNT > 0 ? CORE_FACT_COUNT : 1];
    const char *s[CORE_FACT_COUNT > 0 ? CORE_FACT_COUNT : 1];
    size_t len[CORE_FACT_COUNT > 0 ? CORE_FACT_COUNT : 1];
};
void facts_init(core_facts *f, arena *a);

#ifdef CORE_WIN_POLICY
/* A known location memo can still supply its last drive and size while the check is stale. */
enum { VIEW_LOC_STALE = 2 };

static inline int win_location_unchecked(int32_t os, const core_facts *f) {
    return os == CORE_OS_WIN && f->set[FACT_CONFIG_LOCATION] &&
           !(f->set[FACT_LOC_MEASURED] && f->i[FACT_LOC_MEASURED]);
}
#endif

#endif
