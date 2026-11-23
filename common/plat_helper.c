/* plat_helper.h's calls that are the same code on both OSes (the Mac's temporary folder and file
 * too, so a Mac test runs what the Windows helper runs on top of them). */
#include "plat_helper.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- the C library's gaps */

COMMON_API size_t plat_strlcat(char *dst, const char *src, size_t n) {
    size_t d = strnlen(dst, n), s = strlen(src);
    if (d == n) return n + s;                               /* dst wasn't terminated within n */
    size_t k = s < n - d - 1 ? s : n - d - 1;
    memcpy(dst + d, src, k);
    dst[d + k] = 0;
    return d + s;
}

COMMON_API long plat_getline(char **line, size_t *cap, FILE *f) {
    size_t n = 0;
    int c;
    if (!*line || !*cap) {
        char *b = realloc(*line, 128);
        if (!b) return -1;
        *line = b;
        *cap = 128;
    }
    while ((c = getc(f)) != EOF) {
        if (n + 2 > *cap) {
            if (*cap > ((size_t)LONG_MAX) / 2) return -1;
            char *b = realloc(*line, *cap * 2);
            if (!b) return -1;
            *line = b;
            *cap *= 2;
        }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    (*line)[n] = 0;
    if (!n || ferror(f)) return -1;                         /* the end, or a read error */
    return (long)n;
}

/* ---------------------------------------------------------------- the walk */

#if defined(_WIN32)
#define PH_SEP '\\'
#else
#define PH_SEP '/'
#endif
#define PH_DEPTH 64

typedef struct {
    plat_walk_fn fn;
    void *ctx;
    plat_dir d;
    char *rel;                  /* the path below the top folder, PH_REL_CAP bytes */
    size_t len;
    int depth, bad, stop;
} ph_walk;
#define PH_REL_CAP 32768

static int ph_each(void *c, const char *name, const plat_stat *st) {
    ph_walk *w = c;
    size_t nl = strlen(name), at = w->len;
    if (at + (at ? 1 : 0) + nl + 1 > PH_REL_CAP) { w->bad = 1; return 0; }
    if (at) w->rel[at++] = PH_SEP;
    memcpy(w->rel + at, name, nl + 1);
    if (st->kind == PLAT_FILE) {
        if (w->fn(w->ctx, w->rel, st)) { w->stop = 1; w->rel[w->len] = 0; return 1; }
    } else if (st->kind == PLAT_DIR) {
        plat_dir sub;
        if (w->depth >= PH_DEPTH || plat_dir_open_at(w->d, name, &sub)) w->bad = 1;
        else {
            ph_walk in = *w;
            in.d = sub;
            in.len = at + nl;
            in.depth = w->depth + 1;
            if (plat_dir_each(sub, ph_each, &in) && !in.stop) in.bad = 1;
            plat_dir_close(sub);
            w->bad |= in.bad;
            w->stop = in.stop;
        }
    }
    w->rel[w->len] = 0;
    return w->stop;
}

#if defined(_WIN32) || defined(RBSP_TEST_PREPARE_CACHE_AT)
COMMON_API int plat_walk_at(plat_dir top, plat_walk_fn fn, void *ctx) {
    if (!plat_dir_ok(top)) return -1;
#else
COMMON_API int plat_walk(const char *path, plat_walk_fn fn, void *ctx) {
    plat_dir top;
#endif
    char *rel = malloc(PH_REL_CAP);
    if (!rel) return -1;
    rel[0] = 0;
#if !defined(_WIN32) && !defined(RBSP_TEST_PREPARE_CACHE_AT)
    if (plat_dir_open(path, &top)) { free(rel); return -1; }
#endif
    ph_walk w = {fn, ctx, top, rel, 0, 0, 0, 0};
    if (plat_dir_each(top, ph_each, &w) && !w.stop) w.bad = 1;
#if !defined(_WIN32) && !defined(RBSP_TEST_PREPARE_CACHE_AT)
    plat_dir_close(top);
#endif
    free(rel);
    return w.bad || w.stop ? -1 : 0;
}

#if defined(_WIN32) || defined(RBSP_TEST_PREPARE_CACHE_AT)
COMMON_API int plat_walk(const char *path, plat_walk_fn fn, void *ctx) {
    plat_dir top;
    if (plat_dir_open(path, &top)) return -1;
    int r = plat_walk_at(top, fn, ctx);
    plat_dir_close(top);
    return r;
}
#endif

/* ---------------------------------------------------------------- the Mac's model and track files */

#if defined(__APPLE__)
static int ph_model_stat(const struct stat *s, plat_model_info *info) {
    if (!S_ISREG(s->st_mode)) return EINVAL;
    *info = (plat_model_info){
        .id = {.vol = (uint64_t)s->st_dev, .file = {(uint64_t)s->st_ino, 0}},
        .size = s->st_size, .mtime = s->st_mtimespec.tv_sec, .mtime_nsec = s->st_mtimespec.tv_nsec,
    };
    return 0;
}

COMMON_API int plat_model_stat_file(plat_file f, plat_model_info *info) {
    struct stat s;
    if (fstat(f.fd, &s)) return errno ? errno : EIO;
    return ph_model_stat(&s, info);
}

COMMON_API int plat_model_stat_path(const char *path, plat_model_info *info) {
    struct stat s;
    if (stat(path, &s)) return errno ? errno : EIO;
    return ph_model_stat(&s, info);
}

COMMON_API int plat_stdio_sync(FILE *s) {
    if (fflush(s)) return errno ? errno : EIO;
    int fd = fileno(s);
    if (fd < 0) return EBADF;
    return fsync(fd) == 0 ? 0 : errno ? errno : EIO;       /* 1.3.0's fsync on the same descriptor */
}

COMMON_API int plat_finfo_path(const char *path, plat_finfo *fi) {
    struct stat s;
    if (stat(path, &s) != 0) return errno ? errno : EIO;   /* readable.c's stat, its errno unchanged */
    fi->kind = S_ISREG(s.st_mode) ? PLAT_FILE : S_ISDIR(s.st_mode) ? PLAT_DIR : PLAT_OTHER;
    fi->size = (int64_t)s.st_size;
    fi->mtime_ns = (int64_t)s.st_mtimespec.tv_sec * 1000000000LL + s.st_mtimespec.tv_nsec;   /* manifest.c's mtime_ns */
    return 0;
}

/* ---------------------------------------------------------------- the Mac's temporary files */

COMMON_API int plat_temp_dir(char *out, size_t n) {
    /* big.c's: this account's alone */
    size_t k = confstr(_CS_DARWIN_USER_TEMP_DIR, out, n);
    if (!k || k > n) { if (n) out[0] = 0; return -1; }
    return 0;
}

COMMON_API int plat_temp_file(plat_file *out) {
    char dir[PATH_MAX], p[PATH_MAX + 32];
    out->fd = -1;
    if (plat_temp_dir(dir, sizeof dir)) return ENOENT;
    snprintf(p, sizeof p, "%s/rbsp-pcm.XXXXXX", dir);
    int fd = mkstemp(p);
    if (fd < 0) return errno;
    unlink(p);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    out->fd = fd;
    return 0;
}
#endif
