#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "aac_mf.h"
#include "big.h"
#include "rbsp.h"
#include "pcm.h"
#include "plat_helper.h"

static const char *replacement;
static int opens, resample_rate, resample_ok, candidate_resamples, alac_decodes;
static plat_finfo metadata;

int alac_decode(plat_file f, int64_t size, const pcm_info *p, pcm_out *o, char *why, size_t whyn) {
    (void)f, (void)size;
    alac_decodes++;
    size_t n = (size_t)p->frames;
    float *s = big_calloc(n * 2, sizeof *s);
    if (!s) { snprintf(why, whyn, "out of memory"); return 1; }
    *o = (pcm_out){.s = s, .ch = p->ch, .n = n, .cap = n, .got = n};
    return 0;
}

/* Swap the directory entry as soon as the first open pins the original file. */
int rate_test_open(const char *path, plat_file *f) {
    int rc = plat_file_open_path(path, f);
    if (!rc && ++opens == 1 && rename(replacement, path)) {
        perror("replace track");
        plat_file_close(*f);
        return 1;
    }
    return rc;
}

int plat_finfo_file(plat_file f, plat_finfo *fi) {
    struct stat st;
    if (fstat(f.fd, &st)) return errno;
    *fi = (plat_finfo){PLAT_FILE, st.st_size, st.st_mtimespec.tv_sec * INT64_C(1000000000) + st.st_mtimespec.tv_nsec};
    metadata = *fi;
    return 0;
}

/* resample_win.c's list (helper/tests/resample-win.c checks the real one) */
int rb_resample_proved(int rate) {
    return rate == 16000 || rate == 22050 || rate == 24000 || rate == 32000 ||
           rate == 48000 || rate == 88200 || rate == 96000 || rate == 192000;
}
int rb_resample_build_proved;
#ifdef RBSP_TEST_FORMAT_RATES
int rb_aac_test_unproved;
int rb_rates_test_unproved;
#endif
static int table_ok = 1;
int rb_resample_table_ok(int rate) { return rb_resample_proved(rate) && table_ok; }

/* The Source Reader's negotiated PCM format, or its failure before any samples are read. */
static const char mf_missing[] = "Media Foundation isn't there (a Windows N edition without the Media Feature Pack?)";
static int mf_rate = 22050, mf_asked, mf_decodes, mf_decode_ch = 2;
static size_t mf_frames = 50000;
int aac_mf_available(char *why, size_t whyn) {
    if (mf_rate) return 1;
    snprintf(why, whyn, "%s", mf_missing);
    return 0;
}

int aac_mf_probe(plat_file f, const char *ext, aac_mf_info *info, char *why, size_t whyn) {
    (void)f; (void)ext;
    mf_asked++;
    if (!mf_rate) { snprintf(why, whyn, "%s", mf_missing); return 1; }
    *info = (aac_mf_info){.native_sub = 0x1610, .rate = mf_rate, .ch = mf_decode_ch, .bits = 16};
    return 0;
}

int aac_decode_all(plat_file f, const char *ext, int stream, aac_outputs *o, char *why, size_t whyn) {
    (void)f; (void)ext; (void)stream; (void)why; (void)whyn;
    mf_decodes++;
    int16_t *pcm = big_alloc(mf_frames * (size_t)mf_decode_ch * sizeof *pcm);
    if (!pcm) return 1;
    memset(pcm, 0, mf_frames * (size_t)mf_decode_ch * sizeof *pcm);
    pcm[0] = -32768;
    pcm[1] = 32767;
    pcm[2] = 1;
    pcm[3] = -1;
    *o = (aac_outputs){.pcm = {pcm}, .frames = {mf_frames}, .count = 1, .rate = mf_rate, .ch = mf_decode_ch};
    return 0;
}

void aac_outputs_free(aac_outputs *o) { big_free(o->pcm[0]); }

int rb_resample(const float *in, size_t n, int rate, float **out, size_t *nout) {
    resample_rate = rate;
    if (!resample_ok || !(*out = big_alloc(2 * n * sizeof **out))) return 1;
    memcpy(*out, in, 2 * n * sizeof **out);
    *nout = n;
    return 0;
}

#ifdef RBSP_TEST_FORMAT_RATES
int rb_resample_win_any(const float *in, size_t n, int rate, float **out, size_t *nout) {
    candidate_resamples++;
    return rb_resample(in, n, rate, out, nout);
}
#endif

