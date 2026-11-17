/* plat.h on the Mac: today's calls of the bridge and the helper, one wrapper deep (bridge.md §0b).
 * openat and O_NOFOLLOW, fgetattrlist, st_blocks, GCD with QOS_CLASS_UTILITY for maintenance,
 * dispatch_apply, vDSP, CoreFoundation's "important usage" free space, sysctl, getpwuid_r. Each
 * function does what the bridge's own code does today (named in its comment), so the bridge's
 * switch onto it (T1b2) changes no behaviour. The bridge #includes this file (api.h); the helper
 * and the core compile it on its own.
 *
 * Where a Mac call answers differently from plat_win.c for the same case, the answer is
 * plat_win.c's, so both OSes behave the same behind plat.h (T1b1): a name relative to a folder is
 * one component (else EINVAL, before any call); a link of any kind where a folder is opened is
 * ELOOP (never followed to classify it; T1b2 review TB1); an unlink of a folder is EISDIR (not
 * EPERM); a name the volume can't encode is EINVAL (not EILSEQ); plat_fopen_w of a folder or a FIFO
 * is EINVAL; reads ask for at most 1 GB at a time; plat_par_for from maintenance work stays
 * maintenance work; a deadline past the clock's end never comes. A call that succeeds makes the
 * same system calls as before, behind one check of its name; a failing one may make one more stat.
 *
 * A known difference left in v1: plat_win.c refuses every relative path (wr_nt_path); here only
 * plat_dir_open, plat_volume_holding, plat_owner_check and the _at calls against plat_dir_cwd()
 * do. The other path calls (plat_stat_path, plat_rename_path, plat_unlink_path, plat_fopen_w,
 * plat_read_file, plat_file_open_path, plat_mkdirs) still take one; no Mac caller passes one. */
#include "plat.h"

#include <Accelerate/Accelerate.h>
#include <Block.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

_Static_assert(PLAT_MOUNT_MAX == MNAMELEN, "plat_volume.mount holds a Mac mount point");
_Static_assert(PLAT_EFTYPE == EFTYPE, "the Mac's EFTYPE");

/* ---------------------------------------------------------------- threads, locks, once */

COMMON_API void plat_once(plat_once_t *o, void (*fn)(void)) { pthread_once(o, fn); }

COMMON_API int plat_cond_wait_ns(plat_cond *c, plat_mutex *m, uint64_t ns) {
    struct timespec t = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    return pthread_cond_timedwait_relative_np(c, m, &t) == ETIMEDOUT ? ETIMEDOUT : 0;
}

/* ---------------------------------------------------------------- clocks, the fake clock */

#ifdef PLAT_TEST_CLOCK
/* the fake clock's state and its waiting timers (a list sorted by due time, then arming order) */
typedef struct pm_fake_job { uint64_t due; plat_fn fn; void *ctx; struct pm_fake_job *next; } pm_fake_job;
static plat_mutex pm_fake_lock = PLAT_MUTEX_INIT;
static int pm_fake_on;
static uint64_t pm_fake_mono;
static int64_t pm_fake_wall;
static pm_fake_job *pm_fake_jobs;

COMMON_API void plat_fake_clock(int on, uint64_t mono_ns, int64_t wall_ns) {
    plat_mutex_lock(&pm_fake_lock);
    pm_fake_on = on;
    pm_fake_mono = mono_ns;
    pm_fake_wall = wall_ns;
    plat_mutex_unlock(&pm_fake_lock);
}

/* 1 and the timer kept when the fake clock is on; 0 when it's off (the caller arms a real one) */
static int pm_fake_arm(uint64_t delay_ns, plat_fn fn, void *ctx, int *err) {
    plat_mutex_lock(&pm_fake_lock);
    if (!pm_fake_on) { plat_mutex_unlock(&pm_fake_lock); return 0; }
    pm_fake_job *j = malloc(sizeof *j), **at = &pm_fake_jobs;
    if (!j) { plat_mutex_unlock(&pm_fake_lock); *err = ENOMEM; return 1; }
    /* a deadline past the clock's end is UINT64_MAX, which never comes */
    j->due = delay_ns > UINT64_MAX - pm_fake_mono ? UINT64_MAX : pm_fake_mono + delay_ns;
    j->fn = fn;
    j->ctx = ctx;
    while (*at && (*at)->due <= j->due) at = &(*at)->next;   /* after those due at the same time */
    j->next = *at;
    *at = j;
    plat_mutex_unlock(&pm_fake_lock);
    *err = 0;
    return 1;
}

