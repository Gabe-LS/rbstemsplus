/* manifest.c: kept sets' manifests (manifest.h) */
#include "manifest.h"
#include "path_rules.h"
#include <errno.h>
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
#include <limits.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define getline plat_getline                    /* mingw-w64 has none; the Mac keeps libSystem's */
#endif

static const char HEAD[] = "rbstemsplus-manifest 1";

static int hex64(const char *s) {
    if (strlen(s) != 64) return 0;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}

void manifest_free(manifest *m) {
    for (size_t i = 0; i < m->n; i++) { free(m->f[i].path); free(m->f[i].keys); }
    free(m->f);
    memset(m, 0, sizeof *m);
}

static mfile *add(manifest *m, const char *path) {
    if (strchr(path, '\n')) return NULL;
    if (m->n == m->cap) {
        mfile *t = realloc(m->f, (m->cap ? m->cap * 2 : 64) * sizeof *t);
        if (!t) return NULL;
        m->f = t;
        m->cap = m->cap ? m->cap * 2 : 64;
    }
    mfile *f = &m->f[m->n];
    memset(f, 0, sizeof *f);
    if (!(f->path = strdup(path))) return NULL;
    m->n++;
    return f;
}

mfile *manifest_file(manifest *m, const char *path, const plat_finfo *fi) {
    mfile *f = add(m, path);
    if (f) { f->size = (long long)fi->size; f->mtime_ns = (long long)fi->mtime_ns; }
    return f;
}

void mfile_key(mfile *f, const char *key) {
    if (!f) return;
    for (size_t i = 0; i < f->nkeys; i++)          /* AAC's lengths share most entries */
        if (!memcmp(f->keys[i], key, 64)) return;
    if (f->nkeys == f->cap) {
        char (*t)[65] = realloc(f->keys, (f->cap ? f->cap * 2 : 32) * sizeof *t);
        if (!t) return;
        f->keys = t;
        f->cap = f->cap ? f->cap * 2 : 32;
    }
    memcpy(f->keys[f->nkeys++], key, 65);
}

void manifest_drop_last(manifest *m) {
    if (!m->n) return;
    m->n--;
    free(m->f[m->n].path);
    free(m->f[m->n].keys);
}

void manifest_keep_only(manifest *m, char **paths, size_t n) {
    size_t to = 0;
    for (size_t i = 0; i < m->n; i++) {
        size_t j = 0;
        while (j < n && !paths_same(paths[j], m->f[i].path)) j++;
        if (j < n) m->f[to++] = m->f[i];
        else { free(m->f[i].path); free(m->f[i].keys); }
    }
    m->n = to;
}

const mfile *manifest_by_path(const manifest *m, const char *path) {
    const mfile *f = NULL;
    for (size_t i = 0; i < m->n; i++) if (paths_same(m->f[i].path, path)) f = &m->f[i];
    return f;
}

const mfile *manifest_find(const manifest *m, const char *path, const plat_finfo *fi) {
    for (size_t i = 0; i < m->n; i++)
        if (paths_same(m->f[i].path, path) && m->f[i].size == (long long)fi->size && m->f[i].mtime_ns == (long long)fi->mtime_ns) return &m->f[i];
    return NULL;
}

