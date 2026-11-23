/* model.c's Windows logic on the Mac (helper.md T12, §5 "Threads"): the automatic thread count's
 * clamp, the DirectML/CPU choice, the timing lines' Windows fields, and the pocketfft spectrogram
 * (spec_zero_pf, Windows') held to the vDSP one (spec_zero, the Mac's). model.c is compiled in
 * whole with RBSP_TEST_WIN_MODEL (MODEL_C: another copy, for the mutation checks of
 * model-win-host.sh). */
#ifndef MODEL_C
#define MODEL_C "../model.c"
#endif
#include MODEL_C

#include <assert.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include "entry.h"

static int passed, failed;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (ok) passed++;
    else failed++;
}

static void threads(void) {
    /* helper.md §5: a zero count, odd counts, above 128 */
    static const struct { long ncpu; int want; } t[] = {
        {0, 1}, {-1, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 2}, {7, 3}, {8, 4}, {63, 31}, {127, 63},
        {128, 64}, {129, 64}, {130, 64}, {255, 64}, {256, 64}, {4096, 64},
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (model_default_threads(t[i].ncpu) != t[i].want) {
            printf("     model_default_threads(%ld) = %d, want %d\n", t[i].ncpu, model_default_threads(t[i].ncpu), t[i].want);
            ok = 0;
        }
    check(ok, "the automatic thread count: half the processors, 1-64, a zero count 1");
}

static void ep(void) {
    int ok = model_ep_choice(1, 1, 1) == MODEL_EP_DML;
    for (int m = 0; m < 7; m++) ok &= model_ep_choice(m & 1, m >> 1 & 1, m >> 2 & 1) == MODEL_EP_CPU;
    check(ok, "DirectML only when asked, DirectML.dll loaded and ORT's ordinal 4 found; else the CPU");
    check(model_retry_on_cpu(MODEL_EP_DML) == 1 && model_retry_on_cpu(MODEL_EP_CPU) == 0,
          "a failed DirectML run runs again on the CPU; a failed CPU run stands");
}

static void fields(void) {
    char b[64];
    int ok = 1;
    static const struct { int ac; model_ep ep; const char *want; } t[] = {
        {1, MODEL_EP_CPU, " power=ac ep=cpu"}, {0, MODEL_EP_CPU, " power=battery ep=cpu"},
        {255, MODEL_EP_DML, " power=unknown ep=dml"}, {2, MODEL_EP_CPU, " power=unknown ep=cpu"},
        {1, MODEL_EP_DML, " power=ac ep=dml"},
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        int k = model_timing_fields(b, sizeof b, t[i].ac, t[i].ep);
        if (strcmp(b, t[i].want) || k != (int)strlen(t[i].want)) { printf("     %d: \"%s\" (%d), want \"%s\"\n", t[i].ac, b, k, t[i].want); ok = 0; }
    }
    check(ok, "the timing lines' fields: power=ac|battery|unknown from ACLineStatus, ep=cpu|dml");
    char small[8] = "xxxxxxx";
    check(model_timing_fields(small, sizeof small, 1, MODEL_EP_CPU) == 0 && small[0] == 0, "a timing field that doesn't fit leaves \"\"");
}

static void spectrogram(void) {
    /* a chunk of noise with a tone and a silent stretch, rekordbox's length */
    size_t L = CHUNK_L, F = (L + HL - 1) / HL;
    float *mix = malloc(sizeof(float) * 2 * L), *a = calloc(4 * BINS * F, sizeof(float)), *b = calloc(4 * BINS * F, sizeof(float));
    if (!mix || !a || !b) { check(0, "out of memory"); return; }
    uint32_t s = 12345;
    for (size_t i = 0; i < 2 * L; i++) {
        s = s * 1664525u + 1013904223u;
        double noise = ((double)(s >> 8) / 16777216.0 - 0.5) * 0.4;
        mix[i] = (float)(noise + 0.5 * sin(2 * M_PI * 440.0 * (double)(i % L) / 44100.0) * (i >= L ? 0.7 : 1.0));
        if (i % L > 200000 && i % L < 260000) mix[i] = 0;
    }
    int ra = spec_zero(mix, L, F, a), rb = spec_zero_pf(mix, L, F, b);
    double peak = 0, worst = 0;
    for (size_t i = 0; i < 4 * BINS * F; i++) {
        if (fabs(a[i]) > peak) peak = fabs(a[i]);
        if (fabs((double)a[i] - b[i]) > worst) worst = fabs((double)a[i] - b[i]);
    }
    printf("     spectrogram: peak %.6g, largest difference %.3g\n", peak, worst);
    /* both in double, rounded to float32: within a few float steps of the peak */
    check(ra == 0 && rb == 0 && peak > 1 && worst <= peak * 1e-6, "spec_zero_pf (pocketfft) gives spec_zero's (vDSP's) spectrogram");
    free(mix);
    free(a);
    free(b);
}

static OrtStatus *stub_options(OrtSessionOptions **out) {
    *out = (OrtSessionOptions *)1;
    return NULL;
}

static OrtStatus *stub_threads(OrtSessionOptions *o, int threads) {
    (void)o; (void)threads;
    return NULL;
}

static void stub_release_options(OrtSessionOptions *o) { (void)o; }
static int sessions_released;
static void stub_release_session(OrtSession *s) { (void)s; sessions_released++; }
static void stub_release_value(OrtValue *v) { (void)v; }
static const char *stub_error(const OrtStatus *s) { return (const char *)s; }
static void stub_release_status(OrtStatus *s) { (void)s; }

static OrtStatus *stub_memory(OrtAllocatorType a, OrtMemType t, OrtMemoryInfo **out) {
    (void)a; (void)t; (void)out;
    return NULL;
}

static OrtStatus *stub_tensor(const OrtMemoryInfo *mem, void *data, size_t bytes,
                              const int64_t *dims, size_t ndims, ONNXTensorElementDataType type, OrtValue **out) {
    (void)mem; (void)data; (void)bytes; (void)dims; (void)ndims; (void)type;
    *out = (OrtValue *)1;
    return NULL;
}

enum change { UNCHANGED, REPLACED, TOUCHED, RESIZED };
static const char *fixture, *replacement;
static enum change change_on_create;
static int replace_on_run, session_fails, runs_seen, sessions_seen;

static void set_mtime(const char *path, long ns) {
    const struct timespec times[] = {{0, UTIME_OMIT}, {1700000000, ns}};
    assert(utimensat(AT_FDCWD, path, times, 0) == 0);
}

static void write_fixture(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    assert(f && fputs(contents, f) >= 0 && fclose(f) == 0);
    set_mtime(path, 100);
}

static plat_model_info hash_fixture(void) {
    write_fixture(fixture, "original");
    write_fixture(replacement, "switched");  /* same size and mtime, different file ID */
    char fp[65];
    plat_model_info info;
    assert(file_sha256(fixture, fp, &info) == 0);
    sessions_seen = sessions_released = runs_seen = 0;
    return info;
}

static OrtStatus *stub_run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names,
                           const OrtValue *const *in, size_t n_in, const char *const *out_names,
                           size_t n_out, OrtValue **out) {
    (void)ro; (void)in_names; (void)in; (void)n_in; (void)out_names; (void)n_out; (void)out;
    runs_seen++;
    if (replace_on_run) assert(rename(replacement, fixture) == 0);
    return (OrtStatus *)(s ? "inference failed" : "a null session reached ORT");
}

