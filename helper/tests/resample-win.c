/* resample_win.c (rekordbox for Windows' resampler) on the Mac (T11), against the hardware's
 * fused multiply-adds (arm64 has them in every lane):
 *  - fma_f32 and fma_f64 (the software FMAs) equal fmaf and fma on random and near-tie inputs;
 *  - every standard rate's table equals one built here with the hardware's fma, the formula
 *    written out a second time from the disassembly (0x141123180, 0x141123b00);
 *  - the compiled-in sha256 of each proved rate's table matches the table built;
 *  - rb_resample's output on a fixed noise signal equals a reference convolution with the
 *    hardware's fmaf (the order of 0x1411224a0), and a golden sha256;
 *  - unproved rates are refused.
 * With IN.f32 (planar stereo at RATE) and DUMP.f32 (rekordbox's interleaved output) it also
 * compares a whole row: resample-win RATE IN DUMP. */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rbsp.h"
#include "big.h"
#include "plat.h"

float *rb_resample_win_table(int rate);
float rb_resample_win_fma(float a, float b, float c);
double rb_resample_win_fma64(double a, double b, double c);
int rb_resample_win_table_ok(int rate, const float *tab);

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static float rf(int span) {   /* a random finite float, exponent within +-span */
    uint32_t u = (uint32_t)next();
    int e = 127 + (int)(next() % (2u * (unsigned)span + 1)) - span;
    u = (u & 0x807fffffu) | ((uint32_t)e << 23);
    float f; memcpy(&f, &u, 4); return f;
}
static double rd(int span) {
    uint64_t u = next();
    int e = 1023 + (int)(next() % (2u * (unsigned)span + 1)) - span;
    u = (u & 0x800fffffffffffffull) | ((uint64_t)e << 52);
    double d; memcpy(&d, &u, 8); return d;
}

/* the AVX design again, with the hardware's fma */
static float *ref_table(int rate, double cutoff, double beta) {
    int L = 882, M = rate / 50, N = 29 * L, half = N / 2;
    double fc = (1.0 / (double)(rate < 44100 ? L : M)) * cutoff, hb = beta * 0.5, inv = 1.0 / (double)(N - 1);
    double *proto = malloc(sizeof(double) * N);
    float *tab = malloc(sizeof(float) * L * 29);
    for (int i = 0; i < N; i++) {
        int n = i - half;
        double h = n == 0 ? fc + fc : sin((double)n * 6.283185307179586 * fc) * (0.3183098861837907 / (double)n);
        double t = fma((double)(i + i), inv, -1.0);
        double arg = sqrt(fma(-t, t, 1.0)) * beta * 0.5, s1 = 0, s2 = 0;
        for (int k = 0; k < 22; k++) {
            double f = 1; for (int m = 2; m <= k; m++) f *= m;
            double rcp = 1.0 / (f * f), e = 2.0 * k;
            s1 = fma(pow(arg, e), rcp, s1); s2 = fma(pow(hb, e), rcp, s2);
        }
        proto[i] = (s1 / s2) * h;
    }
    for (int p = 0; p < L; p++)
        for (int j = 0; j < 29; j++) tab[p * 29 + j] = (float)(proto[L * ((M * p) / L + j + 1) - M * p - 1] * (double)L);
    free(proto);
    return tab;
}

static void hex(const void *p, size_t n, char out[65]) {
    unsigned char d[32];
    plat_sha256(p, n, d);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* rekordbox's convolution order with the hardware's fmaf */
static void ref_resample(const float *in, size_t nin, int rate, const float *tab, float *y, size_t n_out) {
    int L = 882, M = rate / 50;
    for (size_t n = 0; n < n_out; n++) {
        size_t q = (size_t)(((int64_t)M * (int64_t)n) / L);
        const float *h = tab + (n % L) * 29;
        for (int c = 0; c < 2; c++) {
            float acc = 0;
            for (int a = 0, b = 28; a <= b; a++, b--) {
                long ia = (long)(q + a) - 28, ib = (long)(q + b) - 28;
                float xa = ia >= 0 && (size_t)ia < nin ? in[c * nin + ia] : 0;
                float xb = ib >= 0 && (size_t)ib < nin ? in[c * nin + ib] : 0;
                acc = fmaf(xa, h[a], acc);
                if (a != b) acc = acc + xb * h[b];
            }
            y[c * n_out + n] = acc;
        }
    }
}

static float *load(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    float *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)sz / 8;
    return b;
}

