/* rekordbox for Windows' sample-rate converter (7.2.19, x64), for the Windows helper only
 * (helper.md §2, T11). The Mac's is resample.c; the Mac never builds this file.
 *
 * rekordbox.exe has two copies of rbxfrm's SamplingFunctionSRC<Float2>/c_src<Float2>: an SSE one
 * (0x14105xxxx: init 0x141058190, design 0x141059c10, convolution 0x14105bf90) and one compiled
 * for AVX2 (0x14112xxxx: init 0x141120410, design 0x141123180 with its Kaiser window 0x141123b00,
 * convolution 0x1411224a0). The STEMS path builds the AVX one unconditionally (0x141113438 ->
 * 0x142d3a470 -> ctor 0x1411202a0); rekordbox's code from 0x1411xxxxx up is AVX2 code throughout,
 * and its only CPU gate is JUCE's hasAVX (CPUID.1:ECX bit 28) for "This function is unavailable
 * on the computer currently being used. To use this function, an AVX ... CPU is required." So
 * every PC that separates stems runs the AVX copy, whose convolution uses FMA3.
 *
 * Proved bit for bit on the owner's PC (2026-11-19, the probe's dumps of the model's input):
 * 17-aiff-24-48k, all 1,984,499 frames of rekordbox's resampled audio equal this code's output
 * on the same decoded samples (helper/tests/resample-win.sh). The Mac's convolution (an FMA on
 * every tap) gives 78% of those frames, the SSE copy's (no FMA) 50%.
 *
 * What differs from the Mac's (resample.c):
 *  - the convolution: per pair from both ends, acc = fma(x[a], h[a], acc), then
 *    acc = acc + round(x[b] * h[b]); the middle tap fused. The FMA is done exactly in software
 *    (fma_f32 below), so the result doesn't depend on the CPU the helper runs on (an x64 CPU
 *    without FMA3, or an emulator that doesn't expose it) and is the hardware's bit for bit;
 *  - the design's rounding: fc = (1/D) * cutoff, h = sin(n * 2pi * fc) * ((1/pi) / n),
 *    window argument sqrt(fma(-t, t, 1)) * beta * 0.5 with t = fma(2i, 1/(N-1), -1), the Bessel
 *    sums fma(pow, 1/k!^2, s), proto = (s1 / s2) * h; the fused steps done exactly in software
 *    as well (fma_f64). Stored as float, 48 kHz's table is the Mac's, and no entry of any standard
 *    rate's table (8 to 192 kHz) comes within 3695 double ulps of a float rounding boundary: sin and
 *    pow results a few ulps off (the OS's UCRT here, MSVC's static copy in rekordbox, with or
 *    without its FMA3 path) give the same table.
 *
 * Rates: only the ones proved against rekordbox's own output are taken (rb_resample_proved, Q12).
 * Each proved rate's table has its sha256 compiled in; a table built here that differs (another
 * math library, another compiler) refuses the file instead of writing entries rekordbox never
 * looks up.
 *
 * Compiled with -ffp-contract=off: nothing is fused by the compiler (fma_f32 and fma_f64 do it). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rbsp.h"
#include "big.h"
#include "plat.h"                       /* plat_sha256 */
#ifdef RBSP_TEST_FORMAT_RATES
#include <stdio.h>
#endif

#define TAPS 29        /* taps per output sample (0x1d) */
#define HIST 28        /* zero samples in the history buffer after reset (0x1c) */

typedef struct { float l, r; } F2;

/* The proved Windows rates and the sha256 of their float tables (882 * 29 floats,
 * little-endian), as the Mac builds them. */
static const struct { int rate; const char *sha256; } proved[] = {
    /* 17-aiff-24-48k, 13-wav-24-48k, w14-flac-mono-48k (mono: the same lanes) */
    {48000, "73eea4fd8ea60c5e13fcea5f5752aa583d2b8883268a6a419624b7bf683cae5d"},
    /* 08-alac-24-96k, 11-flac-24-96k, 18-aiff-24-96k, 15-wav-f32-96k */
    {96000, "247bb73513993c2329a26ceafb725423b5b0aef28c9a41269027db9f92f3bc52"},
    /* w15-mp3-22k: all 1,324,856 frames, from the host's decode with 1,198 single-LSB input
     * differences (the host's libmpg123 against rekordbox's libmpg123.dll); the PC run with the
     * Windows decode confirms the row */
    {22050, "d8770bf0ade5e356008ef90cb647dbef41ebb2e96aeaccef812a21ac41c99cd5"},
    /* 7.2.20 Windows kit, 2026-11-21/22: MP3 CBR/VBR and AAC-LC, mono/stereo */
    {32000, "614892facab211cf698f1ab0f2742b76a1c7d365cbb9f3fd65d9b4b32acff79e"},
    /* 7.2.20 Windows kit, 2026-11-22: MP3 Layer III mono/stereo, AAC-LC stereo */
    {16000, "142c0e0dc184822b8ed75a2df05b9235c216ec676cc51a03204d493e28a17b90"},
    {24000, "1bff4703e09369c767d5aa3a09dd647b957d0ba41069029399697f9a9196f496"},
    /* 7.2.20 Windows kit, 2026-11-22: WAV, FLAC, AIFF and ALAC, mono/stereo */
    {88200, "02104404157d3fad0995928bd2e3aac9869e923b42531559e47c83cf7cc0cc3f"},
    {192000, "c33655fb521d6d30935edfa0647d957aeadaaf64a2d7d83d51a630579b958bd8"},
};

