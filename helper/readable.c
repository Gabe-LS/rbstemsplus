/* readable.c: opening a track without waiting forever (readable.h) */
#include "readable.h"
#include "path_rules.h"
#include <errno.h>
#include <limits.h>
#if !defined(_WIN32)
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)

static const char *const PROTECTED[] = {"Documents", "Desktop", "Downloads"};
#define SETTINGS "System Settings › Privacy & Security › Files and Folders › RB Stems Plus"

const char *home_folder(void) {
    static char h[PATH_MAX];
    if (!h[0]) plat_home(h, sizeof h);          /* "" when there is none */
    return h;
}

const char *protected_folder(const char *path) {
    const char *h = home_folder();
    size_t hl = strlen(h);
    if (!hl || strncmp(path, h, hl) || path[hl] != '/') return "";
    for (size_t i = 0; i < sizeof PROTECTED / sizeof *PROTECTED; i++) {
        size_t l = strlen(PROTECTED[i]);
        if (!strncmp(path + hl + 1, PROTECTED[i], l) && (path[hl + 1 + l] == '/' || !path[hl + 1 + l])) return PROTECTED[i];
    }
    return "";
}

/* the folder a wait is remembered for: the protected folder, else the volume (/Volumes/<name>),
 * else the file's own folder */
static void wait_key(const char *path, char *key, size_t n) {
    const char *pf = protected_folder(path);
    if (pf[0]) { snprintf(key, n, "%s/%s", home_folder(), pf); return; }
    size_t root = paths_other_drive(path);
    if (root) {
        snprintf(key, n, "%.*s", (int)root, path);
        return;
    }
    const char *s = strrchr(path, '/');
    snprintf(key, n, "%.*s", s ? (int)(s - path) : 0, path);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done, abandoned, err;
    plat_finfo st;
    char path[PATH_MAX];
} job;

static void *run(void *arg) {
    job *j = arg;
    plat_finfo st = {0};
    plat_file f;
    int err = plat_finfo_path(j->path, &st);
    if (!err && st.kind == PLAT_FILE && !(err = plat_file_open_path(j->path, &f))) plat_file_close(f);
    pthread_mutex_lock(&j->mu);
    j->done = 1;
    j->err = err;
    j->st = st;
    int gone = j->abandoned;
    pthread_cond_signal(&j->cv);
    pthread_mutex_unlock(&j->mu);
    if (gone) { pthread_mutex_destroy(&j->mu); pthread_cond_destroy(&j->cv); free(j); }
    return NULL;
}

static char waited[32][PATH_MAX];       /* folders whose read was given up */
static int nwaited;

read_state readable(const char *path, double secs, plat_finfo *st, char *why, size_t n, const char **folder) {
    const char *pf = protected_folder(path);
    char key[PATH_MAX];
    *folder = pf;
    why[0] = 0;
    wait_key(path, key, sizeof key);
    char place[PATH_MAX + 16];
    if (pf[0]) snprintf(place, sizeof place, "your %s folder", pf);
    else snprintf(place, sizeof place, "%s", key[0] ? key : "/");
    for (int i = 0; i < nwaited; i++)
        if (!strcmp(waited[i], key)) {
            snprintf(why, n, "macOS is asking whether RB Stems Plus may read %s: answer its prompt, then check again", place);
            return READ_ASKING;
        }
    job *j = calloc(1, sizeof *j);
    if (!j || strlen(path) >= sizeof j->path) { free(j); snprintf(why, n, "the file can't be read: its path is too long"); return READ_FAILED; }
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    snprintf(j->path, sizeof j->path, "%s", path);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, run, j)) {
        pthread_attr_destroy(&a);
        run(j);                         /* no thread: read here (it may wait) */
    } else pthread_attr_destroy(&a);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    long long ns = until.tv_nsec + (long long)(secs * 1e9);
    until.tv_sec += (time_t)(ns / 1000000000LL);
    until.tv_nsec = (long)(ns % 1000000000LL);
    pthread_mutex_lock(&j->mu);
    while (!j->done && pthread_cond_timedwait(&j->cv, &j->mu, &until) != ETIMEDOUT) {}
    if (!j->done) {
        j->abandoned = 1;               /* the thread frees it when its read returns */
        pthread_mutex_unlock(&j->mu);
        if (nwaited < 32) snprintf(waited[nwaited++], PATH_MAX, "%s", key);
        snprintf(why, n, "macOS is asking whether RB Stems Plus may read %s: answer its prompt, then check again", place);
        return READ_ASKING;
    }
    int err = j->err;
    *st = j->st;
    pthread_mutex_unlock(&j->mu);
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    free(j);
    if (!err && st->kind != PLAT_FILE) { snprintf(why, n, "not a file"); return READ_FAILED; }
    if (!err) return READ_OK;
    if (err == ENOENT || err == ENOTDIR) {
        snprintf(why, n, "the file isn't there (moved, renamed, or its drive isn't connected)");
        return READ_ABSENT;
    }
    if (err == EPERM) {                 /* the privacy setting (a file's own permissions: EACCES) */
        if (pf[0]) snprintf(why, n, "macOS doesn't let RB Stems Plus read %s: allow it in " SETTINGS, place);
        else snprintf(why, n, "macOS doesn't let RB Stems Plus read it: allow it in " SETTINGS);
        return READ_DENIED;
    }
    snprintf(why, n, "the file can't be read: %s", strerror(err));
    return READ_FAILED;
}
#else /* _WIN32 */

