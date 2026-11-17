/* The model, run through Pioneer's own ONNX Runtime (the copy the app keeps in
 * /Library/Application Support/rbstemsplus/ort: the library inside rekordbox.app is the bridge),
 * on one chunk at a time. rekordbox's model also takes the chunk's spectrogram (mag): it is
 * computed here as Demucs computes it (spec_zero); our model ignores it (zeros). */
#include <Accelerate/Accelerate.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "onnxruntime_c_api.h"
#include "rbsp.h"

#define NFFT 4096
#define HL 1024

static const OrtApi *R;
static OrtEnv *env;

/* the timing lines (rbsp.h): ORT_RUN is R->Run, timed alone in a timing build (t_ort) */
#ifdef RBSP_TEST_TIMING
uint64_t t_ort;
static OrtStatus *timed_run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names, const OrtValue *const *in,
                            size_t n_in, const char *const *out_names, size_t n_out, OrtValue **out) {
    uint64_t t0 = t_now();
    OrtStatus *st = R->Run(s, ro, in_names, in, n_in, out_names, n_out, out);
    t_ort = t_now() - t0;
    return st;
}
#define ORT_RUN timed_run
#else
#define ORT_RUN R->Run
#endif

struct model {
    OrtSession *s;
    OrtMemoryInfo *mem;
};

int model_load_ort(const char *lib, char *why, size_t whyn) {
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(why, whyn, "ONNX Runtime can't be loaded: %s", dlerror()); return 1; }
    const OrtApiBase *(*get)(void) = (const OrtApiBase *(*)(void))dlsym(h, "OrtGetApiBase");
    const OrtApiBase *b = get ? get() : NULL;
    const char *ver = b ? b->GetVersionString() : "";
    if (!b || strncmp(ver, "1.18.", 5)) { snprintf(why, whyn, "ONNX Runtime 1.18 expected, found %s", ver[0] ? ver : "none"); return 1; }
    R = b->GetApi(ORT_API_VERSION);
    if (!R) { snprintf(why, whyn, "ONNX Runtime has no API %d", ORT_API_VERSION); return 1; }
    OrtStatus *st = R->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "rbsp-prepare", &env);
    if (st) { snprintf(why, whyn, "ONNX Runtime: %s", R->GetErrorMessage(st)); R->ReleaseStatus(st); return 1; }
    return 0;
}

static int fail(OrtStatus *st, char *why, size_t whyn) {
    if (!st) return 0;
    snprintf(why, whyn, "ONNX Runtime: %s", R->GetErrorMessage(st));
    R->ReleaseStatus(st);
    return 1;
}

model *model_open(const char *path, int threads, int ours, char *why, size_t whyn) {
    model *m = calloc(1, sizeof *m);
    OrtSessionOptions *o = NULL;
    if (!m || fail(R->CreateSessionOptions(&o), why, whyn)) { free(m); return NULL; }
    int bad = fail(R->SetIntraOpNumThreads(o, threads), why, whyn) || fail(R->SetInterOpNumThreads(o, 1), why, whyn)
              /* our model: the bridge opens it with the memory pattern off (peak 1.75 GB, not 2.26) */
              || (ours && fail(R->DisableMemPattern(o), why, whyn))
              || fail(R->CreateSession(env, path, o, &m->s), why, whyn)
              || fail(R->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m->mem), why, whyn);
    R->ReleaseSessionOptions(o);
    if (bad) { model_close(m); return NULL; }
    return m;
}

void model_close(model *m) {
    if (!m) return;
    if (m->s) R->ReleaseSession(m->s);
    if (m->mem) R->ReleaseMemoryInfo(m->mem);
    free(m);
}

/* Demucs's spectrogram of a chunk (HDemucs._spec, zero padding): STFT, NFFT 4096, hop 1024,
 * periodic Hann, "normalized" (/ sqrt(NFFT)), centred, after Demucs's 3/2-hop padding; the top bin
 * and the edge frames dropped. mix [2][L] -> mag [4: L.re L.im R.re R.im][BINS][F], in double
 * then rounded to float32 (as the verified prototype) */
