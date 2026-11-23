/* The model, run through Pioneer's own ONNX Runtime (the copy the app keeps in
 * /Library/Application Support/rbstemsplus/ort: the library inside rekordbox.app is the bridge),
 * on one chunk at a time. rekordbox's model also takes the chunk's spectrogram (mag): it is
 * computed here as Demucs computes it (spec_zero); our model ignores it (zeros).
 *
 * Windows (helper.md §3, T12): the app's kept copy of Pioneer's onnxruntime.dll, loaded by its
 * full path (LoadLibraryExW, DLL_LOAD_DIR | SYSTEM32: plat_lib_open), the model's path as
 * wchar_t (ORTCHAR_T), the spectrogram's FFT through pocketfft (spec_zero_pf). The CPU by
 * default; DirectML only when asked (--dml: model_use_dml), with DirectML.dll loaded first by its
 * full path from rekordbox's versioned folder (never System32's older copy) and logged, through
 * the DLL's ordinal 4 (OrtSessionOptionsAppendExecutionProvider_DML, device 0); any DirectML
 * failure falls back to the CPU, said on stderr. */
#if defined(_WIN32)
#include <windows.h>
#include "pocketfft.h"
#else
#include <Accelerate/Accelerate.h>
#include <dlfcn.h>
#endif
#if defined(RBSP_TEST_WIN_MODEL)
#include "pocketfft.h"
#endif
#include <math.h>
#if defined(_WIN32) && !defined(M_PI)
#define M_PI 3.14159265358979323846     /* -std=c11: mingw-w64's math.h doesn't define it */
#endif
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
#if defined(_WIN32) || defined(RBSP_TEST_DML_SESSION)
    const char *path;
    plat_model_info info;
    ORTCHAR_T *w;                       /* the model's path, threads and kind: the CPU session made */
    int threads, ours;                  /* again after a DirectML run failure */
#endif
};

#if defined(_WIN32) || defined(RBSP_TEST_WIN_MODEL)
/* The automatic thread count (helper.md §1 prepare.c:499, §3): half the active processors, as
 * rekordbox's getNumCpus() / 2, clamped to 1-64 (prepare.c refuses more than 64, and the app passes
 * no --threads); a zero or failed count gives 1 */
int model_default_threads(long ncpu) {
    long t = ncpu / 2;
    return t < 1 ? 1 : t > 64 ? 64 : (int)t;
}

/* The session's execution provider: DirectML only when asked (--dml), DirectML.dll loaded from
 * rekordbox's folder and ORT's ordinal 4 found; the CPU otherwise. A DirectML session that then
 * fails (the append or the session itself) is made again on the CPU (model_open) */
model_ep model_ep_choice(int asked, int directml_loaded, int append_found) {
    return asked && directml_loaded && append_found ? MODEL_EP_DML : MODEL_EP_CPU;
}

/* After a failed run on EP: 1 when the chunk runs again on a new CPU session, which the rest of
 * the run keeps (a DirectML run that fails, as on a GPU whose driver takes the session but not
 * the model: T17's VM); 0 when the failure stands (the CPU's: the track is skipped) */
int model_retry_on_cpu(model_ep ep) { return ep == MODEL_EP_DML; }

/* The timing lines' Windows fields (helper.md §1 prepare.c:637, §2a): the power source from
 * GetSystemPowerStatus's ACLineStatus (1 AC, 0 battery, anything else unknown) and the session's
 * execution provider, as " power=ac|battery|unknown ep=cpu|dml"; the characters written (0 when
 * N is too small: BUF is then "") */
int model_timing_fields(char *buf, size_t n, int ac_line_status, model_ep ep) {
    const char *power = ac_line_status == 1 ? "ac" : ac_line_status == 0 ? "battery" : "unknown";
    int k = snprintf(buf, n, " power=%s ep=%s", power, ep == MODEL_EP_DML ? "dml" : "cpu");
    if (k < 0 || (size_t)k >= n) { if (n) buf[0] = 0; return 0; }
    return k;
}
#endif

#if defined(_WIN32) || defined(RBSP_TEST_DML_SESSION)
typedef OrtStatus *(ORT_API_CALL *dml_append_fn)(OrtSessionOptions *o, int device_id);
static dml_append_fn dml_append;        /* ORT's ordinal 4, when DirectML was asked for and loaded */
static model_ep ep_used = MODEL_EP_CPU;
#endif

#if defined(_WIN32)
static int dml_asked, dml_loaded;
model_ep model_ep_used(void) { return ep_used; }