int main(int argc, char **argv) {
    /* the software FMA's edges: infinities in and out, overflow; bit for bit, any NaN = any NaN */
    const float edges[][3] = {{1, 1, -INFINITY}, {-INFINITY, 0.5f, 0}, {-3e38f, 1, -INFINITY}, {1, 1, INFINITY},
                              {FLT_MAX, 1, FLT_MAX}, {-FLT_MAX, 1, -FLT_MAX}, {FLT_MAX, 2, -FLT_MAX}};
    for (size_t i = 0; i < sizeof edges / sizeof *edges; i++) {
        float a = edges[i][0], b = edges[i][1], c = edges[i][2];
        float s = rb_resample_win_fma(a, b, c), h = fmaf(a, b, c);
        CHECK((isnan(s) && isnan(h)) || !memcmp(&s, &h, 4), "fma_f32(%a, %a, %a) = %a, hardware %a", a, b, c, s, h);
    }
    /* the software FMAs */
    long bad32 = 0, bad64 = 0, trials = 4000000;
    for (long i = 0; i < trials; i++) {
        float a = rf(20), b = rf(20), c = i & 1 ? rf(40) : -(a * b);   /* half near-cancelling */
        if (i % 4 == 2) c = -(float)((double)a * b) * (1 + (float)(next() % 3 - 1) * 0x1p-23f);
        float s = rb_resample_win_fma(a, b, c), h = fmaf(a, b, c);
        if (memcmp(&s, &h, 4)) { if (!bad32++) printf("  fma_f32(%a, %a, %a) = %a, hardware %a\n", a, b, c, s, h); }
        double x = rd(200), y = rd(200), z = i & 1 ? rd(400) : -(x * y);
        double ds = rb_resample_win_fma64(x, y, z), dh = fma(x, y, z);
        if (memcmp(&ds, &dh, 8)) { if (!bad64++) printf("  fma_f64(%a, %a, %a) = %a, hardware %a\n", x, y, z, ds, dh); }
    }
    CHECK(!bad32, "fma_f32 differs from the hardware's fmaf in %ld of %ld cases", bad32, trials);
    CHECK(!bad64, "fma_f64 differs from the hardware's fma in %ld of %ld cases", bad64, trials);

    /* every standard rate's table, both ways; the proved one's checksum */
    const struct { int rate; double cutoff, beta; } rates[] = {
        {8000, .45, 12}, {12000, .45, 12}, {16000, .45, 12}, {22050, .45, 12}, {24000, .45, 12},
        {32000, .45, 12}, {48000, .52, 8}, {64000, .45, 12}, {88200, .55, 6}, {96000, .55, 6},
        {176400, .45, 12}, {192000, .45, 12}};
    for (size_t r = 0; r < sizeof rates / sizeof *rates; r++) {
        float *t = rb_resample_win_table(rates[r].rate), *u = ref_table(rates[r].rate, rates[r].cutoff, rates[r].beta);
        char h[65];
        hex(t, sizeof(float) * 882 * 29, h);
        CHECK(t && !memcmp(t, u, sizeof(float) * 882 * 29), "%d Hz: the table differs from the hardware-fma reference", rates[r].rate);
        printf("  table %6d Hz sha256 %s%s\n", rates[r].rate, h, rb_resample_proved(rates[r].rate) ? " (proved)" : "");
        if (rb_resample_proved(rates[r].rate)) CHECK(rb_resample_win_table_ok(rates[r].rate, t), "%d Hz: the compiled-in sha256 isn't this table's", rates[r].rate);
        free(t); free(u);
    }
    const int proved[] = {16000, 22050, 24000, 32000, 48000, 88200, 96000, 192000};
    for (size_t i = 0; i < sizeof proved / sizeof *proved; i++) {
        CHECK(rb_resample_proved(proved[i]), "%d Hz should be proved", proved[i]);
        CHECK(rb_resample_table_ok(proved[i]), "%d Hz: the cached table should pass its SHA-256 check", proved[i]);
    }
    CHECK(!rb_resample_proved(44100) && !rb_resample_proved(176400) && !rb_resample_table_ok(176400),
          "44.1 and 176.4 kHz should stay unproved");

    /* Three seconds of fixed noise per rate: checked conversion against the reference.
     * Keep 48 kHz first so it uses the same samples as its golden hash. */
    const int noise_rates[] = {48000, 32000, 16000, 24000, 88200, 192000};
    float *y;
    size_t ny;
    int rc;
    for (size_t r = 0; r < sizeof noise_rates / sizeof *noise_rates; r++) {
        int rate = noise_rates[r];
        size_t nin = (size_t)rate * 3;
        float *in = malloc(sizeof(float) * 2 * nin);
        for (size_t i = 0; i < 2 * nin; i++) in[i] = (float)((int32_t)(next() >> 40) - (1 << 23)) * 0x1p-23f * 0.9f;
        ny = 0;
        rc = rb_resample(in, nin, rate, &y, &ny);
        CHECK(!rc && ny == 132300, "rb_resample on %d Hz noise: rc %d, %zu frames (132300 expected)", rate, rc, ny);
        if (!rc) {
            float *t = rb_resample_win_table(rate), *z = malloc(sizeof(float) * 2 * ny);
            ref_resample(in, nin, rate, t, z, ny);
            CHECK(!memcmp(y, z, sizeof(float) * 2 * ny), "%d Hz noise: differs from the reference convolution (hardware fmaf)", rate);
            if (rate == 48000) {
                char h[65];
                hex(y, sizeof(float) * 2 * ny, h);
                CHECK(!strcmp(h, RBSP_GOLDEN_48000), "48 kHz noise: sha256 %s, golden %s", h, RBSP_GOLDEN_48000);
            }
            big_free(y); free(t); free(z);
        }
        free(in);
    }
    const float sample[2] = {0};
#ifdef RBSP_TEST_FORMAT_RATES
    const int candidates[] = {16000, 22050, 24000, 32000, 88200, 192000};
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        int rate = candidates[i];
        ny = 0;
        rc = rb_resample_win_any(sample, 1, rate, &y, &ny);
        CHECK(!rc && ny == rb_resample_out_frames(1, rate), "%d Hz: candidate conversion failed", rate);
        if (!rc) big_free(y);
    }
