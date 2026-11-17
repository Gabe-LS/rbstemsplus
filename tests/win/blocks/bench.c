/* Block vs function-pointer dispatch on the Mac's GCD paths (a small, non-exclusive timing): the
 * per-job overhead of dispatch_async vs dispatch_async_f on a serial utility queue, and of
 * dispatch_apply vs dispatch_apply_f on the utility global queue. Median of 15 rounds each. Each
 * apply iteration writes its own slot (no shared counter for the threads to fight over), folded
 * once after the apply. */
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile long sink;
static void job_f(void *c) { sink += (long)c; }
static long slot[256 * 16];                 /* one per iteration, 16 apart: no shared cache line */
static void apply_f(void *c, size_t i) { (void)c; slot[i * 16] += (long)i; }
static void fold(void) { for (int i = 0; i < 256; i++) sink += slot[i * 16]; }
static uint64_t now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double med(double *v, int n) { qsort(v, (size_t)n, sizeof *v, cmp); return v[n / 2]; }

int main(void) {
    dispatch_queue_t q = dispatch_queue_create("bench", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    dispatch_queue_t g = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    enum { N = 20000, R = 15, A = 256, AR = 200 };
    double ab[R], af[R], pb[R], pf[R];
    for (int r = 0; r < R; r++) {
        for (int side = 0; side < 2; side++) {               /* alternate which goes first */
            int blk = (side + r) % 2;
            uint64_t t0 = now();
            if (blk) for (long i = 0; i < N; i++) dispatch_async(q, ^{ sink += i; });
            else for (long i = 0; i < N; i++) dispatch_async_f(q, (void *)i, job_f);
            dispatch_sync(q, ^{});
            (blk ? ab : af)[r] = (double)(now() - t0) / N;
        }
        uint64_t t3 = now();
        for (int k = 0; k < AR; k++) { dispatch_apply(A, g, ^(size_t i) { slot[i * 16] += (long)i; }); fold(); }
        uint64_t t4 = now();
        for (int k = 0; k < AR; k++) { dispatch_apply_f(A, g, NULL, apply_f); fold(); }
        uint64_t t5 = now();
        pb[r] = (double)(t4 - t3) / (AR * A);
        pf[r] = (double)(t5 - t4) / (AR * A);
    }
    printf("dispatch_async   block %.1f ns/job   _f %.1f ns/job\n", med(ab, R), med(af, R));
    printf("dispatch_apply   block %.1f ns/iter  _f %.1f ns/iter\n", med(pb, R), med(pf, R));
    return 0;
}
