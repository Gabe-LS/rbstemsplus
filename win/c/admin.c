/* The app's side of the elevated worker (admin.h). Pure C11 over plat.h. */
#include "plat.h"

#include "admin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"
#endif

enum { WORKER_MAX = 16 * 1024 * 1024, PAIR_MAX = 64 * 1024 };

static void hex(const unsigned char *d, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) out[2 * i] = h[d[i] >> 4], out[2 * i + 1] = h[d[i] & 15];
    out[2 * n] = 0;
}

/* ------------------------------------------------------------------ bin\ */

typedef struct {
    admin_bin_worker *out;
    int max, n, present;
    plat_dir bin;
} lister;

static int one_worker(void *ctx, const char *name, const plat_stat *st) {
    lister *l = ctx;
    if (st->kind != PLAT_DIR || !admin_sha12_ok(name)) return 0;
    /* present, by name and kind, before any data is read: an entry someone holds open, locks or
     * spoils is still there (and refuses the run), never "no worker" (I75) */
    plat_dir w;
    if (plat_dir_open_at(l->bin, name, &w)) {
        l->present++;
        return 0;
    }
    plat_stat fst;
    int e = plat_stat_at(w, ADMIN_WORKER_NAME, &fst);
    if (e == ENOENT) {
        plat_dir_close(w);
        return 0;
    }
    l->present++;
    if (e || fst.kind != PLAT_FILE || l->n == l->max) {
        plat_dir_close(w);
        return 0;
    }
    plat_file f;
    int64_t size = 0;
    e = plat_file_open_at(w, ADMIN_WORKER_NAME, WORKER_MAX, &size, &f);
    plat_dir_close(w);
    if (e) return 0;
    unsigned char *b = malloc(size ? (size_t)size : 1);
    plat_stat fs;
    admin_bin_worker *o = &l->out[l->n];
    memset(o, 0, sizeof *o);
    o->proto = -1;
    unsigned char d[32];
    if (b && (!size || !plat_read_all(f, b, (size_t)size)) && !plat_file_stat(f, &fs) && !plat_sha256(b, (size_t)size, d)) {
        hex(d, 32, o->sha256);
        if (!strncmp(o->sha256, name, 12)) {   /* its bytes are its folder's name: else not a worker */
            memcpy(o->sha12, name, 13);
            o->mtime = fs.mtime;
            if (!admin_marker_find(b, (size_t)size, &o->proto, o->minimum)) o->proto = -1;
            l->n++;
        }
    }
    free(b);
    plat_file_close(f);
    return 0;
}

int admin_list_bin(const char *root, admin_bin_worker *out, int max, int *present) {
    char p[ADMIN_PATH_MAX + 8];
    *present = 0;
    if (snprintf(p, sizeof p, "%s" SEP "bin", root) >= (int)sizeof p) return -1;
    lister l = {out, max, 0, 0, plat_dir_cwd()};
    int e = plat_dir_open(p, &l.bin);
    if (e) return e == ENOENT ? 0 : -1;   /* no root or no bin\: no worker; anything else: unknown */
    int r = plat_dir_each(l.bin, one_worker, &l);
    plat_dir_close(l.bin);
    *present = l.present;
    return r ? -1 : l.n;
}

/* whether a is newer than b: the higher compiled minimum, then the later file */
static int newer(const admin_bin_worker *a, const admin_bin_worker *b) {
    int c = payload_version_cmp(a->minimum, b->minimum);
    if (c == 1) return 1;
    if (c == -1) return 0;
    return a->mtime > b->mtime || (a->mtime == b->mtime && strcmp(a->sha12, b->sha12) > 0);
}

int admin_choose(const admin_bin_worker *w, int n, const char *own_sha256, int proto, int verb, int *index) {
    *index = -1;
    if (n <= 0) return ADMIN_PICK_APP_COPY;
    int own = own_sha256 && payload_is_sha256(own_sha256);
    int match = -1, any = -1;
    for (int i = 0; i < n; i++) {
        if (own && !strcmp(w[i].sha256, own_sha256)) {
            *index = i;
            return ADMIN_PICK_OWN;
        }
        if (w[i].proto == proto && (match < 0 || newer(&w[i], &w[match]))) match = i;
        if (any < 0 || newer(&w[i], &w[any])) any = i;
    }
    int needs_own = verb == ADMIN_INSTALL || verb == ADMIN_INSTALL_WORKER;
    if (!needs_own && match >= 0) {
        *index = match;
        return ADMIN_PICK_MATCHING;
    }
    if (own) {
        /* install-worker isn't refused by any protocol; a matching worker first all the same */
        *index = match >= 0 ? match : any;
        return ADMIN_PICK_INSTALL_OWN;
    }
    return ADMIN_PICK_UPDATE_FIRST;
}