/* Windows (helper.md §1, T14): there is no privacy prompt. The reads that may wait are a cloud
 * placeholder's (OneDrive's "online-only" files: reading one downloads it) and a drive's that
 * doesn't answer (a network share, a disconnected or sleeping disk). The attributes, the open and,
 * for a placeholder, the reads that bring it to this PC (until it is no longer a placeholder, or
 * its end) all run on their own thread (CreateThread, WaitForSingleObject's timeout: no pthreads,
 * no libwinpthread), so the decode's reads after READ_OK don't download it outside the wait.
 *
 * What is remembered for the run (the next run tries again): a drive that didn't answer, by a
 * canonical key (every separator '\', "\\?\UNC\" as "\\", "\\?\X:\" as "X:\", ASCII case folded):
 * "X:\" for a drive letter, the volume's root for a volume GUID path, and for a share
 * "\\server\share\folder" (a DFS namespace's links are folders of one share: a failed link
 * doesn't take its siblings), widened to "\\server\share" once a second folder of it fails or a
 * file directly under it does. A drive that answers with a network error (helper.md's list:
 * ENETUNREACH from plat_helper_win.c) is remembered as one that timed out. A placeholder that
 * took too long isn't (its download goes on, and each track gets its own wait).
 *
 * Reads given up whose thread still runs are counted by kind: another drive's (at most 8: past
 * that, another drive's read isn't started), a placeholder's download (at most 8: past that, a
 * job doesn't start a download), and the system drive's (at most 32, a backstop only), so neither
 * a dead share nor OneDrive keeps the system drive's tracks waiting. The decode's own reads are
 * bounded by readable_watch_start. */
#include <windows.h>

#define RD_OTHER_MAX 8                          /* another drive's given-up reads still running */
#define RD_FETCH_MAX 8                          /* given-up downloads still running */
#define RD_LOCAL_MAX 32                         /* the system drive's (a backstop) */
#define RD_BUSY (-1)                            /* the job's own result: too many downloads already */

const char *home_folder(void) {
    static char h[1024];
    if (!h[0]) plat_home(h, sizeof h);          /* "" when there is none */
    return h;
}

const char *protected_folder(const char *path) {
    (void)path;
    return "";                                  /* no folder asks before a read on Windows */
}

/* the read's step: attributes, open, a placeholder's download; RD_HYDRATE is the test gate's only
 * (each attribute check while a placeholder downloads): stage is never set to it */
enum { RD_LOOK, RD_OPEN, RD_FETCH, RD_HYDRATE, RD_GONE };   /* RD_GONE: given up while looking (stage only) */

static volatile LONG other_drive, fetch, local; /* given-up reads still running, by kind */

typedef struct {
    volatile LONG refs, stage;                  /* refs: the caller's and the thread's */
    volatile LONG state;                        /* 0 reading, 1 done (results set), 2 given up */
    volatile LONG *ctr;                         /* given up: the counter it counts in */
    int err;
    plat_finfo st;
    char *path;
} job;

#ifdef RBSP_READABLE_TEST
/* helper/tests/win-readable.c's: called on the job thread before each step; it may block, may
 * make a file look like a placeholder (*w, at RD_LOOK after the attributes), and returns an
 * errno to fail the step with, or 0 */
