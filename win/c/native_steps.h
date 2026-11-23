/* native_steps.c: the core DLL's native steps (core.h "The flows' steps"; the Mac's
 * app/Shell/HostSteps.swift), as rules over an env of what only the app's files, the network and
 * Windows answer, so the Mac tests run them with fakes. core_native_steps.c gives the real env and
 * turns the answers into facts.
 *
 *  - The flow's payload (core.h: "flow" steps use the payload.json of the last PAYLOAD "latest" or
 *    "saved" on the flow thread): kept here under its lock; "check" and "fetch" never change it.
 *  - The flow's bridge: support\payload\<payload_version>\<windows.bridge.file>, the file
 *    DOWNLOAD "bridge" writes, verified by its windows.bridge.sha256 (hashed uncached, sha_file).
 *  - CACHE_PREPARE hands that file to the native admin (set_bridge) only after it verifies; the
 *    admin stages it only when the app's verified installed-release record names exactly it (NA2:
 *    else "update the app first" before any UAC prompt). */
#ifndef RBSP_WIN_NATIVE_STEPS_H
#define RBSP_WIN_NATIVE_STEPS_H

#include "compat.h"
#include "payload.h"
#include "cache_owner.h"

#include <stdint.h>

enum { NS_PATH_MAX = 1024 };

/* R1's folder (CORE_SLOW_R1's "" key, review CW1): the current rekordbox version folder under
 * rb_parent only when compat_find_rekordbox verified one (its cur, COMPAT_RB_OK, the watcher's
 * rule), into folder (n bytes). 1 with it; 0 when there is no verified current folder (rekordbox
 * gone, unverified, no answer): no answer, never a startup paths.rekordbox. */
int native_r1_folder(const compat_os *os, const char *rb_parent, char *folder, size_t n);
/* the same, with that folder's folder_version ("7.2.19", the watcher's rekordbox_version: R1's
 * dismissed line names it, N10) into version (vn bytes; NULL: not wanted) */
int native_r1_current(const compat_os *os, const char *rb_parent, char *folder, size_t n, char *version, size_t vn);

/* update.c's update_check outcomes, as fetch answers them */
/* (UPDATE_CHECK_OLDER is unverified, as the Mac maps .older) */
enum { NS_FETCH_OK = 0, NS_FETCH_NO_NEWS = 1, NS_FETCH_OFFLINE = 2, NS_FETCH_UNVERIFIED = 3 };

