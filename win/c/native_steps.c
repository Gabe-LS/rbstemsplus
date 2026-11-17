/* The core DLL's native steps as rules (native_steps.h). C11 over common/plat.h. */
#include "native_steps.h"

#include "admin_proto.h"
#include "compat.h"
#include "fs_write.h"
#include "marker.h"       /* common/: the location's marker check, the bridge's */
#include "path_rules.h"   /* PATHS_CACHE_FOLDER, PATHS_LOCATION_MAX */
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

int native_support_write(const char *support, const char *name, const char *text, size_t n) {
    if (!support || !support[0]) return ENOENT;
    plat_dir d;
    int e = plat_dir_open(support, &d);
    if (e == ENOENT) {   /* made only when missing: a dangling junction gives ELOOP, nothing created (models' MR3) */
        plat_mkdirs(support);
        e = plat_dir_open(support, &d);
    }
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
    static const char *const ext[2] = {".kept", ".manifest"};
    for (int i = 0; i < 2; i++) {
        snprintf(f, sizeof f, "%s%s", name, ext[i]);
        if (!plat_stat_at(d, f, &st) && st.kind == PLAT_FILE) plat_unlink_at(d, f);
    }
    snprintf(f, sizeof f, "%s.kept", name);
    e = plat_stat_at(d, f, &st);
    plat_dir_close(d);
    return e != ENOENT;   /* gone only when it surely is */
}

/* ASCII letters' case ignored */
static int same_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