/* the estimate's probe and the run refuse PATH alike, with LINE, before the resampler: 0 */
static int refused_alike(const char *name, const char *path, const char *line) {
    resample_rate = 0;
    mf_decodes = alac_decodes = 0;
    double sec = 0;
    int aac = 0;
    char pwhy[400] = "unwritten", why[400] = "unwritten";
    tracks ts;
    int prc = probe_track(path, &sec, &aac, pwhy, sizeof pwhy);
    int rc = load_tracks(path, "", &ts, why, sizeof why);
    tracks_free(&ts);
    if (!prc || !rc || resample_rate || mf_decodes || alac_decodes || strcmp(pwhy, why) || strcmp(why, line)) {
        fprintf(stderr, "FAIL %s: probe %d (%s), run %d (%s), %d AAC and %d ALAC decodes, resampler rate %d Hz\n",
                name, prc, pwhy, rc, why, mf_decodes, alac_decodes, resample_rate);
        return 1;
    }
    return 0;
}

static int accepted_aac(const char *name, const char *path, int candidate) {
    resample_rate = 0;
    mf_decodes = 0;
    candidate_resamples = 0;
    double sec = 0;
    int aac = 0;
    char pwhy[400] = "", why[400] = "";
    tracks ts;
    int prc = probe_track(path, &sec, &aac, pwhy, sizeof pwhy);
    int rc = load_tracks(path, "", &ts, why, sizeof why);
    const track *t = &ts.t[0];
    int mono = mf_decode_ch == 1;
    int ok = !prc && aac && !rc && mf_decodes == 1 && resample_rate == mf_rate && ts.source_rate == mf_rate &&
             candidate_resamples == candidate &&
             ts.count == 1 && t->s && t->ch == 2 && t->n >= 2 && !t->p16 &&
             lround(sec * 44100) == (long)rb_resample_out_frames(t->n, mf_rate) &&
             t->s[0] == -1.0f && t->s[1] == (mono ? 32767.0f : 1.0f) / 32768 &&
             t->s[t->n] == (mono ? -1.0f : 32767.0f / 32768) &&
             t->s[t->n + 1] == (mono ? 32767.0f : -1.0f) / 32768;
    if (!ok) fprintf(stderr, "FAIL %s: probe %d (%s), run %d (%s), %d decodes, %d candidate conversions at %d Hz, or wrong PCM\n",
                     name, prc, pwhy, rc, why, mf_decodes, candidate_resamples, resample_rate);
    tracks_free(&ts);
    return !ok;
}

static int refused_without_proof(const char *name, const char *path, int rate) {
    char line[400];
    rb_resample_build_proved = 0;
    snprintf(line, sizeof line,
             "%d Hz: this rekordbox version's resampler isn't proved yet, so RB Stems Plus doesn't prepare files it resamples", rate);
    int failed = refused_alike(name, path, line);
    rb_resample_build_proved = 1;

    table_ok = 0;
    snprintf(line, sizeof line, "%d Hz: the resampler's filter built on this PC isn't the proved one", rate);
    failed += refused_alike(name, path, line);
    table_ok = 1;
    return failed;
}

static int mp3_test_proved(const char *dir) {
    const char *const modes[] = {"cbr", "vbr"};
    int failed = 0;
    for (int ch = 1; ch <= 2; ch++) {
        for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
            char path[4096], name[80], why[400] = "";
            snprintf(name, sizeof name, "32 kHz MP3 %s %dch", modes[i], ch);
            snprintf(path, sizeof path, "%s/mp3-%s-%dch.mp3", dir, modes[i], ch);
            failed += refused_without_proof(name, path, 32000);

            double sec = 0;
            int aac = -1;
            tracks ts;
            candidate_resamples = resample_rate = 0;
            int prc = probe_track(path, &sec, &aac, why, sizeof why);
            int rc = load_tracks(path, dir, &ts, why, sizeof why);
            const track *t = &ts.t[0];
            int ok = !prc && !aac && !rc && ts.source_rate == 32000 && resample_rate == 32000 &&
                     candidate_resamples == 0 && ts.count == 1 && t->s && t->ch == 2 && t->n > 32000 &&
                     lround(sec * 44100) == (long)rb_resample_out_frames(t->n, 32000) &&
                     (!memcmp(t->s, t->s + t->n, t->n * sizeof *t->s) == (ch == 1));
            if (!ok) fprintf(stderr, "FAIL %s: probe %d, run %d (%s), %d candidate conversions at %d Hz\n",
                             name, prc, rc, why, candidate_resamples, resample_rate);
            failed += !ok;
            tracks_free(&ts);
        }
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/pcm32.wav", dir);
    return failed + refused_alike("32 kHz WAV", path,
        "32000 Hz: RB Stems Plus doesn't prepare files rekordbox resamples from this rate on Windows yet");
}

