/* The core DLL's native steps (core_native_steps.h): native_steps.c's rules with the real env.
 *
 *  - PAYLOAD: update.c's update_check (WinHTTP, the releases' tag, payload_accept under the floor)
 *    for "latest" and "fetch"; the saved payload\ checked again (payload_saved) for "saved" and
 *    "check"; update_seen for what each verified one means (the version shown, DisplayVersion);
 *    the age from payload\payload.json's last write.
 *  - DOWNLOAD: update.c's update_fetch_asset into the flow payload's folder, resumed from a part;
 *    its progress as core_flow_progress(DOWNLOAD): -1 "Checking" first, then the bytes of the
 *    total when the server says it, else -2 "Downloading".
 *  - REKORDBOX: compat.c's process list (rekordbox.exe in any session, Pioneer's updater or
 *    uninstaller) and the visible top-level windows this session's rekordbox.exe owns
 *    (EnumWindows); one in another session counts as open (native_rb_windows): running with none
 *    is rekordbox 7.2.19's linger, FACT_REKORDBOX_CLOSING.
 *  - SPACE: native_steps.c's measure, sizes and volumes through plat.h; the current version
 *    folder (current_folder), each file charged to the volume it goes to (the root's: e->root).
 *  - BLOCKED: native_facts.c's facts for the current version folder against the verified lists
 *    (the saved payload, else the installed-release record; none: nothing listed); Pioneer's ORT
 *    by sha256 (FACT_ORT_LISTED). The same facts make "check"'s blockedByVersionList.
 *  - CACHE_PREPARE: the flow payload's bridge, support\payload\<version>\<windows.bridge.file>,
 *    hashed uncached (sha_file) against its windows.bridge.sha256, then handed to the native
 *    admin (nw_admin_set_bridge); the admin stages it only when the app's verified
 *    installed-release record names exactly it (NA2). Pioneer's ORT's sha256 for the log from
 *    compat.c's R1 read of the current version folder under <Program Files>\rekordbox, against
 *    our root <Program Files>\RB Stems Plus (Windows' own Program Files as core_new read it:
 *    native_core_programs, the worker's rule; paths.rekordbox when there is no current one).
 *  - The support folder's small files (SET_CHOSEN's installed, BUSY_MARKER's busy, WELCOME's
 *    welcomed, REQUEST's request): native_steps.c's atomic write, remove and age, the texts the
 *    core's own (state_chosen_text, busy_marker_text), as the watcher reads them.
 *  - PRUNE_PAYLOADS: the other payload versions' folders, listed and deleted through one handle on
 *    support\payload, opened without following links (plat_remove_tree_at).
 *  - BRIDGE_SHA and STATE_LINE: the current version folder's onnxruntime.dll and rekordbox's model
 *    hashed through the backend's memo; STATE_LINE's bridge is R1's live DLL being ours.
 *  - CACHE_FOLDERS: the current version folder and the root (s->root) there.
 *  - UPDATE_CHECK: update_in_place (the exe is <apps>\<own>\, `current` names it), then the apps
 *    folder writable (plat_dir_writable).
 *  - OPEN_URL (http and https only) and REVEAL (a folder opened with the file selected): the shell,
 *    COM initialized on the calling thread for the call.
 *  - UPDATE_SWAP: update.c's update_step_swap (stage, the new app's --swap, then the shell's quit
 *    through update_shell_quit; none registered: nothing started, 3), the download's progress as
 *    DOWNLOAD's. UPDATE_RESULT: update_result_take (the start's failed key, said once). */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include "plat.h"
#include "plat_win.h"

#ifndef RBSP_CORE_PLAT_SHIM_H
#  define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h has the shim's names */
#endif
#include "internal.h"   /* the core's state_chosen, state_chosen_text, busy_marker_text */

#include "core_native_steps.h"

#include "afterexit.h"
#include "cfg_ini.h"
#include "compat.h"
#include "native_config.h"
#include "native_facts.h"
#include "lifecycle.h"
#include "models.h"
#include "native_steps.h"
#include "win_rules.h"
#include "sha_memo.h"
#include "update.h"
#include "update_shell.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NS_KEYS_MAX = 4 };

struct nw_steps {
    core *c;
    nw_admin *admin;
    char *support, *apps, *own, *model, *rekordbox, *kept, *cache_default;
    sha_memo *shas;
    char root[512];       /* <Program Files>\RB Stems Plus, read once here: BLOCKED's, SPACE's and the ORT's */
    char rb_parent[512];  /* <Program Files>\rekordbox, where the version folders are */
    /* LocalAppData (plat_home) + \rbstemsplus: the only support folder REMOVE_OWN_FOLDERS
     * deletes, whatever paths.support says (review S53); "" when LocalAppData can't be read */
    char expected_support[NS_PATH_MAX];
    sig_pubkey keys[NS_KEYS_MAX];
    size_t nkeys;
    update_env ue;
    native_flow flow;
    /* the last current rekordbox version folder a worker verified (current_folder), and its
     * rekordbox.exe's stat then: the prepare helper's --rekordbox, read at the spawn with one stat
     * (nw_steps_rekordbox_known, review PF1); calloc's zero is SRWLOCK_INIT */
    SRWLOCK rb_lock;
    char rb_known[COMPAT_PATH_MAX];
    plat_stat rb_exe;
};

static char *dup_or_null(const char *s) { return s && s[0] ? _strdup(s) : NULL; }

static void ue_log(void *ctx, const char *line) {
    nw_steps *s = ctx;
    char b[1100];
    snprintf(b, sizeof b, "  %s", line);
    native_core_log(s->c, b);
}

/* this account's support folder, LocalAppData + \rbstemsplus, from the shell's known folder
 * (plat_home), never from the environment or the shell's paths; a CORE_TEST_HOOKS build takes
 * RBSP_TEST_LOCALAPPDATA first (the VM's fake trees, as the watcher's). "" when there is none */
static void expected_support(char *out, size_t n) {
    char home[NS_PATH_MAX];
    int r = -1;
    out[0] = 0;
#ifdef CORE_TEST_HOOKS
    const char *t = getenv("RBSP_TEST_LOCALAPPDATA");
    if (t && *t) r = snprintf(home, sizeof home, "%s", t) < (int)sizeof home ? 0 : -1;
    else
#endif
        r = plat_home(home, sizeof home);
    if (r) return;
    int k = snprintf(out, n, "%s\\rbstemsplus", home);
    if (k < 0 || (size_t)k >= n) out[0] = 0;
}

