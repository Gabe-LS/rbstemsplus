/* native_location.c: CORE_SLOW_LOCATION's facts on Windows (review host-facts HF5), the Mac's
 * LocationMonitor.measureCheapState over plat.h: config.ini's location (common/cfg_ini.c), the
 * chosen folder's probe (Location.swift probeLocation, step for step), whether rekordbox runs and
 * the bridge's state for it (parseBridgeState and liveBridgeState: core/state.c's), the folder in
 * use (folderInUse) and its floor (cachePausedFloor). What only Windows answers, or what another
 * lane reads (the live bridge's lines: host_facts), comes through native_location_ops, so the Mac
 * tests run the whole measure on a scratch tree.
 *
 * The DLL (core_native.c) answers the live bridge's presence and location=1 from host_facts.c's
 * read (host_facts_live_bridge: the current rekordbox's onnxruntime.dll, compat.c's memoised
 * scan), and answers CORE_SLOW_LOCATION through native_location_slow_fact: the measure through
 * native_location_measure_bounded, never on the UI thread, waited for a bounded time. The location
 * choice and measure share native_location.c's release gate. */
#ifndef RBSP_WIN_NATIVE_LOCATION_H
#define RBSP_WIN_NATIVE_LOCATION_H

#include "path_rules.h"
#include "plat.h"
#include "volumes.h"
#ifndef RBSP_CORE_PLAT_SHIM_H
#  define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h has the shim's names */
#endif
#include "internal.h"   /* the core's arena, and pure.h's LOC_* and PROBE_* */

#include <stddef.h>
#include <stdint.h>

#define NL_PATH_MAX 1024
#define NL_DRIVE_MAX 256
#define NL_IDENTITY_MAX 49       /* volume serial and 128-bit directory ID, in hex */

void native_location_identity(plat_id id, char out[NL_IDENTITY_MAX]);

typedef struct {
    void *ctx;
    /* stemsCachePresent() and bridgeSaysLocation(), from one read of the live bridge: 0 with
     * *present (the live onnxruntime.dll is our bridge) and *location (its line says location=1),
     * or -1 when there is no answer (the measure has none then) */
    int (*live_bridge)(void *ctx, int *present, int *location);
    /* rekordboxOpen(): 1, 0, -1 the process list couldn't be read (no answer then) */
    int (*rekordbox_running)(void *ctx);
    /* the process's start time in seconds since 1970 (bridge-state's start=), -1 no such process */
    int64_t (*process_start)(void *ctx, int32_t pid);
    /* the volume holding dir, named (plat_volume_name): 0 with its label and its drive letter
     * ("E:"), either "" when it has none; an errno when it isn't there (native_location_drive_name) */
    int (*volume_name)(void *ctx, const char *dir, char *label, size_t ln, char *letter, size_t lt);
    /* whether dir is on a network drive, which the table of mounts doesn't list (Windows:
     * plat_win_drive_remote); NULL: never */
    int (*remote)(void *ctx, const char *dir);
    /* the volume holding dir: 0 with its root and the picker's volume facts, or an errno.
     * NULL uses the Windows GUID root (host tests give a scratch folder as a volume). */
    int (*volume)(void *ctx, const char *dir, char *mount, size_t n, vol_facts *facts);
    /* the paused floor's inputs for the folder at path (review NL6): free bytes (0 unknown) and the
     * volume's total (0 unknown). NULL: plat_free_bytes and plat_volume_info, as the bridge measures */
    void (*space)(void *ctx, const char *path, uint64_t *avail, uint64_t *total);
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    /* Read-only This PC check: 0 verified, ENOENT absent, other error unusable. */
    int (*pc_cache_probe)(const char *support, const char *cache_default);
#endif
} native_location_ops;

typedef struct {
    const char *support;         /* %LOCALAPPDATA%\rbstemsplus: config.ini, Logs\bridge-state */
    const char *cache_default;   /* defaultCacheDir: support\Cache, this PC's folder */
    const native_location_ops *ops;
} native_location_env;

