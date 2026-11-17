/* plat.h on Windows (bridge.md §1, tasks T3a-T3d): the Mac's plat_mac.c's twin, for x86_64 (the
 * bridge, the helper, the admin worker) and aarch64 (the arm64 core DLL).
 *
 *  - Files: the NT layer, relative to a folder's handle (NtCreateFile with RootDirectory and
 *    FILE_OPEN_REPARSE_POINT; a reparse point at the last component is refused as the Mac's
 *    O_NOFOLLOW refuses a link: ELOOP). Every open shares READ|WRITE|DELETE, so a rename or a
 *    delete by another process is never blocked by us. Errors come from the NTSTATUS through
 *    win_rules.h's map (I83, r4:I47). Absolute paths are opened as NT paths ("\??\X:\..."), so a
 *    DLL needs no long-path opt-in; only the follow-link opens (plat_stat_path with follow,
 *    plat_file_open_path) also take relative, dotted and UNC paths (wr_full_path). Listing is one pass of FileIdExtdDirectoryInformation
 *    (128-bit ids), FileIdBothDirectoryInformation where the folder has no FileIdInformation
 *    (FAT, exFAT).
 *  - Deletes: FileDispositionInformationEx with POSIX semantics where the file system has it,
 *    plain dispose on FAT/exFAT; renames: FileRenameInformationEx with REPLACE_IF_EXISTS and
 *    POSIX semantics, else FileRenameInformation. On a queue thread (writer, maintenance) an
 *    EBUSY is retried 8 times, 125 ms apart, then given up (bridge.md §1).
 *  - Queues: one lazily created thread per serial queue, an SRWLOCK + CONDITION_VARIABLE job
 *    list, delayed jobs sorted by due time. A MAINT queue's thread runs at
 *    THREAD_PRIORITY_BELOW_NORMAL (never background mode), and every handle the file layer opens
 *    under the maintenance work class gets the low I/O priority hint; plat_par_for's workers
 *    inherit the caller's priority and work class (r4:I51).
 *  - sha256: BCrypt through BCRYPT_SHA256_ALG_HANDLE; bcrypt.dll loaded on first use by its full
 *    System32 path, never a static import (I31).
 *  - The inverse real FFT: pocketfft (BSD-3-Clause, release.md §9.2's pin), in double.
 *  - Owner check: the descriptor read from the handle with NtQuerySecurityObject (no on-demand
 *    DLL load: phase 1 of the bridge's start runs it, possibly under the loader lock), every
 *    handle it reads opened with READ_CONTROL (J8, r4:I48).
 *
 * Nothing here runs at DLL load: threads, bcrypt and every static are made on first use (R3).
 * Paths are UTF-8 and converted strictly; an invalid one is EINVAL. */
#include "plat.h"
#include "plat_win.h"
#include "win_rules.h"

#include <winternl.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <fcntl.h>
#include <io.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "pocketfft.h"

#if defined(__BLOCKS__) && !defined(PLAT_NO_BLOCKS)
#include <Block.h>
#endif

/* ---------------------------------------------------------------- NT declarations
 * What mingw-w64's winternl.h lacks, written from Microsoft's documentation (ntifs.h, wdm.h). */

NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                    PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
                                    FILE_INFORMATION_CLASS FileInformationClass, BOOLEAN ReturnSingleEntry,
                                    PUNICODE_STRING FileName, BOOLEAN RestartScan);
NTSTATUS NTAPI NtQuerySecurityObject(HANDLE Handle, SECURITY_INFORMATION SecurityInformation,
                                     PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG Length, PULONG LengthNeeded);

#define PW_FileBasicInformation 4
#define PW_FileStandardInformation 5
#define PW_FileInternalInformation 6
#define PW_FileNameInformation 9
#define PW_FileRenameInformation 10
#define PW_FileDispositionInformation 13
#define PW_FileEndOfFileInformation 20
#define PW_FileAttributeTagInformation 35
#define PW_FileIdBothDirectoryInformation 37
#define PW_FileIoPriorityHintInformation 43
#define PW_FileIdInformation 59
#define PW_FileIdExtdDirectoryInformation 60
#define PW_FileDispositionInformationEx 64
#define PW_FileRenameInformationEx 65
#define PW_FileFsVolumeInformation 1
#define PW_FileFsAttributeInformation 5
#define PW_CLASS(c) ((FILE_INFORMATION_CLASS)(c))

typedef struct { LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime; ULONG FileAttributes; } pw_basic_info;
typedef struct { LARGE_INTEGER AllocationSize, EndOfFile; ULONG NumberOfLinks; BOOLEAN DeletePending, Directory; } pw_standard_info;
typedef struct { ULONG FileAttributes, ReparseTag; } pw_tag_info;
typedef struct { ULONGLONG VolumeSerialNumber; BYTE FileId[16]; } pw_id_info;
typedef struct { LARGE_INTEGER VolumeCreationTime; ULONG VolumeSerialNumber, VolumeLabelLength; BOOLEAN SupportsObjects; WCHAR VolumeLabel[1]; } pw_fs_volume_info;
typedef struct { ULONG FileSystemAttributes; LONG MaximumComponentNameLength; ULONG FileSystemNameLength; WCHAR FileSystemName[1]; } pw_fs_attr_info;
typedef struct {
    ULONG NextEntryOffset, FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
    ULONG FileAttributes, FileNameLength, EaSize;
    CCHAR ShortNameLength;
    WCHAR ShortName[12];
    LARGE_INTEGER FileId;
    WCHAR FileName[1];
} pw_dir_info;
/* FileIdExtdDirectoryInformation's entry: the 128-bit id FileIdInformation gives (ReFS's ids
 * don't fit FileIdBothDirectoryInformation's 64 bits) */
typedef struct {
    ULONG NextEntryOffset, FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
    ULONG FileAttributes, FileNameLength, EaSize, ReparsePointTag;
    BYTE FileId[16];
    WCHAR FileName[1];
} pw_dir_info_extd;
_Static_assert(offsetof(pw_dir_info_extd, FileName) == 88, "FILE_ID_EXTD_DIR_INFORMATION's layout");
/* FileRenameInformation's first member is a BOOLEAN ReplaceIfExists, FileRenameInformationEx's a
 * ULONG Flags in the same place: Flags = 1 is both "replace" */
typedef struct { ULONG Flags; HANDLE RootDirectory; ULONG FileNameLength; WCHAR FileName[1]; } pw_rename_info;
#if defined(_WIN64)
/* FILE_RENAME_INFORMATION(_EX)'s 64-bit layout, which wr_info_len's minimum is about (win_rules.c
 * checks the same numbers on the Mac) */
_Static_assert(offsetof(pw_rename_info, FileName) == 20 && sizeof(pw_rename_info) == 24, "FILE_RENAME_INFORMATION's layout");
#endif
#define PW_RENAME_REPLACE 0x1u
#define PW_RENAME_POSIX 0x2u
#define PW_RENAME_IGNORE_READONLY 0x40u
#define PW_DISPOSE_DELETE 0x1u
#define PW_DISPOSE_POSIX 0x2u
#define PW_DISPOSE_IGNORE_READONLY 0x10u
#define PW_STATUS_NO_MORE_FILES 0x80000006u
#define PW_STATUS_BUFFER_OVERFLOW 0x80000005u
#define PW_STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define PW_STATUS_INVALID_PARAMETER 0xC000000Du
#define PW_FILE_PERSISTENT_ACLS 0x00000008u
#define PW_FILE_READ_ONLY_VOLUME 0x00080000u
#define PW_SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

/* an NTSTATUS a newer information class gets from a file system that lacks it (FAT, exFAT, older
 * NTFS): the caller falls back to the classic class */
static int pw_unsupported(NTSTATUS s) {
    uint32_t u = (uint32_t)s;
    return u == 0xC0000003u /* INVALID_INFO_CLASS */ || u == 0xC000000Du /* INVALID_PARAMETER */
           || u == 0xC00000BBu /* NOT_SUPPORTED */ || u == 0xC0000002u /* NOT_IMPLEMENTED */
           || u == 0xC0000010u /* INVALID_DEVICE_REQUEST */;
}

static int pw_nt(NTSTATUS s, int op) { return wr_errno_nt((uint32_t)s, op); }
static int pw_last(int op) { return wr_errno_win32((uint32_t)GetLastError(), op); }

/* ---------------------------------------------------------------- the thread's work class */

static _Thread_local int pw_work;       /* PLAT_WORK_MAINT on a maintenance queue's thread and its par_for workers */
static _Thread_local int pw_on_queue;   /* a queue's thread (or its workers): EBUSY is retried here */

/* ---------------------------------------------------------------- threads, locks, once */

typedef struct { void (*fn)(void); } pw_once_arg;
static BOOL CALLBACK pw_once_cb(PINIT_ONCE o, PVOID param, PVOID *ctx) {
    (void)o; (void)ctx;
    ((pw_once_arg *)param)->fn();
    return TRUE;
}
COMMON_API void plat_once(plat_once_t *o, void (*fn)(void)) {
    pw_once_arg a = {fn};
    InitOnceExecuteOnce(o, pw_once_cb, &a, NULL);
}

static uint64_t pw_mono_real(void);
COMMON_API int plat_cond_wait_ns(plat_cond *c, plat_mutex *m, uint64_t ns) {
    /* a timed-out wait can end up to a timer tick early on the performance counter (measured in
     * the VM: 16.6 ms for 30): it waits again for what is left, so ETIMEDOUT never comes early. A
     * signal sent while a timed-out wait takes m back is lost (as on any condition variable): the
     * caller sees it at the deadline, at most what was left of ns later */
    uint64_t now = pw_mono_real(), end = ns > UINT64_MAX - now ? UINT64_MAX : now + ns;
    uint32_t ms = wr_ms_of_ns(ns);
    for (;;) {
        if (SleepConditionVariableSRW(c, m, ms, 0)) return 0;
        if (GetLastError() != ERROR_TIMEOUT) return 0;
        now = pw_mono_real();
        if (now >= end) return ETIMEDOUT;          /* a capped (49.7-day) chunk waits again */
        ms = wr_ms_of_ns(end - now);
    }
}

/* ---------------------------------------------------------------- clocks, the fake clock */

#ifdef PLAT_TEST_CLOCK
typedef struct pw_fake_job { uint64_t due; plat_fn fn; void *ctx; struct pw_fake_job *next; } pw_fake_job;
static plat_mutex pw_fake_lock = PLAT_MUTEX_INIT;
static int pw_fake_on;
static uint64_t pw_fake_mono;
static int64_t pw_fake_wall;
static pw_fake_job *pw_fake_jobs;

COMMON_API void plat_fake_clock(int on, uint64_t mono_ns, int64_t wall_ns) {
    plat_mutex_lock(&pw_fake_lock);
    pw_fake_on = on;
    pw_fake_mono = mono_ns;
    pw_fake_wall = wall_ns;
    plat_mutex_unlock(&pw_fake_lock);
}

static int pw_fake_arm(uint64_t delay_ns, plat_fn fn, void *ctx, int *err) {
    plat_mutex_lock(&pw_fake_lock);
    if (!pw_fake_on) { plat_mutex_unlock(&pw_fake_lock); return 0; }
    pw_fake_job *j = malloc(sizeof *j), **at = &pw_fake_jobs;
    if (!j) { plat_mutex_unlock(&pw_fake_lock); *err = ENOMEM; return 1; }
    /* a deadline past the clock's end is UINT64_MAX, which never comes */
    j->due = delay_ns > UINT64_MAX - pw_fake_mono ? UINT64_MAX : pw_fake_mono + delay_ns;
    j->fn = fn;
    j->ctx = ctx;
    while (*at && (*at)->due <= j->due) at = &(*at)->next;
    j->next = *at;
    *at = j;
    plat_mutex_unlock(&pw_fake_lock);
    *err = 0;
    return 1;
}

COMMON_API void plat_fake_advance(uint64_t ns) {
    plat_mutex_lock(&pw_fake_lock);
    uint64_t end = ns > UINT64_MAX - pw_fake_mono ? UINT64_MAX : pw_fake_mono + ns;
    while (pw_fake_jobs && pw_fake_jobs->due <= end && pw_fake_jobs->due != UINT64_MAX) {
        pw_fake_job *j = pw_fake_jobs;
        pw_fake_jobs = j->next;
        if (j->due > pw_fake_mono) {
            pw_fake_wall += (int64_t)(j->due - pw_fake_mono);
            pw_fake_mono = j->due;
        }
        plat_mutex_unlock(&pw_fake_lock);
        j->fn(j->ctx);
        free(j);
        plat_mutex_lock(&pw_fake_lock);
    }
    pw_fake_wall += (int64_t)(end - pw_fake_mono);
    pw_fake_mono = end;
    plat_mutex_unlock(&pw_fake_lock);
}

