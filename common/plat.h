/* plat.h v1: the platform layer shared by the bridge, the helper, the app's C core and the Windows
 * admin worker (the private Windows plan's bridge §0a). Each OS keeps its fast native paths behind it
 * (§0b): on the Mac plat_mac.c is today's code one wrapper deep (openat and O_NOFOLLOW,
 * fgetattrlist, st_blocks, GCD with its QoS classes, CommonCrypto, vDSP, CoreFoundation's free
 * space); on Windows plat_win.c (bridge.md §1, T3a-d).
 *
 * Rules for every function here:
 *  - Paths and names are UTF-8. A name is one component (no separator).
 *  - A function that can fail returns 0 or a positive errno value (EFTYPE: PLAT_EFTYPE). It never
 *    returns -1 unless it says so. The Windows layer maps NTSTATUS to these (bridge.md §1).
 *  - Files are opened without following a link at the last component, close-on-exec (not
 *    inherited), in binary mode, and never block on a FIFO.
 *  - Handles are values: plat_dir and plat_file. A handle a function failed to open is left
 *    closed; the caller closes every one it got.
 *  - The cheap primitives (thread id, mutex, condition, once, sleep, sha256) are static inline in
 *    this header; the rest is declared COMMON_API (api.h): static in the bridge's one translation
 *    unit, external everywhere else.
 *
 * core/plat_shim.h's names are kept as they are (plat_thread_id, plat_mutex, plat_cond,
 * plat_now_mono_ns, plat_sleep_ms...), so the core's move from its shim to this header is an
 * include change (API-V1-NOTES C6). The timer and the fake clock the core waits for are below
 * (Timers, Clocks). */
#ifndef RBSP_PLAT_H
#define RBSP_PLAT_H

#include "api.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>   /* AT_FDCWD: plat_rename_noreplace_at's inline */
#  include <pthread.h>
#endif
#if defined(__APPLE__)
#  include <CommonCrypto/CommonDigest.h>
#endif

/* ---------------------------------------------------------------- errors */

#ifdef EFTYPE
#define PLAT_EFTYPE EFTYPE      /* not a regular file, or over the size asked for */
#else
#define PLAT_EFTYPE 79          /* the Mac's value: Windows has no EFTYPE */
#endif

/* ---------------------------------------------------------------- threads, locks, once */