/* "a.b.c.d" from a loaded module's own version resource (VS_FIXEDFILEINFO), kernel32 only */
static void module_version(HMODULE m, char *out, size_t n) {
    snprintf(out, n, "unknown");
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));   /* RT_VERSION */
    HGLOBAL g = r ? LoadResource(m, r) : NULL;
    const unsigned char *p = g ? LockResource(g) : NULL;
    DWORD size = r ? SizeofResource(m, r) : 0;
    for (DWORD i = 0; p && i + 52 <= size; i += 4) {
        uint32_t sig, ms, ls;
        memcpy(&sig, p + i, 4);
        if (sig != 0xFEEF04BDu) continue;
        memcpy(&ms, p + i + 8, 4);
        memcpy(&ls, p + i + 12, 4);
        snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ms >> 16), (unsigned)(ms & 0xffff), (unsigned)(ls >> 16), (unsigned)(ls & 0xffff));
        return;
    }
}

int model_use_dml(const char *rekordbox_dir) {
    char p[4096];
    const char *err = NULL;
    dml_asked = 1;
    size_t l = strlen(rekordbox_dir);
    int sep = l && (rekordbox_dir[l - 1] == '\\' || rekordbox_dir[l - 1] == '/');
    if (snprintf(p, sizeof p, "%s%sDirectML.dll", rekordbox_dir, sep ? "" : "\\") >= (int)sizeof p) err = "the path is too long";
    plat_lib *h = err ? NULL : plat_lib_open(p, &err);
    if (!h) {
        fprintf(stderr, "DirectML: %s can't be loaded (%s): the model runs on the CPU\n", p, err ? err : "?");
        return 1;
    }
    char ver[64];
    const char *where = plat_lib_path_of((const void *)plat_lib_sym(h, "DMLCreateDevice"));
    module_version((HMODULE)(void *)h, ver, sizeof ver);
    fprintf(stderr, "DirectML: %s version %s\n", where ? where : p, ver);
    dml_loaded = 1;
    return 0;
}

long model_active_processors(void) { return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); }

int model_ac_line_status(void) {
    SYSTEM_POWER_STATUS s;
    return GetSystemPowerStatus(&s) ? s.ACLineStatus : 255;
}

int model_background(void) {
    /* below normal, next to a playing rekordbox (the Mac's utility QoS class); ORT's threads
     * inherit it. And out of Windows' own EcoQoS heuristics: the windowless helper's speed must not
     * depend on them (helper.md §1 prepare.c:637; opting into EcoQoS would have to pass P-H1) */
    PROCESS_POWER_THROTTLING_STATE t = {PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
    int bad = !SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    bad |= !SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &t, sizeof t) << 1;
    return bad;
}

int model_load_ort(const char *lib, char *why, size_t whyn) {
    const char *err = NULL;
    plat_lib *h = plat_lib_open(lib, &err);
    if (!h) { snprintf(why, whyn, "ONNX Runtime can't be loaded: %s", err ? err : "?"); return 1; }
    const OrtApiBase *(ORT_API_CALL *get)(void) = (const OrtApiBase *(ORT_API_CALL *)(void))plat_lib_sym(h, "OrtGetApiBase");
    if (!get) get = (const OrtApiBase *(ORT_API_CALL *)(void))plat_lib_sym(h, (const char *)(uintptr_t)1);   /* Pioneer's: ordinal 1 */
    if (dml_asked && dml_loaded) {
        dml_append = (dml_append_fn)plat_lib_sym(h, (const char *)(uintptr_t)4);   /* ordinal 4, as Demucs3.dll imports it */
        if (!dml_append) fprintf(stderr, "DirectML: ONNX Runtime has no ordinal 4: the model runs on the CPU\n");
    }
#else
int model_load_ort(const char *lib, char *why, size_t whyn) {
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(why, whyn, "ONNX Runtime can't be loaded: %s", dlerror()); return 1; }
    const OrtApiBase *(*get)(void) = (const OrtApiBase *(*)(void))dlsym(h, "OrtGetApiBase");
#endif
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
#if defined(_WIN32)
    /* DirectML's messages end in CRLF: one line on stderr and in a skipped line's why */
    for (size_t k = strlen(why); k && (why[k - 1] == '\r' || why[k - 1] == '\n' || why[k - 1] == ' '); k--) why[k - 1] = 0;
#endif
    return 1;
}

static int model_unchanged(const char *path, const plat_model_info *info, char *why, size_t whyn) {
    plat_model_info current;
    if (!plat_model_stat_path(path, &current) && current.size == info->size &&
        current.mtime == info->mtime && current.mtime_nsec == info->mtime_nsec &&
        plat_id_same(current.id, info->id)) return 1;
    snprintf(why, whyn, "the model file changed or became unavailable after hashing");
    return 0;
}