static int aac_test_proved(const char *dir) {
    static const struct { int rate, ch; const char *file; } cases[] = {
        {32000, 2, "aac32.m4a"}, {48000, 1, "aac48mono.m4a"}
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char path[4096], name[80];
        mf_rate = cases[i].rate;
        mf_decode_ch = cases[i].ch;
        snprintf(path, sizeof path, "%s/%s", dir, cases[i].file);
        snprintf(name, sizeof name, "AAC-LC %d Hz %d ch", mf_rate, mf_decode_ch);
        failed += refused_without_proof(name, path, mf_rate);
        failed += accepted_aac(name, path, 0);
    }
    mf_rate = 48000;
    mf_decode_ch = 2;
    return failed;
}

static int aac48_decode_refused(const char *name, const char *path, const char *line) {
    char why[400] = "";
    tracks ts;
    mf_decodes = 0;
    resample_rate = 0;
    int rc = load_tracks(path, "", &ts, why, sizeof why);
    tracks_free(&ts);
    if (rc && mf_decodes == 1 && !resample_rate && !strcmp(why, line)) return 0;
    fprintf(stderr, "FAIL %s: run %d (%s), %d decodes, resampler rate %d Hz\n", name, rc, why, mf_decodes, resample_rate);
    return 1;
}

enum rate_case { RATE_REFUSED, RATE_RESEARCH, RATE_PROVED };

static int accepted_rate(const char *dir, const char *name, const char *path, int rate, int aac, int candidate) {
    if (aac) return accepted_aac(name, path, candidate);
    char why[400] = "";
    double sec = 0;
    int is_aac = -1;
    tracks ts;
    candidate_resamples = resample_rate = 0;
    int prc = probe_track(path, &sec, &is_aac, why, sizeof why);
    int rc = load_tracks(path, dir, &ts, why, sizeof why);
    int ok = !prc && sec > 0 && !is_aac && !rc && ts.source_rate == rate && resample_rate == rate &&
             candidate_resamples == candidate && ts.count == 1 && ts.t[0].s && ts.t[0].ch == 2 && ts.t[0].n;
    if (!ok)
        fprintf(stderr, "FAIL %s: probe %d, run %d (%s), %d candidate conversions (expected %d) at %d Hz\n",
                name, prc, rc, why, candidate_resamples, candidate, resample_rate);
    tracks_free(&ts);
    return !ok;
}

static int test_rate(const char *dir, int rate, int ch, const char *suffix, int aac, enum rate_case expected) {
    char path[4096], name[80], line[160];
    snprintf(name, sizeof name, "%d Hz %d ch%s", rate, ch, suffix);
    snprintf(path, sizeof path, "%s/rate-%d-%d%s", dir, rate, ch, suffix);
    snprintf(line, sizeof line, "%d Hz: RB Stems Plus doesn't prepare files rekordbox resamples from this rate on Windows yet", rate);
    mf_rate = rate;
    mf_decode_ch = ch;
    int failed;
    if (expected == RATE_PROVED) {
        failed = refused_without_proof(name, path, rate);
        failed += accepted_rate(dir, name, path, rate, aac, 0);
    } else {
        failed = refused_alike(name, path, line);
    }
#ifdef RBSP_TEST_FORMAT_RATES
    rb_rates_test_unproved = 1;
    if (expected == RATE_REFUSED) {
        failed += refused_alike(name, path, line);
    } else {
        if (expected == RATE_PROVED) failed += refused_without_proof(name, path, rate);
        failed += accepted_rate(dir, name, path, rate, aac, expected == RATE_RESEARCH);
    }
    rb_rates_test_unproved = 0;
#endif
    return failed;
}