int readable_test_step(int step, const char *path, plat_where *w);
void readable_test_counts(long *o, long *f, long *l);
void readable_test_counts(long *o, long *f, long *l) {
    *o = InterlockedCompareExchange(&other_drive, 0, 0);
    *f = InterlockedCompareExchange(&fetch, 0, 0);
    *l = InterlockedCompareExchange(&local, 0, 0);
}
#define TEST_STEP(step, path, w) readable_test_step((step), (path), (w))
#else
#define TEST_STEP(step, path, w) 0
#endif

static void job_drop(job *j) {
    if (InterlockedDecrement(&j->refs) == 0) { free(j->path); free(j); }
}

/* a placeholder brought to this PC: read on (discarded) until it is no longer one, or its end */
static int hydrate(job *j, plat_file f) {
    char *b = malloc(1 << 20);
    if (!b) return ENOMEM;
    int err = 0;
    for (size_t want = 65536;; want = 1 << 20) {
        size_t got = 0;
        if ((err = plat_read(f, b, want, &got)) || !got) break;
        plat_where w;
        /* hydrated once no RECALL_ON_* is left: an OFFLINE-only file keeps the one 64 KB read
         * (reading never clears OFFLINE, review HR4) */
        if ((err = plat_path_where(j->path, &w)) || (err = TEST_STEP(RD_HYDRATE, j->path, &w)) || !w.placeholder) break;   /* RR2-4 */
    }
    free(b);
    return err;
}

static DWORD WINAPI run(void *arg) {
    job *j = arg;
    plat_where w;
    plat_finfo st = {0};
    plat_file f;
    /* the attributes only: nothing is downloaded yet (a drive that doesn't answer waits here) */
    int err = TEST_STEP(RD_LOOK, j->path, NULL);
    if (!err) err = plat_path_where(j->path, &w);
    if (!err) err = TEST_STEP(RD_LOOK, j->path, &w);
    if (!err) err = plat_finfo_path(j->path, &st);
    if (!err && st.kind == PLAT_FILE) {
        int dl = w.placeholder || w.offline;
        /* leaves RD_LOOK only if the caller hasn't given it up there (one CAS on stage, the
         * caller's too: review HR1, RR2-1), so a download is always counted as one */
        if (InterlockedCompareExchange(&j->stage, dl ? RD_FETCH : RD_OPEN, RD_LOOK) != RD_LOOK) err = ECANCELED;
        else if (dl && InterlockedCompareExchange(&fetch, 0, 0) >= RD_FETCH_MAX) err = RD_BUSY;   /* before any download */
        if (!err) err = TEST_STEP(dl ? RD_FETCH : RD_OPEN, j->path, NULL);
        if (!err && !(err = plat_track_open(j->path, &f))) {
            if (dl) err = hydrate(j, f);
            plat_file_close(f);
        }
    }
    j->err = err;
    j->st = st;
    if (InterlockedCompareExchange(&j->state, 1, 0) == 2) InterlockedDecrement(j->ctr);   /* given up: no longer running */
    job_drop(j);
    return 0;
}

/* ---------------------------------------------------------------- what didn't answer */

static char **keys;                             /* canonical keys, malloc'd; the caller's thread only */
static size_t nkeys, capkeys;

/* PATH's canonical form (malloc'd; NULL when out of memory) and, in *root and *folder, the
 * lengths of its drive's key and of its share's first-folder key (0: none); *unc for a share */
