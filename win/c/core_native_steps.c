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
 *  - BLOCKED: the facts of the verified lists, with Stems Cache eligibility
 *    aggregated over the inventory. The same facts make "check"'s blockedByVersionList.
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
 *    DOWNLOAD's. UPDATE_RESULT: update_result_take (the start's failed key, said once).
 *  - LAUNCH_AGENT_LOAD, WATCHER_CHECK, WATCHER_STATE: native_steps.c's native_watcher_* over the
 *    Run value (lifecycle.h's life_run_*: <apps>\RB Stems Plus.exe --watcher) and update_os's
 *    watchers_in and spawn; only an app in its install (update_in_place) names itself.
 *  - SETTINGS: host_facts.c's config.ini read, native_settings_facts; Change… when the gate and
 *    bridge allow it, and Clear for a chosen folder from a fresh measure. LOCATION_NOW and PAUSED
 *    use the bounded measure with the gate on. LOCATION_NOW can report the configured folder
 *    without granting a deletion gate (review NS11). LOCATION_REMEASURE: the core's memo remeasures.
 *  - The location choice (plan N3): LOCATION_PROBE, DISCOVER, SIZE, WRITABLE, CREATE, WRITE and
 *    THIS_MAC_DELETE, native_steps.c's rules with this PC's answers (vol_facts_of, vol_guid_path:
 *    cache_dir's volume-GUID form, the contract with the bridge; CoCreateGuid's id; the
 *    not-indexed attribute).
 *  - DATE_TEXT: GetDateFormatEx's long date. NOTIFY: notify_win.c's notification.
 *  - REPORT_FACTS: host_facts.c's refresh rows read now, the model's sha256 (uncached), the
 *    location's measure. REPORT_ZIP: report_build.c's zip with Windows' facts (report_lines), the
 *    event log and WER (report_win.c), masked by core_mask (the profile folder, the account's
 *    name); the issue's link (report_issue_url). */
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
#include "dismissed.h"
#include "host_facts.h"
#include "notify.h"
#include "report.h"
#include "native_config.h"
#include "native_facts.h"
#include "lifecycle.h"
#include "models.h"
#include "native_choice_win.h"
#include "native_steps.h"
#include "win_rules.h"
#include "sha_memo.h"
#include "update.h"
#include "update_shell.h"
#include "volumes.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert((int)WIN_LOCATION_TIMED_OUT == (int)NS_CHOICE_TIMED_OUT &&
               (int)WIN_LOCATION_SAVED_UNCHECKED == (int)NS_CHOICE_SAVED_UNCHECKED, "location step codes");

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
    /* The picker and Create Report each run on the serial core flow. */
    uint64_t choice_deadline, location_now_deadline;
    char choice_mount[NS_PATH_MAX], choice_label[NL_DRIVE_MAX];
    int choice_has_label;
    native_location report_location;
    int report_measured;
    /* A publication carries every covered EXE and its common resampling proof.
     * rb_gen issues tickets atomically; rb_applied orders results under rb_lock. */
    SRWLOCK rb_lock;
    SRWLOCK compat_refresh_lock;
    char rb_known[COMPAT_PATH_MAX];
    host_covered_exe rb_covered[INV_MAX], rb_fallback;
    int rb_ncovered, rb_proved;
    LONG64 rb_gen, rb_applied;
    nw_steps *next;
};

/* The shell's after-render worker has no core handle. Keep live backends available for its
 * installed-release refresh, with their lifetimes pinned while it uses them. */
static SRWLOCK steps_lock = SRWLOCK_INIT;
static nw_steps *steps_list;

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
    AcquireSRWLockExclusive(&steps_lock);
    s->next = steps_list;
    steps_list = s;
    ReleaseSRWLockExclusive(&steps_lock);
    return s;
}