static int measured_rates(const char *dir) {
    const int lossless_rates[] = {88200, 192000}, mp3_rates[] = {16000, 24000};
    const int aac_rates[] = {16000, 22050, 24000, 32000};
    const char *const lossless[] = {".wav", ".flac", ".fla", ".aif", ".aiff", ".m4a", "-alac24.m4a"};
    const char *const mp3[] = {"-cbr.mp3", "-vbr.mp3"}, *const aac[] = {"-aac.m4a", "-aac.mp4"};
    int failed = 0;
    for (int ch = 1; ch <= 2; ch++) {
        for (size_t i = 0; i < sizeof lossless_rates / sizeof *lossless_rates; i++) {
            for (size_t j = 0; j < sizeof lossless / sizeof *lossless; j++)
                failed += test_rate(dir, lossless_rates[i], ch, lossless[j], 0, RATE_PROVED);
            failed += test_rate(dir, lossless_rates[i], ch, ".mp4", 0, RATE_RESEARCH);
        }
        for (size_t i = 0; i < sizeof mp3_rates / sizeof *mp3_rates; i++)
            for (size_t j = 0; j < sizeof mp3 / sizeof *mp3; j++)
                failed += test_rate(dir, mp3_rates[i], ch, mp3[j], 0, RATE_PROVED);
        for (size_t i = 0; i < sizeof aac_rates / sizeof *aac_rates; i++)
            for (size_t j = 0; j < sizeof aac / sizeof *aac; j++) {
                int rate = aac_rates[i];
                if (rate == 32000 && ch == 2) continue;  /* already proved */
                enum rate_case expected = RATE_PROVED;
                if (ch == 1 && rate != 32000) expected = RATE_RESEARCH;
                /* 22.05 kHz stereo: proved 2026-11-22 (frame-0 type change) */
                failed += test_rate(dir, rate, ch, aac[j], 1, expected);
            }
    }
    failed += test_rate(dir, 88200, 3, ".wav", 0, RATE_REFUSED);
    failed += test_rate(dir, 192000, 3, ".wav", 0, RATE_REFUSED);
    failed += test_rate(dir, 88200, 1, "-wav.flac", 0, RATE_REFUSED);
    failed += test_rate(dir, 16000, 1, ".wav", 0, RATE_REFUSED);
    failed += test_rate(dir, 24000, 1, ".wav", 0, RATE_REFUSED);
    failed += test_rate(dir, 16000, 2, "-aac.wav", 1, RATE_REFUSED);
    failed += test_rate(dir, 16000, 2, "-main.m4a", 1, RATE_REFUSED);
    failed += test_rate(dir, 24000, 1, "-layer2.mp3", 0, RATE_REFUSED);
    /* decode.c's win_cells at 22.05, 48 and 96 kHz: each proved cell, and one refusal per
     * dimension the cells name (extension, encoding, layer, channels) */
    const char *const cell_exts[] = {".wav", ".flac", ".fla", ".aif", ".aiff", ".m4a"};
    for (int ch = 1; ch <= 2; ch++) {
        for (size_t j = 0; j < sizeof cell_exts / sizeof *cell_exts; j++) {
            failed += test_rate(dir, 48000, ch, cell_exts[j], 0, RATE_PROVED);
            failed += test_rate(dir, 96000, ch, cell_exts[j], 0, RATE_PROVED);
            /* the PC's 38 rows, 2026-11-23 (RB_WIN_LOSSLESS_22050_PROVED) */
            failed += test_rate(dir, 22050, ch, cell_exts[j], 0, RATE_PROVED);
        }
        failed += test_rate(dir, 48000, ch, ".mp4", 0, RATE_REFUSED);       /* ALAC: .m4a only */
        failed += test_rate(dir, 96000, ch, ".mp4", 0, RATE_REFUSED);
        failed += test_rate(dir, 22050, ch, ".mp4", 0, RATE_REFUSED);
        failed += test_rate(dir, 22050, ch, "-cbr.mp3", 0, RATE_PROVED);
        failed += test_rate(dir, 48000, ch, "-cbr.mp3", 0, RATE_PROVED);
    }
    failed += test_rate(dir, 22050, 1, "-layer2.mp3", 0, RATE_REFUSED);     /* Layer III only */
    failed += test_rate(dir, 32000, 1, "-layer2.mp3", 0, RATE_REFUSED);
    failed += test_rate(dir, 48000, 1, "-layer2.mp3", 0, RATE_REFUSED);
    failed += test_rate(dir, 48000, 2, "-aac.wav", 1, RATE_REFUSED);        /* AAC: .m4a and .mp4 only */
    failed += test_rate(dir, 22050, 2, "-aac.flac", 1, RATE_REFUSED);
    failed += test_rate(dir, 48000, 2, "-s32.wav", 0, RATE_REFUSED);        /* the proved encodings only */
    failed += test_rate(dir, 48000, 2, "-f32.wav", 0, RATE_REFUSED);
    failed += test_rate(dir, 96000, 2, "-f32.wav", 0, RATE_PROVED);         /* 15-wav-f32-96k */
    failed += test_rate(dir, 96000, 2, "-u8.wav", 0, RATE_REFUSED);
    failed += test_rate(dir, 48000, 2, "-sowt.aiff", 0, RATE_REFUSED);
    failed += test_rate(dir, 48000, 6, ".wav", 0, RATE_PROVED);             /* w09-wav-5.1 */
    failed += test_rate(dir, 48000, 6, ".flac", 0, RATE_PROVED);            /* w08-flac-5.1 */
    failed += test_rate(dir, 48000, 6, ".fla", 0, RATE_REFUSED);
    failed += test_rate(dir, 48000, 3, ".wav", 0, RATE_REFUSED);
    mf_rate = 48000;
    mf_decode_ch = 2;
    if (!failed) puts("rate-host measured rates: proved rows use checked tables, refused rows stay gated, and remaining candidates require the switch");
    return failed;
}