nw_steps *nw_steps_open(core *c, nw_admin *admin, sha_memo *shas) {
    nw_steps *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->c = c;
    s->admin = admin;
    s->shas = shas;
    const char *support = native_core_path(c, "support"), *app = native_core_path(c, "app"), *model = native_core_path(c, "model"),
               *rb = native_core_path(c, "rekordbox"), *own = native_core_version(c);
    s->support = dup_or_null(support);
    s->model = dup_or_null(model);
    s->rekordbox = dup_or_null(rb);
    s->own = dup_or_null(own);
    const char *kept = native_core_path(c, "kept"), *cache = native_core_path(c, "cache_default");
    s->kept = dup_or_null(kept);
    s->cache_default = dup_or_null(cache);
    /* the apps folder: the app's own folder is <apps>\<version> */
    s->apps = dup_or_null(app);
    if (s->apps) {
        char *cut = strrchr(s->apps, '\\');
        if (cut && cut != s->apps) *cut = 0;
    }
    const char *pf = native_core_programs(c);
    if ((support && support[0] && !s->support) || (model && model[0] && !s->model) || (rb && rb[0] && !s->rekordbox) || (own && own[0] && !s->own)
        || (app && app[0] && !s->apps) || (kept && kept[0] && !s->kept) || (cache && cache[0] && !s->cache_default) || !pf
        || wr_program_path(pf, WR_PF_ROOT, s->root, sizeof s->root) < 0 || wr_program_path(pf, WR_PF_REKORDBOX, s->rb_parent, sizeof s->rb_parent) < 0) {
        nw_steps_close(s);
        return NULL;
    }
    expected_support(s->expected_support, sizeof s->expected_support);
    for (int i = 0; i < native_n_key_texts && s->nkeys < NS_KEYS_MAX; i++)
        if (!sig_key_from_text(&native_key_texts[i], &s->keys[s->nkeys])) s->nkeys++;
    s->ue.support = s->support;
    s->ue.apps = s->apps;
    s->ue.own = s->own;
#if defined(__aarch64__)
    s->ue.arch = "arm64";
#else
    s->ue.arch = "x64";
#endif
    s->ue.keys = s->keys;
    s->ue.nkeys = s->nkeys;
    s->ue.net = update_net_windows();
    s->ue.os = update_os_windows();
    s->ue.log = ue_log;
    s->ue.ctx = s;
#ifdef CORE_TEST_HOOKS
    s->ue.base = update_test_base();   /* the TEST build's server (update_shell.h); NULL: UPDATE_BASE */
#endif
    (void)update_self_capture(&s->ue);   /* the running exe's id, taken at start (review UH1): first call wins */
    native_flow_init(&s->flow);
    return s;
}

void nw_steps_close(nw_steps *s) {
    if (!s) return;
    if (s->ue.ctx) native_flow_free(&s->flow);
    free(s->support);
    free(s->apps);
    free(s->own);
    free(s->model);
    free(s->rekordbox);
    free(s->kept);
    free(s->cache_default);
    free(s);
}

/* ---------------------------------------------------------------- the env */

static int e_fetch(void *ctx, payload_manifest *out) {
    nw_steps *s = ctx;
    if (!s->support || !s->own) return NS_FETCH_OFFLINE;
    switch (update_check(&s->ue, out)) {
    case UPDATE_CHECK_OK: return NS_FETCH_OK;
    case UPDATE_CHECK_NO_NEWS: return NS_FETCH_NO_NEWS;
    case UPDATE_CHECK_UNVERIFIED:
    case UPDATE_CHECK_OLDER: return NS_FETCH_UNVERIFIED;   /* the Mac's .older */
    default: return NS_FETCH_OFFLINE;
    }
}

static int e_saved(void *ctx, payload_manifest *out) {
    nw_steps *s = ctx;
    if (!s->support || !s->own) return 0;
    update_store st;
    update_store_init(&s->ue, &st);
    return payload_saved(&st.files, out) == PAYLOAD_OK && out->has_windows;
}

static void e_seen(void *ctx, const payload_manifest *m) {
    nw_steps *s = ctx;
    if (!s->support || !s->own) return;
    char shown[PAYLOAD_VERSION_MAX];
    update_seen(&s->ue, m, shown);
}

static int64_t e_age(void *ctx) {
    nw_steps *s = ctx;
    char p[NS_PATH_MAX];
    plat_stat st;
    if (!s->support || snprintf(p, sizeof p, "%s\\payload\\payload.json", s->support) >= (int)sizeof p || plat_stat_path(p, 0, &st) || st.kind != PLAT_FILE)
        return -1;
    /* a payload.json written in the future is no age at all (unknown: due), never 0 */
    return plat_now_wall_s() - st.mtime;
}

static int e_sha(void *ctx, const char *path, char out[65]) {
    (void)ctx;
    return sha_file(path, out);   /* uncached: it gates what the admin stages */
}

/* the current version folder now (compat_find_rekordbox's rule, as watch_facts.c reads it) under
 * <Program Files>\rekordbox (Windows' own Program Files as core_new read it: native_core_programs,
 * so a rekordbox installed while the app is open is found too, never a hard-coded C:), and its
 * version ("7.2.19.0342": the uninstall entry's, else the folder's; "" none); paths.rekordbox
 * itself, a core_new snapshot, when there is none: an update of rekordbox since the start moves
 * it. 1 when a current one was found */
static int current_folder(nw_steps *s, char folder[COMPAT_PATH_MAX], char version[COMPAT_VERSION_MAX]) {
    snprintf(folder, COMPAT_PATH_MAX, "%s", s->rekordbox ? s->rekordbox : "");
    version[0] = 0;
    compat_rekordbox *rb = calloc(COMPAT_ENTRIES_MAX, sizeof *rb);
    int found = 0;
    if (rb) {
        int cur = -1, state = 0;
        compat_find_rekordbox(compat_os_windows(), s->rb_parent, rb, COMPAT_ENTRIES_MAX, &cur, &state);
        if (cur >= 0) {
            found = 1;
            snprintf(folder, COMPAT_PATH_MAX, "%s", rb[cur].folder);
            snprintf(version, COMPAT_VERSION_MAX, "%s", rb[cur].display_version[0] ? rb[cur].display_version : rb[cur].folder_version);
        }
        free(rb);
    }
    /* the verified folder kept for the helper's spawn, with its rekordbox.exe as it is now (never
     * the paths.rekordbox fallback) */
    char exe[COMPAT_PATH_MAX + 16];
    plat_stat st;
    int keep = found && (size_t)snprintf(exe, sizeof exe, "%s\\rekordbox.exe", folder) < sizeof exe && !plat_stat_path(exe, 0, &st)
               && st.kind == PLAT_FILE;
    AcquireSRWLockExclusive(&s->rb_lock);
    if (keep) {
        snprintf(s->rb_known, sizeof s->rb_known, "%s", folder);
        s->rb_exe = st;
    } else {
        s->rb_known[0] = 0;
    }
    ReleaseSRWLockExclusive(&s->rb_lock);
    return found;
}