int rb_resample_build_proved;   /* --resample-proved (decode.c checks it, Q15) */

int rb_resample_proved(int rate) {
    for (size_t i = 0; i < sizeof proved / sizeof *proved; i++)
        if (proved[i].rate == rate) return 1;
    return 0;
}

/* SamplingFunctionSRC<Float2>::init(int in, int out) (0x141120410): cutoff and Kaiser beta */
static int pick_params(int in, int out, double *cutoff, double *beta)
{
    if (in - 8000u >= 0x2ee01u || out - 8000u >= 0x2ee01u) return 0; /* 8000..200000 */
    if (in == 44100 && out == 48000)       { *cutoff = 0.54; *beta = 8;  }
    else if (in == 44100 && (out == 96000 || out == 88200))  { *cutoff = 0.58; *beta = 10; }
    else if (in == 44100 && (out == 176400 || out == 192000)) { *cutoff = 0.48; *beta = 9; }
    else if (in == 48000 && out == 44100)  { *cutoff = 0.52; *beta = 8;  }
    else if (in == 48000 && out == 96000)  { *cutoff = 0.45; *beta = 10; }
    else if (in == 96000 && out == 48000)  { *cutoff = 0.47; *beta = 10; }
    else if ((in == 96000 || in == 88200) && out == 44100) { *cutoff = 0.55; *beta = 6; }
    else                                   { *cutoff = 0.45; *beta = 12; } /* incl. 192000->44100 */
    return 1;
}

/* a + b rounded to odd: the sum, or when it isn't exact, the neighbour of the rounded sum with
 * an odd last bit on the side of the exact sum (TwoSum gives the error exactly) */
static inline double add_odd(double a, double b)
{
    double s = a + b, bb = s - a;
    double e = (a - (s - bb)) + (b - bb);
    if (!isfinite(s)) return s;   /* inf (overflow or an inf operand) and NaN as the hardware gives them */
    if (e != 0.0) {
        uint64_t u;
        memcpy(&u, &s, sizeof u);
        if (!(u & 1)) {
            if ((e > 0) == (s > 0)) u++; else u--;   /* away from zero when the error is */
            memcpy(&s, &u, sizeof s);
        }
    }
    return s;
}

/* fmaf(a, b, c) exactly, in double: the product is exact (24 + 24 bits), and the sum rounded to
 * odd in 53 bits rounds to float as the exact sum would (53 >= 24 + 2): the FMA3 instruction's
 * result for finite inputs (helper/tests/resample-win.c checks it against the hardware's) */
static inline float fma_f32(float a, float b, float c)
{
    return (float)add_odd((double)a * (double)b, (double)c);
}

/* fma(a, b, c) exactly over the design's operands. Its domain is bounded: finite, no overflow,
 * and Dekker's residual is exact only while it doesn't underflow (u = 2^-1074:
 * fma_f64(u, 0.5, u) gives u, the hardware 2u). In the design, t and 1 - t^2 are far from the
 * subnormal range; the only products that can underflow are pow(arg, 2k) / k!^2 near the window's
 * edges, and they are added to a Bessel sum of at least 1 (its k = 0 term), far below half its
 * ulp, so the result is the hardware's either way (resample-win.c checks every standard rate's
 * table against the hardware's fma). The algorithm: Dekker's exact product a*b = ph + pl, then
 * (th, tl) = TwoSum(c, ph) and RN(th + RO(tl + pl)): Boldo and Melquiond's emulated FMA (2008,
 * "Emulation of FMA and correctly rounded sums: proved algorithms using rounding to odd") */
static double two_sum(double a, double b, double *err)
{
    double s = a + b, bb = s - a;
    *err = (a - (s - bb)) + (b - bb);
    return s;
}
static double fma_f64(double a, double b, double c)
{
    const double split = 134217729.0;                 /* 2^27 + 1 */
    double ca = split * a, ah = ca - (ca - a), al = a - ah;
    double cb = split * b, bh = cb - (cb - b), bl = b - bh;
    double ph = a * b;
    double pl = ((ah * bh - ph) + ah * bl + al * bh) + al * bl;   /* a*b = ph + pl exactly */
    double tl, th = two_sum(c, ph, &tl);
    return th + add_odd(tl, pl);
}