#if defined(_WIN32)
typedef DWORD plat_thread_id;
typedef SRWLOCK plat_mutex;
typedef CONDITION_VARIABLE plat_cond;
typedef INIT_ONCE plat_once_t;
#define PLAT_MUTEX_INIT SRWLOCK_INIT
#define PLAT_ONCE_INIT INIT_ONCE_STATIC_INIT
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
static inline void plat_cond_broadcast(plat_cond *c) { WakeAllConditionVariable(c); }
static inline void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
typedef pthread_t plat_thread_id;
typedef pthread_mutex_t plat_mutex;
typedef pthread_cond_t plat_cond;
typedef pthread_once_t plat_once_t;
#define PLAT_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER   /* not recursive, as SRWLOCK */
#define PLAT_ONCE_INIT PTHREAD_ONCE_INIT
static inline plat_thread_id plat_thread_self(void) { return pthread_self(); }
static inline int plat_thread_equal(plat_thread_id a, plat_thread_id b) { return pthread_equal(a, b) != 0; }
static inline void plat_mutex_init(plat_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void plat_mutex_destroy(plat_mutex *m) { pthread_mutex_destroy(m); }
static inline void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(m); }
static inline void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(m); }
static inline void plat_cond_init(plat_cond *c) { pthread_cond_init(c, NULL); }
static inline void plat_cond_destroy(plat_cond *c) { pthread_cond_destroy(c); }
static inline void plat_cond_wait(plat_cond *c, plat_mutex *m) { pthread_cond_wait(c, m); }
static inline void plat_cond_signal(plat_cond *c) { pthread_cond_signal(c); }
static inline void plat_cond_broadcast(plat_cond *c) { pthread_cond_broadcast(c); }
static inline void plat_sleep_ms(uint32_t ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
#endif
/* runs fn once per *o (static, PLAT_ONCE_INIT), the other callers waiting until it has */
COMMON_API void plat_once(plat_once_t *o, void (*fn)(void));
/* waits on c until signalled or about ns nanoseconds have passed: 0 signalled (or spurious),
 * ETIMEDOUT. Measured on the real monotonic clock, never the fake one. */
COMMON_API int plat_cond_wait_ns(plat_cond *c, plat_mutex *m, uint64_t ns);

/* ---------------------------------------------------------------- clocks */

/* The monotonic clock (Mac: CLOCK_MONOTONIC, the bridge's; it doesn't stop in sleep) and the wall
 * clock (since 1970). A test build (PLAT_TEST_CLOCK) can switch both to a fake clock, which moves
 * only through plat_fake_advance; release builds have no fake clock. */
COMMON_API uint64_t plat_now_mono_ns(void);
COMMON_API int64_t plat_now_wall_ns(void);
COMMON_API int64_t plat_now_wall_s(void);
/* local time of t (seconds since 1970), as localtime_r */
COMMON_API void plat_localtime(int64_t t, struct tm *out);

/* ---------------------------------------------------------------- queues and timers */

typedef void (*plat_fn)(void *ctx);
typedef struct plat_queue plat_queue;
/* What a queue's work is. MAINT: maintenance (validate, index loads, prune): the Mac's
 * QOS_CLASS_UTILITY; Windows: below-normal priority and the low I/O hint (bridge.md §1). */
enum { PLAT_WORK_DEFAULT, PLAT_WORK_MAINT };

/* A serial queue: its jobs run one at a time, in the order queued, off the caller's thread.
 * Mac: a GCD serial queue (MAINT at QOS_CLASS_UTILITY). Never freed (the bridge's live as long as
 * the process); NULL when it can't be made. */
COMMON_API plat_queue *plat_queue_new(const char *label, int work);
/* queues fn(ctx); 0, or an errno when it can't be queued (never on the Mac: GCD can't refuse) */
COMMON_API int plat_async(plat_queue *q, plat_fn fn, void *ctx);
/* queues fn(ctx) on q after delay_ns (not cancellable: see plat_timer); 0 or an errno */
COMMON_API int plat_after(plat_queue *q, uint64_t delay_ns, plat_fn fn, void *ctx);
/* returns once every job queued on q before the call has run. Never from a job of q itself. */
COMMON_API void plat_queue_wait(plat_queue *q);
/* fn(ctx, i) for every i < n, in parallel on the work class's threads (Mac: dispatch_apply on
 * the global queue of that QoS), returning when all have run */
COMMON_API void plat_par_for(size_t n, int work, void (*fn)(void *ctx, size_t i), void *ctx);
/* the calling thread's work class (PLAT_WORK_MAINT inside a maintenance queue's job) */
COMMON_API int plat_work_class(void);

/* A timer that can be cancelled: fn(ctx) runs once on q (NULL: the layer's own maintenance
 * queue) after delay_ns, unless plat_timer_cancel comes first. The owner calls plat_timer_cancel
 * exactly once per timer, whether or not it has fired, and never uses it after: it returns 1 when
 * fn will never run, 0 when fn has run or is running. NULL when out of memory. The core's delayed
 * post to main is this timer's fn calling core_post_main (on Windows there is no main queue here:
 * main is the shell's). */
typedef struct plat_timer plat_timer;
COMMON_API plat_timer *plat_timer_new(plat_queue *q, uint64_t delay_ns, plat_fn fn, void *ctx);
COMMON_API int plat_timer_cancel(plat_timer *t);

#ifdef PLAT_TEST_CLOCK
/* Test builds only. on=1: the clocks read mono_ns and wall_ns from now on, and every timer
 * (plat_after, plat_timer_new) armed while it is on waits for plat_fake_advance. on=0: the real
 * clocks again (timers still waiting stay so until advanced). */
COMMON_API void plat_fake_clock(int on, uint64_t mono_ns, int64_t wall_ns);
/* moves both fake clocks by ns; every waiting timer then due runs, earliest first (equal times in
 * the order armed), on the calling thread, with the clocks at its due time while it runs;
 * timers those arm that are due by the end run too */
COMMON_API void plat_fake_advance(uint64_t ns);
/* how many timers wait for plat_fake_advance (cancelled ones not counted) */
COMMON_API size_t plat_fake_pending(void);
#endif

#if defined(__BLOCKS__) && !defined(PLAT_NO_BLOCKS)
/* The same with Clang blocks, for the bridge and the helper on both OSes: the Phase 0 spike kept
 * blocks (bridge.md §0 "Blocks"): on Windows, -fblocks with the vendored BlocksRuntime, and
 * plat_win.c implements these with Block_copy/Block_release. The core and the worker build with
 * PLAT_NO_BLOCKS and use the function-pointer forms only, so they never link BlocksRuntime. */
COMMON_API int plat_async_b(plat_queue *q, void (^b)(void));
COMMON_API int plat_after_b(plat_queue *q, uint64_t delay_ns, void (^b)(void));
COMMON_API void plat_par_for_b(size_t n, int work, void (^b)(size_t i));
#endif

/* ---------------------------------------------------------------- sha256 */

/* SHA-256, bit-identical on both OSes (the cache's keys move with a drive). Mac: CommonCrypto,
 * inline. Windows: BCrypt through the BCRYPT_SHA256_ALG_HANDLE pseudo-handle, bcrypt.dll loaded
 * on first use by its full System32 path (bridge.md §0a, I31): there init can fail (nonzero), and
 * then nothing else may be called on the context. After a successful init, final must always be
 * called, also on a path that gives up (it releases the hash). update can fail silently on
 * Windows; final then returns nonzero, and the digest must not be used. */
#if defined(__APPLE__)
typedef CC_SHA256_CTX plat_sha256_ctx;
static inline int plat_sha256_init(plat_sha256_ctx *c) { CC_SHA256_Init(c); return 0; }
static inline void plat_sha256_update(plat_sha256_ctx *c, const void *p, size_t n) {
    for (; n > 0x40000000u; n -= 0x40000000u, p = (const char *)p + 0x40000000u) CC_SHA256_Update(c, p, 0x40000000u);
    CC_SHA256_Update(c, p, (CC_LONG)n);
}
static inline int plat_sha256_final(plat_sha256_ctx *c, unsigned char out[32]) { CC_SHA256_Final(out, c); return 0; }
#else
typedef struct { void *hash; } plat_sha256_ctx;
COMMON_API int plat_sha256_init(plat_sha256_ctx *c);
COMMON_API void plat_sha256_update(plat_sha256_ctx *c, const void *p, size_t n);
COMMON_API int plat_sha256_final(plat_sha256_ctx *c, unsigned char out[32]);
#endif
/* one buffer's digest; 0, or nonzero when the OS's hash can't be had (Windows only) */
static inline int plat_sha256(const void *p, size_t n, unsigned char out[32]) {
    plat_sha256_ctx c;
    if (plat_sha256_init(&c)) return -1;
    plat_sha256_update(&c, p, n);
    return plat_sha256_final(&c, out);
}

/* ---------------------------------------------------------------- inverse real FFT */

/* The inverse real FFT of size n = 2^log2n that the bridge's rebuild_spec runs on rekordbox's
 * thread. Mac: vDSP (Accelerate); Windows: pocketfft (bridge.md §1, T2), checked against it on
 * the Mac. Input, packed: re[0] the DC term, im[0] the Nyquist term (both real), re[k] + i im[k]
 * for 0 < k < n/2. Output out[0..n-1]: the plain sum x[t] = sum over all n bins of X[k]
 * e^(2 pi i k t / n), X Hermitian (no 1/n). re and im are clobbered. One object per thread at a
 * time; NULL when out of memory. */
typedef struct plat_irfft plat_irfft;
COMMON_API plat_irfft *plat_irfft_new(unsigned log2n);
COMMON_API void plat_irfft_run(plat_irfft *f, float *re, float *im, float *out);
COMMON_API void plat_irfft_free(plat_irfft *f);

/* ---------------------------------------------------------------- files and folders */

/* Handles. Mac: a descriptor. Windows: a HANDLE (NtCreateFile relative to its folder). */
#if defined(_WIN32)
typedef struct { void *h; } plat_dir;
typedef struct { void *h; } plat_file;
#else
typedef struct { int fd; } plat_dir;
typedef struct { int fd; } plat_file;
#endif
/* the folder relative names are opened against when there is none: an absolute path then */
COMMON_API plat_dir plat_dir_cwd(void);
COMMON_API int plat_dir_ok(plat_dir d);       /* whether d holds an open folder */
COMMON_API int plat_file_ok(plat_file f);
COMMON_API void plat_dir_close(plat_dir d);   /* a handle that isn't open is ignored */
COMMON_API void plat_file_close(plat_file f);

/* A file's identity: its volume and its number on it. Mac: st_dev and st_ino. Windows: the
 * volume serial and the 128-bit FileId. Compare with plat_id_same. */
typedef struct { uint64_t vol, file[2]; } plat_id;
COMMON_API int plat_id_same(plat_id a, plat_id b);
/* the 64-bit file number (the inode; Windows: the low 64 bits of the FileId — the whole index on
 * NTFS/FAT, not unique per file on ReFS), for names like model-<n> */
COMMON_API uint64_t plat_id_number(plat_id a);

/* What a name is, never following a link: a regular file, a folder, or OTHER (a link, a FIFO, a
 * device, a reparse point). alloc: the disk space it takes (Mac: st_blocks * 512; Windows:
 * AllocationSize), the cache's size rule (common/entries.c). mtime: seconds since 1970. */
enum { PLAT_OTHER, PLAT_FILE, PLAT_DIR };
typedef struct { int kind; int64_t size, alloc, mtime; plat_id id; } plat_stat;

/* -- folders */
/* opens the folder at an absolute path (no link at its last component: a link there: ELOOP, its
 * target never looked up) */
COMMON_API int plat_dir_open(const char *path, plat_dir *out);
/* opens folder `name` in d (no link: a link there: ELOOP, its target never looked up) */
COMMON_API int plat_dir_open_at(plat_dir d, const char *name, plat_dir *out);
/* opens the folder at an absolute path following links, the last component's included (Mac:
 * open with O_DIRECTORY; Windows: without FILE_OPEN_REPARSE_POINT, so a junction or a folder
 * symlink there is resolved): ENOTDIR when it ends at no folder. Only for a parent whose child
 * the caller then checks by identity through it (plat_stat_at against an id it took itself), as
 * the Mac's removefile follows the parents of what it deletes (review SF2). On the Mac it is
 * static inline here, never in plat_mac.c, so the shipped helper and bridge, which never call it,
 * stay byte-identical (mac-unchanged) */
#if defined(_WIN32)
COMMON_API int plat_dir_open_follow(const char *path, plat_dir *out);
#else
#  include <fcntl.h>
static inline int plat_dir_open_follow(const char *path, plat_dir *out) {
    out->fd = -1;
    if (path[0] != '/') return EINVAL;   /* an absolute path, as on Windows */
    out->fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (out->fd >= 0) return 0;
    return errno == EILSEQ ? EINVAL : errno ? errno : EIO;   /* plat_mac.c's pm_err */
}
#endif
/* makes folder `name` in d (0755; EEXIST when there is something of that name) */
COMMON_API int plat_mkdir_at(plat_dir d, const char *name);
/* makes path and every missing folder above it (0755), errors ignored, as `mkdir -p` would */
COMMON_API void plat_mkdirs(const char *path);
/* whether d is stored under exactly this name: a volume that ignores case opens "AB" for "ab"
 * (Mac: fgetattrlist's ATTR_CMN_NAME; Windows: FileNameInfo) */
COMMON_API int plat_dir_name_is(plat_dir d, const char *name);
COMMON_API int plat_dir_stat(plat_dir d, plat_stat *st);
/* whether this process may create names in d (Mac: faccessat W_OK with the effective ids;
 * Windows: AccessCheck of the folder's descriptor): 0, EROFS, EACCES or another errno. Nothing is
 * created. */
COMMON_API int plat_dir_writable(plat_dir d);
/* fn(ctx, name, st) for every name in d but "." and "..", from the first each time, st as
 * plat_stat_at gives it (names gone meanwhile are skipped; Windows: ids are FileIdInformation's, except on a file system
 * that lists only 64-bit ids (FAT, exFAT, some third-party drivers), where they are the 64-bit index). 0 when the folder was read whole; -1 when it couldn't be (a name that
 * can't be looked at, a read error) or fn returned nonzero (which stops the walk). */
typedef int (*plat_each_fn)(void *ctx, const char *name, const plat_stat *st);
COMMON_API int plat_dir_each(plat_dir d, plat_each_fn fn, void *ctx);
#if defined(__BLOCKS__) && !defined(PLAT_NO_BLOCKS)
COMMON_API int plat_dir_each_b(plat_dir d, int (^f)(const char *name, const plat_stat *st));
#endif

/* -- names in a folder */
COMMON_API int plat_stat_at(plat_dir d, const char *name, plat_stat *st);
COMMON_API int plat_unlink_at(plat_dir d, const char *name);
/* sets the modification time of `name` in d to now, without following a link there (a link's own
 * time; Mac: utimensat with AT_SYMLINK_NOFOLLOW) */
COMMON_API int plat_touch_at(plat_dir d, const char *name);
/* renames over an existing file (atomic on the Mac and on NTFS); on Windows retried on EBUSY on
 * the maintenance and writer threads (bridge.md §1) */
COMMON_API int plat_rename_at(plat_dir from, const char *a, plat_dir to, const char *b);
/* renames, never over an existing name: EEXIST when b is there, whatever it is (Mac: renameatx_np
 * with RENAME_EXCL; Windows: the rename with ReplaceIfExists FALSE, retried on EBUSY as
 * plat_rename_at). For a file put in place only if nothing is there (models.c's saved originals).
 * The Mac's is static inline: the helper links plat_mac.c, and a new function there would change
 * its bytes (make mac-unchanged); only win/c and the tests call it on the Mac */
#if defined(_WIN32)
COMMON_API int plat_rename_noreplace_at(plat_dir from, const char *a, plat_dir to, const char *b);
#else
static inline int plat_name_plain_(plat_dir d, const char *n) {   /* plat_mac.c's pm_name_ok */
    if (d.fd == AT_FDCWD) return n[0] == '/';
    if (!n[0] || (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])))) return 0;
    for (const char *p = n; *p; p++)
        if (*p == '/') return 0;
    return 1;
}
static inline int plat_rename_noreplace_at(plat_dir from, const char *a, plat_dir to, const char *b) {
    if (!plat_name_plain_(from, a) || !plat_name_plain_(to, b)) return EINVAL;
    if (!renameatx_np(from.fd, a, to.fd, b, RENAME_EXCL)) return 0;
    return errno == EILSEQ ? EINVAL : errno ? errno : EIO;
}
#endif

