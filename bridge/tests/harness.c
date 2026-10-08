/* bridge/tests harness: linked against the bridge by @rpath, as rekordbox is, and run by run.sh
 * with HOME pointed at a scratch folder. Prints "ok ..." or "FAIL ..." lines; exits 1 on a FAIL.
 *
 *   harness load REAL           OrtGetApiBase is the bridge's: 1.18.x, Run/CreateSession/
 *                               ReleaseSession hooked, everything else REAL's; the CPU provider works
 *   harness passthrough FAKE    a library that isn't 1.18.x is handed back untouched
 *   harness missing             no real library: NULL, and no crash
 *   harness run MODEL WHAT...   runs chunks through a session on MODEL and says, for each, whether
 *                               the answer is the model's own ("model") or a cached one ("cache")
 *                               WHAT: a (the base chunk), b (a shifted 300 samples later),
 *                               c (a with a loud tone burst), d (unrelated audio), wait (until the
 *                               background writer has stored every miss so far), sleep (1 s, for
 *                               the writer to finish when nothing is expected to be stored),
 *                               free=GB (the free space the test bridge sees from now on)
 *   harness prun MODEL WHAT...  the same with a model shaped like rekordbox's own (make_model.py
 *                               --pioneer): mag [1, 4, 2048, F] with bin 0 of the real parts at 1,
 *                               x non-zero. Each answer is "model" (exactly the model's), "cache" (x
 *                               zero, xt the stems rebuilt as rekordbox would, within the cache's
 *                               16-bit rounding; for b, a near hit, only x zero is checked) or
 *                               "wrong". More WHAT: w=W0,W1,W2,W3 (the model's weights, needed to
 *                               check answers), frames=N (F from now on; default ceil(L / 1024)),
 *                               gain=G (each chunk from now on scaled by G; answers then unchecked)
 */
#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "onnxruntime_c_api.h"

/* the bridge's two other exports (declared in ONNX Runtime's provider headers) */
OrtStatus *OrtSessionOptionsAppendExecutionProvider_CPU(OrtSessionOptions *o, int use_arena);
OrtStatus *OrtSessionOptionsAppendExecutionProvider_CoreML(OrtSessionOptions *o, uint32_t flags);

static int failed;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed = 1;
}

/* the file an address belongs to */
static const char *owner(const void *p) {
    Dl_info i;
    return p && dladdr(p, &i) ? i.dli_fname : "?";
}

static int same_file(const char *a, const char *b) {
    char ra[1024], rb[1024];
    return realpath(a, ra) && realpath(b, rb) && !strcmp(ra, rb);
}

/* ---------------------------------------------------------------- load, passthrough, missing */

static int load(const char *real) {
    const OrtApiBase *b = OrtGetApiBase();
    check(b != NULL, "OrtGetApiBase returns a base");
    if (!b) return 1;
    const char *v = b->GetVersionString();
    check(!strncmp(v, "1.18.", 5), "the version is the real library's 1.18.x");
    check(same_file(owner((const void *)b), owner((const void *)OrtGetApiBase)), "the base is the bridge's");
    const OrtApi *a = b->GetApi(ORT_API_VERSION);
    check(a != NULL, "GetApi(ORT_API_VERSION) returns an OrtApi");
    if (!a) return 1;
    const char *bridge = owner((const void *)OrtGetApiBase);
    check(same_file(owner((const void *)a->Run), bridge), "Run is hooked");
    check(same_file(owner((const void *)a->CreateSession), bridge), "CreateSession is hooked");
    check(same_file(owner((const void *)a->ReleaseSession), bridge), "ReleaseSession is hooked");
    check(same_file(owner((const void *)a->CreateEnv), real), "CreateEnv is the real library's");
    check(same_file(owner((const void *)a->CreateTensorAsOrtValue), real), "CreateTensorAsOrtValue is the real library's");
    check(b->GetApi(ORT_API_VERSION) == a, "GetApi answers the same table twice");
    OrtEnv *env = NULL;
    OrtSessionOptions *so = NULL;
    OrtStatus *st = a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "rbstems-tests", &env);
    check(!st && env, "CreateEnv works");
    st = a->CreateSessionOptions(&so);
    check(!st && so, "CreateSessionOptions works");
    if (so) {
        st = OrtSessionOptionsAppendExecutionProvider_CPU(so, 1);
        check(!st, "the CPU provider passes through to the real library");
        if (st) a->ReleaseStatus(st);
        a->ReleaseSessionOptions(so);
    }
    if (env) a->ReleaseEnv(env);
    return failed;
}

