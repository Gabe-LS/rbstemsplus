/* Exercise prepare's CLI and kept files with a fault-injected list stream and a stub model. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "rbsp.h"

static void *test_malloc(size_t size) {
    if (size == sizeof(float) * 2 * CHUNK_L && getenv("RBSP_TEST_CHUNK_OOM")) return NULL;
    return malloc(size);
}

static int list_read(void *cookie, char *buf, int n) {
    const char **line = cookie;
    if (!*line) { errno = EIO; return -1; }
    int len = (int)strlen(*line);
    assert(len <= n);
    memcpy(buf, *line, (size_t)len);
    *line = NULL;
    return len;
}

static FILE *test_fopen(const char *path, const char *mode) {
    if (getenv("RBSP_TEST_BINARY_LIST") && strcmp(mode, "rb")) {
        errno = EINVAL;
        return NULL;
    }
    static const char *row;
    if (strcmp(path, "fault-list")) return fopen(path, mode);
    row = getenv("RBSP_TEST_LIST_ROW");
    assert(row);
    return funopen(&row, list_read, NULL, NULL, NULL);
}

#include "path_rules.h"

static int test_local_file(const char *path) {
    return path[0] == '/' && path[1] != '/' ? paths_local_file(path) : paths_local_file_win(path);
}

static size_t test_other_drive(const char *path) {
    return paths_other_drive(path) || paths_other_drive_win(path, 'C');
}

#define paths_local_file test_local_file
#define paths_other_drive test_other_drive
#define fopen test_fopen
#define malloc test_malloc
static uint64_t test_free_bytes(const char *path, int important, const char **how) { return UINT64_C(100000000000); }
#define plat_free_bytes test_free_bytes
#include "../prepare.c"
#undef plat_free_bytes
#undef fopen
#undef malloc

void library_default_path(char *path, size_t n) { path[0] = 0; }
struct library { int dummy; };
library *library_open(const char *path, char *why, size_t n) {
    static library fixture;
    assert(!strcmp(path, "pf2-fixture"));
    return &fixture;
}
void library_close(library *l) {}
int library_list(library *l, FILE *out, char *why, size_t n) { fputs("{}\n", out); return 0; }
int library_tracks(library *l, const char *id, char ***paths, size_t *count, char *why, size_t n) {
    assert(!strcmp(id, "pf2"));
    *paths = malloc(sizeof **paths);
    assert(*paths);
    (*paths)[0] = strdup("C:\\fixture.wav");
    assert((*paths)[0]);
    *count = 1;
    return 0;
}

struct model { int unused; };
int model_load_ort(const char *lib, char *why, size_t n) { assert(!getenv("RBSP_TEST_NO_MODEL")); return 0; }
model *model_open(const char *path, const plat_model_info *info, int threads, int ours, char *why, size_t n) {
    static model m;
    return &m;
}
void model_close(model *m) {}
int model_run(model *m, const float *mix, size_t L, int ours, float **x, float **xt, size_t *F, char *why, size_t n) {
    if (getenv("RBSP_TEST_BATCH_MODEL_ERROR")) {
        snprintf(why, n, "ONNX Runtime: CPU session creation failed");
        return MODEL_RUN_BATCH_ERROR;
    }
    if (getenv("RBSP_TEST_STOP")) {
        assert(getchar() == EOF);
        on_signal(SIGTERM);             /* exercise the same chunk boundary with the host's stop latch */
    }
    return 0;
}

int file_sha256(const char *path, char out[65], plat_model_info *info) {
    if (info) *info = (plat_model_info){0};
    memcpy(out, OURS_V4, 65);
    return 0;
}
int rebuilt_id(const char *fp, char out[65]) { abort(); }
int chunk_key(const char *model, const float *mix, size_t L, size_t F, char key[65]) {
    snprintf(key, 65, "%064d", (int)mix[0]);
    return 0;
}
void mean_std(const float *mix, size_t count, double *mean, double *std) { *mean = 0; *std = 1; }
int ours_pcm(const float *xt, const float *mix, size_t L, int16_t *pcm, const char **why) { return 0; }
int rebuilt_pcm(const float *x, size_t F, const float *xt, const float *mix, size_t L, int16_t *pcm, const char **why) { abort(); }
int store(plat_dir x, const char *key, const int16_t *pcm, const float *mix, size_t L, int rebuilt, int chosen,
          const char *helper, const char **why, int *err) {
    const char *mode = getenv("RBSP_TEST_STEMS");
    if (mode && (!strcmp(mode, "none") || (!strcmp(mode, "partial") && mix[0] == 2))) return 1;
    /* Minimal entry metadata for prepare's reuse check. */
    char name[80], head[81] = {76};
    snprintf(name, sizeof name, "%s.flac", key);
    snprintf(head + 4, sizeof head - 4, "RBSTEMS_KEY=%s", key);
    plat_file f;
    int e = plat_unlink_at(x, name);
    assert(!e || e == ENOENT);
    assert(!plat_file_create_at(x, name, 0, &f));
    FILE *out = plat_file_stdio(f, "w");
    const char *tag = HELPER_VERSION;
    unsigned char length[4] = {strlen("RBSTEMS_HELPER=") + strlen(tag)};
    assert(out && fwrite(head, 1, sizeof head, out) == sizeof head &&
           fwrite(length, 1, sizeof length, out) == sizeof length && fprintf(out, "RBSTEMS_HELPER=%s", tag) > 0 &&
           !fclose(out));
    return 0;
}

