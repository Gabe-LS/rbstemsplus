/* The Windows app's updater, the C part (update.h; app.md §5.4, release.md §1.4, §2, §7). */
#include "plat.h"

#include "update.h"

#include "compat.h"
#include "fs_write.h"
#include "unzip.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"
#endif

#define APP_EXE "RB Stems Plus.exe"

#ifdef UPDATE_TEST_HOOKS
int (*update_test_step)(int step);
#  define STEP(s) (update_test_step ? update_test_step(s) : 0)
#else
#  define STEP(s) 0
#endif

static const char json_name[] = "payload.json", sig_name[] = "payload.json.sig";
static const char shown_name[] = "shown-version", installed_dir[] = "installed-release", update_dir[] = "update";
static const char pending_name[] = "pending", started_name[] = "started", result_name[] = "result", current_name[] = "current";
static const char watcher_name[] = "watcher";   /* update\watcher: a watcher was running when the commit stopped it */

/* ------------------------------------------------------------------ small helpers */

static void say(const update_env *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(const update_env *e, const char *fmt, ...) {
    if (!e->log) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    e->log(e->ctx, line);
}

/* out = a SEP b [SEP c]: 0, or -1 when it doesn't fit */
static int join(char *out, size_t n, const char *a, const char *b, const char *c) {
    int k = c ? snprintf(out, n, "%s" SEP "%s" SEP "%s", a, b, c) : snprintf(out, n, "%s" SEP "%s", a, b);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

static int older(const char *a, const char *b) { return payload_version_cmp(a, b) == -1; }

static void copy_str(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src); }

/* a small file's bytes (at most max): 0 with *buf (malloc'd, NUL-terminated) and *n; ENOENT; another errno */
static int read_small(const char *path, size_t max, char **buf, size_t *n) {
    *buf = NULL;
    *n = 0;
    return plat_read_file(path, max, buf, n);
}

/* removes the file dir SEP name (never a folder); ENOENT counts as done */
static void remove_file(const char *dir, const char *name) {
    plat_dir d;
    if (plat_dir_open(dir, &d)) return;
    plat_stat st;
    if (!plat_stat_at(d, name, &st) && st.kind != PLAT_DIR) plat_unlink_at(d, name);
    plat_dir_close(d);
}

/* opens path's last component in its folder, never following a link there; a regular file of at
 * most max bytes */
static int open_file(const char *path, int64_t max, int64_t *size, plat_file *f) {
    const char *slash = strrchr(path, SEP[0]);
    if (!slash || slash == path) return EINVAL;
    char dir[UPDATE_PATH_MAX];
    size_t dn = (size_t)(slash - path);
    if (dn >= sizeof dir) return ENAMETOOLONG;
    memcpy(dir, path, dn);
    dir[dn] = 0;
#if defined(_WIN32)
    if (dn == 2 && dir[1] == ':') {   /* "C:" is a drive's current folder; its root is "C:\" */
        dir[2] = '\\';
        dir[3] = 0;
    }
#endif
    plat_dir d;
    int e = plat_dir_open(dir, &d);
    if (e) return e;
    e = plat_file_open_at(d, slash + 1, max, size, f);
    plat_dir_close(d);
    return e;
}

/* a whole file of at most max bytes, never through a link at its name, into a buffer of its own
 * size (plat_read_file's is max + 1 bytes, too much for the zip): 0 with *buf (malloc'd) and *n */
static int read_whole(const char *path, int64_t max, char **buf, size_t *n) {
    plat_file f;
    int64_t size = 0;
    *buf = NULL;
    *n = 0;
    int e = open_file(path, max, &size, &f);
    if (e) return e;
    char *b = malloc(size ? (size_t)size : 1);
    e = b ? plat_read_all(f, b, (size_t)size) : ENOMEM;
    plat_file_close(f);
    if (e) {
        free(b);
        return e;
    }
    *buf = b;
    *n = (size_t)size;
    return 0;
}

static void hex(const unsigned char *d, size_t n, char *out) {
    static const char x[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = x[d[i] >> 4];
        out[2 * i + 1] = x[d[i] & 15];
    }
    out[2 * n] = 0;
}

/* a file's SHA-256 in hex: 0, or an errno (ENOENT: none) */
/* the sha256 of the open file f, read from where it is to its end; f is closed */
static int file_sha_f(plat_file f, payload_sha out) {
    out[0] = 0;
    int e = 0;
    plat_sha256_ctx c;
    if (plat_sha256_init(&c)) {
        plat_file_close(f);
        return EIO;
    }
    enum { CHUNK = 1 << 20 };
    unsigned char *buf = malloc(CHUNK);
    e = buf ? 0 : ENOMEM;
    for (size_t got = 0; !e;) {
        e = plat_read(f, buf, CHUNK, &got);
        if (e || !got) break;
        plat_sha256_update(&c, buf, got);
    }
    unsigned char md[32];
    if (plat_sha256_final(&c, md) && !e) e = EIO;
    free(buf);
    plat_file_close(f);
    if (!e) hex(md, 32, out);
    return e;
}

static int file_sha(const char *path, payload_sha out) {
    plat_file f;
    int64_t size;
    out[0] = 0;
    int e = open_file(path, INT64_MAX, &size, &f);
    return e ? e : file_sha_f(f, out);
}

/* rel ('/'-separated) under the folder d, every folder on the way opened through the one above it
 * and none followed, then the file as plat_file_open_at opens it */
static int open_rel(plat_dir d, const char *rel, int64_t max, int64_t *size, plat_file *f) {
    char part[UPDATE_PATH_MAX];
    plat_dir cur = d;
    int owned = 0, e = 0;
    for (const char *s = rel;;) {
        const char *slash = strchr(s, '/');
        if (!slash) {
            e = plat_file_open_at(cur, s, max, size, f);
            break;
        }
        size_t k = (size_t)(slash - s);
        if (!k || k >= sizeof part) {
            e = EINVAL;
            break;
        }
        memcpy(part, s, k);
        part[k] = 0;
        plat_dir next;
        e = plat_dir_open_at(cur, part, &next);
        if (owned) plat_dir_close(cur);
        if (e) {
            owned = 0;
            break;
        }
        cur = next;
        owned = 1;
        s = slash + 1;
    }
    if (owned) plat_dir_close(cur);
    return e;
}

/* whether rel under d is the file of that sha256 */
static int verified_at(plat_dir d, const char *rel, const char *sha) {
    plat_file f;
    int64_t size;
    payload_sha got;
    return !open_rel(d, rel, INT64_MAX, &size, &f) && !file_sha_f(f, got) && !strcmp(got, sha);
}

/* ------------------------------------------------------------------ downloads */

typedef struct {
    const update_env *e;
    char *buf;
    size_t n, cap, max;
    FILE *f;          /* a file sink: the .part */
    const char *path;
    int64_t have, done, total;
    int last_pct;
    uint64_t deadline;   /* plat_now_mono_ns() past which the download stops (0: none) */
    int timed_out;
} sinkctx;

static int cancelled(void *ctx) {
    sinkctx *s = ctx;
    if (s->deadline && plat_now_mono_ns() >= s->deadline) {
        s->timed_out = 1;
        return 1;
    }
    return s->e->cancelled ? s->e->cancelled(s->e->ctx) : 0;
}

static int mem_begin(void *ctx, int64_t at, int64_t total) {
    sinkctx *s = ctx;
    (void)total;
    if (at != 0) return EINVAL;
    s->n = 0;
    return 0;
}

static int mem_write(void *ctx, const void *p, size_t n) {
    sinkctx *s = ctx;
    if (n > s->max - s->n) return EFBIG;
    if (s->n + n > s->cap) {
        size_t cap = s->cap ? s->cap : 4096;
        while (cap < s->n + n) cap *= 2;
        char *b = realloc(s->buf, cap + 1);
        if (!b) return ENOMEM;
        s->buf = b;
        s->cap = cap;
    }
    memcpy(s->buf + s->n, p, n);
    s->n += n;
    return 0;
}

/* GET url into memory (at most max bytes): UPDATE_NET_* with *buf (malloc'd, NUL-terminated) on OK */
static int fetch_mem(const update_env *e, const char *url, size_t max, char **buf, size_t *n, int *status) {
    /* a small file: the whole download within a deadline (curl's --max-time 60), past it offline */
    uint64_t ms = e->small_fetch_ms ? e->small_fetch_ms : 60000;
    sinkctx s = {.e = e, .max = max, .deadline = plat_now_mono_ns() + ms * 1000000u};
    update_sink sink = {mem_begin, mem_write, cancelled, &s, s.deadline};
    int err = 0;
    *status = 0;
    int r = e->net->get(e->net->ctx, url, 0, (int64_t)max, &sink, status, &err);
    if (r == UPDATE_NET_CANCELLED && s.timed_out) r = UPDATE_NET_OFFLINE;
    if (r == UPDATE_NET_OK && !s.buf) s.buf = malloc(1);
    if (r != UPDATE_NET_OK || !s.buf) {
        free(s.buf);
        *buf = NULL;
        *n = 0;
        return r == UPDATE_NET_OK ? UPDATE_NET_SINK : r;
    }
    s.buf[s.n] = 0;
    *buf = s.buf;
    *n = s.n;
    return r;
}

static int file_begin(void *ctx, int64_t at, int64_t total) {
    sinkctx *s = ctx;
    s->total = total;
    if (at == s->have) {
        s->done = at;
        return 0;
    }
    if (at != 0) return EINVAL;
    /* the server sent the whole file: from scratch */
    fclose(s->f);
    s->f = plat_fopen_w(s->path, 0);
    s->have = s->done = 0;
    return s->f ? 0 : errno ? errno : EIO;
}

static int file_write(void *ctx, const void *p, size_t n) {
    sinkctx *s = ctx;
    if (n && fwrite(p, 1, n, s->f) != n) return errno ? errno : EIO;
    s->done += (int64_t)n;
    if (s->e->progress) {
        int pct = s->total > 0 ? (int)(s->done * 100 / s->total) : (int)(s->done >> 20);
        if (pct != s->last_pct) {
            s->last_pct = pct;
            s->e->progress(s->e->ctx, s->done, s->total);
        }
    }
    return 0;
}

static int64_t size_of(const char *path) {
    plat_stat st;
    return plat_stat_path(path, 0, &st) || st.kind != PLAT_FILE ? -1 : st.size;
}

static int verified(const char *path, const char *sha) {
    payload_sha got;
    return !file_sha(path, got) && !strcmp(got, sha);
}

enum { FETCH_OK = UPDATE_FETCH_OK, FETCH_OFFLINE = UPDATE_FETCH_OFFLINE, FETCH_NO_SPACE = UPDATE_FETCH_NO_SPACE, FETCH_DAMAGED = UPDATE_FETCH_DAMAGED };

/* this asset's other partial downloads in a folder: <name>.<12 hex>.part with another sha12 than
 * keep's, removed; another asset's parts (the model's and the bridge's share a folder) stay */
typedef struct {
    plat_dir d;
    const char *keep, *name;
} parts_ctx;
static int part_each(void *ctx, const char *name, const plat_stat *st) {
    parts_ctx *c = ctx;
    size_t n = strlen(name), k = strlen(c->name);
    if (st->kind != PLAT_FILE || n != k + 1 + 12 + 5 || strncmp(name, c->name, k) || name[k] != '.' || strcmp(name + k + 13, ".part") || !strcmp(name, c->keep))
        return 0;
    for (size_t i = k + 1; i < k + 13; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f') || (name[i] >= 'A' && name[i] <= 'F'))) return 0;
    plat_unlink_at(c->d, name);
    return 0;
}
static void remove_other_parts(const char *dir, const char *name, const char *keep) {
    parts_ctx c = {{0}, keep, name};
    if (plat_dir_open(dir, &c.d)) return;
    plat_dir_each(c.d, part_each, &c);
    plat_dir_close(c.d);
}

/* fetchAsset: dir SEP name holds the file with this sha256, kept when it is there already, else
 * downloaded from url into name.<sha12>.part (a partial one of the same file resumed; this name's
 * other parts removed, so a part is never resumed with another release's bytes, while another
 * asset's part in the folder stays), verified and renamed; once more from scratch when the bytes
 * don't match. A part past max is dropped before resuming; a file past max (UPDATE_NET_TOO_BIG)
 * is damaged, its part removed, as the Mac's. */
static int fetch_file(const update_env *e, const char *url, const char *dir, const char *name, const char *sha, int64_t max) {
    char dest[UPDATE_PATH_MAX], part[UPDATE_PATH_MAX], partname[PAYLOAD_NAME_MAX + 24];
    snprintf(partname, sizeof partname, "%s.%.12s.part", name, sha);
    if (join(dest, sizeof dest, dir, name, NULL) || join(part, sizeof part, dir, partname, NULL)) return FETCH_DAMAGED;
    remove_other_parts(dir, name, partname);
    if (verified(dest, sha)) return FETCH_OK;
    remove_file(dir, name);
    for (int attempt = 1; attempt <= 2; attempt++) {
        int64_t have = attempt == 1 ? size_of(part) : -1;
        if (have < 0 || have > max) {   /* past max: never resumed (at max, the server's 416 restarts it) */
            remove_file(dir, partname);
            have = 0;
        }
        if (have > 0 && verified(part, sha)) {   /* finished by an earlier try */
            if (!plat_rename_path(part, dest)) return FETCH_OK;
        }
        sinkctx s = {.e = e, .path = part, .have = have, .total = -1, .last_pct = -1};
        s.f = plat_fopen_w(part, have > 0);
        if (!s.f) {
            say(e, "%s: couldn't write %s (%s)", name, part, strerror(errno));
            return errno == ENOSPC ? FETCH_NO_SPACE : FETCH_OFFLINE;
        }
        update_sink sink = {file_begin, file_write, cancelled, &s, 0};   /* no deadline: a big file takes its time */
        int status = 0, err = 0;
        int r = e->net->get(e->net->ctx, url, have, max, &sink, &status, &err);
        if (s.f && fclose(s.f) && r == UPDATE_NET_OK) {
            r = UPDATE_NET_SINK;
            err = errno ? errno : EIO;
        }
        if (r != UPDATE_NET_OK) {
            say(e, "%s: download failed (%d, HTTP %d, %s)%s", name, r, status, err ? strerror(err) : "-", have ? ", resuming" : "");
            if (r == UPDATE_NET_SINK && err == ENOSPC) return FETCH_NO_SPACE;
            if (r == UPDATE_NET_TOO_BIG) {   /* more than the payload says: damaged, never offline */
                remove_file(dir, partname);
                return FETCH_DAMAGED;
            }
            if (have > 0 && (r == UPDATE_NET_HTTP || r == UPDATE_NET_RANGE)) continue;   /* the server refused to resume: from scratch */
            return FETCH_OFFLINE;   /* a partial file stays for the next try */
        }
        if (verified(part, sha) && !plat_rename_path(part, dest)) {
            say(e, "%s: downloaded and verified", name);
            return FETCH_OK;
        }
        say(e, "%s: checksum didn't match (%lld bytes): %s", name, (long long)size_of(part), attempt == 1 ? "downloading again" : "refused");
        remove_file(dir, partname);
    }
    return FETCH_DAMAGED;
}

int update_fetch_asset(const update_env *e, const char *tag, const char *dir, const char *name, const char *sha, int64_t max) {
    char url[UPDATE_PATH_MAX];
    if (update_url(e->base, tag, name, url, sizeof url)) return FETCH_DAMAGED;
    plat_mkdirs(dir);   /* a folder that can't be made: the part's write says why */
    return fetch_file(e, url, dir, name, sha, max);
}

/* ------------------------------------------------------------------ the store and the records */

void update_store_init(const update_env *e, update_store *out) {
    memset(out, 0, sizeof *out);
    join(out->payload_dir, sizeof out->payload_dir, e->support, "payload", NULL);
    out->files.payload_dir = out->payload_dir;
    out->files.floor_dir = e->support;
    out->files.keys = e->keys;
    out->files.nkeys = e->nkeys;
    out->files.minimum = e->own;
}

/* the payload pair in dir: 0 with both (malloc'd); else an errno (ENOENT: no payload.json) */
static int read_pair(const char *dir, char **json, size_t *jn, char **sig, size_t *sn) {
    char p[UPDATE_PATH_MAX];
    *json = *sig = NULL;
    *jn = *sn = 0;
    if (join(p, sizeof p, dir, json_name, NULL)) return ENAMETOOLONG;
    int r = read_small(p, PAYLOAD_MAX, json, jn);
    if (r) return r;
    if (join(p, sizeof p, dir, sig_name, NULL) || read_small(p, 4096, sig, sn)) *sig = NULL, *sn = 0;
    return 0;
}

/* the pair written into dir (made when missing): the signature first, so a stop between the two
 * leaves a pair that doesn't verify */
static int write_pair(const char *dir, const char *json, size_t jn, const char *sig, size_t sn) {
    plat_mkdirs(dir);
    int r = win_write_atomic(dir, sig_name, sig, sn);
    return r ? r : win_write_atomic(dir, json_name, json, jn);
}

/* the pair in dir as an installed-release record: checked again (no floor) and naming this app */
static int installed_in(const update_env *e, const char *dir, payload_manifest *out) {
    char *json, *sig;
    size_t jn, sn;
    int r = read_pair(dir, &json, &jn, &sig, &sn);
    if (r) return r == PLAT_EFTYPE ? PAYLOAD_TOO_BIG : PAYLOAD_NONE;
    r = payload_check(json, jn, sig, sn, e->keys, e->nkeys, NULL, out);
    free(json);
    free(sig);
    if (r == PAYLOAD_OK && (!out->has_windows || strcmp(out->win_app.version, e->own))) return PAYLOAD_NONE;
    return r;
}

int update_installed_at(const update_env *e, payload_manifest *out, char *at, size_t n) {
    char dir[UPDATE_PATH_MAX], prev[UPDATE_PATH_MAX];
    if (at && n) at[0] = 0;
    if (join(dir, sizeof dir, e->support, installed_dir, NULL) || join(prev, sizeof prev, dir, "prev", NULL)) return PAYLOAD_NONE;
    int r = installed_in(e, dir, out);
    const char *which = r == PAYLOAD_OK ? dir : NULL;
    /* a write stopped between the signature and payload.json: the record before it */
    if (r != PAYLOAD_OK && installed_in(e, prev, out) == PAYLOAD_OK) r = PAYLOAD_OK, which = prev;
    if (which && at && n) {
        if (strlen(which) >= n) return PAYLOAD_NONE;
        memcpy(at, which, strlen(which) + 1);
    }
    return r;
}

int update_installed(const update_env *e, payload_manifest *out) { return update_installed_at(e, out, NULL, 0); }

/* installed-release replaced: the record there now, when it verifies, is kept as prev\ first, so
 * a stop between the signature's write and payload.json's still leaves one (update_installed) */
static int write_installed(const update_env *e, const char *json, size_t jn, const char *sig, size_t sn) {
    char dir[UPDATE_PATH_MAX], prev[UPDATE_PATH_MAX], *oj, *os;
    size_t ojn, osn;
    if (join(dir, sizeof dir, e->support, installed_dir, NULL) || join(prev, sizeof prev, dir, "prev", NULL)) return ENAMETOOLONG;
    int r = 0;
    if (!read_pair(dir, &oj, &ojn, &os, &osn)) {
        payload_manifest *m = malloc(sizeof *m);
        if (m && payload_check(oj, ojn, os, osn, e->keys, e->nkeys, NULL, m) == PAYLOAD_OK) r = write_pair(prev, oj, ojn, os, osn);
        free(m);
        free(oj);
        free(os);
        if (r) return r;
    }
    plat_mkdirs(dir);
    r = win_write_atomic(dir, sig_name, sig, sn);
    if (!r) r = STEP(UPDATE_STEP_INSTALLED_HALF);
    return r ? r : win_write_atomic(dir, json_name, json, jn);
}

static int read_shown(const update_env *e, update_shown *out) {
    char p[UPDATE_PATH_MAX], *b;
    size_t n;
    if (join(p, sizeof p, e->support, shown_name, NULL) || read_small(p, 64, &b, &n)) return 0;
    int ok = update_shown_parse(b, n, out);
    free(b);
    return ok;
}

void update_shown_now(const update_env *e, char out[PAYLOAD_VERSION_MAX]) {
    update_shown r;
    copy_str(out, PAYLOAD_VERSION_MAX, update_shown_version(e->own, read_shown(e, &r) ? &r : NULL));
}

/* the saved payload pair (payload\), checked again, if it is m: written as installed-release */
static void install_record(const update_env *e, const payload_manifest *m) {
    update_store st;
    update_store_init(e, &st);
    char *json, *sig, dir[UPDATE_PATH_MAX];
    size_t jn, sn;
    if (read_pair(st.payload_dir, &json, &jn, &sig, &sn)) return;
    payload_manifest *chk = malloc(sizeof *chk);
    if (chk && payload_check(json, jn, sig, sn, e->keys, e->nkeys, NULL, chk) == PAYLOAD_OK &&
        !strcmp(chk->payload_version, m->payload_version) && chk->has_windows && !strcmp(chk->win_app.version, e->own) &&
        !join(dir, sizeof dir, e->support, installed_dir, NULL)) {
        int r = write_installed(e, json, jn, sig, sn);
        if (r) say(e, "installed-release: couldn't be written (%s)", strerror(r));
        else say(e, "installed-release: payload %s (this app, %s)", m->payload_version, e->own);
    }
    free(chk);
    free(json);
    free(sig);
}

/* update_seen reads, changes and writes back installed-release, shown-version and DisplayVersion:
 * the core's flow thread and a background worker may both call it (the native PAYLOAD step), so
 * the whole of it runs under one lock. One process per store: the app's (the watcher only reads). */
static plat_mutex seen_lock = PLAT_MUTEX_INIT;

static int seen_locked(const update_env *e, const payload_manifest *m, char shown[PAYLOAD_VERSION_MAX]) {
    if (m && !m->signed_by) m = NULL;   /* only a verified payload moves anything (noteShownVersion) */
    payload_manifest *inst = malloc(sizeof *inst);
    int has_inst = inst && update_installed(e, inst) == PAYLOAD_OK;
    /* a release whose Windows files are this app's: no download, the record follows it */
    if (m && m->has_windows && !strcmp(m->win_app.version, e->own) && (!has_inst || older(inst->payload_version, m->payload_version))) {
        install_record(e, m);
        has_inst = inst && update_installed(e, inst) == PAYLOAD_OK;
    }
    update_shown before, mid, after;
    int had = read_shown(e, &before);
    int have = update_next_shown(e->own, had ? &before : NULL, has_inst ? inst->payload_version : NULL, has_inst ? inst->win_app.version : NULL, &mid);
    have = update_next_shown(e->own, have ? &mid : NULL, m && m->has_windows ? m->payload_version : NULL, m && m->has_windows ? m->win_app.version : NULL, &after);
    char p[UPDATE_PATH_MAX];
    join(p, sizeof p, e->support, shown_name, NULL);
    if (have) {
        if (!had || strcmp(before.shown, after.shown) || strcmp(before.app, after.app)) {
            char rec[2 * PAYLOAD_VERSION_MAX + 2];
            size_t n = update_shown_format(&after, rec, sizeof rec);
            plat_mkdirs(e->support);
            if (n && !win_write_atomic(e->support, shown_name, rec, n) && e->os->set_display_version) e->os->set_display_version(e->os->ctx, after.shown);
        }
    } else if (had || size_of(p) >= 0) {   /* a record for another app (this one replaced it): cleared */
        remove_file(e->support, shown_name);
        if (e->os->set_display_version) e->os->set_display_version(e->os->ctx, e->own);
    }
    if (shown) copy_str(shown, PAYLOAD_VERSION_MAX, have ? after.shown : e->own);
    free(inst);
    return update_newer(m, e->own);
}

int update_seen(const update_env *e, const payload_manifest *m, char shown[PAYLOAD_VERSION_MAX]) {
    plat_mutex_lock(&seen_lock);   /* nothing it calls takes this lock again */
    int r = seen_locked(e, m, shown);
    plat_mutex_unlock(&seen_lock);
    return r;
}

/* ------------------------------------------------------------------ the check */

typedef struct {
    char *json, *sig;
    size_t jn, sn;
    int fetched;   /* payload.json came */
    int r;         /* payload_check's, or -1 */
    payload_manifest m;
} cand;

static void cand_free(cand *c) {
    free(c->json);
    free(c->sig);
    c->json = c->sig = NULL;
}

/* latest's payload.json and its signature, checked */
static void fetch_latest(const update_env *e, const char *floor, cand *c) {
    char url[UPDATE_URL_MAX];
    int status;
    c->r = -1;
    if (update_url(e->base, NULL, json_name, url, sizeof url)) return;
    int r = fetch_mem(e, url, PAYLOAD_MAX, &c->json, &c->jn, &status);
    if (r != UPDATE_NET_OK) {
        say(e, "payload.json: couldn't download (%d, HTTP %d)", r, status);
        return;
    }
    c->fetched = 1;
    /* the signature: only an HTTP answer means none is published (refused below as unsigned); a
     * network error is offline, never "unverified" */
    r = update_url(e->base, NULL, sig_name, url, sizeof url) ? UPDATE_NET_HTTP : fetch_mem(e, url, 4096, &c->sig, &c->sn, &status);
    if (r != UPDATE_NET_OK && r != UPDATE_NET_HTTP) {
        say(e, "payload.json.sig: couldn't download (%d, HTTP %d)", r, status);
        c->fetched = 0;
        return;
    }
    if (r != UPDATE_NET_OK) c->sig = NULL, c->sn = 0;
    c->r = payload_check(c->json, c->jn, c->sig, c->sn, e->keys, e->nkeys, floor, &c->m);
    char why[160];
    if (c->r == PAYLOAD_OK)
        say(e, "payload.json: payload %s, %s, signed with the %s key", c->m.payload_version,
            c->m.has_windows ? "Windows app" : "no Windows block", c->m.signed_by ? c->m.signed_by : "?");
    else
        say(e, "payload.json: REFUSED, %s",
            payload_refusal_text(c->r, c->r == PAYLOAD_OLDER ? c->m.payload_version : NULL, floor, why, sizeof why));
}

int update_check(const update_env *e, payload_manifest *out) {
    update_store st;
    update_store_init(e, &st);
    char rec[64], floor[64];
    int fa = payload_accepted_floor(payload_read_floor(&st.files, rec) ? rec : NULL, e->own, floor);
    if (fa < 0) return UPDATE_CHECK_UNVERIFIED;
    cand *c = calloc(1, sizeof *c);
    if (!c) return UPDATE_CHECK_OFFLINE;
    fetch_latest(e, fa ? floor : NULL, c);
    int result;
    if (c->r != PAYLOAD_OK || !c->m.has_windows) {
        /* valid without the windows block: no news; older than the floor: older; else unverified
         * or offline */
        result = c->r == PAYLOAD_OK    ? UPDATE_CHECK_NO_NEWS
               : c->r == PAYLOAD_OLDER ? UPDATE_CHECK_OLDER
               : c->fetched            ? UPDATE_CHECK_UNVERIFIED
                                       : UPDATE_CHECK_OFFLINE;
    } else {
        plat_mkdirs(st.payload_dir);   /* the store writes into folders that exist */
        int r = payload_accept(&st.files, c->json, c->jn, c->sig, c->sn, out);
        if (r == PAYLOAD_OK) {
            say(e, "payload %s taken", out->payload_version);
            result = UPDATE_CHECK_OK;
        } else {
            result = r == PAYLOAD_NOT_SAVED ? UPDATE_CHECK_OFFLINE : UPDATE_CHECK_UNVERIFIED;
            say(e, "payload %s: not saved (%d)", c->m.payload_version, r);
        }
    }
    cand_free(c);
    free(c);
    return result;
}

/* ------------------------------------------------------------------ staging */

static int read_current(const update_env *e, char out[PAYLOAD_VERSION_MAX]) {
    char p[UPDATE_PATH_MAX], *b;
    size_t n;
    out[0] = 0;
    if (join(p, sizeof p, e->apps, current_name, NULL) || read_small(p, UPDATE_RECORD_MAX, &b, &n)) return 0;
    int ok = update_current_parse(b, n, out);
    free(b);
    return ok;
}

/* current read through the folder's handle d (never by path): 1 with the version, 0 none */
static int read_current_at(plat_dir d, char out[PAYLOAD_VERSION_MAX]) {
    plat_file f;
    int64_t size = 0;
    out[0] = 0;
    if (plat_file_open_at(d, current_name, UPDATE_RECORD_MAX, &size, &f)) return 0;
    char *b = malloc(size ? (size_t)size : 1);
    int e = b ? plat_read_all(f, b, (size_t)size) : ENOMEM;
    plat_file_close(f);
    int ok = !e && update_current_parse(b, (size_t)size, out);
    free(b);
    return ok;
}

#ifndef ESTALE
#  define ESTALE 151   /* not in the UCRT's errno.h (plat_win.h's) */
#endif

/* The running exe's id, taken once at start (review UH1): what apps_open and update_in_place
 * compare <own>\RB Stems Plus.exe with, so a folder swapped in later never passes as this app's.
 * One per update_os (a process has one: update_os_windows; the tests' worlds have one per app),
 * the first capture for it wins. */
static plat_mutex self_lock = PLAT_MUTEX_INIT;
static struct {
    const update_os *os;
    plat_id id;
} selves[8];
static int nselves;

int update_self_capture(const update_env *e) {
    char self[UPDATE_PATH_MAX];
    plat_stat st;
    if (!e || !e->os || !e->os->self_path) return EINVAL;
    plat_mutex_lock(&self_lock);
    int r = 0, have = 0;
    for (int i = 0; i < nselves && !have; i++) have = selves[i].os == e->os;
    if (!have) {
        r = e->os->self_path(e->os->ctx, self, sizeof self) ? EIO : plat_stat_path(self, 0, &st);
        if (!r && st.kind != PLAT_FILE) r = EINVAL;
        if (!r && nselves == (int)(sizeof selves / sizeof selves[0])) r = ENOSPC;
        if (!r) {
            selves[nselves].os = e->os;
            selves[nselves++].id = st.id;
        }
    }
    plat_mutex_unlock(&self_lock);
    return r;
}

#ifdef UPDATE_TEST_HOOKS
void update_test_self_reset(void) {
    plat_mutex_lock(&self_lock);
    nselves = 0;
    plat_mutex_unlock(&self_lock);
}
#endif

/* 1 with the id captured at start for e's OS, 0 none was (fail closed) */
static int self_id(const update_env *e, plat_id *out) {
    int have = 0;
    plat_mutex_lock(&self_lock);
    for (int i = 0; i < nselves && !have; i++)
        if (selves[i].os == e->os) {
            *out = selves[i].id;
            have = 1;
        }
    plat_mutex_unlock(&self_lock);
    return have;
}

/* The apps folder through one handle, for every read, delete and rename in it (nothing there is
 * read, deleted or renamed by path): opened without following a link at it, and checked to be the
 * folder this app runs from (<own>\RB Stems Plus.exe, through this handle, is the running exe's
 * id, taken at start: update_self_capture). 0, or an errno with nothing open (ESTALE: another
 * folder, or no id taken): nothing in it is changed then. quiet: no log line (update_in_place's
 * every refresh). */
static int apps_open_q(const update_env *e, plat_dir *d, int quiet) {
    plat_stat b;
    plat_id self;
    plat_dir od;
    if (!e->apps || !e->own) return EINVAL;
    int r = plat_dir_open(e->apps, d);
    if (r) {
        if (!quiet) say(e, "update: %s can't be opened (%s): nothing in it is changed", e->apps, strerror(r));
        return r;
    }
    r = self_id(e, &self) ? 0 : ESTALE;
    if (!r && !(r = plat_dir_open_at(*d, e->own, &od))) {
        r = plat_stat_at(od, APP_EXE, &b);
        plat_dir_close(od);
    }
    if (!r && (b.kind != PLAT_FILE || !plat_id_same(b.id, self))) r = ESTALE;
    if (!r) (void)STEP(UPDATE_STEP_APPS_OPEN);   /* test builds: a swap after the check */
    if (r) {
        if (!quiet) say(e, "update: %s isn't the folder this app runs from (%s): nothing in it is changed", e->apps, strerror(r));
        plat_dir_close(*d);
    }
    return r;
}
static int apps_open(const update_env *e, plat_dir *d) { return apps_open_q(e, d, 0); }

int update_in_place(const update_env *e) {
    char cur[PAYLOAD_VERSION_MAX];
    plat_dir d;
    if (apps_open_q(e, &d, 1)) return 1;
    int ok = read_current_at(d, cur) && !strcmp(cur, e->own);
    plat_dir_close(d);
    return ok ? 0 : 1;
}

int update_where(const update_env *e) {
    if (!e->apps || !e->own || update_in_place(e)) return 1;
    plat_dir d;
    int r = plat_dir_open(e->apps, &d);
    if (!r) {
        r = plat_dir_writable(d);
        plat_dir_close(d);
    }
    return r ? 2 : 0;
}

int update_app_fate(const update_env *e) { return update_where(e) ? 2 : 0; }

/* the files a Windows app zip must hold (release.md §3.1, T7's file list), '/'-separated */
static const char *const required[] = {
    APP_EXE, "x64/rbsp-core.dll", "arm64/rbsp-core.dll", "rbsp-watch-x64.exe", "rbsp-watch-arm64.exe",
    "admin/" APP_EXE, "stub/x64/" APP_EXE, "stub/arm64/" APP_EXE, "NOTICE", "NOTICE-windows.txt", "LICENSE",
};

typedef struct {
    plat_file f;
} pe_src;
static int pe_read(void *ctx, uint64_t off, void *buf, size_t n) {
    pe_src *s = ctx;
    size_t got = 0;
    return plat_pread(s->f, buf, n, (int64_t)off, &got) || got != n;
}

/* whether everything under d is a regular file or a folder (no link, no reparse point) */
static int plain_tree(plat_dir d, int depth);
typedef struct {
    plat_dir d;
    int depth, bad;
} tree_walk;
static int tree_each(void *ctx, const char *name, const plat_stat *st) {
    tree_walk *w = ctx;
    if (st->kind == PLAT_FILE) return 0;
    if (st->kind != PLAT_DIR || w->depth >= UNZIP_DEPTH_MAX) return w->bad = 1;
    plat_dir sub;
    if (plat_dir_open_at(w->d, name, &sub)) return w->bad = 1;
    if (!plain_tree(sub, w->depth + 1)) w->bad = 1;
    plat_dir_close(sub);
    return w->bad;
}
static int plain_tree(plat_dir d, int depth) {
    tree_walk w = {d, depth, 0};
    return plat_dir_each(d, tree_each, &w) == 0 && !w.bad;
}

/* the unpacked app in the folder d (the staging handle: what is checked is what gets renamed,
 * review UH2) is the one m names: 0, or a reason for the log */
static const char *check_app_at(plat_dir d, const payload_manifest *m) {
    for (size_t i = 0; i < sizeof required / sizeof required[0]; i++) {
        plat_file f;
        int64_t size;
        if (open_rel(d, required[i], INT64_MAX, &size, &f)) return "a file of the app is missing";
        plat_file_close(f);
    }
    static const struct {
        const char *rel;
        size_t off;
    } hashed[] = {
        {"admin/" APP_EXE, offsetof(payload_manifest, win_app.admin_sha256)},
        {"stub/x64/" APP_EXE, offsetof(payload_manifest, win_app.stub_x64)},
        {"stub/arm64/" APP_EXE, offsetof(payload_manifest, win_app.stub_arm64)},
    };
    for (size_t i = 0; i < sizeof hashed / sizeof hashed[0]; i++) {
        if (!verified_at(d, hashed[i].rel, (const char *)m + hashed[i].off)) return "the worker or a stub isn't the one the payload names";
    }
    /* the app's ProductVersion is the version the payload names (a "+build" suffix aside) */
    pe_src src;
    int64_t size;
    if (plat_file_open_at(d, APP_EXE, INT64_MAX, &size, &src.f)) return "the app can't be read";
    compat_pe pe;
    int pr = compat_pe_read(pe_read, &src, &pe);
    plat_file_close(src.f);
    size_t vn = strlen(m->win_app.version);
    if (pr || strncmp(pe.product_version, m->win_app.version, vn) || (pe.product_version[vn] && pe.product_version[vn] != '+'))
        return "the app's version isn't the one the payload names";
    if (strcmp(pe.product_name, "RB Stems Plus")) return "the app's ProductName isn't RB Stems Plus";
    return plain_tree(d, 0) ? NULL : "it holds a link or a reparse point";
}

/* name in the apps folder's handle and its tree (never a link followed); ENOENT counts as done */
static int remove_tree_at(const update_env *e, plat_dir apps, const char *name) {
    int r = e->os->remove_tree_at(e->os->ctx, apps, name);
    return r == ENOENT ? 0 : r;
}


static int read_pending(const update_env *e, update_pending *p);

int update_stage(const update_env *e, char to[PAYLOAD_VERSION_MAX], int *key) {
    int k = UPDATE_FAIL_NONE, result = UPDATE_STAGE_FAILED;
    to[0] = 0;
    /* this app's own update still committing (its after-render on a worker while a flow asks for
     * the next): no new one over its records */
    update_pending own_p;
    if (read_pending(e, &own_p) == 1 && !strcmp(own_p.to, e->own)) {
        say(e, "update: this app's own update from %s isn't finished yet: no new one now", own_p.from);
        if (key) *key = UPDATE_FAIL_NONE;
        return UPDATE_STAGE_FAILED;
    }
    update_store st;
    update_store_init(e, &st);
    payload_manifest *m = malloc(sizeof *m);
    char upd[UPDATE_PATH_MAX], zip[UPDATE_PATH_MAX], url[UPDATE_URL_MAX], staging[UPDATE_PATH_MAX], target[UPDATE_PATH_MAX], name[96];
    char cur[PAYLOAD_VERSION_MAX], *zbuf = NULL, *json = NULL, *sig = NULL;
    size_t zn = 0, jn = 0, sn = 0;
    plat_dir apps, sd;   /* the one checked handle every change in the apps folder goes through; the staging folder's */
    plat_stat sdst;
    int have_apps = 0, have_sd = 0;
    if (!m) goto done;
    int r = payload_saved(&st.files, m);
    if (r != PAYLOAD_OK || !update_newer(m, e->own)) {
        say(e, "update: no newer app in the saved payload.json (%d)", r);
        goto done;
    }
    if (update_in_place(e)) {
        say(e, "update: this app isn't %s" SEP "%s" SEP APP_EXE " named by current", e->apps, e->own);
        k = UPDATE_FAIL_APP;
        goto done;
    }
    const char *v = m->win_app.version;
    snprintf(name, sizeof name, ".staging-%s", v);
    if (join(upd, sizeof upd, e->support, update_dir, NULL) || join(zip, sizeof zip, upd, m->win_app.zip, NULL) ||
        update_url(e->base, m->payload_version, m->win_app.zip, url, sizeof url) || join(staging, sizeof staging, e->apps, name, NULL) ||
        join(target, sizeof target, e->apps, v, NULL)) {
        k = UPDATE_FAIL_DOWNLOAD;
        goto done;
    }
    plat_mkdirs(upd);
    switch (fetch_file(e, url, upd, m->win_app.zip, m->win_app.sha256, UPDATE_ZIP_MAX)) {
    case FETCH_OK: break;
    case FETCH_OFFLINE: result = UPDATE_STAGE_OFFLINE; goto done;
    case FETCH_NO_SPACE: result = UPDATE_STAGE_NO_SPACE; goto done;
    default: k = UPDATE_FAIL_DOWNLOAD; goto done;
    }
    /* unpacked into a fresh staging folder */
    if (apps_open(e, &apps)) {
        k = UPDATE_FAIL_STAGING;
        goto done;
    }
    have_apps = 1;
    if (remove_tree_at(e, apps, name)) {
        say(e, "update: %s can't be removed", staging);
        k = UPDATE_FAIL_STAGING;
        goto done;
    }
    r = plat_mkdir_at(apps, name);
    /* the staging folder through its own handle: unpacked and checked there, and the folder
     * renamed only while it is still that one (its id; review UH2) */
    if (!r && !(r = plat_dir_open_at(apps, name, &sd))) {
        have_sd = 1;
        if ((r = plat_dir_stat(sd, &sdst))) say(e, "update: %s can't be looked at (%s)", staging, strerror(r));
    }
    if (r || (r = read_whole(zip, UPDATE_ZIP_MAX, &zbuf, &zn))) {
        say(e, "update: couldn't make %s or read the zip (%s)", staging, strerror(r));
        k = UPDATE_FAIL_COPY;
        if (r == ENOSPC) result = UPDATE_STAGE_NO_SPACE;
        goto fail_staging;
    }
    int err = 0, u = unzip_extract_at(zbuf, zn, sd, &err);
    if (u != UNZIP_OK) {
        say(e, "update: couldn't unpack %s (%d, %s)", m->win_app.zip, u, err ? strerror(err) : "-");
        k = UPDATE_FAIL_COPY;
        if (err == ENOSPC) result = UPDATE_STAGE_NO_SPACE;
        goto fail_staging;
    }
    const char *why = check_app_at(sd, m);
    if (why) {
        say(e, "update: the new app didn't check out: %s", why);
        k = UPDATE_FAIL_COPY_CHECK;
        goto fail_staging;
    }
    (void)STEP(UPDATE_STEP_STAGED);   /* test builds: a swap between the check and the rename */
    plat_stat now;
    if (plat_stat_at(apps, name, &now) || now.kind != PLAT_DIR || !plat_id_same(now.id, sdst.id)) {
        say(e, "update: %s isn't the folder that was checked", staging);
        k = UPDATE_FAIL_STAGING;
        goto fail_staging;
    }
    /* into its version folder; one left by an earlier try goes first (never this app's or current's,
     * current read through the same handle: review UH3) */
    read_current_at(apps, cur);
    if (!strcmp(v, e->own) || !strcmp(v, cur) || remove_tree_at(e, apps, v) || plat_rename_at(apps, name, apps, v)) {
        say(e, "update: %s is in the way", target);
        k = UPDATE_FAIL_STAGING;
        goto fail_staging;
    }
    /* the payload that installs it, for the new app's installed-release record, then pending */
    update_pending pend;
    char rec[UPDATE_RECORD_MAX];
    copy_str(pend.from, sizeof pend.from, e->own);
    copy_str(pend.to, sizeof pend.to, v);
    pend.pid = (uint32_t)plat_pid();
    pend.started = plat_process_start();
    size_t rn = update_pending_format(&pend, rec, sizeof rec);
    if (read_pair(st.payload_dir, &json, &jn, &sig, &sn) || write_pair(upd, json, jn, sig, sn) || !rn ||
        (r = win_write_atomic(upd, pending_name, rec, rn))) {
        say(e, "update: couldn't write %s" SEP "pending", upd);
        k = UPDATE_FAIL_STAGING;
        remove_tree_at(e, apps, v);
        goto done;
    }
    remove_file(upd, m->win_app.zip);
    say(e, "update: %s -> %s staged in %s", e->own, v, target);
    copy_str(to, PAYLOAD_VERSION_MAX, v);
    result = UPDATE_STAGED;
    goto done;
fail_staging:
    if (have_sd) plat_dir_close(sd);
    have_sd = 0;
    remove_tree_at(e, apps, name);
done:
    if (have_sd) plat_dir_close(sd);
    if (have_apps) plat_dir_close(apps);
    if (key) *key = k;
    free(zbuf);
    free(json);
    free(sig);
    free(m);
    return result;
}

static void remove_update_records(const update_env *e) {
    char upd[UPDATE_PATH_MAX];
    if (join(upd, sizeof upd, e->support, update_dir, NULL)) return;
    remove_file(upd, started_name);
    remove_file(upd, json_name);
    remove_file(upd, sig_name);
    remove_file(upd, watcher_name);
    remove_file(upd, pending_name);   /* last: a stop before it leaves the update to finish or undo */
}

int update_launch(const update_env *e, const char *to) {
    char exe[UPDATE_PATH_MAX];
    if (join(exe, sizeof exe, e->apps, to, APP_EXE)) return ENAMETOOLONG;
    const char *args[] = {"--swap", NULL};
    int r = e->os->spawn(e->os->ctx, exe, args);
    if (r) {
        say(e, "update: %s didn't start (%s)", exe, strerror(r));
        remove_update_records(e);
    }
    return r;
}

static int read_pending(const update_env *e, update_pending *p) {
    char path[UPDATE_PATH_MAX], *b;
    size_t n;
    if (join(path, sizeof path, e->support, update_dir, pending_name) || read_small(path, UPDATE_RECORD_MAX, &b, &n)) return -1;
    int ok = update_pending_parse(b, n, p);
    free(b);
    return ok ? 1 : 0;   /* 0: there, but not a record */
}

int update_swap_wait(const update_env *e, uint32_t ms) {
    update_pending p;
    if (read_pending(e, &p) != 1 || strcmp(p.to, e->own)) return 1;
    return e->os->wait_exit(e->os->ctx, p.pid, p.started, ms) ? ETIMEDOUT : 0;
}

static int write_result(const update_env *e, int key) {
    char upd[UPDATE_PATH_MAX], line[64];
    const char *l = update_fail_line(key);
    if (!l || join(upd, sizeof upd, e->support, update_dir, NULL)) return EINVAL;
    plat_mkdirs(upd);
    snprintf(line, sizeof line, "%s\n", l);
    return win_write_atomic(upd, result_name, line, strlen(line));
}

int update_swap_failed(const update_env *e, int key) {
    say(e, "update: the new app (%s) failed its checks: %s", e->own, update_fail_line(key) ? update_fail_line(key) : "?");
    int r = write_result(e, key);
    if (!r) remove_update_records(e);   /* else pending stays: the old app reads it as failed */
    char stub[UPDATE_PATH_MAX];
    if (join(stub, sizeof stub, e->apps, APP_EXE, NULL)) return ENAMETOOLONG;
    const char *args[] = {NULL};
    int s = e->os->spawn(e->os->ctx, stub, args);
    return r ? r : s;
}

/* ------------------------------------------------------------------ at start, after the first render */

static int take_result(const update_env *e) {
    char upd[UPDATE_PATH_MAX], p[UPDATE_PATH_MAX], *b;
    size_t n;
    if (join(upd, sizeof upd, e->support, update_dir, NULL) || join(p, sizeof p, upd, result_name, NULL)) return UPDATE_FAIL_NONE;
    int r = read_small(p, UPDATE_RECORD_MAX, &b, &n);
    if (r == ENOENT) return UPDATE_FAIL_NONE;
    int key = r ? UPDATE_FAIL_GENERIC : update_fail_parse(b, n);
    free(b);
    /* kept until the flow's UPDATE_RESULT has said it (update_result_take): a start that ends
     * before that says it at the next start (review WC4) */
    say(e, "the update couldn't replace the app (%s)", update_fail_line(key));
    return key;
}

int update_at_start(const update_env *e) {
    update_pending p;
    int rp = read_pending(e, &p);
    if (rp == 1 && !strcmp(p.to, e->own)) return UPDATE_FAIL_NONE;   /* this app is the update: update_after_render */
    if (rp == 1 && !strcmp(p.from, e->own)) {
        char cur[PAYLOAD_VERSION_MAX];
        read_current(e, cur);
        if (!strcmp(cur, p.to)) return UPDATE_FAIL_NONE;   /* past the commit point: the new app finishes it */
        char dir[UPDATE_PATH_MAX];
        if (join(dir, sizeof dir, e->apps, p.to, NULL)) return UPDATE_FAIL_NONE;
        /* The old app that started the update first: while it runs it can still start the new one,
         * so a probe for the new one would prove nothing (and it is checked before that probe). */
        if (e->os->wait_exit && e->os->wait_exit(e->os->ctx, p.pid, p.started, 0) == ETIMEDOUT) {
            say(e, "update: the app that started the update to %s still runs: this start leaves it alone", p.to);
            return UPDATE_BUSY;
        }
        /* Then the new app, which may still run its --swap (this start came from a pin meanwhile):
         * waited for, `current` read again after every probe (it may commit at any moment). A probe
         * that can't be made changes nothing. Only a probe finding nothing, followed by a `current`
         * that still names this app, lets the update be undone: nothing can start the new app then. */
        if (e->os->running_under) {
            uint64_t bound = (uint64_t)(e->busy_wait_ms ? e->busy_wait_ms : UPDATE_SWAP_WAIT_MS) * 1000000u, t0 = plat_now_mono_ns();
            for (;;) {
                int run = e->os->running_under(e->os->ctx, dir);
                read_current(e, cur);
                if (!strcmp(cur, p.to)) return UPDATE_FAIL_NONE;   /* it committed */
                if (run < 0) {
                    say(e, "update: the process list couldn't be read: the update is left to a later start");
                    return UPDATE_FAIL_NONE;
                }
                if (!run) break;
                if (plat_now_mono_ns() - t0 >= bound) {
                    say(e, "update: %s still runs from %s: this start leaves it alone", p.to, dir);
                    return UPDATE_BUSY;
                }
                plat_sleep_ms(20);
            }
        } else {
            read_current(e, cur);
        }
        if (strcmp(cur, p.from)) {   /* current names neither app: nothing here is known to be undone */
            say(e, "update: current names %s, neither %s nor %s: the update is left alone", cur[0] ? cur : "nothing", p.from, p.to);
            return UPDATE_FAIL_NONE;
        }
        /* the new app never got to its first render, or failed --swap: the pointers never moved */
        {   /* through one checked handle, current read through it again first (review UH3) */
            plat_dir ad;
            int r = apps_open(e, &ad);
            if (!r) {
                r = read_current_at(ad, cur) && !strcmp(cur, p.from) ? remove_tree_at(e, ad, p.to) : ESTALE;
                plat_dir_close(ad);
            }
            if (r) say(e, "update: %s couldn't be removed yet (%s)", dir, strerror(r));
        }
        int key = take_result(e);
        if (key == UPDATE_FAIL_NONE) {
            key = UPDATE_FAIL_GENERIC;
            say(e, "the update to %s didn't finish: the new app never started; %s stays", p.to, e->own);
            /* written before the records go, so a start that ends before UPDATE_RESULT says it
             * next time (WC4) */
            int w = write_result(e, key);
            if (w) say(e, "update: the result couldn't be kept (%s)", strerror(w));
        }
        remove_update_records(e);
        return key;
    }
    if (rp >= 0) {   /* a stale or broken record (neither app is this one) */
        say(e, "update: a stale pending record removed");
        remove_update_records(e);
    }
    return take_result(e);
}

/* the stub of the machine's architecture, when the payload names another one than the installed
 * one: written as .new and checked, the old one copied (flushed) to .old, then one replacing rename
 * of .new onto the stub's name, so that name exists at every point (a stop anywhere leaves the old
 * stub or the new one there). A failure keeps .new and .old: a later start retries
 * (update_after_render). */
static int swap_stub(const update_env *e, const payload_manifest *m) {
    const char *want = !strcmp(e->arch, "arm64") ? m->win_app.stub_arm64 : m->win_app.stub_x64;
    char rel[UPDATE_PATH_MAX];
    if (snprintf(rel, sizeof rel, "%s/stub/%s/" APP_EXE, e->own, !strcmp(e->arch, "arm64") ? "arm64" : "x64") >= (int)sizeof rel) return ENAMETOOLONG;
    /* one checked handle on the apps folder for the whole swap: .new written, hashed and renamed
     * through it, its id taken before the hash and compared again before the rename (review UH2) */
    plat_dir apps;
    int r = apps_open(e, &apps);
    if (r) return r;
    if (verified_at(apps, APP_EXE, want)) {
        plat_stat st;
        if (!plat_stat_at(apps, APP_EXE ".new", &st) && st.kind != PLAT_DIR) plat_unlink_at(apps, APP_EXE ".new");
        plat_dir_close(apps);
        return 0;
    }
    char *b = NULL;
    size_t n = 0;
    payload_sha got;
    plat_file f;
    int64_t size = 0;
    r = open_rel(apps, rel, 16 * 1024 * 1024, &size, &f);
    if (!r) {
        b = malloc(size ? (size_t)size : 1);
        r = b ? plat_read_all(f, b, (size_t)size) : ENOMEM;
        n = (size_t)size;
        plat_file_close(f);
    }
    if (!r) {
        unsigned char md[32];
        if (plat_sha256(b, n, md)) {
            r = EIO;
        } else {
            hex(md, 32, got);
            if (strcmp(got, want)) r = EINVAL;   /* not the stub the payload names */
        }
    }
    if (!r) r = STEP(UPDATE_STEP_STUB_NEW);
    if (!r) r = win_write_at(apps, APP_EXE ".new", b, n);
    free(b);
    plat_stat fresh;
    if (!r && (plat_stat_at(apps, APP_EXE ".new", &fresh) || fresh.kind != PLAT_FILE || !verified_at(apps, APP_EXE ".new", want))) r = EIO;
    if (r) {
        say(e, "update: the new stub couldn't be prepared (%s)", strerror(r));
        plat_dir_close(apps);
        return r;
    }
    plat_stat old;
    if (!plat_stat_at(apps, APP_EXE, &old) && old.kind == PLAT_FILE) {   /* the old one kept as a copy: its name is never empty */
        char *ob = NULL;
        r = STEP(UPDATE_STEP_STUB_OLD);
        if (!r) r = plat_file_open_at(apps, APP_EXE, 16 * 1024 * 1024, &size, &f);
        if (!r) {
            ob = malloc(size ? (size_t)size : 1);
            r = ob ? plat_read_all(f, ob, (size_t)size) : ENOMEM;
            plat_file_close(f);
        }
        if (!r) r = win_write_at(apps, APP_EXE ".old", ob, (size_t)size);
        free(ob);
        if (r) {
            say(e, "update: the stub couldn't be kept as .old (%s); retried at a later start", strerror(r));
            plat_dir_close(apps);
            return r;
        }
    }
    r = STEP(UPDATE_STEP_STUB_REPLACE);
    plat_stat now;
    if (!r && (plat_stat_at(apps, APP_EXE ".new", &now) || now.kind != PLAT_FILE || !plat_id_same(now.id, fresh.id))) r = ESTALE;   /* not the .new checked */
    if (!r) r = plat_rename_at(apps, APP_EXE ".new", apps, APP_EXE);   /* one replacing rename, in the checked folder */
    plat_dir_close(apps);
    if (r) {
        say(e, "update: the new stub couldn't take its place (%s); retried at a later start", strerror(r));
        return r;
    }
    say(e, "update: the start stub replaced (the old one kept until the next start)");
    return 0;
}

static int commit(const update_env *e, const update_pending *p) {
    char upd[UPDATE_PATH_MAX], line[PAYLOAD_VERSION_MAX + 2];
    if (join(upd, sizeof upd, e->support, update_dir, NULL)) return ENAMETOOLONG;
    snprintf(line, sizeof line, "%s\n", e->own);
    int r = win_write_atomic(upd, started_name, line, strlen(line));
    if (!r) r = win_write_atomic(e->apps, current_name, line, strlen(line));   /* the commit point */
    if (r) {
        say(e, "update: couldn't move current to %s (%s)", e->own, strerror(r));
        return r;
    }
    say(e, "update: %s started; current is %s", e->own, e->own);
    /* installed-release: the payload that installed this app, under update_seen's lock (the native
     * PAYLOAD step may record one at the same time) and only over an older record, by
     * install_record's rule: a newer payload naming this app, recorded meanwhile, stays */
    payload_manifest *m = malloc(sizeof *m), *inst = malloc(sizeof *inst);
    char *json, *sig;
    size_t jn, sn;
    int have_m = 0, ierr = 0;
    char shown[PAYLOAD_VERSION_MAX];
    plat_mutex_lock(&seen_lock);
    if (m && inst && !read_pair(upd, &json, &jn, &sig, &sn)) {
        char dir[UPDATE_PATH_MAX];
        if (payload_check(json, jn, sig, sn, e->keys, e->nkeys, NULL, m) == PAYLOAD_OK && m->has_windows &&
            !strcmp(m->win_app.version, e->own) && !join(dir, sizeof dir, e->support, installed_dir, NULL)) {
            have_m = 1;
            ierr = STEP(UPDATE_STEP_INSTALLED);
            if (!ierr && update_installed(e, inst) == PAYLOAD_OK && !older(inst->payload_version, m->payload_version)) {
                say(e, "installed-release: payload %s already recorded for this app: kept", inst->payload_version);
                *m = *inst;
            } else if (!ierr) {
                ierr = write_installed(e, json, jn, sig, sn);
            }
            if (ierr) say(e, "installed-release: couldn't be written (%s)", strerror(ierr));
        }
        free(json);
        free(sig);
    }
    if (!have_m && m && update_installed(e, m) == PAYLOAD_OK) have_m = 1;   /* written by an earlier run of this commit */
    seen_locked(e, NULL, shown);
    plat_mutex_unlock(&seen_lock);
    free(inst);
    if (e->os->set_display_version) e->os->set_display_version(e->os->ctx, shown);
    if (have_m) swap_stub(e, m);   /* a failure doesn't hold the records: a later start retries it */
    else say(e, "update: no verified payload for this app: the stub stays");
    free(m);
    /* the old watcher stopped by its event, the new one started through the stub. That a watcher
     * (by full path and this session, watchers_in: never another process under the folder) was
     * running is noted first (update\watcher), so a commit stopped between the two still starts
     * the new one when it runs again, unless a watcher already runs from this app's folder. */
    char old[UPDATE_PATH_MAX], cur[PAYLOAD_VERSION_MAX], stub[UPDATE_PATH_MAX], mark[UPDATE_PATH_MAX], ownd[UPDATE_PATH_MAX];
    if (strcmp(p->from, e->own) && !join(old, sizeof old, e->apps, p->from, NULL) && !join(mark, sizeof mark, upd, watcher_name, NULL) &&
        !join(ownd, sizeof ownd, e->apps, e->own, NULL)) {
        int had = size_of(mark) >= 0;
        int w = e->os->watchers_in ? e->os->watchers_in(e->os->ctx, old) : 0;
        /* noted before it is stopped; a failed note stops here, the watcher running. A list that
         * can't be read (-1) isn't noted: the stop still finds a watcher of this session, and its
         * 1 starts the new one, unless its own list is cut short too or holds an uninspectable
         * watcher (-1): then none starts until the next logon's Run value (review UF5); a commit
         * cut off after it is repaired by the next logon's Run
         * value (review GH3) */
        if (!had && w == 1) {
            int mr = STEP(UPDATE_STEP_MARKER);
            if (!mr) mr = win_write_atomic(upd, watcher_name, "1\n", 2);
            if (mr) {
                say(e, "update: couldn't note the running watcher (%s): the next start finishes the update", strerror(mr));
                return mr;
            }
            had = 1;   /* noted: a stop that gives -1 still starts the new one (review GH2) */
        }
        int s = e->os->stop_watchers(e->os->ctx, old);
        int stop = STEP(UPDATE_STEP_WATCHER);
        if (stop) return stop;   /* test builds: a stop between the two */
        int spawn = s == 1 || (had && (!e->os->watchers_in || e->os->watchers_in(e->os->ctx, ownd) != 1));
        if (spawn && !join(stub, sizeof stub, e->apps, APP_EXE, NULL)) {
            const char *args[] = {"--watcher", NULL};
            int sp = e->os->spawn(e->os->ctx, stub, args);
            say(e, "update: the watcher restarted from %s%s", e->own, sp ? " FAILED" : "");
        }
        /* the old app's folder, only while current (read through the same handle: review UH3)
         * doesn't name it, and only when the stop gave >= 0 and nothing runs from it (running_under
         * 0, review UF1; asked after the stop, so the stopped watcher is gone from the list):
         * another session's watcher or old app, or an elevated process of ours, keeps its files
         * (review UN7, GH4); the sweep takes the folder once nothing runs there */
        plat_dir ad;
        int busy = s < 0 ? -1 : e->os->running_under ? e->os->running_under(e->os->ctx, old) : 0;
        if (busy) say(e, "update: something may still run from %s: kept for now", old);
        else if (!apps_open(e, &ad)) {
            if (read_current_at(ad, cur) && strcmp(p->from, cur)) {
                int d = remove_tree_at(e, ad, p->from);
                if (d) say(e, "update: %s couldn't be removed yet (%s)", old, strerror(d));
            }
            plat_dir_close(ad);
        }
    }
    if (ierr) {   /* the records stay: the next start runs the commit again */
        say(e, "update: %s -> %s not finished: installed-release wasn't written; the next start finishes it", p->from, e->own);
        return ierr;
    }
    remove_update_records(e);
    say(e, "update: %s -> %s done", p->from, e->own);
    return 0;
}

/* the apps folder's writers in this process: the leftover sweep after the first render and
 * UPDATE_SWAP's staging and launch, one at a time, so the sweep never takes a folder being staged
 * (review WC3). Neither calls the other under it. */
static plat_mutex apps_lock = PLAT_MUTEX_INIT;

#ifdef UPDATE_TEST_HOOKS
/* a test's "kill" (a longjmp out of a step hook) leaves the lock held, as a dead process would
 * leave nothing: the next "start" gets a fresh one */
void update_test_killed(void) { plat_mutex_init(&apps_lock); }
#endif

typedef struct {
    const char *own, *cur;
    char **names;   /* the stale folders' names, deleted after the listing */
    int n, cap;
} sweep_ctx;

static int sweep_each(void *ctx, const char *name, const plat_stat *st) {
    sweep_ctx *s = ctx;
    if (st->kind != PLAT_DIR) return 0;
    int stale = !strncmp(name, ".staging-", 9) || (update_is_version(name) && strcmp(name, s->own) && strcmp(name, s->cur));
    if (!stale) return 0;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 8;
        char **more = realloc(s->names, (size_t)cap * sizeof *more);
        if (!more) return 0;   /* this one stays for a later start */
        s->names = more;
        s->cap = cap;
    }
    size_t k = strlen(name) + 1;
    if ((s->names[s->n] = malloc(k))) memcpy(s->names[s->n++], name, k);
    return 0;
}

static int after_render_locked(const update_env *e) {
    update_pending p;
    int rp = read_pending(e, &p);
    if (rp == 1 && !strcmp(p.to, e->own)) return commit(e, &p);
    if (rp >= 0) return 0;   /* another app's update: left to update_at_start */
    /* the next successful start: the stub kept by the last swap goes; a swap that failed is tried
     * again (one hash of the stub when there is nothing to do); then leftover folders */
    plat_dir od;   /* through the checked handle, never by path (apps_open's contract; UH's GPT re-run, G4b) */
    if (!apps_open_q(e, &od, 1)) {
        plat_stat st;
        if (!plat_stat_at(od, APP_EXE ".old", &st) && st.kind == PLAT_FILE) plat_unlink_at(od, APP_EXE ".old");
        plat_dir_close(od);
    }
    payload_manifest *m = malloc(sizeof *m);
    if (m && update_installed(e, m) == PAYLOAD_OK) swap_stub(e, m);
    free(m);
    /* current read, the folder listed and the stale ones deleted through one checked handle
     * (nothing by path: reviews UH7, UH3) */
    char cur[PAYLOAD_VERSION_MAX];
    plat_dir d;
    if (apps_open(e, &d)) return 0;
    if (!read_current_at(d, cur)) {   /* no current: nothing is known to be stale */
        plat_dir_close(d);
        return 0;
    }
    sweep_ctx s = {e->own, cur, NULL, 0, 0};
    (void)STEP(UPDATE_STEP_SWEEP);
    plat_dir_each(d, sweep_each, &s);
    for (int i = 0; i < s.n; i++) {
        char where[UPDATE_PATH_MAX];
        int bad = join(where, sizeof where, e->apps, s.names[i], NULL);
        /* a version folder only once nothing runs from it and the list was read (running_under
         * 0): another session's watcher keeps it (review UN7, GH4). running_under, not
         * watchers_in: watchers_in counts this session's watchers only (review GR1) */
        if (strncmp(s.names[i], ".staging-", 9) && e->os->running_under && (bad || e->os->running_under(e->os->ctx, where) != 0)) {
            say(e, "leftover %s kept: something may still run from it", bad ? s.names[i] : where);
        } else {
            int r = remove_tree_at(e, d, s.names[i]);
            say(e, "leftover %s %s", bad ? s.names[i] : where, r ? "couldn't be removed yet" : "removed");
        }
        free(s.names[i]);
    }
    free(s.names);
    plat_dir_close(d);
    return 0;
}

int update_after_render(const update_env *e) {
    plat_mutex_lock(&apps_lock);
    int r = after_render_locked(e);
    plat_mutex_unlock(&apps_lock);
    return r;
}

/* ------------------------------------------------------------------ the steps that need the shell */

int update_step_swap(const update_env *e, void (*quit)(void *ctx), void *qctx) {
    if (!quit) {
        say(e, "update: nothing here can quit the app: the update isn't started");
        return UPDATE_STAGE_FAILED;
    }
    char to[PAYLOAD_VERSION_MAX];
    int key = UPDATE_FAIL_NONE;
    plat_mutex_lock(&apps_lock);   /* the sweep waits until the new app has started (WC3) */
    int r = update_stage(e, to, &key);
    int l = r == UPDATE_STAGED ? update_launch(e, to) : 0;
    plat_mutex_unlock(&apps_lock);
    if (r != UPDATE_STAGED) return r;
    if (l) return UPDATE_STAGE_FAILED;   /* pending removed: this app runs on as it was */
    say(e, "update: %s started with --swap: quitting", to);
    quit(qctx);
    return 0;
}

static atomic_int kept_result = UPDATE_FAIL_NONE;

void update_result_keep(int key) {
    atomic_store(&kept_result, key >= UPDATE_FAIL_GENERIC && update_fail_line(key) ? key : UPDATE_FAIL_NONE);
}

int update_result_take(const update_env *e, const char **line) {
    int key = atomic_exchange(&kept_result, UPDATE_FAIL_NONE);
    const char *l = key >= UPDATE_FAIL_GENERIC ? update_fail_line(key) : NULL;
    if (line) *line = l ? l : "";
    char upd[UPDATE_PATH_MAX];
    if (e && !join(upd, sizeof upd, e->support, update_dir, NULL)) remove_file(upd, result_name);   /* said now: gone */
    return l != NULL;
}