int manifest_read(const char *dir, const char *name, manifest *m) {
    memset(m, 0, sizeof *m);
    char p[300];                                /* a name in the folder: at most 255 bytes */
    plat_dir d;
    plat_file fd;
    if (snprintf(p, sizeof p, "%s.manifest", name) >= (int)sizeof p || plat_dir_open(dir, &d)) return -1;
    /* a regular file, no link followed, never blocking on a FIFO */
    int e = plat_file_open_at(d, p, INT64_MAX, NULL, &fd);
    plat_dir_close(d);
    FILE *in = e ? NULL : plat_file_stdio(fd, "r");
    if (!in) { plat_file_close(fd); return -1; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t r;
    int n = 0, bad = 0;
    mfile *f = NULL;
    size_t want = 0;
    while (!bad && (r = getline(&line, &cap, in)) > 0) {
        if (line[r - 1] != '\n') { bad = 1; break; }
        line[--r] = 0;
        n++;
        if (n == 1) bad = strcmp(line, HEAD) != 0;
        else if (n == 2) bad = sscanf(line, "helper %15s", m->helper) != 1;
        else if (n == 3) {
            if (!(bad = strncmp(line, "model ", 6) || !hex64(line + 6))) memcpy(m->model, line + 6, 65);
        } else if (n == 4) {
            if (!(bad = strncmp(line, "playlist ", 9) || strlen(line + 9) > 64)) strcpy(m->playlist, line + 9);
        }
        else if (n == 5) bad = sscanf(line, "rate %lld %lf %d", &m->runs, &m->run_s, &m->threads) != 3;
        else if (want) {
            if (!hex64(line)) { bad = 1; break; }
            mfile_key(f, line);
            want--;
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
        } else if (!strncmp(line, "decoder ", 8)) {
            const char *version = line + 8;
            size_t len = strlen(version);
            if (!f || f->decoder[0] || !len || len >= sizeof f->decoder ||
                strspn(version, "0123456789.") != len) { bad = 1; break; }
            memcpy(f->decoder, version, len + 1);
#endif
        } else {
            long long size, t;
            int complete, off = 0;
            size_t k;
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
            if (sscanf(line, "file %lld %lld %d %zu %n", &size, &t, &complete, &k, &off) != 4 || !off) { bad = 1; break; }
            const char *path = line + off;
            int source_rate = 0;
            if (!strncmp(path, "hz=", 3)) {
                char *end;
                long hz = strtol(path + 3, &end, 10);
                if (hz <= 0 || hz > INT_MAX || *end != ' ') { bad = 1; break; }
                source_rate = (int)hz;
                path = end + 1;
            }
            if (!*path || !(f = add(m, path))) { bad = 1; break; }
#else
            if (sscanf(line, "file %lld %lld %d %zu %n", &size, &t, &complete, &k, &off) != 4 || !off || !line[off]
                || !(f = add(m, line + off))) { bad = 1; break; }
#endif
            f->size = size;
            f->mtime_ns = t;
            f->complete = complete == 1;
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
            if (complete == MFILE_COMPLETE_AAC) f->complete = MFILE_COMPLETE_AAC;
            f->source_rate = source_rate;
#endif
            want = k;
        }
    }
    free(line);
    fclose(in);
    if (bad || n < 5 || want) { manifest_free(m); return -1; }
    if (!strcmp(m->playlist, "-")) m->playlist[0] = 0;
    return 0;
}

static void put(FILE *o, const mfile *f) {
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
    if (f->source_rate > 0)
        fprintf(o, "file %lld %lld %d %zu hz=%d %s\n", f->size, f->mtime_ns, f->complete, f->nkeys, f->source_rate, f->path);
    else
#endif
    fprintf(o, "file %lld %lld %d %zu %s\n", f->size, f->mtime_ns, f->complete, f->nkeys, f->path);
    for (size_t k = 0; k < f->nkeys; k++) fprintf(o, "%s\n", f->keys[k]);
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
    if (f->decoder[0]) fprintf(o, "decoder %s\n", f->decoder);
#endif
}

int manifest_write(const char *dir, const char *name, const manifest *m, const manifest *earlier, char *why, size_t n) {
    char tmp[320], path[300];
    plat_dir d;
    plat_file fd = {0};
    int e = 0;
    FILE *o = NULL;
    if (snprintf(tmp, sizeof tmp, ".%s.manifest.%d.tmp", name, plat_pid()) >= (int)sizeof tmp
        || snprintf(path, sizeof path, "%s.manifest", name) >= (int)sizeof path) e = ENAMETOOLONG;
    else if (!(e = plat_dir_open(dir, &d))) {
        plat_unlink_at(d, tmp);                 /* an earlier process's with this pid */
        if (!(e = plat_file_create_at(d, tmp, 0, &fd)) && !(o = plat_file_stdio(fd, "w"))) {
            e = errno ? errno : EIO;
            plat_file_close(fd);
            plat_unlink_at(d, tmp);
        }
        if (!o) plat_dir_close(d);
    }
    if (!o) {
        snprintf(why, n, "the set's manifest can't be written: %s", strerror(e));
        return -1;
    }
    fprintf(o, "%s\nhelper %s\nmodel %s\nplaylist %s\nrate %lld %.3f %d\n", HEAD, m->helper, m->model,
            m->playlist[0] ? m->playlist : "-", m->runs, m->run_s, m->threads);
    for (size_t i = 0; i < m->n; i++) put(o, &m->f[i]);
    for (size_t i = 0; earlier && i < earlier->n; i++) {
        int seen = 0;
        for (size_t j = 0; j < m->n && !seen; j++) seen = paths_same(m->f[j].path, earlier->f[i].path);
        if (!seen) put(o, &earlier->f[i]);
    }
    int bad = (e = plat_stdio_sync(o)) != 0;   /* the stream owns fd now: never touched again */
    if (fclose(o) && !bad) { bad = 1; e = errno ? errno : EIO; }
    if (!bad && (e = plat_rename_at(d, tmp, d, path)) != 0) bad = 1;
    if (bad) {
        snprintf(why, n, "the set's manifest can't be written: %s", strerror(e));
        plat_unlink_at(d, tmp);
    }
    plat_dir_close(d);
    return bad ? -1 : 0;
}
