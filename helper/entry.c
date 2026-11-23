/* A cache entry as the bridge writes one (store_entry): <key>.flac (8 channels, 16-bit, the four
 * stems L/R at half level) then <key>.desc (the chunk's summary and residuals), each through a
 * temporary file renamed when complete, with the bridge's refusals. The functions between the
 * "from the bridge" lines are copies of bridge/rbstems_bridge.c's, kept identical
 * (helper/tests/same-as-bridge.sh checks). The FLAC also names the helper's version
 * (RBSTEMS_HELPER: the bridge ignores other tags), so a later helper replaces older entries. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <Accelerate/Accelerate.h>          /* vDSP; Windows: plat.h's pocketfft (plat_irfft) */
#elif !defined(M_PI)
#define M_PI 3.14159265358979323846         /* -std=c11: mingw-w64's math.h doesn't define it */
#endif
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include "entry.h"

#define FORMAT "rbstems-v1"
#define DESC_FRAME 4410                 /* summary resolution: 0.1 s */
#define DESC_MAX_FRAMES 512
#define NFFT 4096
#define HOP 1024
#define BINS 2048                       /* NFFT / 2: the model's bins (the top one is dropped) */

/* ---------------------------------------------------------------- from the bridge */

static int summarize(const float *mix, size_t L, float *out) {
    int n = (int)(L / DESC_FRAME);
    if (n > DESC_MAX_FRAMES) n = DESC_MAX_FRAMES;
    const float *l = mix, *r = mix + L;
    double prev = 0;
    for (int f = 0; f < n; f++) {
        double e0 = 0, e1 = 0;
        for (size_t i = (size_t)f * DESC_FRAME; i < (size_t)(f + 1) * DESC_FRAME; i++) {
            double m = 0.5 * ((double)l[i] + r[i]);
            e0 += m * m;
            e1 += (m - prev) * (m - prev);
            prev = m;
        }
        out[f] = (float)log10(sqrt(e0 / DESC_FRAME) + 1e-6);
        out[n + f] = (float)log10(sqrt(e1 / DESC_FRAME) + 1e-6);
    }
    return n;
}

static void residual(const int16_t *pcm, const float *mix, size_t L, long d, int frames,
                     float *frame_db /* [2][frames] */, float *total_db /* [2] */, double *mixe /* [2][frames] */) {
    for (int c = 0; c < 2; c++) {
        const float *m = mix + (size_t)c * L;
        double rt = 0, mt = 0;
        for (int f = 0; f < frames; f++) {
            double re = 0, me = 0;
            for (size_t n = (size_t)f * DESC_FRAME; n < (size_t)(f + 1) * DESC_FRAME; n++) {
                long k = (long)n - d;
                double s = 0;
                if (k >= 0 && k < (long)L) {
                    const int16_t *p = pcm + (size_t)k * 8 + c;
                    s = (p[0] + p[2] + p[4] + p[6]) / 32767.0 * 2.0;
                } else {
                    s = m[n];
                }
                double e = m[n] - s;
                re += e * e;
                me += (double)m[n] * m[n];
            }
            frame_db[c * frames + f] = (float)(10 * log10((re + 1e-12) / (me + 1e-12)));
            if (mixe) mixe[c * frames + f] = me / DESC_FRAME;
            rt += re;
            mt += me;
        }
        total_db[c] = (float)(10 * log10((rt + 1e-12) / (mt + 1e-12)));
    }
}

static int all_finite(const float *v, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!isfinite(v[i])) return 0;
    return 1;
}

static void monos(const int16_t *pcm, const float *mix, size_t L, float *s, float *m) {
    for (size_t i = 0; i < L; i++) {
        double v = 0;
        for (int ch = 0; ch < 8; ch++) v += pcm[i * 8 + ch];
        s[i] = (float)(v / 32767.0 * 2.0 * 0.5);                 /* stems doubled, L+R halved */
        m[i] = 0.5f * (mix[i] + mix[L + i]);
    }
}

static void corr_at(const float *s, const float *m, size_t L, long d, double ss, double mm, double *r, double *g) {
    double sm = 0;
    for (size_t n = 0; n < L; n++) {
        long k = (long)n - d;
        if (k >= 0 && k < (long)L) sm += (double)m[n] * s[k];
    }
    *r = sm / sqrt(ss * mm + 1e-30);
    *g = sm / (mm + 1e-30);
}