int rb_resample_build_proved;
#ifdef RBSP_TEST_FORMAT_RATES
int rb_aac_test_unproved, rb_mp3_test_unproved, rb_rates_test_unproved;
#endif

int rb_win_rate_refused(int rate, const char *when, char *why, size_t n) {
    if (rb_resample_build_proved) return 0;
    snprintf(why, n, "%d Hz%s: this rekordbox version's resampler isn't proved yet, so RB Stems Plus doesn't prepare files it resamples", rate, when);
    return 1;
}

static int source_rate(void) {
    return getenv("RBSP_TEST_TRACK_48K") ? 48000 : 44100;
}

const char *aac_mf_decoder_version(void) {
    const char *version = getenv("RBSP_TEST_AAC_DECODER");
    return version ? version : "";
}

static int test_aac(const char *path) {
    const char *ext = strrchr(path, '.');
    return ext && !strcmp(ext, ".mp4");
}

int probe_track(const char *path, double *seconds, int *aac, char *why, size_t n) {
    const char *refuse = getenv("RBSP_TEST_PROBE_REFUSE");   /* e.g. a resampled rate without --resample-proved */
    if (refuse) { snprintf(why, n, "%s", refuse); return 1; }
    int rate = source_rate();
    if (rate != 44100 && rb_win_rate_refused(rate, "", why, n)) return 1;
    if (getenv("RBSP_TEST_AAC_ESTIMATE")) {
        *seconds = (double)(24 * CHUNK_HOP - CHUNK_PRE + 1) / 44100;
        *aac = 1;
        return 0;
    }
    if (getenv("RBSP_TEST_AAC_LENGTH")) {
        *seconds = (double)(CHUNK_HOP - CHUNK_PRE + 1) / 44100;
        *aac = 1;
        return 0;
    }
    *seconds = (double)CHUNK_HOP / 44100;
    *aac = test_aac(path);
    return 0;
}
int load_tracks(const char *path, const char *app, tracks *ts, char *why, size_t n) {
    if (getenv("RBSP_TEST_STEMS")) {
        int rate = source_rate();
        if (rate != 44100 && rb_win_rate_refused(rate, "", why, n)) return 1;
        *ts = (tracks){.count = 1, .t = {{.n = CHUNK_HOP}}, .source_rate = rate};
        FILE *f = fopen(path, "rb");
        if (!f) return 1;
        int mark = fgetc(f);
        fclose(f);
        if (mark != EOF) ts->t[0].owner = mark;
        const char *length = getenv("RBSP_TEST_AAC_LENGTH");
        ts->aac = length || test_aac(path);
        if (ts->aac) ts->aac_length = AAC_LENGTH_COMPLETE;
        if (length) {
            int short_aac = !strcmp(length, "short");
            ts->t[0].n = CHUNK_HOP - CHUNK_PRE + !short_aac;
            ts->aac_length = short_aac ? AAC_LENGTH_SHORT : AAC_LENGTH_COMPLETE;
            if (short_aac) snprintf(ts->how, sizeof ts->how, "incomplete AAC decode");
        }
        if (ts->aac && getenv("RBSP_TEST_AAC_NEW_PCM")) ts->t[0].owner += 10;
        return 0;
    }
    snprintf(why, n, "no audio in the test track");
    return 1;
}
void tracks_free(tracks *ts) {}
size_t n_chunks(size_t n) { return (n + CHUNK_PRE + CHUNK_HOP - 1) / CHUNK_HOP; }
void chunk_at(const track *t, size_t i, float *c) { c[0] = (float)(i + 1 + t->owner * 10); }
