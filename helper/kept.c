/* kept.c: kept sets and the cache's room (kept.h) */
#include "kept.h"
#include "entries.h"
#include "plat_helper.h"
#include <errno.h>
#include <limits.h>
#include "win_path_max.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define KEPT_FILE_MAX (8 << 20)                 /* the bridge's */
#define KEPT_LINE 130
static const char HEAD[] = "rbstemsplus-kept 1\nuntil ";

/* ---------------------------------------------------------------- keys */

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static int unhex64(const char *s, unsigned char out[32]) {     /* 64 lowercase hex digits */
    for (int i = 0; i < 32; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return 0;
}

static void hex32(const unsigned char in[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", in[i]);
}

/* a set of 32-byte keys (open addressing; an all-zero key, never a SHA-256 in practice, marks a
 * free slot). A chunk's key is made from its model's folder name, so the key alone names it. */
typedef struct { unsigned char (*s)[32]; size_t n, cap; } kset;
static const unsigned char ZERO[32];

static size_t slot(const kset *t, const unsigned char *k) {
    uint64_t h;
    memcpy(&h, k, 8);
    size_t i = (size_t)h & (t->cap - 1);
    while (memcmp(t->s[i], ZERO, 32) && memcmp(t->s[i], k, 32)) i = (i + 1) & (t->cap - 1);
    return i;
}

static int kset_has(const kset *t, const unsigned char *k) { return t->cap && memcmp(t->s[slot(t, k)], ZERO, 32); }

/* 1 if added, 0 if there already, -1 out of memory */
static int kset_add(kset *t, const unsigned char *k) {
    if (!memcmp(k, ZERO, 32)) return 0;
    if ((t->n + 1) * 2 > t->cap) {
        kset b = {calloc(t->cap ? t->cap * 2 : 1024, 32), 0, t->cap ? t->cap * 2 : 1024};
        if (!b.s) return -1;
        for (size_t i = 0; i < t->cap; i++)
            if (memcmp(t->s[i], ZERO, 32)) { memcpy(b.s[slot(&b, t->s[i])], t->s[i], 32); b.n++; }
        free(t->s);
        *t = b;
    }
    size_t i = slot(t, k);
    if (memcmp(t->s[i], ZERO, 32)) return 0;
    memcpy(t->s[i], k, 32);
    t->n++;
    return 1;
}

/* backward-shift deletion: what follows in its probe run moves up, so no search stops short */
static void kset_del(kset *t, const unsigned char *k) {
    if (!t->cap) return;
    size_t i = slot(t, k), mask = t->cap - 1;
    if (!memcmp(t->s[i], ZERO, 32)) return;
    for (size_t j = (i + 1) & mask; memcmp(t->s[j], ZERO, 32); j = (j + 1) & mask) {
        uint64_t h;
        memcpy(&h, t->s[j], 8);
        if (((j - ((size_t)h & mask)) & mask) >= ((j - i) & mask)) { memcpy(t->s[i], t->s[j], 32); i = j; }
    }
    memset(t->s[i], 0, 32);
    t->n--;
}

/* entries as written: model folder and key */
typedef struct { unsigned char m[32], k[32]; } line;
typedef struct { line *v; size_t n, cap; } lines;

static int lines_add(lines *l, const unsigned char *m, const unsigned char *k) {
    if (l->n == l->cap) {
        line *t = realloc(l->v, (l->cap ? l->cap * 2 : 1024) * sizeof *t);
        if (!t) return -1;
        l->v = t;
        l->cap = l->cap ? l->cap * 2 : 1024;
    }
    memcpy(l->v[l->n].m, m, 32);
    memcpy(l->v[l->n].k, k, 32);
    l->n++;
    return 0;
}

static int by_line(const void *a, const void *b) { return memcmp(a, b, sizeof(line)); }

struct kept {
#if KEPT_CACHE_AT
    char dir[PATH_MAX], name[65];
    kept_cache cache;
#else
    char dir[PATH_MAX], name[65], cache[PATH_MAX];
#endif
    time_t until;
    kset others, ours, fresh;           /* other sets' keys; this set's (earlier and this run's); this run's */
    kset held;                          /* this set's earlier keys a track of this run still holds */
    kset counted;                       /* the keys whose size is in bytes (one not in the cache has none yet) */
    lines earlier, now;                 /* this set's earlier file; this run's entries */
    long long bytes;
};

/* ---------------------------------------------------------------- files */

void kept_default_dir(char *buf, size_t n) {
    char home[PATH_MAX];
    buf[0] = 0;
    if (plat_home(home, sizeof home) == 0) {
#ifdef _WIN32
        int r = snprintf(buf, n, "%s\\rbstemsplus\\kept", home);               /* LocalAppData's */
#else
        int r = snprintf(buf, n, "%s/Library/Application Support/rbstemsplus/kept", home);
#endif
        if (r < 0 || (size_t)r >= n) buf[0] = 0;
    }
}

/* a kept file's bytes, as the bridge reads them (parse_kept): its date and, for each entry,
 * f(model, key); -1 if they aren't exactly the format */
static int parse(const char *b, size_t n, long long *until, void (^f)(const unsigned char *, const unsigned char *)) {
    size_t h = sizeof HEAD - 1, i;
    long long u = 0;
    if (n < h || memcmp(b, HEAD, h)) return -1;
    for (i = h; i < n && i - h < 12 && b[i] >= '0' && b[i] <= '9'; i++) u = u * 10 + (b[i] - '0');
    if (i == h || i >= n || b[i] != '\n' || (b[h] == '0' && i - h > 1)) return -1;
    i++;
    if ((n - i) % KEPT_LINE) return -1;
    for (size_t j = i; j < n; j += KEPT_LINE) {
        unsigned char m[32], k[32];
        if (unhex64(b + j, m) || b[j + 64] != ' ' || unhex64(b + j + 65, k) || b[j + 129] != '\n') return -1;
    }
    *until = u;
    for (; f && i < n; i += KEPT_LINE) {
        unsigned char m[32], k[32];
        unhex64(b + i, m);
        unhex64(b + i + 65, k);
        f(m, k);
    }
    return 0;
}

/* a whole file (at most KEPT_FILE_MAX, a plain file not followed through a link), or NULL */
static char *slurp(plat_dir dir, const char *name, size_t *len) {
    plat_file f;
    int64_t size = 0;
    char *b = NULL;
    if (plat_file_open_at(dir, name, KEPT_FILE_MAX, &size, &f)) return NULL;
    if ((b = malloc((size_t)size + 1))) {
        size_t got = 0, r = 0;
        int e = 0;
        while (got <= (size_t)size && !(e = plat_read(f, b + got, (size_t)size + 1 - got, &r)) && r > 0) got += r;
        if (e || got != (size_t)size) { free(b); b = NULL; }
        else *len = got;
    }
    plat_file_close(f);
    return b;
}

#if KEPT_CACHE_AT
static int sub_open(plat_dir parent, const char *name, int create, plat_dir *out) {
    int e = plat_dir_open_at(parent, name, out);
    if (!create || e != ENOENT) return e;
    e = plat_mkdir_at(parent, name);
    if (e && e != EEXIST) return e;
    return plat_dir_open_at(parent, name, out);
}

int kept_prefix_dir(kept_cache cache, const char *model, const char *key, int create, plat_dir *x) {
    *x = plat_dir_cwd();
    if (!plat_dir_ok(cache)) return ENOENT;
    plat_dir d;
    int e = sub_open(cache, model, create, &d);
    if (e) return e;
    char prefix[3] = {key[0], key[1], 0};
    e = sub_open(d, prefix, create, x);
    plat_dir_close(d);
    return e;
}

static int prefix_open(kept_cache cache, const char *model, const char *key, plat_dir *x) {
    return kept_prefix_dir(cache, model, key, 0, x);
}
#else
/* the prefix folder <cache>/<model>/<key[0:2]> (no link at its last component) */
static int prefix_open(const char *cache, const char *model, const char *key, plat_dir *x) {
    char p[PATH_MAX + 200];
    snprintf(p, sizeof p, "%s/%s/%.2s", cache, model, key);
    return plat_dir_open(p, x);
}
#endif

/* the disk space of entry MODEL/KEY in CACHE: its .flac, .desc and their companions (the cache's
 * size rule, entries_size_at) */
static long long entry_bytes(kept_cache cache, const char *model, const char *key) {
    plat_dir x;
#if !KEPT_CACHE_AT
    if (!cache[0]) return 0;                /* no cache to look at (--quick, another drive) */
#endif
    if (prefix_open(cache, model, key, &x)) return 0;
    long long sum = entries_size_at(x, key);
    plat_dir_close(x);
    return sum;
}

#if KEPT_CACHE_AT
static long long found_entry(kept_cache cache, const char *model, const char *key, int touch) {
#else
long long kept_entry_bytes(kept_cache cache, const char *model, const char *key) {
#endif
    plat_dir x;
    plat_stat st;
    char name[80];
    if (prefix_open(cache, model, key, &x)) return -1;
    entries_file(name, sizeof name, key, 0);                /* <key>.flac */
    long long sum = plat_stat_at(x, name, &st) || st.kind != PLAT_FILE ? -1 : entries_size_at(x, key);
#if KEPT_CACHE_AT
    if (sum >= 0 && touch) {
        plat_file f;
        if (plat_file_open_at(x, name, INT64_MAX, NULL, &f)) sum = -1;
        else { plat_file_touch(f); plat_file_close(f); }
    }
#endif
    plat_dir_close(x);
    return sum;
}

#if KEPT_CACHE_AT
long long kept_entry_bytes(kept_cache cache, const char *model, const char *key) {
    return found_entry(cache, model, key, 0);
}

long long kept_touch_entry(kept_cache cache, const char *model, const char *key) {
    return found_entry(cache, model, key, 1);
}
#endif

/* PATH (absolute) and the folders above it, as mkdir -p: 0, or -1 with errno that of the first
 * that couldn't be made (one there already is fine) */
#ifdef _WIN32
#define IS_SEP(c) ((c) == '/' || (c) == '\\')
#define ROOT_LEN(p) ((p)[0] && (p)[1] == ':' ? 3 : 1)       /* after "X:\" */
#else
#define IS_SEP(c) ((c) == '/')
#define ROOT_LEN(p) 1
#endif
static int mkdirs(const char *path) {
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) { errno = ENAMETOOLONG; return -1; }
    if (strlen(p) < (size_t)ROOT_LEN(p)) { errno = EINVAL; return -1; }
    for (char *c = p + ROOT_LEN(p);; c++) {
        if (*c && !IS_SEP(*c)) continue;
        char was = *c;
        *c = 0;
        int e = plat_mkdir_at(plat_dir_cwd(), p);
        if (e && e != EEXIST) { errno = e; return -1; }
        if (!was) return 0;
        *c = was;
    }
}

static int name_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 64) return 0;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
    return 1;
}

