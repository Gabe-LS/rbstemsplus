/* The facts a watcher round reads (watch.h): rekordbox and R1's facts through compat.c, the
 * verified payload.json's lists through payload_store.c, and the support folder's small files
 * (State.swift's installed, dismissed, busy and request) through common/plat.h. */
#include "watch.h"
#include "fs_write.h"
#include "native_facts.h"

#ifndef RBSP_CORE_PLAT_SHIM_H
#  define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h (through compat.h) has the shim's names */
#endif
#include "internal.h"   /* the core's state_chosen, busy_marker_* */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"      /* the Mac tests: a scratch tree */
#endif

enum { SMALL_MAX = 64 * 1024 };   /* installed, dismissed, busy, demucs3_ver.txt: small files */

static int join(char *out, size_t n, const char *a, const char *b) {
    int k = snprintf(out, n, "%s" SEP "%s", a, b);
    return k > 0 && (size_t)k < n;
}

/* a small file's text: 0 with *buf (malloc'd, NUL-terminated; free it) or ENOENT and the like */
static int small(const char *dir, const char *name, char **buf, size_t *n) {
    char p[WATCH_PATH_MAX];
    *buf = NULL;
    *n = 0;
    if (!join(p, sizeof p, dir, name)) return ENAMETOOLONG;
    int e = plat_read_file(p, SMALL_MAX, buf, n);
    if (!e) (*buf)[*n] = 0;   /* plat_read_file's buffer holds max + 1 bytes */
    return e;
}

void watch_reader_free(watch_reader *r) {
    free(r->dismissed);
    r->dismissed = NULL;
    r->dismissed_n = 0;
}

/* The record in dir as an installed-release record, as W3a's update_installed reads it: checked
 * again with no floor (a newer payload accepted but not installed yet raises the floor above it),
 * and naming this app (windows.app.version). 1 into m, or 0. One copy of this rule
 * belongs in payload_store.c once both lanes are merged. */
static int installed_in(const watch_env *env, const char *dir, payload_manifest *m) {
    char *json, *sig;
    size_t jn, sn;
    if (small(dir, "payload.json", &json, &jn)) return 0;
    if (small(dir, "payload.json.sig", &sig, &sn)) sig = NULL, sn = 0;
    int r = payload_check(json, jn, sig, sn, env->keys, env->nkeys, NULL, m);
    free(json);
    free(sig);
    return r == PAYLOAD_OK && m->has_windows && env->version && !strcmp(m->win_app.version, env->version);
}

/* installed-release\, else its prev\ (a write stopped between the signature and payload.json) */
static int installed(const watch_env *env, payload_manifest *m) {
    char d[WATCH_PATH_MAX], prev[WATCH_PATH_MAX];
    if (!join(d, sizeof d, env->support, "installed-release") || !join(prev, sizeof prev, d, "prev")) return 0;
    return installed_in(env, d, m) || installed_in(env, prev, m);
}

/* the verified payload.json in dir under support's floor: 1 into m, or 0 */
static int payload_in(const watch_env *env, const char *dir, payload_manifest *m) {
    char pd[WATCH_PATH_MAX];
    if (!join(pd, sizeof pd, env->support, dir)) return 0;
    payload_files pf = {pd, env->support, env->keys, env->nkeys, env->version};
    int r = payload_saved(&pf, m);
    if (r == PAYLOAD_OK && m->has_windows) return 1;
    if (r != PAYLOAD_NONE) {
        char why[256], line[512];
        payload_refusal_text(r == PAYLOAD_OK ? PAYLOAD_NO_NEWS : r, m->payload_version, env->version, why, sizeof why);
        snprintf(line, sizeof line, "%s" SEP "payload.json ignored: %s", dir, why);
        if (env->log) env->log(env->ctx, line);
    }
    return 0;
}