enum { SESSION_OK, SESSION_ERROR, SESSION_MODEL_CHANGED };

static int create_session(model *m, const ORTCHAR_T *ort_path, OrtSessionOptions *o,
                          const char *path, const plat_model_info *info, char *why, size_t whyn) {
    OrtStatus *st = R->CreateSession(env, ort_path, o, &m->s);
    int bad = fail(st, why, whyn);
    if (!model_unchanged(path, info, why, whyn)) return SESSION_MODEL_CHANGED;
    return bad ? SESSION_ERROR : SESSION_OK;
}

#if defined(_WIN32) || defined(RBSP_TEST_DML_SESSION)
/* one session on EP (DirectML: the memory pattern off and sequential execution, as ORT's DirectML
 * provider requires) */
static int session(model *m, model_ep ep, char *why, size_t whyn) {
    OrtSessionOptions *o = NULL;
    if (fail(R->CreateSessionOptions(&o), why, whyn)) return SESSION_ERROR;
    int bad = fail(R->SetIntraOpNumThreads(o, m->threads), why, whyn) || fail(R->SetInterOpNumThreads(o, 1), why, whyn)
              /* our model: the bridge opens it with the memory pattern off (peak 1.75 GB, not 2.26) */
              || ((m->ours || ep == MODEL_EP_DML) && fail(R->DisableMemPattern(o), why, whyn))
              || (ep == MODEL_EP_DML && fail(R->SetSessionExecutionMode(o, ORT_SEQUENTIAL), why, whyn))
              || (ep == MODEL_EP_DML && fail(dml_append(o, 0), why, whyn));
    bad = bad ? SESSION_ERROR : create_session(m, m->w, o, m->path, &m->info, why, whyn);
    R->ReleaseSessionOptions(o);
    if (bad && m->s) { R->ReleaseSession(m->s); m->s = NULL; }
    return bad;
}

static int cpu_fallback(model *m, char *why, size_t whyn) {
    if (!model_unchanged(m->path, &m->info, why, whyn)) return SESSION_MODEL_CHANGED;
    fprintf(stderr, "DirectML: %s: the model runs on the CPU\n", why);
    return session(m, MODEL_EP_CPU, why, whyn);
}
#endif

#if defined(_WIN32)
model *model_open(const char *path, const plat_model_info *info, int threads, int ours, char *why, size_t whyn) {
    model *m = calloc(1, sizeof *m);
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *w = wn > 0 ? malloc(sizeof(wchar_t) * (size_t)wn) : NULL;
    if (!m || !w || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, wn) != wn) {
        snprintf(why, whyn, "the model's path can't be used: %s", path);
        free(m);
        free(w);
        return NULL;
    }
    m->path = path;
    m->info = *info;
    m->w = w;
    m->threads = threads;
    m->ours = ours;
    model_ep ep = model_ep_choice(dml_asked, dml_loaded, dml_append != NULL);
    int bad = session(m, ep, why, whyn);
    if (bad == SESSION_ERROR && ep == MODEL_EP_DML) {
        ep = MODEL_EP_CPU;
        bad = cpu_fallback(m, why, whyn);
    }
    if (bad || fail(R->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m->mem), why, whyn)) {
        model_close(m);
        return NULL;
    }
    ep_used = ep;
    return m;
}
#else
model *model_open(const char *path, const plat_model_info *info, int threads, int ours, char *why, size_t whyn) {
    model *m = calloc(1, sizeof *m);
    OrtSessionOptions *o = NULL;
    if (!m || fail(R->CreateSessionOptions(&o), why, whyn)) { free(m); return NULL; }
    int bad = fail(R->SetIntraOpNumThreads(o, threads), why, whyn) || fail(R->SetInterOpNumThreads(o, 1), why, whyn)
              /* our model: the bridge opens it with the memory pattern off (peak 1.75 GB, not 2.26) */
              || (ours && fail(R->DisableMemPattern(o), why, whyn))
              || create_session(m, path, o, path, info, why, whyn)
              || fail(R->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m->mem), why, whyn);
    R->ReleaseSessionOptions(o);
    if (bad) { model_close(m); return NULL; }
    return m;
}
#endif

void model_close(model *m) {
    if (!m) return;
    if (m->s) R->ReleaseSession(m->s);
    if (m->mem) R->ReleaseMemoryInfo(m->mem);
#if defined(_WIN32)
    free(m->w);
#endif
    free(m);
}