void nw_steps_close(nw_steps *s) {
    if (!s) return;
    if (s->ue.ctx) {
        AcquireSRWLockExclusive(&steps_lock);
        nw_steps **at = &steps_list;
        while (*at != s) at = &(*at)->next;
        *at = s->next;
        ReleaseSRWLockExclusive(&steps_lock);
        native_flow_free(&s->flow);
    }
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

/* BLOCKED and Prepare use a verified saved payload first, even if its list is empty. */
static int read_installed_list(nw_steps *s, payload_manifest *out) {
    return s->support && s->own && update_installed(&s->ue, out) == PAYLOAD_OK && out->has_windows;
}

static host_facts *read_rb_coverage(nw_steps *s) {
    host_facts *h = malloc(sizeof *h);
    if (h && host_facts_read_core(s->c, h)) {
        host_facts_free(h);
        free(h);
        return NULL;
    }
    return h;
}

static void free_rb_coverage(host_facts *h) {
    if (h) host_facts_free(h);
    free(h);
}

static host_covered_exe fallback_exe(const compat_rekordbox *rb) {
    host_covered_exe out = {0};
    if (rb) {
        strcpy(out.folder, rb->folder);
        out.id = rb->exe.id;
        out.size = rb->exe.size;
        out.mtime = rb->exe.mtime;
    }
    return out;
}

static int proved_coverage(const host_facts *h) {
    if (!h || !h->cache_answered || !h->ncovered) return 0;
    for (int i = 0; i < h->ncovered; i++) {
        const host_covered_exe *exe = &h->covered[i];
        if (!compat_resample_proved(h->cache_manifest, exe->product_version, exe->sha256)) return 0;
    }
    return 1;
}

/* rb_lock held. */
static void set_rb_coverage(nw_steps *s, LONG64 gen, host_covered_exe fallback, const host_facts *h, int proved) {
    s->rb_fallback = fallback;
    s->rb_ncovered = h ? h->ncovered : 0;
    if (s->rb_ncovered) memcpy(s->rb_covered, h->covered, (size_t)s->rb_ncovered * sizeof s->rb_covered[0]);
    strcpy(s->rb_known, s->rb_ncovered ? s->rb_covered[0].folder : fallback.folder);
    s->rb_proved = proved;
    s->rb_applied = gen;
}

static int publish_rb_folder(nw_steps *s, LONG64 gen, const compat_rekordbox *rb) {
    host_facts *h = read_rb_coverage(s);
    int proved = proved_coverage(h);
    host_covered_exe fallback = fallback_exe(rb);
    AcquireSRWLockExclusive(&s->rb_lock);
    int accepted = gen > s->rb_applied;
    if (accepted) set_rb_coverage(s, gen, fallback, h, proved);
    ReleaseSRWLockExclusive(&s->rb_lock);
    free_rb_coverage(h);
    return accepted;
}

/* A payload or installed-release change can change coverage and proof together. */
static void refresh_rb_proved(nw_steps *s) {
    for (;;) {
        LONG64 gen = InterlockedIncrement64(&s->rb_gen);
        AcquireSRWLockShared(&s->rb_lock);
        LONG64 copied = s->rb_applied;
        host_covered_exe fallback = s->rb_fallback;
        ReleaseSRWLockShared(&s->rb_lock);
        host_facts *h = read_rb_coverage(s);
        int proved = proved_coverage(h);
        AcquireSRWLockExclusive(&s->rb_lock);
        int stale = gen <= s->rb_applied;
        int same = copied == s->rb_applied;
        if (!stale && same) set_rb_coverage(s, gen, fallback, h, proved);
        ReleaseSRWLockExclusive(&s->rb_lock);
        free_rb_coverage(h);
        if (stale || same) return;
    }
}

void nw_steps_installed_changed(const char *support, const char *own) {
    AcquireSRWLockShared(&steps_lock);
    for (nw_steps *s = steps_list; s; s = s->next)
        if (s->support && s->own && !_stricmp(s->support, support) && !strcmp(s->own, own)) refresh_rb_proved(s);
    ReleaseSRWLockShared(&steps_lock);
}

void nw_steps_refresh_inventory(core *c) {
    AcquireSRWLockShared(&steps_lock);
    for (nw_steps *s = steps_list; s; s = s->next)
        if (s->c == c) refresh_rb_proved(s);
    ReleaseSRWLockShared(&steps_lock);
}

int nw_steps_cache_covers_prepare(nw_steps *s) {
    if (!s) return 0;
    AcquireSRWLockShared(&s->rb_lock);
    int covered = s->rb_ncovered > 0;
    ReleaseSRWLockShared(&s->rb_lock);
    return covered;
}

static int verified_folder(nw_steps *s, char *folder, size_t n, char *version, size_t vn, int short_version, int publish) {
    if (n) folder[0] = 0;
    if (vn) version[0] = 0;
    compat_rekordbox *rb = calloc(COMPAT_ENTRIES_MAX, sizeof *rb);
    int cur, found = 0;
    /* If a payload refresh beat discovery to publication, look again with a fresh ticket. */
    for (;;) {
        LONG64 gen = publish ? InterlockedIncrement64(&s->rb_gen) : 0;
        int state = 0;
        cur = -1;
        if (rb) compat_find_rekordbox(compat_os_windows(), s->rb_parent, rb, COMPAT_ENTRIES_MAX, &cur, &state);
        if (!publish || publish_rb_folder(s, gen, cur >= 0 ? &rb[cur] : NULL)) break;
    }
    if (cur >= 0 && (size_t)snprintf(folder, n, "%s", rb[cur].folder) < n) {
        const char *v = short_version || !rb[cur].display_version[0] ? rb[cur].folder_version : rb[cur].display_version;
        found = !version || !vn || (size_t)snprintf(version, vn, "%s", v) < vn;
    }
    free(rb);
    if (!found) {
        if (n) folder[0] = 0;
        if (vn) version[0] = 0;
    }
    return found;
}

/* The flow's display version and the paths.rekordbox fallback for its measurements and log. */
static int current_folder(nw_steps *s, char folder[COMPAT_PATH_MAX], char version[COMPAT_VERSION_MAX]) {
    if (verified_folder(s, folder, COMPAT_PATH_MAX, version, COMPAT_VERSION_MAX, 0, 1)) return 1;
    snprintf(folder, COMPAT_PATH_MAX, "%s", s->rekordbox ? s->rekordbox : "");
    return 0;
}

static int covered_exe_holds(const host_covered_exe *then) {
    char path[COMPAT_PATH_MAX + 16];
    plat_stat now;
    return then->folder[0] && (size_t)snprintf(path, sizeof path, "%s\\rekordbox.exe", then->folder) < sizeof path
        && !plat_stat_path(path, 0, &now) && now.kind == PLAT_FILE
        && plat_id_same(now.id, then->id) && now.size == then->size && now.mtime == then->mtime;
}

int nw_steps_rekordbox_known(nw_steps *s, char *folder, size_t n, int *proved) {
    if (n) folder[0] = 0;
    *proved = 0;
    if (!s) return 0;
    host_covered_exe covered[INV_MAX];
    AcquireSRWLockShared(&s->rb_lock);
    LONG64 applied = s->rb_applied;
    int count = s->rb_ncovered;
    int bit = s->rb_proved;
    host_covered_exe primary = count ? s->rb_covered[0] : s->rb_fallback;
    if (count > 1) memcpy(covered + 1, s->rb_covered + 1, (size_t)(count - 1) * sizeof covered[0]);
    ReleaseSRWLockShared(&s->rb_lock);

    int primary_ok = covered_exe_holds(&primary), held = primary_ok;
    for (int i = 1; i < count; i++) held &= covered_exe_holds(&covered[i]);
    if (!held && count) {
        AcquireSRWLockExclusive(&s->rb_lock);
        if (s->rb_applied == applied) s->rb_proved = 0;
        ReleaseSRWLockExclusive(&s->rb_lock);
    }
    if (!primary_ok || strlen(primary.folder) >= n) return 0;
    strcpy(folder, primary.folder);
    AcquireSRWLockShared(&s->rb_lock);
    *proved = bit && held && s->rb_applied == applied && s->rb_proved;
    ReleaseSRWLockShared(&s->rb_lock);
    return 1;
}

/* R1 only on a verified current rekordbox (review CW1): no paths.rekordbox fallback, which the
 * logging and measuring steps above keep */
int nw_steps_rekordbox_folder(nw_steps *s, char *folder, size_t n) {
    return s ? native_r1_folder(compat_os_windows(), s->rb_parent, folder, n) : 0;
}

int nw_steps_rekordbox_current(nw_steps *s, char *folder, size_t n, char *version, size_t vn) {
    if (version && vn) version[0] = 0;
    return s && folder && n ? verified_folder(s, folder, n, version, version ? vn : 0, 1, 0) : 0;
}

int nw_steps_r1_dismissed(nw_steps *s, const char *version) {
    char key[COMPAT_VERSION_MAX + 8];
    if (!s || !s->support || !version || !version[0] || watch_r1_dismissed_key(version, key, sizeof key) >= sizeof key) return 0;
    return dismissed_has(s->support, key);
}

/* WIN_R1_DISMISS (N10, core.h): shown, the versions Don't Ask Again showed ("7.2.20,7.2.19"):
 * "r1:<version>" for each one whose folder R1 still holds now (looked up in the inventory read
 * now), added to dismissed in one write under the answer lock (the watcher's writer). A version
 * that wasn't shown is never written; FACT_STEP_TEXT the versions written, ", "-joined */
static int32_t r1_dismiss(nw_steps *s, const char *shown, core_facts *out) {
    if (!shown || !shown[0]) return 1;
    host_facts *h = malloc(sizeof *h);
    if (!h) return 1;
    if (host_facts_read_core(s->c, h) || !h->inventory) {
        free_rb_coverage(h);
        return 1;
    }
    char keys[INV_MAX * (COMPAT_VERSION_MAX + 8)] = "", written[INV_MAX * (COMPAT_VERSION_MAX + 2)] = "";
    size_t kn = 0, wn = 0;
    for (const char *p = shown; *p;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        for (int i = 0; n && n < COMPAT_VERSION_MAX && i < h->inventory->n; i++) {
            const inv_install *row = &h->inventory->it[i];
            if (row->trust == INV_STALE || strlen(row->version) != n || memcmp(row->version, p, n)) continue;
            char key[COMPAT_VERSION_MAX + 8];
            size_t k = watch_r1_dismissed_key(row->version, key, sizeof key);
            /* nothing to put back there now (or no answer), or no room: not written */
            if (k >= sizeof key || h->assessment[i].r1 <= 0
                || kn + k + 1 >= sizeof keys || wn + n + 3 >= sizeof written) break;
            memcpy(keys + kn, key, k);
            kn += k;
            keys[kn++] = '\n';
            keys[kn] = 0;
            wn += (size_t)snprintf(written + wn, sizeof written - wn, "%s%s", wn ? ", " : "", row->version);
            break;
        }
        p += n + (e != NULL);
    }
    free_rb_coverage(h);
    if (!kn) return 1;
    core_fact_str(out, FACT_STEP_TEXT, written, CORE_NTS);
    int e = !s->support ? ENOENT : dismissed_add_locked(s->support, keys, kn);
    if (!e) return 0;
    core_fact_i64(out, FACT_STEP_CODE, e);
    char b[160];
    snprintf(b, sizeof b, "  couldn't write dismissed (error %d)", e);
    native_core_log(s->c, b);
    return 2;
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

static void read_lists(nw_steps *s, payload_manifest *saved, int *has_saved, payload_manifest *inst, int *has_inst) {
    *has_saved = e_saved(s, saved);
    *has_inst = read_installed_list(s, inst);
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
    }
    if (want_cache) {
        host_facts *h = read_rb_coverage(s);
        if (h) {
            o->b = h->blocked;
            o->found = h->installed;
            strcpy(o->version, h->version);
            if (!o->found && o->b.rekordbox_supported) {
                for (int i = 0; i < h->inventory->n; i++) {
                    const inv_install *r = &h->inventory->it[i];
                    if (r->trust != INV_VERIFIED || !h->assessment[i].original_bound) continue;
                    strcpy(o->version, r->display_version[0] ? r->display_version : r->version);
                    o->found = 1;
                    break;
                }
            }
            o->cache_r = (h->cache_answered || h->blocked.rekordbox_supported) ? 0 : -1;
        }
        free_rb_coverage(h);
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

/* STATE_LINE: state() */
static void state_line(nw_steps *s, char *out, size_t n) {
    host_facts *h = read_rb_coverage(s);
    char sha[65];
    memo_sha(s, s->model, sha);
    native_state_line(out, n, h && h->cache_states[0] ? h->cache_states : "inventory unavailable", h ? h->engine : "", sha,
                      h && h->cache_present, h && h->chosen_model, h && h->chosen_cache);
    free_rb_coverage(h);
}

/* Called on the background queue. Record the attempt before going online, including when the
 * server is still serving the same compatibility list or is unreachable. */
static int claim_compat_refresh(nw_steps *s) {
    enum { HISTORY_MAX = 64 * 1024 };
    static const char name[] = "compat-refresh";
    char path[NS_PATH_MAX], added[INV_REFRESH_KEYS_MAX];
    if (!s->support || snprintf(path, sizeof path, "%s\\%s", s->support, name) >= (int)sizeof path) return 0;
    host_facts *h = read_rb_coverage(s);
    size_t n = 0, extra = 0;
    char *seen = NULL;
    if (h && h->chosen_cache && h->ncovered && h->inventory && h->inventory->complete && h->cache_manifest) {
        inv_row_view rows[INV_MAX];
        int count = inv_rows(h->inventory, h->assessment, h->cache_manifest, rows, INV_MAX);
        int err = plat_read_file(path, HISTORY_MAX, &seen, &n);
        if (!err || err == ENOENT) extra = inv_refresh_keys(rows, count, h->cache_manifest, seen ? seen : "", added);
    }
    free_rb_coverage(h);
    int claimed = 0;
    if (extra && n + extra + 1 <= HISTORY_MAX) {
        char *next = realloc(seen, n + extra + 1);
        if (next) {
            seen = next;
            if (n && seen[n - 1] != '\n') seen[n++] = '\n';
            memcpy(seen + n, added, extra);
            claimed = !native_support_write(s->support, name, seen, n + extra);
        }
    }
    free(seen);
    return claimed;
}

static int refresh_compat_payload(nw_steps *s, const native_steps_env *e, native_payload_facts *out) {
    AcquireSRWLockExclusive(&s->compat_refresh_lock);
    int r = 1;
    if (claim_compat_refresh(s)) {
        ue_log(s, "checking the compatibility list for newly seen rekordbox versions");
        r = native_step_payload(e, &s->flow, "fetch", out);
    }
    ReleaseSRWLockExclusive(&s->compat_refresh_lock);
    return r;
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
    (void)ctx;
    life_run r;
    /* The stub path is only needed to identify which install the Run value names. */
    if (life_run_read("", &r)) return -1;
    return r.present && r.off;
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

/* ---------------------------------------------------------------- the watcher's registration */

/* LAUNCH_AGENT_LOAD, WATCHER_CHECK, WATCHER_STATE (native_watcher_*): the Run value names the stub
 * <apps>\RB Stems Plus.exe with --watcher (life_run_*); the watcher runs from <apps>\<own>\ */
typedef struct {
    nw_steps *s;
    char stub[NS_PATH_MAX], own_dir[NS_PATH_MAX];
} wl_ctx;
static int wl_read(void *ctx, int *present, int *ours, int *off) {
    life_run r;
    int e = life_run_read(((wl_ctx *)ctx)->stub, &r);
    if (e) return e;
    *present = r.present;
    *ours = r.ours;
    *off = r.off;
    return 0;
}
static int wl_write(void *ctx) { return life_run_write(((wl_ctx *)ctx)->stub); }
static int wl_running(void *ctx) {
    wl_ctx *w = ctx;
    return w->s->ue.os && w->s->ue.os->watchers_in ? w->s->ue.os->watchers_in(w->s->ue.os->ctx, w->own_dir) : -1;
}
static int wl_start(void *ctx) {
    wl_ctx *w = ctx;
    const char *args[] = {"--watcher", NULL};
    return w->s->ue.os && w->s->ue.os->spawn ? w->s->ue.os->spawn(w->s->ue.os->ctx, w->stub, args) : ENOSYS;
}
static void wl_log(void *ctx, const char *line) { native_core_log(((wl_ctx *)ctx)->s->c, line); }

/* Always supply the ops. 0 with the paths needed for repair, -1 when they aren't available. */
static int watcher_ops(nw_steps *s, wl_ctx *w, native_watcher_ops *ops) {
    w->s = s;
    *ops = (native_watcher_ops){wl_read, wl_write, wl_running, wl_start, wl_log, w};
    if (!s->apps || !s->own || snprintf(w->stub, sizeof w->stub, "%s\\RB Stems Plus.exe", s->apps) >= (int)sizeof w->stub
        || snprintf(w->own_dir, sizeof w->own_dir, "%s\\%s", s->apps, s->own) >= (int)sizeof w->own_dir)
        return -1;
    return 0;
}

static int watcher_step(nw_steps *s, core_step kind, core_facts *out) {
    wl_ctx w;
    native_watcher_ops ops;
    int have = !watcher_ops(s, &w, &ops), off;
    if (kind == CORE_STEP_WATCHER_STATE || !have || update_in_place(&s->ue)) {
        if (kind == CORE_STEP_LAUNCH_AGENT_LOAD) native_core_log(s->c, "watcher not installed: RB Stems Plus isn't in its install");
        w.stub[0] = 0;   /* the reminders are measured by Run value name, regardless of the stub path */
        off = native_watcher_off(&ops);
    } else if (kind == CORE_STEP_LAUNCH_AGENT_LOAD) {
        native_watcher_load(&ops, &off);
    } else {
        native_watcher_check(&ops, &off);
    }
    /* Startup treats an unset flag on success as on. LOAD's caller checks the flag itself. */
    if (off < 0) return kind != CORE_STEP_LAUNCH_AGENT_LOAD;
    core_fact_flag(out, FACT_STEP_FLAG, off);
    return 0;
}

/* ---------------------------------------------------------------- where the stems are (SETTINGS, PAUSED, LOCATION_NOW) */

/* LocationMonitor's measure now: while the location's gate is off, This PC (support\Cache in use);
 * else the measure, bounded (nw_location_measure_now). 0 with *l, or -1 no answer */
static int location_now_until(nw_steps *s, native_location *l, uint64_t deadline) {
    memset(l, 0, sizeof *l);
    if (!native_location_live()) {
        l->kind = LOC_THIS_MAC;
        l->probe = -1;
        l->paused_floor = -1;
        if (s->cache_default) {
            snprintf(l->in_use, sizeof l->in_use, "%s", s->cache_default);
            l->has_in_use = 1;
        }
        return 0;
    }
    uint64_t now = plat_now_mono_ns();
    return now < deadline ? nw_location_measure_now(s->support, s->cache_default, deadline - now, l) : -1;
}

static int location_now(nw_steps *s, native_location *l) {
    return location_now_until(s, l, plat_now_mono_ns() + NL_WAIT_NS);
}

typedef struct {
    char support[NS_PATH_MAX], dir[NS_PATH_MAX], id[40];
    int unreadable, location;
    double max_gb, max_days;
} flow_location_config;

static void run_flow_config(void *ctx) {
    flow_location_config *c = ctx;
    host_facts h;
    host_facts_config(c->support, &h);
    c->unreadable = h.config_unreadable[0] != 0;
    c->location = h.config_location == HF_CONFIG_CHOSEN ? 1 : h.config_location == HF_CONFIG_WRONG ? 2 : 0;
    c->max_gb = h.config_max_gb;
    c->max_days = h.config_max_days;
    snprintf(c->dir, sizeof c->dir, "%s", h.config_dir);
    snprintf(c->id, sizeof c->id, "%s", h.config_id);
    host_facts_free(&h);
}

static int flow_config(nw_steps *s, uint64_t deadline, flow_location_config *c) {
    *c = (flow_location_config){0};
    if (!s->support || snprintf(c->support, sizeof c->support, "%s", s->support) >= (int)sizeof c->support) return 1;
    return native_support_read_bounded(run_flow_config, c, sizeof *c, deadline, c->support);
}

typedef struct {
    char support[NS_PATH_MAX], dir[NS_PATH_MAX], label[NL_DRIVE_MAX];
} flow_location_label;

static void run_flow_label(void *ctx) {
    flow_location_label *l = ctx;
    native_location_env le = {.support = l->support};
    native_location_drive_name(&le, l->dir, 0, l->label, sizeof l->label);
}

static void flow_remembered_label(const flow_location_config *c, uint64_t deadline, char label[NL_DRIVE_MAX]) {
    label[0] = 0;
    if (!c->dir[0]) return;
    flow_location_label l;
    memcpy(l.support, c->support, sizeof l.support);
    memcpy(l.dir, c->dir, sizeof l.dir);
    if (!native_support_read_bounded(run_flow_label, &l, sizeof l, deadline, l.support))
        memcpy(label, l.label, sizeof l.label);
}

/* SETTINGS (settingsFacts): config.ini's state now and Stems Cache's bridge */
static int settings_step(nw_steps *s, int config_only, core_facts *out) {
    uint64_t deadline = plat_now_mono_ns() + NL_WAIT_NS;
    flow_location_config c;
    int config_failed = flow_config(s, deadline, &c);
    if (config_failed == NL_TIMEOUT || config_failed == NL_BUSY) {
        core_fact_i64(out, FACT_STEP_CODE, WIN_LOCATION_TIMED_OUT);
        return 1;
    }
    native_settings_in in;
    memset(&in, 0, sizeof in);
    in.unreadable = config_failed || c.unreadable;
    in.location = c.location;
    in.dir = c.dir;
    in.id = c.id;
    in.max_gb = c.max_gb;
    in.max_days = c.max_days;
    int live = native_location_live();
    native_location l;
    char pc_identity[NS_CACHE_IDENTITY_MAX];
    /* review N3R18: startup and Use This PC only need the config and its remembered drive */
    if (!config_only && !in.unreadable) {
        int present = 1, says = 0;
        if (host_facts_live_bridge(compat_os_windows(), s->rb_parent, &present, &says)) present = 1, says = 0;
        in.can_choose = live && (!present || says);
        if (in.location != 2 && live && !location_now_until(s, &l, deadline)
            && native_location_matches_config(&l, in.dir, in.id)) in.snap = &l;
        if (in.location != 1 || (in.snap && in.snap->kind == LOC_BRIDGE_TOO_OLD))
            in.this_pc_folder = !native_pc_cache_identity_bounded(s->support, s->cache_default, deadline, pc_identity);
    }
    char remembered[NL_DRIVE_MAX];
    if (in.snap && in.snap->has_status_drive) in.drive = in.snap->status_drive;
    else {
        flow_remembered_label(&c, deadline, remembered);
        in.drive = remembered;
    }
    native_settings o;
    native_settings_facts(&in, &o);
    core_fact_i64(out, FACT_SET_LOCATION, o.location);
    core_fact_str(out, FACT_STEP_TEXT, o.drive, CORE_NTS);
    if (o.location == 1) core_fact_str(out, FACT_SET_LABEL_PATH, o.label_path, CORE_NTS);
    if (config_only) return config_failed != 0;
    core_fact_str(out, FACT_SET_GB, o.gb, CORE_NTS);
    core_fact_str(out, FACT_SET_DAYS, o.days, CORE_NTS);
    core_fact_flag(out, FACT_SET_UNREADABLE, o.unreadable);
    core_fact_flag(out, FACT_SET_CAN_CHOOSE, o.can_choose);
    core_fact_flag(out, FACT_STEP_FLAG, !live);
    core_fact_i64(out, FACT_SET_SIZE_KIND, o.size_kind);
    if (o.size_kind == 4 && in.snap->has_in_use) core_fact_str(out, FACT_LOC_IN_USE, in.snap->in_use, CORE_NTS);
    core_fact_i64(out, FACT_SET_CLEAR, o.clear);
    if (o.clear == 3) {   /* the chosen folder's gate (LocationGate: config.ini's dir and id) */
        core_fact_str(out, FACT_LOC_DIR, o.clear_target.dir, CORE_NTS);
        core_fact_str(out, FACT_LOC_GATE_ID, o.clear_target.gate_id, CORE_NTS);
        core_fact_str(out, FACT_LOC_DRIVE, o.clear_target.drive, CORE_NTS);
        core_fact_str(out, FACT_STEP_PATH, o.clear_target.dir, CORE_NTS);
        core_fact_str(out, FACT_STEP_TEXT2, o.clear_target.identity, CORE_NTS);
    } else if (o.clear == 1) {
        core_fact_str(out, FACT_STEP_TEXT2, pc_identity, CORE_NTS);
    }
    return 0;
}

/* LOCATION_NOW (uninstallEverything's and removeOwnFiles' read): the folder in use now, its drive
 * and gate; args[0] the core memo's folder in use: the size is the core's (4) when it is this one.
 * When the measure has no answer now (busy, past its wait), or the gate is off while config.ini
 * names a folder (set by hand), that folder with no gate, no size and FACT_LOC_MEASURED=0
 * (review NS11, NS12, N3R20). The uninstall says the drive couldn't be checked now. */
static int location_now_step(nw_steps *s, const char *const *args, int32_t nargs, core_facts *out) {
    native_location l;
    uint64_t deadline = plat_now_mono_ns() + NL_WAIT_NS;
    s->location_now_deadline = deadline;
    flow_location_config c;
    if (flow_config(s, deadline, &c)) {
        core_fact_flag(out, FACT_LOC_MEASURED, 0);
        return 1;
    }
    int measured = !location_now_until(s, &l, deadline), live = native_location_live();
    char named[NL_DRIVE_MAX] = "";
    int chosen = !c.unreadable && c.location == 1;
    /* The measurement's final config read must still match the pre-prompt snapshot. */
    if (live && measured && !native_location_matches_config(&l, c.dir, c.id)) measured = 0;
    if (chosen && (!live || !measured)) flow_remembered_label(&c, deadline, named);
    native_location_target o;
    int64_t size_kind;
    int rc = native_location_now_facts(live, measured ? &l : NULL, chosen ? c.dir : NULL, c.id, named,
                                      nargs >= 1 ? args[0] : NULL, &o, &size_kind);
    core_fact_flag(out, FACT_LOC_MEASURED, live && measured && !rc);   /* review N3R20 */
    if (!rc) {
        core_fact_str(out, FACT_LOC_DIR, o.dir, CORE_NTS);
        core_fact_str(out, FACT_LOC_DRIVE, o.drive, CORE_NTS);
        core_fact_str(out, FACT_STEP_TEXT2, o.dir[0] ? o.drive : "", CORE_NTS);
        /* Mount names only: this lookup opens neither the folder nor the volume (reviews N3R2, N3R16). */
        char shown[VOL_PATH_MAX + PATHS_LOCATION_MAX];
        core_fact_str(out, FACT_STEP_TEXT, vol_uninstall_path(o.dir, vol_display_path, shown, sizeof shown), CORE_NTS);
        core_fact_str(out, FACT_LOC_GATE_ID, o.gate_id, CORE_NTS);
        if (o.identity[0]) core_fact_str(out, FACT_STEP_PATH, o.identity, CORE_NTS);
        core_fact_i64(out, FACT_LOC_SIZE_KIND, size_kind);
    }
    return rc;
}

/* ---------------------------------------------------------------- the location choice (plan N3) */

/* config.ini's path for "Settings not saved" */
static void config_path_fact(nw_steps *s, core_facts *out) {
    char p[NS_PATH_MAX];
    if (s->support && snprintf(p, sizeof p, "%s\\config.ini", s->support) < (int)sizeof p) core_fact_str(out, FACT_STEP_PATH, p, CORE_NTS);
}

/* The bounded worker owns these paths and returns facts as values. The flow writes
 * core_facts and logs after a successful wait, while its core handle is still live. */
typedef struct {
    core_step kind;
    int rc;
    char path[NS_PATH_MAX], mount[NS_PATH_MAX], support[NS_PATH_MAX];
    char log[VOL_PATH_MAX + 64], old_drive[NL_DRIVE_MAX];
    char label[NL_DRIVE_MAX];
    int has_label;
    union {
        native_choice_probe probe;
        native_choice_found found;
        int64_t size;
        int writable;
    } out;
} location_choice_check;

static void choice_facts_log(void *ctx, const char *line) {
    location_choice_check *check = ctx;
    snprintf(check->log, sizeof check->log, "%s", line);
}

static int choice_volume(void *ctx, const char *folder, int *refusal, char *root, size_t rn, char *real, size_t n) {
    return nc_volume(ctx, folder, refusal, root, rn, real, n, choice_facts_log);
}

static void choice_check_drive(void *ctx, const char *dir, int remember, char *out, size_t n) {
    (void)remember;
    location_choice_check *check = ctx;
    native_location_env le = {.support = check->support, .ops = &nw_location_ops};
    check->has_label = native_location_picker_name(&le, dir, check->label, out, n);
}

static void run_location_choice_check(void *ctx) {
    location_choice_check *check = ctx;
    switch (check->kind) {
    case CORE_STEP_LOCATION_PROBE: {
        native_choice_ops o = {.ctx = check, .volume = choice_volume, .drive_name = choice_check_drive, .support = check->support};
        check->rc = native_choice_probe_folder(&o, check->path, &check->out.probe);
        break;
    }
    case CORE_STEP_LOCATION_DISCOVER: {
        native_choice_found *f = &check->out.found;
        check->rc = native_choice_discover(check->support, check->path, check->mount, f);
        if (!check->rc && f->current[0]) {
            native_location_env le = {.support = check->support};
            native_location_drive_name(&le, f->current, 0, check->old_drive, sizeof check->old_drive);
        }
        break;
    }
    case CORE_STEP_LOCATION_SIZE: check->out.size = native_choice_size(check->path); break;
    case CORE_STEP_LOCATION_WRITABLE: check->out.writable = native_choice_writable(check->path); break;
    default: check->rc = 1; break;
    }
}

static int location_choice_read_step(nw_steps *s, core_step kind, const char *path, const char *mount, core_facts *out) {
    if (kind == CORE_STEP_LOCATION_PROBE) {
        s->choice_deadline = plat_now_mono_ns() + NL_WAIT_NS;
        s->choice_has_label = 0;
        core_fact_str(out, FACT_STEP_TEXT2, path, CORE_NTS);
    }
    if (kind == CORE_STEP_LOCATION_SIZE && !strcmp(path, "this_mac")) path = s->cache_default;
    location_choice_check check = {.kind = kind};
    if (!path || !path[0] || snprintf(check.path, sizeof check.path, "%s", path) >= (int)sizeof check.path ||
        (s->support && snprintf(check.support, sizeof check.support, "%s", s->support) >= (int)sizeof check.support) ||
        (mount && snprintf(check.mount, sizeof check.mount, "%s", mount) >= (int)sizeof check.mount) ||
        native_choice_read(run_location_choice_check, &check, sizeof check, s->choice_deadline) || check.rc)
        return 1;
    if (kind == CORE_STEP_LOCATION_SIZE && check.out.size < 0) return 1;
    if (check.log[0]) native_core_log(s->c, check.log);
    switch (kind) {
    case CORE_STEP_LOCATION_PROBE: {
        native_choice_probe *p = &check.out.probe;
        if (!p->code && check.has_label) {
            s->choice_has_label = 1;
            memcpy(s->choice_mount, p->mount, sizeof s->choice_mount);
            memcpy(s->choice_label, check.label, sizeof s->choice_label);
        }
        core_fact_i64(out, FACT_STEP_CODE, p->code);
        if (p->support_conflict) core_fact_flag(out, FACT_STEP_FLAG, 1);
        core_fact_str(out, FACT_STEP_TEXT, p->real, CORE_NTS);
        core_fact_str(out, FACT_LOC_DRIVE, p->drive, CORE_NTS);
        core_fact_str(out, FACT_STEP_PATH, p->mount, CORE_NTS);
        break;
    }
    case CORE_STEP_LOCATION_DISCOVER: {
        native_choice_found *f = &check.out.found;
        core_fact_i64(out, FACT_STEP_CODE, f->code);
        if (f->support_conflict) core_fact_flag(out, FACT_STEP_FLAG, 1);
        if (f->code == 2) {
            core_fact_str(out, FACT_LOC_DIR, f->found, CORE_NTS);
            core_fact_str(out, FACT_LOC_GATE_ID, f->id, CORE_NTS);
        }
        if (f->code) core_fact_str(out, FACT_STEP_PATH, f->code == 2 ? f->identity : f->found, CORE_NTS);
        core_fact_str(out, FACT_STEP_TEXT, f->current, CORE_NTS);
        core_fact_str(out, FACT_STEP_TEXT2, f->current_id, CORE_NTS);
        core_fact_str(out, FACT_LOC_DRIVE, check.old_drive, CORE_NTS);
        break;
    }
    case CORE_STEP_LOCATION_SIZE: core_fact_i64(out, FACT_STEP_CODE, check.out.size); break;
    case CORE_STEP_LOCATION_WRITABLE: core_fact_flag(out, FACT_STEP_FLAG, check.out.writable); break;
    default: return 1;
    }
    return 0;
}

static int location_choice_step(nw_steps *s, const native_steps_env *e, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    const char *a0 = nargs >= 1 && args[0] ? args[0] : "";
    const native_choice_ops o = {.new_id = nc_new_id};
    switch (kind) {
    case CORE_STEP_LOCATION_PROBE:
    case CORE_STEP_LOCATION_SIZE:
    case CORE_STEP_LOCATION_WRITABLE:
        return location_choice_read_step(s, kind, a0, NULL, out);
    case CORE_STEP_LOCATION_DISCOVER:
        return nargs >= 2 && args[1] ? location_choice_read_step(s, kind, a0, args[1], out) : 1;
    case CORE_STEP_LOCATION_CREATE: {
        native_choice_commit c = {.create = 1};
        s->choice_deadline = plat_now_mono_ns() + NL_WAIT_NS;
        if (!a0[0] || !s->support || snprintf(c.path, sizeof c.path, "%s", a0) >= (int)sizeof c.path ||
            snprintf(c.support, sizeof c.support, "%s", s->support) >= (int)sizeof c.support) return 1;
        core_fact_str(out, FACT_STEP_PATH, a0, CORE_NTS);
        int r = native_choice_commit_run(&c, &o, NULL, s->choice_deadline);
        if (r) {
            if (c.left_folder) core_fact_flag(out, FACT_STEP_FLAG, 1);
            if (r == NS_CHOICE_CREATE_LEFT_FOLDER) {
                core_fact_flag(out, FACT_STEP_FLAG, 1);
                r = 3;
            }
            core_fact_i64(out, FACT_STEP_CODE, r);
            if (r != 1) core_fact_str(out, FACT_STEP_TEXT, c.err, CORE_NTS);
            return 1;
        }
        core_fact_str(out, FACT_LOC_DIR, c.dir, CORE_NTS);
        core_fact_str(out, FACT_LOC_GATE_ID, c.id, CORE_NTS);
        core_fact_str(out, FACT_STEP_TEXT2, c.identity, CORE_NTS);
        return 0;
    }
    case CORE_STEP_LOCATION_WRITE: {
        if (nargs < 4 || !args[0] || !args[1] || !args[2] || !args[3]) {
            config_path_fact(s, out);
            return 1;
        }
        native_choice_commit c = {.created = !strcmp(args[3], "1")};
        if (!c.created) s->choice_deadline = plat_now_mono_ns() + NL_WAIT_NS;
        if (!s->support || snprintf(c.support, sizeof c.support, "%s", s->support) >= (int)sizeof c.support ||
            snprintf(c.path, sizeof c.path, "%s", args[0]) >= (int)sizeof c.path ||
            snprintf(c.id, sizeof c.id, "%s", args[1]) >= (int)sizeof c.id ||
            snprintf(c.mount, sizeof c.mount, "%s", args[2]) >= (int)sizeof c.mount ||
            (nargs >= 5 && args[4] && snprintf(c.identity, sizeof c.identity, "%s", args[4]) >= (int)sizeof c.identity) ||
            (s->cache_default && snprintf(c.cache_default, sizeof c.cache_default, "%s", s->cache_default) >= (int)sizeof c.cache_default)) {
            config_path_fact(s, out);
            return 1;
        }
        c.has_label = s->choice_has_label && !strcmp(c.mount, s->choice_mount);
        if (c.has_label) memcpy(c.label, s->choice_label, sizeof c.label);
        int r = native_choice_commit_run(&c, &o, NULL, s->choice_deadline);
        if (r == NS_CHOICE_SAVED_UNCHECKED) core_fact_i64(out, FACT_STEP_CODE, r);
        else if (r) {
            if (r == 2 || r == NS_CHOICE_TIMED_OUT) core_fact_i64(out, FACT_STEP_CODE, r);
            else config_path_fact(s, out);
            if (c.left_folder) core_fact_flag(out, FACT_STEP_FLAG, 1);
            return 1;
        }
        core_fact_str(out, FACT_STEP_TEXT, args[0], CORE_NTS);
        return 0;
    }
    case CORE_STEP_THIS_MAC_DELETE: {
        if (!strcmp(a0, "check")) {
            int present = 1, says = 0;   /* no answer: as an older Stems Cache (the flag, then the launch retry) */
            if (host_facts_live_bridge(compat_os_windows(), s->rb_parent, &present, &says)) present = 1, says = 0;
            core_fact_flag(out, FACT_STEP_FLAG, !present || says);
            return 0;
        }
        if (!strcmp(a0, "target")) {
            char identity[NS_CACHE_IDENTITY_MAX];
            uint64_t deadline = nargs >= 2 && args[1] && !strcmp(args[1], "choice") ? s->choice_deadline : s->location_now_deadline;
            if (!deadline) deadline = plat_now_mono_ns() + NL_WAIT_NS;
            if (!native_pc_cache_identity_bounded(s->support, s->cache_default, deadline, identity))
                core_fact_str(out, FACT_STEP_PATH, identity, CORE_NTS);
            return 0;
        }
        const char *identity = nargs >= 2 && args[1] ? args[1] : "";
        if (!strcmp(a0, "delete")) return native_this_pc_remove_stems(e, s->cache_default, identity);
        if (!strcmp(a0, "defer")) return !identity[0] || native_support_write(s->support, NS_PENDING_DELETION, identity, strlen(identity));
        if (!strcmp(a0, "unflag")) return native_support_remove(s->support, NS_PENDING_DELETION) ? 1 : 0;
        if (strcmp(a0, "facts")) return 1;
        /* retryThisMacDeletion's inputs: LocationMonitor.current() is the measure now (bounded) */
        native_location l;
        if (location_now(s, &l)) return 1;   /* no answer: nothing decided this start */
        int present = 1, says = 0;
        if (host_facts_live_bridge(compat_os_windows(), s->rb_parent, &present, &says)) return 1;
        char pending[NS_PATH_MAX], *saved = NULL;
        size_t size;
        if (s->support && snprintf(pending, sizeof pending, "%s\\%s", s->support, NS_PENDING_DELETION) < (int)sizeof pending &&
            !plat_read_file(pending, NS_CACHE_IDENTITY_MAX - 1, &saved, &size) && size && !memchr(saved, 0, size)) {
            core_fact_flag(out, FACT_STEP_FLAG, 1);
            core_fact_str(out, FACT_STEP_PATH, saved, (int32_t)size);
        }
        free(saved);
        core_fact_flag(out, FACT_STEP_FLAG2, present && says);
        core_fact_i64(out, FACT_STEP_CODE, l.kind == LOC_CHOSEN && l.probe == PROBE_AVAILABLE ? 1 : 0);
        core_fact_str(out, FACT_STEP_TEXT, native_location_code(l.kind, l.probe), CORE_NTS);
        return 0;
    }
    default: return CORE_E_STATE;
    }
}

/* DATE_TEXT: a YYYY-MM-DD date in the user's long date format (GetDateFormatEx: Windows has no
 * medium style; the long one names the month, as the Mac's medium does) */
static int date_text(const char *ymd, core_facts *out) {
    int y, m, d;
    char rest;
    if (!ymd || sscanf(ymd, "%4d-%2d-%2d%c", &y, &m, &d, &rest) != 3 || y < 1601 || y > 30827 || m < 1 || m > 12 || d < 1 || d > 31) return 1;
    SYSTEMTIME t;
    memset(&t, 0, sizeof t);
    t.wYear = (WORD)y;
    t.wMonth = (WORD)m;
    t.wDay = (WORD)d;
    wchar_t w[128];
    int k = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &t, NULL, w, 128, NULL);
    char u[400];
    if (k < 2 || wr_utf16_to_8((const uint16_t *)w, (size_t)(k - 1), u, sizeof u) <= 0) return 1;
    core_fact_str(out, FACT_STEP_TEXT, u, CORE_NTS);
    return 0;
}

/* ---------------------------------------------------------------- Create Report (REPORT_FACTS, REPORT_ZIP) */

/* the model's sha256 now ("" none, "unreadable"), uncached as reportMeasure's */
static void model_sha_now(nw_steps *s, char out[65 + 16]) {
    plat_stat st;
    if (!s->model || plat_stat_path(s->model, 1, &st)) snprintf(out, 65 + 16, "missing");
    else if (sha_file(s->model, out)) snprintf(out, 65 + 16, "unreadable");
}

/* REPORT_FACTS (reportLights of reportMeasure): the window's cheap facts read now (host_facts.c's,
 * the refresh's own rows), the model's sha256, and where the stems are with its floor */
static int report_facts_step(nw_steps *s, core_facts *out) {
    s->report_measured = 0;
    host_facts h;
    if (host_facts_read_core(s->c, &h)) {
        host_facts_free(&h);
        return 1;
    }
    core_fact_flag(out, FACT_STEMS_PLUS_PRESENT, h.plus_present);
    core_fact_flag(out, FACT_STEMS_CACHE_PRESENT, h.cache_present);
    core_fact_flag(out, FACT_CHOSEN_CACHE, h.chosen_cache);
    core_fact_flag(out, FACT_CONFIG_ENABLED, h.config_enabled);
    core_fact_flag(out, FACT_CONFIG_RB_MODEL, h.config_rb_model);
    core_fact_str(out, FACT_ENGINE, h.engine, CORE_NTS);
    if (h.has_bridge_models) core_fact_str(out, FACT_BRIDGE_MODELS, h.bridge_models, CORE_NTS);
    int live = native_location_live();
    if (live) core_fact_i64(out, FACT_CONFIG_LOCATION, h.config_location);
    char sha[65 + 16];
    model_sha_now(s, sha);
    core_fact_str(out, FACT_STEP_TEXT, strlen(sha) == 64 ? sha : "", CORE_NTS);
    native_location *l = &s->report_location;
    s->report_measured = !location_now(s, l) && native_location_matches_config(l, h.config_dir, h.config_id);
    if (s->report_measured && live) nw_location_write(l, out);   /* the location light, as the window's */
    host_facts_free(&h);
    return 0;
}

static int report_events(void *ctx, char **out, size_t *len) {
    (void)ctx;
    return report_event_log(REPORT_EVENT_MINUTES, REPORT_EVENT_LINES, REPORT_TEXT_MAX, out, len);
}
static void report_say(void *ctx, const char *line) { native_core_log(((nw_steps *)ctx)->c, line); }

/* a folder's names, sorted, one space between ("" none or unreadable); recursive: their own names
 * too, as "dir/name", at most max bytes */
typedef struct {
    char **v;
    int n, cap;
} names_list;
static int names_add(void *ctx, const char *name, const plat_stat *st) {
    (void)st;
    names_list *l = ctx;
    if (l->n == l->cap) {
        int c = l->cap ? l->cap * 2 : 32;
        char **q = realloc(l->v, sizeof *q * (size_t)c);
        if (!q) return 1;
        l->v = q;
        l->cap = c;
    }
    l->v[l->n] = _strdup(name);
    if (l->v[l->n]) l->n++;
    return 0;
}
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void folder_names(const char *dir, int depth, const char *prefix, char *out, size_t max) {
    plat_dir d;
    if (!dir || plat_dir_open(dir, &d)) return;
    names_list l = {0};
    plat_dir_each(d, names_add, &l);
    plat_dir_close(d);
    qsort(l.v, (size_t)l.n, sizeof *l.v, name_cmp);
    for (int i = 0; i < l.n; i++) {
        size_t k = strlen(out);
        if (k + strlen(prefix) + strlen(l.v[i]) + 3 < max) snprintf(out + k, max - k, "%s%s%s", k ? " " : "", prefix, l.v[i]);
        char sub[NS_PATH_MAX], pre[NS_PATH_MAX];
        plat_stat st;
        if (depth > 0 && snprintf(sub, sizeof sub, "%s\\%s", dir, l.v[i]) < (int)sizeof sub && !plat_stat_path(sub, 0, &st) && st.kind == PLAT_DIR
            && snprintf(pre, sizeof pre, "%s%s/", prefix, l.v[i]) < (int)sizeof pre)
            folder_names(sub, depth - 1, pre, out, max);
        free(l.v[i]);
    }
    free(l.v);
}

static const char *yn(int b) { return b ? "yes" : "no"; }

static void sha12(const char *sha, char out[16]) {
    if (strlen(sha) == 64) snprintf(out, 16, "%.12s", sha);
    else snprintf(out, 16, "%s", sha[0] ? sha : "missing");
}

/* reportFacts: Windows' lines, most important first (Report.swift's order: the app and its two
 * lights, the PC, rekordbox, what is installed and chosen, the disk, the watcher and the last
 * update check, R1; then the rest). Never the library, its key, a kept set or a track */
static void report_lines(nw_steps *s, const char *const *args, const native_location *measured_location,
                         const native_location_details *details, report_facts *f) {
    host_facts h;
    int have = !host_facts_read_core(s->c, &h);
    const char *plus_look = args[0], *plus_tip = args[1], *cache_look = args[2], *cache_tip = args[3];
    int cache_is_location = !strcmp(args[4], "1");
    const native_location *l = measured_location;
    const char *code = !l ? "unknown" : l->kind == LOC_THIS_MAC ? "thisPC" : l->kind == LOC_CHOSEN ? "chosen" : l->kind == LOC_BRIDGE_TOO_OLD ? "bridgeTooOld"
                                                                               : l->kind == LOC_CONFIG_UNREADABLE ? "configUnreadable" : "configMalformed";
    char loc_code[160];
    snprintf(loc_code, sizeof loc_code, "%s%s%s%s%s%s", code, l && l->has_bridge ? " (bridge " : "", l && l->has_bridge ? l->bridge_state : "",
             l && l->has_bridge && l->bridge_reason[0] ? "/" : "", l && l->has_bridge ? l->bridge_reason : "", l && l->has_bridge ? ")" : "");
    /* the app and its lights; the location's text names a drive: in the link, its state's code */
    char shown[PAYLOAD_VERSION_MAX] = "", p[NS_PATH_MAX], *t = NULL;
    size_t tn = 0;
    update_shown sh;
    if (s->support && snprintf(p, sizeof p, "%s\\shown-version", s->support) < (int)sizeof p && !plat_read_file(p, 4096, &t, &tn)
        && !update_shown_parse(t, tn, &sh))
        snprintf(shown, sizeof shown, "%s", sh.shown);
    free(t);
    const char *own = s->own ? s->own : "?";
    char version[200];
    if (!shown[0] || !strcmp(shown, own)) snprintf(version, sizeof version, "%s (%s)", own, s->ue.arch);
    else snprintf(version, sizeof version, "%s (Windows app %s, %s)", shown, own, s->ue.arch);
    report_fact_add(f, 1, "RB Stems Plus: %s; lights: Demucs v4 %s (%s), Stems Cache %s (%s%s)", version, report_light_colour(plus_look), plus_tip,
                    report_light_colour(cache_look), cache_is_location ? "location: " : "", cache_is_location ? loc_code : cache_tip);
    if (cache_is_location) report_fact_add(f, 0, "Stems Cache light: %s", cache_tip);
    uint32_t ma = 0, mi = 0, build = 0;
    char locale[96];
    report_locale(locale, sizeof locale);
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char *arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64" : "other";
    if (life_os_version(&ma, &mi, &build)) ma = mi = build = 0;
    report_fact_add(f, 1, "Windows: %u.%u build %u; PC: %s, %llu GB memory; language: %s", ma, mi, build, arch,
                    (unsigned long long)(report_memory() / 1073741824ull), locale[0] ? locale : "?");
    /* rekordbox */
    compat_procs pr;
    int running = !compat_processes(compat_os_windows(), NULL, &pr) && pr.running;
    report_fact_add(f, 1, "rekordbox: %s, running: %s; STEMS Engine: %s", have && h.installed ? h.version : "not installed", yn(running),
                    have && h.engine[0] ? h.engine : "not downloaded");
    char msha[65 + 16], m12[16];
    model_sha_now(s, msha);
    sha12(msha, m12);
    char size[48] = "";
    if (have && h.has_model) snprintf(size, sizeof size, " (%lld bytes)", (long long)h.model_size);
    report_fact_add(f, 1, "installed: Demucs v4 %s, Stems Cache %s; chosen: Demucs v4 %s, Stems Cache %s; model in rekordbox: %s%s", yn(have && h.plus_present),
                    yn(have && h.cache_present), yn(have && h.chosen_model), yn(have && h.chosen_cache), m12, size);
    /* the disk of the folder in use */
    if (!details->has_disk) report_fact_add(f, 1, "disk: unknown; Stems Cache's floor: unknown");
    else {
        char gfree[32], gdisk[32];
        int64_t avail = (int64_t)details->available, disk = details->total ? (int64_t)details->total : -1;
        int64_t floor = state_free_floor(disk > 0 ? disk : 0);
        report_gb(avail > 0 ? avail : -1, gfree, sizeof gfree);
        report_gb(disk, gdisk, sizeof gdisk);
        report_fact_add(f, 1, "disk: %s free of %s; Stems Cache's floor: %lld GB%s", gfree, gdisk, (long long)((floor + 500000000) / 1000000000),
                        avail > 0 && avail < floor ? ", under it: saving paused" : "");
    }
    /* the watcher and the last update check */
    wl_ctx w;
    native_watcher_ops ops;
    int present = 0, ours = 0, off = 0, running_w = -1;
    if (!watcher_ops(s, &w, &ops)) {
        if (wl_read(&w, &present, &ours, &off)) present = -1;
        running_w = wl_running(&w);
    }
    payload_manifest *m = malloc(sizeof *m);
    int has_m = m && e_saved(s, m);
    char ago[32];
    report_ago(e_age(s), ago, sizeof ago);
    report_fact_add(f, 1, "watcher: %s; payload: %s; last update check: %s",
                    present < 0 ? "unknown" : !present ? "not installed" : !ours ? "installed, starting something else" : off ? "installed, turned off in Startup apps"
                                       : running_w == 1 ? "installed, running" : running_w == 0 ? "installed, not running" : "installed",
                    has_m ? m->payload_version : "none", ago);
    /* R1: rekordbox's onnxruntime.dll, what the window's Stems Cache light checks first */
    char folder[COMPAT_PATH_MAX];
    int r1 = nw_steps_rekordbox_folder(s, folder, sizeof folder) ? nw_admin_r1(s->admin, folder) : -1;
    static const char *const r1_names[] = {"ok", "live DLL missing", "live DLL not Pioneer's or ours", "Pioneer's copy missing"};
    report_fact_add(f, 1, "R1 (rekordbox's onnxruntime.dll): %s", r1 >= 0 && r1 <= 3 ? r1_names[r1] : "unknown");
    /* then the rest, still in the link while it fits */
    blocked_read br;
    blocked_facts(s, 1, 1, &br);
    report_fact_add(f, 1, "can install: Demucs v4: engine %s %s; Stems Cache: rekordbox %s, Pioneer's ONNX Runtime %s", br.engine[0] ? br.engine : "none",
                    br.plus_r ? "unknown" : br.engine_ok ? "supported" : "not supported", br.cache_r ? "unknown" : br.b.rekordbox_supported ? "supported" : "not supported",
                    br.cache_r ? "unknown" : br.b.ort_listed ? "listed" : br.b.lib_exists ? "not listed" : "missing");
    if (has_m) {
        char ms[16], bs[16];
        sha12(m->model.sha256, ms);
        sha12(m->win_bridge.sha256, bs);
        report_fact_add(f, 1, "payload.json: payload %s, Windows app %s, model %s, bridge %s", m->payload_version, m->win_app.version, ms, bs);
        char rbs[600] = "", engines[600] = "";
        for (int i = 0; i < m->n_win_rekordbox; i++) snprintf(rbs + strlen(rbs), sizeof rbs - strlen(rbs), "%s%s", i ? " " : "", m->win_rekordbox[i]);
        for (int i = 0; i < m->n_win_stems_engine; i++)
            snprintf(engines + strlen(engines), sizeof engines - strlen(engines), "%s%s", i ? " " : "", m->win_stems_engine[i]);
        report_fact_add(f, 1, "compatible: rekordbox %s, ONNX Runtime %d listed, STEMS Engine %s", rbs, m->n_win_ort_sha256, engines);
    }
    free(m);
    if (have) {
        char gb[32], days[32];
        fmt_config_number(h.config_max_gb, gb);
        fmt_config_number(h.config_max_days, days);
        report_fact_add(f, 1, "settings: enabled=%d max_gb=%s max_days=%s%s%s", h.config_enabled, gb, days, h.config_unreadable[0] ? "; config.ini unreadable: " : "",
                        h.config_unreadable);
    }
    report_fact_add(f, 1, "saved stems location: %s", loc_code);
    if (have && h.config_location == HF_CONFIG_CHOSEN) report_fact_add(f, 0, "saved stems folder: %s", h.config_dir);
    report_fact_add(f, 1, "administrator: %s; app at %s\\%s%s", yn(native_user_is_admin() == 1), s->apps ? s->apps : "?", own,
                    update_in_place(&s->ue) ? " (not its install)" : "");
    char originals[NS_PATH_MAX], list[4096] = "";
    if (s->support && snprintf(originals, sizeof originals, "%s\\originals", s->support) < (int)sizeof originals) folder_names(originals, 0, "", list, sizeof list);
    report_fact_add(f, 1, "Pioneer's models saved: %s", list[0] ? list : "none");
    char ort[65], ort12[16], copy[65] = "", copy12[16], rp[NS_PATH_MAX];
    e_ort_sha(s, ort);
    sha12(ort, ort12);
    if (snprintf(rp, sizeof rp, "%s\\ort\\onnxruntime.dll", s->root) < (int)sizeof rp) memo_sha(s, rp, copy);
    sha12(copy, copy12);
    report_fact_add(f, 1, "ONNX Runtime in rekordbox: %s, version %s; copy for the bridge: %s", ort12, br.b.ort_version[0] ? br.b.ort_version : "?", copy12);
    /* only in summary.txt */
    report_fact_add(f, 0, "model in rekordbox, full: %s", msha);
    report_fact_add(f, 0, "ONNX Runtime in rekordbox, full: %s; copy for the bridge: %s", ort[0] ? ort : "missing", copy[0] ? copy : "missing");
    char payloads[NS_PATH_MAX], root_list[8192] = "";
    list[0] = 0;
    if (s->support && snprintf(payloads, sizeof payloads, "%s\\payload", s->support) < (int)sizeof payloads) folder_names(payloads, 0, "", list, sizeof list);
    report_fact_add(f, 0, "payload folders: %s", list);
    folder_names(s->root, 2, "", root_list, sizeof root_list);
    report_fact_add(f, 0, "root folder: %s", root_list);
    host_facts_free(&h);   /* a read that failed part way may hold config.ini's bytes */
}

/* REPORT_ZIP (buildReport with the lights the core decided; issueURL) */
static int report_zip_step(nw_steps *s, const char *const *args, int32_t nargs, core_facts *out) {
    if (nargs < 5 || !s->support) return 1;
    host_facts h;
    host_facts_config(s->support, &h);
    const char *dir = h.config_location == HF_CONFIG_CHOSEN ? h.config_dir : "";
    const native_location *snap = s->report_measured ? &s->report_location : NULL;
    s->report_measured = 0;
    const native_location *current = snap && native_location_matches_config(snap, dir, h.config_id) ? snap : NULL;
    native_location_env le = {.support = s->support};
    native_location_details details;
    native_location_describe(&le, current, dir, h.config_id, &details);
    report_facts f = {0};
    report_lines(s, args, current, &details, &f);
    if (f.err) {
        host_facts_free(&h);
        report_facts_free(&f);
        return 1;
    }
    report_mask_names mn;
    memset(&mn, 0, sizeof mn);
    report_profile(mn.home, sizeof mn.home);
    report_short_profile(mn.home, mn.short_home, sizeof mn.short_home);
    report_user_name(mn.user, sizeof mn.user);
    report_full_name(mn.user, mn.full, sizeof mn.full);
    const char *last = strrchr(mn.home, '\\');
    if (last) snprintf(mn.folder_name, sizeof mn.folder_name, "%s", last + 1);
    char logs[NS_PATH_MAX], fallback[NS_PATH_MAX], desktop[NS_PATH_MAX] = "";
    char roots_buf[REPORT_WER_ROOTS][REPORT_PATH_MAX];
    const char *roots[REPORT_WER_ROOTS];
    int nroots = report_wer_roots(roots_buf);
    for (int i = 0; i < nroots; i++) roots[i] = roots_buf[i];
    snprintf(logs, sizeof logs, "%s\\Logs", s->support);
    snprintf(fallback, sizeof fallback, "%s\\Logs\\reports", s->support);
    report_desktop(desktop, sizeof desktop);
    const char *names[3];
    int nnames = 0;
    if (details.drive[0]) names[nnames++] = details.drive;
    /* The lights and log can still contain names from the snapshot if config changed. */
    if (snap && snap->has_status_drive && snap->status_drive[0] && strcmp(snap->status_drive, details.drive)) names[nnames++] = snap->status_drive;
    if (snap && snap->has_bridge_drive && snap->bridge_drive[0]) names[nnames++] = snap->bridge_drive;
    report_env e;
    memset(&e, 0, sizeof e);
    e.ctx = &mn;
    e.mask = report_mask_with;
    e.events = report_events;
    e.support = s->support;
    e.logs = logs;
    e.wer_roots = roots;
    e.nwer = nroots;
    e.desktop = desktop;
    e.fallback = fallback;
    e.facts = &f;
    e.location_dir = dir;
    e.sysdrive = report_system_drive();
    e.drive_names = names;
    e.ndrive_names = nnames;
    e.now = plat_now_wall_s();
    e.log = report_say;
    e.log_ctx = s;
    report_result r;
    int rc = report_build(&e, &r);
    if (rc) {
        char b[96];
        snprintf(b, sizeof b, "report: not made (error %d)", rc);
        report_say(s, b);
    }
    host_facts_free(&h);
    report_facts_free(&f);
    if (rc) return 1;
    size_t ml = 0;
    char *shown = report_mask_with(&mn, r.path, strlen(r.path), NULL, 0, &ml);
    char *url = report_issue_url(r.title, r.details);
    core_fact_str(out, FACT_STEP_PATH, r.path, CORE_NTS);
    core_fact_str(out, FACT_STEP_TEXT, shown ? shown : r.path, CORE_NTS);
    core_fact_str(out, FACT_STEP_URL, url ? url : "", CORE_NTS);
    free(shown);
    free(url);
    report_result_free(&r);
    return 0;
}

/* ---------------------------------------------------------------- the entry */

static int scope_version(char *out, size_t cap, size_t *at, const char *version) {
    size_t n = strlen(version);
    if (*at + n + 1 >= cap) return 0;
    memcpy(out + *at, version, n);
    *at += n;
    out[(*at)++] = '\n';
    out[*at] = 0;
    return 1;
}

static int win_scope(nw_steps *s, const char *verb, core_facts *out) {
    int sweep = !strcmp(verb, "uninstall");
    int kind = sweep ? INV_VERB_UNINSTALL : INV_VERB_INSTALL;
    if (!strcmp(verb, "reinstall")) kind = INV_VERB_REINSTALL;
    else if (!strcmp(verb, "restore")) kind = INV_VERB_RESTORE;
    else if (!sweep && strcmp(verb, "install")) return 1;

    host_facts *h = malloc(sizeof *h);
    if (!h) return 1;
    payload_manifest *flow = NULL;
    /* install's token ends in "@<sha256>" of the flow payload's bytes, the scope's source, which
     * the worker's --manifest must be (re-review finding 6; native_admin_targets) */
    payload_sha from = "";
    if (!sweep && kind != INV_VERB_RESTORE) {
        flow = malloc(sizeof *flow);
        if (!flow) { free(h); return 1; }
        plat_mutex_lock(&s->flow.mu);
        int has = s->flow.has && s->flow.m.has_windows && payload_is_sha256(s->flow.m.json_sha256);
        if (has) *flow = s->flow.m;
        plat_mutex_unlock(&s->flow.mu);
        if (!has) { free(flow); free(h); return 1; }
        memcpy(from, flow->json_sha256, sizeof from);
    }
    int r = host_facts_read_scope(s->c, flow, h);
    free(flow);
    if (r || !h->inventory) {
        free_rb_coverage(h);
        return 1;
    }
    int incomplete = !h->inventory->complete;
    admin_target targets[INV_MAX];
    char token[ADMIN_TARGETS_MAX * (ADMIN_VERSION_MAX + 17) + 66] = "";
    char versions[INV_MAX * COMPAT_VERSION_MAX + 1] = "";
    size_t at = 0;
    int n = inv_targets(kind, h->inventory, h->assessment, h->cache_manifest, targets, INV_MAX);
    if (sweep) {
        /* the worker sweeps the disk ("*"); its affected versions are the rule's. An incomplete
         * inventory still allows putting Pioneer's file back where our bridge and its receipt
         * are (D3: the root stays); with none of those, nothing may be changed now */
        strcpy(token, "*");
        if (n > 0) incomplete = 0;
        for (int i = 0; i < n; i++) scope_version(versions, sizeof versions, &at, targets[i].version);
    } else if (n < 0 || n > ADMIN_TARGETS_MAX || (n && admin_targets_format(targets, n, token, sizeof token - 66) < 0)) incomplete = 1;
    else {
        for (int i = 0; i < n; i++) scope_version(versions, sizeof versions, &at, targets[i].version);
        if (n && from[0]) snprintf(token + strlen(token), sizeof token - strlen(token), "@%s", from);
    }
    core_fact_i64(out, FACT_STEP_CODE, incomplete);
    core_fact_str(out, FACT_WIN_SCOPE, incomplete ? "" : token, CORE_NTS);
    core_fact_str(out, FACT_WIN_SCOPE_VERSIONS, sweep ? "" : versions, CORE_NTS);
    core_fact_str(out, FACT_WIN_SCOPE_AFFECTED, sweep || kind == INV_VERB_RESTORE ? versions : "", CORE_NTS);
    free_rb_coverage(h);
    return 0;
}

static int win_admin_targets(nw_steps *s, core_facts *out) {
    admin_target_result targets[ADMIN_TARGETS_MAX];
    int root_kept = 0;
    int n = nw_admin_last_targets(s->admin, targets, ADMIN_TARGETS_MAX, &root_kept);
    if (n < 0) return 1;
    char rows[ADMIN_TARGETS_MAX * (ADMIN_VERSION_MAX + ADMIN_TARGET_WHAT_MAX + 16) + 1] = "";
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        const admin_target_result *t = &targets[i];
        char code[16] = "-";
        if (t->code >= 0) snprintf(code, sizeof code, "%d", t->code);
        int k = snprintf(rows + at, sizeof rows - at, "%s\t%s\t%s\n", t->version, code, t->what);
        if (k < 0 || (size_t)k >= sizeof rows - at) return 1;
        at += (size_t)k;
    }
    core_fact_str(out, FACT_STEP_TEXT, rows, at);
    core_fact_flag(out, FACT_STEP_FLAG, root_kept);
    return 0;
}

static void open_versions(nw_steps *s, int open, core_facts *out) {
    char versions[INV_MAX * COMPAT_VERSION_MAX + 1] = "";
    if (open) {
        inv_list *l = malloc(sizeof *l);
        inv_assess_row a[INV_MAX] = {0};
        const compat_os *os = compat_os_windows();
        if (l && !host_facts_inventory(os, s->rb_parent, l)) native_inventory_running(os, l, a, versions, sizeof versions);
        free(l);
    }
    core_fact_str(out, FACT_WIN_RB_OPEN_VERSIONS, versions, CORE_NTS);
}

int32_t nw_steps_run(nw_steps *s, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    native_steps_env e;
    env_of(s, &e);
    switch (kind) {
    case CORE_STEP_WIN_R1_DISMISS: return r1_dismiss(s, nargs >= 1 ? args[0] : NULL, out);
    case CORE_STEP_WIN_SCOPE: return nargs >= 1 ? win_scope(s, args[0], out) : 1;
    case CORE_STEP_WIN_ADMIN_TARGETS: return win_admin_targets(s, out);
    case CORE_STEP_PAYLOAD: {
        native_payload_facts f;
        if (nargs < 1) return 1;
        int r = !strcmp(args[0], "refresh-compat") ? refresh_compat_payload(s, &e, &f) : native_step_payload(&e, &s->flow, args[0], &f);
        if (r) return 1;
        /* The fetch can change the saved payload, and update_seen can write installed-release. */
        refresh_rb_proved(s);
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
        open_versions(s, open, out);
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
        core_fact_str(out, FACT_REKORDBOX_VERSION, r.version, CORE_NTS);
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
        else if (!strcmp(args[1], "1")) native_pc_cache_create(&e, s->cache_default);
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
        /* Refresh Prepare's snapshot on the flow queue even when there is no reinstall request. */
        if (nargs == 0) {
            char folder[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX];
            current_folder(s, folder, version);
        }
        int64_t age = native_support_age(s->support, "request", plat_now_wall_s());
        if (age >= 0 && nargs >= 1 && !strcmp(args[0], "take")) {
            int r = native_support_remove(s->support, "request");
            if (r) log_failed(s, "remove request", r);
        } else if (age >= 0 && nargs == 0) {
            char path[NS_PATH_MAX], *request = NULL;
            size_t n = 0;
            if (snprintf(path, sizeof path, "%s\\request", s->support) < (int)sizeof path &&
                !plat_read_file(path, 32, &request, &n) && !memchr(request, 0, n))
                core_fact_str(out, FACT_STEP_TEXT, request, n);
            else core_fact_str(out, FACT_STEP_TEXT, "?", CORE_NTS);
            free(request);
        }
        core_fact_i64(out, FACT_STEP_CODE, age);
        return 0;
    }
    case CORE_STEP_BRIDGE_SHA: {
        if (!nargs) {   /* the original step contract for hosts querying the current folder */
            char folder[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX], p[COMPAT_PATH_MAX + 32], sha[65] = "";
            int found = current_folder(s, folder, version);
            if ((found || s->rekordbox) && snprintf(p, sizeof p, "%s\\onnxruntime.dll", folder) < (int)sizeof p) memo_sha(s, p, sha);
            core_fact_str(out, FACT_STEP_TEXT, sha, CORE_NTS);
            return 0;
        }
        if (nargs != 2 || strcmp(args[0], "outdated")) return 1;
        host_facts *h = read_rb_coverage(s);
        const char *sha = h && h->inventory && h->cache_manifest && !strcmp(h->cache_manifest->win_bridge.sha256, args[1])
            ? inv_outdated_bridge(h->inventory, h->assessment, args[1]) : "";
        core_fact_str(out, FACT_STEP_TEXT, sha, CORE_NTS);
        free_rb_coverage(h);
        return 0;
    }
    case CORE_STEP_STATE_LINE: {
        char line[INV_STATE_TEXT_MAX + 256];
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
    case CORE_STEP_REMOVE_OWN_FOLDERS: {
        int r = 0;
        if (nargs >= 2 && args[1] && args[1][0] && s->expected_support[0] && s->support &&
            !_stricmp(s->expected_support, s->support)) {
            int link;
            r = native_clear_cache(&e, s->cache_default, args[1], 1, &link);
        }
        r |= native_remove_own_folders(&e, s->expected_support, nargs >= 1 && !strcmp(args[0], "1"));
        refresh_rb_proved(s);   /* payload\ and installed-release\ may be gone, even in part: a failed read publishes 0 (review RQ3) */
        return r;
    }
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
        int r = nargs == 4 ? native_remove_location(&e, args[0], args[1], !strcmp(args[2], "1"), name, err, args[3]) : 6;
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
        int link, r = native_clear_cache(&e, s->cache_default, nargs >= 1 ? args[0] : NULL, 0, &link);
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
    case CORE_STEP_LAUNCH_AGENT_LOAD:
    case CORE_STEP_WATCHER_CHECK:
    case CORE_STEP_WATCHER_STATE: return watcher_step(s, kind, out);
    case CORE_STEP_SETTINGS: return settings_step(s, nargs >= 1 && args[0] && !strcmp(args[0], "config"), out);
    case CORE_STEP_LOCATION_NOW: return location_now_step(s, args, nargs, out);
    case CORE_STEP_PAUSED: {
        /* asked only while the core keeps no location memo of its own */
        native_location l;
        if (location_now(s, &l) || !native_location_checked(&l)) return 1;
        core_fact_i64(out, FACT_STEP_CODE, l.paused_floor);
        core_fact_str(out, FACT_STEP_TEXT, l.kind == LOC_CHOSEN && l.has_status_drive ? l.status_drive : "", CORE_NTS);
        return 0;
    }
    case CORE_STEP_LOCATION_REMEASURE: return 0;   /* no host monitor on Windows: the core's memo remeasures (EF_REMEASURE) */
    case CORE_STEP_LOCATION_PROBE:
    case CORE_STEP_LOCATION_DISCOVER:
    case CORE_STEP_LOCATION_SIZE:
    case CORE_STEP_LOCATION_WRITABLE:
    case CORE_STEP_LOCATION_CREATE:
    case CORE_STEP_LOCATION_WRITE:
    case CORE_STEP_THIS_MAC_DELETE: return location_choice_step(s, &e, kind, args, nargs, out);   /* plan N3 */
    case CORE_STEP_DATE_TEXT: return date_text(nargs >= 1 ? args[0] : NULL, out);
    case CORE_STEP_NOTIFY: return nargs >= 1 && args[0] ? (notify_show(args[0], nargs >= 2 && args[1] ? args[1] : "") ? 1 : 0) : 1;
    case CORE_STEP_REPORT_FACTS: return report_facts_step(s, out);
    case CORE_STEP_REPORT_ZIP: return report_zip_step(s, args, nargs, out);
    default: return CORE_E_STATE;   /* not reached on Windows (win/c/tests/native-steps-reach.py): as a step entry not provided */
    }
}