/* ------------------------------------------------------------------ the source folder */

static int write_file(plat_dir d, const char *name, const void *data, size_t n) {
    int e = plat_unlink_at(d, name);
    if (e && e != ENOENT) return e;
    plat_file f;
    if ((e = plat_file_create_at(d, name, 1, &f))) return e;
    void *addr = NULL;
    if (n && !(e = plat_map_rw(f, (int64_t)n, &addr))) {
        memcpy(addr, data, n);
        e = plat_map_flush(addr, (int64_t)n);   /* the view written, then the file flushed below (MD4's order) */
        plat_unmap(addr, (int64_t)n);
    }
    if (!e) e = plat_fsync(f);
    plat_file_close(f);
    if (e) plat_unlink_at(d, name);
    return e;
}

static int copy_in(plat_dir d, const char *name, const char *from, size_t max) {
    char *b = NULL;
    size_t n = 0;
    int e = plat_read_file(from, max, &b, &n);
    if (!e) e = write_file(d, name, b, n);
    free(b);
    return e;
}

static int empty_files(void *ctx, const char *name, const plat_stat *st) {
    if (st->kind != PLAT_DIR) plat_unlink_at(*(plat_dir *)ctx, name);
    return 0;
}

int admin_source_prepare(const char *dir, const char *payload_dir, const char *bridge_path, const char *bridge_name, const char *worker_path) {
    char p[ADMIN_PATH_MAX + 64];
    plat_mkdirs(dir);
    plat_dir d, a = plat_dir_cwd();
    int e = plat_dir_open(dir, &d);
    if (e) return e;
    plat_dir_each(d, empty_files, &d);
    e = plat_mkdir_at(d, ADMIN_APP_COPY_DIR);
    if (!e || e == EEXIST) e = plat_dir_open_at(d, ADMIN_APP_COPY_DIR, &a);
    if (!e) plat_dir_each(a, empty_files, &a);
    if (!e && snprintf(p, sizeof p, "%s" SEP "payload.json", payload_dir) < (int)sizeof p) e = copy_in(d, "payload.json", p, PAIR_MAX);
    if (!e && snprintf(p, sizeof p, "%s" SEP "payload.json.sig", payload_dir) < (int)sizeof p) e = copy_in(d, "payload.json.sig", p, 4096);
    if (!e && bridge_path) e = admin_plain_name_ok(bridge_name) ? copy_in(d, bridge_name, bridge_path, (size_t)ADMIN_BRIDGE_MAX) : EINVAL;
    if (!e && worker_path) e = copy_in(a, ADMIN_WORKER_NAME, worker_path, WORKER_MAX);
    if (plat_dir_ok(a)) plat_dir_close(a);
    plat_dir_close(d);
    return e;
}

/* ------------------------------------------------------------------ a run */

typedef struct {
    const admin_client *c;
    admin_result *res;
} liner;

static void on_line(void *ctx, const char *s, size_t n) {
    liner *l = ctx;
    char v[80];
    switch (admin_line_kind(s, n, v, sizeof v)) {
    case ADMIN_LINE_TEXT:
        if (l->c->log) l->c->log(l->c->ctx, s, n);
        break;
    case ADMIN_LINE_STILL_CLOSING: l->res->still_closing = 1; break;
    case ADMIN_LINE_RESTART: l->res->restart_pending = 1; break;
    case ADMIN_LINE_BRIDGE:
        if (payload_is_sha256(v)) memcpy(l->res->bridge, v, 65);
        break;
    case ADMIN_LINE_FLOOR:
        if (strlen(v) < sizeof l->res->floor) memcpy(l->res->floor, v, strlen(v) + 1);
        break;
    case ADMIN_LINE_TARGET:
        if (l->res->ntargets < ADMIN_TARGETS_MAX) {
            admin_target_result *t = &l->res->targets[l->res->ntargets];
            if (!admin_target_value(v, t->version, &t->code, t->what)) l->res->ntargets++;
        }
        break;
    case ADMIN_LINE_ROOT_KEPT: l->res->root_kept = 1; break;
    default: break;   /* a newer worker's record: ignored */
    }
}

enum { RUN_NO_SOURCE = -2 };   /* run_one: the verb needs the source folder and there is none */