static int pw_timer_cancelled(void *t);
static void pw_timer_fire(void *p);
COMMON_API size_t plat_fake_pending(void) {
    size_t n = 0;
    plat_mutex_lock(&pw_fake_lock);
    for (pw_fake_job *j = pw_fake_jobs; j; j = j->next) n += !(j->fn == pw_timer_fire && pw_timer_cancelled(j->ctx));
    plat_mutex_unlock(&pw_fake_lock);
    return n;
}
#endif

static uint64_t pw_mono_real(void) {
    LARGE_INTEGER freq, c;                                  /* both read the shared user page: cheap */
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t)freq.QuadPart, v = (uint64_t)c.QuadPart;
    return v / f * 1000000000u + v % f * 1000000000u / f;
}

COMMON_API uint64_t plat_now_mono_ns(void) {
#ifdef PLAT_TEST_CLOCK
    plat_mutex_lock(&pw_fake_lock);
    int on = pw_fake_on;
    uint64_t v = pw_fake_mono;
    plat_mutex_unlock(&pw_fake_lock);
    if (on) return v;
#endif
    return pw_mono_real();
}

static int64_t pw_filetime_now(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}

COMMON_API int64_t plat_now_wall_ns(void) {
#ifdef PLAT_TEST_CLOCK
    plat_mutex_lock(&pw_fake_lock);
    int on = pw_fake_on;
    int64_t v = pw_fake_wall;
    plat_mutex_unlock(&pw_fake_lock);
    if (on) return v;
#endif
    return wr_filetime_ns(pw_filetime_now());
}

COMMON_API int64_t plat_now_wall_s(void) {
#ifdef PLAT_TEST_CLOCK
    plat_mutex_lock(&pw_fake_lock);
    int on = pw_fake_on;
    int64_t v = pw_fake_wall;
    plat_mutex_unlock(&pw_fake_lock);
    if (on) return v >= 0 ? v / 1000000000 : -((-v + 999999999) / 1000000000);
#endif
    return wr_filetime_s(pw_filetime_now());
}

COMMON_API void plat_localtime(int64_t t, struct tm *out) {
    __time64_t tt = (__time64_t)t;
    if (_localtime64_s(out, &tt)) memset(out, 0, sizeof *out);
}

/* ---------------------------------------------------------------- queues */

/* on_stack: plat_queue_wait's barrier, on its caller's stack: never freed, and never touched
 * after its fn has run (its caller may have returned by then) */
typedef struct pw_job { uint64_t due; plat_fn fn; void *ctx; struct pw_job *next; int on_stack; } pw_job;
struct plat_queue {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    pw_job *head, *tail;                /* ready, in the order queued */
    pw_job *later;                      /* delayed, by due time then the order armed */
    int work, started;
};

static DWORD WINAPI pw_queue_main(LPVOID p) {
    plat_queue *q = p;
    pw_work = q->work;
    pw_on_queue = 1;
    if (q->work == PLAT_WORK_MAINT) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    AcquireSRWLockExclusive(&q->lock);
    for (;;) {
        uint64_t now = pw_mono_real();
        while (q->later && q->later->due <= now) {          /* due delayed jobs join the ready ones */
            pw_job *j = q->later;
            q->later = j->next;
            j->next = NULL;
            if (q->tail) q->tail->next = j; else q->head = j;
            q->tail = j;
        }
        if (q->head) {
            pw_job *j = q->head;
            if (!(q->head = j->next)) q->tail = NULL;
            plat_fn fn = j->fn;                             /* copied under the lock: a barrier job */
            void *ctx = j->ctx;                             /* is gone once its fn has run */
            int keep = j->on_stack;
            ReleaseSRWLockExclusive(&q->lock);
            fn(ctx);
            if (!keep) free(j);
            AcquireSRWLockExclusive(&q->lock);
            continue;
        }
        DWORD wait = INFINITE;
        if (q->later) wait = wr_ms_of_ns(q->later->due - now);
        SleepConditionVariableSRW(&q->cv, &q->lock, wait, 0);
    }
    return 0;
}

COMMON_API plat_queue *plat_queue_new(const char *label, int work) {
    (void)label;                                            /* GCD's label: nothing to name here */
    plat_queue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    InitializeSRWLock(&q->lock);
    InitializeConditionVariable(&q->cv);
    q->work = work;
    return q;
}

/* adds j (ready when delay is 0, else delayed), starting the queue's thread on its first job:
 * never at DLL load (R3). 0, or an errno (j freed, unless it is on its caller's stack). */
static int pw_enqueue(plat_queue *q, pw_job *j, int delayed) {
    if (!q) {                                               /* plat_queue_new failed: nothing runs (BW5) */
        if (!j->on_stack) free(j);
        return EINVAL;
    }
    AcquireSRWLockExclusive(&q->lock);
    if (!q->started) {
        HANDLE t = CreateThread(NULL, 0, pw_queue_main, q, 0, NULL);
        if (!t) {
            ReleaseSRWLockExclusive(&q->lock);
            if (!j->on_stack) free(j);
            return EAGAIN;
        }
        CloseHandle(t);
        q->started = 1;
    }
    j->next = NULL;
    if (!delayed) {
        if (q->tail) q->tail->next = j; else q->head = j;
        q->tail = j;
    } else {
        pw_job **at = &q->later;
        while (*at && (*at)->due <= j->due) at = &(*at)->next;
        j->next = *at;
        *at = j;
    }
    ReleaseSRWLockExclusive(&q->lock);
    WakeConditionVariable(&q->cv);
    return 0;
}

COMMON_API int plat_async(plat_queue *q, plat_fn fn, void *ctx) {
    pw_job *j = malloc(sizeof *j);
    if (!j) return ENOMEM;
    j->due = 0;
    j->fn = fn;
    j->ctx = ctx;
    j->on_stack = 0;
    return pw_enqueue(q, j, 0);
}

COMMON_API int plat_after(plat_queue *q, uint64_t delay_ns, plat_fn fn, void *ctx) {
#ifdef PLAT_TEST_CLOCK
    int err;
    if (pw_fake_arm(delay_ns, fn, ctx, &err)) return err;
#endif
    pw_job *j = malloc(sizeof *j);
    if (!j) return ENOMEM;
    uint64_t now = pw_mono_real();                          /* past the clock's end: UINT64_MAX, never */
    j->due = delay_ns > UINT64_MAX - now ? UINT64_MAX : now + delay_ns;
    j->fn = fn;
    j->ctx = ctx;
    j->on_stack = 0;
    return pw_enqueue(q, j, 1);
}

typedef struct { SRWLOCK l; CONDITION_VARIABLE c; int done; } pw_waiter;
static void pw_wake(void *p) {
    pw_waiter *w = p;
    AcquireSRWLockExclusive(&w->l);
    w->done = 1;
    WakeAllConditionVariable(&w->c);                        /* before the unlock, so the unlock is the last */
    ReleaseSRWLockExclusive(&w->l);                         /* access to *w (its waiter may then return) */
}

COMMON_API void plat_queue_wait(plat_queue *q) {
    pw_waiter w = {SRWLOCK_INIT, CONDITION_VARIABLE_INIT, 0};
    pw_job j = {0, pw_wake, &w, NULL, 1};                   /* allocation-free: only EAGAIN (or no queue) refuses it */
    if (pw_enqueue(q, &j, 0)) return;                       /* no thread: nothing queued will run either */
    AcquireSRWLockExclusive(&w.l);
    while (!w.done) SleepConditionVariableSRW(&w.c, &w.l, INFINITE, 0);
    ReleaseSRWLockExclusive(&w.l);
}

typedef struct {
    void (*fn)(void *, size_t);
    void *ctx;
    size_t n;
    atomic_size_t next;
    int priority, work, on_queue;
} pw_par;

static void pw_par_run(pw_par *p) {
    size_t i;
    while ((i = atomic_fetch_add(&p->next, 1)) < p->n) p->fn(p->ctx, i);
}

static DWORD WINAPI pw_par_main(LPVOID v) {
    pw_par *p = v;
    pw_work = p->work;
    pw_on_queue = p->on_queue;
    SetThreadPriority(GetCurrentThread(), p->priority);
    pw_par_run(p);
    return 0;
}

COMMON_API void plat_par_for(size_t n, int work, void (*fn)(void *ctx, size_t i), void *ctx) {
    /* min(8, n) threads, the caller one of them; each worker at the caller's priority and work
     * class (bridge.md §1, r4:I51). A worker that can't be started leaves its share to the others. */
    if (n == 0) return;
    pw_par p = {fn, ctx, n, 0, GetThreadPriority(GetCurrentThread()), 0, pw_on_queue};
    p.work = work == PLAT_WORK_MAINT || pw_work == PLAT_WORK_MAINT ? PLAT_WORK_MAINT : PLAT_WORK_DEFAULT;
    if (p.priority == THREAD_PRIORITY_ERROR_RETURN) p.priority = THREAD_PRIORITY_NORMAL;
    if (p.work == PLAT_WORK_MAINT && p.priority > THREAD_PRIORITY_BELOW_NORMAL) p.priority = THREAD_PRIORITY_BELOW_NORMAL;
    HANDLE t[7];
    DWORD k = 0;
    for (size_t i = 1; i < n && i < 8; i++) {
        HANDLE h = CreateThread(NULL, 0, pw_par_main, &p, 0, NULL);
        if (h) t[k++] = h;
    }
    int saved_work = pw_work;
    pw_work = p.work;                                       /* the caller's own share, as a worker */
    pw_par_run(&p);
    pw_work = saved_work;
    if (k) WaitForMultipleObjects(k, t, TRUE, INFINITE);
    for (DWORD i = 0; i < k; i++) CloseHandle(t[i]);
}

COMMON_API int plat_work_class(void) { return pw_work; }

/* ---------------------------------------------------------------- timers (as on the Mac) */

struct plat_timer { _Atomic int state, refs; plat_fn fn; void *ctx; };
enum { T_ARMED, T_FIRED, T_CANCELLED };

static void pw_timer_release(plat_timer *t) {
    if (atomic_fetch_sub(&t->refs, 1) == 1) free(t);
}

static void pw_timer_fire(void *p) {
    plat_timer *t = p;
    int s = T_ARMED;
    if (atomic_compare_exchange_strong(&t->state, &s, T_FIRED)) t->fn(t->ctx);
    pw_timer_release(t);
}

#ifdef PLAT_TEST_CLOCK
static int pw_timer_cancelled(void *t) { return atomic_load(&((plat_timer *)t)->state) == T_CANCELLED; }
#endif

static plat_queue *pw_timer_q;
static void pw_timer_q_init(void) { pw_timer_q = plat_queue_new("rbsp.plat.timers", PLAT_WORK_MAINT); }

COMMON_API plat_timer *plat_timer_new(plat_queue *q, uint64_t delay_ns, plat_fn fn, void *ctx) {
    static plat_once_t o = PLAT_ONCE_INIT;
    plat_timer *t = malloc(sizeof *t);
    if (!t) return NULL;
    atomic_init(&t->state, T_ARMED);
    atomic_init(&t->refs, 2);
    t->fn = fn;
    t->ctx = ctx;
    if (!q) {
        plat_once(&o, pw_timer_q_init);
        q = pw_timer_q;
    }
    if (!q || plat_after(q, delay_ns, pw_timer_fire, t)) { free(t); return NULL; }
    return t;
}

COMMON_API int plat_timer_cancel(plat_timer *t) {
    int s = T_ARMED, r = atomic_compare_exchange_strong(&t->state, &s, T_CANCELLED);
    pw_timer_release(t);
    return r;
}

#if defined(__BLOCKS__) && !defined(PLAT_NO_BLOCKS)
/* the block forms, for the bridge and the helper: a copied block runs once, then is released */
static void pw_block_run(void *p) {
    void (^b)(void) = (void (^)(void))p;
    b();
    Block_release(b);
}

COMMON_API int plat_async_b(plat_queue *q, void (^b)(void)) {
    void (^c)(void) = Block_copy(b);
    if (!c) return ENOMEM;                                  /* the copy is a malloc */
    int e = plat_async(q, pw_block_run, (void *)c);
    if (e) Block_release(c);
    return e;
}

COMMON_API int plat_after_b(plat_queue *q, uint64_t delay_ns, void (^b)(void)) {
    void (^c)(void) = Block_copy(b);
    if (!c) return ENOMEM;                                  /* the copy is a malloc */
    int e = plat_after(q, delay_ns, pw_block_run, (void *)c);
    if (e) Block_release(c);
    return e;
}