static void trim(char *s) {
    size_t n = strlen(s), a = 0;
    while (a < n && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (n > a && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    memmove(s, s + a, n - a);
    s[n - a] = 0;
}

int watch_read_facts(const watch_env *env, watch_reader *r, watch_facts *f) {
    memset(f, 0, sizeof *f);
    f->version = env->version;

    /* the choices and the answers */
    char *t;
    size_t n;
    if (!small(env->support, "installed", &t, &n)) {
        state_chosen(t, n, &f->chosen_model, &f->chosen_cache);
        free(t);
    }
    watch_reader_free(r);
    if (!small(env->support, "dismissed", &r->dismissed, &r->dismissed_n)) {
        f->dismissed = r->dismissed;
        f->dismissed_n = r->dismissed_n;
    }

    /* the lists: payload\'s, else installed-release\'s (the one that installed this app) */
    int inst = installed(env, &r->installed);
    int saved = payload_in(env, "payload", &r->saved);
    f->m = saved ? &r->saved : inst ? &r->installed : NULL;
    f->have_lists = f->m != NULL;
    payload_sha bridges[2];
    int nb = 0;
    if (saved) memcpy(bridges[nb++], r->saved.win_bridge.sha256, sizeof bridges[0]);
    if (inst) memcpy(bridges[nb++], r->installed.win_bridge.sha256, sizeof bridges[0]);

    /* One inventory supplies the Cache rows and the current candidate for Demucs v4. */
    inv_list *inventory = malloc(sizeof *inventory);
    compat_rekordbox *rb = calloc(INV_MAX, sizeof *rb);
    inv_assess_row *assessment = calloc(INV_MAX, sizeof *assessment);
    native_inv_caps *caps = calloc(INV_MAX, sizeof *caps);
    if (!inventory || !rb || !assessment || !caps || inv_read(env->os, env->rekordbox, 0, inventory)) {
        free(inventory);
        free(rb);
        free(assessment);
        free(caps);
        if (env->log) env->log(env->ctx, "not checking: couldn't read rekordbox inventory");
        return -1;
    }
    native_inventory_assess(env->os, inventory, env->root, f->m, inst ? &r->installed : NULL, assessment, caps);
    char running[INV_MAX * COMPAT_VERSION_MAX + 1];
    native_inventory_running(env->os, inventory, assessment, running, sizeof running);
    f->nrows = inv_rows(inventory, assessment, f->m, f->rows, INV_MAX);
    f->inv_complete = inventory->complete;
    for (int i = 0; i < inventory->n; i++)
        if (inventory->it[i].trust == INV_SETTLING) f->rb_unsettled = 1;
    int cur, state;
    int nrb = inv_current(inventory, rb, INV_MAX, &cur, &state);
    free(caps);
    free(assessment);
    free(inventory);
    f->rb_state = state;
    for (int i = 0; i < nrb; i++)
        if (rb[i].candidate) {
            snprintf(f->rekordbox_version, sizeof f->rekordbox_version, "%s", rb[i].folder_version);
            const char *name = rb[i].folder + strlen(rb[i].folder);
            while (name > rb[i].folder && name[-1] != '\\' && name[-1] != '/') name--;
            snprintf(f->watch_name, sizeof f->watch_name, "%s", name);
        }
    if (state == COMPAT_RB_OK && cur >= 0) {
        snprintf(f->folder, sizeof f->folder, "%s", rb[cur].folder);
        /* R1's facts and the bridge: the live DLL, read once */
        compat_r1_facts(env->os, f->folder, env->root, bridges, nb, f->m, &f->r1);
        f->r1_known = f->r1.live != COMPAT_LIVE_UNREADABLE;
        f->cache_present = f->r1.live == COMPAT_LIVE_OURS || f->r1.live == COMPAT_LIVE_OURS_UNLISTED;
    }
    free(rb);

    /* Demucs v4 and the STEMS Engine, in rekordbox's model folder */
    if (!small(env->models, "demucs3_ver.txt", &t, &n)) {
        snprintf(f->engine, sizeof f->engine, "%s", t);
        trim(f->engine);
        free(t);
    }
    char p[WATCH_PATH_MAX];
    plat_stat st;
    if (join(p, sizeof p, env->models, "hdemucs.onnx") && !plat_stat_path(p, 1, &st) && st.kind == PLAT_FILE)
        f->plus_present = st.size == WATCH_OUR_MODEL_SIZE || (f->m && st.size == f->m->model.size);
    return 0;
}

/* the busy marker, parsed into a: 1 with *m, 0 none, -1 out of memory; *written its mtime */
static int marker(const watch_env *env, arena *a, busy_marker *m, int32_t *has_written, int64_t *written) {
    char *t;
    size_t n;
    *has_written = 0;
    if (small(env->support, "busy", &t, &n)) return 0;
    int r = busy_marker_parse(a, t, n, m);
    free(t);
    char p[WATCH_PATH_MAX];
    plat_stat st;
    if (r == 1 && join(p, sizeof p, env->support, "busy") && !plat_stat_path(p, 0, &st)) {
        *has_written = 1;
        *written = st.mtime;
    }
    return r;
}

int32_t watch_app_busy(const watch_env *env) {
    arena a = {0};
    busy_marker m;
    int32_t hw;
    int64_t w = 0;
    int32_t live = 0;
    if (marker(env, &a, &m, &hw, &w) == 1) live = busy_marker_live(&m, env->process_start ? env->process_start(env->ctx, m.pid) : -1, hw, w);
    arena_free(&a);
    return live;
}

int32_t watch_half_uninstalled(const watch_env *env, char *id, size_t cap) {
    arena a = {0};
    busy_marker m;
    int32_t hw, r = 0;
    int64_t w = 0;
    if (marker(env, &a, &m, &hw, &w) == 1 && m.has_action && m.id.n >= 9 && !memcmp(m.id.s, "uninstall", 9)) {
        snprintf(id, cap, "%.*s", (int)m.id.n, m.id.s);
        r = 1;
    }
    arena_free(&a);
    return r;
}

int watch_write_support(const watch_env *env, const char *name, const char *text, size_t n) {
    int e = win_write_atomic(env->support, name, text, n);
    if (e && env->log) {
        char b[512];
        snprintf(b, sizeof b, "couldn't write %s (error %d)", name, e);
        env->log(env->ctx, b);
    }
    return e;
}