typedef struct {
    /* the latest payload downloaded, verified and saved (update_check): NS_FETCH_* with *out on OK */
    int (*fetch)(void *ctx, payload_manifest *out);
    /* the saved payload.json checked again (payload_saved under its floor): 1 with *out (with its
     * windows block), or 0 */
    int (*saved)(void *ctx, payload_manifest *out);
    /* manifestSeen / noteShownVersion (update_seen): m verified, or NULL for the record's own rule */
    void (*seen)(void *ctx, const payload_manifest *m);
    /* seconds since the saved payload.json was written (manifestAge); negative when unknown or in
     * the future (no age: "check" is due) */
    int64_t (*age)(void *ctx);
    /* a file's sha256, uncached (sha_file): 0, or nonzero */
    int (*sha)(void *ctx, const char *path, char out[65]);
    /* Pioneer's ONNX Runtime's sha256 for the log (the live DLL, or ort\'s copy with our bridge in;
     * "" unreadable) */
    void (*ort_sha)(void *ctx, char out[65]);
    /* the native admin's flow bridge (nw_admin_set_bridge): 0, or an errno. NULL, NULL clears it:
     * "latest" and "saved" do when the flow payload changes, and CACHE_PREPARE before it checks */
    int (*set_bridge)(void *ctx, const char *path, const char *name);
    void (*log)(void *ctx, const char *line);
    /* DOWNLOAD: the release asset `name` of m's tag with this sha256 (at most max bytes) into
     * dir (update_fetch_asset): NS_DL_*; the progress is the env's own */
    int (*download)(void *ctx, const payload_manifest *m, const char *dir, const char *name, const char *sha, int64_t max);
    /* REKORDBOX: rekordbox's processes (compat_processes) and the visible windows they own,
     * counted by native_rb_windows (another session's process counts as one): 0 with *running,
     * *updating, *windows; -1 when they couldn't be listed */
    int (*processes)(void *ctx, int *running, int *updating, int *windows);
    /* SPACE: a file's size (-1: none), the free bytes on a path's volume for this account (-1:
     * unknown), whether two paths are on one volume */
    int64_t (*size_of)(void *ctx, const char *path);
    int64_t (*free_bytes)(void *ctx, const char *path);
    int (*same_volume)(void *ctx, const char *a, const char *b);
    void *ctx;
    const char *support;   /* %LOCALAPPDATA%\rbstemsplus */
    const char *own;       /* this app's version */
    const char *model;     /* rekordbox's model file (paths.model), FACT_STEP_PATH of PAYLOAD */
    const char *rekordbox; /* the current rekordbox version folder (paths.rekordbox) */
    const char *root;      /* our root, <Program Files>\RB Stems Plus (native_core_programs) */
    /* blockedByVersionList (BLOCKED's facts: a version not listed yet), for "check"'s manifestDue,
     * asked only when it isn't due otherwise; NULL: not blocked */
    int (*blocked)(void *ctx);
    /* PRUNE_PAYLOADS: `name` in d and its tree deleted (plat_remove_tree_at: never follows a link
     * or reparse point): 0, ENOENT, or an errno. An env call: plat_remove_tree_at is plat_win.h's */
    int (*remove_tree_at)(void *ctx, plat_dir d, const char *name);
    /* REMOVE_LOCATION's last delete: `name` in d only when it is the folder `expect` names and
     * empty (plat_remove_empty_dir_at, review MT1): 0, or an errno */
    int (*remove_empty_dir_at)(void *ctx, plat_dir d, const char *name, plat_id expect);
} native_steps_env;

/* PAYLOAD's facts (core.h; HostSteps.swift's payloadFacts) */
typedef struct {
    int fetch;                       /* FACT_PAYLOAD_FETCH */
    int found;                       /* FACT_PAYLOAD_FOUND */
    int has_age;
    int64_t age;                     /* FACT_PAYLOAD_AGE */
    const char *step_path;           /* FACT_STEP_PATH: the model file */
    int has_flag, flag;              /* FACT_STEP_FLAG ("check": manifestDue) */
    /* when found */
    char version[PAYLOAD_VERSION_MAX], signed_by[32], app_version[PAYLOAD_VERSION_MAX];
    payload_sha bridge_sha, model_sha;
    int bridge_here;                 /* FACT_PAYLOAD_BRIDGE_HERE */
} native_payload_facts;

typedef struct {
    plat_mutex mu;
    int has;                         /* a flow payload */
    payload_manifest m;
} native_flow;

void native_flow_init(native_flow *f);
void native_flow_free(native_flow *f);

/* support\payload\<payload_version>\<windows.bridge.file> into out: 0, or -1 (no windows block,
 * too long) */
int native_bridge_path(const char *support, const payload_manifest *m, char *out, size_t n);
/* manifestDue (Update.swift): fetch at launch when none is saved, it is a day old, it names an
 * app older than this one, or blocked (blockedByVersionList, the caller's; NULL: not blocked) says
 * so; blocked is asked only when the other gates say "not due", as the Mac's (it reads the
 * registry, the payloads and Pioneer's ORT) */
int native_manifest_due(int64_t age, const char *saved_app, const char *own, int (*blocked)(void *ctx), void *ctx);