static OrtStatus *stub_session(const OrtEnv *e, const ORTCHAR_T *path,
                               const OrtSessionOptions *o, OrtSession **out) {
    (void)e; (void)o;
    sessions_seen++;
    switch (change_on_create) {
    case REPLACED: assert(rename(replacement, path) == 0); break;
    case TOUCHED:  set_mtime(path, 200); break;
    case RESIZED:  assert(truncate(path, 1) == 0); set_mtime(path, 100); break;
    case UNCHANGED: break;
    }
    if (session_fails) return (OrtStatus *)"CPU session creation failed";
    *out = (OrtSession *)1;
    return NULL;
}

static const OrtApi stub = {
    .CreateSessionOptions = stub_options, .SetIntraOpNumThreads = stub_threads,
    .SetInterOpNumThreads = stub_threads, .ReleaseSessionOptions = stub_release_options,
    .CreateSession = stub_session, .ReleaseSession = stub_release_session, .CreateCpuMemoryInfo = stub_memory,
    .CreateTensorWithDataAsOrtValue = stub_tensor, .Run = stub_run, .ReleaseValue = stub_release_value,
    .GetErrorMessage = stub_error, .ReleaseStatus = stub_release_status,
};

static int model_change_error(const char *why) {
    return !strcmp(why, "the model file changed or became unavailable after hashing");
}