/* -- files */
/* Opens `name` in d for reading only if it is a regular file of at most max bytes (else
 * PLAT_EFTYPE), without following a link or blocking on a FIFO. *size: its size (may be NULL). */
COMMON_API int plat_file_open_at(plat_dir d, const char *name, int64_t max, int64_t *size, plat_file *out);
/* opens an absolute path for reading, following links (a model file rekordbox names), only if it
 * is a regular file (else EINVAL), without blocking on a FIFO */
COMMON_API int plat_file_open_path(const char *path, plat_file *out);
/* creates `name` in d, which must not exist (0644, no link followed); rw: readable too */
COMMON_API int plat_file_create_at(plat_dir d, const char *name, int rw, plat_file *out);
/* A stdio stream on f in binary mode ("b" is added when mode lacks it). On success the stream
 * owns f (fclose closes it); NULL and f still the caller's otherwise. */
COMMON_API FILE *plat_file_stdio(plat_file f, const char *mode);
/* exactly n bytes into buf: 0, or an errno (EIO for a file shorter than that) */
COMMON_API int plat_read_all(plat_file f, void *buf, size_t n);
/* up to n bytes: 0 with *got (0 at the end), or an errno */
COMMON_API int plat_read(plat_file f, void *buf, size_t n, size_t *got);
COMMON_API int plat_file_stat(plat_file f, plat_stat *st);
/* sets the file's modification time to now (a cache hit's "last used") */
COMMON_API int plat_file_touch(plat_file f);
/* up to n bytes at offset off, the file's position unchanged: 0 with *got (0 past the end), or an
 * errno */