static void pw_block_iter(void *ctx, size_t i) {
    void (^b)(size_t) = (void (^)(size_t))ctx;
    b(i);
}
COMMON_API void plat_par_for_b(size_t n, int work, void (^b)(size_t i)) { plat_par_for(n, work, pw_block_iter, (void *)b); }
#endif

/* ---------------------------------------------------------------- sha256 (BCrypt, loaded lazily) */

typedef NTSTATUS(WINAPI *pw_create_hash)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *pw_hash_data)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *pw_finish_hash)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *pw_destroy_hash)(BCRYPT_HASH_HANDLE);
static struct { pw_create_hash create; pw_hash_data data; pw_finish_hash finish; pw_destroy_hash destroy; } pw_bc;

/* a function pointer from GetProcAddress (FARPROC), without a cast between function types */
#define PW_PROC(dst, mod, name)                    \
    do {                                           \
        FARPROC f_ = GetProcAddress((mod), (name)); \
        memcpy(&(dst), &f_, sizeof f_);            \
    } while (0)

static void pw_bcrypt_load(void) {
    /* bcrypt.dll isn't a KnownDLL: only its full System32 path, never the search order (I31) */
    wchar_t path[MAX_PATH + 16];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    memcpy(path + n, L"\\bcrypt.dll", sizeof L"\\bcrypt.dll");
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return;                                         /* never freed: it serves the process's life */
    pw_create_hash c;
    pw_hash_data d;
    pw_finish_hash f;
    pw_destroy_hash x;
    PW_PROC(c, m, "BCryptCreateHash");
    PW_PROC(d, m, "BCryptHashData");
    PW_PROC(f, m, "BCryptFinishHash");
    PW_PROC(x, m, "BCryptDestroyHash");
    if (c && d && f && x) {
        pw_bc.data = d;
        pw_bc.finish = f;
        pw_bc.destroy = x;
        pw_bc.create = c;
    }
}

COMMON_API int plat_sha256_init(plat_sha256_ctx *c) {
    static plat_once_t o = PLAT_ONCE_INIT;
    plat_once(&o, pw_bcrypt_load);
    c->hash = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    if (!pw_bc.create || pw_bc.create(BCRYPT_SHA256_ALG_HANDLE, &h, NULL, 0, NULL, 0, 0) != 0 || !h) return -1;
    c->hash = h;
    return 0;
}

COMMON_API void plat_sha256_update(plat_sha256_ctx *c, const void *p, size_t n) {
    const unsigned char *b = p;
    while (c->hash && n > 0) {
        ULONG k = n > 0x40000000u ? 0x40000000u : (ULONG)n;   /* ULONG lengths: 1 GB pieces */
        if (pw_bc.data(c->hash, (PUCHAR)b, k, 0) != 0) {
            pw_bc.destroy(c->hash);                         /* failed: final says so */
            c->hash = NULL;
        }
        b += k;
        n -= k;
    }
}

COMMON_API int plat_sha256_final(plat_sha256_ctx *c, unsigned char out[32]) {
    if (!c->hash) return -1;
    NTSTATUS s = pw_bc.finish(c->hash, out, 32, 0);
    pw_bc.destroy(c->hash);
    c->hash = NULL;
    return s == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- inverse real FFT (pocketfft) */

#include "irfft_pocketfft.c"

/* ---------------------------------------------------------------- files: helpers */

#define PW_NAME_CAP 1024                /* UTF-16 units of a name (NTFS: 255 a component) */

/* the low I/O priority hint on a handle opened under the maintenance work class (r4:I51) */
static void pw_hint(HANDLE h) {
    if (pw_work != PLAT_WORK_MAINT) return;
    /* 8-byte aligned: Windows refuses this class's buffer at a 4 (mod 8) address with
     * STATUS_DATATYPE_MISALIGNMENT (measured in the VM, ARM64 and x64 builds) */
    _Alignas(8) ULONG hint = 1;                             /* IoPriorityLow */
    IO_STATUS_BLOCK io;
    NtSetInformationFile(h, &io, &hint, sizeof hint, PW_CLASS(PW_FileIoPriorityHintInformation));
}

/* Opens `name` relative to root, or the absolute path `name` when root is NULL (plat_dir_cwd),
 * sharing what `share` says (every other open shares READ|WRITE|DELETE: pw_open). sync: the
 * handle is for I/O (SYNCHRONIZE and synchronous I/O); 0 for a handle that is only inspected
 * (its descriptor, attributes) or held, which then asks for exactly `access`: an object whose
 * DACL grants nothing (an empty one) still opens for its owner's READ_CONTROL (measured in the
 * VM: SYNCHRONIZE alone refused it). 0 or an errno; *out is NULL on failure. */
/* NtCreateFile on the name w (n units, or n < 0: refused, EINVAL), relative to root or an NT path */
static int pw_create(HANDLE root, const uint16_t *w, long n, ACCESS_MASK access, ULONG share, ULONG disp, ULONG opts, int op, int sync,
                     HANDLE *out) {
    *out = NULL;
    if (n < 0) return EINVAL;
    if (n > 32767) return ENAMETOOLONG;
    UNICODE_STRING us = {(USHORT)(n * 2), (USHORT)(n * 2), (PWSTR)w};
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, root, NULL);
    IO_STATUS_BLOCK io;
    HANDLE h = NULL;
    NTSTATUS s = NtCreateFile(&h, access | (sync ? SYNCHRONIZE : 0), &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, share, disp,
                              opts | (sync ? FILE_SYNCHRONOUS_IO_NONALERT : 0), NULL, 0);
    int e = pw_nt(s, op);
    if (e == 0) {
        *out = h;
        pw_hint(h);
    }
    return e;
}

static int pw_open_as(HANDLE root, const char *name, ACCESS_MASK access, ULONG share, ULONG disp, ULONG opts, int op, int sync,
                      HANDLE *out) {
    uint16_t small[PW_NAME_CAP], *w = small;
    long n;
    *out = NULL;
    if (root) n = wr_name16(name, w, PW_NAME_CAP);
    else {
        if (!(w = malloc(sizeof(uint16_t) * (WR_NT_MAX + 1)))) return ENOMEM;
        n = wr_nt_path(name, w, WR_NT_MAX + 1);
    }
    int e = pw_create(root, w, n, access, share, disp, opts, op, sync, out);
    if (w != small) free(w);
    return e;
}

/* The NT path of a path a follow-link open is given (B14, BW11: plat_stat_path with follow and
 * plat_file_open_path, the model path rekordbox names and opens with Win32's rules): a "\\?\" or
 * "\??\" path as it is when its root is X:\, UNC\server\share or Volume{GUID}\ (BX7); any other
 * through GetFullPathNameW (relative to the process's current folder, "." and ".." folded, '/' as
 * '\', as Win32 reads it), then "\??\X:\..." or "\??\UNC\server\share\..." (win_rules.h's
 * wr_full_nt). The no-follow opens keep wr_nt_path's strict rule. out: WR_NT_MAX + 1 units. 0
 * or an errno (EINVAL: a path it refuses), *units set. */
static int wr_full_path(const char *p, uint16_t *out, long *units) {
    size_t cap = WR_NT_MAX + 1;
    uint16_t *w = malloc(sizeof(uint16_t) * cap), *full = NULL;
    long n;
    int e = 0;
    *units = -1;
    if (!w) return ENOMEM;
    if ((n = wr_utf8_to_16(p, strlen(p), w, cap)) <= 0) e = EINVAL;   /* empty, invalid or too long */
    else if (wr_prefixed16(w, (size_t)n)) n = wr_full_nt(w, (size_t)n, out, cap);
    else if (!(full = malloc(sizeof(uint16_t) * cap))) e = ENOMEM;
    else {
        /* the length without the NUL; one over cap when it doesn't fit; 0 on a failure */
        DWORD r = GetFullPathNameW((LPCWSTR)w, (DWORD)cap, (LPWSTR)full, NULL);
        n = r == 0 || r >= cap ? -1 : wr_full_nt(full, r, out, cap);
    }
    if (!e && n < 0) e = EINVAL;
    free(full);
    free(w);
    if (!e) *units = n;
    return e;
}

/* a follow-link open of a path (wr_full_path's), sharing READ|WRITE|DELETE, for I/O */
static int pw_open_full(const char *path, ACCESS_MASK access, ULONG opts, HANDLE *out) {
    uint16_t *w = malloc(sizeof(uint16_t) * (WR_NT_MAX + 1));
    long n = -1;
    *out = NULL;
    if (!w) return ENOMEM;
    int e = wr_full_path(path, w, &n);
    if (!e) e = pw_create(NULL, w, n, access, PW_SHARE_ALL, FILE_OPEN, opts, WR_OP_OPEN, 1, out);
    free(w);
    return e;
}

static int pw_open_share(HANDLE root, const char *name, ACCESS_MASK access, ULONG share, ULONG disp, ULONG opts, int op, HANDLE *out) {
    return pw_open_as(root, name, access, share, disp, opts, op, 1, out);
}

static int pw_open(HANDLE root, const char *name, ACCESS_MASK access, ULONG disp, ULONG opts, int op, HANDLE *out) {
    return pw_open_share(root, name, access, PW_SHARE_ALL, disp, opts, op, out);
}

static int pw_attrs(HANDLE h, ULONG *attrs) {
    pw_tag_info t;
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtQueryInformationFile(h, &io, &t, sizeof t, PW_CLASS(PW_FileAttributeTagInformation));
    if (!s) {
        *attrs = t.FileAttributes;
        return 0;
    }
    if ((uint32_t)s != PW_STATUS_INVALID_PARAMETER) return pw_nt(s, WR_OP_OPEN);
    /* exFAT and FAT32 don't answer that class (STATUS_INVALID_PARAMETER, measured in the VM) and
     * have no reparse points: the basic information's attributes say the same */
    pw_basic_info b;
    s = NtQueryInformationFile(h, &io, &b, sizeof b, PW_CLASS(PW_FileBasicInformation));
    if (s) return pw_nt(s, WR_OP_OPEN);
    *attrs = b.FileAttributes;
    return 0;
}

/* an opened last component that is a reparse point (a link, a junction, a mount point) is
 * refused: the Mac's O_NOFOLLOW gives ELOOP */
static int pw_no_reparse(HANDLE h) {
    ULONG a = 0;
    int e = pw_attrs(h, &a);
    if (e) return e;
    return a & FILE_ATTRIBUTE_REPARSE_POINT ? ELOOP : 0;
}

static int pw_open_nofollow(HANDLE root, const char *name, ACCESS_MASK access, ULONG disp, ULONG opts, int op, HANDLE *out) {
    int e = pw_open(root, name, access, disp, opts | FILE_OPEN_REPARSE_POINT, op, out);
    if (!e && (e = pw_no_reparse(*out))) {
        NtClose(*out);
        *out = NULL;
    }
    return e;
}

static int pw_id(HANDLE h, plat_id *id) {
    IO_STATUS_BLOCK io;
    pw_id_info fi;
    NTSTATUS q = NtQueryInformationFile(h, &io, &fi, sizeof fi, PW_CLASS(PW_FileIdInformation));
    if (q == 0) {
        id->vol = fi.VolumeSerialNumber;
        memcpy(&id->file[0], fi.FileId, 8);                 /* the 64-bit file index on NTFS */
        memcpy(&id->file[1], fi.FileId + 8, 8);
        return 0;
    }
    /* one name, one id source: only a system without the class falls back (any other error is
     * one, never the other form of the id, which plat_id_same would call another file) */
    if (!pw_unsupported(q)) return pw_nt(q, WR_OP_OPEN);
    /* FAT and older systems: the 32-bit volume serial and the 64-bit index */
    LARGE_INTEGER ix;
    struct { pw_fs_volume_info v; WCHAR more[64]; } vi;
    NTSTATUS s = NtQueryInformationFile(h, &io, &ix, sizeof ix, PW_CLASS(PW_FileInternalInformation));
    if (s) return pw_nt(s, WR_OP_OPEN);
    s = NtQueryVolumeInformationFile(h, &io, &vi, sizeof vi, (FS_INFORMATION_CLASS)PW_FileFsVolumeInformation);
    if (s && (uint32_t)s != PW_STATUS_BUFFER_OVERFLOW) return pw_nt(s, WR_OP_OPEN);
    id->vol = vi.v.VolumeSerialNumber;
    id->file[0] = (uint64_t)ix.QuadPart;
    id->file[1] = 0;
    return 0;
}