static void open_changed_model(enum change change, const char *label) {
    plat_model_info info = hash_fixture();
    change_on_create = change;
    char why[100] = "";
    model *m = model_open(fixture, &info, 1, 0, why, sizeof why);
    check(!m && model_change_error(why) && sessions_seen == 1 && sessions_released == 1, label);
    model_close(m);
    change_on_create = UNCHANGED;
}

static int run_fixture(model *m, char *why, size_t n) {
    const float mix[2] = {0};
    float *x = NULL, *xt = NULL;
    size_t F = 0;
    return model_run(m, mix, 1, 1, &x, &xt, &F, why, n);
}

static void fallback_failure(char *path) {
    model m = {.s = (OrtSession *)1, .w = path, .path = path, .info = hash_fixture(), .threads = 1};
    char why[100];
    ep_used = MODEL_EP_DML;
    session_fails = 1;
    check(run_fixture(&m, why, sizeof why) == MODEL_RUN_BATCH_ERROR &&
          !strcmp(why, "ONNX Runtime: CPU session creation failed") && !m.s &&
          sessions_seen == 1 && runs_seen == 1, "failed CPU session creation is a batch error");
    check(run_fixture(&m, why, sizeof why) == MODEL_RUN_BATCH_ERROR &&
          !strcmp(why, "no ONNX Runtime session") && runs_seen == 1, "missing session never reaches ORT");
    session_fails = 0;
}

static void fallback_changed_model(char *path, int before_creation, const char *label) {
    model m = {.s = (OrtSession *)1, .w = path, .path = path, .info = hash_fixture(), .threads = 1};
    char why[100];
    ep_used = MODEL_EP_DML;
    replace_on_run = before_creation;
    change_on_create = before_creation ? UNCHANGED : REPLACED;
    check(run_fixture(&m, why, sizeof why) == MODEL_RUN_BATCH_ERROR && model_change_error(why) &&
          !m.s && runs_seen == 1 && sessions_seen == !before_creation &&
          sessions_released == 1 + !before_creation, label);
    replace_on_run = 0;
    change_on_create = UNCHANGED;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    fixture = argv[1];
    replacement = argv[2];
    R = &stub;
    threads();
    ep();
    fields();
    spectrogram();
    fallback_failure(argv[1]);
    open_changed_model(REPLACED, "opening rejects a replacement with the same size and mtime");
    open_changed_model(TOUCHED, "opening detects a subsecond mtime change");
    open_changed_model(RESIZED, "opening detects a size change with the same mtime and file ID");
    fallback_changed_model(argv[1], 1, "a model replaced before CPU fallback never reaches CreateSession");
    fallback_changed_model(argv[1], 0, "a model replaced during CPU fallback never reaches inference");
    printf("model-win-host: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