COMMON_API int plat_pread(plat_file f, void *buf, size_t n, int64_t off, size_t *got);
/* the file's data to its disk (Mac: fsync; Windows: FlushFileBuffers): 0 or an errno. The cache
 * never needs it (an entry is a cache); the worker's and the core's state files may. */
COMMON_API int plat_fsync(plat_file f);
/* Maps f read-write and shared, after setting its size to size bytes (Mac: ftruncate + mmap;
 * Windows: SetEndOfFile + a file mapping): 0 with *addr, or an errno. plat_unmap ends it; writes
 * reach the file by then (not necessarily its disk). */
COMMON_API int plat_map_rw(plat_file f, int64_t size, void **addr);
COMMON_API void plat_unmap(void *addr, int64_t size);
/* the mapped bytes [addr, addr + size) written to the file, before plat_unmap, where the file is
 * then flushed to its disk with plat_fsync: 0 or an errno (Windows: FlushViewOfFile, which
 * FlushFileBuffers then completes, as Microsoft documents for mapped writes; the Mac: nothing to
 * do, the page cache is the file's, so a static inline 0 and the Mac's objects don't change).
 * Never called inside plat_unmap: the cache's mapped writes don't need it (the helper's speed). */
#if defined(_WIN32)
COMMON_API int plat_map_flush(void *addr, int64_t size);
#elif defined(plat_map_flush)
/* a test build's stub (-Dplat_map_flush=<its name>, win/c/tests/flush_fault.c, review WF6) */
int plat_map_flush(void *addr, int64_t size);
#else
static inline int plat_map_flush(void *addr, int64_t size) {
    (void)addr;
    (void)size;
    return 0;
}
#endif