COMMON_API void plat_fake_advance(uint64_t ns) {
    plat_mutex_lock(&pm_fake_lock);
    uint64_t end = ns > UINT64_MAX - pm_fake_mono ? UINT64_MAX : pm_fake_mono + ns;
    while (pm_fake_jobs && pm_fake_jobs->due <= end && pm_fake_jobs->due != UINT64_MAX) {
        pm_fake_job *j = pm_fake_jobs;
        pm_fake_jobs = j->next;
        if (j->due > pm_fake_mono) {                        /* the clocks read the job's time while it runs */
            pm_fake_wall += (int64_t)(j->due - pm_fake_mono);
            pm_fake_mono = j->due;
        }
        plat_mutex_unlock(&pm_fake_lock);
        j->fn(j->ctx);
        free(j);
        plat_mutex_lock(&pm_fake_lock);
    }
    pm_fake_wall += (int64_t)(end - pm_fake_mono);
    pm_fake_mono = end;
    plat_mutex_unlock(&pm_fake_lock);
}

static int pm_timer_cancelled(void *t);
static void pm_timer_fire(void *p);
COMMON_API size_t plat_fake_pending(void) {
    size_t n = 0;
    plat_mutex_lock(&pm_fake_lock);
    for (pm_fake_job *j = pm_fake_jobs; j; j = j->next) n += !(j->fn == pm_timer_fire && pm_timer_cancelled(j->ctx));
    plat_mutex_unlock(&pm_fake_lock);
    return n;
}
#endif

COMMON_API uint64_t plat_now_mono_ns(void) {
#ifdef PLAT_TEST_CLOCK
    plat_mutex_lock(&pm_fake_lock);
    int on = pm_fake_on;
    uint64_t v = pm_fake_mono;
    plat_mutex_unlock(&pm_fake_lock);
    if (on) return v;
#endif
    return clock_gettime_nsec_np(CLOCK_MONOTONIC);         /* the bridge's (load_index, space_ok) */
}

COMMON_API int64_t plat_now_wall_ns(void) {
#ifdef PLAT_TEST_CLOCK
    plat_mutex_lock(&pm_fake_lock);
    int on = pm_fake_on;
    int64_t v = pm_fake_wall;
    plat_mutex_unlock(&pm_fake_lock);
    if (on) return v;
#endif
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

COMMON_API int64_t plat_now_wall_s(void) {
#ifdef PLAT_TEST_CLOCK
    plat_mutex_lock(&pm_fake_lock);
    int on = pm_fake_on;
    int64_t v = pm_fake_wall;
    plat_mutex_unlock(&pm_fake_lock);
    if (on) return v >= 0 ? v / 1000000000 : -((-v + 999999999) / 1000000000);
#endif
    return (int64_t)time(NULL);
}

COMMON_API void plat_localtime(int64_t t, struct tm *out) {
    time_t tt = (time_t)t;
    localtime_r(&tt, out);
}

/* ---------------------------------------------------------------- queues and timers */

#define PM_Q(q) ((dispatch_queue_t)(void *)(q))

/* delay_ns from now; a delay past INT64_MAX ns (292 years) is INT64_MAX, never a negative one
 * (which would run the job at once). INT64_MAX is DISPATCH_TIME_FOREVER, which dispatch_after
 * drops: the job never runs. */
static dispatch_time_t pm_when(uint64_t delay_ns) {
    return dispatch_time(DISPATCH_TIME_NOW, delay_ns > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)delay_ns);
}

COMMON_API plat_queue *plat_queue_new(const char *label, int work) {
    /* the bridge's writer (serial) and maint (serial, QOS_CLASS_UTILITY) */
    dispatch_queue_attr_t a = work == PLAT_WORK_MAINT
                                  ? dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)
                                  : DISPATCH_QUEUE_SERIAL;
    return (plat_queue *)(void *)dispatch_queue_create(label, a);
}

COMMON_API int plat_async(plat_queue *q, plat_fn fn, void *ctx) {
    dispatch_async_f(PM_Q(q), ctx, fn);
    return 0;
}

COMMON_API int plat_after(plat_queue *q, uint64_t delay_ns, plat_fn fn, void *ctx) {
#ifdef PLAT_TEST_CLOCK
    int err;
    if (pm_fake_arm(delay_ns, fn, ctx, &err)) return err;
#endif
    dispatch_after_f(pm_when(delay_ns), PM_Q(q), ctx, fn);
    return 0;
}

static void pm_nothing(void *ctx) { (void)ctx; }
COMMON_API void plat_queue_wait(plat_queue *q) { dispatch_sync_f(PM_Q(q), NULL, pm_nothing); }