static char *canon(const char *path, size_t *root, size_t *folder, int *unc) {
    const char *p = path;
    char head[8] = "";
    *root = *folder = 0;
    *unc = 0;
    int sep0 = p[0] == '\\' || p[0] == '/', sep1 = p[1] == '\\' || p[1] == '/';
    if (sep0 && sep1 && p[2] == '?' && (p[3] == '\\' || p[3] == '/')) {
        if ((p[4] | 0x20) == 'u' && (p[5] | 0x20) == 'n' && (p[6] | 0x20) == 'c' && (p[7] == '\\' || p[7] == '/')) {
            p += 8;                             /* \\?\UNC\ -> \\ */
            strcpy(head, "\\\\");
            *unc = 1;
        } else if (((p[4] | 0x20) >= 'a' && (p[4] | 0x20) <= 'z') && p[5] == ':') p += 4;   /* \\?\X: -> X: */
    } else if (sep0 && sep1) *unc = 1;
    size_t n = strlen(head) + strlen(p);
    char *c = malloc(n + 1);
    if (!c) return NULL;
    snprintf(c, n + 1, "%s%s", head, p);
    for (char *x = c; *x; x++) {
        if (*x == '/') *x = '\\';
        else if (*x >= 'A' && *x <= 'Z') *x = (char)(*x | 0x20);
    }
    if (!strncmp(c, "\\\\?\\volume{", 11)) {          /* a volume GUID path: its volume's root, never a share */
        char g[50];
        *unc = 0;
        if (strlen(c) >= 49) {
            memcpy(g, c, 49);
            g[49] = 0;
            g[4] = 'V';                         /* paths_root_win spells it "Volume{" */
            *root = paths_root_win(g) == 49 ? 49 : 0;
        }
        return c;
    }
    size_t r = paths_other_drive(c);
    *root = r;
    if (*unc && r && c[r] == '\\' && c[r + 1]) {
        size_t e = r + 1;
        while (c[e] && c[e] != '\\') e++;
        if (c[e]) *folder = e;                  /* a folder under the share, not the file itself */
    }
    return c;
}

/* whether a stored key covers canonical path C (a prefix at a component boundary) */
static const char *remembered(const char *c) {
    for (size_t i = 0; i < nkeys; i++) {
        size_t l = strlen(keys[i]);
        if (!strncmp(c, keys[i], l) && (!c[l] || c[l] == '\\' || keys[i][l - 1] == '\\')) return keys[i];
    }
    return NULL;
}

static void store(const char *k, size_t l) {
    for (size_t i = 0; i < nkeys; i++)
        if (strlen(keys[i]) == l && !strncmp(keys[i], k, l)) return;
    if (nkeys == capkeys) {
        char **t = realloc(keys, sizeof *keys * (capkeys ? capkeys * 2 : 16));
        if (!t) return;                         /* not stored: still counted as another drive's */
        keys = t;
        capkeys = capkeys ? capkeys * 2 : 16;
    }
    char *s = malloc(l + 1);
    if (!s) return;
    memcpy(s, k, l);
    s[l] = 0;
    keys[nkeys++] = s;
}

/* canonical C's drive didn't answer: its folder key (a share), widened to the share once a second
 * folder of it, or a file directly under it, has failed; else its drive's key */
static void remember(const char *c, size_t root, size_t folder, int unc) {
    if (!root) return;
    if (!unc) { store(c, root); return; }
    if (!folder) { store(c, root); return; }    /* directly under the share */
    for (size_t i = 0; i < nkeys; i++)          /* another folder of this share failed already */
        if (strlen(keys[i]) > root && !strncmp(keys[i], c, root) && keys[i][root] == '\\'
            && (strlen(keys[i]) != folder || strncmp(keys[i], c, folder))) {
            store(c, root);
            return;
        }
    store(c, folder);
}

int readable_may_wait(const char *path) {
    if (paths_other_drive(path)) return 1;
    plat_where w;
    if (plat_path_where(path, &w)) return 0;    /* not there: looking costs nothing */
    return w.placeholder || w.offline || w.remote || w.removable;
}

