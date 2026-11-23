/* The core DLL's native steps as rules (native_steps.h). C11 over common/plat.h. */
#include "native_steps.h"

#include "admin_proto.h"
#include "cfg_ini.h"      /* cfg_uuid_ok, CFG_MAX */
#include "compat.h"
#include "fs_write.h"
#include "native_config.h"
#include "native_facts.h"
#include "native_location.h"
#include "volumes.h"      /* VOL_* refusals, vol_entries_size */
#include "marker.h"       /* common/: the location's marker check, the bridge's */
#include "path_rules.h"   /* PATHS_CACHE_FOLDER, PATHS_LOCATION_MAX */
#include "win_rules.h"    /* WR_TREE_DEPTH and Windows name/error rules */
#ifndef RBSP_CORE_PLAT_SHIM_H
#  define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h (through native_steps.h) has the shim's names */
#endif
#include "internal.h"   /* the core's busy_marker_* */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(_WIN32)
#  include "plat_win.h"
#  include <winternl.h>
#  define SEP "\\"
#else
#  define SEP "/"
#endif

void native_flow_init(native_flow *f) {
    memset(f, 0, sizeof *f);
    plat_mutex_init(&f->mu);
}

void native_flow_free(native_flow *f) { plat_mutex_destroy(&f->mu); }