static int fla_alias(const char *dir, int bits) {
    char path[4096], why[400] = "";
    tracks flac, fla;
    double seconds = 0;
    int aac = 0;
    snprintf(path, sizeof path, "%s/flac-%d.flac", dir, bits);
    int ref = load_tracks(path, "", &flac, why, sizeof why);
    snprintf(path, sizeof path, "%s/flac-%d.FLA", dir, bits);
    int probe = probe_track(path, &seconds, &aac, why, sizeof why);
    int run = load_tracks(path, "", &fla, why, sizeof why);
    int ok = !ref && !probe && !run && seconds == 1.0 && !aac && fla.count == 1 &&
             fla.t[0].n == 44100 && flac.t[0].n == fla.t[0].n &&
             !memcmp(flac.t[0].s, fla.t[0].s, 2 * fla.t[0].n * sizeof(float));
    if (!ok) fprintf(stderr, "FAIL %d-bit FLA: probe %d, run %d (%s)\n", bits, probe, run, why);
    tracks_free(&flac);
    tracks_free(&fla);
    return !ok;
}

int main(int argc, char **argv) {
    if (argc != 9) return 2;
    replacement = argv[2];
    struct stat st;
    if (stat(argv[1], &st)) { perror("stat original track"); return 2; }
    char why[400] = "";
    tracks ts;
    int rc = load_tracks(argv[1], "", &ts, why, sizeof why);
    int original = !rc && ts.count == 1 && ts.t[0].n == 4 && ts.t[0].s && ts.t[0].s[0] == 0.25f;
    tracks_free(&ts);
    if (opens != 1 || resample_rate || !original) {
        fprintf(stderr, "FAIL rate swap: %d opens, resampler rate %d Hz, original decoded %d (%s)\n",
                opens, resample_rate, original, why);
        return 1;
    }
    if (metadata.kind != PLAT_FILE || metadata.size != st.st_size ||
        metadata.mtime_ns != st.st_mtimespec.tv_sec * INT64_C(1000000000) + st.st_mtimespec.tv_nsec) {
        fprintf(stderr, "FAIL rate swap: manifest metadata differs from the original track\n");
        return 1;
    }

    rc = load_tracks(argv[1], "", &ts, why, sizeof why);
    tracks_free(&ts);
    if (!rc || resample_rate || !strstr(why, "176400 Hz")) {
        fprintf(stderr, "FAIL 176.4 kHz input: returned %d, resampler rate %d Hz (%s)\n", rc, resample_rate, why);
        return 1;
    }

    /* a proved rate on a rekordbox build that isn't proved (no --resample-proved): refused */
    rc = load_tracks(argv[3], "", &ts, why, sizeof why);
    tracks_free(&ts);
    if (!rc || resample_rate || !strstr(why, "48000 Hz: this rekordbox version's resampler isn't proved")) {
        fprintf(stderr, "FAIL 48 kHz input, build not proved: returned %d, resampler rate %d Hz (%s)\n", rc, resample_rate, why);
        return 1;
    }

    /* a proved rate on a proved build goes on to the resampler (this stub refuses it) */
    rb_resample_build_proved = 1;
    rc = load_tracks(argv[3], "", &ts, why, sizeof why);
    tracks_free(&ts);
    if (!rc || resample_rate != 48000 || !strstr(why, "48000 Hz: rekordbox's resampler")) {
        fprintf(stderr, "FAIL 48 kHz input: returned %d, resampler rate %d Hz (%s)\n", rc, resample_rate, why);
        return 1;
    }
    resample_ok = 1;
    rc = load_tracks(argv[3], "", &ts, why, sizeof why);
    if (rc || ts.source_rate != 48000) {
        fprintf(stderr, "FAIL resampled track records source rate: got %d, expected 48000\n", ts.source_rate);
        tracks_free(&ts);
        return 1;
    }
    tracks_free(&ts);

    /* 48 kHz stereo LC, with and without the optional no-SBR sync extension. */
    mf_rate = 48000;
    char lc[4096];
    snprintf(lc, sizeof lc, "%s/lc.m4a", argv[8]);
    int failed = accepted_aac("48 kHz AAC-LC on a proved build", lc, 0);
    failed += accepted_aac("48 kHz AAC-LC with a no-SBR sync extension", argv[4], 0);
    rb_resample_build_proved = 0;
    failed += refused_alike("48 kHz AAC-LC on an unproved build", lc,
                            "48000 Hz: this rekordbox version's resampler isn't proved yet, so RB Stems Plus doesn't prepare files it resamples");
    rb_resample_build_proved = 1;
    static const char unsupported48[] = "48000 Hz: RB Stems Plus doesn't prepare files rekordbox resamples from this rate on Windows yet";
    const char *restricted[] = {"he", "sbr", "main", "unknown", "multi-description"};
    for (size_t i = 0; i < sizeof restricted / sizeof *restricted; i++) {
        char path[4096], name[100];
        snprintf(path, sizeof path, "%s/%s.m4a", argv[8], restricted[i]);
        snprintf(name, sizeof name, "48 kHz AAC (%s)", restricted[i]);
        failed += refused_alike(name, path, unsupported48);
    }
    mf_decode_ch = 1;
    failed += aac48_decode_refused("48 kHz AAC decoded as mono", lc,
                                   "48000 Hz after decoding: RB Stems Plus doesn't prepare files rekordbox resamples from this rate on Windows yet");
    mf_decode_ch = 2;
    mf_frames = 48127;
    failed += aac48_decode_refused("48 kHz AAC with an incomplete decode", lc,
                                   "incomplete AAC decode: Media Foundation returned 48127 frames, the sample table says 48128");
    mf_frames = 50000;
#ifdef RBSP_TEST_FORMAT_RATES
    rb_aac_test_unproved = rb_mp3_test_unproved = 1;
#endif
    failed += aac_test_proved(argv[8]);
    failed += mp3_test_proved(argv[8]);
#ifdef RBSP_TEST_FORMAT_RATES
    rb_aac_test_unproved = rb_mp3_test_unproved = 0;
#endif
    failed += measured_rates(argv[8]);
    if (failed) return 1;
    /* a 22.05 kHz AAC header: Media Foundation's output rate decides. AAC-LC (it gives 22.05 kHz):
     * proved for stereo on 2026-11-22, so the probe and the run accept it alike, both having asked */
    mf_rate = 22050;
    mf_asked = 0;
    if (accepted_aac("22.05 kHz AAC-LC", argv[5], 0))
        return 1;
    if (mf_asked != 2) { fprintf(stderr, "FAIL 22.05 kHz AAC-LC: Media Foundation asked %d times (2 expected)\n", mf_asked); return 1; }
#ifdef RBSP_TEST_FORMAT_RATES
    rb_aac_test_unproved = 1;
    if (accepted_aac("22.05 kHz AAC-LC with the AAC diagnostic switch", argv[5], 0)) return 1;
    rb_aac_test_unproved = 0;
#endif
    /* implicit-SBR HE-AAC (it gives 44.1 kHz): the probe and the run take it at 44.1 kHz */
    mf_rate = 44100;
    {
        double hs = 0;
        int ha = 0;
        char hwhy[400] = "";
        int hp = probe_track(argv[5], &hs, &ha, hwhy, sizeof hwhy);
        rc = load_tracks(argv[5], "", &ts, why, sizeof why);
        if (!rc) tracks_free(&ts);
        if (hp || !ha || rc) {
            fprintf(stderr, "FAIL 22.05 kHz HE-AAC: probe %d (%s), run %d (%s)\n", hp, hwhy, rc, why);
            return 1;
        }
    }

    mf_rate = 0;
    mf_asked = 0;
    if (refused_alike("44.1 kHz AAC on Windows N", argv[7], mf_missing) ||
        refused_alike("22.05 kHz AAC on Windows N", argv[5], mf_missing)) return 1;
    mf_rate = 44100;
    double ordinary_sec = 0;
    int ordinary_aac = 0;
    if (probe_track(argv[7], &ordinary_sec, &ordinary_aac, why, sizeof why) || !ordinary_aac || ordinary_sec <= 0 || mf_asked) {
        fprintf(stderr, "FAIL ordinary AAC estimate or Windows N refusal opened %d MF readers\n", mf_asked);
        return 1;
    }

    /* the estimate's length at 48 kHz: 524,657 frames give the converter's 482,029 (2 chunks),
     * not seconds x 44100 truncated (482,028, 1 chunk) */
    double sec = 0;
    int aac = 0;
    if (probe_track(argv[6], &sec, &aac, why, sizeof why) || fabs(sec * 44100 - 482029) > 1e-6 ||
        rb_resample_out_frames(524657, 48000) != 482029 || n_chunks((size_t)round(sec * 44100)) != 2) {
        fprintf(stderr, "FAIL 48 kHz length: %.9f s, %zu frames, %zu chunks (%s)\n", sec, (size_t)round(sec * 44100),
                n_chunks((size_t)round(sec * 44100)), why);
        return 1;
    }

    /* a table built on this PC that isn't the proved one: the probe and the run refuse alike */
    table_ok = 0;
    if (refused_alike("48 kHz, a table that isn't the proved one", argv[3], "48000 Hz: the resampler's filter built on this PC isn't the proved one"))
        return 1;
    table_ok = 1;
    int fla_failed = fla_alias(argv[8], 16) + fla_alias(argv[8], 24);
    if (fla_failed) return 1;
    char path[4096];
    snprintf(path, sizeof path, "%s/alac20.m4a", argv[8]);
    failed = refused_alike("20-bit ALAC", path, "20-bit ALAC: RB Stems Plus doesn't prepare this rekordbox format on Windows yet");
    const int alac_rates[] = {44100, 48000};
    for (size_t i = 0; i < sizeof alac_rates / sizeof *alac_rates; i++) {
        int rate = alac_rates[i];
        for (int ch = 1; ch <= 2; ch++) {
            char name[80];
            snprintf(name, sizeof name, "32-bit ALAC at %d Hz, %d ch", rate, ch);
            snprintf(path, sizeof path, "%s/alac32-%d-%d.m4a", argv[8], rate, ch);
            failed += refused_alike(name, path, "32-bit ALAC: RB Stems Plus doesn't prepare this rekordbox format on Windows yet");
        }
    }
    if (failed) return 1;
    puts("rate-host: PASS (four 32 kHz MP3 fixtures and two AAC-LC promotions use checked tables on proved builds, 32 kHz WAV refused, 20/32-bit ALAC skipped with the generic reason, 16/24-bit FLA identical to FLAC, one open, original metadata, 176.4 kHz refused without resampling, 48 kHz refused on an unproved build, "
         "48 kHz stereo AAC-LC resampled on a proved build only, unsupported AAC configurations and decodes refused, "
         "22.05 kHz AAC-LC refused alike by the estimate and the run, implicit-SBR HE-AAC taken at 44.1 kHz, "
         "ordinary AAC estimated without a reader, 44.1 and 22.05 kHz AAC refused with MF missing, resampled track records its source rate, the 48 kHz estimate's exact length, "
         "a mismatching table refused alike)");
    return 0;
}
