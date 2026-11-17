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

#include "payload.h"
#include "plat.h"

#include <stdint.h>

enum { NS_PATH_MAX = 1024 };

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

/* STOP_KEEPING (stopKeeping): name by keptNameOK (1-64 of A-Z a-z 0-9 - _); kept\<name>.kept and
 * .manifest removed where they are regular files. 0: the .kept is gone; nonzero: it is still there
 * (or the name isn't one) */
int native_stop_keeping(const char *kept, const char *name);

/* REMOVE_OWN_FOLDERS (removeOwnFolders): this account's own folders deleted, the support folder
 * (e->support) and others (the saved stems, the logs); with keep_originals and support\originals
 * a folder, everything in support but it. e->support must be expected_support (this account's
 * LocalAppData + "\rbstemsplus", nw_steps_open's; ASCII case ignored): another one, or none, is
 * refused ("refused to delete …" logged), nothing deleted there, 1. support is opened without
 * following a link: a link, a reparse point or a file there is left (logged), 1; not there, gone;
 * another error, nothing deleted in it, 1. What stays is told by its id (plat_stat_at's): a link or
 * reparse point at support\Cache or support\Logs (left, logged, 1: support stays too), and in keep
 * mode the originals folder (whatever its name's case). Every other entry is deleted with
 * remove_tree_at, a link as itself (what it points to untouched); an entry that can't be looked at
 * or deleted, or a listing that fails, counts as not gone and keeps support. Support itself goes
 * last, only when everything in it went: through its parent opened following links (a junction at
 * LocalAppData; plat_dir_open_follow), and only when the folder there is the one the walk's
 * handle held (its id) and empty (remove_empty_dir_at; review SF2). Each of others through one handle on its parent: it must
 * be a folder (a link or a file there is left, logged). Folders that aren't there count as gone.
 * 0: all went; 1: not all */
int native_remove_own_folders(const native_steps_env *e, const char *expected_support, int keep_originals, const char *const *others,
                              int nothers);
#ifdef NATIVE_STEPS_TEST_HOOKS
/* test builds only: asked before REMOVE_OWN_FOLDERS' each plat_dir_open (op OPEN, the path) and
 * plat_stat_at (op STAT, the name; also REMOVE_LOCATION's stats of the marker, its companion and
 * their case variants); a nonzero errno is that call's answer. LIST_ID (the name):
 * nonzero gives that name a listing id other than its stat id. LIST (name ""): nonzero makes the
 * walk's listing one that couldn't be read whole */
enum { NS_FAULT_OPEN = 1, NS_FAULT_STAT, NS_FAULT_LIST_ID, NS_FAULT_LIST };
extern int (*native_steps_test_fault)(int op, const char *name);
#endif

/* CLEAR_THIS_MAC (safeRemove(defaultCacheDir, whole)): only when cache_default (paths.cache_default)
 * is exactly e->support + "\Cache" (the name's case ignored), else "refused to delete …" is logged
 * and nothing is touched: the name deleted is always "Cache", never the one cache_default gives.
 * Through one handle on support, opened without following a link: support or Cache not there, 0;
 * Cache not a folder: 'refused to delete …' logged, 1 and *link when it is a link or reparse point
 * (left, nor what it points to); else Cache and its tree deleted with remove_tree_at: 0, or 1
 * (logged), as when support or Cache can't be opened or read for a reason other than not being
 * there */
int native_clear_cache(const native_steps_env *e, const char *cache_default, int *link);

/* REMOVE_LOCATION (removeInLocation): a chosen location's stems, only when its folder's marker is
 * the location's (id) and through handles opened one component at a time from the drive's root,
 * never following a link and never onto another drive. whole 0 (Clear Saved Stems): only the
 * model folders (64 lowercase hex names) and "._" files in it; whole 1 (the complete uninstall):
 * everything in it, then the marker last, then the folder itself, only once it is empty, and the
 * "._" companion beside it. Any other entry that is the marker's or its companion's file (by id,
 * under any name: a case or Unicode case-fold alias the drive resolves, or a hard link) is kept
 * with them and goes after the marker; a different file goes as any other (review MS6, MQ1).
 * dir: "X:\…\RB Stems Plus Cache.noindex" (the Mac's tests: "/…"),
 * plain components, at most PATHS_LOCATION_MAX bytes (the \\?\Volume{GUID}\ form is T3c's: refused
 * for now). 0 all went; else core.h's code: 1 not connected, 2 the drive can't be opened, 3 it
 * can't be read, 4 an entry couldn't be opened (name, err: its name, the OS error), 5 an entry is
 * on another drive (name), 6 the marker isn't the one expected, 7 some files couldn't be deleted */
int native_remove_location(const native_steps_env *e, const char *dir, const char *id, int whole, char name[256], char err[128]);
/* the same below the folder top (the drive's root: native_remove_location's; the Mac's tests a
 * scratch folder), rel its relative path */
int native_remove_location_under(const native_steps_env *e, const char *top, const char *rel, const char *id, int whole, char name[256],
                                 char err[128]);

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