int native_bridge_path(const char *support, const payload_manifest *m, char *out, size_t n) {
    if (!support || !m->has_windows || !m->win_bridge.file[0]) return -1;
    int k = snprintf(out, n, "%s" SEP "payload" SEP "%s" SEP "%s", support, m->payload_version, m->win_bridge.file);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

int native_payload_dir(const char *support, const payload_manifest *m, char *out, size_t n) {
    if (!support || !m->payload_version[0]) return -1;
    int k = snprintf(out, n, "%s" SEP "payload" SEP "%s", support, m->payload_version);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

int native_model_path(const char *support, const payload_manifest *m, char *out, size_t n) {
    if (!support || !m->model.file[0]) return -1;
    int k = snprintf(out, n, "%s" SEP "payload" SEP "%s" SEP "%s", support, m->payload_version, m->model.file);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

int native_manifest_due(int64_t age, const char *saved_app, const char *own, int (*blocked)(void *ctx), void *ctx) {
    if (age < 0 || !saved_app || !saved_app[0]) return 1;
    if (age > 86400 || (own && payload_version_cmp(own, saved_app) > 0)) return 1;
    return blocked && blocked(ctx) != 0;
}

/* payloadFacts: what PAYLOAD says of m (NULL: none), fetch as FACT_PAYLOAD_FETCH; m is seen */
static void facts_of(const native_steps_env *e, const payload_manifest *m, int fetch, native_payload_facts *out) {
    memset(out, 0, sizeof *out);
    out->fetch = fetch;
    out->found = m != NULL;
    int64_t age = e->age(e->ctx);
    if (age >= 0) out->has_age = 1, out->age = age;
    out->step_path = e->model ? e->model : "";
    if (!m) return;
    snprintf(out->version, sizeof out->version, "%s", m->payload_version);
    snprintf(out->signed_by, sizeof out->signed_by, "%s", m->signed_by ? m->signed_by : "");
    snprintf(out->app_version, sizeof out->app_version, "%s", m->win_app.version);
    memcpy(out->bridge_sha, m->win_bridge.sha256, sizeof out->bridge_sha);
    memcpy(out->model_sha, m->model.sha256, sizeof out->model_sha);
    char path[NS_PATH_MAX], got[65];
    out->bridge_here = !native_bridge_path(e->support, m, path, sizeof path) && !e->sha(e->ctx, path, got) && !strcmp(got, m->win_bridge.sha256);
    e->seen(e->ctx, m);
}

static void flow_set(native_flow *f, const payload_manifest *m) {
    plat_mutex_lock(&f->mu);
    f->has = m != NULL;
    if (m) f->m = *m;
    plat_mutex_unlock(&f->mu);
}

int native_step_payload(const native_steps_env *e, native_flow *f, const char *arg, native_payload_facts *out) {
    payload_manifest *a = malloc(sizeof *a), *b = malloc(sizeof *b);
    int r = 0;
    if (!a || !b || !arg) r = -1;
    else if (!strcmp(arg, "latest")) {
        /* install's: the latest, else the last good one; the one found becomes the flow's */
        /* FACT_PAYLOAD_FETCH 0 only for a payload downloaded, verified and taken: a no-news one falls
         * back to the saved payload as 2, so the saved one's gate still applies (flows.c) */
        int x = e->fetch(e->ctx, a), fetch = x == NS_FETCH_OK ? 0 : x == NS_FETCH_UNVERIFIED ? 1 : 2;
        const payload_manifest *m = x == NS_FETCH_OK && a->has_windows ? a : e->saved(e->ctx, b) ? b : NULL;
        flow_set(f, m);
        e->set_bridge(e->ctx, NULL, NULL);   /* a new flow payload: the last one's checked bridge goes */
        facts_of(e, m, fetch, out);
    } else if (!strcmp(arg, "saved")) {
        const payload_manifest *m = e->saved(e->ctx, b) ? b : NULL;
        flow_set(f, m);
        e->set_bridge(e->ctx, NULL, NULL);
        facts_of(e, m, 2, out);
    } else if (!strcmp(arg, "check")) {
        /* checkForUpdate's first part: the record's rule, the saved one seen, whether to fetch */
        const payload_manifest *m = e->saved(e->ctx, b) ? b : NULL;
        if (!m) e->seen(e->ctx, NULL);   /* also clears a record a replaced app left */
        facts_of(e, m, 2, out);
        out->has_flag = 1;
        /* manifestDue, with blockedByVersionList from BLOCKED's facts */
        out->flag = native_manifest_due(out->has_age ? out->age : -1, m ? m->win_app.version : NULL, e->own, e->blocked, e->ctx);
    } else if (!strcmp(arg, "fetch")) {
        /* its background download: never the flow's */
        int x = e->fetch(e->ctx, a);
        if (x == NS_FETCH_OK && a->has_windows) facts_of(e, a, 0, out);
        else facts_of(e, NULL, x == NS_FETCH_OK ? 0 : x == NS_FETCH_UNVERIFIED ? 1 : 2, out);
    } else
        r = -1;
    free(a);
    free(b);
    return r;
}

/* the flow payload, copied: 1, or 0 when there is none (m: malloc'd by the caller) */
static int flow_get(native_flow *f, payload_manifest *m) {
    plat_mutex_lock(&f->mu);
    int has = f->has;
    if (has) *m = f->m;
    plat_mutex_unlock(&f->mu);
    return has;
}

int native_step_download(const native_steps_env *e, native_flow *f, const char *arg) {
    payload_manifest *m = malloc(sizeof *m);
    char dir[NS_PATH_MAX];
    int r = 1;
    if (m && arg && flow_get(f, m) && !native_payload_dir(e->support, m, dir, sizeof dir)) {
        if (!strcmp(arg, "model")) r = e->download(e->ctx, m, dir, m->model.file, m->model.sha256, m->model.size);
        else if (!strcmp(arg, "bridge") && m->has_windows) r = e->download(e->ctx, m, dir, m->win_bridge.file, m->win_bridge.sha256, ADMIN_BRIDGE_MAX);   /* the admin's cap, so it never fails after UAC */
    }
    free(m);
    return r;
}

int native_rb_windows(int n_other_or_unknown, int windows_here) {
    if (n_other_or_unknown > 0) return n_other_or_unknown + (windows_here > 0 ? windows_here : 0);
    return windows_here < 0 ? -1 : windows_here;
}

int native_step_rekordbox(const native_steps_env *e, int *open, int *closing, int *updating) {
    int running = 0, upd = 0, windows = 0;
    *open = *closing = *updating = 0;
    if (e->processes(e->ctx, &running, &upd, &windows)) return 1;
    *open = running != 0;
    *closing = running && windows == 0;
    *updating = upd != 0;
    return 0;
}

static int64_t size0(const native_steps_env *e, const char *path) {
    int64_t s = path ? e->size_of(e->ctx, path) : -1;
    return s < 0 ? 0 : s;
}

int native_step_space(const native_steps_env *e, native_flow *f, int with_model, int with_cache, int64_t *need) {
    *need = 0;
    int64_t in_support = 0, in_root = 0;
    payload_manifest *m = malloc(sizeof *m);
    if (!m) return 1;
    int has = flow_get(f, m);
    if (with_model && !has) {
        free(m);
        return 1;
    }
    char path[NS_PATH_MAX], part[NS_PATH_MAX + 32];
    if (with_model && m->model.size > 0 && !native_model_path(e->support, m, path, sizeof path)) {
        int64_t s = m->model.size, have = e->size_of(e->ctx, path);
        if (have != s) {   /* update.c's part: <file>.<sha12>.part */
            snprintf(part, sizeof part, "%s.%.12s.part", path, m->model.sha256);
            have = size0(e, part);
            have = have > s ? 0 : have;   /* a part past the size is dropped (update.c), never credited */
        }
        in_support += s - have + s + size0(e, e->model);
    }
    int64_t in_vfolder = 0;
    if (with_cache && e->rekordbox) {
        char live[NS_PATH_MAX], sums[NS_PATH_MAX], v[64];
        snprintf(live, sizeof live, "%s" SEP "onnxruntime.dll", e->rekordbox);
        int64_t lib = size0(e, live), bridge = 0;
        if (has && !native_bridge_path(e->support, m, path, sizeof path)) bridge = size0(e, path);
        /* the saved set is complete only with its SHA256SUMS, written last (worker.c) */
        int saved = compat_folder_version(e->rekordbox, v, sizeof v) && e->root
                    && snprintf(sums, sizeof sums, "%s" SEP "pioneer" SEP "%s" SEP "SHA256SUMS", e->root, v) < (int)sizeof sums
                    && e->size_of(e->ctx, sums) >= 0;
        in_root += (saved ? 0 : lib) + lib;   /* the root's pioneer\<v>\ and ort\ */
        in_vfolder += bridge;                  /* the bridge staged in the version folder */
    }
    free(m);
    const int64_t spare = (int64_t)200 << 20;
    /* each file charged to the volume it goes to; the pairs on one volume merged into the first,
     * the support folder's first */
    const char *paths[3] = {e->support, with_cache ? e->root : NULL, with_cache ? e->rekordbox : NULL};
    int64_t needs[3] = {in_support, in_root, in_vfolder};
    int n = 0;
    const char *vp[3];
    int64_t vn[3];
    for (int i = 0; i < 3; i++) {
        if (!paths[i] || !paths[i][0]) continue;
        int j = 0;
        while (j < n && !e->same_volume(e->ctx, vp[j], paths[i])) j++;
        if (j == n) vp[n] = paths[i], vn[n++] = 0;
        vn[j] += needs[i];
    }
    for (int i = 0; i < n; i++) {
        if (vn[i] <= 0) continue;
        int64_t free_b = e->free_bytes(e->ctx, vp[i]);
        if (free_b < 0) continue;   /* unknown: let the copy say so */
        if (free_b < vn[i] + spare) {
            *need = vn[i] + spare;
            return 0;
        }
    }
    return 0;
}

int native_step_cache_prepare(const native_steps_env *e, native_flow *f, char ort_out[65 + 16]) {
    payload_manifest *m = malloc(sizeof *m);
    if (!m) return 1;
    plat_mutex_lock(&f->mu);
    int has = f->has;
    if (has) *m = f->m;
    plat_mutex_unlock(&f->mu);
    int r = 1;
    e->set_bridge(e->ctx, NULL, NULL);   /* set again only once this prepare succeeds */
    if (has) {
        char ort[65] = "", line[96];
        e->ort_sha(e->ctx, ort);
        snprintf(ort_out, 65 + 16, "%s", ort[0] ? ort : "unreadable");
        snprintf(line, sizeof line, "Pioneer's ONNX Runtime: %.12s", ort_out);
        e->log(e->ctx, line);
        /* the flow payload's bridge, whole: exactly the file DOWNLOAD verified */
        char path[NS_PATH_MAX], got[65];
        if (!native_bridge_path(e->support, m, path, sizeof path) && !e->sha(e->ctx, path, got) && !strcmp(got, m->win_bridge.sha256)) {
            if (!e->set_bridge(e->ctx, path, m->win_bridge.file)) r = 0;
            else e->log(e->ctx, "the bridge couldn't be handed to the install");
        }
    }
    free(m);
    return r;
}

/* ---------------------------------------------------------------- the support folder's small files */

static int join2(char *out, size_t n, const char *a, const char *b) {
    int k = snprintf(out, n, "%s" SEP "%s", a, b);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

static int open_support_for_write(const char *support, plat_dir *d, const native_location_io *io) {
    if (!support || !support[0]) return ENOENT;
    int e = plat_dir_open(support, d);
    if (e == ENOENT) {   /* made only when missing: a dangling junction gives ELOOP, nothing created (models' MR3) */
        if (native_location_io_expired(io)) return ETIMEDOUT;
        plat_mkdirs(support);
        e = plat_dir_open(support, d);
    }
    return e;
}

int native_support_write(const char *support, const char *name, const char *text, size_t n) {
    plat_dir d;
    int e = open_support_for_write(support, &d, NULL);
    if (e) return e;
    e = native_support_write_at(d, name, text, n);
    plat_dir_close(d);
    return e;
}

int native_support_write_at(plat_dir d, const char *name, const char *text, size_t n) { return win_write_at(d, name, text, n); }

int native_support_remove(const char *support, const char *name) {
    if (!support || !support[0]) return 0;
    plat_dir d;
    int e = plat_dir_open(support, &d);
    if (e) return e == ENOENT ? 0 : e;
    plat_stat st;
    e = plat_stat_at(d, name, &st);
    if (!e) e = st.kind == PLAT_DIR ? EISDIR : plat_unlink_at(d, name);
    plat_dir_close(d);
    return e == ENOENT ? 0 : e;
}

int native_support_exists(const char *support, const char *name) {
    char p[NS_PATH_MAX];
    plat_stat st;
    return support && !join2(p, sizeof p, support, name) && !plat_stat_path(p, 1, &st);
}

int64_t native_support_age(const char *support, const char *name, int64_t now) {
    char p[NS_PATH_MAX];
    plat_stat st;
    if (!support || join2(p, sizeof p, support, name) || plat_stat_path(p, 0, &st)) return -1;
    return now > st.mtime ? now - st.mtime : 0;
}

/* ---------------------------------------------------------------- PRUNE_PAYLOADS, STATE_LINE */

enum { NS_PRUNE_MAX = 32, NS_PRUNE_FAILED_MAX = 256 };
typedef struct {
    const char *keep;
    int n, nf;
    char names[NS_PRUNE_MAX][256];
    char failed[NS_PRUNE_FAILED_MAX][256];   /* tried once this run and not deleted: never again */
} prune_list;

static int prune_failed(const prune_list *l, const char *name) {
    for (int i = 0; i < l->nf; i++)
        if (!strcmp(l->failed[i], name)) return 1;
    return 0;
}

static int prune_each(void *ctx, const char *name, const plat_stat *st) {
    prune_list *l = ctx;
    if (st->kind == PLAT_DIR && strcmp(name, l->keep) && l->n < NS_PRUNE_MAX && strlen(name) < sizeof l->names[0] && !prune_failed(l, name))
        snprintf(l->names[l->n++], sizeof l->names[0], "%s", name);
    return 0;
}

/* support, then its payload folder, each opened without following a link (the Mac's mayRemove
 * refuses a link at either): 0 with both open, ENOENT when either isn't there, or another errno */
static int open_payload_root(const char *support, plat_dir *sd, plat_dir *pd) {
    int e = plat_dir_open(support, sd);
    if (e) return e;
    e = plat_dir_open_at(*sd, "payload", pd);
    if (e) plat_dir_close(*sd);
    return e;
}

int native_step_prune_payloads(const native_steps_env *e, native_flow *f) {
    payload_manifest *m = malloc(sizeof *m);
    prune_list *l = malloc(sizeof *l);
    plat_dir sd, pd, kd;
    plat_stat st;
    int err, here, full = 0;
    if (!m || !l || !e->support || !flow_get(f, m) || !m->payload_version[0] || !m->model.file[0]) goto done;
    if ((err = open_payload_root(e->support, &sd, &pd))) {
        if (err != ENOENT) {
            char line[128];
            snprintf(line, sizeof line, "support" SEP "payload isn't a plain folder: old payload versions kept (error %d)", err);
            e->log(e->ctx, line);
        }
        goto done;
    }
    /* the flow's model here (modelPath exists), through the same handles: its version folder no link */
    here = !plat_dir_open_at(pd, m->payload_version, &kd);
    if (here) {
        here = !plat_stat_at(kd, m->model.file, &st) && st.kind == PLAT_FILE;
        plat_dir_close(kd);
    }
    /* list up to NS_PRUNE_MAX not tried yet, delete them, again until a pass finds none: each pass
     * deletes a name or adds one to `failed` (each failure tried and logged once per run), so it
     * ends; only more than NS_PRUNE_FAILED_MAX stuck folders stop it early (the next flow tries
     * again). Every delete relative to pd, never following a link */
    l->keep = m->payload_version;
    l->nf = 0;
    for (int pass = 0; here && !full && pass < 1024; pass++) {
        l->n = 0;
        plat_dir_each(pd, prune_each, l);   /* a folder half read: what was read goes */
        if (!l->n) break;
        for (int i = 0; i < l->n; i++) {
            int r = e->remove_tree_at(e->ctx, pd, l->names[i]);
            if (r && r != ENOENT) {
                char line[400];
                snprintf(line, sizeof line, "payload" SEP "%s couldn't be deleted (error %d)", l->names[i], r);
                e->log(e->ctx, line);
                if (l->nf < NS_PRUNE_FAILED_MAX) memcpy(l->failed[l->nf++], l->names[i], sizeof l->failed[0]);
                if (l->nf == NS_PRUNE_FAILED_MAX) full = 1;
            }
        }
    }
    plat_dir_close(pd);
    plat_dir_close(sd);
done:
    free(l);
    free(m);
    return 0;
}

/* ---------------------------------------------------------------- LAUNCH_AGENT_UNLOAD */

typedef struct {
    const native_unload_ops *ops;
    const char *apps;
    int failed;
} unload_walk;

static void unload_say(const native_unload_ops *ops, const char *a, const char *b, const char *c) {
    if (!ops->log) return;
    char line[512];
    snprintf(line, sizeof line, "%s%s%s", a, b ? b : "", c ? c : "");
    ops->log(ops->ctx, line);
}

static int unload_each(void *ctx, const char *name, const plat_stat *st) {
    unload_walk *w = ctx;
    if (st->kind != PLAT_DIR) return 0;   /* `current`, the stub; a link is never a version folder */
    char dir[1024];
    int k = snprintf(dir, sizeof dir, "%s" SEP "%s", w->apps, name);
    if (k < 0 || (size_t)k >= sizeof dir) {
        w->failed = 1;
        return 0;
    }
    int r = w->ops->stop_watchers(w->ops->ctx, dir);
    if (r > 0) unload_say(w->ops, "watcher: the one running from ", name, " stopped");
    if (r < 0) w->failed = 1;
    return 0;
}

int native_watcher_unload(const native_unload_ops *ops, const char *apps, int *off) {
    *off = -1;
    int bad = 0;
    int e = ops->run_remove(ops->ctx);
    if (e) {
        unload_say(ops, "watcher: the Run value couldn't be removed: ", strerror(e), NULL);
        bad = 1;
    }
    if (apps && apps[0]) {
        plat_dir d;
        int oe = plat_dir_open(apps, &d);
        if (!oe) {
            unload_walk w = {ops, apps, 0};
            if (plat_dir_each(d, unload_each, &w)) w.failed = 1;
            plat_dir_close(d);
            if (w.failed) {
                unload_say(ops, "watcher: the running watchers couldn't all be checked", NULL, NULL);
                bad = 1;
            }
        } else if (oe != ENOENT) {
            unload_say(ops, "watcher: ", apps, " couldn't be read");
            bad = 1;
        }
    }
    int o = ops->reminders_off(ops->ctx);
    *off = o == 0 || o == 1 ? o : -1;
    return bad;
}

/* ---------------------------------------------------------------- LAUNCH_AGENT_LOAD, WATCHER_CHECK, WATCHER_STATE */

static void watcher_say(const native_watcher_ops *ops, const char *a, const char *b, const char *c) {
    if (!ops->log) return;
    char line[512];
    snprintf(line, sizeof line, "%s%s%s", a, b ? b : "", c ? c : "");
    ops->log(ops->ctx, line);
}

int native_watcher_off(const native_watcher_ops *ops) {
    int present = 0, ours = 0, off = 0;
    if (ops->run_read(ops->ctx, &present, &ours, &off)) return -1;
    return present && off ? 1 : 0;
}

int native_watcher_load(const native_watcher_ops *ops, int *off) {
    int present = 0, ours = 0, was_off = 0;
    int e = ops->run_read(ops->ctx, &present, &ours, &was_off);
    if (e || !present || !ours) {
        int w = ops->run_write(ops->ctx);
        if (w) watcher_say(ops, "watcher: COULD NOT BE INSTALLED (the Run value: ", strerror(w), ")");
        else watcher_say(ops, "watcher: installed (the Run value starts this app's watcher)", NULL, NULL);
    }
    int now_off = native_watcher_off(ops);   /* the Run value may have changed, but the user's choice stays */
    int r = ops->running(ops->ctx);
    if (r == 0 && now_off == 1) {
        watcher_say(ops, "watcher: turned off in Startup apps: not started", NULL, NULL);
    } else if (r == 0) {
        int s = ops->start(ops->ctx);
        watcher_say(ops, "watcher: started", s ? " FAILED: " : "", s ? strerror(s) : NULL);
    } else if (r < 0) {
        watcher_say(ops, "watcher: whether it runs couldn't be checked: not started (the next logon starts it)", NULL, NULL);
    }
    *off = now_off;
    return 0;
}

int native_watcher_check(const native_watcher_ops *ops, int *off) {
    int present = 0, ours = 0, was_off = 0;
    int e = ops->run_read(ops->ctx, &present, &ours, &was_off);
    if (e) watcher_say(ops, "watcher: the Run value couldn't be read: ", strerror(e), NULL);
    else if (present && !ours) {
        int w = ops->run_write(ops->ctx);
        watcher_say(ops, "watcher: the Run value started something else: pointing it at this app's watcher", w ? ": FAILED: " : "",
                    w ? strerror(w) : NULL);
    }
    *off = native_watcher_off(ops);
    return 0;
}

/* ---------------------------------------------------------------- LOCATION_NOW and SETTINGS */

static int location_can_delete(const native_location *snap, const char *dir) {
    return dir && vol_guid_root(dir) && snap->kind == LOC_CHOSEN && snap->probe == PROBE_AVAILABLE && snap->identity[0];
}

int native_location_now_facts(int live, const native_location *snap, const char *dir, const char *id, const char *hint,
                              const char *memo_dir, native_location_target *out, int64_t *size_kind) {
    *out = (native_location_target){"", "", "", ""};
    *size_kind = -1;
    if (live && snap) {
        if (dir && (snap->kind == LOC_CHOSEN || snap->kind == LOC_BRIDGE_TOO_OLD)) {
            out->dir = dir;
            if (location_can_delete(snap, dir)) {
                out->gate_id = id;
                out->identity = snap->identity;
            }
        }
        if (snap->has_status_drive) out->drive = snap->status_drive;
    } else if (dir) {
        /* review NS11, NS12, N3R3: keep naming the chosen folder even without a current measure */
        out->dir = dir;
        out->drive = hint;
        return 0;
    } else if (!snap) return 1;
    if (memo_dir && memo_dir[0] && snap->has_in_use && !strcasecmp(memo_dir, snap->in_use)
        && (!out->dir[0] || !strcasecmp(out->dir, snap->in_use))) *size_kind = 4;
    return 0;
}

void native_settings_facts(const native_settings_in *in, native_settings *out) {
    memset(out, 0, sizeof *out);
    fmt_config_number(in->max_gb, out->gb);
    fmt_config_number(in->max_days, out->days);
    out->unreadable = in->unreadable != 0;
    out->can_choose = in->can_choose != 0;
    int loc = in->unreadable ? -1 : in->location;
    const native_location *snap = in->snap;
    out->size_kind = -1;
    if (loc == 0 || loc == 1) {
        const char *dir = loc == 0 ? "" : in->dir, *id = loc == 0 ? "" : in->id;
        out->size_kind = snap && (loc == 1 || snap->kind == LOC_THIS_MAC) && native_location_checked(snap)
                             && dir && id && native_location_matches_config(snap, dir, id) ? 4 : 0;
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
        if (loc == 0 && snap && snap->kind == LOC_THIS_MAC && snap->probe == PROBE_MARKER_MISSING_OR_WRONG)
            out->size_kind = 2;
#endif
    }
    out->clear = loc == 0 || loc == 2 ? (in->this_pc_folder ? 1 : 2) : 0;
    if (loc == 1 && snap) {
        if (snap->kind == LOC_BRIDGE_TOO_OLD) out->clear = in->this_pc_folder ? 1 : 2;
        /* review N3R3: only offer the gate when settingsStems measured this config.ini's folder */
        else if (location_can_delete(snap, in->dir) && snap->has_in_use && !strcmp(snap->in_use, in->dir)) {
            out->clear = 3;
            out->clear_target =
                (native_location_target){in->dir, in->id, snap->has_status_drive ? snap->status_drive : in->drive, snap->identity};
        }
    }
    out->location = in->unreadable ? 2 : loc == 1 ? 1 : loc == 2 ? 3 : 0;
    if (loc != 1 || !in->dir) return;
    snprintf(out->drive, sizeof out->drive, "%s", in->drive ? in->drive : "");
    /* labelPath: the parent's components below the volume's root */
    const char *dir = in->dir, *end = dir + strlen(dir);
    while (end > dir && (end[-1] == '\\' || end[-1] == '/')) end--;
    const char *last = end;
    while (last > dir && last[-1] != '\\' && last[-1] != '/') last--;   /* the chosen folder's own name: not below */
    size_t skip = 0, ml = in->mount ? strlen(in->mount) : 0;
    if (ml && !strncasecmp(dir, in->mount, ml)) skip = ml;
    else if (!ml) skip = paths_root_win(dir);
    if (!skip && ((dir[0] >= 'A' && dir[0] <= 'Z') || (dir[0] >= 'a' && dir[0] <= 'z')) && dir[1] == ':') skip = 2;
    size_t k = 0;
    for (const char *p = dir + skip; p < last;) {
        while (p < last && (*p == '\\' || *p == '/')) p++;
        const char *q = p;
        while (q < last && *q != '\\' && *q != '/') q++;
        if (q > p) {
            size_t n = (size_t)(q - p);
            if (k + n + 2 > sizeof out->label_path) break;
            if (k) out->label_path[k++] = '/';
            memcpy(out->label_path + k, p, n);
            k += n;
            out->label_path[k] = 0;
        }
        p = q;
    }
}

int native_stop_keeping(const char *kept, const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (!kept || !n || n > 64) return 1;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 1;
    }
    plat_dir d;
    int e = plat_dir_open(kept, &d);
    if (e) return e != ENOENT && e != ENOTDIR;   /* none, or kept is a file (Swift: nil); a link or an unreadable one: still there */
    char f[96];
    plat_stat st;
    snprintf(f, sizeof f, "%s.kept", name);
    if (!plat_stat_at(d, f, &st) && st.kind == PLAT_FILE) {
        e = plat_unlink_at(d, f);
        if (e && e != ENOENT) {
            plat_dir_close(d);
            return 1;
        }
    }
    e = plat_stat_at(d, f, &st);
    int failed = e != ENOENT;
    if (!failed) {
        snprintf(f, sizeof f, "%s.manifest", name);
        if (!plat_stat_at(d, f, &st) && st.kind == PLAT_FILE) {
            e = plat_unlink_at(d, f);
            failed = e && e != ENOENT;
        }
    }
    plat_dir_close(d);
    return failed;
}

/* ASCII letters' case ignored */
static int same_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

static int cache_refused(int r) {
    return r == ELOOP || r == PLAT_EFTYPE || r == ENOTDIR;
}

/* The GUID and file ID both come from the directory we will use. */
static int cache_identity_at(plat_dir d, const char *expected_path, char out[NS_CACHE_IDENTITY_MAX]) {
    plat_stat st;
    out[0] = 0;
    if (plat_dir_stat(d, &st) || st.kind != PLAT_DIR) return EIO;
    size_t root = 0;
#if defined(_WIN32)
    int r = vol_dir_guid(d, out);
    if (r) return r;
    root = VOL_GUID_LEN;
    if (expected_path && (vol_guid_root(expected_path) != root || strncasecmp(expected_path, out, root))) return EXDEV;
#else
    (void)expected_path;
#endif
    cache_owner_identity(out, root, st.id);
    return 0;
}

static int open_pc_support(const native_steps_env *e, const char *cache_default, plat_dir *s) {
    if (!e->support || !e->support[0] || !cache_default) return PLAT_EFTYPE;
    char expected[NS_PATH_MAX];
    int n = snprintf(expected, sizeof expected, "%s" SEP "Cache", e->support);
    if (n < 0 || n >= (int)sizeof expected || !same_ci(cache_default, expected)) return PLAT_EFTYPE;
    return plat_dir_open(e->support, s);
}

#ifdef NATIVE_STEPS_TEST_HOOKS
int (*native_steps_test_fault)(int op, const char *name);
int native_steps_test_hold_cache_tree;
#  define FAULT(op, name) (native_steps_test_fault ? native_steps_test_fault(op, name) : 0)
#else
#  define FAULT(op, name) 0
#endif

typedef enum { CACHE_OPEN_READ, CACHE_OPEN_CLEAR, CACHE_OPEN_REMOVE } cache_open_mode;

static int cache_open_at(plat_dir parent, const char *name, plat_dir *d, cache_open_mode mode) {
#if defined(_WIN32)
    if (mode == CACHE_OPEN_REMOVE) return plat_dir_open_at_for_removal(parent, name, d);
    if (mode == CACHE_OPEN_CLEAR) return plat_dir_open_at_held(parent, name, d);
#else
    (void)mode;
#endif
    return plat_dir_open_at(parent, name, d);
}

/* Only support\Cache, whatever the shell's JSON says. Success gives the caller both handles. */
static int open_pc_cache(const native_steps_env *e, const char *cache_default, plat_dir *s, plat_dir *d, int *link, cache_open_mode mode) {
    int r = open_pc_support(e, cache_default, s);
    if (!r) {
        r = FAULT(NS_FAULT_OPEN, cache_default);
        if (!r) r = cache_open_at(*s, "Cache", d, mode);
        if (r) {
            plat_stat st;
            /* A file symlink can give ENOTDIR when opened as a directory on Windows. */
            if (link) *link = r == ELOOP || (r == ENOTDIR && !plat_stat_at(*s, "Cache", &st) && st.kind == PLAT_OTHER);
            plat_dir_close(*s);
        }
    }
    if (e->log && cache_refused(r)) {
        char line[NS_PATH_MAX + 32];
        snprintf(line, sizeof line, "refused to delete %s", cache_default ? cache_default : "(none)");
        e->log(e->ctx, line);
    }
    return r;
}

/* ---------------------------------------------------------------- REMOVE_LOCATION */

static int loc_stat_at(plat_dir d, const char *name, plat_stat *st) {
    int r = FAULT(NS_FAULT_STAT, name);
    return r ? r : plat_stat_at(d, name, st);
}

/* plain components (none empty, "." or "..", no control character or '"'; on Windows none of
 * / : * ? < > |), the last our folder's name */
static int location_rel_ok(const char *rel) {
    if (!rel || !rel[0] || strlen(rel) > PATHS_LOCATION_MAX) return 0;
    const char *c = rel, *last = c;
    for (const char *q = c;; q++) {
        if (*q == SEP[0] || !*q) {
            size_t k = (size_t)(q - c);
            if (!k || (k == 1 && c[0] == '.') || (k == 2 && c[0] == '.' && c[1] == '.')) return 0;
            last = c;
            if (!*q) break;
            c = q + 1;
            continue;
        }
        unsigned char u = (unsigned char)*q;
        if (u < 0x20 || u == '"' || u == 0x7f) return 0;
#if defined(_WIN32)
        if (strchr("/:*?<>|", u)) return 0;
#endif
    }
    return !strcmp(last, PATHS_CACHE_FOLDER);
}

/* a chosen location's path: its root's length (Windows "X:\" or the volume-GUID root cache_dir
 * takes, "\\?\Volume{GUID}\": paths_root_win; the Mac's "/"), or 0 */
static size_t location_root(const char *p) {
    size_t n = p ? strlen(p) : 0;
    if (!n || n > PATHS_LOCATION_MAX) return 0;
#if defined(_WIN32)
    size_t root = paths_root_win(p);
    if (!root || n <= root) return 0;
#else
    if (p[0] != '/') return 0;
    size_t root = 1;
#endif
    return location_rel_ok(p + root) ? root : 0;
}

static int lower_hex64(const char *s) {
    if (strlen(s) != 64) return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}

typedef struct {
    char **names;   /* each its own copy: any name plat_dir_each gives (review MT5) */
    int n, cap, failed;
} loc_list;

static int loc_each(void *ctx, const char *name, const plat_stat *st) {
    (void)st;
    loc_list *l = ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        char **more = realloc(l->names, (size_t)cap * sizeof *more);
        if (!more) return -1;
        l->names = more;
        l->cap = cap;
    }
    size_t k = strlen(name) + 1;
    if (!(l->names[l->n] = malloc(k))) return -1;
    memcpy(l->names[l->n++], name, k);
    return 0;
}

static void loc_free(loc_list *l) {
    for (int i = 0; i < l->n; i++) free(l->names[i]);
    free(l->names);
}

/* New local caches record their own volume and file ID in a marker. An old or copied marker
 * cannot authorize this slot, even when a new operation measures it. */
static int pc_cache_identity_at(plat_dir d, char out[NS_CACHE_IDENTITY_MAX]) {
    int r = cache_identity_at(d, NULL, out);
    return r ? r : cache_owner_check(d, out);
}

static int unlink_cache_marker(plat_dir d, const char *name) {
    int r = FAULT(NS_FAULT_UNLINK_MARKER, name);
    return r ? r : plat_unlink_at(d, name);
}

static int dispose_verified_cache(const native_steps_env *e, plat_dir parent, const char *name, plat_dir held) {
#if defined(_WIN32)
    (void)e; (void)parent; (void)name;
    return plat_dir_dispose(held);
#else
    /* Host fixtures inject races into this identity-checked removal. */
    plat_stat st;
    int r = plat_dir_stat(held, &st);
    return r ? r : e->remove_empty_dir_at(e->ctx, parent, name, st.id);
#endif
}

#if defined(_WIN32)
/* plat_unlink_at only accepts non-directories. A directory reparse point is also a leaf,
 * so open it relative to the held parent, and verify the reparse point on the delete handle. */
static int remove_cache_reparse_point(plat_dir parent, const char *name) {
    uint16_t wide[256];
    long n = wr_name16(name, wide, sizeof wide / sizeof wide[0]);
    if (n <= 0) return EINVAL;
    UNICODE_STRING path = {(USHORT)(n * 2), (USHORT)(n * 2), (PWSTR)(void *)wide};
    OBJECT_ATTRIBUTES attrs;
    InitializeObjectAttributes(&attrs, &path, OBJ_CASE_INSENSITIVE, parent.h, NULL);
    IO_STATUS_BLOCK io;
    HANDLE h = NULL;
    NTSTATUS status = NtCreateFile(&h, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attrs, &io, NULL, 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                   FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    int r = wr_errno_nt((uint32_t)status, WR_OP_RENAME_DELETE);
    if (r) return r;
    plat_dir held = {h};
    plat_stat st;
    r = plat_dir_stat(held, &st);
    if (!r) r = st.kind == PLAT_OTHER ? plat_dir_dispose(held) : EBUSY;
    plat_dir_close(held);
    return r;
}
#endif

#if defined(_WIN32) || defined(NATIVE_STEPS_TEST_HOOKS)
static int remove_cache_tree_held(const native_steps_env *e, plat_dir parent, const char *name, int depth) {
    plat_stat st;
    int r = plat_stat_at(parent, name, &st);
    if (r) return r;
#if defined(_WIN32)
    if (st.kind == PLAT_OTHER) return remove_cache_reparse_point(parent, name);
#endif
    if (st.kind != PLAT_DIR) return plat_unlink_at(parent, name);
    if (depth >= WR_TREE_DEPTH) return ENAMETOOLONG;

    plat_dir held;
    r = FAULT(NS_FAULT_OPEN, name);
    if (!r) r = cache_open_at(parent, name, &held, CACHE_OPEN_REMOVE);
    if (r) return r;
    loc_list entries = {0};
    int listed = !plat_dir_each(held, loc_each, &entries);
    if (!listed) r = EIO;
    /* Keep the directory's listing and DELETE handle through every recursive call. */
    for (int i = 0; listed && i < entries.n; i++) {
        int child = remove_cache_tree_held(e, held, entries.names[i], depth + 1);
        if (!r && child != ENOENT) r = child;
    }
    loc_free(&entries);
    if (!r) r = dispose_verified_cache(e, parent, name, held);
    plat_dir_close(held);
    return r;
}
#endif

static int remove_cache_tree(const native_steps_env *e, plat_dir parent, const char *name) {
#if defined(_WIN32)
    return remove_cache_tree_held(e, parent, name, 0);
#else
#  if defined(NATIVE_STEPS_TEST_HOOKS)
    if (native_steps_test_hold_cache_tree) return remove_cache_tree_held(e, parent, name, 0);
#  endif
    return e->remove_tree_at(e->ctx, parent, name);
#endif
}

static void restore_cache_marker(const native_steps_env *e, plat_dir held, const char *name, const char *value) {
    if (win_write_new_at(held, name, value, strlen(value)) && e->log)
        e->log(e->ctx, "couldn't restore the cache ownership marker");
}

static void restore_external_marker(const native_steps_env *e, plat_dir held, const char *id) {
    char marker[96];
    snprintf(marker, sizeof marker, "id=%s\nformat=%s\n", id, MARKER_FORMAT);
    restore_cache_marker(e, held, MARKER_NAME, marker);
}

int native_pc_cache_identity(const native_steps_env *e, const char *cache_default, char identity[NS_CACHE_IDENTITY_MAX]) {
    plat_dir s, d;
    int r = open_pc_cache(e, cache_default, &s, &d, NULL, CACHE_OPEN_READ);
    identity[0] = 0;
    if (!r) {
        r = pc_cache_identity_at(d, identity);
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
        if (r == ENOENT) r = EBUSY;   /* the slot exists, but the ownership marker is missing */
#endif
        plat_dir_close(d);
        plat_dir_close(s);
    }
    if (r) identity[0] = 0;
    return r;
}

typedef struct {
    char support[NS_PATH_MAX], cache[NS_PATH_MAX], identity[NS_CACHE_IDENTITY_MAX];
    int rc;
} pc_cache_check;

static void run_pc_cache_check(void *ctx) {
    pc_cache_check *c = ctx;
    native_steps_env e = {.support = c->support};
    c->rc = native_pc_cache_identity(&e, c->cache, c->identity);
}

int native_pc_cache_identity_bounded(const char *support, const char *cache_default, uint64_t deadline,
                                     char identity[NS_CACHE_IDENTITY_MAX]) {
    identity[0] = 0;
    pc_cache_check c;
    if (!support || !cache_default || snprintf(c.support, sizeof c.support, "%s", support) >= (int)sizeof c.support ||
        snprintf(c.cache, sizeof c.cache, "%s", cache_default) >= (int)sizeof c.cache) return NL_NO_ANSWER;
    int rc = native_support_read_bounded(run_pc_cache_check, &c, sizeof c, deadline, cache_default);
    if (rc) return rc;
    memcpy(identity, c.identity, sizeof c.identity);
    return c.rc;
}
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
int native_pc_cache_probe(const char *support, const char *cache_default) {
    native_steps_env e = {.support = support};
    char identity[NS_CACHE_IDENTITY_MAX];
    return native_pc_cache_identity(&e, cache_default, identity);
}
#endif

int native_clear_cache(const native_steps_env *e, const char *cache_default, const char *identity, int whole, int *link) {
    *link = 0;
    plat_dir s, d;
    int r = open_pc_cache(e, cache_default, &s, &d, link, whole ? CACHE_OPEN_REMOVE : CACHE_OPEN_CLEAR);
    if (!r) {
        char opened[NS_CACHE_IDENTITY_MAX];
        plat_stat marker;
        loc_list l = {0};
        if (!identity || !identity[0] || pc_cache_identity_at(d, opened) || strcmp(identity, opened)) r = EBUSY;
        if (!r) r = plat_stat_at(d, NS_PC_MARKER, &marker);
        if (r == ENOENT && whole) r = EBUSY;
        if (!r && (plat_dir_each(d, loc_each, &l) || l.failed)) r = EIO;
        if (!r) {
            for (int i = 0; i < l.n; i++) {
                plat_stat entry;
                int x = plat_stat_at(d, l.names[i], &entry);
                if (!x && !plat_id_same(entry.id, marker.id)) x = remove_cache_tree(e, d, l.names[i]);
                if (!r && x && x != ENOENT) r = x;
            }
        }
        if (!r && whole) {
            r = unlink_cache_marker(d, NS_PC_MARKER);
            if (r == ENOENT) r = EBUSY;
            if (!r && (r = dispose_verified_cache(e, s, "Cache", d)))
                restore_cache_marker(e, d, NS_PC_MARKER, opened);
        }
        loc_free(&l);
        plat_dir_close(d);
        plat_dir_close(s);
    } else if (cache_refused(r)) {
        return 1;
    }
    if (r && r != ENOENT) {
        char line[NS_PATH_MAX + 32];
        snprintf(line, sizeof line, "couldn't clear the stems saved on this PC (error %d)", r);
        e->log(e->ctx, line);
        return 1;
    }
    return 0;
}

int native_remove_location(const native_steps_env *e, const char *dir, const char *id, int whole, char name[256], char err[128],
                           const char *identity) {
    name[0] = err[0] = 0;
    size_t root = location_root(dir);
    if (!root || !vol_guid_root(dir) || !paths_location_ok_win(dir)) return 1;
    if (!identity || !*identity) return 6;
    char top[VOL_GUID_MAX];
    snprintf(top, sizeof top, "%.*s", (int)root, dir);
#if defined(_WIN32)
    if (vol_saved_refusal(top) != VOL_OK) return 1;
#endif
    return native_remove_location_under(e, top, dir + root, id, whole, name, err, identity);
}

int native_remove_location_under(const native_steps_env *e, const char *top, const char *rel, const char *id, int whole, char name[256],
                                 char err[128], const char *identity) {
    name[0] = err[0] = 0;
    if (!location_rel_ok(rel) || !id || !id[0]) return 1;
    char path[PATHS_LOCATION_MAX + 1];
    snprintf(path, sizeof path, "%s", rel);
    plat_dir parent, ours;
    plat_stat st;
    int e0 = plat_dir_open(top, &parent);
    if (e0) return e0 == ENOENT ? 1 : 2;
    if (plat_dir_stat(parent, &st)) {
        plat_dir_close(parent);
        return 3;
    }
    uint64_t vol = st.id.vol;
    /* one component at a time, never through a link, never onto another drive */
    for (char *save = NULL, *c = strtok_r(path, SEP, &save), *next; c; c = next) {
        next = strtok_r(NULL, SEP, &save);
        cache_open_mode mode = next ? CACHE_OPEN_READ : whole ? CACHE_OPEN_REMOVE : CACHE_OPEN_CLEAR;
        int oe = cache_open_at(parent, c, &ours, mode);
        if (oe) {
            snprintf(name, 256, "%s", c);
            snprintf(err, 128, "%s", strerror(oe));
            plat_dir_close(parent);
            return 4;
        }
        if (plat_dir_stat(ours, &st) || st.id.vol != vol) {
            snprintf(name, 256, "%s", c);
            plat_dir_close(ours);
            plat_dir_close(parent);
            return 5;
        }
        if (!next) break;
        plat_dir_close(parent);
        parent = ours;
    }
    char opened[NL_IDENTITY_MAX];
    native_location_identity(st.id, opened);
    int r = ((identity && strcmp(identity, opened)) || strlen(id) != 36 || marker_check_at(ours, id)) ? 6 : 0;
#if defined(_WIN32)
    char guid_identity[NS_CACHE_IDENTITY_MAX];
    if (!r && cache_identity_at(ours, top, guid_identity)) r = 6;
#endif
    static const char companion[] = "._" MARKER_NAME;
    /* the marker and its companion by identity (review MS6, MP3): taken before the walk; the
     * companion may be missing, the marker can't be (it was just checked) */
    plat_stat ms, cs;
    int ce = 0;
    if (!r && loc_stat_at(ours, MARKER_NAME, &ms)) r = 6;
    if (!r && (ce = loc_stat_at(ours, companion, &cs)) && ce != ENOENT) r = 6;
    if (!r) {
        loc_list l = {NULL, 0, 0, 0}, variants = {NULL, 0, 0, 0};
        int ok = !plat_dir_each(ours, loc_each, &l) && !l.failed;
        for (int i = 0; i < l.n; i++) {
            const char *n = l.names[i];
            /* the marker (and its companion) go last, only once everything else went: any other
             * entry that is the marker's or its companion's file (by id, under any name: a case
             * or Unicode case-fold alias the drive resolves, or a hard link) is kept with them and
             * goes after the marker; a different file goes as any other (review MS6, MQ1) */
            if (!strcmp(n, MARKER_NAME) || !strcmp(n, companion)) continue;
            {
                plat_stat vs;
                int se = loc_stat_at(ours, n, &vs);
                if (se) {
                    if (se != ENOENT) ok = 0;   /* can't tell: kept, and so the marker is (MP3) */
                    continue;
                }
                if (vs.kind == PLAT_FILE && (plat_id_same(vs.id, ms.id) || (!ce && plat_id_same(vs.id, cs.id)))) {
                    if (loc_each(&variants, n, &vs)) ok = 0;
                    continue;
                }
            }
            if (!whole && !lower_hex64(n) && strncmp(n, "._", 2)) continue;
            int x = remove_cache_tree(e, ours, n);
            if (x && x != ENOENT) ok = 0;
        }
        loc_free(&l);
        if (!ok) r = 7;
        else if (whole) {
            int x = plat_unlink_at(ours, companion);
            if (x && x != ENOENT) r = 7;
            int marker_removed = !r && !unlink_cache_marker(ours, MARKER_NAME);
            if (!marker_removed) r = 7;
            /* the kept variants: gone with it on a case-insensitive drive (ENOENT); a remaining
             * one fails the emptiness check below */
            for (int i = 0; !r && i < variants.n; i++) plat_unlink_at(ours, variants.names[i]);
            /* the folder itself, only empty now and only the one checked (review MT1: by
             * identity, never by name, never recursive: whatever appeared meanwhile stays) */
            loc_list rest = {NULL, 0, 0, 0};
            if (!r && (plat_dir_each(ours, loc_each, &rest) || rest.n || rest.failed)) r = 7;
            loc_free(&rest);
            if (!r && dispose_verified_cache(e, parent, PATHS_CACHE_FOLDER, ours)) r = 7;
            if (r && marker_removed) restore_external_marker(e, ours, id);
        }
        loc_free(&variants);
    }
    plat_dir_close(ours);
    plat_dir_close(parent);
    return r;
}

/* ---------------------------------------------------------------- REMOVE_OWN_FOLDERS */

static int own_dir_open(const char *path, plat_dir *d) {
    int r = FAULT(NS_FAULT_OPEN, path);
    return r ? r : plat_dir_open(path, d);
}

static int own_stat_at(plat_dir d, const char *name, plat_stat *st) {
    int r = FAULT(NS_FAULT_STAT, name);
    return r ? r : plat_stat_at(d, name, st);
}

#ifdef NATIVE_STEPS_TEST_HOOKS
/* the walk's listing; with NS_FAULT_LIST_ID asked and answered for a name, its listing id differs
 * from its stat id (review SF4: what stays is decided by stat ids, never the listing's) */
typedef struct {
    plat_each_fn fn;
    void *ctx;
} list_fault;
static int list_fault_each(void *ctx, const char *name, const plat_stat *st) {
    list_fault *lf = ctx;
    plat_stat copy = *st;
    if (FAULT(NS_FAULT_LIST_ID, name)) copy.id.file[1] ^= 1;
    return lf->fn(lf->ctx, name, &copy);
}
static int own_dir_each(plat_dir d, plat_each_fn fn, void *ctx) {
    list_fault lf = {fn, ctx};
    int r = plat_dir_each(d, list_fault_each, &lf);
    return FAULT(NS_FAULT_LIST, "") ? -1 : r;   /* a listing that couldn't be read whole (review S61's m4) */
}
#else
#  define own_dir_each plat_dir_each
#endif

static void refused(const native_steps_env *e, const char *path, const char *name) {
    char line[NS_PATH_MAX + 300];
    snprintf(line, sizeof line, "refused to delete %s%s%s", path ? path : "(none)", name ? SEP : "", name ? name : "");
    e->log(e->ctx, line);
}

/* support itself, emptied, by identity (review SF2): its parent opened following links (a
 * junction or a link at LocalAppData is the account's own layout, as the Mac's removefile follows
 * the parents of what it deletes), then only the folder whose id the walk's handle gave, only
 * empty, through remove_empty_dir_at: 0 gone (or none), 1 not (logged) */
static int remove_support(const native_steps_env *e, const char *path, plat_id id) {
    char parent[NS_PATH_MAX], line[NS_PATH_MAX + 64];
    if (snprintf(parent, sizeof parent, "%s", path) >= (int)sizeof parent) return 1;
    char *cut = strrchr(parent, SEP[0]);
    if (!cut || cut == parent || !cut[1]) return 1;
    *cut = 0;
    const char *name = cut + 1;
    plat_dir d;
    int r = FAULT(NS_FAULT_OPEN, parent);
    if (!r) r = plat_dir_open_follow(parent, &d);
    if (r) {
        if (r != ENOENT) {
            snprintf(line, sizeof line, "couldn't open %s (error %d)", parent, r);
            e->log(e->ctx, line);
        }
        return r != ENOENT;
    }
    plat_stat st;
    r = own_stat_at(d, name, &st);
    if (!r && (st.kind != PLAT_DIR || !plat_id_same(st.id, id))) {   /* not the folder just emptied: left */
        refused(e, parent, name);
        plat_dir_close(d);
        return 1;
    }
    if (!r) r = e->remove_empty_dir_at(e->ctx, d, name, id);
    plat_dir_close(d);
    if (r && r != ENOENT) {
        snprintf(line, sizeof line, "couldn't remove %s (error %d)", path, r);
        e->log(e->ctx, line);
        return 1;
    }
    return 0;
}

typedef enum { SUPPORT_ROOT, SUPPORT_LOGS, SUPPORT_ORIGINALS, SUPPORT_PAYLOAD, SUPPORT_RELEASE, SUPPORT_UPDATE,
               SUPPORT_KEPT, SUPPORT_ADMIN, SUPPORT_FILES, SUPPORT_ADMIN_WORKER, SUPPORT_REPORTS } support_folder;

static int named(const char *name, const char *const *names, size_t count) {
    for (size_t i = 0; i < count; i++) if (same_ci(name, names[i])) return 1;
    return 0;
}
#define NAMED(name, list) named(name, list, sizeof(list) / sizeof((list)[0]))

/* The name rules used by host_facts' kept-set reader and report_build's zip writer. */
static int support_kept_file(const char *name) {
    const char *suffix = strrchr(name, '.');
    if (!suffix || (!same_ci(suffix, ".kept") && !same_ci(suffix, ".manifest"))) return 0;
    size_t n = strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
    return n >= 1 && n <= 64 && name + n == suffix;
}

static int support_report_file(const char *name) {
    static const char prefix[] = "RB Stems Plus report ";
    static const char stamp[] = "####-##-## ######";
    size_t n = sizeof prefix - 1, k = sizeof stamp - 1;
    if (strlen(name) != n + k + 4 || strncasecmp(name, prefix, n) || !same_ci(name + n + k, ".zip")) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = name[n + i];
        if (stamp[i] == '#' ? c < '0' || c > '9' : c != stamp[i]) return 0;
    }
    return 1;
}

static int support_file(support_folder folder, const char *name) {
    static const char *const root[] = {"config.ini", "installed", "busy", "dismissed", "request", "window", "welcomed",
                                      NS_LOCATION_HINT, "location-drive", NS_PENDING_DELETION, "shown-version", "payload-version"};
    static const char *const logs[] = {"app.log", "watcher.log", "bridge.log", "bridge.log.1", "bridge-state"};
    static const char *const payload[] = {"payload.json", "payload.json.sig"};
    static const char *const downloads[] = {"rbstemsplus-bridge-win-x64.dll", "stemsplus-model.onnx", "rbstemsplus-windows.zip"};
    static const char *const update[] = {"pending", "started", "result", "watcher"};
    size_t n = strlen(name);
    switch (folder) {
    case SUPPORT_ROOT: return NAMED(name, root);
    case SUPPORT_LOGS: return NAMED(name, logs);
    case SUPPORT_PAYLOAD: case SUPPORT_RELEASE: return NAMED(name, payload);
    case SUPPORT_ADMIN: return NAMED(name, payload) || same_ci(name, "rbstemsplus-bridge-win-x64.dll");
    case SUPPORT_ADMIN_WORKER: return same_ci(name, ADMIN_WORKER_NAME);
    case SUPPORT_REPORTS: return support_report_file(name);
    case SUPPORT_KEPT: return support_kept_file(name);
    case SUPPORT_FILES: return NAMED(name, payload) || NAMED(name, downloads);
    case SUPPORT_UPDATE: return NAMED(name, update) || NAMED(name, payload) || same_ci(name, "rbstemsplus-windows.zip");
    case SUPPORT_ORIGINALS: {
        char hash[65];
        if (n <= 64 || (!same_ci(name + 64, ".onnx") && !same_ci(name + 64, ".engine") &&
                        !same_ci(name + 64, ".onnx.damaged"))) return 0;
        memcpy(hash, name, 64);
        hash[64] = 0;
        return lower_hex64(hash);
    }
    default: return 0;
    }
}

static int support_subfolder(support_folder folder, const char *name) {
    if (folder == SUPPORT_ROOT) {
        static const char *const dirs[] = {"Logs", "originals", "payload", "installed-release", "update", "kept", "admin-source"};
        for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) if (same_ci(name, dirs[i])) return SUPPORT_LOGS + (int)i;
    }
    if (folder == SUPPORT_PAYLOAD && name[0] >= '0' && name[0] <= '9' && name[strspn(name, "0123456789.")] == 0)
        return SUPPORT_FILES;
    if (folder == SUPPORT_RELEASE && same_ci(name, "prev")) return SUPPORT_FILES;
    if (folder == SUPPORT_ADMIN && same_ci(name, ADMIN_APP_COPY_DIR)) return SUPPORT_ADMIN_WORKER;
    if (folder == SUPPORT_LOGS && same_ci(name, "reports")) return SUPPORT_REPORTS;
    return -1;
}

/* Every level selects the app's artifact names and types. Unknown trees are left intact,
 * including any legacy cache reachable by a drive alias or left after a location change. */
static int remove_support_entries(const native_steps_env *e, plat_dir d, support_folder folder, int keep_originals,
                                  int *kept, int *originals_kept) {
    loc_list l = {0};
    int bad = own_dir_each(d, loc_each, &l) || l.failed;
    for (int i = 0; !bad && i < l.n; i++) {
        const char *name = l.names[i];
        plat_stat st;
        int r = own_stat_at(d, name, &st);
        if (r == ENOENT) continue;
        if (r) { *kept = 1; continue; }
        int sub = support_subfolder(folder, name);
        if (st.kind == PLAT_FILE && support_file(folder, name)) {
            r = plat_unlink_at(d, name);
        } else if (st.kind == PLAT_DIR && sub >= 0) {
            if (keep_originals && sub == SUPPORT_ORIGINALS) {
                *originals_kept = 1;
                continue;
            }
            plat_dir child;
            r = plat_dir_open_at(d, name, &child);
            if (!r) {
                plat_stat opened;
                r = plat_dir_stat(child, &opened);
                if (!r && !plat_id_same(opened.id, st.id)) r = EBUSY;
                int child_kept = 0;
                if (!r) r = remove_support_entries(e, child, (support_folder)sub, keep_originals, &child_kept, originals_kept);
                plat_dir_close(child);
                if (!r && !child_kept) r = e->remove_empty_dir_at(e->ctx, d, name, st.id);
                if (child_kept) *kept = 1;
            }
        } else {
            refused(e, e->support, name);
            *kept = 1;
        }
        if (r && r != ENOENT) *kept = 1;
    }
    loc_free(&l);
    return bad;
}

int native_remove_own_folders(const native_steps_env *e, const char *expected_support, int keep_originals) {
    int bad = 0, kept = 0;
    char line[NS_PATH_MAX + 64];
    /* 1. only the support folder this account's LocalAppData names (review S53), whatever the
     *    shell's paths say */
    if (!e->support || !e->support[0] || !expected_support || !expected_support[0] || !same_ci(e->support, expected_support)) {
        refused(e, e->support && e->support[0] ? e->support : NULL, NULL);
        bad = 1;
    } else {
        plat_dir sd;
        int r = own_dir_open(e->support, &sd);
        if (r == ELOOP || r == ENOTDIR) {   /* a link, a reparse point or a file at support: left (S59) */
            refused(e, e->support, NULL);
            bad = 1;
        } else if (r && r != ENOENT) {   /* fail closed (S52): nothing in support deleted */
            snprintf(line, sizeof line, "couldn't open %s (error %d): nothing deleted there", e->support, r);
            e->log(e->ctx, line);
            bad = 1;
        } else if (!r) {
            int preserve = 0;
            bad = remove_support_entries(e, sd, SUPPORT_ROOT, keep_originals, &kept, &preserve);
            plat_stat st;
            int sid = plat_dir_stat(sd, &st);
            plat_dir_close(sd);
            bad |= kept || sid;
            if (!bad && !preserve) bad = remove_support(e, e->support, st.id);
        }
    }
    return bad;
}

/* ---------------------------------------------------------------- LEFTOVER */

void native_leftover(const char *support, int32_t self_pid, int64_t (*start)(void *ctx, int32_t pid), void (*nap)(void *ctx), void *ctx,
                     char *id, size_t idn, char *label, size_t labeln) {
    char p[NS_PATH_MAX], *t = NULL;
    size_t tn = 0;
    id[0] = label[0] = 0;
    if (!support || snprintf(p, sizeof p, "%s" SEP "busy", support) >= (int)sizeof p || plat_read_file(p, 64 * 1024, &t, &tn)) return;
    arena a = {0};
    busy_marker m;
    if (busy_marker_parse(&a, t, tn, &m) == 1) {
        plat_stat st;
        int32_t hw = !plat_stat_path(p, 0, &st);
        int64_t w = hw ? st.mtime : 0;
        int live = 1;
        for (int waited = 0;; waited++) {
            int64_t started = start(ctx, m.pid);
            live = started == -2 || busy_marker_live(&m, started, hw, w);
            if (live && m.pid == self_pid) break;   /* this process's own marker: none */
            if (!live || waited == 20) break;
            nap(ctx);
        }
        if (!live && m.has_action) {
            snprintf(id, idn, "%.*s", (int)m.id.n, m.id.s);
            snprintf(label, labeln, "%.*s", (int)m.label.n, m.label.s);
        }
    }
    arena_free(&a);
    free(t);
}

void native_state_line(char *out, size_t n, const char *rekordbox, const char *engine, const char *model_sha, int bridge, int chosen_model,
                       int chosen_cache) {
    snprintf(out, n, "rekordbox %s, engine %s, model %.12s, bridge %s, chosen (model: %s, cache: %s)", rekordbox ? rekordbox : "",
             engine ? engine : "", model_sha ? model_sha : "", bridge ? "yes" : "no", chosen_model ? "true" : "false",
             chosen_cache ? "true" : "false");
}

/* ---------------------------------------------------------------- R1's folder (review CW1) */

int native_r1_assessed(const inv_list *list, const inv_assess_row *rows, const char *folder) {
    for (int i = 0; i < list->n; i++) {
        const inv_install *r = &list->it[i];
        if (r->trust != INV_STALE && !strcasecmp(r->folder, folder)) return rows[i].r1;
    }
    return -1;
}

int native_r1_current(const compat_os *os, const char *rb_parent, char *folder, size_t n, char *version, size_t vn) {
    if (version && vn) version[0] = 0;
    if (!folder || n == 0) return 0;
    folder[0] = 0;
    compat_rekordbox *rb = calloc(COMPAT_ENTRIES_MAX, sizeof *rb);
    if (!rb) return 0;
    int cur = -1, state = 0, ok = 0;
    compat_find_rekordbox(os, rb_parent, rb, COMPAT_ENTRIES_MAX, &cur, &state);
    if (cur >= 0 && state == COMPAT_RB_OK && rb[cur].folder[0]) {
        ok = snprintf(folder, n, "%s", rb[cur].folder) < (int)n;
        if (ok && version && vn) ok = snprintf(version, vn, "%s", rb[cur].folder_version) < (int)vn;
    }
    if (!ok) {
        folder[0] = 0;
        if (version && vn) version[0] = 0;
    }
    free(rb);
    return ok;
}

int native_r1_folder(const compat_os *os, const char *rb_parent, char *folder, size_t n) {
    return native_r1_current(os, rb_parent, folder, n, NULL, 0);
}

/* ---------------------------------------------------------------- the location choice (plan N3) */

static int choice_path_ok(const char *p) {
#if defined(_WIN32)
    return paths_location_ok_win(p);
#else
    return paths_location_ok(p);
#endif
}

/* A path's syntactic root: Windows "X:\" or "\\?\Volume{GUID}\" (paths_root_win), Mac "/". */
static size_t choice_root(const char *p) {
#if defined(_WIN32)
    return paths_root_win(p);
#else
    return p[0] == '/' ? 1 : 0;
#endif
}

#if !defined(_WIN32)
/* The host tests use actual mount directories in place of Windows volume GUIDs.
 * Look them up by prefix, so no link below the mount can select a different root.
 * macOS can spell the temp directory under /var or /private/var, including in the mount table. */
static int choice_host_root(const char **path, char canonical[NS_PATH_MAX], char mount[NS_PATH_MAX]) {
    static const char private[] = "/private", var[] = "/var";
    const size_t pn = sizeof private - 1, vn = sizeof var - 1;
    const char *alias = !strncmp(*path, private, pn) ? *path + pn : *path;
    if (strncmp(alias, var, vn) || (alias[vn] && alias[vn] != '/')) alias = NULL;
    if (alias && alias == *path) {
        if (snprintf(canonical, NS_PATH_MAX, "%s%s", private, alias) >= NS_PATH_MAX) return ENAMETOOLONG;
        *path = canonical;
    }
    int system;
    int r = plat_volume_holding(*path, mount, NS_PATH_MAX, &system);
    if (r || !alias) return r;
    char alias_mount[NS_PATH_MAX];
    r = plat_volume_holding(alias, alias_mount, sizeof alias_mount, &system);
    if (!r && alias_mount[1] && pn + strlen(alias_mount) > strlen(mount)) {
        if (snprintf(mount, NS_PATH_MAX, "%s%s", private, alias_mount) >= NS_PATH_MAX) return ENAMETOOLONG;
    }
    return r;
}
#endif

/* Open from the volume root, refusing reparse points and volume changes along the way.
 * EEXIST means that the optional excluded directory was encountered. */
static int open_plain_path_except(const char *path, const plat_id *exclude, plat_dir *out, plat_dir *parent) {
    if (strlen(path) >= NS_PATH_MAX) return EINVAL;
#if defined(_WIN32)
    size_t root = choice_root(path);
    if (!root) return EINVAL;
    char top[64];
    snprintf(top, sizeof top, "%.*s", (int)root, path);
#else
    char top[NS_PATH_MAX], canonical[NS_PATH_MAX];
    int err = choice_host_root(&path, canonical, top);
    if (err) return err;
    size_t root = strlen(top);
    if (path[root] == '/') root++;
#endif
    char rel[NS_PATH_MAX];
    snprintf(rel, sizeof rel, "%s", path + root);
    plat_dir d, prev = plat_dir_cwd();
    int r = plat_dir_open(top, &d);
    if (r) return r;
    plat_stat st = {0};
    r = plat_dir_stat(d, &st);
    uint64_t vol = st.id.vol;
    char *c = rel;
    while (!r) {
        if (exclude && plat_id_same(*exclude, st.id)) {
            r = EEXIST;
            break;
        }
        if (!*c) break;
        char *end = strchr(c, SEP[0]);
        if (end) *end = 0;
        plat_dir next;
        r = plat_dir_open_at(d, c, &next);
        if (r) break;
        if (parent) {
            plat_dir_close(prev);
            prev = d;
        } else {
            plat_dir_close(d);
        }
        d = next;
        r = plat_dir_stat(d, &st);
        if (!r && st.id.vol != vol) r = ELOOP;
        c = end ? end + 1 : c + strlen(c);
    }
    if (r) {
        plat_dir_close(prev);
        plat_dir_close(d);
    } else {
        *out = d;
        if (parent) *parent = prev;
    }
    return r;
}

static int open_plain_path(const char *path, plat_dir *out) { return open_plain_path_except(path, NULL, out, NULL); }

/* LocalAppData can be redirected. Match the resolved support directory by identity. */
static int open_choice_outside_support(const char *support, const char *path, plat_dir *out, plat_dir *parent) {
    plat_dir sd;
    int r = support && support[0] ? plat_dir_open_follow(support, &sd) : ENOENT;
    if (r == ENOENT) return open_plain_path_except(path, NULL, out, parent);
    if (r) return r;
    plat_stat st;
    r = plat_dir_stat(sd, &st);
    if (!r) r = open_plain_path_except(path, &st.id, out, parent);
    plat_dir_close(sd);
    return r;
}

static int choice_support_conflict(const char *support, const char *path) {
    plat_dir d;
    int r = open_choice_outside_support(support, path, &d, NULL);
    if (!r) plat_dir_close(d);
    return r && r != ENOENT;
}

/* a then b with one separator between them (a may end in one: a volume's root): 0, or -1 */
static int cjoin(char *out, size_t n, const char *a, const char *b) {
    size_t al = strlen(a);
    int k = snprintf(out, n, "%s%s%s", a, al && a[al - 1] == SEP[0] ? "" : SEP, b);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

/* the last component of p (after its last separator) */
static const char *last_name(const char *p) {
    const char *s = strrchr(p, SEP[0]);
    return s ? s + 1 : p;
}

/* our folder's path for a folder chosen (real): itself when it is named so, else our folder in it */
static int our_folder(const char *real, char *out, size_t n) {
    if (!strcmp(last_name(real), PATHS_CACHE_FOLDER)) return snprintf(out, n, "%s", real) < (int)n ? 0 : -1;
    return cjoin(out, n, real, PATHS_CACHE_FOLDER);
}

/* every component of p after its first `from` bytes a real folder, none followed: 0; 1 a link,
 * junction, mount point or other reparse point; 2 missing, not a folder or unreadable */
static int walk_plain(const char *p, size_t from) {
    char b[NS_PATH_MAX];
    size_t n = strlen(p);
    if (n >= sizeof b || from > n) return 2;
    memcpy(b, p, n + 1);
    for (size_t i = from; i <= n; i++) {
        if (i < n && b[i] != SEP[0]) continue;
        if (i == from || b[i - 1] == SEP[0]) continue;   /* the root itself, or an empty component */
        char c = b[i];
        b[i] = 0;
        plat_stat st;
        int e = plat_stat_path(b, 0, &st);
        b[i] = c;
        if (e) return 2;
        if (st.kind == PLAT_OTHER) return 1;
        if (st.kind != PLAT_DIR) return 2;
    }
    return 0;
}

/* whether the folder at dir holds a marker of any kind (fileType != nil) */
static int has_marker(const char *dir) {
    char p[NS_PATH_MAX];
    plat_stat st;
    return !cjoin(p, sizeof p, dir, MARKER_NAME) && !plat_stat_path(p, 0, &st);
}

int native_choice_probe_folder(const native_choice_ops *o, const char *folder, native_choice_probe *out) {
    memset(out, 0, sizeof *out);
    if (!folder || !folder[0]) return 1;
    int refusal = VOL_REFUSE_UNREADABLE;
    if (o->volume(o->ctx, folder, &refusal, out->mount, sizeof out->mount, out->real, sizeof out->real)) refusal = VOL_REFUSE_UNREADABLE;
    if (o->drive_name) o->drive_name(o->ctx, refusal == VOL_OK && out->real[0] ? out->real : folder, 0, out->drive, sizeof out->drive);
    static const int64_t codes[] = {[VOL_OK] = 0, [VOL_REFUSE_UNREADABLE] = 9, [VOL_REFUSE_NETWORK] = 2, [VOL_REFUSE_INTERNAL] = 1,
                                    [VOL_REFUSE_FILE_SYSTEM] = 7, [VOL_REFUSE_READ_ONLY] = 3};
    out->code = refusal >= VOL_OK && refusal <= VOL_REFUSE_READ_ONLY ? codes[refusal] : 9;
    if (!out->code && (!out->real[0] || !out->mount[0])) out->code = 9;
    if (out->code) {
        out->real[0] = out->mount[0] = 0;
        return 0;
    }
    /* no link from the root down: the folder as chosen, each component a real folder */
#if defined(_WIN32)
    size_t root = choice_root(folder);
#else
    char top[NS_PATH_MAX], canonical[NS_PATH_MAX];
    size_t root = choice_host_root(&folder, canonical, top) ? 0 : strlen(top);
#endif
    int w = root ? walk_plain(folder, root) : 1;
    if (w) {
        out->code = w == 1 ? 6 : 9;
        return 0;
    }
    size_t ml = strlen(out->mount), rl = strlen(out->real);
    int msep = out->mount[ml - 1] == SEP[0], at_root = !strcmp(out->real, out->mount);
    if (!at_root && (rl <= ml || strncmp(out->real, out->mount, ml) || (!msep && out->real[ml] != SEP[0]))) {
        out->code = 6;   /* not below its volume's root */
        return 0;
    }
    /* not inside our folder, no marker above it up to the root (its own is fine: it is our folder) */
    if (!at_root) {
        if (has_marker(out->mount)) {
            out->code = 5;
            return 0;
        }
        char p[NS_PATH_MAX];
        snprintf(p, sizeof p, "%s", out->real);
        size_t lastc = (size_t)(last_name(p) - p), start = msep ? ml : ml + 1;
        for (size_t c = start, i = start; i < lastc; i++) {
            if (p[i] != SEP[0]) continue;
            size_t cl = i - c;   /* p[c..i): a folder above the one chosen */
            if (cl == strlen(PATHS_CACHE_FOLDER) && !strncasecmp(p + c, PATHS_CACHE_FOLDER, cl)) {
                out->code = 5;
                return 0;
            }
            p[i] = 0;
            int m = has_marker(p);
            p[i] = SEP[0];
            if (m) {
                out->code = 5;
                return 0;
            }
            c = i + 1;
        }
    }
    char dir[NS_PATH_MAX];
    if (our_folder(out->real, dir, sizeof dir) || !choice_path_ok(dir)) out->code = 8;
    else if (choice_support_conflict(o->support, dir)) {
        out->code = 5;
        out->support_conflict = 1;
    }
    return 0;
}

/* Read the marker ID from the opened directory. */
static int marker_id_at(plat_dir d, char id[40]) {
    plat_file f;
    int64_t size;
    int e = plat_file_open_at(d, MARKER_NAME, MARKER_MAX, &size, &f);
    if (e) return -1;
    char b[MARKER_MAX + 1];
    size_t got = 0;
    e = size < 0 || size > MARKER_MAX ? -1 : plat_read(f, b, (size_t)size, &got);
    plat_file_close(f);
    if (e || got != (size_t)size) return -1;
    b[got] = 0;
    for (size_t i = 0; i < got;) {
        const char *l = b + i, *nl = memchr(l, '\n', got - i);
        size_t ll = nl ? (size_t)(nl - l) : got - i;
        i += ll + 1;
        if (ll < 3 || strncmp(l, "id=", 3)) continue;
        if (ll != 39) return -1;   /* every id the same: the first decides */
        char want[40];
        memcpy(want, l + 3, 36);
        want[36] = 0;
        if (!cfg_uuid_ok(want) || !marker_ok(b, got, want)) return -1;
        memcpy(id, want, 37);
        return 0;
    }
    return -1;
}

/* the location hint (readLocationHint) */
typedef struct {
    char mount[NS_PATH_MAX], relative[NS_PATH_MAX];
    char own[16][40];
    int nown;
} choice_hint;

static void hint_read(const char *support, choice_hint *h) {
    memset(h, 0, sizeof *h);
    char p[NS_PATH_MAX], *b = NULL;
    size_t n = 0;
    if (!support || join2(p, sizeof p, support, NS_LOCATION_HINT) || plat_read_file(p, 64 * 1024, &b, &n)) return;
    for (size_t i = 0; i < n;) {
        const char *s = b + i, *nl = memchr(s, '\n', n - i), *end = nl ? nl : b + n, *eq = memchr(s, '=', (size_t)(end - s));
        i += (size_t)(end - s) + 1;
        if (!eq || memchr(s, 0, (size_t)(end - s))) continue;
        size_t kl = (size_t)(eq - s), vl = (size_t)(end - eq - 1);
        if (kl == 5 && !memcmp(s, "mount", 5) && vl < sizeof h->mount) snprintf(h->mount, sizeof h->mount, "%.*s", (int)vl, eq + 1);
        else if (kl == 8 && !memcmp(s, "relative", 8) && vl < sizeof h->relative)
            snprintf(h->relative, sizeof h->relative, "%.*s", (int)vl, eq + 1);
        else if (kl == 3 && !memcmp(s, "own", 3) && vl == 36 && h->nown < 16) {
            snprintf(h->own[h->nown], sizeof h->own[0], "%.36s", eq + 1);
            if (cfg_uuid_ok(h->own[h->nown])) h->nown++;
        }
    }
    free(b);
}

static int id_cmp(const void *a, const void *b) { return strcmp(a, b); }

/* writeLocationHint: mount, relative, then the ids upper-cased, sorted, each once */
static int hint_write(const char *support, const choice_hint *h) {
    if (strpbrk(h->mount, "\r\n") || strpbrk(h->relative, "\r\n")) return -1;
    char ids[16][40];
    int n = 0;
    for (int i = 0; i < h->nown && n < 16; i++, n++) {
        snprintf(ids[n], sizeof ids[n], "%s", h->own[i]);
        for (char *c = ids[n]; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    }
    qsort(ids, (size_t)n, sizeof ids[0], id_cmp);
    char text[2 * NS_PATH_MAX + 16 * 48 + 32];
    int k = snprintf(text, sizeof text, "mount=%s\nrelative=%s\n", h->mount, h->relative);
    for (int i = 0; i < n && k > 0 && (size_t)k < sizeof text; i++)
        if (!i || strcmp(ids[i], ids[i - 1])) k += snprintf(text + k, sizeof text - (size_t)k, "own=%s\n", ids[i]);
    if (k < 0 || (size_t)k >= sizeof text) return -1;
    return native_support_write(support, NS_LOCATION_HINT, text, (size_t)k);
}

/* readConfigState's location now (common/cfg_ini.c, the bridge's reader, as the measure reads it):
 * 0 This PC, 1 chosen (dir and id), 2 set wrongly; -1 unreadable */
static int config_location(const char *support, char *dir, size_t dn, char *id, size_t in) {
    dir[0] = id[0] = 0;
    char p[NS_PATH_MAX], *b = NULL;
    size_t n = 0;
    if (!support || join2(p, sizeof p, support, "config.ini")) return -1;
    int e = FAULT(NS_FAULT_OPEN, p);
    if (!e) e = plat_read_file(p, CFG_MAX, &b, &n);
    if (e == ENOENT) return 0;
    cfg_config *c = calloc(1, sizeof *c);
    int loc = !c || e || cfg_parse(b, n, c, NULL) < 0 ? -1 : c->location == 1 ? 1 : c->location == 0 ? 0 : 2;
    if (loc == 1) {
        snprintf(dir, dn, "%s", c->dir);
        snprintf(id, in, "%s", c->id);
    }
    free(c);
    free(b);
    return loc;
}

int native_choice_discover(const char *support, const char *real, const char *mount, native_choice_found *out) {
    memset(out, 0, sizeof *out);
    if (!real || !real[0] || !mount || !mount[0]) return 1;
    config_location(support, out->current, sizeof out->current, out->current_id, sizeof out->current_id);
    /* relativeOnDrive: the hint's, else config.ini's folder below its root */
    choice_hint hint;
    hint_read(support, &hint);
    const char *rel = hint.relative[0] ? hint.relative : NULL;
    size_t cr = out->current[0] ? choice_root(out->current) : 0;
    if (!rel && cr && out->current[cr]) rel = out->current + cr;
#if defined(_WIN32)
    if (cr && !vol_guid_root(out->current)) {
        /* Treat an explicit picker selection as a new choice when the saved volume is unknown. */
        rel = NULL;
        out->current_id[0] = 0;
    }
#endif
    char cand[2][NS_PATH_MAX];
    if (our_folder(real, cand[0], sizeof cand[0])) return 0;
    int nc = 1;
    if (rel && location_rel_ok(rel) && !cjoin(cand[1], sizeof cand[1], mount, rel)) nc = 2;
    for (int i = 0; i < nc; i++) {
        plat_dir d;
        int r = open_choice_outside_support(support, cand[i], &d, NULL);
        if (r == ENOENT) continue;
        if (r && r != ENOTDIR && r != ELOOP) {
            if (i > 0) continue;
            out->code = 1;
            out->support_conflict = 1;
            snprintf(out->found, sizeof out->found, "%s", cand[i]);
            return 0;
        }
        if (!r) {
            int owned = !marker_id_at(d, out->id) && !cache_identity_at(d, cand[i], out->identity);
            plat_dir_close(d);
            if (owned) {
                out->code = 2;
                snprintf(out->found, sizeof out->found, "%s", cand[i]);
                return 0;
            }
        }
        if (i == 0) {
            out->code = 1;
            snprintf(out->found, sizeof out->found, "%s", cand[i]);
            return 0;
        }
    }
    return 0;
}

int64_t native_choice_size(const char *dir) {
    return dir && dir[0] ? vol_entries_size(dir) : 0;
}

typedef struct {
    plat_dir d;
    int ok;
} writable_ctx;

static int writable_each(void *ctx, const char *name, const plat_stat *st) {
    writable_ctx *w = ctx;
    if (!lower_hex64(name)) return 0;
    plat_dir m;
    if (st->kind != PLAT_DIR || plat_dir_open_at(w->d, name, &m)) {
        w->ok = 0;
        return 1;
    }
    if (plat_dir_writable(m)) w->ok = 0;
    plat_dir_close(m);
    return w->ok ? 0 : 1;
}

static int choice_writable_at(plat_dir d) {
    writable_ctx w = {d, 1};
    return !plat_dir_writable(d) && !plat_dir_each(d, writable_each, &w) && w.ok;
}

int native_choice_writable(const char *dir) {
    plat_dir d;
    if (!dir || !dir[0] || open_plain_path(dir, &d)) return 0;
    int ok = choice_writable_at(d);
    plat_dir_close(d);
    return ok;
}

/* the folder above dir into out (a root keeps its separator): 0, or -1 */
static int parent_of(const char *dir, char *out, size_t n) {
    const char *s = strrchr(dir, SEP[0]);
    if (!s || !s[1]) return -1;
    size_t k = (size_t)(s - dir), root = choice_root(dir);
    if (k + 1 == root || k == 0) k++;   /* "X:\", "\\?\Volume{…}\", the Mac's "/" */
    if (k >= n) return -1;
    memcpy(out, dir, k);
    out[k] = 0;
    return 0;
}

/* only an empty folder `name` in p goes, its marker first (createLocationFolder's undo,
 * removeNewLocationFolder): nothing else in it is touched */
static void remove_new_at(const native_steps_env *e, plat_dir p, const char *dir, plat_dir d,
                          const char *id, const char *identity) {
    const char *name = last_name(dir);
#if defined(_WIN32) || defined(NATIVE_STEPS_TEST_HOOKS)
    plat_dir held;
    char opened[NS_CACHE_IDENTITY_MAX];
    if (!cache_open_at(p, name, &held, CACHE_OPEN_REMOVE)) {
        if (!cache_identity_at(held, dir, opened) && !strcmp(identity, opened) &&
            !marker_check_at(held, id) && !unlink_cache_marker(held, MARKER_NAME) &&
            dispose_verified_cache(e, p, name, held)) restore_external_marker(e, held, id);
        plat_dir_close(held);
    }
#else
    (void)identity;
    plat_stat st;
    int ok = !plat_dir_stat(d, &st) && st.kind == PLAT_DIR;
    if (ok) ok = !marker_check_at(d, id) && !plat_unlink_at(d, MARKER_NAME);
    if (ok) e->remove_empty_dir_at(e->ctx, p, name, st.id);
#endif
    plat_dir_close(d);
}

#if !defined(_WIN32)
/* Host stand-in for the Windows FILE_CREATE call. */
static int plat_mkdir_open_at(plat_dir d, const char *name, int not_indexed, plat_dir *out) {
    (void)not_indexed;
    int r = plat_mkdir_at(d, name);
    return r ? r : plat_dir_open_at(d, name, out);
}
#endif

static int native_pc_creation_identity(plat_dir d, const char *path, int created,
                                       char identity[NS_CACHE_IDENTITY_MAX]) {
    (void)path;
    int err = created ? FAULT(NS_FAULT_OPEN_CREATED, path) : 0;
    return err ? err : cache_identity_at(d, NULL, identity);
}

void native_pc_cache_create(const native_steps_env *e, const char *cache_default) {
    plat_dir s;
    int err = open_pc_support(e, cache_default, &s);
    if (!err) {
        err = cache_owner_init(s, "Cache", cache_default, native_pc_creation_identity, plat_mkdir_open_at);
        plat_dir_close(s);
    }
    if (err && e->log) {
        char line[160];
        snprintf(line, sizeof line, "This PC cache unavailable (%s, error %d)", cache_owner_reason(err), err);
        e->log(e->ctx, line);
    }
}

static int choice_volume_eligible(const native_choice_ops *o, const char *path) {
#if defined(_WIN32)
    (void)o;
    return vol_saved_refusal(path) == VOL_OK;
#else
    int refusal;
    char root[NS_PATH_MAX], real[NS_PATH_MAX];
    return o && o->volume && !o->volume(o->ctx, path, &refusal, root, sizeof root, real, sizeof real) && refusal == VOL_OK;
#endif
}

static int choice_create(const native_choice_ops *o, const native_steps_env *e, const char *real, char dir[NS_PATH_MAX], char id[40],
                         char err[128], char identity[NS_CACHE_IDENTITY_MAX], native_location_io *io) {
    dir[0] = id[0] = err[0] = identity[0] = 0;
    char want[NS_PATH_MAX], parent[NS_PATH_MAX];
    if (!real || our_folder(real, want, sizeof want) || parent_of(want, parent, sizeof parent)) {
        snprintf(err, 128, "%s", strerror(ENAMETOOLONG));
        return 2;
    }
    const char *name = last_name(want);
    plat_dir p, d;
    int r = open_choice_outside_support(e->support, parent, &p, NULL);
    if (r) {
        snprintf(err, 128, "%s", strerror(r));
        return 2;
    }
    if (cache_identity_at(p, want, identity) || !choice_volume_eligible(o, want)) {
        plat_dir_close(p);
        return 4;
    }
    char nid[40] = "", text[96];
    if (o->new_id(o->ctx, nid) || !cfg_uuid_ok(nid)) {
        snprintf(err, 128, "%s", strerror(EIO));
        plat_dir_close(p);
        return 3;
    }
    if (io && !native_location_begin_mutation(io, 0)) {
        plat_dir_close(p);
        return NS_CHOICE_TIMED_OUT;
    }
    if ((r = plat_mkdir_open_at(p, name, 1, &d))) {
        if (r != EEXIST) snprintf(err, 128, "%s", strerror(r));
        plat_dir_close(p);
        return r == EEXIST ? 1 : 2;
    }
    r = FAULT(NS_FAULT_OPEN_CREATED, want);
    if (!r) r = cache_identity_at(d, want, identity);
    if (!r && native_location_io_expired(io)) r = ETIMEDOUT;
    if (!r) {
        int k = snprintf(text, sizeof text, "id=%s\nformat=%s\n", nid, MARKER_FORMAT);
        r = win_write_new_at(d, MARKER_NAME, text, (size_t)k);
    }
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    /* FILE_CREATE gave us DELETE access and protects this handle against replacement. */
    int left_folder = r && dispose_verified_cache(e, p, name, d);
#endif
    plat_dir_close(d);
    plat_dir_close(p);
    if (r) {
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
        identity[0] = 0;
        snprintf(err, 128, "%s", strerror(r));
        return left_folder ? NS_CHOICE_CREATE_LEFT_FOLDER : 3;
#else
        snprintf(err, 128, "the attempt may have left a folder here. Choose another parent folder. Error %d", r);
        return 3;
#endif
    }
#if !defined(_WIN32)
    if (o->not_indexed) o->not_indexed(o->ctx, want);
#endif
    snprintf(dir, NS_PATH_MAX, "%s", want);
    snprintf(id, 40, "%s", nid);
    return 0;
}

int native_choice_create(const native_choice_ops *o, const native_steps_env *e, const char *real, char dir[NS_PATH_MAX], char id[40],
                         char err[128], char identity[NS_CACHE_IDENTITY_MAX]) {
    return choice_create(o, e, real, dir, id, err, identity, NULL);
}

/* Once replacement starts the caller stays in its saving state until the outcome is known. */
static int replace_choice_config(void *ctx, plat_dir d, const char *tmp, const char *name) {
    native_location_io *io = ctx;
    if (!native_location_begin_mutation(io, 1)) return ETIMEDOUT;
    int r = FAULT(NS_FAULT_LOCATION_CONFIG_REPLACE, name);
    if (!r) r = plat_rename_at(d, tmp, d, name);
    native_location_end_replace(io, !r);
    return r;
}

/* config.ini rewritten with values set and remove's keys gone (writeConfigValues): 0, or -1 */
static int config_rewrite(const char *support, const native_config_pair *v, int nv, const char *const *rm, int nrm,
                          native_location_io *io) {
    char p[NS_PATH_MAX], *b = NULL, *t = NULL;
    size_t n = 0, tn = 0;
    if (!support || join2(p, sizeof p, support, "config.ini")) return -1;
    int e = FAULT(NS_FAULT_LOCATION_CONFIG_READ, p);
    if (!e) e = plat_read_file(p, CFG_MAX, &b, &n);
    if (e == ENOENT) b = NULL, n = 0;
    else if (e || !native_config_readable(b, n)) {
        free(b);
        return -1;
    }
    int r = native_config_rewrite(b, n, v, nv, rm, nrm, &t, &tn);
    if (!r && !io) r = native_support_write(support, "config.ini", t, tn);
    else if (!r && !native_location_io_expired(io)) {
        plat_dir d;
        r = open_support_for_write(support, &d, io);
        if (!r) {
            if (native_location_io_expired(io)) r = ETIMEDOUT;
            else r = win_write_with_replace(d, "config.ini", t, tn, replace_choice_config, io);
            plat_dir_close(d);
        }
    } else if (!r) r = ETIMEDOUT;
    free(b);
    free(t);
    return r ? -1 : 0;
}

static int choice_write(const native_choice_ops *o, const native_steps_env *e, const char *dir, const char *id, const char *mount,
                        int created, const char *identity, const char *label, native_location_io *io) {
    static const char *const keys[] = {"cache_dir", "cache_id"};
    char rd[NS_PATH_MAX], ri[64];
    if (!dir || !dir[0]) {   /* This PC */
        if (config_rewrite(e->support, NULL, 0, keys, 2, io)) return 1;
        if (native_location_io_expired(io)) return NS_CHOICE_SAVED_UNCHECKED;
        if (config_location(e->support, rd, sizeof rd, ri, sizeof ri) != 0)
            return 1;
        native_support_remove(e->support, NS_PENDING_DELETION);
        return 0;
    }
    char quoted[NS_PATH_MAX + 4], opened[NS_CACHE_IDENTITY_MAX];
    if (!id || !identity || !choice_path_ok(dir) || !cfg_uuid_ok(id) ||
        snprintf(quoted, sizeof quoted, "\"%s\"", dir) >= (int)sizeof quoted)
        return 2;
    plat_dir d, p;
    if (open_choice_outside_support(e->support, dir, &d, &p)) return 2;
    if (cache_identity_at(d, dir, opened) || strcmp(identity, opened) || !choice_volume_eligible(o, dir) ||
        marker_check_at(d, id) || !choice_writable_at(d)) {
        plat_dir_close(d);
        plat_dir_close(p);
        return 2;
    }
    native_config_pair v[2] = {{"cache_dir", quoted}, {"cache_id", id}};
    int written = !config_rewrite(e->support, v, 2, NULL, 0, io);
    if (!written && created) remove_new_at(e, p, dir, d, id, opened);
    else plat_dir_close(d);
    plat_dir_close(p);
    if (!written) return 1;
    if (native_location_io_expired(io)) return NS_CHOICE_SAVED_UNCHECKED;
    if (config_location(e->support, rd, sizeof rd, ri, sizeof ri) != 1 || strcmp(rd, dir) || strcmp(ri, id)) return 1;
    /* hintFor: the drive's root and our folder below it; the ids this account made */
    choice_hint h;
    hint_read(e->support, &h);
    snprintf(h.mount, sizeof h.mount, "%s", mount ? mount : "");
    size_t ml = strlen(h.mount);
    h.relative[0] = 0;
    if (ml && !strncmp(dir, h.mount, ml)) {
        const char *r = dir + ml;
        while (*r == SEP[0]) r++;
        snprintf(h.relative, sizeof h.relative, "%s", r);
    }
    if (created) {
        if (h.nown == 16) memmove(h.own[0], h.own[1], sizeof h.own[0] * 15), h.nown--;
        snprintf(h.own[h.nown++], sizeof h.own[0], "%s", id);
    }
    hint_write(e->support, &h);   /* best effort, as the Mac's */
    if (label) native_location_remember_label(e->support, dir, label);
    else if (!io && o && o->drive_name) {   /* standalone test/diagnostic caller */
        char name[256];
        o->drive_name(o->ctx, dir, 1, name, sizeof name);
    }
    return 0;
}

int native_choice_write(const native_choice_ops *o, const native_steps_env *e, const char *dir, const char *id, const char *mount,
                        int created, const char *identity) {
    return choice_write(o, e, dir, id, mount, created, identity, NULL, NULL);
}

typedef struct {
    native_choice_commit commit;
    native_choice_ops ops;
    int (*remove_empty)(void *, plat_dir, const char *, plat_id);
    int rc;
} choice_commit_job;

static void run_choice_commit(native_location_io *io, void *ctx) {
    choice_commit_job *j = ctx;
    native_choice_commit *c = &j->commit;
    native_steps_env e = {.support = c->support, .ctx = j->ops.ctx, .remove_empty_dir_at = j->remove_empty};
    if (c->create)
        j->rc = choice_create(&j->ops, &e, c->path, c->dir, c->id, c->err, c->identity, io);
    else {
        j->rc = choice_write(&j->ops, &e, c->path, c->id, c->mount, c->created, c->identity,
                              c->has_label ? c->label : NULL, io);
        if (!j->rc && !c->path[0] && c->cache_default[0]) native_pc_cache_create(&e, c->cache_default);
    }
}

int native_choice_commit_run(native_choice_commit *c, const native_choice_ops *ops,
                             int (*remove_empty)(void *, plat_dir, const char *, plat_id), uint64_t deadline) {
    c->left_folder = 0;
    choice_commit_job job = {*c, *ops, remove_empty, 1};
    int mutating = 0, replaced = 0;
    int local = !c->create && !c->path[0];
    int rc = native_location_commit_wait(run_choice_commit, &job, sizeof job, deadline, c->support, local, &mutating, &replaced);
    if (rc) {
        if (replaced) return NS_CHOICE_SAVED_UNCHECKED;
        c->left_folder = c->create ? mutating : c->created;
        return rc == NL_TIMEOUT || rc == NL_BUSY ? NS_CHOICE_TIMED_OUT : 1;
    }
    *c = job.commit;
    return job.rc;
}

static int models_each(void *ctx, const char *name, const plat_stat *st) {
    return lower_hex64(name) ? loc_each(ctx, name, st) : 0;
}

/* Keep a matching retry record in place. Only retire the record for this confirmed cache. */
static int pc_pending_update(plat_dir support, const char *identity, int retry) {
    plat_file f;
    int64_t n;
    char saved[NS_CACHE_IDENTITY_MAX];
    size_t len = strlen(identity);
    int r = plat_file_open_at(support, NS_PENDING_DELETION, sizeof saved, &n, &f);
    int matches = 0;
    if (!r) {
        matches = n == (int64_t)len && !plat_read_all(f, saved, len) && !memcmp(saved, identity, len);
        plat_file_close(f);
    }
    if (retry) return matches ? 0 : win_write_at(support, NS_PENDING_DELETION, identity, len);
    if (!matches) return 0;
    r = plat_unlink_at(support, NS_PENDING_DELETION);
    return r == ENOENT ? 0 : r;
}

int native_this_pc_remove_stems(const native_steps_env *e, const char *cache_default, const char *identity) {
    if (!identity || !identity[0]) return 0;
    plat_dir s, d;
    int r = open_pc_cache(e, cache_default, &s, &d, NULL, CACHE_OPEN_CLEAR);
    int retry = r != ENOENT;
    if (!r) {
        char opened[NS_CACHE_IDENTITY_MAX];
        if (!pc_cache_identity_at(d, opened)) {
            retry = 0;
            if (!strcmp(identity, opened)) {
                loc_list models = {0};
                retry = plat_dir_each(d, models_each, &models) != 0;
                for (int i = 0; !retry && i < models.n; i++) {
                    int x = remove_cache_tree(e, d, models.names[i]);
                    retry = x && x != ENOENT;
                }
                loc_free(&models);
            }
        }
        plat_dir_close(d);
    } else {
        int support_err = open_pc_support(e, cache_default, &s);
        if (support_err) {
            if (retry) e->log(e->ctx, "couldn't record the pending This PC stem deletion");
            return retry;
        }
    }
    int pending_err = pc_pending_update(s, identity, retry);
    plat_dir_close(s);
    if (pending_err) e->log(e->ctx, "couldn't update the pending This PC stem deletion");
    return retry || pending_err;
}

const char *native_location_code(int kind, int probe) {
    static const char *const probes[] = {"available", "notConnected", "folderMissing", "markerMissingOrWrong",
                                         "link",      "network",      "readOnly",      "noPermission"};
    switch (kind) {
    case LOC_THIS_MAC: return "thisMac";
    case LOC_CHOSEN: return probe >= 0 && probe <= PROBE_NO_PERMISSION ? probes[probe] : "?";
    case LOC_BRIDGE_TOO_OLD: return "bridgeTooOld";
    case LOC_CONFIG_UNREADABLE: return "configUnreadable";
    case LOC_CONFIG_MALFORMED: return "configMalformedLocation";
    default: return "?";
    }
}