static int rebuild_spec(const float *x, size_t L, size_t F, float *out) {
    const size_t pad = HOP / 2 * 3, start = NFFT / 2 + pad;  /* centre trim, then Demucs's padding */
    if (F != (L + HOP - 1) / HOP) return 1;
    const size_t frames = F + 4, full = NFFT + HOP * (frames - 1);
    if (start + L > full) return 1;
#if defined(_WIN32)
    plat_irfft *setup = plat_irfft_new(12);                 /* pocketfft: vDSP's packing and sum (irfft_cmp.c) */
#else
    FFTSetup setup = vDSP_create_fftsetup(12, kFFTRadix2);    /* per call: no shared state between threads */
#endif
    float *win = malloc(sizeof(float) * NFFT), *env = calloc(full, sizeof(float)), *y = malloc(sizeof(float) * full);
    float *re = malloc(sizeof(float) * BINS), *im = malloc(sizeof(float) * BINS), *frame = malloc(sizeof(float) * NFFT);
    int ok = setup && win && env && y && re && im && frame;
    if (ok) {
        for (int n = 0; n < NFFT; n++) win[n] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / NFFT);
        for (size_t t = 0; t < frames; t++)              /* the window envelope covers every frame, empty ones too */
            for (int n = 0; n < NFFT; n++) env[t * HOP + n] += win[n] * win[n];
        /* vDSP's inverse real FFT returns the plain sum (no 1/NFFT): times sqrt(NFFT) for
         * "normalized", over NFFT for the inverse DFT */
        const float scale = (float)(sqrt((double)NFFT) / NFFT);
#if !defined(_WIN32)
        DSPSplitComplex sc = {re, im};
#endif
        for (int s = 0; s < 4; s++)
            for (int c = 0; c < 2; c++) {
                memset(y, 0, sizeof(float) * full);
                const float *xr = x + (size_t)(s * 4 + 2 * c) * BINS * F, *xi = x + (size_t)(s * 4 + 2 * c + 1) * BINS * F;
                for (size_t f = 0; f < F; f++) {
                    /* packed: re[0] = DC, im[0] = Nyquist (the restored top bin, zero) */
                    for (size_t k = 0; k < BINS; k++) { re[k] = xr[k * F + f]; im[k] = xi[k * F + f]; }
                    im[0] = 0.0f;
#if defined(_WIN32)
                    plat_irfft_run(setup, re, im, frame);
#else
                    vDSP_fft_zrip(setup, &sc, 1, 12, kFFTDirection_Inverse);
                    vDSP_ztoc(&sc, 1, (DSPComplex *)frame, 2, BINS);
#endif
                    const size_t off = (f + 2) * HOP;
                    for (int n = 0; n < NFFT; n++) y[off + n] += frame[n] * scale * win[n];
                }
                float *o = out + (size_t)(s * 2 + c) * L;
                for (size_t n = 0; n < L; n++) o[n] = y[start + n] / env[start + n];
            }
    }
#if defined(_WIN32)
    if (setup) plat_irfft_free(setup);
#else
    if (setup) vDSP_destroy_fftsetup(setup);
#endif
    free(win); free(env); free(y); free(re); free(im); free(frame);
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------- end of the bridge's code */

