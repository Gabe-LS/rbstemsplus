/* A minimal stand-in for common/plat.h (bridge §0a), which doesn't exist yet: thread identity,
 * a mutex and a condition, a monotonic and a wall clock, and a sleep. The core reaches threads and
 * time only through these, so moving to plat.h (and to its fake clock in the scenario tests) is a
 * rename, with no API change. plat.h's timed wait and its cancellable timer are here too, with
 * plat.h's names and contract (the timer runs fn on a thread of its own: q must be NULL, which
 * on plat.h is its maintenance queue); the fake clock is plat.h's only.
 *
 * The Windows branch (SRWLOCK, CONDITION_VARIABLE, GetCurrentThreadId: no libwinpthread) is
 * written but unverified: no Windows toolchain on this host. */
#ifndef RBSP_CORE_PLAT_SHIM_H
#define RBSP_CORE_PLAT_SHIM_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

struct plat_timer;
static inline void plat_timer_run(struct plat_timer *t);

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef DWORD plat_thread_id;
typedef SRWLOCK plat_mutex;
typedef CONDITION_VARIABLE plat_cond;
static inline plat_thread_id plat_thread_self(void) { return GetCurrentThreadId(); }
static inline int plat_thread_equal(plat_thread_id a, plat_thread_id b) { return a == b; }
static inline void plat_mutex_init(plat_mutex *m) { InitializeSRWLock(m); }
static inline void plat_mutex_destroy(plat_mutex *m) { (void)m; }
static inline void plat_mutex_lock(plat_mutex *m) { AcquireSRWLockExclusive(m); }
static inline void plat_mutex_unlock(plat_mutex *m) { ReleaseSRWLockExclusive(m); }
static inline void plat_cond_init(plat_cond *c) { InitializeConditionVariable(c); }
static inline void plat_cond_destroy(plat_cond *c) { (void)c; }
static inline void plat_cond_wait(plat_cond *c, plat_mutex *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static inline void plat_cond_signal(plat_cond *c) { WakeConditionVariable(c); }
static inline uint64_t plat_now_mono_ns(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
static inline void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
/* waits on c until signalled or about ns have passed: 0 signalled (or spurious), ETIMEDOUT */
static inline int plat_cond_wait_ns(plat_cond *c, plat_mutex *m, uint64_t ns) {
    uint64_t ms = (ns + 999999u) / 1000000u;
    return SleepConditionVariableSRW(c, m, ms >= INFINITE ? INFINITE - 1 : (DWORD)ms, 0) ? 0 : ETIMEDOUT;
}
static inline DWORD WINAPI plat_timer_thread(LPVOID t) {
    plat_timer_run(t);
    return 0;
}
static inline int plat_timer_thread_start(struct plat_timer *t) {
    HANDLE h = CreateThread(NULL, 0, plat_timer_thread, t, 0, NULL);
    if (!h) return -1;
    CloseHandle(h);
    return 0;
}
#else
#  include <pthread.h>
#  include <time.h>
typedef pthread_t plat_thread_id;
typedef pthread_mutex_t plat_mutex;
typedef pthread_cond_t plat_cond;
static inline plat_thread_id plat_thread_self(void) { return pthread_self(); }
static inline int plat_thread_equal(plat_thread_id a, plat_thread_id b) { return pthread_equal(a, b) != 0; }
static inline void plat_mutex_init(plat_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void plat_mutex_destroy(plat_mutex *m) { pthread_mutex_destroy(m); }
static inline void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(m); }
static inline void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(m); }
#if defined(__APPLE__)
static inline void plat_cond_init(plat_cond *c) { pthread_cond_init(c, NULL); }
#else
static inline void plat_cond_init(plat_cond *c) {   /* timed waits on the monotonic clock (plat_cond_wait_ns) */
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}
#endif
static inline void plat_cond_destroy(plat_cond *c) { pthread_cond_destroy(c); }
static inline void plat_cond_wait(plat_cond *c, plat_mutex *m) { pthread_cond_wait(c, m); }
static inline void plat_cond_signal(plat_cond *c) { pthread_cond_signal(c); }
static inline uint64_t plat_now_mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static inline void plat_sleep_ms(uint32_t ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
/* waits on c until signalled or about ns have passed: 0 signalled (or spurious), ETIMEDOUT */
static inline int plat_cond_wait_ns(plat_cond *c, plat_mutex *m, uint64_t ns) {
#if defined(__APPLE__)   /* a relative wait, as common/plat_mac.c's: the wall clock never enters */
    struct timespec rel = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    return pthread_cond_timedwait_relative_np(c, m, &rel) == ETIMEDOUT ? ETIMEDOUT : 0;
#else   /* plat_cond_init put c on the monotonic clock */
    struct timespec now, at;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t t = (uint64_t)now.tv_nsec + ns % 1000000000u;
    at.tv_sec = now.tv_sec + (time_t)(ns / 1000000000u) + (time_t)(t / 1000000000u);
    at.tv_nsec = (long)(t % 1000000000u);
    return pthread_cond_timedwait(c, m, &at) == ETIMEDOUT ? ETIMEDOUT : 0;
#endif
}
static inline void *plat_timer_thread(void *t) {
    plat_timer_run(t);
    return NULL;
}
static inline int plat_timer_thread_start(struct plat_timer *t) {
    pthread_t th;
    pthread_attr_t at;
    if (pthread_attr_init(&at)) return -1;
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&th, &at, plat_timer_thread, t);
    pthread_attr_destroy(&at);
    return r ? -1 : 0;
}
#endif

/* plat.h's cancellable timer: fn(ctx) runs once after delay_ns unless plat_timer_cancel comes
 * first. The owner calls plat_timer_cancel exactly once per timer, fired or not, and never uses
 * it after: 1 when fn will never run, 0 when it has run or is running. NULL: not made. */
typedef struct plat_queue plat_queue;
typedef void (*plat_fn)(void *ctx);
typedef struct plat_timer {
    plat_mutex mu;
    plat_cond cv;
    int state;   /* 0 waiting, 1 cancelled, 2 fired */
    int refs;    /* the thread's and the owner's */
    uint64_t due;
    plat_fn fn;
    void *ctx;
} plat_timer;

static inline void plat_timer_unref(plat_timer *t) {   /* called holding t->mu, which it releases */
    int last = --t->refs == 0;
    plat_mutex_unlock(&t->mu);
    if (!last) return;
    plat_cond_destroy(&t->cv);
    plat_mutex_destroy(&t->mu);
    free(t);
}

static inline void plat_timer_run(struct plat_timer *t) {
    plat_mutex_lock(&t->mu);
    for (uint64_t now; !t->state && (now = plat_now_mono_ns()) < t->due;) plat_cond_wait_ns(&t->cv, &t->mu, t->due - now);
    int fire = !t->state;
    if (fire) t->state = 2;
    plat_mutex_unlock(&t->mu);
    if (fire) t->fn(t->ctx);
    plat_mutex_lock(&t->mu);
    plat_timer_unref(t);
}

static inline plat_timer *plat_timer_new(plat_queue *q, uint64_t delay_ns, plat_fn fn, void *ctx) {
    (void)q;
    plat_timer *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    plat_mutex_init(&t->mu);
    plat_cond_init(&t->cv);
    t->refs = 2;
    t->due = plat_now_mono_ns() + delay_ns;
    t->fn = fn;
    t->ctx = ctx;
    if (plat_timer_thread_start(t)) {
        plat_cond_destroy(&t->cv);
        plat_mutex_destroy(&t->mu);
        free(t);
        return NULL;
    }
    return t;
}

static inline int plat_timer_cancel(plat_timer *t) {
    plat_mutex_lock(&t->mu);
    int never = !t->state;
    if (never) {
        t->state = 1;
        plat_cond_signal(&t->cv);
    }
    plat_timer_unref(t);
    return never;
}

#endif
