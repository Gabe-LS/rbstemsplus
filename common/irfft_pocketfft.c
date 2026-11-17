/* plat.h's inverse real FFT over pocketfft (BSD-3-Clause, commit 81d171a, release.md §9.2): the
 * Windows twin of plat_mac.c's vDSP (bridge.md §1, T2). plat_win.c #includes it; on the Mac
 * common/tests/irfft_cmp.c compiles it under other names and holds it to vDSP and to the plain
 * sum (bridge.md §4 item 6).
 *
 * plat.h's packing (re[0] the DC term, im[0] the Nyquist term, re[k] + i im[k] below n/2) becomes
 * pocketfft's half-complex order (r0, r1, i1, ..., r(n/2-1), i(n/2-1), r(n/2)); rfft_backward with
 * factor 1 is the plain sum over all n bins, as vDSP's inverse is. pocketfft works in double. */
#include "plat.h"
#include "pocketfft.h"

#include <math.h>
#include <stdlib.h>

struct plat_irfft { rfft_plan plan; unsigned log2n; double *buf; };

COMMON_API plat_irfft *plat_irfft_new(unsigned log2n) {
    plat_irfft *f = log2n >= 1 && log2n <= 20 ? malloc(sizeof *f) : NULL;
    if (!f) return NULL;
    size_t n = (size_t)1 << log2n;
    f->log2n = log2n;
    f->plan = make_rfft_plan(n);
    f->buf = malloc(sizeof(double) * n);
    if (!f->plan || !f->buf) {
        if (f->plan) destroy_rfft_plan(f->plan);
        free(f->buf);
        free(f);
        return NULL;
    }
    return f;
}

COMMON_API void plat_irfft_run(plat_irfft *f, float *re, float *im, float *out) {
    size_t n = (size_t)1 << f->log2n, h = n / 2;
    double *b = f->buf;
    b[0] = re[0];
    for (size_t k = 1; k < h; k++) {
        b[2 * k - 1] = re[k];
        b[2 * k] = im[k];
    }
    b[n - 1] = im[0];
    /* rfft_backward allocates its scratch: out of memory gives NaN, which the bridge's checks of
     * a rebuilt chunk refuse (never silence that looks real) */
    if (rfft_backward(f->plan, b, 1.0) != 0)
        for (size_t t = 0; t < n; t++) out[t] = NAN;
    else
        for (size_t t = 0; t < n; t++) out[t] = (float)b[t];
}

COMMON_API void plat_irfft_free(plat_irfft *f) {
    if (!f) return;
    destroy_rfft_plan(f->plan);
    free(f->buf);
    free(f);
}