/* DOWNLOAD's codes (core.h): ok, offline, the disk is full, damaged */
enum { NS_DL_OK = 0, NS_DL_OFFLINE = 1, NS_DL_NO_SPACE = 2, NS_DL_DAMAGED = 3 };
/* the payload's folder, support\payload\<payload_version>: 0, or -1 */
int native_payload_dir(const char *support, const payload_manifest *m, char *out, size_t n);
/* the flow payload's model, support\payload\<payload_version>\<model.file>: 0, or -1 */
int native_model_path(const char *support, const payload_manifest *m, char *out, size_t n);

/* PAYLOAD with arg "latest", "saved", "check" or "fetch": 0 with *out, or nonzero (another arg) */
int native_step_payload(const native_steps_env *e, native_flow *f, const char *arg, native_payload_facts *out);
/* CACHE_PREPARE: the flow payload's bridge verified and handed to the admin; Pioneer's ORT's
 * sha256 into ort_out (FACT_STEP_TEXT, "unreadable" when it can't be read). 0; 1 when there is no
 * flow payload or its bridge isn't there whole (the bridge changed after its check) */
int native_step_cache_prepare(const native_steps_env *e, native_flow *f, char ort_out[65 + 16]);
/* DOWNLOAD "model" or "bridge" (the flow payload's, into its folder): NS_DL_*; 1 too with no flow
 * payload or another arg (as the Mac's) */
int native_step_download(const native_steps_env *e, native_flow *f, const char *arg);
/* REKORDBOX's window count (env processes' *windows): EnumWindows sees only this session's
 * desktop, so a rekordbox.exe in another session, or one whose session can't be read
 * (n_other_or_unknown), counts as a window: open, never closing. windows_here: the visible windows
 * of this session's rekordbox.exe processes (-1: they couldn't be listed). The count, or -1 */
int native_rb_windows(int n_other_or_unknown, int windows_here);
/* REKORDBOX: *open (FACT_REKORDBOX_OPEN: a rekordbox.exe runs), *closing (FACT_REKORDBOX_CLOSING:
 * it runs with no window left, rekordbox 7.2.19's linger), *updating (FACT_PIONEER_UPDATING). 0, or
 * 1 when they couldn't be checked (the core stops the action) */
int native_step_rekordbox(const native_steps_env *e, int *open, int *closing, int *updating);
/* SPACE (spaceShort's measure, Windows' files): with_model, the flow payload's model is copied
 * (its download left, its copy, rekordbox's model file kept: the support folder's volume); with_cache,
 * the Stems Cache install (Pioneer's onnxruntime.dll saved once per version under the root's
 * pioneer\ unless that set is complete, and ort\'s copy of it: the root's volume, e->root; the
 * bridge staged beside it: the version folder's, e->rekordbox, the current one). Each file is
 * charged to the volume it goes to; the needs on one volume (same_volume) are summed, in the
 * order support, root, version folder, and checked in that order; a volume with unknown free
 * bytes, or need 0, is skipped. *need: the first short volume's need + 200 MiB, else 0. 0, or 1
 * (with_model and no flow payload). */
int native_step_space(const native_steps_env *e, native_flow *f, int with_model, int with_cache, int64_t *need);

/* The support folder's small files (State.swift: installed, busy, request, welcomed), which the
 * watcher reads too (watch_facts.c). */
/* support\name written atomically (a temp name, flushed, renamed over it), support made first: 0,
 * or an errno */
int native_support_write(const char *support, const char *name, const char *text, size_t n);
/* the same in the open folder d (the caller's handle, so no path is looked up again) */
int native_support_write_at(plat_dir d, const char *name, const char *text, size_t n);
/* support\name removed (a file, never a folder; none is done): 0, or an errno */
int native_support_remove(const char *support, const char *name);
/* whether support\name exists (fileExists: a link followed) */
int native_support_exists(const char *support, const char *name);
/* REQUEST (reinstallRequestAge): whole seconds since support\name was last written, 0 when that is
 * in the future; -1 none */
int64_t native_support_age(const char *support, const char *name, int64_t now);