/* LocationMonitor's snapshot, as CORE_SLOW_LOCATION's facts carry it (facts.def FACT_LOC_*) */
typedef struct native_location {
    int kind;                            /* FACT_LOC_KIND: LOC_* */
    int probe;                           /* FACT_LOC_PROBE: chosen folder or offline This PC, else -1 */
    char status_drive[NL_DRIVE_MAX];     /* FACT_LOC_STATUS_DRIVE: driveName(dir), LOC_CHOSEN and LOC_BRIDGE_TOO_OLD */
    int has_status_drive;
    int rekordbox_running;               /* FACT_REKORDBOX_RUNNING */
    int has_bridge;                      /* a live bridge state: the three below */
    char bridge_state[16], bridge_reason[64];
    char bridge_drive[NL_DRIVE_MAX];     /* FACT_LOC_BRIDGE_DRIVE: not set when its location is this PC */
    int has_bridge_drive;
    int64_t paused_floor;                /* FACT_LOC_PAUSED_FLOOR: -1 none */
    char in_use[NL_PATH_MAX];            /* FACT_LOC_IN_USE: folderInUse(status) */
    int has_in_use;
    uint64_t available, total;           /* disk space read with the paused floor */
    char config_dir[PATHS_LOCATION_MAX + 1], config_id[37];  /* the chosen pair the measure read (review N3R18) */
    char identity[NL_IDENTITY_MAX];       /* the marked folder as opened by the probe */
} native_location;

/* The config.ini pair read by the measure, "" and "" when there is no chosen location. */
int native_location_matches_config(const native_location *l, const char *dir, const char *id);

/* This PC needs matching live bridge state or a successful local probe. */
int native_location_checked(const native_location *l);

/* probeLocation(dir, id): PROBE_* (app.md §5's table): no eligible GUID volume NOT_CONNECTED
 * (a network drive NETWORK), a component that can't be
 * looked at for want of permission NO_PERMISSION, one missing or not a folder FOLDER_MISSING, one
 * a link or reparse point LINK, the folder or its marker unreadable for want of permission
 * NO_PERMISSION, the marker missing or another id's MARKER_MISSING_OR_WRONG, a read-only volume
 * READ_ONLY, a folder that can't be written READ_ONLY (EROFS) or NO_PERMISSION, else AVAILABLE */
int native_location_probe(const native_location_ops *ops, const char *dir, const char *id);

/* measureCheapState: 0 with *out, or -1 when an input has no answer (the live bridge, the process
 * list). CORE_SLOW_LOCATION is CORE_SLOW_NOT_NOW then (core_native.c, review NW1): the core keeps
 * its memo, its size and its 10-minute timer, never a guess */
int native_location_measure(const native_location_env *e, native_location *out);
/* The picker displays a live label or drive letter, but only the label is persisted. */
int native_location_picker_name(const native_location_env *e, const char *dir, char label[NL_DRIVE_MAX], char *out, size_t n);
void native_location_remember_label(const char *support, const char *dir, const char *label);

/* CORE_SLOW_LOCATION's worker (the NL ruling's worker check; app.md §5.8: a sleeping USB disk can
 * block a volume query for seconds, a lost network drive for longer). The measure runs on the
 * runner's own serial queue, one at a time, and the caller (a core worker) waits for it at most
 * wait_ns. Callers for the same paths and config generation share a running measure. An older
 * generation for the same target keeps its slot until it finishes, then the caller can retry.
 * 0 with *out when config.ini still matches the measure; else, with no answer:
 *  - NL_ON_UI: called on the UI thread *ui (ui NULL: not checked); nothing is measured;
 *  - NL_BUSY: both workers are occupied, or a measure for other support/default paths is running;
 *  - NL_TIMEOUT: the caller's wait ran out; the measure goes on for any other waiters;
 *  - NL_NO_ANSWER: the measure had none, config.ini's location changed, or the queue or memory
 *    couldn't be had.
 * r lives as long as the process (static, NATIVE_LOCATION_RUNNER_INIT). The paths and ops table
 * are copied; the ops' ctx must outlive the measure. */
enum { NL_NO_ANSWER = -1, NL_ON_UI = -2, NL_BUSY = -3, NL_TIMEOUT = -4 };
/* Each caller waits up to 15 seconds, including when it joins a measure already running. */
#define NL_WAIT_NS (15ull * 1000000000ull)
#define NL_MAX_JOBS 2
/* Bounded I/O jobs own a byte copy of their inputs and publish completed results only
 * within the deadline. Support-folder jobs use dedicated workers. Commits for a support
 * directory serialize config replacement, and the waiter stays through an in-flight replace. */