read_state readable(const char *path, double secs, plat_finfo *st, char *why, size_t n, const char **folder) {
    *folder = "";
    why[0] = 0;
    size_t root, fold;
    int unc;
    char *c = canon(path, &root, &fold, &unc);
    if (!c) { snprintf(why, n, "out of memory"); return READ_FAILED; }
    int other = root != 0 || paths_other_drive(path) != 0;
    read_state rs = READ_FAILED;
    const char *k = other ? remembered(c) : NULL;
    if (k) {
        snprintf(why, n, "its drive %s didn't answer: check that it's connected, then check again", k);
        rs = READ_ASKING;
        goto out;
    }
    if (InterlockedCompareExchange(other ? &other_drive : &local, 0, 0) >= (other ? RD_OTHER_MAX : RD_LOCAL_MAX)) {
        snprintf(why, n, "Windows hasn't answered earlier reads %s yet: check again later", other ? "on other drives" : "of this PC's drive");
        rs = READ_ASKING;
        goto out;
    }
    job *j = calloc(1, sizeof *j);
    if (j) j->path = strdup(path);
    if (!j || !j->path) { if (j) free(j); snprintf(why, n, "out of memory"); goto out; }
    j->refs = 2;
    j->stage = RD_LOOK;
    HANDLE t = CreateThread(NULL, 0, run, j, 0, NULL);
    if (!t) {                                   /* never read on this thread: no wait would bound it */
        free(j->path);
        free(j);
        snprintf(why, n, "Windows couldn't start the read: check again");
        rs = READ_ASKING;
        goto out;
    }
    double ms = secs > 0 ? secs * 1000.0 + 0.5 : 0;
    DWORD ended = WaitForSingleObject(t, ms >= 4e9 ? INFINITE - 1 : (DWORD)ms) == WAIT_OBJECT_0;
    CloseHandle(t);                             /* the thread goes on if it's still reading */
    /* timed out while still looking: frozen there, it opens nothing (RR2-1) */
    LONG stage = ended ? InterlockedCompareExchange(&j->stage, 0, 0) : InterlockedCompareExchange(&j->stage, RD_GONE, RD_LOOK);
    /* given up unless its results are in: counted by kind until its thread ends */
    j->ctr = stage == RD_FETCH ? &fetch : other ? &other_drive : &local;
    InterlockedIncrement(j->ctr);
    LONG was = InterlockedCompareExchange(&j->state, 2, 0);
    if (was == 0 || (!ended && stage == RD_LOOK)) {
        if (was) InterlockedDecrement(j->ctr);  /* it ended at the freeze (or just then): given up all the same */
        job_drop(j);                            /* the thread frees it when its read returns */
        if (stage == RD_FETCH)
            snprintf(why, n, "it isn't on this PC yet: OneDrive (or another cloud service) is still downloading it; check again in a moment");
        else if (other && root) {
            remember(c, root, fold, unc);
            const char *r = remembered(c);
            snprintf(why, n, "its drive %s didn't answer within %.0f seconds: check that it's connected, then check again", r ? r : c, secs);
        } else if (other) snprintf(why, n, "its drive didn't answer within %.0f seconds: check that it's connected, then check again", secs);
        else snprintf(why, n, "Windows didn't open it within %.0f seconds: check again", secs);
        rs = READ_ASKING;
        goto out;
    }
    InterlockedDecrement(j->ctr);               /* it finished in time after all */
    int err = j->err;
    stage = j->stage;
    *st = j->st;
    job_drop(j);
    if (!err && st->kind != PLAT_FILE) { snprintf(why, n, "not a file"); goto out; }
    if (!err) { rs = READ_OK; goto out; }
    if (err == RD_BUSY) {
        snprintf(why, n, "OneDrive is still downloading earlier tracks: check again in a moment");
        rs = READ_ASKING;
    } else if (err == ENOENT || err == ENOTDIR) {
        snprintf(why, n, "the file isn't there (moved, renamed, or its drive isn't connected)");
        rs = READ_ABSENT;
    } else if (err == ENETUNREACH && other && stage != RD_FETCH) {   /* a network error: remembered as a drive that didn't answer */
        remember(c, root, fold, unc);
        const char *r = remembered(c);
        snprintf(why, n, "its drive %s didn't answer: check that it's connected, then check again", r ? r : c);
        rs = READ_ASKING;
    } else if (stage == RD_FETCH)
        snprintf(why, n, "it isn't on this PC, and OneDrive (or another cloud service) couldn't download it: check again once it's there");
    else if (err == EACCES || err == EPERM) snprintf(why, n, "the file can't be read: Windows doesn't let RB Stems Plus open it");
    else if (err == EINVAL) snprintf(why, n, "the file can't be read: Windows can't use its path");
    else snprintf(why, n, "the file can't be read: %s", strerror(err));
out:
    free(c);
    return rs;
}

/* ---------------------------------------------------------------- the decode's reads */

static struct {
    HANDLE thread, watcher, quit;               /* the reading thread, the watcher, its stop */
    DWORD ms;
    volatile LONG cancelled;
    char *path;
    DWORD owner;
    HANDLE stream_file, stream_thread;
    ULONGLONG stream_started;
} rw;
static SRWLOCK rw_lock = SRWLOCK_INIT;

void readable_watch_stream(plat_file f) {
    AcquireSRWLockExclusive(&rw_lock);
    if (rw.owner == GetCurrentThreadId()) rw.stream_file = f.h;
    ReleaseSRWLockExclusive(&rw_lock);
}