int nw_steps_rekordbox_known(nw_steps *s, char *folder, size_t n) {
    if (n) folder[0] = 0;
    if (!s) return 0;
    char f[COMPAT_PATH_MAX];
    plat_stat then;
    AcquireSRWLockShared(&s->rb_lock);
    memcpy(f, s->rb_known, sizeof f);
    then = s->rb_exe;
    ReleaseSRWLockShared(&s->rb_lock);
    if (!f[0]) return 0;
    /* one stat, no registry, no hash, no signature check: still the same rekordbox.exe */
    char exe[COMPAT_PATH_MAX + 16];
    plat_stat now;
    if ((size_t)snprintf(exe, sizeof exe, "%s\\rekordbox.exe", f) >= sizeof exe || plat_stat_path(exe, 0, &now) || now.kind != PLAT_FILE
        || !plat_id_same(now.id, then.id) || now.size != then.size || now.mtime != then.mtime)
        return 0;
    if ((size_t)snprintf(folder, n, "%s", f) >= n) {
        if (n) folder[0] = 0;
        return 0;
    }
    return 1;
}

static void e_ort_sha(void *ctx, char out[65]) {
    nw_steps *s = ctx;
    out[0] = 0;
    char folder[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX];
    if (!current_folder(s, folder, version) && !s->rekordbox) return;
    compat_r1 r;
    compat_r1_facts(compat_os_windows(), folder, s->root, NULL, 0, NULL, &r);
    if (r.ort_sha256[0]) memcpy(out, r.ort_sha256, 65);
}

static int e_set_bridge(void *ctx, const char *path, const char *name) {
    nw_steps *s = ctx;
    return nw_admin_set_bridge(s->admin, path, name);
}

static void e_log(void *ctx, const char *line) { ue_log(ctx, line); }

/* BLOCKED: the verified lists (the saved payload, the installed-release record) */
static void read_lists(nw_steps *s, payload_manifest *saved, int *has_saved, payload_manifest *inst, int *has_inst) {
    *has_saved = e_saved(s, saved);
    *has_inst = s->support && s->own && update_installed(&s->ue, inst) == PAYLOAD_OK && inst->has_windows;
}

/* BLOCKED's facts, from one read of the lists: "plus"'s (engine, engine_ok) when want_plus,
 * "cache"'s (b, and the current version folder's found and version: current_folder) when
 * want_cache; plus_r, cache_r 0, or -1: no answer */
typedef struct {
    int plus_r, cache_r;
    char engine[64];
    int engine_ok;
    native_blocked b;
    int found;
    char version[COMPAT_VERSION_MAX];
} blocked_read;

static void blocked_facts(nw_steps *s, int want_plus, int want_cache, blocked_read *o) {
    memset(o, 0, sizeof *o);
    o->plus_r = o->cache_r = -1;
    payload_manifest *saved = malloc(sizeof *saved), *inst = malloc(sizeof *inst);
    if (saved && inst) {
        int hs, hi;
        read_lists(s, saved, &hs, inst, &hi);
        const payload_manifest *m = hs ? saved : hi ? inst : NULL;
        if (want_plus) {
            native_blocked_plus(s->model, m, o->engine, sizeof o->engine, &o->engine_ok);
            o->plus_r = 0;
        }
        char folder[COMPAT_PATH_MAX];
        if (want_cache) {
            o->found = current_folder(s, folder, o->version);
            o->cache_r = native_blocked_cache(compat_os_windows(), o->found ? folder : NULL, o->version, s->root, hs ? saved : NULL,
                                              hi ? inst : NULL, NULL, &o->b);
        }
    }
    free(saved);
    free(inst);
}

/* blockedByVersionList for "check": a feature off only because a version isn't listed yet, by the
 * core's ordered rules (native.h's native_blocked_by_list): rekordbox installed is a current
 * version folder found, the account's from native_user_is_admin; a part with no answer counts as
 * 0. Asked only when the check isn't due already (native_manifest_due). */
static int e_blocked(void *ctx) {
    nw_steps *s = ctx;
    blocked_read r;
    blocked_facts(s, 1, 1, &r);
    int admin = native_user_is_admin();
    native_blocked_facts f;
    memset(&f, 0, sizeof f);
    f.installed = r.found;
    f.plus_answered = r.plus_r == 0;
    f.engine = r.engine;
    f.engine_supported = r.engine_ok;
    f.cache_answered = r.cache_r == 0 && admin >= 0;
    f.admin = admin == 1;
    f.rekordbox_version = r.version;
    f.rekordbox_supported = r.b.rekordbox_supported;
    f.root_ort_exists = r.b.root_ort_exists;
    f.lib_exists = r.b.lib_exists;
    f.lib_signed = r.b.lib_signed;
    f.ort_listed = r.b.ort_listed;
    return native_blocked_by_list(&f);
}

/* DOWNLOAD's progress: the bytes of the total, else "Downloading" */
static void dl_progress(void *ctx, int64_t done, int64_t total) {
    nw_steps *s = ctx;
    if (total > 0) core_flow_progress(s->c, CORE_PROGRESS_DOWNLOAD, done < total ? done : total, total);
    else core_flow_progress(s->c, CORE_PROGRESS_DOWNLOAD, -2, 0);
}

static int e_download(void *ctx, const payload_manifest *m, const char *dir, const char *name, const char *sha, int64_t max) {
    nw_steps *s = ctx;
    core_flow_progress(s->c, CORE_PROGRESS_DOWNLOAD, -1, 0);   /* "Checking …" */
    update_env ue = s->ue;   /* this download's own progress, not a shared field */
    ue.progress = dl_progress;
    switch (update_fetch_asset(&ue, m->payload_version, dir, name, sha, max)) {
    case UPDATE_FETCH_OK: return NS_DL_OK;
    case UPDATE_FETCH_NO_SPACE: return NS_DL_NO_SPACE;
    case UPDATE_FETCH_DAMAGED: return NS_DL_DAMAGED;
    default: return NS_DL_OFFLINE;
    }
}

/* the visible top-level windows of this session's rekordbox.exe processes; a rekordbox.exe in
 * another session, or one whose session can't be read, counts as one (native_rb_windows):
 * EnumWindows sees only this session's desktop */
typedef struct {
    DWORD pids[64];
    int n, windows;
} rb_windows;