/* PRUNE_PAYLOADS: when the flow payload's model is here, the folders of other payload versions
 * under support\payload deleted (prunePayloads; the files there, payload.json and its .sig, and
 * links stay (Windows; the Mac's prunePayloads unlinks a link there)). Listed and deleted through
 * one handle on support\payload, opened without following links (support's too: a link at support
 * or at payload, nothing deleted, one line logged); the model checked through it. Each folder that
 * can't be deleted is tried and logged once per run. 0, also with no flow payload (as the Mac's) */
int native_step_prune_payloads(const native_steps_env *e, native_flow *f);

/* LAUNCH_AGENT_UNLOAD (removeWatcher; Watcher.swift's unload of its LaunchAgent): nothing starts
 * the watcher any more, and none runs. What only Windows does, in ops (core_native_steps.c; the
 * tests' fakes). */
typedef struct {
    /* the Run value and its StartupApproved value removed (lifecycle.h's life_run_remove): 0, or an errno */
    int (*run_remove)(void *ctx);
    /* update_os's stop_watchers: the watchers running from dir stopped by their stop event, then
     * their process handle (by full path): 1 one was running, 0 none, -1 the list couldn't be read */
    int (*stop_watchers)(void *ctx, const char *dir);
    /* the reminders' state afterwards, as WATCHER_STATE measures it (life_run_read's `off`): 0 or
     * 1; -1 when it can't be read */
    int (*reminders_off)(void *ctx);
    void (*log)(void *ctx, const char *line);   /* may be NULL */
    void *ctx;
} native_unload_ops;
/* The Run value first (a logon meanwhile starts nothing), then every version folder of apps (each
 * folder directly in it: the watcher runs from <apps>\<version>\) has its watchers stopped. 0 when
 * the Run value is gone and every folder could be checked, else 1 (logged). *off: the reminders'
 * state afterwards, -1 unknown (FACT_STEP_FLAG then isn't set). apps NULL or "": no folder (the
 * app isn't in its install), only the Run value. */
int native_watcher_unload(const native_unload_ops *ops, const char *apps, int *off);

/* LAUNCH_AGENT_LOAD, WATCHER_CHECK and WATCHER_STATE (installWatcher, checkWatcher,
 * updateWatcherState; app.md §5.7: "Repair = the Run value names this install's stub with
 * --watcher"). What only Windows does, in ops (core_native_steps.c; the tests' fakes). */
typedef struct {
    /* life_run_read of the stub: 0 with *present (a Run value of our name), *ours (it starts this
     * install's stub with --watcher) and *off (StartupApproved turned it off); an errno */
    int (*run_read)(void *ctx, int *present, int *ours, int *off);
    /* life_run_write of the stub (StartupApproved, the user's own switch, is never touched): 0 or
     * an errno */
    int (*run_write)(void *ctx);
    /* update_os's watchers_in of this app's own folder: 1 one runs (this session), 0 none, -1 the
     * list couldn't be read */
    int (*running)(void *ctx);
    /* the stub started with --watcher (update_os's spawn): 0 or an errno */
    int (*start)(void *ctx);
    void (*log)(void *ctx, const char *line);   /* may be NULL */
    void *ctx;
} native_watcher_ops;
/* installWatcher: the Run value written unless it already names this install's stub, then the
 * watcher started when none of this app's runs and Startup apps doesn't say off. Every
 * outcome is logged ("watcher: installed", "watcher: COULD NOT BE INSTALLED (why)"); 0 always, as
 * the Mac's installWatcher never fails the flow. *off: the reminders' state afterwards (WATCHER_STATE's
 * rule), -1 unknown. (The caller checks the app is in its install first: else nothing to name.) */
int native_watcher_load(const native_watcher_ops *ops, int *off);
/* checkWatcher at launch: a Run value of our name that starts anything else (an older layout, a
 * moved stub) is pointed at this install's stub; none is left alone (only installWatcher adds
 * one), as is ours. 0 always (what was done is logged); *off as native_watcher_load's. */
