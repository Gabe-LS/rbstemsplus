/* The Phase 0 blocks spike (bridge.md §0 "Blocks"): does code shaped like the bridge's blocks build
 * and link with the pinned llvm-mingw, -fblocks and a vendored BlocksRuntime, as a DLL with no
 * extra import? The shapes the bridge uses: a global block, stack blocks passed to a function that
 * copies them for another thread (dispatch_async), __block variables shared with that thread
 * (load_index, prune), a block calling a block (each_name inside dispatch_apply), Block_copy and
 * Block_release. A tiny serial queue stands in for plat_win.c's (one thread, SRWLOCK +
 * CONDITION_VARIABLE). spike_run() returns 0 when every check passes; spike.exe calls it. */
#include <Block.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

typedef void (^job_b)(void);
typedef struct qjob { job_b b; struct qjob *next; } qjob;
static SRWLOCK lk = SRWLOCK_INIT;
static CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
static qjob *head, *tail;
static int stop, ran;

static DWORD WINAPI worker(void *p) {
    (void)p;
    for (;;) {
        AcquireSRWLockExclusive(&lk);
        while (!head && !stop) SleepConditionVariableSRW(&cv, &lk, INFINITE, 0);
        qjob *j = head;
        if (j) { head = j->next; if (!head) tail = NULL; }
        ReleaseSRWLockExclusive(&lk);
        if (!j) return 0;
        j->b();
        Block_release(j->b);
        free(j);
        AcquireSRWLockExclusive(&lk);
        ran++;
        WakeAllConditionVariable(&cv);
        ReleaseSRWLockExclusive(&lk);
    }
}

static void async_b(job_b b) {                  /* plat_async_b: the block copied to the heap */
    qjob *j = malloc(sizeof *j);
    j->b = Block_copy(b);
    j->next = NULL;
    AcquireSRWLockExclusive(&lk);
    if (tail) tail->next = j; else head = j;
    tail = j;
    WakeAllConditionVariable(&cv);
    ReleaseSRWLockExclusive(&lk);
}

static void wait_ran(int n) {
    AcquireSRWLockExclusive(&lk);
    while (ran < n) SleepConditionVariableSRW(&cv, &lk, INFINITE, 0);
    ReleaseSRWLockExclusive(&lk);
}

static int each(int n, int (^f)(int i)) {       /* each_name's shape */
    for (int i = 0; i < n; i++) if (f(i)) return -1;
    return 0;
}

__declspec(dllexport) int spike_run(void) {
    int fails = 0;
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!t) return 100;
    static void (^const global)(void) = ^{};     /* a global block */
    async_b(global);
    __block int sum = 0;
    __block char *msg = NULL;
    for (int k = 1; k <= 100; k++) async_b(^{ sum += k; });   /* stack blocks capturing k, a __block sum */
    char *text = malloc(16);
    snprintf(text, 16, "bridge");
    async_b(^{ msg = text; });                  /* capture a pointer, set a __block one */
    __block int found = 0;
    async_b(^{                                  /* a block running a block (prune: each_name in a job) */
        found = each(10, ^int(int i) { return i == 7 ? (found = i, 1) : 0; }) == -1 ? found : -1;
    });
    wait_ran(103);
    if (sum != 5050) { printf("sum %d\n", sum); fails++; }
    if (!msg || strcmp(msg, "bridge")) { printf("msg\n"); fails++; }
    if (found != 7) { printf("found %d\n", found); fails++; }
    AcquireSRWLockExclusive(&lk);
    stop = 1;
    WakeAllConditionVariable(&cv);
    ReleaseSRWLockExclusive(&lk);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    free(text);
    return fails;
}