int readable_watch_pread(plat_file f, void *buf, size_t n, int64_t off, size_t *got) {
    HANDLE thread = NULL;
    DWORD id = GetCurrentThreadId();
    AcquireSRWLockExclusive(&rw_lock);
    if (rw.stream_file == f.h && rw.owner != id) {
        thread = OpenThread(THREAD_TERMINATE | THREAD_QUERY_INFORMATION, FALSE, id);
        if (thread) {
            rw.stream_thread = thread;
            rw.stream_started = GetTickCount64();
        } else {
            ReleaseSRWLockExclusive(&rw_lock);
            return EIO;
        }
    }
    ReleaseSRWLockExclusive(&rw_lock);
    int err = plat_pread(f, buf, n, off, got);
    if (thread) {
        AcquireSRWLockExclusive(&rw_lock);
        if (rw.stream_thread == thread) rw.stream_thread = NULL;
        ReleaseSRWLockExclusive(&rw_lock);
        CloseHandle(thread);
    }
    return err;
}

static DWORD WINAPI watch(void *arg) {
    (void)arg;
    IO_COUNTERS last = {0}, now;
    GetProcessIoCounters(GetCurrentProcess(), &last);
    ULONGLONG still = GetTickCount64();         /* since when no byte moved */
    while (WaitForSingleObject(rw.quit, 100) == WAIT_TIMEOUT) {
        BOOL pending = FALSE;
        if (!GetProcessIoCounters(GetCurrentProcess(), &now)) continue;
        AcquireSRWLockExclusive(&rw_lock);
        HANDLE thread = rw.stream_thread ? rw.stream_thread : rw.thread;
        if (rw.stream_thread && rw.stream_started > still) still = rw.stream_started;
        if (now.ReadTransferCount != last.ReadTransferCount || now.ReadOperationCount != last.ReadOperationCount
            || !GetThreadIOPendingFlag(thread, &pending) || !pending) {
            last = now;
            still = GetTickCount64();
        } else if (GetTickCount64() - still >= rw.ms) {
            if (CancelSynchronousIo(thread)) InterlockedExchange(&rw.cancelled, 1);   /* only a cancel that found a read */
            still = GetTickCount64();
        }
        ReleaseSRWLockExclusive(&rw_lock);
    }
    return 0;
}

int readable_watch_start(const char *path, double secs) {
    if (rw.watcher) return -1;
    rw.ms = secs > 0 && secs < 4e6 ? (DWORD)(secs * 1000.0 + 0.5) : 60000;
    rw.cancelled = 0;
    rw.path = strdup(path);
    rw.thread = OpenThread(THREAD_TERMINATE | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    rw.quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (rw.path && rw.thread && rw.quit) rw.watcher = CreateThread(NULL, 0, watch, NULL, 0, NULL);
    if (!rw.watcher) {
        if (rw.thread) CloseHandle(rw.thread);
        if (rw.quit) CloseHandle(rw.quit);
        free(rw.path);
        rw.thread = rw.quit = NULL;
        rw.path = NULL;
        return -1;
    }
    AcquireSRWLockExclusive(&rw_lock);
    rw.owner = GetCurrentThreadId();
    ReleaseSRWLockExclusive(&rw_lock);
    return 0;
}

int readable_watch_stop(int failed) {
    if (!rw.watcher) return 0;
    SetEvent(rw.quit);
    WaitForSingleObject(rw.watcher, INFINITE);
    AcquireSRWLockExclusive(&rw_lock);
    rw.owner = 0;
    rw.stream_file = rw.stream_thread = NULL;
    ReleaseSRWLockExclusive(&rw_lock);
    CloseHandle(rw.watcher);
    CloseHandle(rw.thread);
    CloseHandle(rw.quit);
    rw.watcher = rw.thread = rw.quit = NULL;
    int c = InterlockedCompareExchange(&rw.cancelled, 0, 0) != 0 && failed;
    if (c) {                                    /* its drive: not waited for again this run */
        size_t root, fold;
        int unc;
        char *k = canon(rw.path, &root, &fold, &unc);
        if (k) remember(k, root, fold, unc);   /* nothing when it isn't another drive (root 0) */
        free(k);
    }
    free(rw.path);
    rw.path = NULL;
    return c;
}
#endif