COMMON_API int plat_work_class(void) {
    qos_class_t c = qos_class_self();
    return c != QOS_CLASS_UNSPECIFIED && c <= QOS_CLASS_UTILITY ? PLAT_WORK_MAINT : PLAT_WORK_DEFAULT;
}

/* the global queue plat_par_for runs on: maintenance work's (QOS_CLASS_UTILITY) when asked for it
 * or when the caller is itself maintenance work, as plat_win.c's workers inherit the caller's
 * class (r4:I51) */
static dispatch_queue_t pm_par_queue(int work) {
    int m = work == PLAT_WORK_MAINT || plat_work_class() == PLAT_WORK_MAINT;
    return dispatch_get_global_queue(m ? QOS_CLASS_UTILITY : QOS_CLASS_DEFAULT, 0);
}

COMMON_API void plat_par_for(size_t n, int work, void (*fn)(void *ctx, size_t i), void *ctx) {
    /* load_index's dispatch_apply on the utility global queue */
    if (n) dispatch_apply_f(n, pm_par_queue(work), ctx, fn);
}

/* timers: dispatch_after can't be cancelled, so a timer is a state the firing and the owner race
 * for, and two references (the queued firing's and the owner's) */
struct plat_timer { _Atomic int state, refs; plat_fn fn; void *ctx; };
enum { T_ARMED, T_FIRED, T_CANCELLED };

static void pm_timer_release(plat_timer *t) {
    if (atomic_fetch_sub(&t->refs, 1) == 1) free(t);
}

static void pm_timer_fire(void *p) {
    plat_timer *t = p;
    int s = T_ARMED;
    if (atomic_compare_exchange_strong(&t->state, &s, T_FIRED)) t->fn(t->ctx);
    pm_timer_release(t);
}

#ifdef PLAT_TEST_CLOCK
static int pm_timer_cancelled(void *t) { return atomic_load(&((plat_timer *)t)->state) == T_CANCELLED; }
#endif

static plat_queue *pm_timer_q;
static void pm_timer_q_init(void) { pm_timer_q = plat_queue_new("rbsp.plat.timers", PLAT_WORK_MAINT); }

COMMON_API plat_timer *plat_timer_new(plat_queue *q, uint64_t delay_ns, plat_fn fn, void *ctx) {
    static plat_once_t o = PLAT_ONCE_INIT;
    plat_timer *t = malloc(sizeof *t);
    if (!t) return NULL;
    atomic_init(&t->state, T_ARMED);
    atomic_init(&t->refs, 2);
    t->fn = fn;
    t->ctx = ctx;
    if (!q) {
        plat_once(&o, pm_timer_q_init);
        q = pm_timer_q;
    }
    if (!q || plat_after(q, delay_ns, pm_timer_fire, t)) { free(t); return NULL; }
    return t;
}

COMMON_API int plat_timer_cancel(plat_timer *t) {
    int s = T_ARMED, r = atomic_compare_exchange_strong(&t->state, &s, T_CANCELLED);
    pm_timer_release(t);
    return r;
}

#if defined(__BLOCKS__) && !defined(PLAT_NO_BLOCKS)
COMMON_API int plat_async_b(plat_queue *q, void (^b)(void)) {
    dispatch_async(PM_Q(q), b);
    return 0;
}

#ifdef PLAT_TEST_CLOCK
static void pm_block_run(void *p) {
    void (^b)(void) = (void (^)(void))p;
    b();
    Block_release(b);
}
#endif

COMMON_API int plat_after_b(plat_queue *q, uint64_t delay_ns, void (^b)(void)) {
#ifdef PLAT_TEST_CLOCK
    void (^c)(void) = Block_copy(b);
    int err;
    if (pm_fake_arm(delay_ns, pm_block_run, (void *)c, &err)) { if (err) Block_release(c); return err; }
    Block_release(c);
#endif
    dispatch_after(pm_when(delay_ns), PM_Q(q), b);
    return 0;
}

COMMON_API void plat_par_for_b(size_t n, int work, void (^b)(size_t i)) {
    if (n) dispatch_apply(n, pm_par_queue(work), b);
}
#endif

/* ---------------------------------------------------------------- inverse real FFT (vDSP) */

struct plat_irfft { FFTSetup setup; unsigned log2n; };

COMMON_API plat_irfft *plat_irfft_new(unsigned log2n) {
    plat_irfft *f = log2n >= 1 && log2n <= 20 ? malloc(sizeof *f) : NULL;
    if (!f) return NULL;
    f->log2n = log2n;
    if (!(f->setup = vDSP_create_fftsetup(log2n, kFFTRadix2))) { free(f); return NULL; }
    return f;
}