/* -- paths (logs, state, config: files under the account's own folders) */
/* stat of a path, following links when follow is set (else as plat_stat_at) */
COMMON_API int plat_stat_path(const char *path, int follow, plat_stat *st);
COMMON_API int plat_rename_path(const char *from, const char *to);
COMMON_API int plat_unlink_path(const char *path);
/* opens path for writing, made if missing (0644), not following a link at it: appending, or
 * truncated. Only a regular file (else errno EINVAL), never blocking on a FIFO. NULL with errno
 * set when it can't. */
COMMON_API FILE *plat_fopen_w(const char *path, int append);
/* A whole small file (config.ini), following links: at most max bytes of a regular file, read
 * into *buf (malloc'd, max + 1 bytes; the caller frees it), its length in *n. 0; ENOENT when it
 * doesn't exist; another errno (PLAT_EFTYPE: not a regular file, or too big) otherwise. */
COMMON_API int plat_read_file(const char *path, size_t max, char **buf, size_t *n);

/* ---------------------------------------------------------------- volumes and free space */

#define PLAT_MOUNT_MAX 1024     /* the Mac's MNAMELEN */
typedef struct {
    char mount[PLAT_MOUNT_MAX]; /* where its volume is mounted (Windows: \\?\Volume{GUID}\) */
    char fstype[16];            /* apfs, exfat, msdos, ntfs... (for the log) */
    int local, readonly;
    uint64_t total, avail;      /* bytes: the volume's size and what this account may still use */
} plat_volume;
/* the volume holding path, as the OS reports it for that path (Mac: statfs) */
COMMON_API int plat_volume_info(const char *path, plat_volume *v);
/* The mount point holding path, found in the table of mounts without looking the path up (no
 * link can lead it elsewhere): the longest one that is path itself or a prefix of it at a
 * separator. *system:
 * whether it is this computer's own volume (Mac: "/" or the Data volume; Windows: the volume
 * holding %SystemRoot%). 0; ENOENT when none holds it (the drive isn't connected); another errno
 * when the table can't be read. */