int native_watcher_check(const native_watcher_ops *ops, int *off);
/* updateWatcherState's measurement: 1 the Run value is ours' name and StartupApproved turned it off,
 * 0 not, -1 it can't be read */
int native_watcher_off(const native_watcher_ops *ops);

struct native_location;

/* LOCATION_NOW (review NS11, NS12, N3R3): with no measure or the gate off, a chosen folder keeps
 * its config.ini path and remembered label, with no gate or size. NULL dir means no chosen folder;
 * NULL snap means no answer. The output strings are borrowed from the inputs or are literals. */
typedef struct {
    const char *dir, *gate_id, *drive, *identity;
} native_location_target;
int native_location_now_facts(int live, const struct native_location *snap, const char *dir, const char *id, const char *hint,
                              const char *memo_dir, native_location_target *out, int64_t *size_kind);

/* SETTINGS (settingsFacts, HostSteps.swift): the Settings sheet's facts from config.ini's read
 * (host_facts.c's: readConfigState) and what the caller measured. The size is always the core's
 * (kind 4: Windows keeps no snapshot of its own, the core's memo is the size), none while config.ini
 * is unreadable or set wrongly. Clear: this PC's folder (1 when support\Cache is a folder, else 2)
 * for This PC and a location set wrongly; for a chosen folder, from the measure of it (snap:
 * settingsStems' snapshot, plan N3): the bridge too old to use it, this PC's folder; the folder
 * there with our marker (LOC_CHOSEN, PROBE_AVAILABLE), 3 with a gate for config.ini's dir and id
 * (review N3R3); anything else, or no measure, 0. A chosen folder's label path: the folders between its
 * volume's root (mount, else the root in dir, with a drive-letter fallback) and its parent,
 * '/'-separated. */
typedef struct {
    int unreadable;          /* config.ini can't be read */
    int location;            /* 0 This PC, 1 chosen (dir), 2 set wrongly */
    const char *dir;         /* the chosen folder */
    const char *id;          /* its marker id */
    double max_gb, max_days; /* configNumber's inputs */
    int can_choose;          /* !stemsCachePresent() || bridgeSaysLocation(), and the choice offered on Windows */
    int this_pc_folder;      /* support\Cache is a folder */
    const char *mount;       /* the chosen folder's volume root, "" or NULL unknown */
    const char *drive;       /* driveName of the chosen folder (native_location_drive_name), "" none */
    const struct native_location *snap;   /* NULL when gated off or without a measure */
} native_settings_in;
typedef struct {
    char gb[32], days[32];
    int unreadable, can_choose;
    int64_t size_kind, clear, location;
    char label_path[NS_PATH_MAX];
    char drive[256];
    native_location_target clear_target;  /* borrowed strings, set when clear is 3 */
} native_settings;
void native_settings_facts(const native_settings_in *in, native_settings *out);

/* STOP_KEEPING (stopKeeping): name by keptNameOK (1-64 of A-Z a-z 0-9 - _); kept\<name>.kept and
 * .manifest removed where they are regular files. 0: the .kept is gone; nonzero: it is still there
 * (or the name isn't one) */
int native_stop_keeping(const char *kept, const char *name);

/* Uninstall removes recognized app artifacts from this account's support directory. Cache and
 * unknown entries are left, as are saved originals in keep mode. Support itself is removed only
 * if empty and still the directory opened for cleanup. Returns 1 if files could not be removed. */
int native_remove_own_folders(const native_steps_env *e, const char *expected_support, int keep_originals);
#ifdef NATIVE_STEPS_TEST_HOOKS
/* test builds only: asked before REMOVE_OWN_FOLDERS' each plat_dir_open (op OPEN, the path) and
 * plat_stat_at (op STAT, the name; also REMOVE_LOCATION's stats of the marker, its companion and
 * their case variants); a nonzero errno is that call's answer. LIST_ID (the name):
 * nonzero gives that name a listing id other than its stat id. LIST (name ""): nonzero makes the
 * walk's listing one that couldn't be read whole */
