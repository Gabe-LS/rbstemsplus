/* core/plat_shim.h's Windows branch on its own (W0): SRWLOCK as the mutex, CONDITION_VARIABLE as
 * the condition, GetCurrentThreadId as the identity, QueryPerformanceCounter as the clock, Sleep.
 * 8 threads add under the mutex; a ping-pong of 20,000 hand-offs through one condition, the shape
 * of flow.c's flow_query (wait until done); the identity and the clock. Exit 0 when all pass.
 * Built by tests/win/w0/build.sh with scripts/win-cc.sh, no libwinpthread. */
#include "../../../core/plat_shim.h"
#include <stdio.h>

static plat_mutex mu;
static plat_cond cv;
static long counter;
static int turn, rounds_done;
static plat_thread_id ids[9];
enum { THREADS = 8, ADDS = 200000, PINGS = 20000 };

static DWORD WINAPI adder(LPVOID arg) {
    ids[(int)(intptr_t)arg] = plat_thread_self();
    for (int i = 0; i < ADDS; i++) {
        plat_mutex_lock(&mu);
        counter++;
        plat_mutex_unlock(&mu);
    }
    return 0;
}

static DWORD WINAPI ponger(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < PINGS; i++) {
        plat_mutex_lock(&mu);
        while (turn != 1) plat_cond_wait(&cv, &mu);
        turn = 0;
        rounds_done++;
        plat_cond_signal(&cv);
        plat_mutex_unlock(&mu);
    }
    return 0;
}

int main(void) {
    int fails = 0;
    plat_mutex_init(&mu);
    plat_cond_init(&cv);
    HANDLE h[THREADS];
    uint64_t t0 = plat_now_mono_ns();
    for (int i = 0; i < THREADS; i++) h[i] = CreateThread(NULL, 0, adder, (LPVOID)(intptr_t)i, 0, NULL);
    WaitForMultipleObjects(THREADS, h, TRUE, INFINITE);
    uint64_t t1 = plat_now_mono_ns();
    for (int i = 0; i < THREADS; i++) CloseHandle(h[i]);
    int ok = counter == (long)THREADS * ADDS;
    fails += !ok;
    printf("%s mutex: %ld adds from %d threads (%.1f ms)\n", ok ? "PASS" : "FAIL", counter, THREADS, (double)(t1 - t0) / 1e6);

    ids[8] = plat_thread_self();
    int distinct = 1;
    for (int i = 0; i < 9; i++)
        for (int j = i + 1; j < 9; j++)
            if (plat_thread_equal(ids[i], ids[j])) distinct = 0;
    ok = distinct && plat_thread_equal(plat_thread_self(), ids[8]);
    fails += !ok;
    printf("%s thread identity: 9 distinct ids, self equal to self\n", ok ? "PASS" : "FAIL");

    HANDLE p = CreateThread(NULL, 0, ponger, NULL, 0, NULL);
    uint64_t t2 = plat_now_mono_ns();
    for (int i = 0; i < PINGS; i++) {
        plat_mutex_lock(&mu);
        turn = 1;
        plat_cond_signal(&cv);
        while (turn != 0) plat_cond_wait(&cv, &mu);
        plat_mutex_unlock(&mu);
    }
    uint64_t t3 = plat_now_mono_ns();
    WaitForSingleObject(p, INFINITE);
    CloseHandle(p);
    ok = rounds_done == PINGS;
    fails += !ok;
    printf("%s condition: %d hand-offs (%.2f us each)\n", ok ? "PASS" : "FAIL", rounds_done, (double)(t3 - t2) / 1e3 / PINGS);

    uint64_t a = plat_now_mono_ns();
    plat_sleep_ms(50);
    uint64_t b = plat_now_mono_ns();
    double ms = (double)(b - a) / 1e6;
    ok = b > a && ms >= 45 && ms < 500;
    fails += !ok;
    printf("%s clock and sleep: plat_sleep_ms(50) took %.2f ms on the monotonic clock\n", ok ? "PASS" : "FAIL", ms);

    plat_cond_destroy(&cv);
    plat_mutex_destroy(&mu);
    printf("shimtest: %s\n", fails ? "FAILED" : "passed");
    return fails;
}