COMMON_API void plat_irfft_run(plat_irfft *f, float *re, float *im, float *out) {
    /* rebuild_spec's two calls: vDSP's inverse returns the plain sum, interleaved by vDSP_ztoc */
    DSPSplitComplex sc = {re, im};
    vDSP_fft_zrip(f->setup, &sc, 1, f->log2n, kFFTDirection_Inverse);
    vDSP_ztoc(&sc, 1, (DSPComplex *)out, 2, (vDSP_Length)1 << (f->log2n - 1));
}

COMMON_API void plat_irfft_free(plat_irfft *f) {
    if (!f) return;
    vDSP_destroy_fftsetup(f->setup);
    free(f);
}

/* ---------------------------------------------------------------- files and folders */

COMMON_API plat_dir plat_dir_cwd(void) { return (plat_dir){AT_FDCWD}; }
COMMON_API int plat_dir_ok(plat_dir d) { return d.fd >= 0; }
COMMON_API int plat_file_ok(plat_file f) { return f.fd >= 0; }
COMMON_API void plat_dir_close(plat_dir d) { if (d.fd >= 0) close(d.fd); }
COMMON_API void plat_file_close(plat_file f) { if (f.fd >= 0) close(f.fd); }

COMMON_API int plat_id_same(plat_id a, plat_id b) { return a.vol == b.vol && a.file[0] == b.file[0] && a.file[1] == b.file[1]; }
COMMON_API uint64_t plat_id_number(plat_id a) { return a.file[0]; }

static void pm_from_stat(const struct stat *s, plat_stat *o) {
    o->kind = S_ISREG(s->st_mode) ? PLAT_FILE : S_ISDIR(s->st_mode) ? PLAT_DIR : PLAT_OTHER;
    o->size = (int64_t)s->st_size;
    o->alloc = (int64_t)s->st_blocks * 512;                /* entry_size, prune */
    o->mtime = (int64_t)s->st_mtime;
    o->id.vol = (uint64_t)s->st_dev;
    o->id.file[0] = (uint64_t)s->st_ino;
    o->id.file[1] = 0;
}

/* errno of a failed call (r != 0). A name that isn't valid in the volume's encoding (APFS:
 * EILSEQ) is EINVAL, as plat_win.c's strict UTF-8 refuses it. */
static int pm_err(int r) {
    if (r == 0) return 0;
    return errno == EILSEQ ? EINVAL : errno ? errno : EIO;
}

/* plat.h's names: relative to a folder, one component (not "", "." or "..", no '/'), as
 * plat_win.c's wr_name16 has it, so no name leaves the folder or follows a link on the way;
 * relative to plat_dir_cwd(), an absolute path */
static int pm_name_ok(plat_dir d, const char *name) {
    if (d.fd == AT_FDCWD) return name[0] == '/';
    return name[0] && strcmp(name, ".") && strcmp(name, "..") && !strchr(name, '/');
}

/* A folder open that failed with ENOTDIR on a link (O_NOFOLLOW with O_DIRECTORY gives ENOTDIR
 * there) is ELOOP, whatever the link leads to, as plat_win.c refuses a directory reparse point:
 * any link at the last component, classified from the no-follow stat alone. Its target is never
 * looked up (a link to a hung mount must not stall the caller; T1b2 review TB1). */
