/* Prepare's cache tests run with real files and a one-chunk model and decoder. */
#include <stdlib.h>
#include <string.h>
#include "entry.h"
#include "library.h"
#include "model.h"
#include "rbsp.h"

int file_sha256(const char *path, char out[65], plat_model_info *info) {
    (void)path;
    if (info) *info = (plat_model_info){0};
    memcpy(out, OURS_V4, 65);
    return 0;
}
int rebuilt_id(const char *fp, char out[65]) { (void)fp; (void)out; abort(); }
int chunk_key(const char *id, const float *mix, size_t n, size_t f, char key[65]) {
    (void)id; (void)mix; (void)n; (void)f;
    memset(key, 'a', 64);
    key[64] = 0;
    return 0;
}

int model_load_ort(const char *path, char *why, size_t n) { (void)path; (void)why; (void)n; return 0; }
struct model { int unused; };
model *model_open(const char *path, const plat_model_info *info, int threads, int ours, char *why, size_t n) {
    (void)path; (void)info; (void)threads; (void)ours; (void)why; (void)n;
    static model m;
    return &m;
}
void model_close(model *m) { (void)m; }
int model_run(model *m, const float *c, size_t n, int ours, float **xo, float **xt, size_t *f, char *why, size_t wn) {
    (void)m; (void)c; (void)n; (void)ours; (void)xo; (void)xt; (void)f; (void)why; (void)wn;
    return 0;
}
int ours_pcm(const float *xt, const float *mix, size_t n, int16_t *pcm, const char **why) {
    (void)xt; (void)mix; (void)n; (void)pcm; (void)why;
    return 0;
}
int rebuilt_pcm(const float *x, size_t f, const float *xt, const float *mix, size_t n, int16_t *pcm, const char **why) {
    (void)x; (void)f; (void)xt; (void)mix; (void)n; (void)pcm; (void)why;
    abort();
}
int store(plat_dir x, const char *key, const int16_t *pcm, const float *mix, size_t n, int rebuilt, int chosen,
          const char *helper, const char **why, int *err) {
    (void)pcm; (void)mix; (void)n; (void)rebuilt; (void)chosen; (void)helper; (void)why;
    char name[80];
    snprintf(name, sizeof name, "%s.flac", key);
    plat_file f;
    if ((*err = plat_file_create_at(x, name, 0, &f))) return -1;
    plat_file_close(f);
    return 0;
}

int load_tracks(const char *path, const char *rb, tracks *ts, char *why, size_t n) {
    (void)path; (void)rb; (void)why; (void)n;
    *ts = (tracks){.count = 1};
    ts->t[0].n = 1;
    return 0;
}
int probe_track(const char *path, double *secs, int *aac, char *why, size_t n) {
    (void)path; (void)secs; (void)aac; (void)why; (void)n;
    abort();
}
void tracks_free(tracks *ts) { (void)ts; }
size_t n_chunks(size_t n) { return n != 0; }
void chunk_at(const track *t, size_t i, float *c) { (void)t; (void)i; (void)c; }
void mean_std(const float *c, size_t count, double *mean, double *std) { (void)c; (void)count; *mean = 0; *std = 1; }

void library_default_path(char *path, size_t n) { if (n) path[0] = 0; }
library *library_open(const char *path, char *why, size_t n) { (void)path; (void)why; (void)n; abort(); }
void library_close(library *l) { (void)l; abort(); }
int library_list(library *l, FILE *out, char *why, size_t n) { (void)l; (void)out; (void)why; (void)n; abort(); }
int library_tracks(library *l, const char *id, char ***paths, size_t *count, char *why, size_t n) {
    (void)l; (void)id; (void)paths; (void)count; (void)why; (void)n;
    abort();
}

uint64_t prepare_cache_test_free_bytes(const char *path, int important, const char **how) {
    (void)path; (void)important; (void)how;
    return 100000000000ULL;
}