static int is_set(const char *name) {           /* <name>.kept, not a dot file (someone's temporary) */
    size_t l = strlen(name);
    return name[0] != '.' && l > 5 && !strcmp(name + l - 5, ".kept");
}

kept *kept_open(const char *dir, const char *name, time_t until, kept_cache cache, char *why, size_t n) {
    if (name && !name_ok(name)) { snprintf(why, n, "a kept set's name is 1 to 64 letters, digits, - or _: %s", name); return NULL; }
#if !defined(_WIN32) && !defined(RBSP_TEST_WIN_PREPARE)
    if (name && (!dir[0] || mkdirs(dir))) { snprintf(why, n, "the kept sets' folder can't be made: %s", dir[0] ? strerror(errno) : "no home folder"); return NULL; }
#endif
    __block kept *k = calloc(1, sizeof *k);
    if (!k) { snprintf(why, n, "out of memory"); return NULL; }
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
    int r = snprintf(k->dir, sizeof k->dir, "%s", dir);
    if (r < 0 || (size_t)r >= sizeof k->dir) {
        snprintf(why, n, "the kept sets' folder path is too long");
        free(k);
        return NULL;
    }
    if (name && (!dir[0] || mkdirs(dir))) {
        snprintf(why, n, "the kept sets' folder can't be made: %s", dir[0] ? strerror(errno) : "no home folder");
        free(k);
        return NULL;
    }
#else
    snprintf(k->dir, sizeof k->dir, "%s", dir);
#endif
    snprintf(k->name, sizeof k->name, "%s", name ? name : "");
#if KEPT_CACHE_AT
    k->cache = cache;
#else
    snprintf(k->cache, sizeof k->cache, "%s", cache);
#endif
    k->until = until;
    plat_dir d = {0};
    int de = dir[0] ? plat_dir_open(dir, &d) : ENOENT;
    if (de && !name && (!dir[0] || de == ENOENT)) return k;               /* nothing is kept */
    if (de) { snprintf(why, n, "the kept sets' folder can't be read: %s", strerror(de)); kept_close(k); return NULL; }
    char own_[80] = "";
    const char *own = own_;
    if (name) snprintf(own_, sizeof own_, "%s.kept", name);
    time_t now = time(NULL);
    __block size_t others = 0;
    __block int oom = 0;
    errno = 0;
    int listed = plat_dir_each_b(d, ^(const char *ename, const plat_stat *est) {
        (void)est;
        if (!is_set(ename)) return 0;
        size_t len = 0;
        long long u = 0;
        char *b = slurp(d, ename, &len);
        int mine = !strcmp(ename, own), ok = b && parse(b, len, &u, NULL) == 0;
        if (ok && u <= now) {                   /* its date has passed: it goes, with its manifest */
            char mf[300];
            snprintf(mf, sizeof mf, "%.*s.manifest", (int)(strlen(ename) - 5), ename);
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
            int e = plat_unlink_at(d, ename);
            /* The bridge counts files before reading them, including expired sets. */
            if (e && e != ENOENT) {
                if (!mine) others++;
            } else {
                plat_unlink_at(d, mf);
            }
#else
            plat_unlink_at(d, ename);
            plat_unlink_at(d, mf);
#endif
            free(b);
            return 0;
        }
        if (!mine) others++;          /* one the bridge can't read still counts towards its cap */
        if (ok)
            parse(b, len, &u, ^(const unsigned char *m, const unsigned char *key) {
                int r = kset_add(mine ? &k->ours : &k->others, key);
                if (r < 0 || (mine && lines_add(&k->earlier, m, key))) { oom = 1; return; }
                /* this set's own count only as its current tracks hold them (kept_hold, kept_add) */
                if (!mine && r == 1 && !kset_has(&k->counted, key)) {
                    char mh[65], kh[65];
                    hex32(m, mh);
                    hex32(key, kh);
                    long long size = entry_bytes(k->cache, mh, kh);
                    if (size > 0) {
                        if (kset_add(&k->counted, key) < 0) { oom = 1; return; }
                        k->bytes += size;
                    }
                }
            });
        free(b);
        return 0;
    });
    int le = errno ? errno : EIO;
    plat_dir_close(d);
    if (listed || oom) { snprintf(why, n, "the kept sets can't be read: %s", oom ? "out of memory" : strerror(le)); kept_close(k); return NULL; }
    if (name && others >= KEPT_SETS_MAX) {
        snprintf(why, n, "%d playlists are kept already: stop keeping one first", KEPT_SETS_MAX);
        kept_close(k);
        return NULL;
    }
    return k;
}