static int passthrough(const char *fake) {
    const OrtApiBase *b = OrtGetApiBase();
    check(b != NULL, "OrtGetApiBase returns a base");
    if (!b) return 1;
    check(same_file(owner((const void *)b), fake), "the base is the fake library's own");
    check(!strcmp(b->GetVersionString(), "1.17.0"), "the version is the fake's 1.17.0");
    const OrtApi *a = b->GetApi(ORT_API_VERSION);
    check(a && same_file(owner((const void *)a), fake), "GetApi is the fake's, untouched");
    OrtStatus *s = OrtSessionOptionsAppendExecutionProvider_CPU(NULL, 1);
    check(s && same_file(owner((const void *)s), fake), "the CPU provider passes through");
    s = OrtSessionOptionsAppendExecutionProvider_CoreML(NULL, 0);
    check(s && same_file(owner((const void *)s), fake), "the CoreML provider passes through");
    return failed;
}

static int missing(void) {
    check(OrtGetApiBase() == NULL, "OrtGetApiBase returns NULL without a real library");
    check(OrtSessionOptionsAppendExecutionProvider_CPU(NULL, 1) == NULL, "the CPU provider returns without crashing");
    check(OrtSessionOptionsAppendExecutionProvider_CoreML(NULL, 0) == NULL, "the CoreML provider returns without crashing");
    return failed;
}

/* ---------------------------------------------------------------- run */

#define K 2.475f                          /* the test model's factor (make_model.py) */
#define L 88200                                 /* 2 s: 20 summary frames */
#define SRC (L + 1000)
#ifndef BURST
#define BURST 0.05
#endif

/* deterministic stereo "music": filtered noise and a tone under a slowly moving envelope */
static void source(unsigned seed, float *l, float *r) {
    uint32_t s = seed * 2654435761u + 1;
    double lp = 0, lp2 = 0;
    for (int i = 0; i < SRC; i++) {
        s = s * 1664525u + 1013904223u;
        double n1 = (s >> 8) / 8388608.0 - 1.0;
        s = s * 1664525u + 1013904223u;
        double n2 = (s >> 8) / 8388608.0 - 1.0;
        lp += 0.2 * (n1 - lp);
        lp2 += 0.2 * (n2 - lp2);
        double t = i / 44100.0;
        double env = 0.6 + 0.4 * sin(2 * M_PI * (0.7 + 0.1 * seed) * t);
        double tone = 0.3 * sin(2 * M_PI * (220.0 + 20 * seed) * t);
        l[i] = (float)(0.3 * env * (lp + tone));
        r[i] = (float)(0.3 * env * (0.8 * lp + 0.3 * lp2 + tone));
    }
}

/* the chunk "what" as planar [2][L]. The source is shifted and scaled so that its first L samples
 * have mean 0 and std 0.1 (as the bridge measures them): the test model's stems, xt * std + mean
 * = 0.2475 * mix each, then add up to 0.99 * mix, a residual of -40 dB, like a good model. */
static void chunk(char what, float *mix) {
    static float l[SRC], r[SRC];
    source(what == 'd' ? 7 : 1, l, r);
    double sum = 0, ss = 0;
    for (int i = 0; i < L; i++) sum += (double)l[i] + r[i];
    double mean = sum / (2 * L);
    for (int i = 0; i < L; i++) ss += (l[i] - mean) * (l[i] - mean) + (r[i] - mean) * (r[i] - mean);
    double scale = 0.1 / sqrt(ss / (2 * L - 1));
    for (int i = 0; i < SRC; i++) {
        l[i] = (float)((l[i] - mean) * scale);
        r[i] = (float)((r[i] - mean) * scale);
    }
    int off = what == 'b' ? 300 : 0;
    memcpy(mix, l + off, L * sizeof(float));
    memcpy(mix + L, r + off, L * sizeof(float));
    if (what == 'c')                                    /* 0.5-1.5 s: a quiet 1 kHz tone, left only */
        for (int i = 22050; i < 66150; i++) mix[i] += (float)(BURST * sin(2 * M_PI * 1000.0 * i / 44100.0));
}

/* number of complete .flac entries in the cache */
static int count_flac(const char *dir) {
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t len = strlen(e->d_name);
        if (e->d_type == DT_DIR) n += count_flac(p);
        else if (len > 5 && !strcmp(e->d_name + len - 5, ".flac")) n++;
    }
    closedir(d);
    return n;
}