enum { NS_FAULT_OPEN = 1, NS_FAULT_STAT, NS_FAULT_LIST_ID, NS_FAULT_LIST, NS_FAULT_OPEN_CREATED, NS_FAULT_UNLINK_MARKER,
       NS_FAULT_LOCATION_CONFIG_READ, NS_FAULT_LOCATION_CONFIG_REPLACE };
extern int native_steps_test_hold_cache_tree;
extern int (*native_steps_test_fault)(int op, const char *name);
#endif

/* Local cache ownership is recorded when support/Cache is first created. Clear and whole
 * removal require the recorded volume GUID and directory ID from confirmation and the marker
 * from the same opened directory. Clear keeps the marker. Whole removal deletes it last,
 * then removes the same empty directory. A link at Cache sets *link. */
#define NS_CACHE_IDENTITY_MAX CACHE_OWNER_IDENTITY_MAX
#define NS_PC_MARKER CACHE_OWNER_MARKER
int native_pc_cache_identity(const native_steps_env *e, const char *cache_default, char identity[NS_CACHE_IDENTITY_MAX]);
/* No identity is returned unless the read-only ownership check completed in time. */
int native_pc_cache_identity_bounded(const char *support, const char *cache_default, uint64_t deadline,
                                     char identity[NS_CACHE_IDENTITY_MAX]);
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
int native_pc_cache_probe(const char *support, const char *cache_default);
#endif
void native_pc_cache_create(const native_steps_env *e, const char *cache_default);
int native_clear_cache(const native_steps_env *e, const char *cache_default, const char *identity, int whole, int *link);

/* REMOVE_LOCATION (removeInLocation): a chosen location's stems, only when its folder's marker is
 * the location's (id) and through handles opened one component at a time from the drive's root,
 * never following a link and never onto another drive. whole 0 (Clear Saved Stems): only the
 * model folders (64 lowercase hex names) and "._" files in it; whole 1 (the complete uninstall):
 * everything in it, then the marker last, then the folder itself, only once it is empty.
 * Any other entry that is the marker's or its companion's file (by id,
 * under any name: a case or Unicode case-fold alias the drive resolves, or a hard link) is kept
 * with them and goes after the marker; a different file goes as any other (review MS6, MQ1).
 * dir: "X:\…\RB Stems Plus Cache.noindex" or the volume-GUID form cache_dir takes
 * ("\\?\Volume{GUID}\…", plan N3; the Mac's tests: "/…"), plain components, at most
 * PATHS_LOCATION_MAX bytes. 0 all went; else core.h's code: 1 not connected, 2 the drive can't be opened, 3 it
 * can't be read, 4 an entry couldn't be opened (name, err: its name, the OS error), 5 an entry is
 * on another drive (name), 6 the marker isn't the one expected, 7 some files couldn't be deleted */
int native_remove_location(const native_steps_env *e, const char *dir, const char *id, int whole, char name[256], char err[128],
                           const char *identity);
/* the same below the folder top (the drive's root: native_remove_location's; the Mac's tests a
 * scratch folder), rel its relative path */
int native_remove_location_under(const native_steps_env *e, const char *top, const char *rel, const char *id, int whole, char name[256],
                                 char err[128], const char *identity);

/* ---------------------------------------------------------------- the location choice (plan N3)
 *
 * Settings' Change… and Use This PC (LocationActions.swift's changeLocation, commitLocation,
 * useThisMac) and the switch's deletion of this PC's stems: LOCATION_PROBE, LOCATION_DISCOVER,
 * LOCATION_SIZE, LOCATION_WRITABLE, LOCATION_CREATE, LOCATION_WRITE and THIS_MAC_DELETE as rules
 * over plat.h, the Windows-only answers through native_choice_ops (core_native_steps.c's; the Mac
 * tests' fakes). The contract with the bridge (app.md §5.8, bridge.md §1): config.ini's cache_dir
 * is written in the volume-GUID form ("\\?\Volume{GUID}\…\RB Stems Plus Cache.noindex"), since
 * drive letters change between plugs: the probe's real path and mount are in that form, and
 * discover, create, write and the hint keep it. The support folder's files: location-hint (the
 * Mac's: mount=, relative=, own= the ids this account made; relative's separator '\' on Windows)
 * and pending-deletion (this PC's stems left after a switch). */