void kept_close(kept *k) {
    if (!k) return;
    free(k->others.s);
    free(k->ours.s);
    free(k->fresh.s);
    free(k->counted.s);
    free(k->held.s);
    free(k->earlier.v);
    free(k->now.v);
    free(k);
}

void kept_add(kept *k, const char *model, const char *key, long long bytes) {
    unsigned char m[32], b[32];
    if (unhex64(model, m) || unhex64(key, b)) return;
    if (kset_add(&k->fresh, b) == 1) lines_add(&k->now, m, b);
    kset_add(&k->ours, b);
    if (bytes > 0 && kset_add(&k->counted, b) == 1) k->bytes += bytes;
}

void kept_hold(kept *k, const char *model, const char *key, int count) {
    unsigned char b[32];
    if (unhex64(key, b) || !kset_has(&k->ours, b)) return;
    kset_add(&k->held, b);
    if (!count || kset_has(&k->counted, b)) return;
    long long size = entry_bytes(k->cache, model, key);
    if (size > 0 && kset_add(&k->counted, b) == 1) k->bytes += size;
}

void kept_unhold(kept *k, const char *key) {
    unsigned char b[32];
    if (!unhex64(key, b) && !kset_has(&k->fresh, b)) kset_del(&k->ours, b);
}

long long kept_adds(const kept *k, const char *model, const char *key) {
    unsigned char b[32];
    return unhex64(key, b) || kset_has(&k->counted, b) ? 0 : entry_bytes(k->cache, model, key);
}