static BOOL WINAPI count_window(HWND w, LPARAM lp) {
    rb_windows *c = (rb_windows *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    for (int i = 0; i < c->n; i++)
        if (c->pids[i] == pid && IsWindowVisible(w)) {
            c->windows++;
            break;
        }
    return TRUE;
}

static int rekordbox_windows(void) {
    rb_windows c;
    memset(&c, 0, sizeof c);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    DWORD here = 0;
    int have_here = ProcessIdToSessionId(GetCurrentProcessId(), &here), other = 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more && c.n < 64; more = Process32NextW(snap, &pe))
        if (CompareStringOrdinal(pe.szExeFile, -1, L"rekordbox.exe", -1, TRUE) == CSTR_EQUAL) {
            DWORD sid = 0;
            if (have_here && ProcessIdToSessionId(pe.th32ProcessID, &sid) && sid == here) c.pids[c.n++] = pe.th32ProcessID;
            else other++;
        }
    CloseHandle(snap);
    int windows = 0;
    if (c.n) windows = EnumWindows(count_window, (LPARAM)&c) ? c.windows : -1;
    return native_rb_windows(other, windows);
}

static int e_processes(void *ctx, int *running, int *updating, int *windows) {
    nw_steps *s = ctx;
    compat_procs p;
    if (compat_processes(compat_os_windows(), s->rekordbox, &p)) return -1;
    *running = p.running;
    *updating = p.updating;
    *windows = 0;
    if (p.running && (*windows = rekordbox_windows()) < 0) return -1;
    return 0;
}

static int64_t e_size_of(void *ctx, const char *path) {
    (void)ctx;
    plat_stat st;
    return plat_stat_path(path, 1, &st) || st.kind != PLAT_FILE ? -1 : st.size;
}

static int64_t e_free(void *ctx, const char *path) {
    (void)ctx;
    plat_volume v;
    if (plat_volume_info(path, &v)) return -1;
    return v.avail > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)v.avail;
}

static int e_same(void *ctx, const char *a, const char *b) {
    (void)ctx;
    char ma[PLAT_MOUNT_MAX], mb[PLAT_MOUNT_MAX];
    int sa = 0, sb = 0;
    if (plat_volume_holding(a, ma, sizeof ma, &sa) || plat_volume_holding(b, mb, sizeof mb, &sb)) return 0;
    return !strcmp(ma, mb);
}

static int e_remove_tree_at(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    return plat_remove_tree_at(d, name);
}

static int e_remove_empty_dir_at(void *ctx, plat_dir d, const char *name, plat_id expect) {
    (void)ctx;
    return plat_remove_empty_dir_at(d, name, expect);
}

/* ---------------------------------------------------------------- the small steps */

static void log_failed(nw_steps *s, const char *what, int e) {
    char b[160];
    snprintf(b, sizeof b, "couldn't %s (error %d)", what, e);
    native_core_log(s->c, b);
}

/* a sha256 through the backend's memo; "" when unreadable */
static void memo_sha(nw_steps *s, const char *path, char out[65]) {
    if (!path || !s->shas || sha_memo_file(s->shas, path, out)) out[0] = 0;
}

/* stemsCachePresent: the live onnxruntime.dll in folder is our bridge (R1's read against the
 * verified lists' bridges, listed or not) */
static int cache_present(nw_steps *s, const char *folder) {
    payload_manifest *saved = malloc(sizeof *saved), *inst = malloc(sizeof *inst);
    int r = 0;
    if (saved && inst) {
        int hs, hi, nb = 0;
        read_lists(s, saved, &hs, inst, &hi);
        payload_sha bridges[2];
        if (hs) memcpy(bridges[nb++], saved->win_bridge.sha256, sizeof bridges[0]);
        if (hi) memcpy(bridges[nb++], inst->win_bridge.sha256, sizeof bridges[0]);
        compat_r1 r1;
        compat_r1_facts(compat_os_windows(), folder, s->root, bridges, nb, hs ? saved : hi ? inst : NULL, &r1);
        r = r1.live == COMPAT_LIVE_OURS || r1.live == COMPAT_LIVE_OURS_UNLISTED;
    }
    free(saved);
    free(inst);
    return r;
}

/* STATE_LINE: state() */
static void state_line(nw_steps *s, char *out, size_t n) {
    char folder[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX] = "", engine[64], sha[65], p[NS_PATH_MAX];
    int found = current_folder(s, folder, version), supported;
    native_blocked_plus(s->model, NULL, engine, sizeof engine, &supported);
    memo_sha(s, s->model, sha);
    int32_t model = 0, cache = 0;
    char *t = NULL;
    size_t tn = 0;
    if (s->support && snprintf(p, sizeof p, "%s\\installed", s->support) < (int)sizeof p && !plat_read_file(p, 64 * 1024, &t, &tn))
        state_chosen(t, tn, &model, &cache);
    free(t);
    native_state_line(out, n, version, engine, sha, found && cache_present(s, folder), model, cache);
}

/* LEFTOVER (native_leftover): a writer's start from the system's process list, naps of 100 ms */
static int64_t leftover_start(void *ctx, int32_t pid) {
    (void)ctx;
    return life_process_start((uint32_t)pid);
}

static void leftover_nap(void *ctx) {
    (void)ctx;
    Sleep(100);
}

/* appBusy: support\busy's writer still runs (this process's own marker included); a writer whose
 * start can't be asked counts as running */
static int app_busy(nw_steps *s) {
    char p[NS_PATH_MAX], *t = NULL;
    size_t tn = 0;
    if (!s->support || snprintf(p, sizeof p, "%s\\busy", s->support) >= (int)sizeof p || plat_read_file(p, 64 * 1024, &t, &tn)) return 0;
    arena a = {0};
    busy_marker m;
    int live = 0;
    if (busy_marker_parse(&a, t, tn, &m) == 1) {
        plat_stat st;
        int32_t hw = !plat_stat_path(p, 0, &st);
        int64_t started = life_process_start((uint32_t)m.pid);
        live = started == -2 || busy_marker_live(&m, started, hw, hw ? st.mtime : 0);
    }
    arena_free(&a);
    free(t);
    return live;
}

/* LAUNCH_AGENT_UNLOAD (removeWatcher; native_watcher_unload): the Run value and its
 * StartupApproved value through lifecycle.h, the running watchers through update_os's
 * stop_watchers (their stop event, then their handle, by full path); FACT_STEP_FLAG the reminders'
 * state afterwards (life_run_read's `off`) when it could be read. The step returns 0 even when
 * something stayed (logged), as the Mac's removeWatcher never fails it (review UN15) */
