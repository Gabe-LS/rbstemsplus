/* plat_mac.c's unit tests (bridge.md §0a T1b1): each function against what the bridge's own code
 * does today, and against plat_win.c's answer where a Mac call's own would differ (the twin is
 * common/tests/plat_win.c), on a scratch folder under $TMPDIR. Built by common/tests/run.sh with the fake clock
 * (PLAT_TEST_CLOCK), plain, with ASan and UBSan, and with TSan.
 *
 *   plat-test SCRATCH       exits 1 when a check fails */
#include "../plat_helper.h"
#include "check.h"

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static char root[1024];

static void path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", root, name); }

static void write_file(const char *name, const char *text) {
    char p[1100];
    path(p, sizeof p, name);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void hexs(const unsigned char *d, char *out) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* ---------------------------------------------------------------- sha256 */

static void t_sha256(void) {
    unsigned char d[32];
    char h[65];
    plat_sha256("", 0, d);
    hexs(d, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty: %s", h);
    plat_sha256("abc", 3, d);
    hexs(d, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "abc: %s", h);
    plat_sha256_ctx c;
    char a[1000];
    memset(a, 'a', sizeof a);
    CHECK(plat_sha256_init(&c) == 0, "init");
    for (int i = 0; i < 1000; i++) plat_sha256_update(&c, a, sizeof a);
    CHECK(plat_sha256_final(&c, d) == 0, "final");
    hexs(d, h);
    CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "a million a: %s", h);
    /* over 1 GB in one update (CommonCrypto takes 32-bit lengths): the same as 64 MB pieces */
    size_t big = 0x40000000u + 4097;
    unsigned char *z = mmap(NULL, big, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0), e[32];
    if (z == MAP_FAILED) { CHECK(0, "mmap %zu", big); return; }
    plat_sha256(z, big, d);
    plat_sha256_init(&c);
    for (size_t at = 0; at < big; at += 64u << 20) plat_sha256_update(&c, z + at, big - at < (64u << 20) ? big - at : 64u << 20);
    plat_sha256_final(&c, e);
    CHECK(!memcmp(d, e, 32), "one update over 1 GB = the same bytes in pieces");
    munmap(z, big);
}

/* ---------------------------------------------------------------- inverse real FFT */

static void t_irfft(void) {
    for (unsigned lg = 2; lg <= 12; lg += 5) {
        size_t n = (size_t)1 << lg, h = n / 2;
        float *re = malloc(sizeof(float) * h), *im = malloc(sizeof(float) * h), *out = malloc(sizeof(float) * n);
        double *X = malloc(sizeof(double) * h * 2), worst = 0, peak = 0;
        uint32_t s = 12345;
        for (size_t k = 0; k < h; k++) {
            s = s * 1103515245u + 12345u; X[2 * k] = (double)(s >> 8) / (1 << 24) - 0.5;
            s = s * 1103515245u + 12345u; X[2 * k + 1] = (double)(s >> 8) / (1 << 24) - 0.5;
            re[k] = (float)X[2 * k];
            im[k] = (float)X[2 * k + 1];
        }
        plat_irfft *f = plat_irfft_new(lg);
        CHECK(f != NULL, "irfft %zu", n);
        if (!f) return;
        plat_irfft_run(f, re, im, out);
        plat_irfft_free(f);
        /* the plain sum over all n bins, X Hermitian: X[0] = re[0], X[n/2] = im[0] */
        for (size_t t = 0; t < n; t++) {
            double v = X[0] + X[1] * (t % 2 ? -1 : 1);
            for (size_t k = 1; k < h; k++) {
                double a = 2 * M_PI * (double)k * (double)t / (double)n;
                v += 2 * (X[2 * k] * cos(a) - X[2 * k + 1] * sin(a));
            }
            worst = fmax(worst, fabs(v - out[t]));
            peak = fmax(peak, fabs(v));
        }
        CHECK(worst <= 1e-5 * fmax(1, peak) * lg, "irfft %zu: off by %g (peak %g)", n, worst, peak);
        free(re); free(im); free(out); free(X);
    }
    CHECK(plat_irfft_new(0) == NULL && plat_irfft_new(21) == NULL, "irfft sizes outside 2..2^20 refused");
}

/* ---------------------------------------------------------------- locks, once, clocks */

static atomic_int once_runs;
static void once_fn(void) { atomic_fetch_add(&once_runs, 1); }
static plat_once_t once = PLAT_ONCE_INIT;
static void once_job(void *ctx, size_t i) { (void)ctx; (void)i; plat_once(&once, once_fn); }

static void t_locks_clocks(void) {
    plat_par_for(64, PLAT_WORK_DEFAULT, once_job, NULL);
    CHECK(atomic_load(&once_runs) == 1, "plat_once ran %d times", atomic_load(&once_runs));
    plat_mutex m = PLAT_MUTEX_INIT;
    plat_cond c;
    plat_cond_init(&c);
    uint64_t t0 = plat_now_mono_ns();
    plat_mutex_lock(&m);
    int r = plat_cond_wait_ns(&c, &m, 20000000);
    plat_mutex_unlock(&m);
    uint64_t took = plat_now_mono_ns() - t0;
    CHECK(r == ETIMEDOUT && took >= 19000000, "timed wait: %d after %llu ns", r, (unsigned long long)took);
    plat_cond_destroy(&c);
    CHECK(plat_thread_equal(plat_thread_self(), plat_thread_self()), "thread id");
    int64_t w = plat_now_wall_s(), wn = plat_now_wall_ns();
    CHECK(llabs(w - (int64_t)time(NULL)) <= 1 && llabs(wn / 1000000000 - w) <= 1, "wall clock %lld %lld", (long long)w, (long long)wn);
    struct tm tm;
    plat_localtime(86400 * 365, &tm);
    CHECK(tm.tm_year == 70 || tm.tm_year == 71, "localtime year %d", tm.tm_year);
    uint64_t a = plat_now_mono_ns();
    plat_sleep_ms(5);
    CHECK(plat_now_mono_ns() - a >= 4000000, "mono clock and sleep");
}

/* ---------------------------------------------------------------- queues and timers */

typedef struct {
    plat_mutex m;
    plat_cond c;
    int order[1000], n, cls, bad_cls;
    plat_thread_id last;
    uint64_t at;
} qstate;

static void q_job(void *ctx) {
    qstate *s = ctx;
    plat_mutex_lock(&s->m);
    s->order[s->n] = s->n;
    s->n++;
    if (plat_work_class() != s->cls) s->bad_cls++;
    plat_mutex_unlock(&s->m);
}

static void q_stamp(void *ctx) {
    qstate *s = ctx;
    plat_mutex_lock(&s->m);
    s->at = plat_now_mono_ns();
    s->n++;
    plat_cond_broadcast(&s->c);
    plat_mutex_unlock(&s->m);
}

static atomic_int seen[1000];
static atomic_int maint_threads;
static void par_job(void *ctx, size_t i) {
    (void)ctx;
    atomic_fetch_add(&seen[i], 1);
    if (plat_work_class() == PLAT_WORK_MAINT) atomic_fetch_add(&maint_threads, 1);
}

static void par_from_maint(void *ctx) { plat_par_for(1000, PLAT_WORK_MAINT, par_job, ctx); }

/* plat_par_for asked for default work from a maintenance job: its iterations are maintenance
 * work too, as plat_win.c's workers inherit the caller's class (r4:I51) */
static atomic_int inherit_maint, inherit_runs;
static void inherit_job(void *ctx, size_t i) {
    (void)ctx; (void)i;
    atomic_fetch_add(&inherit_runs, 1);
    if (plat_work_class() == PLAT_WORK_MAINT) atomic_fetch_add(&inherit_maint, 1);
}
static void par_default_from_maint(void *ctx) { plat_par_for(200, PLAT_WORK_DEFAULT, inherit_job, ctx); }
static void never_job(void *ctx, size_t i) { (void)ctx; (void)i; atomic_fetch_add(&inherit_runs, 1000000); }

/* timers racing their cancel: each one's fn runs exactly when its cancel says 0 (TSan's case) */
enum { RACE_N = 2000 };
static atomic_int race_ran[RACE_N];
static void race_fn(void *ctx) { atomic_fetch_add((atomic_int *)ctx, 1); }

static int wait_n(qstate *s, int n, uint64_t ns) {
    uint64_t end = plat_now_mono_ns() + ns;
    plat_mutex_lock(&s->m);
    while (s->n < n && plat_now_mono_ns() < end) plat_cond_wait_ns(&s->c, &s->m, 1000000);
    int ok = s->n >= n;
    plat_mutex_unlock(&s->m);
    return ok;
}

static void t_queues(void) {
    plat_queue *w = plat_queue_new("rbsp.test.writer", PLAT_WORK_DEFAULT), *mq = plat_queue_new("rbsp.test.maint", PLAT_WORK_MAINT);
    CHECK(w && mq, "queues made");
    qstate s = {.m = PLAT_MUTEX_INIT};
    plat_cond_init(&s.c);
    s.cls = PLAT_WORK_DEFAULT;
    for (int i = 0; i < 1000; i++) plat_async(w, q_job, &s);
    plat_queue_wait(w);
    int inorder = s.n == 1000;
    for (int i = 0; i < s.n; i++) inorder &= s.order[i] == i;
    CHECK(inorder && !s.bad_cls, "a serial queue runs its jobs in order (%d, %d off class)", s.n, s.bad_cls);
    s.n = 0;
    s.cls = PLAT_WORK_MAINT;
    for (int i = 0; i < 100; i++) plat_async(mq, q_job, &s);
    plat_queue_wait(mq);
    CHECK(s.n == 100 && !s.bad_cls, "the maintenance queue's jobs run at QOS_CLASS_UTILITY (%d off)", s.bad_cls);
    CHECK(plat_work_class() == PLAT_WORK_DEFAULT, "the test's own thread isn't maintenance work");

    s.n = 0;
    uint64_t t0 = plat_now_mono_ns();
    CHECK(plat_after(mq, 30000000, q_stamp, &s) == 0, "after");
    CHECK(wait_n(&s, 1, 2000000000), "plat_after ran");
    CHECK(s.at - t0 >= 29000000, "plat_after waited %llu ns", (unsigned long long)(s.at - t0));

    plat_async(mq, par_from_maint, NULL);                  /* as load_index: from the maintenance queue */
    plat_queue_wait(mq);
    int once_each = 1;
    for (int i = 0; i < 1000; i++) once_each &= atomic_load(&seen[i]) == 1;
    CHECK(once_each, "par_for runs every index once");
    CHECK(atomic_load(&maint_threads) == 1000, "par_for's maintenance work runs at QOS_CLASS_UTILITY (%d)", atomic_load(&maint_threads));
    plat_async(mq, par_default_from_maint, NULL);
    plat_queue_wait(mq);
    CHECK(atomic_load(&inherit_runs) == 200 && atomic_load(&inherit_maint) == 200,
          "par_for from maintenance work stays maintenance work (%d of %d)", atomic_load(&inherit_maint), atomic_load(&inherit_runs));
    plat_par_for(0, PLAT_WORK_DEFAULT, never_job, NULL);
    CHECK(atomic_load(&inherit_runs) == 200, "par_for of 0 runs nothing");

    /* a timer that fires, one cancelled first, one cancelled after it fired, one on the layer's queue */
    s.n = 0;
    plat_timer *a = plat_timer_new(mq, 10000000, q_stamp, &s), *b = plat_timer_new(mq, 200000000, q_stamp, &s);
    plat_timer *d = plat_timer_new(NULL, 1000000, q_stamp, &s);
    CHECK(a && b && d, "timers made");
    CHECK(plat_timer_cancel(b) == 1, "a timer cancelled in time doesn't run");
    CHECK(wait_n(&s, 2, 2000000000), "two timers fired");
    CHECK(plat_timer_cancel(a) == 0 && plat_timer_cancel(d) == 0, "cancel after firing says it ran");
    plat_sleep_ms(250);
    plat_mutex_lock(&s.m);
    CHECK(s.n == 2, "the cancelled timer never ran (%d)", s.n);
    plat_mutex_unlock(&s.m);

    /* RACE_N timers due within 50 us, on two queues and the layer's own, each cancelled while it
     * may be firing: fn runs once when cancel says 0, never when it says 1 */
    static plat_timer *rt[RACE_N];
    static int said[RACE_N];
    int made = 1, ones = 0;
    for (int i = 0; i < RACE_N; i++) {
        rt[i] = plat_timer_new(i % 3 == 0 ? mq : i % 3 == 1 ? w : NULL, (uint64_t)(i % 50) * 1000, race_fn, &race_ran[i]);
        made &= rt[i] != NULL;
        if (i % 7 == 0) plat_sleep_ms(0);
    }
    CHECK(made, "race timers made");
    for (int i = 0; i < RACE_N; i++) ones += said[i] = rt[i] ? plat_timer_cancel(rt[i]) : 1;
    int settled = 0;
    for (int tries = 0; tries < 400 && !settled; tries++) {   /* each cancel that said 0: its fn has run or is running */
        settled = 1;
        for (int i = 0; i < RACE_N; i++) settled &= said[i] || atomic_load(&race_ran[i]) == 1;
        if (!settled) plat_sleep_ms(5);
    }
    plat_sleep_ms(200);                                     /* past every due time: the cancelled stay unrun */
    int wrong = 0;
    for (int i = 0; i < RACE_N; i++) wrong += atomic_load(&race_ran[i]) != (said[i] ? 0 : 1);
    CHECK(settled && !wrong, "timer vs cancel: %d of %d timers wrong (%d cancelled in time)", wrong, RACE_N, ones);
    atomic_int far = 0;
    plat_timer *ft = plat_timer_new(mq, UINT64_MAX, race_fn, &far);
    plat_sleep_ms(50);
    CHECK(ft && plat_timer_cancel(ft) == 1 && atomic_load(&far) == 0, "a delay past INT64_MAX ns never runs at once");

#if defined(__BLOCKS__)
    __block int bn = 0;
    plat_async_b(w, ^{ bn++; });
    plat_queue_wait(w);
    CHECK(bn == 1, "plat_async_b");
    s.n = 0;
    qstate *sp = &s;
    plat_after_b(w, 1000000, ^{ q_stamp(sp); });
    CHECK(wait_n(&s, 1, 2000000000), "plat_after_b");
    __block atomic_int bsum = 0;
    plat_par_for_b(100, PLAT_WORK_MAINT, ^(size_t i) { atomic_fetch_add(&bsum, (int)i); });
    CHECK(atomic_load(&bsum) == 4950, "plat_par_for_b");
#endif
    plat_cond_destroy(&s.c);
}

/* ---------------------------------------------------------------- the fake clock */

#ifdef PLAT_TEST_CLOCK
static char fired[64];
static size_t nfired;
static plat_queue *fake_q;
static void fk(void *ctx) { if (nfired < sizeof fired - 1) fired[nfired++] = *(const char *)ctx; }
static void fk_rearm(void *ctx) { fk(ctx); plat_after(fake_q, 0, fk, "z"); }

static void t_fake_clock(void) {
    fake_q = plat_queue_new("rbsp.test.fake", PLAT_WORK_DEFAULT);
    plat_fake_clock(1, 1000, 5000000000);
    CHECK(plat_now_mono_ns() == 1000 && plat_now_wall_ns() == 5000000000 && plat_now_wall_s() == 5, "the fake clock reads");
    plat_after(fake_q, 30, fk, "c");
    plat_after(fake_q, 10, fk, "a");
    plat_after(fake_q, 30, fk, "d");                        /* the same time: in the order armed */
    plat_after(fake_q, 20, fk_rearm, "b");                  /* arms z, due at once */
    plat_timer *x = plat_timer_new(fake_q, 15, fk, "x"), *y = plat_timer_new(NULL, 25, fk, "y");
    CHECK(plat_fake_pending() == 6, "six waiting (%zu)", plat_fake_pending());
    CHECK(plat_timer_cancel(x) == 1, "a fake timer cancelled");
    CHECK(plat_fake_pending() == 5, "the cancelled one isn't counted (%zu)", plat_fake_pending());
    plat_fake_advance(9);
    CHECK(nfired == 0 && plat_now_mono_ns() == 1009, "nothing due yet");
    plat_fake_advance(21);
    fired[nfired] = 0;
    CHECK(!strcmp(fired, "abzycd"), "fired in due order, then arming order: %s", fired);
    CHECK(plat_fake_pending() == 0 && plat_timer_cancel(y) == 0, "all fired");
    CHECK(plat_now_wall_ns() == 5000000030, "the wall clock moved with it");
    atomic_int never = 0;
    plat_timer *nt = plat_timer_new(fake_q, UINT64_MAX, race_fn, &never);
    plat_fake_advance(0);
    plat_fake_advance(1000000);
    CHECK(nt && atomic_load(&never) == 0 && plat_timer_cancel(nt) == 1, "a fake UINT64_MAX timer: its deadline never comes");
#if defined(__BLOCKS__)
    __block int bn = 0;
    plat_after_b(fake_q, 5, ^{ bn++; });
    plat_fake_advance(4);
    CHECK(bn == 0, "a block timer waits");
    plat_fake_advance(1);
    CHECK(bn == 1, "and fires");
#endif
    plat_fake_clock(0, 0, 0);
    uint64_t m = plat_now_mono_ns();
    CHECK(m > 1000000 && llabs(plat_now_wall_s() - (int64_t)time(NULL)) <= 1, "the real clocks again");
}
#endif

/* ---------------------------------------------------------------- files and folders */

typedef struct { char names[16][64]; int kinds[16], n, stop_at; } listing;
static int list_fn(void *ctx, const char *name, const plat_stat *st) {
    listing *l = ctx;
    if (l->n < 16) { snprintf(l->names[l->n], 64, "%s", name); l->kinds[l->n] = st->kind; }
    l->n++;
    return l->stop_at && l->n >= l->stop_at;
}
static int listed(const listing *l, const char *name, int kind) {
    for (int i = 0; i < l->n && i < 16; i++)
        if (!strcmp(l->names[i], name)) return l->kinds[i] == kind;
    return 0;
}

static void t_files(void) {
    char p[1100], q[1100];
    plat_dir d, sub, bad;
    plat_stat st0;
    CHECK(plat_dir_open(root, &d) == 0 && plat_dir_ok(d), "open the scratch folder");
    CHECK(plat_mkdir_at(d, "ab") == 0 && plat_mkdir_at(d, "ab") == EEXIST, "mkdir_at, then EEXIST");
    CHECK(plat_dir_open_at(d, "ab", &sub) == 0, "open_at");
    CHECK(plat_dir_name_is(sub, "ab") && !plat_dir_name_is(sub, "AB"), "name_is");
    plat_dir up;
    int e = plat_dir_open_at(d, "AB", &up);
    if (e == 0) {                                           /* a volume that ignores case */
        CHECK(!plat_dir_name_is(up, "AB") && plat_dir_name_is(up, "ab"), "name_is tells AB from ab");
        plat_dir_close(up);
    } else CHECK(e == ENOENT, "AB on a case-sensitive volume: %d", e);
    path(p, sizeof p, "ab");
    path(q, sizeof q, "link");
    CHECK(symlink(p, q) == 0, "symlink");
    e = plat_dir_open(q, &bad);
    CHECK(e == ELOOP && !plat_dir_ok(bad), "a link to a folder isn't opened: ELOOP, as a reparse point on Windows (%d)", e);
    e = plat_dir_open_at(d, "link", &bad);
    CHECK(e == ELOOP && !plat_dir_ok(bad), "nor through open_at (%d)", e);
    char fl[1100], ft[1100];
    path(ft, sizeof ft, "ftarget");
    path(fl, sizeof fl, "flink");
    write_file("ftarget", "x");
    CHECK(symlink(ft, fl) == 0, "symlink to a file");
    e = plat_dir_open(fl, &bad);
    int e2 = plat_dir_open_at(d, "flink", &bad);
    CHECK(e == ELOOP && e2 == ELOOP && !plat_dir_ok(bad), "a link to a file: ELOOP, never followed (%d, %d)", e, e2);
    path(ft, sizeof ft, "nowhere");
    path(fl, sizeof fl, "dlink");
    CHECK(symlink(ft, fl) == 0, "a dangling link");
    e = plat_dir_open(fl, &bad);
    e2 = plat_dir_open_at(d, "dlink", &bad);
    CHECK(e == ELOOP && e2 == ELOOP && !plat_dir_ok(bad), "a dangling link: ELOOP (%d, %d)", e, e2);
    /* plat_dir_open_follow (review SF2): the link to a folder is followed to that folder (its id);
     * one to a file is ENOTDIR, a dangling one ENOENT, a relative path EINVAL */
    {
        plat_dir fd;
        plat_stat a, b;
        path(q, sizeof q, "link");
        e = plat_dir_open_follow(q, &fd);
        CHECK(e == 0 && plat_dir_ok(fd) && !plat_dir_stat(fd, &a) && !plat_dir_stat(sub, &b) && plat_id_same(a.id, b.id),
              "open_follow: a link to a folder opens that folder (%d)", e);
        plat_dir_close(fd);
        CHECK(plat_dir_open_follow(root, &fd) == 0 && plat_dir_ok(fd), "open_follow: a plain folder");
        plat_dir_close(fd);
        path(ft, sizeof ft, "flink");
        e = plat_dir_open_follow(ft, &fd);
        CHECK(e == ENOTDIR && !plat_dir_ok(fd), "open_follow: a link to a file: ENOTDIR (%d)", e);
        path(ft, sizeof ft, "dlink");
        e = plat_dir_open_follow(ft, &fd);
        CHECK(e == ENOENT && !plat_dir_ok(fd), "open_follow: a dangling link: ENOENT (%d)", e);
        fd.fd = 7;
        CHECK(plat_dir_open_follow("relative", &fd) == EINVAL && !plat_dir_ok(fd), "open_follow: an absolute path only");
    }
    plat_unlink_at(d, "flink");
    plat_unlink_at(d, "ftarget");
    plat_unlink_at(d, "dlink");
    CHECK(plat_dir_open_at(d, "nope", &bad) == ENOENT, "ENOENT");
    bad.fd = 7;
    CHECK(plat_dir_open("relative", &bad) == EINVAL && !plat_dir_ok(bad), "dir_open: an absolute path only");

    /* names relative to a folder: one component, as plat_win.c's wr_name16 */
    static const char *const not_names[] = {"", ".", "..", "ab/", "../ab", "ab/.", "a/b", "/abs"};
    int refused = 1;
    for (size_t i = 0; i < sizeof not_names / sizeof *not_names; i++) {
        const char *nm = not_names[i];
        plat_dir od = {7};
        plat_file of = {7};
        plat_stat ost;
        int64_t osz;
        int r[9] = {plat_dir_open_at(d, nm, &od), plat_mkdir_at(d, nm), plat_stat_at(d, nm, &ost), plat_unlink_at(d, nm),
                    plat_touch_at(d, nm), plat_rename_at(d, nm, d, "zz"), plat_rename_at(d, "zz", d, nm),
                    plat_file_open_at(d, nm, 100, &osz, &of), 0};
        r[8] = plat_file_create_at(d, nm, 0, &of);
        for (int k = 0; k < 9; k++) {
            if (r[k] != EINVAL) { refused = 0; fprintf(stderr, "  \"%s\": call %d gave %d\n", nm, k, r[k]); }
        }
        if (plat_dir_ok(od) || plat_file_ok(of)) refused = 0;
    }
    CHECK(refused, "\"\", \".\", \"..\" and names with a '/' refused by every _at call (EINVAL), the handles left closed");
    CHECK(plat_stat_at(plat_dir_cwd(), "ab", &st0) == EINVAL, "relative to plat_dir_cwd(): an absolute path only");
    path(p, sizeof p, "ab");
    CHECK(plat_stat_at(plat_dir_cwd(), p, &st0) == 0 && st0.kind == PLAT_DIR, "an absolute path through plat_dir_cwd()");

    /* create, write, open checked */
    plat_file f, g;
    CHECK(plat_file_create_at(sub, "x.tmp", 0, &f) == 0, "create");
    CHECK(plat_file_create_at(sub, "x.tmp", 0, &g) == EEXIST, "create refuses an existing name");
    FILE *s = plat_file_stdio(f, "w");
    CHECK(s != NULL, "stdio");
    if (s) {
        fputs("0123456789", s);
        CHECK(plat_stdio_sync(s) == 0, "synced through the stream (plat_helper.h; the stream owns f)");
        CHECK(fclose(s) == 0, "closed");
    }
    CHECK(plat_rename_at(sub, "x.tmp", sub, "x") == 0, "rename_at");
    /* never over an existing name: EEXIST, both kept; to a free name, done */
    plat_file nf;
    plat_stat nst;
    CHECK(plat_file_create_at(sub, "n.tmp", 1, &nf) == 0, "create n.tmp");
    plat_file_close(nf);
    CHECK(plat_rename_noreplace_at(sub, "n.tmp", sub, "x") == EEXIST && !plat_stat_at(sub, "n.tmp", &nst) && !plat_stat_at(sub, "x", &nst) && nst.size == 10,
          "rename_noreplace_at over a file: EEXIST, both kept");
    CHECK(plat_rename_noreplace_at(sub, "n.tmp", sub, "n") == 0 && plat_stat_at(sub, "n.tmp", &nst) == ENOENT && !plat_stat_at(sub, "n", &nst),
          "rename_noreplace_at to a free name");
    CHECK(plat_rename_noreplace_at(sub, "n", sub, "../n") == EINVAL, "rename_noreplace_at: a name with a slash, EINVAL");
    plat_unlink_at(sub, "n");
    int64_t size = -1;
    CHECK(plat_file_open_at(sub, "x", 10, &size, &f) == 0 && size == 10, "open_at, size %lld", (long long)size);
    char buf[32] = {0};
    CHECK(plat_read_all(f, buf, 10) == 0 && !memcmp(buf, "0123456789", 10), "read_all");
    CHECK(plat_read_all(f, buf, 1) == EIO, "read_all past the end: EIO");
    plat_file_close(f);
    CHECK(plat_file_open_at(sub, "x", 9, &size, &f) == PLAT_EFTYPE && !plat_file_ok(f), "over max: EFTYPE");
    CHECK(plat_file_open_at(d, "ab", 1 << 20, &size, &f) == PLAT_EFTYPE, "a folder: EFTYPE");
    CHECK(plat_file_open_at(d, "link", 1 << 20, &size, &f) == ELOOP, "a link: not followed");
    path(p, sizeof p, "fifo");
    CHECK(mkfifo(p, 0600) == 0, "mkfifo");
    CHECK(plat_file_open_at(d, "fifo", 1 << 20, &size, &f) == PLAT_EFTYPE, "a FIFO: EFTYPE, without blocking");
    alarm(5);                                               /* a FIFO open that blocks fails the test, never hangs it */
    e = plat_file_open_path(p, &f);
    CHECK(e == EINVAL && !plat_file_ok(f), "open_path of a FIFO: EINVAL, without blocking (%d)", e);
    errno = 0;
    FILE *ff = plat_fopen_w(p, 1);
    CHECK(!ff && errno == EINVAL, "fopen_w of a FIFO no one reads: EINVAL, without blocking (%d)", errno);
    if (ff) fclose(ff);
    char fo[1100];
    path(fo, sizeof fo, "ab");
    CHECK(plat_file_open_path(fo, &f) == EINVAL, "open_path of a folder: EINVAL");
    errno = 0;
    CHECK(plat_fopen_w(fo, 1) == NULL && errno == EINVAL, "fopen_w of a folder: EINVAL (%d)", errno);
    errno = 0;
    CHECK(plat_fopen_w(fo, 0) == NULL && errno == EINVAL, "fopen_w of a folder, truncating: EINVAL (%d)", errno);
    char nd[1100];
    path(nd, sizeof nd, "nodir/log");
    errno = 0;
    CHECK(plat_fopen_w(nd, 1) == NULL && errno == ENOENT, "fopen_w in a missing folder: ENOENT (%d)", errno);
    alarm(0);
    path(p, sizeof p, "ab/x");
    CHECK(plat_file_open_at(plat_dir_cwd(), p, 100, NULL, &f) == 0, "an absolute path through plat_dir_cwd");
    size_t got = 0;
    CHECK(plat_read(f, buf, 4, &got) == 0 && got == 4, "plat_read");
    CHECK(plat_read(f, buf, 32, &got) == 0 && got == 6, "plat_read, the rest");
    CHECK(plat_read(f, buf, 32, &got) == 0 && got == 0, "plat_read at the end");
    plat_file_close(f);
    /* counts over INT_MAX, which read(2) refuses: a short read (at most 1 GB at a time) */
    size_t huge = (size_t)3 << 30;
    char *hb = mmap(NULL, huge, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (hb == MAP_FAILED) CHECK(0, "mmap %zu", huge);
    else {
        CHECK(plat_file_open_at(sub, "x", 100, NULL, &f) == 0, "reopen for big reads");
        CHECK(plat_read(f, hb, huge, &got) == 0 && got == 10 && !memcmp(hb, "0123456789", 10), "plat_read of 3 GB (%zu)", got);
        CHECK(plat_pread(f, hb, huge, 2, &got) == 0 && got == 8, "plat_pread of 3 GB (%zu)", got);
        CHECK(plat_read_all(f, hb, huge) == EIO, "plat_read_all of 3 GB: EIO for a short file, not EINVAL");
        plat_file_close(f);
        munmap(hb, huge);
    }
    CHECK(plat_file_open_path(p, &f) == 0, "open_path");
    plat_file_close(f);
    CHECK(plat_file_create_at(sub, "w", 1, &f) == 0, "create rw");
    s = plat_file_stdio(f, "w+");
    if (s) { fputs("abc", s); rewind(s); CHECK(fgetc(s) == 'a', "w+ reads back"); fclose(s); }

    /* stat, identity, sizes */
    plat_stat st, st2, ds;
    CHECK(plat_stat_at(sub, "x", &st) == 0 && st.kind == PLAT_FILE && st.size == 10, "stat_at a file");
    CHECK(st.alloc >= 512 && st.alloc % 512 == 0, "alloc: blocks * 512 (%lld)", (long long)st.alloc);
    struct stat ps;
    path(p, sizeof p, "ab/x");
    stat(p, &ps);
    CHECK(plat_id_number(st.id) == (uint64_t)ps.st_ino && st.id.vol == (uint64_t)ps.st_dev, "id: dev and inode");
    CHECK(plat_stat_at(d, "link", &st2) == 0 && st2.kind == PLAT_OTHER, "a link is OTHER");
    CHECK(plat_stat_at(d, "ab", &st2) == 0 && st2.kind == PLAT_DIR && plat_dir_stat(sub, &ds) == 0 && plat_id_same(st2.id, ds.id),
          "a folder's identity, by name and by handle");
    CHECK(!plat_id_same(st.id, ds.id), "different files differ");
    CHECK(plat_stat_path(q, 0, &st2) == 0 && st2.kind == PLAT_OTHER && plat_stat_path(q, 1, &st2) == 0 && st2.kind == PLAT_DIR,
          "stat_path follows only when asked");
    plat_file h;
    CHECK(plat_file_open_at(sub, "x", 100, NULL, &h) == 0, "reopen");
    struct timeval old[2] = {{1000000000, 0}, {1000000000, 0}};
    utimes(p, old);
    CHECK(plat_file_stat(h, &st2) == 0 && st2.mtime == 1000000000, "an old modification time");
    CHECK(plat_file_touch(h) == 0 && plat_file_stat(h, &st2) == 0 && llabs(st2.mtime - (int64_t)time(NULL)) <= 2, "touch: now");
    plat_file_close(h);

    /* pread, fsync, touch_at (a link's own time), map_rw */
    CHECK(plat_file_open_at(sub, "x", 100, NULL, &h) == 0, "reopen for pread");
    CHECK(plat_pread(h, buf, 4, 3, &got) == 0 && got == 4 && !memcmp(buf, "3456", 4), "pread at 3");
    CHECK(plat_read(h, buf, 2, &got) == 0 && got == 2 && !memcmp(buf, "01", 2), "pread leaves the position");
    CHECK(plat_pread(h, buf, 4, 100, &got) == 0 && got == 0, "pread past the end");
    plat_file_close(h);
    utimes(p, old);
    char xl[1100];
    path(xl, sizeof xl, "ab/xlink");
    symlink(p, xl);
    CHECK(plat_touch_at(sub, "xlink") == 0, "touch_at a link");
    CHECK(plat_stat_path(p, 0, &st2) == 0 && st2.mtime == 1000000000, "the link's target keeps its time");
    CHECK(plat_touch_at(sub, "x") == 0 && plat_stat_path(p, 0, &st2) == 0 && llabs(st2.mtime - (int64_t)time(NULL)) <= 2,
          "touch_at a file: now");
    CHECK(plat_touch_at(sub, "nope") == ENOENT, "touch_at: ENOENT");
    plat_unlink_at(sub, "xlink");
    plat_file m;
    void *mp = NULL;
    CHECK(plat_file_create_at(sub, "map", 1, &m) == 0, "create for map");
    CHECK(plat_map_rw(m, 8192, &mp) == 0 && mp, "map_rw");
    if (mp) {
        memcpy((char *)mp + 5000, "mapped", 6);
        CHECK(plat_map_flush(mp, 8192) == 0 && plat_map_flush(NULL, 0) == 0, "map_flush (the Mac: nothing to do)");
        plat_unmap(mp, 8192);
    }
    CHECK(plat_fsync(m) == 0, "fsync");
    CHECK(plat_pread(m, buf, 6, 5000, &got) == 0 && got == 6 && !memcmp(buf, "mapped", 6), "map_rw's writes reach the file");
    CHECK(plat_file_stat(m, &st2) == 0 && st2.size == 8192, "map_rw sets the size");
    CHECK(plat_map_rw(m, 0, &mp) == EINVAL && !mp, "map_rw of 0 bytes: EINVAL");
    plat_file_close(m);
    plat_unlink_at(sub, "map");

    /* listing */
    listing l = {.n = 0};
    CHECK(plat_dir_each(d, list_fn, &l) == 0, "each: read whole");
    CHECK(l.n == 3 && listed(&l, "ab", PLAT_DIR) && listed(&l, "link", PLAT_OTHER) && listed(&l, "fifo", PLAT_OTHER),
          "each: the names but . and .. (%d)", l.n);
    listing l2 = {.n = 0, .stop_at = 2};
    e = plat_dir_each(d, list_fn, &l2);
    CHECK(e == -1 && l2.n == 2, "each: stopped by its function (%d, %d)", e, l2.n);
#if defined(__BLOCKS__)
    __block int files = 0;
    CHECK(plat_dir_each_b(sub, ^int(const char *name, const plat_stat *bst) { files += bst->kind == PLAT_FILE && name[0]; return 0; }) == 0
              && files == 2,
          "each_b (%d)", files);
#endif

    /* unlink, write access */
    CHECK(plat_unlink_at(sub, "w") == 0 && plat_unlink_at(sub, "w") == ENOENT, "unlink_at");
    CHECK(plat_unlink_at(d, "ab") == EISDIR, "unlink_at of a folder: EISDIR, as on Windows");
    path(p, sizeof p, "ab");
    CHECK(plat_unlink_path(p) == EISDIR, "unlink_path of a folder: EISDIR");
    CHECK(plat_unlink_at(d, "link") == 0 && symlink(p, q) == 0, "unlink_at of a link: the link goes");
    CHECK(plat_stat_at(d, "ab", &st0) == 0 && st0.kind == PLAT_DIR, "its folder stays");
    plat_volume rv;
    if (plat_volume_info(root, &rv) == 0 && !strcmp(rv.fstype, "apfs")) {
        e = plat_mkdir_at(d, "bad\xff");
        CHECK(e == EINVAL, "a name that isn't UTF-8: EINVAL (APFS's EILSEQ), as plat_win.c refuses it (%d)", e);
    } else printf("note: the scratch folder isn't on APFS: the invalid UTF-8 name check skipped\n");
    CHECK(plat_dir_writable(d) == 0, "the scratch folder is writable");
    CHECK(plat_mkdir_at(d, "ro") == 0, "ro");
    plat_dir ro;
    path(p, sizeof p, "ro");
    chmod(p, 0555);
    plat_dir_open_at(d, "ro", &ro);
    CHECK(geteuid() == 0 || plat_dir_writable(ro) == EACCES, "a folder without write permission: EACCES");
    plat_dir_close(ro);
    chmod(p, 0755);
    /* a folder that can be listed but not searched: -1 with the names' EACCES, not a stale 0 */
    CHECK(plat_mkdir_at(d, "ns") == 0, "ns");
    write_file("ns/one", "1");
    path(p, sizeof p, "ns");
    chmod(p, 0644);
    plat_dir ns;
    if (geteuid() != 0 && plat_dir_open_at(d, "ns", &ns) == 0) {
        listing l3 = {.n = 0};
        errno = 0;
        e = plat_dir_each(ns, list_fn, &l3);
        CHECK(e == -1 && errno == EACCES && l3.n == 0, "each of a folder without search permission: -1, EACCES (%d, %d, %d)", e, errno, l3.n);
        plat_dir_close(ns);
    } else CHECK(geteuid() == 0, "open ns");
    chmod(p, 0755);
    plat_dir_close(sub);

    /* paths: logs, state, config */
    path(p, sizeof p, "log");
    FILE *lf = plat_fopen_w(p, 1);
    if (lf) { fputs("one\n", lf); fclose(lf); }
    lf = plat_fopen_w(p, 1);
    if (lf) { fputs("two\n", lf); fclose(lf); }
    char *b = NULL;
    size_t n = 0;
    CHECK(plat_read_file(p, 100, &b, &n) == 0 && n == 8 && !memcmp(b, "one\ntwo\n", 8), "fopen_w appends; read_file");
    free(b);
    lf = plat_fopen_w(p, 0);
    if (lf) { fputs("3", lf); fclose(lf); }
    CHECK(plat_read_file(p, 100, &b, &n) == 0 && n == 1, "fopen_w truncates");
    free(b);
    CHECK(plat_read_file(p, 0, &b, &n) == PLAT_EFTYPE && !b, "read_file over max: EFTYPE");
    CHECK(plat_fopen_w(q, 0) == NULL && errno == ELOOP, "fopen_w refuses a link");
    path(q, sizeof q, "nope");
    CHECK(plat_read_file(q, 100, &b, &n) == ENOENT, "read_file: ENOENT");
    path(q, sizeof q, "ab");
    CHECK(plat_read_file(q, 100, &b, &n) == PLAT_EFTYPE, "read_file of a folder: EFTYPE");
    path(q, sizeof q, "loglink");
    symlink(p, q);
    CHECK(plat_read_file(q, 100, &b, &n) == 0 && n == 1, "read_file follows a link (config.ini may be one)");
    free(b);
    path(q, sizeof q, "log.1");
    CHECK(plat_rename_path(p, q) == 0 && plat_unlink_path(q) == 0 && plat_unlink_path(q) == ENOENT, "rename_path, unlink_path");
    path(p, sizeof p, "deep/er/still");
    plat_mkdirs(p);
    CHECK(plat_stat_path(p, 0, &st) == 0 && st.kind == PLAT_DIR, "mkdirs");
    plat_dir_close(d);
}

/* ---------------------------------------------------------------- volumes, process, account */

static void t_system(void) {
    plat_volume v;
    CHECK(plat_volume_info("/", &v) == 0 && !strcmp(v.mount, "/") && v.total > 0 && v.local, "volume of /: %s", v.mount);
    CHECK(plat_volume_info(root, &v) == 0 && v.avail > 0 && v.fstype[0], "volume of the scratch folder (%s)", v.fstype);
    char mnt[PLAT_MOUNT_MAX];
    int system = -1;
    CHECK(plat_volume_holding("/Users/someone/RB Stems Plus Cache.noindex", mnt, sizeof mnt, &system) == 0 && system == 1,
          "a folder in the home is on this Mac's volume (%s)", mnt);
    CHECK(plat_volume_mounted("/") && !plat_volume_mounted("/nonexistent-mount"), "volume_mounted");
    CHECK(plat_volume_holding("Users/someone", mnt, sizeof mnt, &system) == EINVAL, "volume_holding: an absolute path only");
    char one[1] = {'x'};
    CHECK(plat_volume_holding("/Users", one, sizeof one, &system) == ENAMETOOLONG && !one[0], "a mount point that doesn't fit: ENAMETOOLONG, \"\"");
    /* a path that is exactly a mount point is held by that mount, not by the one above it */
    plat_volume dv;
    if (plat_volume_info("/System/Volumes/Data", &dv) == 0 && !strcmp(dv.mount, "/System/Volumes/Data")) {
        CHECK(plat_volume_holding("/System/Volumes/Data", mnt, sizeof mnt, &system) == 0 && !strcmp(mnt, "/System/Volumes/Data")
                  && system == 1,
              "the exact mount point: %s", mnt);
        CHECK(plat_volume_holding("/System/Volumes/DataX/y", mnt, sizeof mnt, &system) == 0 && !strcmp(mnt, "/"),
              "a longer name isn't under it: %s", mnt);
    } else printf("note: no /System/Volumes/Data mount here: the exact-mount check skipped\n");
    const char *how = "";
    uint64_t fb = plat_free_bytes(root, 1, &how);
    /* Finder's count needs a system service a sandbox may deny: statfs's then */
    CHECK(fb > 0 && (!strcmp(how, "Finder's count") || !strcmp(how, "statfs")), "free bytes: %llu (%s)", (unsigned long long)fb, how);
    if (strcmp(how, "Finder's count")) printf("note: free bytes from statfs here, not Finder's count (a sandbox?)\n");
    fb = plat_free_bytes(root, 0, &how);
    CHECK(fb > 0 && !strcmp(how, "statfs"), "free bytes, statfs: %llu", (unsigned long long)fb);
    CHECK(plat_pid() == (int)getpid(), "pid");
    int64_t ps = plat_process_start();
    CHECK(ps > 0 && ps <= (int64_t)time(NULL) && ps > (int64_t)time(NULL) - 3600, "process start %lld", (long long)ps);
    char home[1024], small[4];
    CHECK(plat_home(home, sizeof home) == 0 && home[0] == '/', "home %s", home);
    CHECK(plat_home(small, sizeof small) == -1 && !small[0], "a home that doesn't fit: -1, \"\"");

    /* only root can change it: Pioneer's library's rule */
    char bad[1100], f[1100], dir[1100];
    CHECK(plat_owner_check("/usr/bin/true", 2, 0, bad, sizeof bad) == PLAT_OWNER_OK, "a system file: %s", bad);
    CHECK(plat_owner_check("/nonexistent/lib.dylib", 2, 0, bad, sizeof bad) == PLAT_OWNER_MISSING, "missing");
    CHECK(plat_owner_check("usr/bin/true", 2, 0, bad, sizeof bad) == PLAT_OWNER_REFUSED && !bad[0], "not absolute: refused, as on Windows");
    char *longp = malloc(PATH_MAX + 64);
    if (longp) {
        memset(longp, 'a', PATH_MAX + 63);
        longp[0] = '/';
        longp[PATH_MAX + 63] = 0;
        CHECK(plat_owner_check(longp, 2, 0, bad, sizeof bad) == PLAT_OWNER_REFUSED && !bad[0], "too long to check: refused");
        free(longp);
    }
    snprintf(dir, sizeof dir, "%s/own/lib", root);
    plat_mkdirs(dir);
    snprintf(f, sizeof f, "%s/own/lib/x.dylib", root);
    write_file("own/lib/x.dylib", "x");
    chmod(f, 0644);
    snprintf(bad, sizeof bad, "%s/own", root);
    chmod(bad, 0755);
    chmod(dir, 0755);
    CHECK(plat_owner_check(f, 2, 0, bad, sizeof bad) == PLAT_OWNER_FILE && strstr(bad, "/own/lib/x.dylib"), "the user's file: %s", bad);
    CHECK(plat_owner_check(f, 2, 1, bad, sizeof bad) == PLAT_OWNER_OK, "accepted as the test account's: %s", bad);
    chmod(dir, 0775);
    CHECK(plat_owner_check(f, 2, 1, bad, sizeof bad) == PLAT_OWNER_FOLDER && strstr(bad, "/own/lib") && !strstr(bad, ".dylib"),
          "a group-writable folder: %s", bad);
    chmod(dir, 0755);
    chmod(f, 0666);
    CHECK(plat_owner_check(f, 2, 1, bad, sizeof bad) == PLAT_OWNER_FILE, "a file others may write");

    /* libraries */
    const char *err = NULL;
    plat_lib *l = plat_lib_open("/usr/lib/libSystem.B.dylib", &err);
    CHECK(l && plat_lib_sym(l, "getpid") != NULL, "lib_open, lib_sym");
    CHECK(plat_lib_open("/nonexistent/lib.dylib", &err) == NULL && err && err[0], "lib_open fails with the loader's message");
    const char *where = plat_lib_path_of((const void *)&getpid);
    CHECK(where && strstr(where, "libsystem"), "lib_path_of: %s", where ? where : "NULL");
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s SCRATCH\n", argv[0]); return 2; }
    snprintf(root, sizeof root, "%s", argv[1]);
    t_sha256();
    t_irfft();
    t_locks_clocks();
    t_queues();
#ifdef PLAT_TEST_CLOCK
    t_fake_clock();
#endif
    t_files();
    t_system();
    printf("plat: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