static int pw_stat_h(HANDLE h, plat_stat *st) {
    IO_STATUS_BLOCK io;
    pw_basic_info b;
    pw_standard_info s;
    NTSTATUS r = NtQueryInformationFile(h, &io, &b, sizeof b, PW_CLASS(PW_FileBasicInformation));
    if (!r) r = NtQueryInformationFile(h, &io, &s, sizeof s, PW_CLASS(PW_FileStandardInformation));
    if (r) return pw_nt(r, WR_OP_OPEN);
    st->kind = b.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? PLAT_OTHER : s.Directory ? PLAT_DIR : PLAT_FILE;
    if (st->kind == PLAT_FILE && GetFileType(h) != FILE_TYPE_DISK) st->kind = PLAT_OTHER;
    st->size = s.EndOfFile.QuadPart;
    st->alloc = s.AllocationSize.QuadPart;                 /* the cache's size rule */
    st->mtime = wr_filetime_s(b.LastWriteTime.QuadPart);
    return pw_id(h, &st->id);
}

/* runs an operation, retrying an EBUSY on a queue's thread (8 × 125 ms), never elsewhere */
#define PW_RETRY(e, call)                                                     \
    do {                                                                      \
        for (int try_ = 0;; try_++) {                                         \
            (e) = (call);                                                     \
            if ((e) != EBUSY || !pw_on_queue || try_ == 8) break;             \
            Sleep(125);                                                       \
        }                                                                     \
    } while (0)

static int pw_set_mtime_now(HANDLE h) {
    pw_basic_info b;
    memset(&b, 0, sizeof b);                                /* 0: that time is left alone */
    b.LastWriteTime.QuadPart = pw_filetime_now();
    IO_STATUS_BLOCK io;
    return pw_nt(NtSetInformationFile(h, &io, &b, sizeof b, PW_CLASS(PW_FileBasicInformation)), WR_OP_OPEN);
}

/* deletes the open handle's file: POSIX semantics where there are, plain dispose on FAT/exFAT */
static int pw_dispose(HANDLE h) {
    IO_STATUS_BLOCK io;
    ULONG fl = PW_DISPOSE_DELETE | PW_DISPOSE_POSIX | PW_DISPOSE_IGNORE_READONLY;
    NTSTATUS s = NtSetInformationFile(h, &io, &fl, sizeof fl, PW_CLASS(PW_FileDispositionInformationEx));
    if (s && pw_unsupported(s)) {
        BOOLEAN del = TRUE;
        s = NtSetInformationFile(h, &io, &del, sizeof del, PW_CLASS(PW_FileDispositionInformation));
    }
    return pw_nt(s, WR_OP_RENAME_DELETE);
}

static int pw_unlink_once(HANDLE root, const char *name) {
    HANDLE h;
    int e = pw_open(root, name, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT,
                    WR_OP_RENAME_DELETE, &h);
    if (e) return e;
    e = pw_dispose(h);
    NtClose(h);
    return e;
}

/* renames the open handle h to `to` in the folder `dir` (or the absolute path `to` when dir is
 * NULL), replacing a file there */
/* replace: over an existing name (ReplaceIfExists TRUE), else EEXIST when `to` is taken
 * (STATUS_OBJECT_NAME_COLLISION) */
static int pw_rename_h(HANDLE h, HANDLE dir, const char *to, int replace) {
    size_t cap = dir ? PW_NAME_CAP : WR_NT_MAX + 1;
    pw_rename_info *ri = malloc(sizeof *ri + sizeof(WCHAR) * cap);
    if (!ri) return ENOMEM;
    long n = dir ? wr_name16(to, (uint16_t *)ri->FileName, cap) : wr_nt_path(to, (uint16_t *)ri->FileName, cap);
    if (n < 0) { free(ri); return EINVAL; }
    ri->Flags = (replace ? PW_RENAME_REPLACE : 0) | PW_RENAME_POSIX | PW_RENAME_IGNORE_READONLY;
    ri->RootDirectory = dir;
    ri->FileNameLength = (ULONG)n * 2;
    /* never under the struct's size: a 1-character name is 22 bytes of a 24-byte struct (VM round 8) */
    ULONG len = (ULONG)wr_info_len(offsetof(pw_rename_info, FileName), sizeof(pw_rename_info), (size_t)n);
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtSetInformationFile(h, &io, ri, len, PW_CLASS(PW_FileRenameInformationEx));
    if (s && pw_unsupported(s)) {
        ri->Flags = replace ? PW_RENAME_REPLACE : 0;          /* ReplaceIfExists = replace */
        s = NtSetInformationFile(h, &io, ri, len, PW_CLASS(PW_FileRenameInformation));
    }
    free(ri);
    return pw_nt(s, WR_OP_RENAME_DELETE);
}

static int pw_rename_once(HANDLE from, const char *a, HANDLE to, const char *b, int replace) {
    HANDLE h;
    int e = pw_open(from, a, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_OPEN_REPARSE_POINT, WR_OP_RENAME_DELETE, &h);
    if (e) return e;
    e = pw_rename_h(h, to, b, replace);
    NtClose(h);
    return e;
}

/* UTF-8 of a UTF-16 buffer (n units) into out; -1 when it isn't valid */
static long pw_to8(const WCHAR *w, size_t n, char *out, size_t cap) { return wr_utf16_to_8((const uint16_t *)w, n, out, cap); }

/* a Win32 (not NT) wide path of a UTF-8 one, for the volume, module and shell calls; malloc'd */
static wchar_t *pw_win32_path(const char *p) {
    size_t l = strlen(p);
    wchar_t *w = malloc(sizeof(wchar_t) * (l + 1));
    if (w && wr_utf8_to_16(p, l, (uint16_t *)w, l + 1) < 0) { free(w); w = NULL; }
    return w;
}

/* ---------------------------------------------------------------- files and folders */

/* a folder for reading: no FILE_TRAVERSE, which folders granting (R,W,D) without execute refuse
 * (WER's ReportArchive for Authenticated Users); opens below it don't need it on the handle
 * (Bypass traverse checking, every account's) */
#define PW_DIR_ACCESS (FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES)

COMMON_API plat_dir plat_dir_cwd(void) { return (plat_dir){NULL}; }
COMMON_API int plat_dir_ok(plat_dir d) { return d.h != NULL; }
COMMON_API int plat_file_ok(plat_file f) { return f.h != NULL; }
COMMON_API void plat_dir_close(plat_dir d) { if (d.h) NtClose(d.h); }
COMMON_API void plat_file_close(plat_file f) { if (f.h) NtClose(f.h); }

COMMON_API int plat_id_same(plat_id a, plat_id b) { return a.vol == b.vol && a.file[0] == b.file[0] && a.file[1] == b.file[1]; }
COMMON_API uint64_t plat_id_number(plat_id a) { return a.file[0]; }

COMMON_API int plat_dir_open(const char *path, plat_dir *out) {
    HANDLE h;
    int e = pw_open_nofollow(NULL, path, PW_DIR_ACCESS, FILE_OPEN, FILE_DIRECTORY_FILE, WR_OP_OPEN, &h);
    out->h = h;
    return e;
}

COMMON_API int plat_dir_open_at(plat_dir d, const char *name, plat_dir *out) {
    HANDLE h;
    int e = pw_open_nofollow(d.h, name, PW_DIR_ACCESS, FILE_OPEN, FILE_DIRECTORY_FILE, WR_OP_OPEN, &h);
    out->h = h;
    return e;
}

COMMON_API int plat_dir_open_follow(const char *path, plat_dir *out) {
    /* without FILE_OPEN_REPARSE_POINT: a junction or a folder symlink at the last component is
     * resolved, as the ancestors always are */
    HANDLE h = NULL;
    int e = pw_open(NULL, path, PW_DIR_ACCESS, FILE_OPEN, FILE_DIRECTORY_FILE, WR_OP_OPEN, &h);
    out->h = e ? NULL : h;
    return e;
}

COMMON_API int plat_mkdir_at(plat_dir d, const char *name) {
    HANDLE h;
    int e = pw_open(d.h, name, FILE_LIST_DIRECTORY, FILE_CREATE, FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, WR_OP_OPEN, &h);
    if (!e) NtClose(h);
    return e;
}

COMMON_API void plat_mkdirs(const char *path) {
    /* every folder from the root down, as `mkdir -p`; errors ignored */
    const char *rest;
    if (!wr_path_kind(path, &rest)) return;
    size_t l = strlen(path);
    char *p = malloc(l + 1);
    if (!p) return;
    memcpy(p, path, l + 1);
    for (size_t i = (size_t)(rest - path); i <= l; i++) {
        if (p[i] != '\\' && p[i] != '/' && p[i] != 0) continue;
        char c = p[i];
        p[i] = 0;
        HANDLE h;
        if (i > (size_t)(rest - path) && !pw_open(NULL, p, FILE_LIST_DIRECTORY, FILE_OPEN_IF, FILE_DIRECTORY_FILE, WR_OP_OPEN, &h))
            NtClose(h);
        p[i] = c;
    }
    free(p);
}

COMMON_API int plat_dir_name_is(plat_dir d, const char *name) {
    /* the folder's own name as stored (FileNameInformation: its path on the volume), compared
     * exactly: a volume that ignores case opens "AB" for "ab" */
    struct { ULONG len; WCHAR name[2048]; } ni;
    IO_STATUS_BLOCK io;
    if (NtQueryInformationFile(d.h, &io, &ni, sizeof ni, PW_CLASS(PW_FileNameInformation))) return 0;
    size_t n = ni.len / 2, i = n;
    while (i > 0 && ni.name[i - 1] != L'\\') i--;
    char got[1024];
    if (pw_to8(ni.name + i, n - i, got, sizeof got) < 0) return 0;
    return !strcmp(got, name);
}

COMMON_API int plat_dir_stat(plat_dir d, plat_stat *st) { return pw_stat_h(d.h, st); }

/* the security descriptor of a handle opened with READ_CONTROL, malloc'd (r4:I48: read from the
 * handle, never GetSecurityInfo, so no on-demand DLL load) */
static int pw_security(HANDLE h, SECURITY_INFORMATION what, PSECURITY_DESCRIPTOR *out) {
    ULONG len = 512, need = 0;
    *out = NULL;
    for (int i = 0; i < 4; i++) {
        PSECURITY_DESCRIPTOR sd = malloc(len);
        if (!sd) return ENOMEM;
        NTSTATUS s = NtQuerySecurityObject(h, what, sd, len, &need);
        if (s == 0) { *out = sd; return 0; }
        free(sd);
        if ((uint32_t)s != PW_STATUS_BUFFER_TOO_SMALL && (uint32_t)s != PW_STATUS_BUFFER_OVERFLOW) return pw_nt(s, WR_OP_OPEN);
        len = need > len ? need : len * 2;
    }
    return EIO;
}

static int pw_fs_attrs(HANDLE h, ULONG *attrs) {
    struct { pw_fs_attr_info a; WCHAR more[32]; } ai;
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtQueryVolumeInformationFile(h, &io, &ai, sizeof ai, (FS_INFORMATION_CLASS)PW_FileFsAttributeInformation);
    if (s && (uint32_t)s != PW_STATUS_BUFFER_OVERFLOW) return pw_nt(s, WR_OP_OPEN);
    *attrs = ai.a.FileSystemAttributes;
    return 0;
}

COMMON_API int plat_dir_writable(plat_dir d) {
    /* AccessCheck of a copy of this process's token against the folder's own descriptor, for
     * FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY (I89); nothing is created. FAT/exFAT have no ACLs. */
    ULONG fa = 0;
    int e = pw_fs_attrs(d.h, &fa);
    if (e) return e;
    if (fa & PW_FILE_READ_ONLY_VOLUME) return EROFS;
    if (!(fa & PW_FILE_PERSISTENT_ACLS)) return 0;
    HANDLE h;                                               /* the same folder again, with READ_CONTROL (J8) */
    UNICODE_STRING empty = {0, 0, NULL};
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &empty, 0, d.h, NULL);
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtCreateFile(&h, READ_CONTROL | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &io, NULL, 0, PW_SHARE_ALL, FILE_OPEN,
                              FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (s) return pw_nt(s, WR_OP_OPEN);
    PSECURITY_DESCRIPTOR sd;
    e = pw_security(h, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &sd);
    NtClose(h);
    if (e) return e;
    HANDLE tok = NULL, imp = NULL;
    e = EACCES;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &tok)
        && DuplicateToken(tok, SecurityImpersonation, &imp)) {
        GENERIC_MAPPING map = {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        PRIVILEGE_SET ps;
        DWORD pslen = sizeof ps, granted = 0;
        BOOL ok = FALSE;
        if (AccessCheck(sd, imp, FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY, &map, &ps, &pslen, &granted, &ok)) e = ok ? 0 : EACCES;
        else e = pw_last(WR_OP_OPEN);
    } else e = pw_last(WR_OP_OPEN);
    if (imp) CloseHandle(imp);
    if (tok) CloseHandle(tok);
    free(sd);
    return e;
}