/* whether the source folder holds what install and install-worker read */
static int source_ready(const admin_client *c) {
    char p[ADMIN_PATH_MAX + 64];
    plat_stat st;
    if (!c->source || !c->source[0]) return 0;
    if (snprintf(p, sizeof p, "%s" SEP "payload.json", c->source) >= (int)sizeof p || plat_stat_path(p, 0, &st) || st.kind != PLAT_FILE) return 0;
    if (snprintf(p, sizeof p, "%s" SEP ADMIN_APP_COPY_DIR SEP ADMIN_WORKER_NAME, c->source) >= (int)sizeof p || plat_stat_path(p, 0, &st)
        || st.kind != PLAT_FILE)
        return 0;
    return 1;
}

/* "what: path" to the log */
static void say(const admin_client *c, const char *what, const char *path) {
    char msg[ADMIN_PATH_MAX + 128];
    int k = snprintf(msg, sizeof msg, "%s: %s", what, path);
    if (c->log && k > 0) c->log(c->ctx, msg, (size_t)k < sizeof msg ? (size_t)k : sizeof msg - 1);
}

/* a bin\ worker's hold (NT1): released once, by the elevation's started or on the way out */
typedef struct {
    const admin_client *c;
    void *held;
    int on;
} holder;

static void release_hold(void *ctx) {
    holder *h = ctx;
    if (!h->on) return;
    h->on = 0;
    if (h->c->release) h->c->release(h->c->ctx, h->held);
    h->held = NULL;
}

/* exe elevated for verb. sha: the sha256 a bin\ worker was picked by (NULL for the app folder's
 * copy, which admin_client_run checked against own_sha256) */
static int run_one(const admin_client *c, const char *exe, const char *sha, int verb, const char *targets, int force, admin_result *res) {
    const int bin = exe != c->app_copy;
    admin_args a;
    memset(&a, 0, sizeof a);
    a.verb = verb;
    a.proto = ADMIN_PROTO;
    /* --targets: the checked scope for install and restore, "*" for the verbs that sweep */
    if (verb == ADMIN_INSTALL || verb == ADMIN_RESTORE) {
        if (admin_targets_parse(targets ? targets : "", &a) || a.all) {
            if (c->log) c->log(c->ctx, "the scope to change isn't valid", 31);
            return -1;
        }
    } else
        a.all = 1;
    if (verb == ADMIN_INSTALL || verb == ADMIN_INSTALL_WORKER) {
        if (!source_ready(c) || !c->manifest || !payload_is_sha256(c->manifest)) return RUN_NO_SOURCE;
        snprintf(a.source, sizeof a.source, "%s", c->source);
        memcpy(a.manifest, c->manifest, 65);
    }
    a.force = force != 0;
    unsigned char rnd[16];
    c->random(c->ctx, rnd);
    admin_pipe_name(rnd, a.pipe);
    char params[3 * ADMIN_PATH_MAX];
    if (admin_build_params(&a, params, sizeof params) < 0) {
        if (c->log) c->log(c->ctx, "the worker's arguments aren't valid", 35);
        return -1;
    }
    holder h = {c, NULL, 0};
    if (bin) {
        /* a bin\ worker: elevated only while only an administrator can change it, held so from
         * the check through ShellExecuteExW, and only with the bytes it was picked by (NA4, NT1) */
        if (!c->hold) {
#ifndef ADMIN_TEST_HOOKS
            say(c, "the installed worker isn't admin-only", exe);   /* no check is no trust */
            return ADMIN_EXIT_CHECK_FAILED;
#endif
        } else {
            char bad[ADMIN_PATH_MAX + 64] = "";
            if (c->hold(c->ctx, exe, bad, sizeof bad, &h.held)) {
                say(c, "the installed worker isn't admin-only", bad[0] ? bad : exe);
                return ADMIN_EXIT_CHECK_FAILED;
            }
            h.on = 1;
            /* hashed again under the hold, never from a memo: what runas reads is what was picked */
            char *b = NULL, got[65] = "";
            size_t bn = 0;
            unsigned char d[32];
            if (!plat_read_file(exe, WORKER_MAX, &b, &bn) && !plat_sha256(b, bn, d)) hex(d, 32, got);
            free(b);
            if (!sha || !payload_is_sha256(sha) || strcmp(got, sha)) {
                release_hold(&h);
                say(c, "the installed worker changed after it was checked", exe);
                return ADMIN_EXIT_CHECK_FAILED;
            }
        }
    }
    liner l = {c, res};
    int code = -1;
    res->elevations++;
    int r = c->elevate(c->ctx, exe, params, a.pipe, on_line, &l, release_hold, &h, &code);
    release_hold(&h);   /* when the elevation never called started */
    if (r == ADMIN_EXIT_USER_STOPPED) return ADMIN_EXIT_USER_STOPPED;
    return r ? -1 : code;
}