static int pm_dir_err(int at, const char *name) {
    int e = pm_err(-1);
    struct stat s;
    if (e == ENOTDIR && fstatat(at, name, &s, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(s.st_mode))
        return ELOOP;
    return e;
}

/* an unlink of a folder (EPERM on the Mac) is EISDIR, as plat_win.c's FILE_NON_DIRECTORY_FILE */
static int pm_unlink_err(int at, const char *name) {
    int e = pm_err(-1);
    struct stat s;
    if (e == EPERM && fstatat(at, name, &s, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(s.st_mode)) return EISDIR;
    return e;
}

/* One read(2) or pread(2) asks for at most this: the Mac refuses a count over INT_MAX (EINVAL),
 * and plat_win.c's ReadFile takes 1 GB at a time too. */
#define PM_IO_MAX ((size_t)0x40000000u)

COMMON_API int plat_dir_open(const char *path, plat_dir *out) {
    out->fd = -1;
    if (path[0] != '/') return EINVAL;                      /* an absolute path, as on Windows */
    out->fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);   /* open_base, load_kept */
    return out->fd < 0 ? pm_dir_err(AT_FDCWD, path) : 0;
}

COMMON_API int plat_dir_open_at(plat_dir d, const char *name, plat_dir *out) {
    out->fd = -1;
    if (!pm_name_ok(d, name)) return EINVAL;
    out->fd = openat(d.fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);   /* sub_open */
    return out->fd < 0 ? pm_dir_err(d.fd, name) : 0;
}

COMMON_API int plat_mkdir_at(plat_dir d, const char *name) {
    if (!pm_name_ok(d, name)) return EINVAL;
    return pm_err(mkdirat(d.fd, name, 0755));
}

COMMON_API void plat_mkdirs(const char *path) {           /* the bridge's mkdirs */
    char tmp[PATH_MAX];
    int r = snprintf(tmp, sizeof tmp, "%s", path);
    if (r < 0 || (size_t)r >= sizeof tmp) return;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

/* The name in fgetattrlist's ATTR_CMN_NAME reply (got bytes at r: its length, then an
 * attrreference_t whose offset counts from itself): NULL unless it is a string of attr_length
 * bytes with its only NUL last, wholly inside both the bytes got and the length the reply gives
 * itself. Offsets are checked as numbers, never as pointers past the buffer. */
static const char *pm_attr_name(const unsigned char *r, size_t got) {
    uint32_t len;
    attrreference_t ref;
    if (got < sizeof len + sizeof ref) return NULL;
    memcpy(&len, r, sizeof len);
    memcpy(&ref, r + sizeof len, sizeof ref);
    if (len < got) got = len;                               /* the reply's own length bounds it too */
    if (ref.attr_dataoffset < 0 || ref.attr_length == 0) return NULL;
    size_t at = sizeof len + (size_t)ref.attr_dataoffset, l = ref.attr_length;
    if (at > got || l > got - at || r[at + l - 1] || memchr(r + at, 0, l - 1)) return NULL;
    return (const char *)r + at;
}

COMMON_API int plat_dir_name_is(plat_dir d, const char *name) {   /* the bridge's name_is */
    struct attrlist al = {.bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_NAME};
    union { uint32_t align; unsigned char b[4 + sizeof(attrreference_t) + 1024]; } a;
    if (fgetattrlist(d.fd, &al, a.b, sizeof a.b, 0) != 0) return 0;
    const char *n = pm_attr_name(a.b, sizeof a.b);
    return n && !strcmp(n, name);
}

COMMON_API int plat_dir_stat(plat_dir d, plat_stat *st) {
    struct stat s;
    if (fstat(d.fd, &s) != 0) return pm_err(-1);
    pm_from_stat(&s, st);
    return 0;
}

COMMON_API int plat_dir_writable(plat_dir d) {            /* check_location's write check */
    return pm_err(faccessat(d.fd, ".", W_OK, AT_EACCESS));
}

COMMON_API int plat_dir_each(plat_dir d, plat_each_fn fn, void *ctx) {   /* the bridge's each_name */
    int fd = dup(d.fd), r = 0, fe = 0, stop = 0;            /* fe: the first name's errno but ENOENT */
    DIR *dp = fd >= 0 ? fdopendir(fd) : NULL;
    if (!dp) { if (fd >= 0) close(fd); return -1; }
    rewinddir(dp);                                          /* a dup shares d's position: from the start every time */
    struct dirent *e;
    errno = 0;
    while ((e = readdir(dp))) {
        struct stat s;
        plat_stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (fstatat(d.fd, e->d_name, &s, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT && !fe) fe = errno;         /* gone since: fine */
        } else {
            pm_from_stat(&s, &st);
            if (fn(ctx, e->d_name, &st)) { stop = 1; break; }
        }
        errno = 0;
    }
    int le = errno;                                         /* readdir's; fn's own when it stopped the walk */
    if (stop || le) r = -1;
    else if (fe) { r = -1; le = fe; }                       /* listed but a name couldn't be looked at (a */
    closedir(dp);                                           /* folder without search permission: EACCES) */
    if (r) errno = le;
    return r;
}

#if defined(__BLOCKS__) && !defined(PLAT_NO_BLOCKS)
static int pm_each_block(void *ctx, const char *name, const plat_stat *st) {
    int (^f)(const char *, const plat_stat *) = (int (^)(const char *, const plat_stat *))ctx;
    return f(name, st);
}
COMMON_API int plat_dir_each_b(plat_dir d, int (^f)(const char *name, const plat_stat *st)) {
    return plat_dir_each(d, pm_each_block, (void *)f);
}
#endif

COMMON_API int plat_stat_at(plat_dir d, const char *name, plat_stat *st) {
    struct stat s;
    if (!pm_name_ok(d, name)) return EINVAL;
    if (fstatat(d.fd, name, &s, AT_SYMLINK_NOFOLLOW) != 0) return pm_err(-1);
    pm_from_stat(&s, st);
    return 0;
}

COMMON_API int plat_unlink_at(plat_dir d, const char *name) {
    if (!pm_name_ok(d, name)) return EINVAL;
    return unlinkat(d.fd, name, 0) == 0 ? 0 : pm_unlink_err(d.fd, name);
}

COMMON_API int plat_touch_at(plat_dir d, const char *name) {
    if (!pm_name_ok(d, name)) return EINVAL;
    return pm_err(utimensat(d.fd, name, NULL, AT_SYMLINK_NOFOLLOW));
}

COMMON_API int plat_rename_at(plat_dir from, const char *a, plat_dir to, const char *b) {
    if (!pm_name_ok(from, a) || !pm_name_ok(to, b)) return EINVAL;
    return pm_err(renameat(from.fd, a, to.fd, b));
}

COMMON_API int plat_file_open_at(plat_dir d, const char *name, int64_t max, int64_t *size, plat_file *out) {
    /* the bridge's checked_open */
    out->fd = -1;
    if (!pm_name_ok(d, name)) return EINVAL;
    int fd = openat(d.fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC), fl;
    if (fd < 0) return pm_err(-1);
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > max) { close(fd); return EFTYPE; }
    if ((fl = fcntl(fd, F_GETFL)) < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) { int e = pm_err(-1); close(fd); return e; }
    if (size) *size = (int64_t)st.st_size;
    out->fd = fd;
    return 0;
}

/* fd opened with O_NONBLOCK, kept only if it is a regular file (else EINVAL), then blocking again */
static int pm_regular(int fd) {
    struct stat st;
    int fl;
    if (fstat(fd, &st) != 0) return pm_err(-1);
    if (!S_ISREG(st.st_mode)) return EINVAL;
    if ((fl = fcntl(fd, F_GETFL)) < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) return pm_err(-1);
    return 0;
}

COMMON_API int plat_file_open_path(const char *path, plat_file *out) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC), e;   /* file_sha256's model */
    out->fd = -1;
    if (fd < 0) return pm_err(-1);
    if ((e = pm_regular(fd))) { close(fd); return e; }
    out->fd = fd;
    return 0;
}