#endif
    CHECK(rb_resample(sample, 1, 176400, &y, &ny) == 1, "176.4 kHz isn't proved: it should be refused");
    CHECK(rb_resample(sample, 1, 44100, &y, &ny) == 1, "44.1 kHz isn't resampled");
    CHECK(rb_resample(sample, SIZE_MAX, 48000, &y, &ny) == 1, "a SIZE_MAX length should fail cleanly");

    /* the output count (rbsp.h's, the estimate's too) against the converter's old loops */
    size_t badn = 0;
    for (size_t n = 1; n <= 400000; n++) {
        int L = 882, M = 960;
        size_t o = (size_t)(((int64_t)(n - 1) * L) / M) + 1;
        while (o > 0 && (size_t)(((int64_t)M * (int64_t)(o - 1)) / L) > n - 1) o--;
        while ((size_t)(((int64_t)M * (int64_t)o) / L) <= n - 1) o++;
        if (o != rb_resample_out_frames(n, 48000) && !badn++) printf("  %zu frames: %zu, the loops %zu\n", n, rb_resample_out_frames(n, 48000), o);
    }
    CHECK(!badn, "rb_resample_out_frames differs from the converter's loops for %zu lengths", badn);
    /* 524,657 frames at 48 kHz: 482,029 out (2 chunks); seconds x 44100 truncated said 482,028 */
    size_t n5 = 524657;
    float *in5 = calloc(2 * n5, sizeof(float));
    rc = rb_resample(in5, n5, 48000, &y, &ny);
    CHECK(!rc && ny == 482029 && rb_resample_out_frames(n5, 48000) == 482029, "524,657 frames at 48 kHz: rc %d, %zu frames out (482,029 expected)", rc, ny);
    if (!rc) big_free(y);
    free(in5);

    /* whole rows from the owner's PC: RATE IN DUMP, as many as given */
    for (int a = 1; a + 2 < argc; a += 3) {
        int rate = atoi(argv[a]);
        size_t n, nd;
        float *x = load(argv[a + 1], &n), *d = load(argv[a + 2], &nd);
        if (!x || !d) { printf("FAIL can't read %s or %s\n", argv[a + 1], argv[a + 2]); return 1; }
        rc = rb_resample(x, n, rate, &y, &ny);
        size_t eq = 0, first = (size_t)-1;
        for (size_t i = 0; !rc && i < nd && i < ny; i++) {
            int e = !memcmp(&y[i], &d[2 * i], 4) && !memcmp(&y[ny + i], &d[2 * i + 1], 4);
            eq += e;
            if (!e && first == (size_t)-1) first = i;
        }
        /* rekordbox never sends a track's final sample (chunk_at): its dump is one frame shorter */
        CHECK(!rc && nd + 1 == ny && eq == nd, "row %s: rc %d, %zu of %zu frames equal (ours %zu), first difference at %zd",
              argv[a + 2], rc, eq, nd, ny, (ssize_t)first);
        if (!rc && eq == nd) printf("  row at %d Hz: all %zu frames of rekordbox's resampled audio equal, bit for bit\n", rate, nd);
        if (!rc) big_free(y);
        free(x); free(d);
    }
    printf("resample-win: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