int admin_client_run(const admin_client *c, int verb, const char *targets, int force, admin_result *res) {
    memset(res, 0, sizeof *res);
    res->code = -1;
    int installed_own = 0;
    for (int round = 0; round < 3; round++) {
        admin_bin_worker *w = calloc(ADMIN_BIN_MAX, sizeof *w);
        if (!w) return res->code = -1;
        int present = 0, n = admin_list_bin(c->root, w, ADMIN_BIN_MAX, &present), i = -1;
        if (n < 0 || (n == 0 && present > 0)) {
            /* a worker is there but couldn't be listed, read or verified: never the app folder's
             * copy then (I75); someone may be holding it to force exactly that */
            free(w);
            if (c->log) c->log(c->ctx, "couldn't check the installed worker", 35);
            return res->code = ADMIN_EXIT_CHECK_FAILED;
        }
        int pick = admin_choose(w, n, c->own_sha256, ADMIN_PROTO, verb, &i);
        char exe[ADMIN_PATH_MAX + 64], picked[65] = "";
        if (i >= 0) {
            snprintf(exe, sizeof exe, "%s" SEP "bin" SEP "%s" SEP ADMIN_WORKER_NAME, c->root, w[i].sha12);
            memcpy(picked, w[i].sha256, sizeof picked);   /* hashed again under the hold (NT1) */
        }
        free(w);
        if (pick == ADMIN_PICK_UPDATE_FIRST || (pick == ADMIN_PICK_INSTALL_OWN && installed_own)) {
            /* none can serve this app, or its own still isn't there after install-worker */
            res->update_first = 1;
            return res->code = ADMIN_EXIT_PROTOCOL;
        }
        if (pick == ADMIN_PICK_APP_COPY) {
            /* the first install: the app folder's copy must be the app's own worker (it narrows,
             * doesn't close, the window before runas reads it again) */
            char *b = NULL;
            size_t bn = 0;
            unsigned char d[32];
            char sha[65] = "";
            if (!plat_read_file(c->app_copy, WORKER_MAX, &b, &bn) && !plat_sha256(b, bn, d)) hex(d, 32, sha);
            free(b);
            if (!c->own_sha256 || !payload_is_sha256(c->own_sha256)) {
                res->update_first = 1;
                return res->code = ADMIN_EXIT_PROTOCOL;
            }
            if (strcmp(sha, c->own_sha256)) {
                if (c->log) c->log(c->ctx, "the app's worker isn't the one its release names", 48);
                return res->code = ADMIN_EXIT_CHECK_FAILED;
            }
        }
        int x;
        if (pick == ADMIN_PICK_INSTALL_OWN) {
            x = run_one(c, exe, picked, ADMIN_INSTALL_WORKER, "*", 0, res);
            if (x == ADMIN_EXIT_RUN_AGAIN) {
                installed_own = 1;
                continue;
            }
        } else
            x = pick == ADMIN_PICK_APP_COPY ? run_one(c, c->app_copy, NULL, verb, targets, force, res) : run_one(c, exe, picked, verb, targets, force, res);
        if (x == RUN_NO_SOURCE) {
            res->no_source = 1;
            res->update_first = 1;
            return res->code = ADMIN_EXIT_PROTOCOL;
        }
        if (x == ADMIN_EXIT_OLDER || x == ADMIN_EXIT_PROTOCOL) res->update_first = 1;
        return res->code = x;
    }
    return res->code;
}

long admin_quote_argv(const char *const *argv, int argc, char *out, size_t cap) {
    size_t at = 0;
#define PUT(ch)                      \
    do {                             \
        if (at + 1 >= cap) return -1; \
        out[at++] = (ch);            \
    } while (0)
    if (!cap) return -1;
    for (int i = 0; i < argc; i++) {
        if (i) PUT(' ');
        PUT('"');
        for (const char *s = argv[i];; s++) {
            size_t bs = 0;
            while (*s == '\\') bs++, s++;
            if (!*s) {
                for (size_t k = 0; k < 2 * bs; k++) PUT('\\');   /* before the closing quote: doubled */
                break;
            }
            if (*s == '"') {
                for (size_t k = 0; k < 2 * bs + 1; k++) PUT('\\');
                PUT('"');
            } else {
                for (size_t k = 0; k < bs; k++) PUT('\\');
                PUT(*s);
            }
        }
        PUT('"');
    }
#undef PUT
    out[at] = 0;
    return (long)at;
}