COMMON_API int plat_dir_each(plat_dir d, plat_each_fn fn, void *ctx) {
    /* one pass of FileIdExtdDirectoryInformation (FileIdBothDirectoryInformation where the folder
     * has no 128-bit ids: FAT, exFAT): name, attributes, AllocationSize, LastWriteTime and FileId
     * together, so no name is opened (bridge.md §1). The ids are pw_id's: the 128-bit one where
     * FileIdInformation answers (ReFS's doesn't fit 64 bits), else the 64-bit index (B16) */
    enum { BUF = 64 * 1024 };
    unsigned char *buf = malloc(BUF);
    char *name = malloc(1024);
    if (!buf || !name) { free(buf); free(name); return -1; }
    plat_id dir;
    int le = pw_id(d.h, &dir), r = le ? -1 : 0, bad = 0;  /* le: the errno a -1 leaves, as plat_mac.c's */
    IO_STATUS_BLOCK io;
    pw_id_info idi;
    int extd = 0;
    if (r == 0) {   /* pw_id's rule: the 64-bit ids only where the class is missing, never on another error */
        NTSTATUS q = NtQueryInformationFile(d.h, &io, &idi, sizeof idi, PW_CLASS(PW_FileIdInformation));
        if (q == 0) extd = 1;
        else if (!pw_unsupported(q)) { r = -1; le = pw_nt(q, WR_OP_OPEN); }   /* the probe's own error, not 0 (review HF4) */
    }
    for (BOOLEAN restart = TRUE; r == 0; restart = FALSE) {
        NTSTATUS s = NtQueryDirectoryFile(d.h, NULL, NULL, NULL, &io, buf, BUF,
                                          PW_CLASS(extd ? PW_FileIdExtdDirectoryInformation : PW_FileIdBothDirectoryInformation), FALSE,
                                          NULL, restart);
        if (s && restart && extd && pw_unsupported(s)) {   /* FileIdInformation but no FILE_ID_EXTD listing (a third-party driver) */
            extd = 0;
            s = NtQueryDirectoryFile(d.h, NULL, NULL, NULL, &io, buf, BUF, PW_CLASS(PW_FileIdBothDirectoryInformation), FALSE, NULL, TRUE);
        }
        if ((uint32_t)s == PW_STATUS_NO_MORE_FILES || (restart && (uint32_t)s == WR_STATUS_NO_SUCH_FILE)) break;
        if (s) { r = -1; le = pw_nt(s, WR_OP_OPEN); break; }
        for (size_t off = 0;;) {
            const pw_dir_info *e = (const pw_dir_info *)(buf + off);
            const pw_dir_info_extd *x = (const pw_dir_info_extd *)(buf + off);
            ULONG next = extd ? x->NextEntryOffset : e->NextEntryOffset, attrs = extd ? x->FileAttributes : e->FileAttributes;
            const WCHAR *fname = extd ? x->FileName : e->FileName;
            size_t n = (extd ? x->FileNameLength : e->FileNameLength) / 2;
            int dot = (n == 1 && fname[0] == L'.') || (n == 2 && fname[0] == L'.' && fname[1] == L'.');
            if (!dot && pw_to8(fname, n, name, 1024) < 0) bad = 1;   /* a name that can't be looked at (EINVAL): */
            else if (!dot) {                                /* skipped, the walk goes on (as the Mac's), -1 at its end */
                plat_stat st;
                st.kind = attrs & FILE_ATTRIBUTE_REPARSE_POINT ? PLAT_OTHER : attrs & FILE_ATTRIBUTE_DIRECTORY ? PLAT_DIR : PLAT_FILE;
                st.size = (extd ? x->EndOfFile : e->EndOfFile).QuadPart;
                st.alloc = (extd ? x->AllocationSize : e->AllocationSize).QuadPart;
                st.mtime = wr_filetime_s((extd ? x->LastWriteTime : e->LastWriteTime).QuadPart);
                st.id.vol = dir.vol;
                if (extd) {
                    memcpy(&st.id.file[0], x->FileId, 8);   /* as pw_id reads FileIdInformation's */
                    memcpy(&st.id.file[1], x->FileId + 8, 8);
                } else {
                    st.id.file[0] = (uint64_t)e->FileId.QuadPart;
                    st.id.file[1] = 0;
                }
                if (fn(ctx, name, &st)) { r = -1; le = errno; break; }   /* fn's own errno */
            }
            if (!next) break;
            off += next;
        }
    }
    free(buf);
    free(name);
    if (!r && bad) { r = -1; le = EINVAL; }                 /* as plat_mkdir_at refuses such a name */
    if (r) errno = le ? le : EIO;   /* an NTSTATUS that maps to 0 still reads as an error (review HF4) */
    return r;
}

#if defined(__BLOCKS__) && !defined(PLAT_NO_BLOCKS)
static int pw_each_block(void *ctx, const char *name, const plat_stat *st) {
    int (^f)(const char *, const plat_stat *) = (int (^)(const char *, const plat_stat *))ctx;
    return f(name, st);
}
COMMON_API int plat_dir_each_b(plat_dir d, int (^f)(const char *name, const plat_stat *st)) {
    return plat_dir_each(d, pw_each_block, (void *)f);
}
#endif

COMMON_API int plat_stat_at(plat_dir d, const char *name, plat_stat *st) {
    HANDLE h;
    int e = pw_open(d.h, name, FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_OPEN_REPARSE_POINT, WR_OP_OPEN, &h);
    if (e) return e;
    e = pw_stat_h(h, st);
    NtClose(h);
    return e;
}

COMMON_API int plat_unlink_at(plat_dir d, const char *name) {
    int e;
    PW_RETRY(e, pw_unlink_once(d.h, name));
    return e;
}

/* the folder name in root, only when it is the one checked (expect) and empty, through one handle */
static int pw_remove_empty_dir_once(HANDLE root, const char *name, plat_id expect) {
    HANDLE h;
    int e = pw_open_nofollow(root, name, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_DIRECTORY_FILE, WR_OP_RENAME_DELETE, &h);
    if (e) return e;
    plat_stat st;
    e = pw_stat_h(h, &st);
    if (!e && !plat_id_same(st.id, expect)) e = ESTALE;
    if (!e) e = pw_dispose(h);   /* a folder that isn't empty: ENOTEMPTY */
    NtClose(h);
    return e;
}

COMMON_API int plat_remove_empty_dir_at(plat_dir d, const char *name, plat_id expect) {
    int e;
    PW_RETRY(e, pw_remove_empty_dir_once(d.h, name, expect));
    return e;
}

COMMON_API int plat_touch_at(plat_dir d, const char *name) {
    HANDLE h;
    int e = pw_open(d.h, name, FILE_WRITE_ATTRIBUTES, FILE_OPEN, FILE_OPEN_REPARSE_POINT, WR_OP_OPEN, &h);
    if (e) return e;
    e = pw_set_mtime_now(h);
    NtClose(h);
    return e;
}

COMMON_API int plat_rename_at(plat_dir from, const char *a, plat_dir to, const char *b) {
    int e;
    PW_RETRY(e, pw_rename_once(from.h, a, to.h, b, 1));
    return e;
}

COMMON_API int plat_rename_noreplace_at(plat_dir from, const char *a, plat_dir to, const char *b) {
    int e;
    PW_RETRY(e, pw_rename_once(from.h, a, to.h, b, 0));
    return e;
}

/* ---------------------------------------------------------------- a folder tree's deletion (T3b)
 * wr_remove_tree's operations (win_rules.h): every open relative to the parent's handle, with
 * FILE_OPEN_REPARSE_POINT (a reparse point is opened, and deleted, as itself), the listing and the
 * deletion by handle. */

static int pw_tree_open(void *ctx, void *parent, const uint16_t *name, size_t n, void **h, int *is_dir) {
    (void)ctx;
    *h = NULL;
    *is_dir = 0;
    if (!n || n > 32767) return EINVAL;
    UNICODE_STRING us = {(USHORT)(n * 2), (USHORT)(n * 2), (PWSTR)(uintptr_t)name};
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, (HANDLE)parent, NULL);
    IO_STATUS_BLOCK io;
    HANDLE x = NULL;
    /* no data access: a file held without read sharing still goes, and no scan-on-open is invited */
    NTSTATUS s = NtCreateFile(&x, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &io, NULL, 0, PW_SHARE_ALL, FILE_OPEN,
                              FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    int e = pw_nt(s, WR_OP_RENAME_DELETE);
    if (e) return e;
    ULONG a = 0;
    if ((e = pw_attrs(x, &a))) {
        NtClose(x);
        return e;
    }
    *is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
    if (*is_dir) {
        /* a real folder, to be listed: the same object reopened through no path (a swap can't
         * redirect it), with FILE_LIST_DIRECTORY too, as plat_dir_writable does */
        UNICODE_STRING empty = {0, 0, NULL};
        InitializeObjectAttributes(&oa, &empty, 0, x, NULL);
        HANDLE y = NULL;
        s = NtCreateFile(&y, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &io, NULL, 0, PW_SHARE_ALL, FILE_OPEN,
                         FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        NtClose(x);
        if ((e = pw_nt(s, WR_OP_RENAME_DELETE))) {
            *is_dir = 0;
            return e;
        }
        x = y;
    }
    *h = x;
    return 0;
}

static int pw_tree_list(void *ctx, void *h, int (*add)(void *lctx, const uint16_t *name, size_t n), void *lctx) {
    (void)ctx;
    enum { BUF = 64 * 1024 };
    unsigned char *buf = malloc(BUF);
    if (!buf) return ENOMEM;
    int r = 0;
    for (BOOLEAN restart = TRUE; !r; restart = FALSE) {
        IO_STATUS_BLOCK io;
        NTSTATUS s = NtQueryDirectoryFile((HANDLE)h, NULL, NULL, NULL, &io, buf, BUF, PW_CLASS(PW_FileIdBothDirectoryInformation), FALSE,
                                          NULL, restart);
        if ((uint32_t)s == PW_STATUS_NO_MORE_FILES || (restart && (uint32_t)s == WR_STATUS_NO_SUCH_FILE)) break;
        if (s) { r = pw_nt(s, WR_OP_OPEN); break; }
        for (size_t off = 0;;) {
            const pw_dir_info *e = (const pw_dir_info *)(buf + off);
            size_t n = e->FileNameLength / 2;
            int dot = (n == 1 && e->FileName[0] == L'.') || (n == 2 && e->FileName[0] == L'.' && e->FileName[1] == L'.');
            if (!dot && (r = add(lctx, (const uint16_t *)e->FileName, n))) break;
            if (!e->NextEntryOffset) break;
            off += e->NextEntryOffset;
        }
    }
    free(buf);
    return r;
}

static int pw_tree_dispose(void *ctx, void *h) {
    (void)ctx;
    return pw_dispose((HANDLE)h);
}

static void pw_tree_close(void *ctx, void *h) {
    (void)ctx;
    NtClose((HANDLE)h);
}

static const wr_tree_ops pw_tree_ops = {pw_tree_open, pw_tree_list, pw_tree_dispose, pw_tree_close, NULL};

COMMON_API int plat_remove_tree_at(plat_dir d, const char *name) {
    uint16_t w[PW_NAME_CAP];
    long n = wr_name16(name, w, PW_NAME_CAP);
    if (n <= 0) return EINVAL;
    return wr_remove_tree(&pw_tree_ops, d.h, w, (size_t)n, 0);
}

COMMON_API int plat_remove_tree(const char *path) {
    size_t cut, l, plen;
    if (wr_split_leaf(path, &cut, &l, &plen)) return EINVAL;   /* the lengths: wr_nt_path and wr_name16 cap them */
    char *parent = malloc(plen + 1), *leaf = malloc(l - cut + 1);
    int e = ENOMEM;
    if (parent && leaf) {
        memcpy(parent, path, plen);
        parent[plen] = 0;
        memcpy(leaf, path + cut, l - cut);
        leaf[l - cut] = 0;
        plat_dir d;
        if (!(e = plat_dir_open(parent, &d))) {
            e = plat_remove_tree_at(d, leaf);
            plat_dir_close(d);
        }
    }
    free(parent);
    free(leaf);
    return e;
}

static int pw_size(HANDLE h, int64_t *size) {
    pw_standard_info s;
    IO_STATUS_BLOCK io;
    NTSTATUS r = NtQueryInformationFile(h, &io, &s, sizeof s, PW_CLASS(PW_FileStandardInformation));
    if (r) return pw_nt(r, WR_OP_OPEN);
    *size = s.EndOfFile.QuadPart;
    return 0;
}

COMMON_API int plat_file_open_at(plat_dir d, const char *name, int64_t max, int64_t *size, plat_file *out) {
    /* the bridge's checked_open: a regular file of at most max bytes, no link followed; opened
     * with FILE_WRITE_ATTRIBUTES too (a hit's stamp, plat_file_touch), read-only where the volume
     * or the ACL refuses that (bridge.md §1) */
    HANDLE h;
    out->h = NULL;
    int e = pw_open_nofollow(d.h, name, FILE_GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_OPEN, FILE_NON_DIRECTORY_FILE, WR_OP_OPEN, &h);
    if (e == EACCES || e == EROFS)
        e = pw_open_nofollow(d.h, name, FILE_GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, WR_OP_OPEN, &h);
    if (e == EISDIR) return PLAT_EFTYPE;
    if (e) return e;
    int64_t sz = 0;
    if ((e = pw_size(h, &sz)) == 0 && (GetFileType(h) != FILE_TYPE_DISK || sz > max)) e = PLAT_EFTYPE;
    if (e) { NtClose(h); return e; }
    if (size) *size = sz;
    out->h = h;
    return 0;
}

COMMON_API int plat_file_open_path(const char *path, plat_file *out) {
    /* a model file rekordbox names: links followed, only a regular file (else EINVAL); relative,
     * dotted and UNC paths as Win32 reads them (wr_full_path) */
    HANDLE h;
    out->h = NULL;
    int e = pw_open_full(path, FILE_GENERIC_READ, FILE_NON_DIRECTORY_FILE, &h);
    if (e == EISDIR) return EINVAL;
    if (e) return e;
    if (GetFileType(h) != FILE_TYPE_DISK) { NtClose(h); return EINVAL; }
    out->h = h;
    return 0;
}

COMMON_API int plat_file_create_at(plat_dir d, const char *name, int rw, plat_file *out) {
    HANDLE h;
    int e = pw_open(d.h, name, FILE_GENERIC_WRITE | (rw ? FILE_GENERIC_READ : 0), FILE_CREATE,
                    FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, WR_OP_OPEN, &h);
    out->h = e ? NULL : h;
    return e;
}

/* a stdio stream on handle h, in binary mode; on success the stream owns h. NULL with errno set
 * and h still the caller's otherwise. */
static FILE *pw_stdio(HANDLE h, const char *mode, int flags) {
    HANDLE dup;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        errno = pw_last(WR_OP_OPEN);
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)dup, flags | _O_BINARY);
    if (fd < 0) {
        int e = errno;
        CloseHandle(dup);
        errno = e ? e : EMFILE;
        return NULL;
    }
    FILE *s = _fdopen(fd, mode);
    if (!s) {
        int e = errno;
        _close(fd);                                         /* closes dup */
        errno = e ? e : ENOMEM;
        return NULL;
    }
    NtClose(h);
    return s;
}