void kept_release(kept *k) {
    for (size_t i = 0; i < k->earlier.n; i++)
        if (!kset_has(&k->held, k->earlier.v[i].k)) kset_del(&k->ours, k->earlier.v[i].k);
}

long long kept_bytes(const kept *k) { return k ? k->bytes : 0; }

int kept_has(const kept *k, const char *key) {
    unsigned char b[32];
    return k && !unhex64(key, b) && (kset_has(&k->ours, b) || kset_has(&k->others, b));
}

int kept_write(kept *k, int all_done, char *why, size_t n) {
    lines all = {0};
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
    for (size_t i = 0; i < k->now.n; i++)
        if (lines_add(&all, k->now.v[i].m, k->now.v[i].k)) goto oom;
    for (size_t i = 0; !all_done && i < k->earlier.n; i++)
        if (!kset_has(&k->fresh, k->earlier.v[i].k) && lines_add(&all, k->earlier.v[i].m, k->earlier.v[i].k))
            goto oom;
    int header = snprintf(NULL, 0, "%s%lld\n", HEAD, (long long)k->until);
    if (k->until < 0 || (long long)k->until > 999999999999LL || header < 0 ||
        all.n > (KEPT_FILE_MAX - (size_t)header) / KEPT_LINE) {
        free(all.v);
        snprintf(why, n, "the kept set exceeds the bridge's format limits");
        return -1;
    }
#else
    for (size_t i = 0; i < k->now.n; i++) lines_add(&all, k->now.v[i].m, k->now.v[i].k);
    for (size_t i = 0; !all_done && i < k->earlier.n; i++)
        if (!kset_has(&k->fresh, k->earlier.v[i].k)) lines_add(&all, k->earlier.v[i].m, k->earlier.v[i].k);
    if (all.n < k->now.n) { free(all.v); snprintf(why, n, "out of memory"); return -1; }
#endif
    if (all.n) qsort(all.v, all.n, sizeof *all.v, by_line);
    char tmp[120], path[100];
    plat_dir d;
    plat_file fd = {0};
    FILE *f = NULL;
    snprintf(tmp, sizeof tmp, ".%s.kept.%d.tmp", k->name, plat_pid());
    snprintf(path, sizeof path, "%s.kept", k->name);
    int e = plat_dir_open(k->dir, &d);
    if (!e) {
        plat_unlink_at(d, tmp);                 /* an earlier process's with this pid */
        if (!(e = plat_file_create_at(d, tmp, 0, &fd)) && !(f = plat_file_stdio(fd, "w"))) {
            e = errno ? errno : EIO;
            plat_file_close(fd);
            plat_unlink_at(d, tmp);
        }
        if (!f) plat_dir_close(d);
    }
    if (!f) {
        free(all.v);
        snprintf(why, n, "the kept set can't be written: %s", strerror(e));
        return -1;
    }
    fprintf(f, "%s%lld\n", HEAD, (long long)k->until);
    for (size_t i = 0; i < all.n; i++) {
        char mh[65], kh[65];
        hex32(all.v[i].m, mh);
        hex32(all.v[i].k, kh);
        fprintf(f, "%s %s\n", mh, kh);
    }
    free(all.v);
    int bad = (e = plat_stdio_sync(f)) != 0;   /* the stream owns fd now: never touched again */
    if (fclose(f) && !bad) { bad = 1; e = errno ? errno : EIO; }
    if (!bad && (e = plat_rename_at(d, tmp, d, path)) != 0) bad = 1;
    if (bad) {
        snprintf(why, n, "the kept set can't be written: %s", strerror(e));
        plat_unlink_at(d, tmp);
    }
    plat_dir_close(d);
    return bad ? -1 : 0;
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
oom:
    free(all.v);
    snprintf(why, n, "out of memory");
    return -1;
#endif
}