COMMON_API int plat_file_create_at(plat_dir d, const char *name, int rw, plat_file *out) {
    /* write_desc, write_flac */
    out->fd = -1;
    if (!pm_name_ok(d, name)) return EINVAL;
    out->fd = openat(d.fd, name, (rw ? O_RDWR : O_WRONLY) | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    return out->fd < 0 ? pm_err(-1) : 0;
}

COMMON_API FILE *plat_file_stdio(plat_file f, const char *mode) {
    char m[8];
    size_t n = strlen(mode);
    if (n + 2 > sizeof m) { errno = EINVAL; return NULL; }
    memcpy(m, mode, n + 1);
    if (!strchr(m, 'b')) { m[n] = 'b'; m[n + 1] = 0; }
    return fdopen(f.fd, m);
}

COMMON_API int plat_read_all(plat_file f, void *buf, size_t n) {   /* the bridge's read_all */
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(f.fd, (char *)buf + got, n - got > PM_IO_MAX ? PM_IO_MAX : n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (r == 0) errno = EIO; return errno; }
        got += (size_t)r;
    }
    return 0;
}

COMMON_API int plat_read(plat_file f, void *buf, size_t n, size_t *got) {
    ssize_t r;
    if (n > PM_IO_MAX) n = PM_IO_MAX;                       /* a short read, as plat.h allows */
    while ((r = read(f.fd, buf, n)) < 0 && errno == EINTR) {}
    *got = r > 0 ? (size_t)r : 0;
    return r < 0 ? pm_err(-1) : 0;
}

COMMON_API int plat_file_stat(plat_file f, plat_stat *st) {
    struct stat s;
    if (fstat(f.fd, &s) != 0) return pm_err(-1);
    pm_from_stat(&s, st);
    return 0;
}

COMMON_API int plat_file_touch(plat_file f) { return pm_err(futimes(f.fd, NULL)); }   /* a hit */

COMMON_API int plat_pread(plat_file f, void *buf, size_t n, int64_t off, size_t *got) {
    ssize_t r;
    if (n > PM_IO_MAX) n = PM_IO_MAX;
    while ((r = pread(f.fd, buf, n, (off_t)off)) < 0 && errno == EINTR) {}
    *got = r > 0 ? (size_t)r : 0;
    return r < 0 ? pm_err(-1) : 0;
}

COMMON_API int plat_fsync(plat_file f) { return pm_err(fsync(f.fd)); }