/* the Kaiser window (0x141123b00): I0(beta/2 sqrt(1 - t^2)) / I0(beta/2), 22 terms */
static double kaiser(int N, double beta, int i)
{
    double inv = 1.0 / (double)(N - 1);
    double t = fma_f64((double)(i + i), inv, -1.0);            /* vfmsub213sd */
    double arg = sqrt(fma_f64(-t, t, 1.0)) * beta * 0.5;       /* vfnmadd213sd, vsqrtsd */
    double hb = beta * 0.5, s1 = 0.0, s2 = 0.0;
    for (int k = 0; k < 22; k++) {
        double f = 1.0;
        for (int m = 2; m <= k; m++) f *= (double)m;           /* exact for k <= 21 */
        double rcp = 1.0 / (f * f), e = (double)k + (double)k;
        s1 = fma_f64(pow(arg, e), rcp, s1);                    /* vfmadd231sd */
        s2 = fma_f64(pow(hb, e), rcp, s2);                     /* vfmadd231sd */
    }
    return s1 / s2;
}

/* c_src<Float2>'s design (0x141123180): the prototype in double, then the per-phase table of
 * 29 float coefficients */
static float *build_table(int L, int M, double cutoff, double beta, int rate)
{
    int N = 29 * L, half = N / 2;
    double fc = (1.0 / (double)(rate < 44100 ? L : M)) * cutoff;
    double *proto = malloc(sizeof(double) * (size_t)N);
    float *tab = malloc(sizeof(float) * (size_t)L * TAPS);
    if (!proto || !tab) { free(proto); free(tab); return NULL; }
    for (int i = 0; i < N; i++) {
        int n = i - half;
        double h;
        if (n == 0) h = fc + fc;
        else {
            double dn = (double)n;
            h = sin(dn * 6.283185307179586 * fc) * (0.3183098861837907 / dn);  /* 2pi, 1/pi */
        }
        proto[i] = kaiser(N, beta, i) * h;
    }
    double scale = (double)L;
    for (int p = 0; p < L; p++) {
        int idx = (M * p) / L;
        for (int j = 0; j < TAPS; j++)
            tab[p * TAPS + j] = (float)(proto[L * (idx + j + 1) - M * p - 1] * scale);
    }
    free(proto);
    return tab;
}

/* c_src<Float2>::convolute (the AVX copy, 0x1411224a0) */
static inline F2 conv(const F2 *x, const float *h)
{
    float al = 0.f, ar = 0.f;
    int a = 0, b = TAPS - 1;
    for (; a < b; a++, b--) {
        al = fma_f32(x[a].l, h[a], al); ar = fma_f32(x[a].r, h[a], ar);   /* vfmadd231ps */
        al = al + x[b].l * h[b];        ar = ar + x[b].r * h[b];          /* vmulps, vaddps */
    }
    al = fma_f32(x[a].l, h[a], al); ar = fma_f32(x[a].r, h[a], ar);
    F2 o = { al, ar };
    return o;
}