typedef struct {
    void *ctx;
    /* the volume holding the folder chosen (vol_facts_of, vol_refusal): 0 with *refusal (volumes.h's
     * VOL_OK, VOL_REFUSE_UNREADABLE, _NETWORK, _INTERNAL, _FILE_SYSTEM, _READ_ONLY); with VOL_OK
     * also the volume's root as cache_dir names it ("\\?\Volume{…}\") and the folder's final path
     * in that form (vol_guid_path). An errno: unreadable */
    int (*volume)(void *ctx, const char *folder, int *refusal, char *root, size_t rn, char *real, size_t n);
    /* driveName of a folder (native_location_drive_name; remember: the marker is known ours) */
    void (*drive_name)(void *ctx, const char *dir, int remember, char *out, size_t n);
    /* a new marker id, 8-4-4-4-12 upper-case hex from the system's random source: 0, or nonzero */
    int (*new_id)(void *ctx, char out[40]);
    /* .noindex's Windows twin, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED on our new folder (best effort;
     * NULL: none) */
    void (*not_indexed)(void *ctx, const char *dir);
    const char *support;  /* resolved by directory identity when checking a picker candidate */
} native_choice_ops;

/* Read-only picker checks share an absolute monotonic deadline. The job receives an
 * independent, pointer-free copy of data and publishes it only before expiry. */
/* Each check's data supplies its kind and path; output fields are outside the key. */
#define native_choice_read(check, data, size, deadline) \
    native_choice_read_keyed((check), (data), (size), (deadline), (data)->kind, (data)->path)

/* LOCATION_PROBE (realPath, volumeFacts, locationRefusalFor) of the folder as chosen: code 0 may
 * be used, else core.h's refusal: 9 unreadable, then the volume's (2 network, 1 internal, 7 file
 * system, 3 read-only), then 6 a link (a component from its root down that is a link, junction,
 * mount point or other reparse point, or a real path not below its volume's root), 5 inside our
 * folder (a component above it named so, or a marker in a folder above it up to the volume's
 * root, or support cannot be excluded), 8 our folder's path there refused by cache_dir's rule.
 * real and mount: the folder's real
 * path and its volume's root, in cache_dir's form; drive: driveName of the folder chosen ("" none:
 * the core refuses a nameless drive). Nonzero: no folder given. */
typedef struct {
    int64_t code;
    int support_conflict;
    char real[NS_PATH_MAX], mount[NS_PATH_MAX], drive[256];
} native_choice_probe;
int native_choice_probe_folder(const native_choice_ops *o, const char *folder, native_choice_probe *out);

/* LOCATION_DISCOVER (discover with config.ini's location and the hint): code 0 none, 1 a folder of
 * our name without a valid marker or that could be in support (found: it), 2 our folder (found,
 * id). current, current_id:
 * config.ini's chosen folder ("" This PC). Candidates in order: real itself when its last
 * component is our folder's name, else our folder in it; then mount + the hint's relative (else
 * config.ini's folder below its root) when that is a real folder (no link from mount down) whose
 * path holds our folder's name. Nonzero: real or mount missing. */
typedef struct {
    int64_t code;
    int support_conflict;
    char found[NS_PATH_MAX], id[40], current[NS_PATH_MAX], current_id[40], identity[NS_CACHE_IDENTITY_MAX];
} native_choice_found;
int native_choice_discover(const char *support, const char *real, const char *mount, native_choice_found *out);

/* LOCATION_SIZE: entriesSize of dir (vol_entries_size), 0 for no folder, -1 for unknown. */
int64_t native_choice_size(const char *dir);