COMMON_API int plat_volume_holding(const char *path, char *mount, size_t n, int *system);
/* whether a volume is still mounted exactly there */
COMMON_API int plat_volume_mounted(const char *mount);
/* Free bytes on path's volume. finder=1 (the Mac): "available for important usage" as Finder
 * shows it (it counts purgeable space), else the file system's own count; *how says which was
 * used. 0 when neither answers. */
COMMON_API uint64_t plat_free_bytes(const char *path, int finder, const char **how);

/* ---------------------------------------------------------------- the process, its account */

COMMON_API int plat_pid(void);
/* when this process started, in seconds since 1970 (0 when unknown): with the pid it tells this
 * process from a later one with the same pid */
COMMON_API int64_t plat_process_start(void);
/* The account's home folder from the user database (Windows: the LocalAppData known folder),
 * never from the environment: 0, or -1 when there is none, it isn't absolute or it doesn't fit */
COMMON_API int plat_home(char *out, size_t n);

/* Whether only an administrator can change the file at path: the file (after links) and its
 * `levels` parent folders are owned by root and writable by no one else (Windows: no reparse
 * point, owner and every write-granting ACE Administrators, SYSTEM or TrustedInstaller;
 * bridge.md §1). accept_self: the running account may own them too, in a test build only: it is
 * ignored (taken as 0) unless the build defines PLAT_TEST_OWNER, as the fake clock needs
 * PLAT_TEST_CLOCK (the bridge's test builds will define it beside RBSTEMS_TEST_REAL_PATH). */
enum { PLAT_OWNER_OK, PLAT_OWNER_MISSING, PLAT_OWNER_FILE, PLAT_OWNER_FOLDER, PLAT_OWNER_REFUSED };
/* PLAT_OWNER_OK; MISSING (no such file: the loader will say so); FILE or FOLDER with that path in
 * bad (n bytes) when it fails; REFUSED (bad "") when the path can't even be checked (too long) */
COMMON_API int plat_owner_check(const char *path, int levels, int accept_self, char *bad, size_t n);

/* ---------------------------------------------------------------- libraries */

typedef struct plat_lib plat_lib;
/* loads a library by its absolute path, privately and at once (Mac: dlopen RTLD_NOW|RTLD_LOCAL);
 * NULL with *err (the loader's message, valid until the next call) when it can't */
COMMON_API plat_lib *plat_lib_open(const char *path, const char **err);
COMMON_API void *plat_lib_sym(plat_lib *l, const char *name);
/* the file of the module holding addr, or NULL */
COMMON_API const char *plat_lib_path_of(const void *addr);

#endif