static void hex(const unsigned char *d, int n, char *out) {
    for (int i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

int rebuilt_id(const char *fp, char out[65]) {            /* the bridge's rebuilt_id */
    unsigned char d[32];
    plat_sha256_ctx c;
    if (plat_sha256_init(&c)) return -1;
    plat_sha256_update(&c, "rbstems-rebuild-" REBUILD_VERSION ":", strlen("rbstems-rebuild-" REBUILD_VERSION ":"));
    plat_sha256_update(&c, fp, 64);
    if (plat_sha256_final(&c, d)) return -1;
    hex(d, 32, out);
    return 0;
}

/* a model file (it may be a link: rekordbox's copy), read whole */
int file_sha256(const char *path, char out[65], plat_model_info *info) {
    plat_file f;
    if (plat_file_open_path(path, &f)) return -1;
    if (info && plat_model_stat_file(f, info)) { plat_file_close(f); return -1; }
    unsigned char *buf = malloc(1 << 20);
    plat_sha256_ctx c;
    if (!buf || plat_sha256_init(&c)) { free(buf); plat_file_close(f); return -1; }
    size_t n = 0;
    int e;
    while (!(e = plat_read(f, buf, 1 << 20, &n)) && n > 0) plat_sha256_update(&c, buf, n);
    free(buf);
    plat_file_close(f);
    unsigned char d[32];
    if (plat_sha256_final(&c, d) || e) return -1;
    hex(d, 32, out);
    return 0;
}

/* the exact key, as the bridge's hook_Run: format, model, shapes and the audio itself */
int chunk_key(const char *model, const float *mix, size_t L, size_t F, char key[65]) {
    int64_t mixdims[3] = {1, 2, (int64_t)L}, magdims[4] = {1, 4, BINS_, (int64_t)F};
    unsigned char dg[32];
    plat_sha256_ctx c;
    if (plat_sha256_init(&c)) return -1;
    plat_sha256_update(&c, FORMAT, sizeof FORMAT);
    plat_sha256_update(&c, model, 64);
    plat_sha256_update(&c, mixdims, sizeof mixdims);
    plat_sha256_update(&c, magdims, sizeof magdims);
    plat_sha256_update(&c, mix, 2 * L * sizeof(float));
    if (plat_sha256_final(&c, dg)) return -1;
    hex(dg, 32, key);
    return 0;
}

void mean_std(const float *mix, size_t count, double *mean, double *std) {   /* the bridge's, on an array */
    double s = 0, ss = 0;
    for (size_t i = 0; i < count; i++) s += mix[i];
    *mean = s / count;
    for (size_t i = 0; i < count; i++) { double d = mix[i] - *mean; ss += d * d; }
    *std = sqrt(ss / (count - 1));                          /* unbiased, like torch.std */
}

/* our model (x all zero): (xt * std + mean) at half level, 16-bit, clamped; none when xt isn't
 * finite (the bridge doesn't cache that) */
int ours_pcm(const float *xt, const float *mix, size_t L, int16_t *pcm, const char **why) {
    double mean, std;
    mean_std(mix, 2 * L, &mean, &std);
    for (size_t i = 0; i < 8 * L; i++)
        if (!isfinite(xt[i])) { *why = "the model's stems aren't finite"; return 1; }
    for (size_t ch = 0; ch < 8; ch++)
        for (size_t i = 0; i < L; i++) {
            double v = (xt[ch * L + i] * std + mean) * 0.5 * 32767.0;
            pcm[i * 8 + ch] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : lrint(v));
        }
    return 0;
}

/* rekordbox's model: the bridge's rebuilt_pcm, rebuild_spec(x) + xt * std + mean; none where the
 * bridge refuses (not finite, beyond the cache's range) */
int rebuilt_pcm(const float *x, size_t F, const float *xt, const float *mix, size_t L, int16_t *pcm, const char **why) {
    double mean, std;
    mean_std(mix, 2 * L, &mean, &std);
    if (F != (L + HOP - 1) / HOP) { *why = "unexpected shapes for a rebuilt model"; return 1; }
    float *s = malloc(sizeof(float) * 8 * L);
    if (!s || rebuild_spec(x, L, F, s)) { free(s); *why = "the rebuild failed"; return 1; }
    size_t clamped = 0;
    int finite = 1;
    for (size_t ch = 0; ch < 8 && finite; ch++)
        for (size_t i = 0; i < L; i++) {
            double v = ((double)s[ch * L + i] + xt[ch * L + i] * std + mean) * 0.5 * 32767.0;
            if (!isfinite(v)) { finite = 0; break; }
            if (v > 32767 || v < -32767) { clamped++; v = v > 0 ? 32767 : -32767; }
            pcm[i * 8 + ch] = (int16_t)lrint(v);
        }
    free(s);
    if (!finite) { *why = "rebuilt stems aren't finite"; return 1; }
    if (clamped) { *why = "rebuilt samples beyond the cache's range"; return 1; }
    return 0;
}

/* <key>.flac in the folder x, through a temporary file renamed when complete (the bridge's
 * write_flac, plus the helper's version tag); -1 with *err */
static int write_flac(plat_dir x, const char *key, const int16_t *pcm, size_t frames, const char *helper, int *err) {
    char name[80], tmp[96];
    plat_file fd;
    snprintf(name, sizeof name, "%s.flac", key);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", name, plat_pid());
    plat_unlink_at(x, tmp);
    FILE *f = NULL;
    if ((*err = plat_file_create_at(x, tmp, 1, &fd))) return -1;
    if (!(f = plat_file_stdio(fd, "w+b"))) { *err = errno; plat_file_close(fd); plat_unlink_at(x, tmp); return -1; }
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    if (!enc) { fclose(f); plat_unlink_at(x, tmp); *err = ENOMEM; return -1; }
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamMetadata_VorbisComment_Entry ent;
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&ent, "RBSTEMS_KEY", key);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, ent, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&ent, "RBSTEMS_FORMAT", FORMAT " drums,bass,other,vocals L/R half-level");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, ent, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&ent, "RBSTEMS_HELPER", helper);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, ent, false);
    FLAC__stream_encoder_set_channels(enc, 8);
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, 44100);
    FLAC__stream_encoder_set_compression_level(enc, 5);
    FLAC__stream_encoder_set_total_samples_estimate(enc, frames);
    FLAC__stream_encoder_set_metadata(enc, &vc, 1);
    errno = 0;
    int ok = FLAC__stream_encoder_init_FILE(enc, f, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (ok) {
        enum { BLOCK = 4096 };
        static __thread FLAC__int32 buf[BLOCK * 8];
        for (size_t i = 0; ok && i < frames; i += BLOCK) {
            size_t n = frames - i < BLOCK ? frames - i : BLOCK;
            for (size_t j = 0; j < n * 8; j++) buf[j] = pcm[i * 8 + j];
            ok = FLAC__stream_encoder_process_interleaved(enc, buf, (unsigned)n);
        }
        ok = FLAC__stream_encoder_finish(enc) && ok;
    }
    *err = ok ? 0 : errno ? errno : EIO;
    FLAC__stream_encoder_delete(enc);
    FLAC__metadata_object_delete(vc);
    if (ok && (*err = plat_rename_at(x, tmp, x, name)) != 0) ok = 0;
    if (ok) return 0;
    plat_unlink_at(x, tmp);
    return -1;
}

