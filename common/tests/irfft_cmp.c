/* The vDSP vs pocketfft comparison (bridge.md §4 item 6, §1's tolerance 1e-6): Windows's inverse
 * real FFT (common/irfft_pocketfft.c, compiled here under the names pf_*) against the Mac's
 * (plat_mac.c's vDSP) and against the plain sum, at every size the bridge could use and at
 * rebuild_spec's own (n = 4096). Built and run by common/tests/win.sh.
 *
 *   irfft-cmp        exits 1 when a check fails */
#define plat_irfft pf_irfft
#define plat_irfft_new pf_irfft_new
#define plat_irfft_run pf_irfft_run
#define plat_irfft_free pf_irfft_free
#include "../irfft_pocketfft.c"
#undef plat_irfft
#undef plat_irfft_new
#undef plat_irfft_run
#undef plat_irfft_free
#include "check.h"

#include <math.h>

/* plat_mac.c's (plat.h was included above under the other names) */
typedef struct plat_irfft plat_irfft;
plat_irfft *plat_irfft_new(unsigned log2n);
void plat_irfft_run(plat_irfft *f, float *re, float *im, float *out);
void plat_irfft_free(plat_irfft *f);

static uint32_t seed = 12345;
static float rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return (float)((double)(seed >> 8) / (1 << 24) - 0.5);
}

int main(void) {
    for (unsigned lg = 1; lg <= 14; lg++) {
        size_t n = (size_t)1 << lg, h = n / 2;
        float *re = malloc(sizeof(float) * h), *im = malloc(sizeof(float) * h), *re2 = malloc(sizeof(float) * h),
              *im2 = malloc(sizeof(float) * h), *a = malloc(sizeof(float) * n), *b = malloc(sizeof(float) * n);
        if (!re || !im || !re2 || !im2 || !a || !b) return 2;
        for (size_t k = 0; k < h; k++) { re[k] = re2[k] = rnd(); im[k] = im2[k] = rnd(); }
        double *X = malloc(sizeof(double) * n);
        for (size_t k = 0; k < h; k++) { X[2 * k] = re[k]; X[2 * k + 1] = im[k]; }
        pf_irfft *p = pf_irfft_new(lg);
        plat_irfft *v = plat_irfft_new(lg);
        CHECK(p && v, "size %zu", n);
        if (!p || !v) return 1;
        pf_irfft_run(p, re, im, a);
        plat_irfft_run(v, re2, im2, b);
        double worst = 0, peak = 0, wsum = 0;
        int finite = 1;                                     /* an infinite b would make peak, so the bound, infinite */
        for (size_t t = 0; t < n; t++) {
            if (!isfinite(a[t]) || !isfinite(b[t])) { finite = 0; worst = INFINITY; }   /* fmax would drop a NaN */
            worst = fmax(worst, fabs((double)a[t] - b[t]));
            peak = fmax(peak, fabs((double)b[t]));
            if (lg <= 10) {                                 /* the plain sum, in double */
                double s = X[0] + X[1] * (t % 2 ? -1 : 1);
                for (size_t k = 1; k < h; k++) {
                    double w = 2 * M_PI * (double)k * (double)t / (double)n;
                    s += 2 * (X[2 * k] * cos(w) - X[2 * k + 1] * sin(w));
                }
                wsum = fmax(wsum, fabs(s - a[t]));
            }
        }
        /* relative to the output's peak, as the bridge's rebuild uses it (its reference, torch.istft,
         * agreed to 2.4e-7) */
        CHECK(finite, "non-finite output at %zu", n);
        CHECK(worst <= 1e-6 * fmax(1, peak), "pocketfft vs vDSP at %zu: %g (peak %g)", n, worst, peak);
        CHECK(lg > 10 || wsum <= 1e-6 * fmax(1, peak), "pocketfft vs the plain sum at %zu: %g", n, wsum);
        if (lg == 12) printf("n=4096 (rebuild_spec's): pocketfft vs vDSP %.3g, peak %.3g\n", worst, peak);
        pf_irfft_free(p);
        plat_irfft_free(v);
        free(re); free(im); free(re2); free(im2); free(a); free(b); free(X);
    }
    CHECK(pf_irfft_new(0) == NULL && pf_irfft_new(21) == NULL, "sizes outside 2..2^20 refused");
    printf("irfft_cmp: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