int native_choice_read_keyed(plat_fn check, void *data, size_t size, uint64_t deadline, int kind, const char *path);
int native_support_read_bounded(plat_fn check, void *data, size_t size, uint64_t deadline, const char *path);
typedef struct native_location_io native_location_io;
int native_location_commit_wait(void (*commit)(native_location_io *, void *), void *data, size_t size,
                                uint64_t deadline, const char *support, int local, int *mutating, int *replaced);
int native_location_begin_mutation(native_location_io *io, int replace);
int native_location_io_expired(const native_location_io *io);
void native_location_end_replace(native_location_io *io, int replaced);

struct nl_job;
typedef struct {
    plat_mutex lock;
    plat_cond done;
    plat_queue *queues[NL_MAX_JOBS];
    struct nl_job *jobs[NL_MAX_JOBS];
    uint64_t generation;
    int config_kind;
    char config_dir[PATHS_LOCATION_MAX + 1], config_id[37];
} native_location_runner;
#define NATIVE_LOCATION_RUNNER_INIT {.lock = PLAT_MUTEX_INIT}
int native_location_measure_bounded(native_location_runner *r, const native_location_env *e, const plat_thread_id *ui, uint64_t wait_ns,
                                    native_location *out);

/* CORE_SLOW_LOCATION's answer (core_native.c's nw_slow_fact; review NW1, NW4): -1 while the gate
 * (NATIVE_LOCATION_LIVE) is off, on the UI thread (NL_ON_UI) or with no paths; else the bounded
 * measure (above), written by write(ctx, l) when it answered (its answer is the result: 0, else
 * -1); no answer this time (NL_TIMEOUT, NL_BUSY, NL_NO_ANSWER, out of memory) is CORE_SLOW_NOT_NOW:
 * the core keeps its memo, its size and its timer, as no Windows host keeps a snapshot. Each NL_*
 * is a log(ctx, line). NATIVE_LOCATION_TEST_HOOKS: native_location_test_off exercises the disabled
 * path; native_location_test_no_memory fails the snapshot's allocation. */
int32_t native_location_slow_fact(native_location_runner *r, const native_location_env *e, const plat_thread_id *ui, uint64_t wait_ns,
                                  int (*write)(void *ctx, const native_location *l), void (*log)(void *ctx, const char *line), void *ctx);
#ifdef NATIVE_LOCATION_TEST_HOOKS
extern int native_location_test_off, native_location_test_no_memory;
extern void (*native_location_test_config_read)(const char *support);
int native_location_test_waiters(native_location_runner *r);
int native_location_test_commit_pending(const char *support);
#endif
/* The release gate shared by CORE_SLOW_LOCATION and the native steps. */
int native_location_live(void);

/* driveName(dir) on Windows (review NL1, NL4, NL5; HF5): with the volume there, its label, else
 * its drive letter ("E:"). When the volume isn't there (unplugged), the label remembered for dir
 * in the support folder's location-drive file (written here, with remember, whenever the chosen
 * folder's volume answered and its label changed), else "": never a drive letter (letters get
 * reassigned), a "Volume{...}" component or a folder's name; the texts' nameless form for "". */
void native_location_drive_name(const native_location_env *e, const char *dir, int remember, char *out, size_t n);

/* Display and report consumers use the matching snapshot and the saved label. */
typedef struct {
    char drive[NL_DRIVE_MAX];
    uint64_t available, total;
    int has_disk;
} native_location_details;
void native_location_describe(const native_location_env *e, const native_location *l,
                              const char *dir, const char *id, native_location_details *out);

/* bridgeSaysLocation(data:): the bridge's bytes hold "rbstems bridge: " and an "rbstems-cache: "
 * line (up to its ';', at most 4096 bytes) whose tokens before "pioneer=" include "location=1" */
int native_location_bridge_says(const char *b, size_t n);
/* stemsCachePresent()'s rule on the bytes: they hold "rbstems bridge: " */
int native_location_is_bridge(const char *b, size_t n);
/* bridgeSaysLocation's rule on the line itself (what is between "rbstems-cache: " and its ';',
 * compat_file's cache_line): its tokens before "pioneer=", split at spaces, include "location=1" */
int native_location_line_says(const char *line, size_t n);

#endif
