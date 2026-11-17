/* payload.json's saved copy and the floor record (payload.h), over common/plat.h's file layer:
 * plat_win.c on Windows, plat_mac.c in the Mac tests. Payload.swift's savedManifest,
 * acceptManifest and payloadFloor, with writeAtomically's temp-flush-rename. */
#include "plat.h"

#include "payload.h"
#include "fs_write.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"
#endif

enum { PATH_MAX_ = 4096, SIG_READ_MAX = 4096 };

static const char json_name[] = "payload.json", sig_name[] = "payload.json.sig", floor_name[] = "payload-version";

static int join(char *out, const char *dir, const char *name) {
    size_t d = strlen(dir), n = strlen(name);
    if (!d || d + 1 + n + 1 > PATH_MAX_) return -1;
    memcpy(out, dir, d);
    memcpy(out + d, SEP, 1);
    memcpy(out + d + 1, name, n + 1);
    return 0;
}

/* a whole file of at most max bytes: 0 with *buf (malloc'd) and *n; ENOENT; another errno */
static int read_file(const char *dir, const char *name, size_t max, char **buf, size_t *n) {
    char path[PATH_MAX_];
    *buf = NULL;
    *n = 0;
    if (join(path, dir, name)) return ENAMETOOLONG;
    return plat_read_file(path, max, buf, n);
}

int payload_read_floor(const payload_files *f, char out[64]) {
    char *buf;
    size_t n;
    if (read_file(f->floor_dir, floor_name, 256, &buf, &n)) return 0;
    int ok = payload_floor_parse(buf, n, out);
    free(buf);
    return ok;
}

/* the floor now into out: 1, 0 (none), -1 (the minimum isn't a version: refuse everything) */
static int accepted_floor(const payload_files *f, char out[64]) {
    char rec[64];
    return payload_accepted_floor(payload_read_floor(f, rec) ? rec : NULL, f->minimum, out);
}

int payload_saved(const payload_files *f, payload_manifest *out) {
    char *json, *sig;
    size_t jn, sn;
    int e = read_file(f->payload_dir, json_name, PAYLOAD_MAX, &json, &jn);
    if (e == ENOENT) return PAYLOAD_NONE;
    if (e) return e == PLAT_EFTYPE ? PAYLOAD_TOO_BIG : PAYLOAD_NONE;
    if (read_file(f->payload_dir, sig_name, SIG_READ_MAX, &sig, &sn)) sig = NULL, sn = 0;
    char fl[64];
    int a = accepted_floor(f, fl);
    int r = a < 0 ? PAYLOAD_NOT_VALID : payload_check(json, jn, sig, sn, f->keys, f->nkeys, a ? fl : NULL, out);
    free(json);
    free(sig);
    return r;
}

/* One process per store (the app per account, the worker in its own root), so a lock in the
 * process keeps two accepts from interleaving their floor reads and writes. */
static plat_mutex store_lock = PLAT_MUTEX_INIT;

static int accept_locked(const payload_files *f, const void *data, size_t n, const void *sig, size_t sig_n, payload_manifest *out) {
    char rec[64], fl[64];
    int had = payload_read_floor(f, rec);
    int a = accepted_floor(f, fl);
    if (a < 0) return PAYLOAD_NOT_VALID;
    int r = payload_check(data, n, sig, sig_n, f->keys, f->nkeys, a ? fl : NULL, out);
    if (r) return r;
    if (!out->has_windows) return PAYLOAD_NO_NEWS;
    /* the floor first, its write checked: if the pair's write then fails, the floor is a newer
     * signed version and the old pair is refused as older (fails closed) */
    int c = had ? payload_version_cmp(rec, out->payload_version) : -1;
    if (c < 0 || c == 2) {   /* no record, an older one, or one that isn't a version */
        char line[PAYLOAD_VERSION_MAX + 1];
        size_t k = strlen(out->payload_version);
        memcpy(line, out->payload_version, k);
        line[k] = '\n';
        if (win_write_atomic(f->floor_dir, floor_name, line, k + 1)) return PAYLOAD_NOT_SAVED;
    }
    if (win_write_atomic(f->payload_dir, sig_name, sig, sig_n) || win_write_atomic(f->payload_dir, json_name, data, n)) return PAYLOAD_NOT_SAVED;
    return PAYLOAD_OK;
}

int payload_accept(const payload_files *f, const void *data, size_t n, const void *sig, size_t sig_n, payload_manifest *out) {
    plat_mutex_lock(&store_lock);
    int r = accept_locked(f, data, n, sig, sig_n, out);
    plat_mutex_unlock(&store_lock);
    return r;
}