static int spec_zero(const float *mix, size_t L, size_t F, float *mag) {
    const size_t pad = HL / 2 * 3, left = pad + NFFT / 2;
    FFTSetupD setup = vDSP_create_fftsetupD(12, kFFTRadix2);
    double *win = malloc(sizeof(double) * NFFT), *fr = malloc(sizeof(double) * NFFT);
    double *re = malloc(sizeof(double) * NFFT / 2), *im = malloc(sizeof(double) * NFFT / 2);
    int ok = setup && win && fr && re && im;
    if (ok) {
        for (int n = 0; n < NFFT; n++) win[n] = 0.5 - 0.5 * cos(2 * M_PI * n / NFFT);
        const double scale = 0.5 / sqrt((double)NFFT);    /* vDSP's forward real FFT is 2x the DFT */
        DSPDoubleSplitComplex sc = {re, im};
        for (int c = 0; c < 2; c++) {
            const float *x = mix + (size_t)c * L;
            for (size_t f = 0; f < F; f++) {
                /* frame t = f + 2 starts at t * HL in the padded signal: sample t*HL - left of x */
                long start = (long)((f + 2) * HL) - (long)left;
                for (int n = 0; n < NFFT; n++) {
                    long k = start + n;
                    fr[n] = (k >= 0 && k < (long)L ? (double)x[k] : 0.0) * win[n];
                }
                vDSP_ctozD((const DSPDoubleComplex *)fr, 2, &sc, 1, NFFT / 2);
                vDSP_fft_zripD(setup, &sc, 1, 12, kFFTDirection_Forward);
                float *R_ = mag + (size_t)(2 * c) * BINS * F, *I_ = mag + (size_t)(2 * c + 1) * BINS * F;
                /* packed: re[0] = DC, im[0] = Nyquist (dropped: the model's bins are 0..2047) */
                R_[f] = (float)(re[0] * scale);
                I_[f] = 0.0f;
                for (size_t k = 1; k < BINS; k++) {
                    R_[k * F + f] = (float)(re[k] * scale);
                    I_[k * F + f] = (float)(im[k] * scale);
                }
            }
        }
    }
    if (setup) vDSP_destroy_fftsetupD(setup);
    free(win); free(fr); free(re); free(im);
    return ok ? 0 : 1;
}

int model_run(model *m, const float *mix, size_t L, int ours, float **x_out, float **xt_out, size_t *F_out, char *why, size_t whyn) {
    size_t F = (L + HL - 1) / HL;
    float *mag = calloc(4 * BINS * F, sizeof(float));
    if (!mag) { snprintf(why, whyn, "out of memory"); return 1; }
    if (!ours && spec_zero(mix, L, F, mag)) { free(mag); snprintf(why, whyn, "the spectrogram failed"); return 1; }
    int64_t mixdims[3] = {1, 2, (int64_t)L}, magdims[4] = {1, 4, BINS, (int64_t)F};
    OrtValue *in[2] = {NULL, NULL}, *out[2] = {NULL, NULL};
    const char *in_names[2] = {"mix", "mag"}, *out_names[2] = {"x", "xt"};
    int rc = fail(R->CreateTensorWithDataAsOrtValue(m->mem, (void *)mix, sizeof(float) * 2 * L, mixdims, 3,
                                                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[0]), why, whyn)
             || fail(R->CreateTensorWithDataAsOrtValue(m->mem, mag, sizeof(float) * 4 * BINS * F, magdims, 4,
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1]), why, whyn)
             || fail(ORT_RUN(m->s, NULL, in_names, (const OrtValue *const *)in, 2, out_names, 2, out), why, whyn);
    float *xd = NULL, *xtd = NULL;
    OrtTensorTypeAndShapeInfo *ti = NULL;
    size_t nx = 0, nxt = 0;
    if (!rc) rc = fail(R->GetTensorMutableData(out[0], (void **)&xd), why, whyn) || fail(R->GetTensorMutableData(out[1], (void **)&xtd), why, whyn);
    if (!rc && !fail(R->GetTensorTypeAndShape(out[0], &ti), why, whyn)) { rc = fail(R->GetTensorShapeElementCount(ti, &nx), why, whyn); R->ReleaseTensorTypeAndShapeInfo(ti); }
    if (!rc && !fail(R->GetTensorTypeAndShape(out[1], &ti), why, whyn)) { rc = fail(R->GetTensorShapeElementCount(ti, &nxt), why, whyn); R->ReleaseTensorTypeAndShapeInfo(ti); }
    if (!rc && (nx != 16 * BINS * F || nxt != 8 * L)) { snprintf(why, whyn, "the model's outputs have unexpected shapes"); rc = 1; }
    if (!rc) {
        *x_out = malloc(sizeof(float) * nx);
        *xt_out = malloc(sizeof(float) * nxt);
        if (!*x_out || !*xt_out) { free(*x_out); free(*xt_out); snprintf(why, whyn, "out of memory"); rc = 1; }
        else {
            memcpy(*x_out, xd, sizeof(float) * nx);
            memcpy(*xt_out, xtd, sizeof(float) * nxt);
            *F_out = F;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (in[i]) R->ReleaseValue(in[i]);
        if (out[i]) R->ReleaseValue(out[i]);
    }
    free(mag);
    return rc;
}