/* the spectrogram part of a cached answer for a source whose bin 0 is c in every frame (the rest
 * zero), as Demucs's _ispec rebuilds it: c / 64 (normalized inverse FFT) times, at each kept
 * sample, the sum of the Hann windows of the F real frames over the sum of the squared windows
 * of all F + 4 frames (two empty ones on each side) */
static void spec_shape(int F, double *out) {
    const int N = 4096, H = 1024, start = N / 2 + H / 2 * 3;
    for (int n = 0; n < L; n++) {
        double s1 = 0, s2 = 0;
        long at = start + n;
        for (int t = 0; t < F + 4; t++) {
            long m = at - (long)t * H;
            if (m < 0 || m >= N) continue;
            double w = 0.5 - 0.5 * cos(2 * M_PI * m / N);
            s2 += w * w;
            if (t >= 2 && t < F + 2) s1 += w;
        }
        out[n] = s1 / s2 / 64.0;
    }
}

static int run(const char *model, int argc, char **argv, int pioneer) {
    const OrtApiBase *b = OrtGetApiBase();
    const OrtApi *a = b ? b->GetApi(ORT_API_VERSION) : NULL;
    if (!a) { check(0, "an OrtApi"); return 1; }
    OrtEnv *env = NULL;
    OrtSessionOptions *so = NULL;
    OrtSession *s = NULL;
    OrtMemoryInfo *mi = NULL;
    OrtStatus *st = a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "rbstems-tests", &env);
    if (!st) st = a->CreateSessionOptions(&so);
    if (!st) st = a->SetIntraOpNumThreads(so, 1);
    if (!st) st = a->CreateSession(env, model, so, &s);
    if (!st) st = a->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi);
    if (st) { printf("FAIL session: %s\n", a->GetErrorMessage(st)); return 1; }
    char cache[1200];
    snprintf(cache, sizeof cache, "%s/Library/Caches/rbstemsplus", getenv("HOME"));
    static float mix[2 * L];
    static double shape[L];
    float mag1 = 1.0f, *mag = &mag1;
    int64_t mdims[3] = {1, 2, L}, gdims[4] = {1, 1, 1, 1};
    int misses = 0, F = (L + 1023) / 1024;
    double w[4] = {0, 0, 0, 0}, gain = 1;
    for (int k = 0; k < argc; k++) {
        if (!strncmp(argv[k], "gain=", 5)) {
            gain = atof(argv[k] + 5);
            continue;
        }
        if (!strncmp(argv[k], "w=", 2)) {
            sscanf(argv[k] + 2, "%lf,%lf,%lf,%lf", &w[0], &w[1], &w[2], &w[3]);
            continue;
        }
        if (!strncmp(argv[k], "frames=", 7)) {
            F = atoi(argv[k] + 7);
            continue;
        }
        if (pioneer) {                                   /* mag [1, 4, 2048, F]: bin 0 of L.re and R.re at 1 */
            static float *buf;
            free(buf);
            buf = malloc(sizeof(float) * 4 * 2048 * (size_t)F);
            for (size_t i = 0; i < 4 * 2048 * (size_t)F; i++) buf[i] = (float)(0.37 * sin(0.001 * (double)i + 1.0));
            for (int f = 0; f < F; f++) buf[(size_t)0 * 2048 * F + f] = buf[(size_t)2 * 2048 * F + f] = 1.0f;
            mag = buf;
            gdims[1] = 4; gdims[2] = 2048; gdims[3] = F;
            spec_shape(F, shape);
        }
        if (!strcmp(argv[k], "wait")) {               /* the writer stores misses in the background */
            int n = 0;
            for (int t = 0; t < 300 && (n = count_flac(cache)) < misses; t++) usleep(100000);
            usleep(300000);                                /* ...and then its .desc */
            printf("stored %d\n", n);
            continue;
        }
        if (!strncmp(argv[k], "free=", 5)) {             /* after a sleep or wait: the writer is idle */
            setenv("RBSTEMS_TEST_FREE_GB", argv[k] + 5, 1);
            continue;
        }
        if (!strcmp(argv[k], "sleep")) {
            sleep(1);
            printf("stored %d\n", count_flac(cache));
            continue;
        }
        char what = argv[k][0];
        chunk(what, mix);
        if (gain != 1)
            for (int i = 0; i < 2 * L; i++) mix[i] = (float)(mix[i] * gain);
        OrtValue *in[2] = {NULL, NULL}, *out[2] = {NULL, NULL};
        const char *in_names[] = {"mix", "mag"}, *out_names[] = {"x", "xt"};
        st = a->CreateTensorWithDataAsOrtValue(mi, mix, sizeof mix, mdims, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[0]);
        if (!st) st = a->CreateTensorWithDataAsOrtValue(mi, mag, sizeof(float) * (size_t)(gdims[1] * gdims[2] * gdims[3]), gdims, 4,
                                                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1]);
        if (!st) st = a->Run(s, NULL, in_names, (const OrtValue *const *)in, 2, out_names, 2, out);
        if (st) { printf("FAIL run %c: %s\n", what, a->GetErrorMessage(st)); return 1; }
        float *x = NULL, *xt = NULL;
        if (a->GetTensorMutableData(out[0], (void **)&x) || a->GetTensorMutableData(out[1], (void **)&xt)) {
            printf("FAIL run %c: no output data\n", what);
            return 1;
        }
        size_t xn = pioneer ? (size_t)16 * 2048 * F : 4;
        int xzero = 1;
        for (size_t i = 0; i < xn; i++) xzero &= x[i] == 0.0f;
        if (pioneer) {
            /* the model's own answer: x = mag times the mask (w at bin 0 of the real parts), xt = tile(mix) * K */
            int xmodel = 1, xtmodel = 1, near = 1;
            for (int s = 0; s < 4 && xmodel; s++)
                for (int ch = 0; ch < 4 && xmodel; ch++)
                    for (int b = 0; b < 2048 && xmodel; b++)
                        for (int f = 0; f < F; f++) {
                            float want = (b == 0 && (ch == 0 || ch == 2)) ? (float)w[s] * mag[(size_t)ch * 2048 * F + f] : 0.0f;
                            float got = x[(((size_t)s * 4 + ch) * 2048 + b) * F + f];
                            if (!(got == want || (isnan(got) && isnan(want)))) { xmodel = 0; break; }
                        }
            double worst = 0;
            for (int ch = 0; ch < 8; ch++)
                for (int i = 0; i < L; i++) {
                    float m = mix[(ch & 1) * L + i] * K, got = xt[ch * L + i];
                    if (got != m) xtmodel = 0;
                    double want = m + w[ch / 2] * shape[i] / 0.1;   /* the cached stems, (stems - mean) / std */
                    double e = fabs(got - want);
                    if (!(e <= worst)) worst = e;
                }
            near = what == 'b' ? 1 : worst < 1e-3;
            const char *says = xmodel && xtmodel ? "model" : xzero && near ? "cache" : "wrong";
            printf("%c %s (x zero %d, worst %.2e)\n", what, says, xzero, worst);
            if (xmodel && xtmodel) misses++;
            a->ReleaseValue(in[0]); a->ReleaseValue(in[1]); a->ReleaseValue(out[0]); a->ReleaseValue(out[1]);
            continue;
        }
        /* the model's answer is exactly tile(mix) * K; a cached one differs by the 16-bit
         * rounding; a near one (b) is the shifted entry's */
        int exact = 1, finite = 1;
        double err = 0, ref = 0;
        for (int ch = 0; ch < 8; ch++)
            for (int i = 0; i < L; i++) {
                float want = mix[(ch & 1) * L + i] * K, got = xt[ch * L + i];
                if (got != want) exact = 0;
                if (!isfinite(got)) finite = 0;
                err += (double)(got - want) * (got - want);
                ref += (double)want * want;
            }
        double rel = sqrt(err / ref);
        printf("%c %s (x zero %d, finite %d, error %.2e)\n", what, exact ? "model" : "cache", xzero, finite, rel);
        if (exact) misses++;
        a->ReleaseValue(in[0]);
        a->ReleaseValue(in[1]);
        a->ReleaseValue(out[0]);
        a->ReleaseValue(out[1]);
    }
    a->ReleaseSession(s);
    a->ReleaseMemoryInfo(mi);
    a->ReleaseSessionOptions(so);
    a->ReleaseEnv(env);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 3 && !strcmp(argv[1], "load")) return load(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "passthrough")) return passthrough(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "missing")) return missing();
    if (argc >= 3 && !strcmp(argv[1], "run")) return run(argv[2], argc - 3, argv + 3, 0);
    if (argc >= 3 && !strcmp(argv[1], "prun")) return run(argv[2], argc - 3, argv + 3, 1);
    fprintf(stderr, "usage: see the top of harness.c\n");
    return 2;
}