/* LOCATION_WRITABLE (locationWritable): dir and each model folder in it (64 lower-case hex) may be
 * written by this process: 1, else 0. Checked on their directory handles. */
int native_choice_writable(const char *dir);

/* LOCATION_CREATE (createLocationFolder, then .noindex's attribute): our folder in real (real
 * itself when it is named so), then its marker (a new id), nothing else. 0 with dir and id; 1 a
 * folder of that name appeared meanwhile; 2 the folder couldn't be made (err: the OS error); 3 the
 * marker or identity could not be obtained (err); 4 the destination volume is no longer eligible;
 * NS_CHOICE_CREATE_LEFT_FOLDER if the failed Windows create could not dispose of its folder. */
enum { NS_CHOICE_CREATE_LEFT_FOLDER = 5, NS_CHOICE_TIMED_OUT = 6, NS_CHOICE_SAVED_UNCHECKED = 7 };
int native_choice_create(const native_choice_ops *o, const native_steps_env *e, const char *real, char dir[NS_PATH_MAX], char id[40],
                         char err[128], char identity[NS_CACHE_IDENTITY_MAX]);

/* LOCATION_WRITE removes the location keys for This PC. For an external cache it requires
 * eligibility, the confirmed GUID and folder identity, the marker and write access. Returns
 * 0 on successful write and readback, 1 on config failure, or 2 if the destination check fails.
 * For a failed config write, rollback is limited to a cache created by this flow. */
int native_choice_write(const native_choice_ops *o, const native_steps_env *e, const char *dir, const char *id, const char *mount,
                        int created, const char *identity);

/* Confirmed Windows steps. Inputs are copied and the callbacks' context must outlive
 * the job. Treat left_folder as a possible leftover when the waiter times out. */
typedef struct {
    int create, created, has_label;
    char support[NS_PATH_MAX], path[NS_PATH_MAX], mount[NS_PATH_MAX], cache_default[NS_PATH_MAX];
    char label[256], id[40], identity[NS_CACHE_IDENTITY_MAX];
    char dir[NS_PATH_MAX], err[128];
    int left_folder;
} native_choice_commit;
int native_choice_commit_run(native_choice_commit *commit, const native_choice_ops *ops,
                             int (*remove_empty)(void *, plat_dir, const char *, plat_id), uint64_t deadline);

/* Delete the model entries in the confirmed local cache. On failure, record that same
 * identity so a later startup can retry without adopting a replacement folder. */
int native_this_pc_remove_stems(const native_steps_env *e, const char *cache_default, const char *identity);
#define NS_PENDING_DELETION "pending-deletion"
#define NS_LOCATION_HINT "location-hint"
/* LocationStatus.code of a measure's kind and probe (core/pure.h's LOC_*, PROBE_*) */
const char *native_location_code(int kind, int probe);

/* LEFTOVER (leftoverAction, State.swift): the action an earlier run left half-done in support\busy,
 * into id and label ("" none). Its writer is live when start(ctx, pid) (its start time in seconds
 * since 1970: -1 no such process, -2 it couldn't be asked) matches the marker's (busy_marker_live),
 * or is -2 (never offer to finish what may still run). A live marker of self_pid is this process's
 * own: none at once; one of a pid this process reuses after a crash (another start) is offered. A
 * live writer is asked again after each nap(ctx), up to 20 naps (one that is quitting), then none.
 * A marker without an action is none. */
void native_leftover(const char *support, int32_t self_pid, int64_t (*start)(void *ctx, int32_t pid), void (*nap)(void *ctx), void *ctx,
                     char *id, size_t idn, char *label, size_t labeln);

/* STATE_LINE (Controller.state()): "rekordbox V, engine E, model <the sha's first 12>, bridge
 * yes|no, chosen (model: true|false, cache: true|false)" into out */
void native_state_line(char *out, size_t n, const char *rekordbox, const char *engine, const char *model_sha, int bridge, int chosen_model,
                       int chosen_cache);

#endif