/* Demucs's spectrogram of a chunk (HDemucs._spec, zero padding): STFT, NFFT 4096, hop 1024,
 * periodic Hann, "normalized" (/ sqrt(NFFT)), centred, after Demucs's 3/2-hop padding; the top bin
 * and the edge frames dropped. mix [2][L] -> mag [4: L.re L.im R.re R.im][BINS][F], in double
 * then rounded to float32 (as the verified prototype) */
#if defined(_WIN32) || defined(RBSP_TEST_WIN_MODEL)
/* spec_zero on Windows: the same frames and scaling through pocketfft's forward real FFT (the plain
 * DFT in double, half-complex order r0, r1, i1, ..., r(n/2-1), i(n/2-1), r(n/2)); held to the
 * vDSP one on the Mac (helper/tests/model-win-host.sh) */
static int spec_zero_pf(const float *mix, size_t L, size_t F, float *mag) {
    const size_t pad = HL / 2 * 3, left = pad + NFFT / 2;
    rfft_plan plan = make_rfft_plan(NFFT);
    double *win = malloc(sizeof(double) * NFFT), *fr = malloc(sizeof(double) * NFFT);
    int ok = plan && win && fr;
    if (ok) {
        for (int n = 0; n < NFFT; n++) win[n] = 0.5 - 0.5 * cos(2 * M_PI * n / NFFT);
        const double scale = 1.0 / sqrt((double)NFFT);    /* pocketfft's forward is the plain DFT */
        for (int c = 0; c < 2 && ok; c++) {
            const float *x = mix + (size_t)c * L;
            for (size_t f = 0; f < F; f++) {
                long start = (long)((f + 2) * HL) - (long)left;
                for (int n = 0; n < NFFT; n++) {
                    long k = start + n;
                    fr[n] = (k >= 0 && k < (long)L ? (double)x[k] : 0.0) * win[n];
                }
                if (rfft_forward(plan, fr, 1.0) != 0) { ok = 0; break; }
                float *R_ = mag + (size_t)(2 * c) * BINS * F, *I_ = mag + (size_t)(2 * c + 1) * BINS * F;
                R_[f] = (float)(fr[0] * scale);           /* the Nyquist bin, fr[NFFT - 1], dropped */
                I_[f] = 0.0f;
                for (size_t k = 1; k < BINS; k++) {
                    R_[k * F + f] = (float)(fr[2 * k - 1] * scale);
                    I_[k * F + f] = (float)(fr[2 * k] * scale);
                }
            }
        }
    }
    if (plan) destroy_rfft_plan(plan);
    free(win);
    free(fr);
    return ok ? 0 : 1;
}
#endif

#if !defined(_WIN32)
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
#endif

#if defined(_WIN32) || defined(RBSP_TEST_DML_SESSION)
static int run_once(model *m, const float *mix, size_t L, int ours, float **x_out, float **xt_out, size_t *F_out, char *why, size_t whyn);

/* a DirectML run that fails is said on stderr and the chunk runs again on a CPU session, which the
 * rest of the run keeps (model_retry_on_cpu); a CPU session that can't be made stops the batch. */
int model_run(model *m, const float *mix, size_t L, int ours, float **x_out, float **xt_out, size_t *F_out, char *why, size_t whyn) {
    if (!m || !m->s) { snprintf(why, whyn, "no ONNX Runtime session"); return MODEL_RUN_BATCH_ERROR; }
    int rc = run_once(m, mix, L, ours, x_out, xt_out, F_out, why, whyn);
    if (!rc || !model_retry_on_cpu(ep_used)) return rc;
    R->ReleaseSession(m->s);
    m->s = NULL;
    ep_used = MODEL_EP_CPU;
    if (cpu_fallback(m, why, whyn)) return MODEL_RUN_BATCH_ERROR;
    return run_once(m, mix, L, ours, x_out, xt_out, F_out, why, whyn);
}

static int run_once(model *m, const float *mix, size_t L, int ours, float **x_out, float **xt_out, size_t *F_out, char *why, size_t whyn) {
#else
int model_run(model *m, const float *mix, size_t L, int ours, float **x_out, float **xt_out, size_t *F_out, char *why, size_t whyn) {
#endif
    size_t F = (L + HL - 1) / HL;
    float *mag = calloc(4 * BINS * F, sizeof(float));
    if (!mag) { snprintf(why, whyn, "out of memory"); return 1; }
#if defined(_WIN32)
    if (!ours && spec_zero_pf(mix, L, F, mag)) { free(mag); snprintf(why, whyn, "the spectrogram failed"); return 1; }
#else
    if (!ours && spec_zero(mix, L, F, mag)) { free(mag); snprintf(why, whyn, "the spectrogram failed"); return 1; }
#endif
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