int native_clear_cache(const native_steps_env *e, const char *cache_default, int *link) {
    *link = 0;
    size_t sn = e->support ? strlen(e->support) : 0;
    /* only support\Cache, whatever the shell's JSON says: the name compared, never deleted by it */
    if (!e->support || !cache_default || strncmp(cache_default, e->support, sn) || cache_default[sn] != SEP[0]
        || !same_ci(cache_default + sn + 1, "Cache")) {
        char line[NS_PATH_MAX + 32];
        snprintf(line, sizeof line, "refused to delete %s", cache_default ? cache_default : "(none)");
        e->log(e->ctx, line);
        return 1;
    }
    plat_dir d;
    plat_stat st;
    char line[NS_PATH_MAX + 32];
    int r = plat_dir_open(e->support, &d);
    if (!r) {
        r = plat_stat_at(d, "Cache", &st);
        if (!r && st.kind != PLAT_DIR) {   /* a link or a file: safeRemove's refusal (Safety.swift:75) */
            *link = st.kind == PLAT_OTHER;
            plat_dir_close(d);
            snprintf(line, sizeof line, "refused to delete %s", cache_default);
            e->log(e->ctx, line);
            return 1;
        }
        if (!r) r = e->remove_tree_at(e->ctx, d, "Cache");
        plat_dir_close(d);
    }
    if (r && r != ENOENT) {
        snprintf(line, sizeof line, "couldn't clear the stems saved on this PC (error %d)", r);
        e->log(e->ctx, line);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- REMOVE_LOCATION */

#ifdef NATIVE_STEPS_TEST_HOOKS
int (*native_steps_test_fault)(int op, const char *name);
#  define FAULT(op, name) (native_steps_test_fault ? native_steps_test_fault(op, name) : 0)
#else
#  define FAULT(op, name) 0
#endif

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

/* a chosen location's path: its root's length (Windows "X:\", the Mac's "/"), or 0 */
static size_t location_root(const char *p) {
    size_t n = p ? strlen(p) : 0;
    if (!n || n > PATHS_LOCATION_MAX) return 0;
#if defined(_WIN32)
    if (n < 4 || !((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) || p[1] != ':' || p[2] != '\\') return 0;
    size_t root = 3;
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

enum { NS_LOC_DEPTH = 64 };

int native_remove_location(const native_steps_env *e, const char *dir, const char *id, int whole, char name[256], char err[128]) {
    name[0] = err[0] = 0;
    size_t root = location_root(dir);
    if (!root) return 1;   /* not a location this app chose: "not connected", as the Mac */
    char top[8];
    snprintf(top, sizeof top, "%.*s", (int)root, dir);
    return native_remove_location_under(e, top, dir + root, id, whole, name, err);
}

int native_remove_location_under(const native_steps_env *e, const char *top, const char *rel, const char *id, int whole, char name[256],
                                 char err[128]) {
    name[0] = err[0] = 0;
    if (!location_rel_ok(rel) || !id || !id[0]) return 1;
    char path[PATHS_LOCATION_MAX + 1];
    snprintf(path, sizeof path, "%s", rel);
    plat_dir ds[NS_LOC_DEPTH];
    int nd = 0, r = 0;
    plat_stat st;
    int e0 = plat_dir_open(top, &ds[0]);
    if (e0) return e0 == ENOENT ? 1 : 2;
    nd = 1;
    if (plat_dir_stat(ds[0], &st)) r = 3;
    uint64_t vol = st.id.vol;
    /* one component at a time, never through a link, never onto another drive */
    for (char *save = NULL, *c = r ? NULL : strtok_r(path, SEP, &save); !r && c; c = strtok_r(NULL, SEP, &save)) {
        if (nd == NS_LOC_DEPTH) {
            r = 1;
            break;
        }
        int oe = plat_dir_open_at(ds[nd - 1], c, &ds[nd]);
        if (oe) {
            snprintf(name, 256, "%s", c);
            snprintf(err, 128, "%s", strerror(oe));
            r = 4;
            break;
        }
        nd++;
        if (plat_dir_stat(ds[nd - 1], &st) || st.id.vol != vol) {
            snprintf(name, 256, "%s", c);
            r = 5;
        }
    }
    if (!r && nd < 2) r = 1;
    plat_dir ours = ds[nd - 1], parent = nd >= 2 ? ds[nd - 2] : ds[0];
    if (!r && (strlen(id) != 36 || marker_check_at(ours, id))) r = 6;   /* a location id, exactly (review MT3) */
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
            int x = e->remove_tree_at(e->ctx, ours, n);
            if (x && x != ENOENT) ok = 0;
        }
        loc_free(&l);
        if (!ok) r = 7;
        else if (whole) {
            int x = plat_unlink_at(ours, companion);
            if (x && x != ENOENT) r = 7;
            if (!r && plat_unlink_at(ours, MARKER_NAME)) r = 7;
            /* the kept variants: gone with it on a case-insensitive drive (ENOENT); a remaining
             * one fails the emptiness check below */
            for (int i = 0; !r && i < variants.n; i++) plat_unlink_at(ours, variants.names[i]);
            /* the folder itself, only empty now and only the one checked (review MT1: by
             * identity, never by name, never recursive: whatever appeared meanwhile stays) */
            loc_list rest = {NULL, 0, 0, 0};
            plat_stat os;
            if (!r && (plat_dir_each(ours, loc_each, &rest) || rest.n || rest.failed)) r = 7;
            if (!r && plat_dir_stat(ours, &os)) r = 7;
            loc_free(&rest);
            plat_dir_close(ours);
            nd--;
            if (!r && e->remove_empty_dir_at(e->ctx, parent, PATHS_CACHE_FOLDER, os.id)) r = 7;
            /* the "._" companion exFAT and FAT32 wrote beside it */
            if (!r && !plat_stat_at(parent, "._" PATHS_CACHE_FOLDER, &st) && st.kind == PLAT_FILE) plat_unlink_at(parent, "._" PATHS_CACHE_FOLDER);
        }
        loc_free(&variants);
    }
    while (nd > 0) plat_dir_close(ds[--nd]);
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

/* name in d and its tree: 0 gone (or never there), 1 not. want_dir: name is a root, which must be
 * a folder: a link or reparse point (never followed) or a file there is left, logged, 1 */
static int remove_entry(const native_steps_env *e, plat_dir d, const char *parent, const char *name, int want_dir) {
    plat_stat st;
    int r = own_stat_at(d, name, &st);
    if (r) return r != ENOENT;
    if (st.kind == PLAT_OTHER || (want_dir && st.kind != PLAT_DIR)) {
        refused(e, parent, name);
        return 1;
    }
    r = e->remove_tree_at(e->ctx, d, name);
    return r && r != ENOENT;
}

/* the folder at path, through one handle on its parent: 0 gone (or none), 1 not */
static int remove_folder(const native_steps_env *e, const char *path, int want_dir) {
    char parent[NS_PATH_MAX];
    if (!path || !path[0]) return 0;
    if (snprintf(parent, sizeof parent, "%s", path) >= (int)sizeof parent) return 1;
    char *cut = strrchr(parent, SEP[0]);
    if (!cut || cut == parent || !cut[1]) return 1;
    *cut = 0;
    plat_dir d;
    int r = own_dir_open(parent, &d);
    if (r) {
        if (r != ENOENT) {   /* say so: what was in it may be gone already (review SF2) */
            char line[NS_PATH_MAX + 64];
            snprintf(line, sizeof line, "couldn't open %s (error %d)", parent, r);
            e->log(e->ctx, line);
        }
        return r != ENOENT;
    }
    r = remove_entry(e, d, parent, cut + 1, want_dir);
    plat_dir_close(d);
    return r;
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

int native_remove_own_folders(const native_steps_env *e, const char *expected_support, int keep_originals, const char *const *others,
                              int nothers) {
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
            /* 2. what stays, by identity (S51): a link at Cache or Logs (S54), the originals
             *    folder in keep mode; the ids plat_stat_at gives, never the listing's */
            static const char *const roots[3] = {"Cache", "Logs", "originals"};
            plat_id left[3];
            int nleft = 0, walk = 1;
            for (int i = 0; i < (keep_originals ? 3 : 2); i++) {
                plat_stat st;
                int s = own_stat_at(sd, roots[i], &st);
                if (s == ENOENT) continue;
                if (s) {
                    snprintf(line, sizeof line, "couldn't look at %s" SEP "%s (error %d): nothing deleted there", e->support, roots[i], s);
                    e->log(e->ctx, line);
                    bad = 1;
                    walk = 0;
                } else if (i < 2 && st.kind == PLAT_OTHER) {
                    left[nleft++] = st.id;
                    refused(e, e->support, roots[i]);
                    bad = 1;
                } else if (i == 2 && st.kind == PLAT_DIR) {
                    left[nleft++] = st.id;
                    kept = 1;
                }
            }
            /* 3. everything else, a link removed as itself (S60) */
            if (walk) {
                loc_list l = {NULL, 0, 0, 0};   /* names of any length, each its own copy (review S61's m3, as MT5) */
                if (own_dir_each(sd, loc_each, &l) || l.failed) bad = 1;
                for (int i = 0; i < l.n; i++) {
                    plat_stat st;
                    int s = own_stat_at(sd, l.names[i], &st);
                    if (s == ENOENT) continue;
                    if (s) {
                        bad = 1;
                        continue;
                    }
                    int skip = 0;
                    for (int k = 0; k < nleft && !skip; k++) skip = plat_id_same(st.id, left[k]);
                    if (skip) continue;
                    s = e->remove_tree_at(e->ctx, sd, l.names[i]);
                    if (s && s != ENOENT) bad = 1;
                }
                loc_free(&l);
            }
            /* 4. support itself, now empty, when nothing stays: the folder this handle holds (its
             *    id taken before it closes), through a parent that may be a link (review SF2) */
            plat_stat sst;
            int sid = plat_dir_stat(sd, &sst);
            plat_dir_close(sd);
            if (!kept && !bad) {
                if (sid) {
                    snprintf(line, sizeof line, "couldn't look at %s (error %d): it stays", e->support, sid);
                    e->log(e->ctx, line);
                    bad = 1;
                } else {
                    bad |= remove_support(e, e->support, sst.id);
                }
            }
        }
    }
    for (int i = 0; i < nothers; i++) bad |= remove_folder(e, others[i], 1);
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