static int ul_run_remove(void *ctx) {
    (void)ctx;
    return life_run_remove();
}
static int ul_stop(void *ctx, const char *dir) {
    nw_steps *s = ctx;
    return s->ue.os->stop_watchers(s->ue.os->ctx, dir);
}
static int ul_off(void *ctx) {
    nw_steps *s = ctx;
    char stub[NS_PATH_MAX];
    life_run r;
    if (!s->apps || snprintf(stub, sizeof stub, "%s\\RB Stems Plus.exe", s->apps) >= (int)sizeof stub || life_run_read(stub, &r)) return -1;
    return r.off ? 1 : 0;
}
static void ul_log(void *ctx, const char *line) {
    nw_steps *s = ctx;
    native_core_log(s->c, line);
}
static int watcher_unload(nw_steps *s, core_facts *out) {
    native_unload_ops ops = {ul_run_remove, ul_stop, ul_off, ul_log, s};
    int off;
    native_watcher_unload(&ops, s->apps, &off);   /* what stayed is logged there */
    if (off >= 0) core_fact_flag(out, FACT_STEP_FLAG, off);
    return 0;   /* as the Mac's removeWatcher, the step never fails: the flow applies the reminders' flag only on 0 (review UN15) */
}

/* SELF_DELETE (core.h; afterexit.h, release.md T11): args[0] the app to delete, APP_FATE's apps
 * folder, or "" (the app stays: nothing runs after it, and only the notification settings of our
 * AppUserModelID go now, review UN9; the Apps & features entry and the shortcut stay with the
 * install, as on the Mac). The after-exit copy is started first: it waits for this process, then
 * removes the Windows records and the files; then the shell's uninstall quit (no window place, no
 * log file line after it, review UN5). Nonzero, the app staying open, when nothing would quit it
 * or the copy couldn't start. */
static int self_delete(nw_steps *s, const char *me) {
    if (!update_shell_can_quit()) return 1;
    if (me && me[0]) {
        if (!s->apps || !s->own || strcmp(me, s->apps)) return 1;   /* only this install's own folder */
        int e = afterexit_launch(s->apps, s->own, s->ue.arch);
        if (e) {
            char b[160];
            snprintf(b, sizeof b, "self-delete: the after-exit cleanup couldn't start (%s)", strerror(e));
            native_core_log(s->c, b);
            return 1;
        }
    } else if (life_records_remove(LIFE_AUMID)) {
        native_core_log(s->c, "self-delete: the notification settings couldn't be removed");
    }
    update_shell_quit_uninstall();
    return 0;
}

/* ---------------------------------------------------------------- the stems model (models.c) */

_Static_assert(MODELS_UNKNOWN == CORE_MODEL_UNKNOWN && MODELS_UNREADABLE == CORE_MODEL_UNREADABLE && MODELS_NOT_A_FILE == CORE_MODEL_NOT_A_FILE
                   && MODELS_NO_SPACE_SAVE == CORE_MODEL_NO_SPACE_SAVE && MODELS_NOT_SAVED == CORE_MODEL_NOT_SAVED
                   && MODELS_NO_SPACE_COPY == CORE_MODEL_NO_SPACE_COPY && MODELS_NOT_COPIED_WHY == CORE_MODEL_NOT_COPIED_WHY
                   && MODELS_COPY_BAD == CORE_MODEL_COPY_BAD && MODELS_CHANGED == CORE_MODEL_CHANGED
                   && MODELS_NOT_PLACED_WHY == CORE_MODEL_NOT_PLACED_WHY && MODELS_NO_SAVED_COPY == CORE_MODEL_NO_SAVED_COPY
                   && MODELS_NO_SPACE_BACK == CORE_MODEL_NO_SPACE_BACK && MODELS_NOT_BACK == CORE_MODEL_NOT_BACK
                   && MODELS_NOT_BACK_WHY == CORE_MODEL_NOT_BACK_WHY && MODELS_NOT_VERIFIED == CORE_MODEL_NOT_VERIFIED
                   && MODELS_SAVED_DAMAGED == CORE_MODEL_SAVED_DAMAGED,
               "models.h's outcomes are core.h's core_model_outcome");

static void model_log(void *ctx, const char *line) {
    nw_steps *s = ctx;
    native_core_log(s->c, line);
}

/* installedModels: rekordbox's model folder (paths.model's), support\originals, and ours (the
 * built-in model, the verified saved payload's model and previous_models: ourModelChecksums).
 * 0, or -1 when the paths aren't there */
static int models_of(nw_steps *s, models *m, char dir[NS_PATH_MAX], char originals[NS_PATH_MAX]) {
    memset(m, 0, sizeof *m);
    if (!s->model || !s->support || snprintf(dir, NS_PATH_MAX, "%s", s->model) >= NS_PATH_MAX
        || snprintf(originals, NS_PATH_MAX, "%s\\originals", s->support) >= NS_PATH_MAX)
        return -1;
    char *cut = strrchr(dir, '\\');
    if (!cut || cut == dir) return -1;
    *cut = 0;
    m->dir = dir;
    m->originals = originals;
    m->support = s->support;
    m->log = model_log;
    m->ctx = s;
    models_add_ours(m, MODELS_OURS_BUILTIN);
    payload_manifest *p = malloc(sizeof *p);
    if (!p) return -1;   /* never "the built-in model only" for want of memory (review MD11) */
    if (e_saved(s, p)) {   /* signed and verified only */
        models_add_ours(m, p->model.sha256);
        for (int i = 0; i < p->n_previous_models; i++) models_add_ours(m, p->previous_models[i]);
    }
    free(p);
    return 0;
}

/* a model step's outcome into its facts (setModelOutcome): 0 ok, else 1 */
static int model_outcome(core_facts *out, int r, const char *why) {
    if (r == MODELS_OK) return 0;
    if (r > 0) core_fact_i64(out, FACT_STEP_CODE, r);
    if (r > 0 && why[0]) core_fact_str(out, FACT_STEP_TEXT, why, CORE_NTS);
    return 1;
}

/* MODEL_COPY: the flow payload's model (support\payload\<version>\<model.file>) into rekordbox's place */
static int model_copy(nw_steps *s, core_facts *out) {
    models *m = malloc(sizeof *m);
    payload_manifest *p = malloc(sizeof *p);
    char dir[NS_PATH_MAX], originals[NS_PATH_MAX], src[NS_PATH_MAX], why[128] = "";
    int r = 1, has = 0;
    if (m && p) {
        plat_mutex_lock(&s->flow.mu);
        if ((has = s->flow.has)) *p = s->flow.m;
        plat_mutex_unlock(&s->flow.mu);
    }
    /* models.c brackets the copy through the rename in CriticalWork (m->critical NULL: the
     * process's); a closed admission is MODELS_STOPPED: 1, no code (review MC3/MC4) */
    if (has && !models_of(s, m, dir, originals) && !native_payload_dir(s->support, p, src, sizeof src))
        r = model_outcome(out, models_install_ours(m, src, p->model.file, p->model.sha256, why), why);
    free(m);
    free(p);
    return r;
}