static int hexval(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

/* 0 when the table equals the one proved for the rate */
static int table_proved(int rate, const float *tab, size_t n)
{
    const char *want = NULL;
    for (size_t i = 0; i < sizeof proved / sizeof *proved; i++)
        if (proved[i].rate == rate) want = proved[i].sha256;
    if (!want) return 1;
    unsigned char got[32];
    if (plat_sha256(tab, sizeof(float) * n, got)) return 1;
    for (int i = 0; i < 32; i++)
        if (got[i] != (unsigned char)(hexval(want[2 * i]) << 4 | hexval(want[2 * i + 1]))) return 1;
    return 0;
}

#if defined(RBSP_RESAMPLE_WIN_TEST) || defined(RBSP_TEST_FORMAT_RATES)
/* the test's view: the table (malloc'd) */
float *rb_resample_win_table(int rate);
float *rb_resample_win_table(int rate) {
    double cutoff, beta;
    if (rate % 50 || !pick_params(rate, 44100, &cutoff, &beta)) return NULL;
    return build_table(882, rate / 50, cutoff, beta, rate);
}
#endif
#ifdef RBSP_RESAMPLE_WIN_TEST
float rb_resample_win_fma(float a, float b, float c);
float rb_resample_win_fma(float a, float b, float c) { return fma_f32(a, b, c); }
double rb_resample_win_fma64(double a, double b, double c);
double rb_resample_win_fma64(double a, double b, double c) { return fma_f64(a, b, c); }
int rb_resample_win_table_ok(int rate, const float *tab);
int rb_resample_win_table_ok(int rate, const float *tab) { return !table_proved(rate, tab, 882 * TAPS); }
#endif

/* the proved rates' tables, built and checked against their sha256 once (by the first probe or
 * conversion that asks), then kept: tabs[i] NULL when it isn't the proved one */
#define NPROVED (sizeof proved / sizeof *proved)
static float *tabs[NPROVED];
static plat_once_t tabs_once = PLAT_ONCE_INIT;
static void tabs_build(void)
{
    for (size_t i = 0; i < NPROVED; i++) {
        double cutoff, beta;
        int rate = proved[i].rate;
        if (!pick_params(rate, 44100, &cutoff, &beta)) continue;
        float *t = build_table(882, rate / 50, cutoff, beta, rate);
        if (t && table_proved(rate, t, (size_t)882 * TAPS)) { free(t); t = NULL; }
        tabs[i] = t;
    }
}
static const float *proved_table(int rate)
{
    plat_once(&tabs_once, tabs_build);
    for (size_t i = 0; i < NPROVED; i++)
        if (proved[i].rate == rate) return tabs[i];
    return NULL;
}

int rb_resample_table_ok(int rate) { return rb_resample_proved(rate) && proved_table(rate) != NULL; }

/* 0 when done; 1 for a rate it doesn't take, a length it can't hold or no memory; 2 when the
 * table built here isn't the proved one */
static int convert(const float *tab, const float *in, size_t nin, int rate, float **out, size_t *nout);
int rb_resample(const float *in, size_t nin, int rate, float **out, size_t *nout) {
    if (rate % 50 || !rb_resample_proved(rate) || !nin) return 1;
    const float *tab = proved_table(rate);
    if (!tab) return 2;
    return convert(tab, in, nin, rate, out, nout);
}

#if defined(RBSP_RESAMPLE_WIN_TEST) || defined(RBSP_TEST_FORMAT_RATES)
/* The research decoder's candidate path, also used by tests to compare with rekordbox's dump.
 * The caller selects the rate. Research builds report the table's digest for later pinning. */
int rb_resample_win_any(const float *in, size_t nin, int rate, float **out, size_t *nout);
int rb_resample_win_any(const float *in, size_t nin, int rate, float **out, size_t *nout) {
    if (!nin) return 1;
    float *tab = rb_resample_win_table(rate);
    if (!tab) return 1;
#ifdef RBSP_TEST_FORMAT_RATES
    unsigned char digest[32];
    if (plat_sha256(tab, sizeof(float) * 882 * TAPS, digest)) { free(tab); return 1; }
    static const char digits[] = "0123456789abcdef";
    char hex[2 * sizeof digest + 1] = "";
    for (size_t i = 0; i < sizeof digest; i++) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    fprintf(stderr, "Candidate resampler table: %d Hz sha256 %s\n", rate, hex);
#endif
    int rc = convert(tab, in, nin, rate, out, nout);
    free(tab);
    return rc;
}
#endif

/* the conversion with TAB (rate's): 0, or 1 for a length it can't hold or no memory */
static int convert(const float *tab, const float *in, size_t nin, int rate, float **out, size_t *nout) {
    int L = 44100 / 50, M = rate / 50;
    /* output n uses X[q .. q+28], q = floor(M*n/L); produced only when X[q+28] is an input
     * sample, i.e. q <= nin-1 (as resample.c: the same count, proved by the full row). Checked
     * arithmetic: the padding, the output count, M*n in int64 and the output's bytes */
    size_t n_out = rb_resample_out_frames(nin, rate);
    if (!n_out || nin > SIZE_MAX - HIST - TAPS || nin > (size_t)(INT64_MAX / M) || n_out > SIZE_MAX / (2 * sizeof(float)))
        return 1;
    /* X = HIST zeros, then the input (interleaved pairs) */
    F2 *X = big_calloc(nin + HIST + TAPS, sizeof(F2));
    if (!X) return 1;
    for (size_t i = 0; i < nin; i++) { X[HIST + i].l = in[i]; X[HIST + i].r = in[nin + i]; }
    float *Y = big_alloc(sizeof(float) * 2 * n_out);
    if (!Y) { big_free(X); return 1; }
    for (size_t n = 0; n < n_out; n++) {
        size_t q = (size_t)(((int64_t)M * (int64_t)n) / L);
        int p = (int)(n % (size_t)L);
        F2 y = conv(X + q, tab + p * TAPS);
        Y[n] = y.l;
        Y[n_out + n] = y.r;
    }
    big_free(X);
    *out = Y;
    *nout = n_out;
    return 0;
}