COMMON_API FILE *plat_file_stdio(plat_file f, const char *mode) {
    char m[8];
    size_t n = strlen(mode);
    if (n + 2 > sizeof m) { errno = EINVAL; return NULL; }
    memcpy(m, mode, n + 1);
    if (!strchr(m, 'b')) { m[n] = 'b'; m[n + 1] = 0; }       /* binary, always (r4:I50) */
    int flags = m[0] == 'r' && !strchr(m, '+') ? _O_RDONLY : m[0] == 'a' ? _O_APPEND : 0;
    return pw_stdio(f.h, m, flags);
}

COMMON_API int plat_read_all(plat_file f, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        DWORD k = 0, want = n - got > 0x40000000u ? 0x40000000u : (DWORD)(n - got);
        if (!ReadFile(f.h, (char *)buf + got, want, &k, NULL)) return pw_last(WR_OP_OPEN);
        if (k == 0) return EIO;                             /* shorter than n */
        got += k;
    }
    return 0;
}

COMMON_API int plat_read(plat_file f, void *buf, size_t n, size_t *got) {
    DWORD k = 0, want = n > 0x40000000u ? 0x40000000u : (DWORD)n;
    *got = 0;
    if (!ReadFile(f.h, buf, want, &k, NULL)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : pw_last(WR_OP_OPEN);
    *got = k;
    return 0;
}

COMMON_API int plat_file_stat(plat_file f, plat_stat *st) { return pw_stat_h(f.h, st); }

COMMON_API int plat_file_touch(plat_file f) { return pw_set_mtime_now(f.h); }

COMMON_API int plat_pread(plat_file f, void *buf, size_t n, int64_t off, size_t *got) {
    /* an offset read moves a synchronous handle's position: it is put back afterwards (a handle
     * isn't read from two threads at once) */
    LARGE_INTEGER zero = {.QuadPart = 0}, at;
    *got = 0;
    if (off < 0) return EINVAL;
    if (!SetFilePointerEx(f.h, zero, &at, FILE_CURRENT)) return pw_last(WR_OP_OPEN);
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)((uint64_t)off & 0xFFFFFFFFu);
    o.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD k = 0, want = n > 0x40000000u ? 0x40000000u : (DWORD)n;
    int e = 0;
    if (!ReadFile(f.h, buf, want, &k, &o) && GetLastError() != ERROR_HANDLE_EOF) e = pw_last(WR_OP_OPEN);
    SetFilePointerEx(f.h, at, NULL, FILE_BEGIN);
    if (!e) *got = k;
    return e;
}

COMMON_API int plat_fsync(plat_file f) { return FlushFileBuffers(f.h) ? 0 : pw_last(WR_OP_OPEN); }

COMMON_API int plat_map_rw(plat_file f, int64_t size, void **addr) {
    *addr = NULL;
    if (size <= 0) return EINVAL;
    LARGE_INTEGER eof = {.QuadPart = size};
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtSetInformationFile(f.h, &io, &eof, sizeof eof, PW_CLASS(PW_FileEndOfFileInformation));
    if (s) return pw_nt(s, WR_OP_OPEN);
    HANDLE m = CreateFileMappingW(f.h, NULL, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32), (DWORD)((uint64_t)size & 0xFFFFFFFFu), NULL);
    if (!m) return pw_last(WR_OP_OPEN);
    void *p = MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, (SIZE_T)size);
    int e = p ? 0 : pw_last(WR_OP_OPEN);
    CloseHandle(m);                                         /* the view keeps the mapping */
    *addr = p;
    return e;
}

COMMON_API void plat_unmap(void *addr, int64_t size) {
    (void)size;
    if (addr) UnmapViewOfFile(addr);
}

#ifdef PLAT_TEST_FLUSH
/* test builds only (common/tests/win.sh's plat-win-test, never a release object): the next n
 * flushes of plat_map_flush report err (0: ERROR_LOCK_VIOLATION) without flushing, so its retry
 * (WF3) can be proven, and every sleep between its tries is recorded; Windows doesn't fail a flush
 * under another handle's byte-range lock. The tests that set it are single-threaded. */
static int pw_flush_faults;
static DWORD pw_flush_err = ERROR_LOCK_VIOLATION;
static unsigned pw_sleep_ms[16];
static int pw_sleeps;
COMMON_API void plat_test_flush_faults(int n, unsigned err) {
    pw_flush_faults = n > 0 ? n : 0;
    pw_flush_err = err ? (DWORD)err : ERROR_LOCK_VIOLATION;
    pw_sleeps = 0;
}
COMMON_API int plat_test_flush_faults_left(void) { return pw_flush_faults; }
COMMON_API int plat_test_flush_sleeps(const unsigned **ms) {
    *ms = pw_sleep_ms;
    return pw_sleeps;
}
static BOOL pw_flush_view(void *addr, SIZE_T n) {
    if (pw_flush_faults > 0) {
        pw_flush_faults--;
        SetLastError(pw_flush_err);
        return FALSE;
    }
    return FlushViewOfFile(addr, n);
}
static void pw_sleep(DWORD ms) {
    if (pw_sleeps < 16) pw_sleep_ms[pw_sleeps] = (unsigned)ms;
    pw_sleeps++;
    Sleep(ms);
}
#else
#define pw_flush_view FlushViewOfFile
#define pw_sleep Sleep
#endif

COMMON_API int plat_map_flush(void *addr, int64_t size) {
    if (!addr || size <= 0) return 0;
    if ((uint64_t)size > (uint64_t)(SIZE_T)-1) return EINVAL;
    /* a page another writer holds (ERROR_LOCK_VIOLATION) is tried again, 10 times, about 0.3 s in
     * all (review WF3); any other error, or the last one, maps as before */
    for (int i = 0;; i++) {
        if (pw_flush_view(addr, (SIZE_T)size)) return 0;
        DWORD g = GetLastError();
        if (g != ERROR_LOCK_VIOLATION || i == 9) return wr_errno_win32((uint32_t)g, WR_OP_OPEN);
        pw_sleep(1u << (i < 6 ? i : 6));
    }
}

COMMON_API int plat_stat_path(const char *path, int follow, plat_stat *st) {
    /* following links: the path as Win32 reads it (wr_full_path); not following: the strict rule */
    HANDLE h;
    int e = follow ? pw_open_full(path, FILE_READ_ATTRIBUTES, 0, &h)
                   : pw_open(NULL, path, FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_OPEN_REPARSE_POINT, WR_OP_OPEN, &h);
    if (e) return e;
    e = pw_stat_h(h, st);
    NtClose(h);
    return e;
}

COMMON_API int plat_rename_path(const char *from, const char *to) {
    int e;
    PW_RETRY(e, pw_rename_once(NULL, from, NULL, to, 1));
    return e;
}

COMMON_API int plat_unlink_path(const char *path) {
    int e;
    PW_RETRY(e, pw_unlink_once(NULL, path));
    return e;
}

COMMON_API FILE *plat_fopen_w(const char *path, int append) {
    /* the bridge's open_w: made if missing, no link followed, only a regular file (else EINVAL);
     * appending, or truncated */
    HANDLE h;
    ACCESS_MASK a = (append ? FILE_APPEND_DATA : FILE_WRITE_DATA) | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;
    int e = pw_open_nofollow(NULL, path, a, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, WR_OP_OPEN, &h);
    if (e == EISDIR) e = EINVAL;
    if (!e && GetFileType(h) != FILE_TYPE_DISK) { NtClose(h); e = EINVAL; }
    if (e) { errno = e; return NULL; }
    if (!append) {
        LARGE_INTEGER zero = {.QuadPart = 0};
        IO_STATUS_BLOCK io;
        NTSTATUS s = NtSetInformationFile(h, &io, &zero, sizeof zero, PW_CLASS(PW_FileEndOfFileInformation));
        if (s) { NtClose(h); errno = pw_nt(s, WR_OP_OPEN); return NULL; }
    }
    FILE *f = pw_stdio(h, append ? "ab" : "wb", _O_WRONLY | (append ? _O_APPEND : 0));
    if (!f) {
        int k = errno;
        NtClose(h);
        errno = k;
    }
    return f;
}

COMMON_API int plat_read_file(const char *path, size_t max, char **buf, size_t *n) {
    /* config.ini: links followed, a regular file of at most max bytes */
    *buf = NULL;
    *n = 0;
    HANDLE h;
    int e = pw_open(NULL, path, FILE_GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, WR_OP_OPEN, &h);
    if (e == EISDIR) return PLAT_EFTYPE;
    if (e) return e;
    int64_t sz = 0;
    char *b = NULL;
    if ((e = pw_size(h, &sz)) == 0 && (GetFileType(h) != FILE_TYPE_DISK || (uint64_t)sz > max)) e = PLAT_EFTYPE;
    else if (!e && !(b = malloc(max + 1))) e = ENOMEM;
    else if (!e) {
        size_t got = 0;
        for (;;) {
            DWORD k = 0;
            if (got > max) break;
            if (!ReadFile(h, b + got, (DWORD)(max + 1 - got), &k, NULL)) { e = pw_last(WR_OP_OPEN); break; }
            if (k == 0) break;
            got += k;
        }
        if (!e && got > max) e = PLAT_EFTYPE;                /* grew past max while read */
        else if (!e) *n = got;
    }
    NtClose(h);
    if (e) { free(b); return e; }
    *buf = b;
    return 0;
}

/* ---------------------------------------------------------------- volumes and free space */

/* the volume root ("D:\", "C:\mnt\x\" or "\\?\Volume{...}\") holding a Win32 path */
static int pw_volume_root(const wchar_t *w, wchar_t *root, DWORD cap) {
    return GetVolumePathNameW(w, root, cap) ? 0 : pw_last(WR_OP_OPEN);
}