static int model_restore(nw_steps *s, core_facts *out) {
    models *m = malloc(sizeof *m);
    char dir[NS_PATH_MAX], originals[NS_PATH_MAX], why[128] = "";
    int r = 1;
    if (m && !models_of(s, m, dir, originals)) r = model_outcome(out, models_restore(m, why), why);
    free(m);
    return r;
}

/* REMOVE_LEFTOVERS (removeLeftovers): not while another copy is changing things; the update's
 * leftover folders are update.c's (update_after_render), so only the model's temporary files */
static void remove_leftovers(nw_steps *s) {
    if (app_busy(s)) return;
    models *m = malloc(sizeof *m);
    char dir[NS_PATH_MAX], originals[NS_PATH_MAX];
    if (m && !models_of(s, m, dir, originals)) models_remove_leftovers(m);
    free(m);
}

#define TROUBLESHOOTING_URL "https://github.com/Gabe-LS/rbstemsplus/blob/main/docs/troubleshooting.md"   /* Paths.swift's */

/* MODELS_INFO ("saved": only FACT_ORIGINALS_SAVED) */
static int models_info(nw_steps *s, int saved_only, core_facts *out) {
    models *m = malloc(sizeof *m);
    char dir[NS_PATH_MAX], originals[NS_PATH_MAX];
    if (!m || models_of(s, m, dir, originals)) {
        free(m);
        return 1;
    }
    int saved = 0, verified = 0;
    models_originals_info(m, &saved, saved_only ? NULL : &verified);   /* one look decides both (review MP1); "saved" hashes nothing */
    core_fact_flag(out, FACT_ORIGINALS_SAVED, saved);
    if (!saved_only) {
        core_fact_flag(out, FACT_ORIGINALS_VERIFIED, verified);
        core_fact_i64(out, FACT_STEP_CODE, models_left_behind(m));
        core_fact_str(out, FACT_STEP_PATH, originals, CORE_NTS);
        core_fact_str(out, FACT_STEP_URL,
                      verified ? TROUBLESHOOTING_URL "#remove-rb-stems-plus-anyway" : TROUBLESHOOTING_URL "#no-saved-copy-of-rekordboxs-model",
                      CORE_NTS);
    }
    free(m);
    return 0;
}

/* UTF-8 to a malloc'd UTF-16 string; NULL when it isn't UTF-8 or no memory */
static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        w = NULL;
    }
    return w;
}

/* COM on this thread for one shell call (a worker's: the shell may ask its extensions): 1 when a
 * CoUninitialize is owed */
static int com_in(void) {
    HRESULT h = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    return h == S_OK || h == S_FALSE;
}

/* OPEN_URL: the default browser; only an http or https URL, never a file or a program */
static int open_url(const char *url) {
    if (_strnicmp(url, "https://", 8) && _strnicmp(url, "http://", 7)) return 1;
    wchar_t *w = wide(url);
    if (!w) return 1;
    int co = com_in();
    HINSTANCE h = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    if (co) CoUninitialize();
    free(w);
    return (INT_PTR)h > 32 ? 0 : 1;
}

/* REVEAL: Explorer's window of the file's folder, the file selected */
static int reveal(const char *path) {
    char p[NS_PATH_MAX];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) return 1;
    for (char *c = p; *c; c++)
        if (*c == '/') *c = '\\';
    wchar_t *w = wide(p);
    if (!w) return 1;
    int co = com_in();
    PIDLIST_ABSOLUTE id = ILCreateFromPathW(w);
    HRESULT h = id ? SHOpenFolderAndSelectItems(id, 0, NULL, 0) : E_FAIL;
    if (id) ILFree(id);
    if (co) CoUninitialize();
    free(w);
    return SUCCEEDED(h) ? 0 : 1;
}

/* config.ini's bytes (readConfigBytes): 0 with *b (NULL: none; free it), or -1 unreadable */
static int config_bytes(nw_steps *s, char **b, size_t *n) {
    char p[NS_PATH_MAX];
    *b = NULL;
    *n = 0;
    if (!s->support || snprintf(p, sizeof p, "%s\\config.ini", s->support) >= (int)sizeof p) return -1;
    int e = plat_read_file(p, CFG_MAX, b, n);
    if (e == ENOENT) return 0;
    if (e || !native_config_readable(*b, *n)) {
        free(*b);
        *b = NULL;
        return -1;
    }
    return 0;
}

/* CONFIG_WRITE: writeConfigValues of the pairs (max_gb and max_days as configNumber writes them) */
static int config_write(nw_steps *s, const char *const *args, int32_t nargs, core_facts *out) {
    native_config_pair pairs[8];
    char nums[8][32], said[512] = "", p[NS_PATH_MAX];
    int np = native_config_pairs(args, (int)nargs, pairs, nums), ok = np > 0;   /* review S46: host-tested there */
    char *b = NULL, *t = NULL;
    size_t n = 0, tn = 0;
    if (ok && !config_bytes(s, &b, &n) && !native_config_rewrite(b, n, pairs, np, NULL, 0, &t, &tn)) {
        ok = !native_support_write(s->support, "config.ini", t, tn);   /* support made first */
    } else ok = 0;
    free(b);
    free(t);
    if (!ok) {
        if (s->support && snprintf(p, sizeof p, "%s\\config.ini", s->support) < (int)sizeof p) core_fact_str(out, FACT_STEP_PATH, p, CORE_NTS);
        return 1;
    }
    for (int i = 0; i < np; i++) {
        size_t w = strlen(said);
        snprintf(said + w, sizeof said - w, "%s%s=%s", i ? " " : "", pairs[i].key, pairs[i].value);
    }
    core_fact_str(out, FACT_STEP_TEXT, said, CORE_NTS);
    return 0;
}

static void env_of(nw_steps *s, native_steps_env *e) {
    memset(e, 0, sizeof *e);
    e->fetch = e_fetch;
    e->saved = e_saved;
    e->seen = e_seen;
    e->age = e_age;
    e->sha = e_sha;
    e->ort_sha = e_ort_sha;
    e->set_bridge = e_set_bridge;
    e->log = e_log;
    e->download = e_download;
    e->processes = e_processes;
    e->size_of = e_size_of;
    e->free_bytes = e_free;
    e->same_volume = e_same;
    e->blocked = e_blocked;
    e->remove_tree_at = e_remove_tree_at;
    e->remove_empty_dir_at = e_remove_empty_dir_at;
    e->ctx = s;
    e->support = s->support;
    e->own = s->own;
    e->model = s->model;
    e->rekordbox = s->rekordbox;
    e->root = s->root;
}