/* ---------------------------------------------------------------- room */

typedef struct { unsigned char m[32], k[32]; time_t mtime; long long bytes; } cand;

static int by_use(const void *a, const void *b) {
    time_t x = ((const cand *)a)->mtime, y = ((const cand *)b)->mtime;
    return x < y ? -1 : x > y;
}

long long make_room(kept_cache cache, const kept *k, long long *used, long long target) {
    __block cand *v = NULL;
    __block size_t nv = 0, cv = 0;
    /* <model>/<xx>/<key>.flac, as the cache's layout names them (entries_layout), no link followed */
#if KEPT_CACHE_AT
    plat_dir c = cache;
    if (plat_dir_ok(c)) {
#else
    plat_dir c;
    if (!plat_dir_open(cache, &c)) {
#endif
        plat_dir_each_b(c, ^(const char *mname, const plat_stat *mst) {
            unsigned char m_[32];
            const unsigned char *m = m_;            /* a block can't take an array */
            plat_dir md;
            if (entries_layout(mname, 1, mst->kind == PLAT_DIR, mst->kind == PLAT_FILE, NULL, 0) != ENT_MODEL_DIR
                || unhex64(mname, m_) || plat_dir_open_at(c, mname, &md)) return 0;
            plat_dir_each_b(md, ^(const char *pname, const plat_stat *pst) {
                plat_dir x;
                if (entries_layout(pname, 2, pst->kind == PLAT_DIR, pst->kind == PLAT_FILE, NULL, 0) != ENT_PREFIX_DIR
                    || plat_dir_open_at(md, pname, &x)) return 0;
                plat_dir_each_b(x, ^(const char *ename, const plat_stat *st) {
                    unsigned char key[32];
                    if (entries_layout(ename, 3, st->kind == PLAT_DIR, st->kind == PLAT_FILE, pname, 0) != ENT_FLAC
                        || unhex64(ename, key)) return 0;
                    if (k && (kset_has(&k->others, key) || kset_has(&k->ours, key))) return 0;
                    if (nv == cv) {
                        cand *t = realloc(v, (cv ? cv * 2 : 1024) * sizeof *t);
                        if (!t) return 1;
                        v = t;
                        cv = cv ? cv * 2 : 1024;
                    }
                    char kh[65];
                    memcpy(kh, ename, 64);
                    kh[64] = 0;
                    memcpy(v[nv].m, m, 32);
                    memcpy(v[nv].k, key, 32);
                    v[nv].mtime = (time_t)st->mtime;
                    v[nv].bytes = entries_size_at(x, kh);
                    nv++;
                    return 0;
                });
                plat_dir_close(x);
                return 0;
            });
            plat_dir_close(md);
            return 0;
        });
#if !KEPT_CACHE_AT
        plat_dir_close(c);
#endif
    }
    if (nv) qsort(v, nv, sizeof *v, by_use);
    long long removed = 0;
    for (size_t i = 0; i < nv && *used > target; i++) {
        char mh[65], kh[65];
        plat_dir x;
        hex32(v[i].m, mh);
        hex32(v[i].k, kh);
        if (prefix_open(cache, mh, kh, &x)) continue;
        int gone = 0;
        for (int f = 0; f < ENTRIES_FILES; f++) {       /* the FLAC first: without it there is no entry */
            char name[80];
            entries_file(name, sizeof name, kh, f);
            if (plat_unlink_at(x, name) == 0 && f == 0) gone = 1;
        }
        plat_dir_close(x);
        if (gone) { *used -= v[i].bytes; removed++; }
    }
    free(v);
    return removed;
}