/* The volume of a path (T3c): its root as GetVolumePathNameW gives it (a drive "D:\", a folder
 * mount "C:\mnt\x\", or a volume GUID path) into root (cap units), and that volume's GUID path into
 * guid (64 units). A drive path of MAX_PATH bytes or more goes in as "\\?\X:\..." with '\'
 * separators (rekordbox isn't long-path aware: a chosen location's path may be 700 bytes), and a
 * "\\?\X:\..." root comes back without its "\\?\". The GUID path is always
 * GetVolumeNameForVolumeMountPointW's, a GUID root's too (its canonical name: a second name of the
 * same volume never stands for it, review TC3). guid NULL: the root alone (plat_free_bytes: a
 * mapped or SUBST drive has no GUID path but has free space, TC7). 0, or an errno. */
static int pw_volume_of(const char *path, wchar_t *root, DWORD cap, wchar_t *guid) {
    const char *rest;
    size_t l = strlen(path);
    int longp = wr_path_kind(path, &rest) == 1 && l >= MAX_PATH;
    wchar_t *w = malloc(sizeof(wchar_t) * (l + 5));
    if (!w) return ENOMEM;
    long k = wr_utf8_to_16(path, l, (uint16_t *)w + (longp ? 4 : 0), l + 1);
    if (k < 0) { free(w); return EINVAL; }
    if (longp) {
        memcpy(w, L"\\\\?\\", 4 * sizeof(wchar_t));
        for (long i = 4; i < k + 4; i++)
            if (w[i] == L'/') w[i] = L'\\';
    }
    int e = pw_volume_root(w, root, cap);
    free(w);
    if (e) return e;
    size_t rl = wcslen(root);
    if (rl >= 7 && !wcsncmp(root, L"\\\\?\\", 4) && root[5] == L':' && root[6] == L'\\') memmove(root, root + 4, (rl - 3) * sizeof(wchar_t));
    if (!guid) return 0;
    return GetVolumeNameForVolumeMountPointW(root, guid, 64) ? 0 : pw_last(WR_OP_OPEN);
}

/* whether path's drive letter is a network drive (GetDriveTypeW: DRIVE_REMOTE, mapped and
 * connected or not); a volume GUID path never is */
COMMON_API int plat_win_drive_remote(const char *path) {
    const char *rest;
    if (wr_path_kind(path, &rest) != 1) return 0;
    wchar_t root[4] = {(wchar_t)(unsigned char)path[0], L':', L'\\', 0};
    return GetDriveTypeW(root) == DRIVE_REMOTE;
}

COMMON_API int plat_volume_name(const char *path, char *label, size_t ln, char *letter, size_t lt) {
    if (ln) label[0] = 0;
    if (lt) letter[0] = 0;
    wchar_t root[MAX_PATH + 1], guid[64], name[MAX_PATH + 1];
    int e = pw_volume_of(path, root, MAX_PATH + 1, guid);
    if (e) return e;
    /* the volume that answered must be the path's own (review NR1): another one is ENOENT */
    const char *rest;
    int kind = wr_path_kind(path, &rest);
    if (kind == 2) {   /* "\\?\Volume{...}\": the same 49 units, case ignored */
        for (int i = 0; i < 49; i++)
            if (((wchar_t)(unsigned char)path[i] | 0x20) != (guid[i] | 0x20)) return ENOENT;
    } else if (kind == 1 || kind == 3) {   /* the root's letter is the path's (the root has no "\\?\") */
        wchar_t want = (wchar_t)(unsigned char)(kind == 3 ? path[4] : path[0]);
        if ((root[0] | 0x20) != (want | 0x20) || root[1] != L':') return ENOENT;
    }
    if (!GetVolumeInformationW(root, name, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0)) return pw_last(WR_OP_OPEN);
    if (ln && pw_to8(name, wcslen(name), label, ln) < 0) label[0] = 0;
    /* the volume's mount points: a double-NUL list; the first "X:\" one is its letter */
    DWORD need = 0;
    wchar_t small[MAX_PATH + 1], *list = small;
    if (!GetVolumePathNamesForVolumeNameW(guid, list, MAX_PATH + 1, &need)) {
        if (GetLastError() != ERROR_MORE_DATA || !need || !(list = malloc(sizeof(wchar_t) * need))) return 0;   /* the label alone */
        if (!GetVolumePathNamesForVolumeNameW(guid, list, need, &need)) {
            free(list);
            return 0;
        }
    }
    for (const wchar_t *m = list; *m; m += wcslen(m) + 1)
        if (((m[0] >= L'A' && m[0] <= L'Z') || (m[0] >= L'a' && m[0] <= L'z')) && m[1] == L':' && m[2] == L'\\' && !m[3]) {
            if (lt >= 3) {
                letter[0] = (char)(m[0] & ~0x20);
                letter[1] = ':';
                letter[2] = 0;
            }
            break;
        }
    if (list != small) free(list);
    return 0;
}

COMMON_API int plat_volume_info(const char *path, plat_volume *v) {
    wchar_t root[MAX_PATH + 1], guid[64], fs[32];
    int e = pw_volume_of(path, root, MAX_PATH + 1, guid);
    if (e) return e;
    DWORD flags = 0;
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, &flags, fs, 32)) return pw_last(WR_OP_OPEN);
    if (pw_to8(guid, wcslen(guid), v->mount, sizeof v->mount) < 0) return EINVAL;
    if (pw_to8(fs, wcslen(fs), v->fstype, sizeof v->fstype) < 0) v->fstype[0] = 0;
    for (char *c = v->fstype; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');   /* ntfs, exfat, fat32 (for the log) */
    UINT type = GetDriveTypeW(root);
    v->local = type != DRIVE_REMOTE && type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN;
    v->readonly = (flags & FILE_READ_ONLY_VOLUME) != 0;
    ULARGE_INTEGER avail, total;
    if (!GetDiskFreeSpaceExW(root, &avail, &total, NULL)) return pw_last(WR_OP_OPEN);
    v->total = total.QuadPart;
    v->avail = avail.QuadPart;                              /* FreeBytesAvailableToCaller: quotas respected */
    return 0;
}

/* the volume GUID path of the volume holding %SystemRoot% ("this computer") */
static int pw_system_volume(char *out, size_t n) {
    wchar_t win[MAX_PATH + 1], root[MAX_PATH + 1], guid[64];
    UINT k = GetSystemWindowsDirectoryW(win, MAX_PATH + 1);
    if (k == 0 || k > MAX_PATH || pw_volume_root(win, root, MAX_PATH + 1) || !GetVolumeNameForVolumeMountPointW(root, guid, 64))
        return -1;
    return pw_to8(guid, wcslen(guid), out, n) < 0 ? -1 : 0;
}

static int pw_same_ascii_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

COMMON_API int plat_volume_holding(const char *path, char *mount, size_t n, int *system) {
    /* the table of mounts: every volume and the folders it is mounted at; the longest one that is
     * path or a prefix of it at a separator. The path itself is never looked up. */
    const char *rest, *p = path;
    int kind = wr_path_kind(path, &rest);
    if (!kind) return EINVAL;
    if (kind == 3) p = path + 4;                            /* "\\?\X:\..." as "X:\..." */
    wchar_t vol[64];
    HANDLE f = FindFirstVolumeW(vol, 64);
    if (f == INVALID_HANDLE_VALUE) return EIO;
    size_t best = 0, cap = 4096;
    char bestvol[64] = "", one[1024], g[64];
    wchar_t *names = malloc(sizeof(wchar_t) * cap);
    int e = names ? 0 : ENOMEM;
    do {
        if (e || pw_to8(vol, wcslen(vol), g, sizeof g) < 0) continue;
        size_t l = wr_mount_holds(g, path);                 /* the volume's own GUID path */
        if (l > best) { best = l; memcpy(bestvol, g, sizeof g); }
        DWORD got = 0;
        if (!GetVolumePathNamesForVolumeNameW(vol, names, (DWORD)cap, &got)) continue;   /* not mounted anywhere */
        for (const wchar_t *m = names; *m; m += wcslen(m) + 1) {
            if (pw_to8(m, wcslen(m), one, sizeof one) < 0) continue;
            l = wr_mount_holds(one, p);
            if (l > best) { best = l; memcpy(bestvol, g, sizeof g); }
        }
    } while (FindNextVolumeW(f, vol, 64));
    FindVolumeClose(f);
    free(names);
    if (e) return e;
    if (!best) return ENOENT;
    char sys[64];
    if (pw_system_volume(sys, sizeof sys)) return EIO;      /* unknown: never taken for "not this computer's" (TC2) */
    *system = pw_same_ascii_ci(sys, bestvol);
    if (strlen(bestvol) >= n) return ENAMETOOLONG;
    memcpy(mount, bestvol, strlen(bestvol) + 1);
    return 0;
}

COMMON_API int plat_volume_mounted(const char *mount) {
    wchar_t *w = pw_win32_path(mount);
    if (!w) return 0;
    DWORD flags;
    int r = GetVolumeInformationW(w, NULL, 0, NULL, NULL, &flags, NULL, 0) != 0;
    free(w);
    return r;
}

COMMON_API uint64_t plat_free_bytes(const char *path, int finder, const char **how) {
    (void)finder;                                           /* Windows has no purgeable space */
    *how = "GetDiskFreeSpaceEx";
    wchar_t root[MAX_PATH + 1];
    ULARGE_INTEGER avail;
    int ok = !pw_volume_of(path, root, MAX_PATH + 1, NULL) && GetDiskFreeSpaceExW(root, &avail, NULL, NULL);
    return ok ? avail.QuadPart : 0;
}

/* ---------------------------------------------------------------- the process, its account */

COMMON_API int plat_pid(void) { return (int)GetCurrentProcessId(); }