static int write_desc(plat_dir x, const char *key, const float *d, int frames, float r0, float g0, const float *rt,
                      const float *rf, int *err) {           /* the bridge's write_desc */
    char name[80], tmp[96];
    plat_file fd;
    snprintf(name, sizeof name, "%s.desc", key);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", name, plat_pid());
    plat_unlink_at(x, tmp);
    FILE *f = NULL;
    if ((*err = plat_file_create_at(x, tmp, 0, &fd))) return -1;
    if (!(f = plat_file_stdio(fd, "wb"))) { *err = errno; plat_file_close(fd); plat_unlink_at(x, tmp); return -1; }
    size_t n = 2 * (size_t)frames;
    int ok = fwrite("RBD2", 1, 4, f) == 4 && fwrite(&frames, 4, 1, f) == 1 && fwrite(&r0, 4, 1, f) == 1
             && fwrite(&g0, 4, 1, f) == 1 && fwrite(d, sizeof(float), n, f) == n
             && fwrite(rt, sizeof(float), 2, f) == 2 && fwrite(rf, sizeof(float), n, f) == n;
    if (!ok) *err = errno;
    if (fclose(f) != 0 && ok) { ok = 0; *err = errno; }
    if (ok && (*err = plat_rename_at(x, tmp, x, name)) != 0) ok = 0;
    if (!ok) { plat_unlink_at(x, tmp); return -1; }
    return 0;
}

/* the bridge's store_entry, without its location and space checks (the helper's own): 0 written,
 * 1 refused (*why), -1 a write failed (*err) */
int store(plat_dir x, const char *key, const int16_t *pcm, const float *mix, size_t L, int rebuilt, int chosen,
          const char *helper, const char **why, int *err) {
    int frames = 0;
    float *d = malloc(sizeof(float) * 2 * DESC_MAX_FRAMES), *s = malloc(L * sizeof(float)), *m = malloc(L * sizeof(float));
    float *rf = malloc(sizeof(float) * 2 * DESC_MAX_FRAMES), rt[2];
    double r = 0, g = 0;
    int rc = 1;
    *why = NULL;
    if (!d || !s || !m || !rf) { *why = "out of memory"; goto done; }
    frames = summarize(mix, L, d);
    double sse = 0, mme = 0;
    monos(pcm, mix, L, s, m);
    for (size_t i = 0; i < L; i++) { sse += (double)s[i] * s[i]; mme += (double)m[i] * m[i]; }
    corr_at(s, m, L, 0, sse, mme, &r, &g);
    residual(pcm, mix, L, 0, frames, rf, rt, NULL);
    if (!(isfinite(r) && isfinite(g) && all_finite(d, 2 * (size_t)frames) && all_finite(rf, 2 * (size_t)frames))) {
        *why = "its summary isn't finite";
        goto done;
    }
    double me = 0;
    for (size_t i = 0; i < 2 * L; i++) me += (double)mix[i] * mix[i];
    if (rebuilt && me / (2.0 * L) >= FRAME_FLOOR && !(rt[0] <= REBUILT_RESIDUAL_MAX && rt[1] <= REBUILT_RESIDUAL_MAX)) {
        *why = "rebuilt stems don't add up to the chunk";
        goto done;
    }
    if (chosen && me / (2.0 * L) >= FRAME_FLOOR && !(rt[0] <= EXACT_RESIDUAL_MAX && rt[1] <= EXACT_RESIDUAL_MAX)) {
        *why = "its stems don't add up to the chunk (the bar on another drive)";
        goto done;
    }
    rc = write_flac(x, key, pcm, L, helper, err) == 0 && write_desc(x, key, d, frames, (float)r, (float)g, rt, rf, err) == 0 ? 0 : -1;
done:
    free(d); free(s); free(m); free(rf);
    return rc;
}