/* ---------------------------------------------------------------- the entry */

int32_t nw_steps_run(nw_steps *s, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    native_steps_env e;
    env_of(s, &e);
    switch (kind) {
    case CORE_STEP_PAYLOAD: {
        native_payload_facts f;
        if (nargs < 1 || native_step_payload(&e, &s->flow, args[0], &f)) return 1;
        core_fact_i64(out, FACT_PAYLOAD_FETCH, f.fetch);
        core_fact_flag(out, FACT_PAYLOAD_FOUND, f.found);
        if (f.has_age) core_fact_i64(out, FACT_PAYLOAD_AGE, f.age);
        core_fact_str(out, FACT_STEP_PATH, f.step_path, CORE_NTS);
        if (f.has_flag) core_fact_flag(out, FACT_STEP_FLAG, f.flag);
        if (f.found) {
            core_fact_str(out, FACT_PAYLOAD_VERSION, f.version, CORE_NTS);
            core_fact_str(out, FACT_PAYLOAD_SIGNED_BY, f.signed_by, CORE_NTS);
            core_fact_str(out, FACT_PAYLOAD_APP_VERSION, f.app_version, CORE_NTS);
            core_fact_str(out, FACT_PAYLOAD_BRIDGE_SHA, f.bridge_sha, CORE_NTS);
            core_fact_flag(out, FACT_PAYLOAD_BRIDGE_HERE, f.bridge_here);
            core_fact_str(out, FACT_PAYLOAD_MODEL_SHA, f.model_sha, CORE_NTS);
        }
        return 0;
    }
    case CORE_STEP_CACHE_PREPARE: {
        char ort[65 + 16];
        int r = native_step_cache_prepare(&e, &s->flow, ort);
        if (!r) core_fact_str(out, FACT_STEP_TEXT, ort, CORE_NTS);
        return r;
    }
    case CORE_STEP_DOWNLOAD: return native_step_download(&e, &s->flow, nargs >= 1 ? args[0] : NULL);
    case CORE_STEP_REKORDBOX: {
        int open, closing, updating;
        if (native_step_rekordbox(&e, &open, &closing, &updating)) return 1;
        core_fact_flag(out, FACT_REKORDBOX_OPEN, open);
        core_fact_flag(out, FACT_PIONEER_UPDATING, updating);
        core_fact_flag(out, FACT_REKORDBOX_CLOSING, closing);
        return 0;
    }
    case CORE_STEP_BLOCKED: {
        /* stemsPlusBlocked's and stemsCacheBlocked's facts, in their order; the core decides
         * (plus_blocked, cache_blocked_win) */
        if (nargs < 1) return 1;
        blocked_read r;
        if (!strcmp(args[0], "plus")) {
            blocked_facts(s, 1, 0, &r);
            if (r.plus_r) return 1;
            core_fact_str(out, FACT_ENGINE, r.engine, CORE_NTS);
            core_fact_flag(out, FACT_ENGINE_SUPPORTED, r.engine_ok);
            return 0;
        }
        if (strcmp(args[0], "cache")) return 1;
        blocked_facts(s, 0, 1, &r);
        if (r.cache_r) return 1;   /* no answer: the core stops (Q3) */
        const native_blocked b = r.b;
        core_fact_flag(out, FACT_REKORDBOX_SUPPORTED, b.rekordbox_supported);
        if (!b.rekordbox_supported) return 0;
        core_fact_flag(out, FACT_ROOT_ORT_EXISTS, b.root_ort_exists);
        core_fact_flag(out, FACT_PROTOTYPE_EXISTS, 0);   /* the Mac's test build only */
        core_fact_flag(out, FACT_LIB_EXISTS, b.lib_exists);
        if (!b.lib_exists) return 0;
        core_fact_flag(out, FACT_LIB_SIGNED, b.lib_signed);
        if (!b.lib_signed) return 0;
        if (b.ort_version[0]) core_fact_str(out, FACT_ORT_VERSION, b.ort_version, CORE_NTS);
        core_fact_flag(out, FACT_ORT_LISTED, b.ort_listed);
        return 0;
    }
    case CORE_STEP_SPACE: {
        int64_t need = 0;
        int with_model = nargs >= 1 && !strcmp(args[0], "1"), with_cache = nargs >= 2 && !strcmp(args[1], "1");
        /* the version folder now, not the start's snapshot: an update of rekordbox since moves it */
        char folder[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX];
        if (with_cache && (current_folder(s, folder, version) || s->rekordbox)) e.rekordbox = folder;
        if (native_step_space(&e, &s->flow, with_model, with_cache, &need)) return 1;
        core_fact_i64(out, FACT_STEP_CODE, need);
        return 0;
    }
    case CORE_STEP_PRUNE_PAYLOADS: return native_step_prune_payloads(&e, &s->flow);
    case CORE_STEP_SET_CHOSEN: {
        if (nargs < 2) return 1;
        char t[32];
        state_chosen_text(t, sizeof t, !strcmp(args[0], "1"), !strcmp(args[1], "1"));
        int r = native_support_write(s->support, "installed", t, strlen(t));
        if (r) log_failed(s, "write installed", r);
        return 0;   /* setChosen's try?, as the Mac's */
    }
    case CORE_STEP_BUSY_MARKER: {
        int r;
        if (nargs >= 2) {
            char b[2048];
            tbuf t;
            tb_init(&t, CORE_OS_WIN, b, sizeof b);
            busy_marker_text(&t, plat_pid(), plat_process_start(), args[0], args[1]);
            r = t.err ? E2BIG : native_support_write(s->support, "busy", t.p, t.len);
        } else r = native_support_remove(s->support, "busy");
        if (r) log_failed(s, nargs >= 2 ? "write busy" : "remove busy", r);
        return 0;   /* setBusyMarker's try? */
    }
    case CORE_STEP_WELCOME:
        if (nargs < 1) return 1;
        if (!strcmp(args[0], "check")) core_fact_flag(out, FACT_STEP_FLAG, native_support_exists(s->support, "welcomed"));
        else if (!strcmp(args[0], "done")) {
            int r = native_support_write(s->support, "welcomed", "", 0);
            if (r) log_failed(s, "write welcomed", r);
        } else return 1;
        return 0;
    case CORE_STEP_REQUEST: {
        int64_t age = native_support_age(s->support, "request", plat_now_wall_s());
        if (age >= 0 && nargs >= 1 && !strcmp(args[0], "take")) {
            int r = native_support_remove(s->support, "request");
            if (r) log_failed(s, "remove request", r);
        }
        core_fact_i64(out, FACT_STEP_CODE, age);
        return 0;
    }
    case CORE_STEP_BRIDGE_SHA: {
        char folder[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX], p[COMPAT_PATH_MAX + 32], sha[65] = "";
        int found = current_folder(s, folder, version);
        if ((found || s->rekordbox) && snprintf(p, sizeof p, "%s\\onnxruntime.dll", folder) < (int)sizeof p) memo_sha(s, p, sha);
        core_fact_str(out, FACT_STEP_TEXT, sha, CORE_NTS);
        return 0;
    }
    case CORE_STEP_STATE_LINE: {
        char line[512];
        state_line(s, line, sizeof line);
        core_fact_str(out, FACT_STEP_TEXT, line, CORE_NTS);
        return 0;
    }
    case CORE_STEP_CACHE_FOLDERS: {
        /* rekordbox's folder: the current version folder, which the admin's uninstall works in; both
         * not followed (fileType's lstat: a link there, dangling or not, counts) */
        char folder[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX];
        plat_stat st;
        int found = current_folder(s, folder, version), rb = 0;
        if (found || s->rekordbox) rb = !plat_stat_path(folder, 0, &st);
        core_fact_flag(out, FACT_STEP_FLAG, rb);
        core_fact_flag(out, FACT_STEP_FLAG2, !plat_stat_path(s->root, 0, &st));
        return 0;
    }
    case CORE_STEP_UPDATE_CHECK: core_fact_i64(out, FACT_STEP_CODE, update_where(&s->ue)); return 0;   /* update.c, host-tested */
    case CORE_STEP_OPEN_URL: return nargs >= 1 && args[0] ? open_url(args[0]) : 1;
    case CORE_STEP_REVEAL: return nargs >= 1 && args[0] && args[0][0] ? reveal(args[0]) : 1;
    case CORE_STEP_CONFIG_WRITE: return config_write(s, args, nargs, out);
    case CORE_STEP_CONFIG_SETS: {
        if (nargs < 1 || !args[0][0]) return 1;
        char *b;
        size_t n;
        int sets = !config_bytes(s, &b, &n) && native_config_sets(b, n, args[0]);   /* unreadable or none: no */
        free(b);
        core_fact_flag(out, FACT_STEP_FLAG, sets);
        return 0;
    }
    case CORE_STEP_FREE_SPACE: {
        int64_t b = nargs >= 1 && args[0][0] ? e_free(s, args[0]) : -1;
        if (b < 0) return 1;
        core_fact_i64(out, FACT_STEP_CODE, b);
        return 0;
    }
    case CORE_STEP_STOP_KEEPING: return nargs >= 1 ? native_stop_keeping(s->kept, args[0]) : 1;
    case CORE_STEP_REMOVE_OWN_FOLDERS:
        /* the saved stems (Cache\) and the logs (Logs\) are support's own entries on Windows
         * (bridge.md §0): they go with it, and no folder named by the shell's paths is deleted
         * on its own (review S43's rule); paths.support itself only when it is this account's
         * LocalAppData\rbstemsplus (review S53) */
        return native_remove_own_folders(&e, s->expected_support, nargs >= 1 && !strcmp(args[0], "1"), NULL, 0);
    case CORE_STEP_APP_FATE: {
        int code = update_app_fate(&s->ue);   /* update.c, host-tested (review S61) */
        core_fact_i64(out, FACT_STEP_CODE, code);
        if (!code) core_fact_str(out, FACT_STEP_PATH, s->apps, CORE_NTS);
        return 0;
    }
    case CORE_STEP_SELF_DELETE: return self_delete(s, nargs >= 1 ? args[0] : NULL);
    case CORE_STEP_LAUNCH_AGENT_UNLOAD: return watcher_unload(s, out);
    case CORE_STEP_MODEL_COPY: return model_copy(s, out);
    case CORE_STEP_MODEL_RESTORE: return model_restore(s, out);
    case CORE_STEP_MODELS_INFO: return models_info(s, nargs >= 1 && !strcmp(args[0], "saved"), out);
    case CORE_STEP_REMOVE_LEFTOVERS: remove_leftovers(s); return 0;
    case CORE_STEP_REMOVE_LOCATION: {
        char name[256], err[128];
        int r = nargs >= 3 ? native_remove_location(&e, args[0], args[1], !strcmp(args[2], "1"), name, err) : 7;
        if (!r) return 0;
        core_fact_i64(out, FACT_STEP_CODE, r);
        if (r == 4 || r == 5) core_fact_str(out, FACT_STEP_TEXT2, name, CORE_NTS);
        if (r == 4) core_fact_str(out, FACT_STEP_TEXT, err, CORE_NTS);
        return 1;
    }
    case CORE_STEP_MIGRATE_LEGACY:
        /* 1.0's originals\hdemucs.onnx is the Mac's: Windows never had that layout */
        core_fact_flag(out, FACT_STEP_FLAG, 0);
        return 0;
    case CORE_STEP_LEFTOVER: {
        char id[256], label[512];
        native_leftover(s->support, plat_pid(), leftover_start, leftover_nap, NULL, id, sizeof id, label, sizeof label);
        core_fact_str(out, FACT_STEP_TEXT, id, CORE_NTS);
        core_fact_str(out, FACT_STEP_TEXT2, label, CORE_NTS);
        return 0;
    }
    case CORE_STEP_CLEAR_THIS_MAC: {
        int link, r = native_clear_cache(&e, s->cache_default, &link);
        core_fact_flag(out, FACT_STEP_FLAG, link);
        return r;
    }
    case CORE_STEP_UPDATE_SWAP: {
        /* the app's zip staged (its progress as DOWNLOAD's), the new app's --swap started, then
         * the shell's quit (core_win_set_quit: CloseGate.QuitNow, which waits for CriticalWork;
         * this step is never critical itself). No quit registered: nothing started, 3 */
        core_flow_progress(s->c, CORE_PROGRESS_DOWNLOAD, -1, 0);   /* "Checking …" */
        update_env ue = s->ue;
        ue.progress = dl_progress;
        return update_step_swap(&ue, update_shell_can_quit() ? update_shell_quit : NULL, NULL);
    }
    case CORE_STEP_UPDATE_RESULT: {
        /* the start's update_at_start answer (update_result_keep), said once */
        const char *line = "";
        int failed = update_result_take(&s->ue, &line);
        core_fact_flag(out, FACT_STEP_FLAG, failed);
        core_fact_str(out, FACT_STEP_TEXT, line ? line : "", CORE_NTS);
        return 0;
    }
    default: return CORE_E_STATE;   /* not done yet: as a step entry not provided */
    }
}