COMMON_API int64_t plat_process_start(void) {
    FILETIME c, x, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u)) return 0;
    return wr_filetime_s((int64_t)(((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime));
}

/* FOLDERID_LocalAppData, {F1B32785-6FBA-4FCF-9D55-7B8E7F157091} (knownfolders.h) */
static const GUID pw_local_app_data = {0xf1b32785, 0x6fba, 0x4fcf, {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}};

COMMON_API int plat_home(char *out, size_t n) {
    /* LocalAppData from the registry through the shell, never the environment; an absolute local
     * "X:\..." path in strict UTF-8, else none (bridge.md §1). Never under the loader lock. */
    PWSTR p = NULL;
    int r = -1;
    if (n) out[0] = 0;
    if (SHGetKnownFolderPath(&pw_local_app_data, KF_FLAG_DONT_VERIFY, NULL, &p) == S_OK && p) {
        const char *rest;
        if (pw_to8(p, wcslen(p), out, n) >= 0 && wr_path_kind(out, &rest) == 1) {
            wchar_t root[4] = {p[0], L':', L'\\', 0};
            UINT t = GetDriveTypeW(root);
            r = t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_RAMDISK ? 0 : -1;
        }
    }
    if (p) CoTaskMemFree(p);
    if (r && n) out[0] = 0;
    return r;
}

/* ---------------------------------------------------------------- Program Files */

/* HKLM's ProgramFilesDir, the 64-bit view, REG_SZ only (RRF_NOEXPAND: a REG_EXPAND_SZ is read as
 * it is, then refused by its type, so the environment never takes part). ADVAPI32 only: no shell
 * call, nothing loaded on demand (the bridge's phase 1 reads it). */
static int pw_pf_registry(void *ctx, uint16_t *buf, size_t cap, size_t *n) {
    (void)ctx;
    HKEY k;
    DWORD type = 0, size = (DWORD)(cap * sizeof *buf);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"" WR_PF_KEY, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return -1;
    LSTATUS e = RegGetValueW(k, NULL, L"" WR_PF_VALUE, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, buf, &size);
    RegCloseKey(k);
    if (e != ERROR_SUCCESS || type != REG_SZ) return -1;
    *n = size / sizeof *buf;
    return 0;
}

#ifdef PLAT_TEST_PROGRAMS
/* test builds only: RBSP_TEST_PROGRAMS, the VM's fake trees (app.md §7) */
static int pw_pf_test(void *ctx, uint16_t *buf, size_t cap, size_t *n) {
    (void)ctx;
    DWORD k = GetEnvironmentVariableW(L"RBSP_TEST_PROGRAMS", (wchar_t *)buf, (DWORD)cap);
    if (!k || k >= cap) return -1;
    *n = k;
    return 0;
}
#endif

COMMON_API int plat_program_files(const char *sub, char *out, size_t n) {
    char pf[WR_PF_MAX + 1];
    wr_pf_reader read = pw_pf_registry;
#ifdef PLAT_TEST_PROGRAMS
    if (GetEnvironmentVariableW(L"RBSP_TEST_PROGRAMS", NULL, 0)) read = pw_pf_test;
#endif
    if (wr_program_files(read, NULL, pf, sizeof pf) < 0) {
        if (n) out[0] = 0;
        return -1;
    }
    return wr_program_path(pf, sub, out, n) < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------- owner check */

/* S-1-5-32-544 (Administrators), S-1-5-18 (SYSTEM), and TrustedInstaller
 * (S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464), as SID bytes */
static const unsigned char pw_sid_admins[] = {1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 0x02, 0, 0};
static const unsigned char pw_sid_system[] = {1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
static const unsigned char pw_sid_ti[] = {1,    6,    0,    0,    0,    0,    0,    5,    80,   0,    0,    0,    0xB5, 0x89,
                                          0xFB, 0x38, 0x19, 0x84, 0xC2, 0xCB, 0x5C, 0x6C, 0x23, 0x6D, 0x57, 0x00, 0x77, 0x6E,
                                          0xC0, 0x02, 0x64, 0x87};

static int pw_sid_is(PSID s, const unsigned char *want, size_t n) {
    return s && IsValidSid(s) && GetLengthSid(s) == n && !memcmp(s, want, n);
}

/* the running account's SID (a test build's accept_self), malloc'd; NULL when unknown */
static PSID pw_self_sid(void) {
    HANDLE tok;
    PSID r = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return NULL;
    union { TOKEN_USER u; unsigned char b[256]; } tu;
    DWORD len = 0;
    if (GetTokenInformation(tok, TokenUser, &tu, sizeof tu, &len) && (r = malloc(GetLengthSid(tu.u.User.Sid))))
        CopySid(GetLengthSid(tu.u.User.Sid), r, tu.u.User.Sid);
    CloseHandle(tok);
    return r;
}

static int pw_trusted(PSID s, PSID self) {
    return pw_sid_is(s, pw_sid_admins, sizeof pw_sid_admins) || pw_sid_is(s, pw_sid_system, sizeof pw_sid_system)
           || pw_sid_is(s, pw_sid_ti, sizeof pw_sid_ti) || (self && s && EqualSid(s, self));
}

/* the rule on one descriptor: win_rules.h's wr_acl_ok over its owner and its DACL's ACEs */
static int pw_sd_ok(PSECURITY_DESCRIPTOR sd, PSID self) {
    PSID owner = NULL;
    BOOL defaulted, present = FALSE;
    PACL dacl = NULL;
    if (!GetSecurityDescriptorOwner(sd, &owner, &defaulted) || !GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted))
        return 0;
    if (!present || !dacl) return wr_acl_ok(pw_trusted(owner, self), present, 1, NULL, 0);
    ACL_SIZE_INFORMATION si;
    if (!GetAclInformation(dacl, &si, sizeof si, AclSizeInformation)) return 0;
    wr_ace *a = si.AceCount ? malloc(sizeof *a * si.AceCount) : NULL;
    if (si.AceCount && !a) return 0;
    int ok = 1;
    for (DWORD i = 0; i < si.AceCount && ok; i++) {
        ACE_HEADER *h;
        if (!GetAce(dacl, i, (LPVOID *)&h)) { ok = 0; break; }
        a[i].type = h->AceType;
        a[i].flags = h->AceFlags;
        a[i].mask = 0;
        a[i].trusted = 0;
        if (h->AceType == ACCESS_ALLOWED_ACE_TYPE || h->AceType == ACCESS_DENIED_ACE_TYPE) {
            ACCESS_ALLOWED_ACE *x = (ACCESS_ALLOWED_ACE *)h;   /* the same layout for both */
            a[i].mask = x->Mask;
            a[i].trusted = pw_trusted((PSID)&x->SidStart, self);
        }
    }
    if (ok) ok = wr_acl_ok(pw_trusted(owner, self), 1, 0, a, si.AceCount);
    free(a);
    return ok;
}

/* one component of the check: opened without following a reparse point, with READ_CONTROL (J8),
 * then not a reparse point, and the descriptor read from that handle passing the rule.
 * 0 ok, 1 refused, 2 missing. */
static int pw_owner_one(const char *path, int dir, PSID self) {
    HANDLE h;
    int e = pw_open_as(NULL, path, READ_CONTROL | FILE_READ_ATTRIBUTES, PW_SHARE_ALL, FILE_OPEN,
                       FILE_OPEN_REPARSE_POINT | (dir ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE), WR_OP_OPEN, 0, &h);
    if (e) return e == ENOENT ? 2 : 1;
    PSECURITY_DESCRIPTOR sd = NULL;
    int r = pw_no_reparse(h) || pw_security(h, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &sd) || !pw_sd_ok(sd, self);
    free(sd);
    NtClose(h);
    return r;
}

COMMON_API int plat_owner_check(const char *path, int levels, int accept_self, char *bad, size_t n) {
    /* the file and its `levels` parent folders, as written (no reparse point anywhere among them),
     * owned by Administrators, SYSTEM or TrustedInstaller and writable by no one else */
    const char *rest;
#ifndef PLAT_TEST_OWNER
    accept_self = 0;                                        /* a release build never accepts its own account */
#endif
    if (n) bad[0] = 0;
    size_t l = strlen(path);
    if (!wr_path_kind(path, &rest) || l >= 32767) return PLAT_OWNER_REFUSED;
    char *dir = malloc(l + 1);
    if (!dir) return PLAT_OWNER_REFUSED;
    memcpy(dir, path, l + 1);
    PSID self = accept_self ? pw_self_sid() : NULL;
    int r = pw_owner_one(path, 0, self), result = PLAT_OWNER_OK;
    if (r == 2) result = PLAT_OWNER_MISSING;
    else if (r) {
        result = PLAT_OWNER_FILE;
        if (n) snprintf(bad, n, "%s", path);
    } else
        for (int level = 0; level < levels; level++) {
            char *s1 = strrchr(dir, '\\'), *s2 = strrchr(dir, '/'), *slash = !s1 ? s2 : !s2 ? s1 : s1 > s2 ? s1 : s2;
            if (!slash || slash < dir + (rest - path)) break;   /* at the volume's root: not checked, as the Mac's "/" */
            *slash = 0;
            if (pw_owner_one(dir, 1, self)) {
                result = PLAT_OWNER_FOLDER;
                if (n) snprintf(bad, n, "%s", dir);
                break;
            }
        }
    free(self);
    free(dir);
    return result;
}

/* plat_lib_open_checked's step: `name` opened relative to root (or the absolute path when root is
 * NULL), without following a reparse point, for reading (FILE_LIST_DIRECTORY on a folder,
 * FILE_READ_DATA on the file: data access, so the share mode counts) with READ_CONTROL, sharing
 * READ only: no one may write, rename or delete it while it is held, and a handle someone
 * already holds for that fails the open. Its descriptor is then checked on that handle.
 * 0 ok (*h held), 1 refused, 2 missing. */
static int pw_checked_one(HANDLE root, const char *name, int dir, PSID self, HANDLE *h) {
    int e = pw_open_as(root, name, (dir ? FILE_LIST_DIRECTORY : FILE_READ_DATA) | READ_CONTROL | FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ, FILE_OPEN, FILE_OPEN_REPARSE_POINT | (dir ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE),
                       WR_OP_OPEN, 0, h);
    if (e) return e == ENOENT ? 2 : 1;                      /* a sharing violation (EBUSY) is a refusal */
    PSECURITY_DESCRIPTOR sd = NULL;
    int r = pw_no_reparse(*h) || pw_security(*h, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &sd) || !pw_sd_ok(sd, self);
    free(sd);
    if (r) {
        NtClose(*h);
        *h = NULL;
    }
    return r;
}

/* the handles plat_owner_hold keeps open: the parents top-down, then the file */
struct plat_owner_held {
    int n;
    HANDLE h[WR_LEVELS_MAX + 1];
};

COMMON_API void plat_owner_release(plat_owner_held *h) {
    if (!h) return;
    while (h->n > 0) NtClose(h->h[--h->n]);
    free(h);
}

COMMON_API int plat_owner_hold(const char *path, int levels, int accept_self, char *bad, size_t n, plat_owner_held **out) {
    /* plat_owner_check's rule, held (owner decided, review B1): the top parent opened by its path,
     * each one below relative to the one above, the file relative to its folder; all of them
     * stay open until plat_owner_release. Nothing checked can change while they are held. */
    size_t sep[WR_LEVELS_MAX];
    plat_owner_held *held = NULL;
    char *name = NULL;
    int result = PLAT_OWNER_OK;
#ifndef PLAT_TEST_OWNER
    accept_self = 0;                                        /* a release build never accepts its own account */
#endif
    *out = NULL;
    if (n) bad[0] = 0;
    size_t l = strlen(path);
    int k = l < 32767 ? wr_parents(path, levels, sep) : -1;
    if (k < 0 || !(name = malloc(l + 1)) || !(held = calloc(1, sizeof *held))) {
        free(name);
        return PLAT_OWNER_REFUSED;
    }
    PSID self = accept_self ? pw_self_sid() : NULL;
    /* parents, top-down: parent j+1 is path[0, sep[j]); below the top one, by its own name */
    for (int j = k - 1; j >= -1 && result == PLAT_OWNER_OK; j--) {
        size_t from = j == k - 1 ? 0 : sep[j + 1] + 1, to = j >= 0 ? sep[j] : l;
        memcpy(name, path + from, to - from);
        name[to - from] = 0;
        HANDLE root = j == k - 1 ? NULL : held->h[held->n - 1];
        int r = pw_checked_one(root, name, j >= 0, self, &held->h[held->n]);
        if (r == 0) held->n++;
        else if (r == 2) result = PLAT_OWNER_MISSING;       /* a missing folder: the file is missing too */
        else {
            result = j >= 0 ? PLAT_OWNER_FOLDER : PLAT_OWNER_FILE;
            if (n) snprintf(bad, n, "%.*s", (int)to, path);
        }
    }
    free(self);
    free(name);
    if (result == PLAT_OWNER_OK) *out = held;
    else plat_owner_release(held);
    return result;
}

COMMON_API int plat_lib_open_checked(const char *path, int levels, int accept_self, char *bad, size_t n, plat_lib **out,
                                     const char **err) {
    /* LoadLibraryExW while plat_owner_hold's handles are held; then they are closed. Nothing
     * checked can change between the check and the load. */
    plat_owner_held *h = NULL;
    *out = NULL;
    if (err) *err = NULL;
    int result = plat_owner_hold(path, levels, accept_self, bad, n, &h);
    if (result == PLAT_OWNER_OK) *out = plat_lib_open(path, err);   /* the loader's read/execute open shares with ours */
    plat_owner_release(h);
    return result;
}

COMMON_API int plat_win_handle_admin_only(void *handle, int accept_self) {
#ifndef PLAT_TEST_OWNER
    accept_self = 0;                                        /* a release build never accepts its own account */
#endif
    PSID self = accept_self ? pw_self_sid() : NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    int ok = !pw_no_reparse(handle) && !pw_security(handle, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &sd) && pw_sd_ok(sd, self);
    free(sd);
    free(self);
    return ok;
}

/* ---------------------------------------------------------------- libraries */

COMMON_API plat_lib *plat_lib_open(const char *path, const char **err) {
    static _Thread_local char msg[80];
    wchar_t *w = pw_win32_path(path);
    if (!w) {
        if (err) *err = "the path isn't valid UTF-8";
        return NULL;
    }
    /* never LOAD_WITH_ALTERED_SEARCH_PATH: the library's folder and System32 only (helper.md §1, I62).
     * A file that isn't a valid image makes the loader raise a hard error, a modal "Bad Image"
     * box, in a process whose error mode allows it (rekordbox's does: measured in the VM, its
     * start then waits on the box): this thread's error mode fails it instead, for the load only */
    DWORD old_mode = 0;
    BOOL set = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
    HMODULE m = LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    DWORD le = GetLastError();
    if (set) SetThreadErrorMode(old_mode, NULL);
    free(w);
    if (!m && err) {
        snprintf(msg, sizeof msg, "LoadLibraryExW failed (error %lu)", (unsigned long)le);
        *err = msg;
    }
    return (plat_lib *)(void *)m;
}

COMMON_API void *plat_lib_sym(plat_lib *l, const char *name) {
    FARPROC f = GetProcAddress((HMODULE)(void *)l, name);
    void *p;
    memcpy(&p, &f, sizeof p);
    return p;
}

COMMON_API const char *plat_lib_path_of(const void *addr) {
    /* a buffer of the calling thread's, valid until its next call */
    static _Thread_local char path[3 * MAX_PATH + 1];
    HMODULE m = NULL;
    wchar_t w[MAX_PATH + 1];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m))
        return NULL;
    DWORD k = GetModuleFileNameW(m, w, MAX_PATH + 1);
    if (k == 0 || k > MAX_PATH || pw_to8(w, k, path, sizeof path) < 0) return NULL;
    return path;
}