COMMON_API int plat_map_rw(plat_file f, int64_t size, void **addr) {
    *addr = NULL;
    if (size <= 0) return EINVAL;
    if (ftruncate(f.fd, (off_t)size) != 0) return pm_err(-1);
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, f.fd, 0);
    if (p == MAP_FAILED) return pm_err(-1);
    *addr = p;
    return 0;
}

COMMON_API void plat_unmap(void *addr, int64_t size) { if (addr) munmap(addr, (size_t)size); }

COMMON_API int plat_stat_path(const char *path, int follow, plat_stat *st) {
    struct stat s;
    if ((follow ? stat(path, &s) : lstat(path, &s)) != 0) return pm_err(-1);
    pm_from_stat(&s, st);
    return 0;
}

COMMON_API int plat_rename_path(const char *from, const char *to) { return pm_err(rename(from, to)); }
COMMON_API int plat_unlink_path(const char *path) { return unlink(path) == 0 ? 0 : pm_unlink_err(AT_FDCWD, path); }

COMMON_API FILE *plat_fopen_w(const char *path, int append) {   /* the bridge's open_w */
    int fd = open(path, O_WRONLY | (append ? O_APPEND : O_TRUNC) | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644), e;
    if (fd < 0) {
        /* a folder (EISDIR) or a FIFO no one reads (ENXIO, O_NONBLOCK) is "not a regular file":
         * EINVAL, as plat_win.c gives it */
        struct stat s;
        e = pm_err(-1);
        if ((e == EISDIR || e == ENXIO) && lstat(path, &s) == 0 && !S_ISREG(s.st_mode) && !S_ISLNK(s.st_mode)) e = EINVAL;
        errno = e;
        return NULL;
    }
    if ((e = pm_regular(fd))) { close(fd); errno = e; return NULL; }
    FILE *f = fdopen(fd, append ? "ab" : "wb");
    if (!f) { e = errno; close(fd); errno = e; }
    return f;
}

COMMON_API int plat_read_file(const char *path, size_t max, char **buf, size_t *n) {   /* read_config */
    *buf = NULL;
    *n = 0;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return pm_err(-1);
    struct stat st;
    char *b = NULL;
    int e = 0;
    if (fstat(fd, &st) != 0) e = pm_err(-1);
    else if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size > max) e = EFTYPE;
    else if (!(b = malloc(max + 1))) e = ENOMEM;
    else {
        size_t got = 0;
        ssize_t r = 0;
        while (got <= max) {
            size_t want = max + 1 - got;
            if ((r = read(fd, b + got, want > PM_IO_MAX ? PM_IO_MAX : want)) < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (r < 0) e = pm_err(-1);
        else if (got > max) e = EFTYPE;                     /* grew past max while read */
        else *n = got;
    }
    close(fd);
    if (e) { free(b); return e; }
    *buf = b;
    return 0;
}

/* ---------------------------------------------------------------- volumes and free space */

COMMON_API int plat_volume_info(const char *path, plat_volume *v) {   /* log_using, check_location, disk_bytes */
    struct statfs fs;
    if (statfs(path, &fs) != 0) return pm_err(-1);
    strlcpy(v->mount, fs.f_mntonname, sizeof v->mount);
    strlcpy(v->fstype, fs.f_fstypename, sizeof v->fstype);
    v->local = (fs.f_flags & MNT_LOCAL) != 0;
    v->readonly = (fs.f_flags & MNT_RDONLY) != 0;
    v->total = (uint64_t)fs.f_blocks * fs.f_bsize;
    v->avail = (uint64_t)fs.f_bavail * fs.f_bsize;
    return 0;
}

COMMON_API int plat_volume_holding(const char *path, char *mount, size_t n, int *system) {   /* check_location */
    if (path[0] != '/') return EINVAL;                      /* an absolute path, as on Windows */
    int c = getfsstat(NULL, 0, MNT_NOWAIT), bi = -1;
    struct statfs *ms = c > 0 ? calloc((size_t)c + 4, sizeof *ms) : NULL;
    if (!ms) return c < 0 ? pm_err(-1) : c == 0 ? EIO : ENOMEM;
    c = getfsstat(ms, (c + 4) * (int)sizeof *ms, MNT_NOWAIT);
    if (c < 0) { int e = pm_err(-1); free(ms); return e; }   /* the table can't be read: not "none holds it" */
    size_t best = 0;
    for (int i = 0; i < c; i++) {
        const char *m = ms[i].f_mntonname;
        size_t l = strlen(m);
        int holds = !strcmp(m, "/") || (!strncmp(path, m, l) && (path[l] == '/' || path[l] == '\0'));
        if (holds && (bi < 0 || l > best)) { bi = i; best = l; }
    }
    if (bi < 0) { free(ms); return ENOENT; }
    *system = !strcmp(ms[bi].f_mntonname, "/") || !strcmp(ms[bi].f_mntonname, "/System/Volumes/Data") || (ms[bi].f_flags & MNT_ROOTFS);
    int e = strlcpy(mount, ms[bi].f_mntonname, n) >= n ? ENAMETOOLONG : 0;   /* never a cut mount point */
    if (e && n) mount[0] = 0;
    free(ms);
    return e;
}

COMMON_API int plat_volume_mounted(const char *mount) {   /* gone_why */
    struct statfs fs;
    return statfs(mount, &fs) == 0 && !strcmp(fs.f_mntonname, mount);
}

COMMON_API uint64_t plat_free_bytes(const char *path, int finder, const char **how) {   /* free_bytes */
    uint64_t b = 0;
    CFURLRef u = finder ? CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true) : NULL;
    CFTypeRef v = NULL;
    if (u && CFURLCopyResourcePropertyForKey(u, kCFURLVolumeAvailableCapacityForImportantUsageKey, &v, NULL) && v
        && CFGetTypeID(v) == CFNumberGetTypeID()) {
        int64_t x = 0;
        if (CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &x) && x > 0) b = (uint64_t)x;
    }
    if (v) CFRelease(v);
    if (u) CFRelease(u);
    if (b > 0) { *how = "Finder's count"; return b; }
    struct statfs fs;
    *how = "statfs";
    return statfs(path, &fs) == 0 ? (uint64_t)fs.f_bavail * fs.f_bsize : 0;
}

/* ---------------------------------------------------------------- the process, its account */

COMMON_API int plat_pid(void) { return (int)getpid(); }

COMMON_API int64_t plat_process_start(void) {             /* the bridge's process_start */
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && len > 0 ? (int64_t)kp.kp_proc.p_starttime.tv_sec : 0;
}

COMMON_API int plat_home(char *out, size_t n) {           /* the bridge's home_dir, without the test override */
    const char *home = NULL;
    struct passwd pw, *res = NULL;
    char buf[16384];
    if (n) out[0] = 0;                                      /* "" on every failure, as on Windows */
    if (getpwuid_r(getuid(), &pw, buf, sizeof buf, &res) == 0 && res) home = res->pw_dir;
    if (!home || home[0] != '/') return -1;
    int r = snprintf(out, n, "%s", home);
    if (r < 0 || (size_t)r >= n) { if (n) out[0] = 0; return -1; }
    return 0;
}

static int pm_owner_ok(const struct stat *st, int accept_self) {   /* the bridge's pm_owner_ok */
    if (accept_self && st->st_uid == getuid()) return !(st->st_mode & (S_IWGRP | S_IWOTH));
    return st->st_uid == 0 && !(st->st_mode & (S_IWGRP | S_IWOTH));
}

COMMON_API int plat_owner_check(const char *path, int levels, int accept_self, char *bad, size_t n) {
    /* the bridge's only_root_can_change */
    char real[PATH_MAX], dir[PATH_MAX];
    struct stat st;
#ifndef PLAT_TEST_OWNER
    accept_self = 0;                                        /* a release build never accepts its own account */
#endif
    if (n) bad[0] = 0;
    if (path[0] != '/') return PLAT_OWNER_REFUSED;          /* not absolute: as plat_win.c, never from the working folder */
    if (!realpath(path, real)) return errno == ENAMETOOLONG ? PLAT_OWNER_REFUSED : PLAT_OWNER_MISSING;
    if (stat(real, &st) != 0 || !pm_owner_ok(&st, accept_self)) {
        if (n) strlcpy(bad, real, n);
        return PLAT_OWNER_FILE;
    }
    if (strlcpy(dir, real, sizeof dir) >= sizeof dir) return PLAT_OWNER_REFUSED;
    for (int level = 0; level < levels; level++) {
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = 0;
        if (stat(dir, &st) != 0 || !pm_owner_ok(&st, accept_self)) {
            if (n) strlcpy(bad, dir, n);
            return PLAT_OWNER_FOLDER;
        }
    }
    return PLAT_OWNER_OK;
}

/* ---------------------------------------------------------------- libraries */

COMMON_API plat_lib *plat_lib_open(const char *path, const char **err) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);         /* open_real */
    if (!h && err) *err = dlerror();
    return (plat_lib *)h;
}

COMMON_API void *plat_lib_sym(plat_lib *l, const char *name) { return dlsym((void *)l, name); }

COMMON_API const char *plat_lib_path_of(const void *addr) {
    Dl_info ri;
    return dladdr(addr, &ri) ? ri.dli_fname : NULL;
}
