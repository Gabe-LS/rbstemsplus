/*
 * rekordbox stems cache bridge
 *
 * Stands in for rekordbox's libonnxruntime.1.18.0.dylib. Every call goes to the real library
 * (Pioneer's, unmodified, outside the app: see REAL_PATH), except Run() on the Demucs session
 * (the model file named hdemucs.onnx), whose results are cached on disk as 16-bit FLAC. A real
 * library that isn't ONNX Runtime 1.18.x is passed through untouched.
 *
 * Finding a cached answer for the audio chunk rekordbox sends:
 *  1. exact: a SHA-256 of the chunk, the model file's checksum and this format's version names
 *     the file. Works whenever the chunk is bit-identical (rekordbox again, or stems_cache.py
 *     for 44.1 kHz FLAC, which decodes exactly as rekordbox does, but each track's last chunk).
 *  2. similar: each entry keeps a summary of its chunk (<key>.desc). The nearest summaries are
 *     candidates; one is served only if, after aligning it (a time shift searched up to +-4096
 *     samples, accepted up to +-1024: rekordbox's chunks overlap by 2048), the stems explain
 *     the requested chunk as well as they explained their own, on each channel separately and
 *     in every 0.1 s frame (the "residual": what the stems leave unexplained, against the one
 *     stored when the entry was made). A whole-chunk correlation alone let a quiet extra vocal,
 *     a 1 s phrase, swapped channels or side-only content through (review of 2026-10-06).
 *     This covers chunks that are only nearly identical: 48 kHz files resampled by another
 *     resampler, MP3s decoded by another decoder, each track's last chunk.
 *  3. otherwise the model runs, and its result is cached (with its summary).
 *
 * Results whose spectrogram output `x` is all zeros are cached (the replacement model in
 * ../ht_wrap.py, class HTv4XtOnly): the stems are then xt * std(mix) + mean(mix), and a cached
 * answer is rebuilt as x = 0, xt = (stems - mean) / std. Silent or non-finite chunks are never
 * cached, and NaN that the installed model returns for silence (it divides by std(mix)) is
 * replaced by 0 before rekordbox sees it.
 *
 * rekordbox's own models (Pioneer's, listed by checksum in pioneer_models) return real
 * spectrograms. In an account that turned it on (config: rekordbox_model=1), the bridge rebuilds
 * their stems itself, as rekordbox does (Demucs's _ispec, rebuild_spec), and caches those; the
 * model's answer still goes to rekordbox untouched. A cached answer is served the same way
 * (x = 0), which rekordbox plays as the same stems (null test inside rekordbox, 2026-10-08).
 * Rebuilt entries live in their own folder, named from the model's checksum and REBUILD_VERSION
 * (rebuilt_id): a model taken off the list, or a new rebuild version, no longer finds them, and
 * they age out. Anything else passes through.
 *
 * Cache:  ~/Library/Caches/rbstemsplus/<model sha256>/<key[0:2]>/<key>.flac and .desc
 *         8 channels (drums L R, bass L R, other L R, vocals L R), 44.1 kHz, 16-bit, stored at
 *         half level (stems can peak above full scale) and doubled on reading. Or, chosen in the
 *         app's Settings, the same layout in "RB Stems Plus Cache.noindex" on an external drive
 *         (cache_dir): see "where the stems are". The model-hash memos stay in the default folder.
 * Config: ~/Library/Application Support/rbstemsplus/config.ini   enabled=1  max_gb=20  max_days=60
 *         rekordbox_model=0  cache_dir="..."  cache_id=<UUID> (written by the app, read here when
 *         rekordbox loads the library; missing = defaults)
 * State:  ~/Library/Logs/rbstemsplus/bridge-state: this rekordbox's pid and start, and whether
 *         the chosen location is in use, for the app's light
 * Log:    ~/Library/Logs/rbstemsplus/bridge.log (over 5 MB it becomes bridge.log.1)
 * Home:   the account's home from the user database, never $HOME; without one (or with a path too
 *         long to hold the cache's files) there is no cache, and nothing is written elsewhere.
 * Limits: an entry not used for max_days is deleted, and over max_gb the least recently used
 *         go first (a cache hit stamps the file's modification time). The limit counts the disk
 *         space entries take (exFAT's and FAT32's "._" companions too). Checked once when
 *         rekordbox opens the Demucs model, and after writes that take the cache over the limit,
 *         on the maintenance queue. The cleanup touches only names and depths this file writes
 *         (layout()); anything else in the folder is left alone.
 * Kept:   ~/Library/Application Support/rbstemsplus/kept/<name>.kept, written by the prepare
 *         helper: the entries of a prepared playlist, kept until a date (see "kept sets"). Until
 *         then they never age out, and over max_gb every other entry goes before them.
 * Space:  nothing is written to the cache while its disk has less free than its floor: 10% of the
 *         disk's size, at most 50 GB (free_floor); cached stems still load.
 * Memory: our own model (our_models) is opened with ONNX Runtime's memory pattern off; any other
 *         model gets rekordbox's session options untouched.
 * Safety: any failure falls back to running the model; writes go to a temporary file in a
 *         background queue and are renamed when complete.
 */
#if defined(_WIN32)
/* The Windows bridge (bridge.md §1): bridge/win/bridge_win.c includes this file, then its entry
 * points (bridge/win/entry_win.c), into one translation unit; common/plat_win.c stands in for
 * the Mac's frameworks and POSIX calls (the _WIN32 branches below), and the Mac's bytes don't
 * change (make mac-unchanged). */
#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#else
#include <Accelerate/Accelerate.h>
#include <CoreFoundation/CoreFoundation.h>
#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <math.h>
#include <pthread.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#endif

#include "onnxruntime_c_api.h"

/* The platform layer and the modules shared with the helper and the app's core (common/,
 * bridge.md §0a), compiled into this one translation unit: their functions are static here. */
#define COMMON_API static __attribute__((unused))
#include "../common/plat.h"
#if defined(_WIN32)
#include "../common/plat_win.c"
#include "win/ort_win.h"
/* the Mac's POSIX threads, as plat.h's (one name each, so the Mac's lines stay as they are) */
#define pthread_mutex_t plat_mutex
#define PTHREAD_MUTEX_INITIALIZER PLAT_MUTEX_INIT
#define pthread_mutex_lock plat_mutex_lock
#define pthread_mutex_unlock plat_mutex_unlock
#define pthread_once_t plat_once_t
#define PTHREAD_ONCE_INIT PLAT_ONCE_INIT
#define getpid plat_pid
/* the bridge's paths: Windows's long paths (plat_win.c opens them as \\?\), never MAX_PATH */
#undef PATH_MAX
#define PATH_MAX 1024
/* the C library calls the Mac has and the UCRT names otherwise or lacks (bridge.md §1 "Misc") */
static struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 0
#endif
#define clock_gettime_nsec_np(clock) plat_now_mono_ns()
#define usleep(us) plat_sleep_ms((uint32_t)((us) / 1000))   /* the test builds' pauses */
typedef unsigned useconds_t;
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t l = strlen(src);
    if (n) {
        size_t k = l < n - 1 ? l : n - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return l;
}
#else
#include "../common/plat_mac.c"
#endif
#include "../common/path_rules.c"
#include "../common/cfg_ini.c"
#include "../common/marker.c"
#if defined(_WIN32)
/* a chosen location's checks (check_location and gone_why are calls to it here; the Mac's are its own) */
#include "../common/location.c"
#endif
#include "../common/entries.c"
#if defined(_WIN32)
/* the entry points (ordinals 1-5), the loader-safe start, the real library and the refusal stub;
 * they call this file's bridge_core_paths, _log, _start and _hook */
#include "win/entry_win.c"
#ifdef RBSTEMS_TEST_TIMING
#error "the performance timing lines are the Mac's (bridge/tests/perf.sh)"
#endif
#endif

/* a handle that isn't open (plat.h's opens leave one so when they fail) */
#if defined(_WIN32)
#define NO_DIR ((plat_dir){NULL})
#define NO_FILE ((plat_file){NULL})
#else
#define NO_DIR ((plat_dir){-1})
#define NO_FILE ((plat_file){-1})
#endif

#define FORMAT MARKER_FORMAT            /* this format's version: in every key and in a chosen location's marker */
#define EXPORT __attribute__((visibility("default")))

#define DESC_FRAME 4410                 /* summary resolution: 0.1 s */
#define DESC_MAX_FRAMES 512
#define NEAR_MAX 0.15                   /* summary distance (mean |log10 rms| difference) */
#define NEAR_CANDIDATES 3
#define SHIFT_MAX 4096                  /* time shift searched when verifying, in samples */
#define SHIFT_ACCEPT 1024               /* ...and accepted: rekordbox's chunks overlap by 2048 */
#define GAIN_SLACK 0.02
#define M_TOTAL 1.0                     /* dB a near chunk's whole residual may exceed the entry's */
#define M_FRAME 3.0                     /* ...and each frame's */
#define R_FLOOR (-35.0)                 /* a frame residual below this always passes */
#define FRAME_FLOOR 1e-6                /* frames quieter than -60 dBFS (mean square) aren't judged */

/* exported so the bridge can be told from Pioneer's library (the app looks for "rbstems bridge: ") */
EXPORT const char rbstems_marker[] = "rbstems-bridge";

/* rekordbox's own models whose stems this bridge rebuilds and caches (STEMS Engine 0002), and the
 * version of that rebuild. A new rebuild (a fix, a change of maths) takes a new REBUILD_VERSION. */
#define REBUILD_VERSION "1"
#define PIONEER_0002 "435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a"
static const char *const pioneer_models[] = {PIONEER_0002};
/* what this bridge can cache, for the app, which reads it from the file (keep in step with
 * pioneer_models): "rbstems-cache: rebuild=<v> location=1 kept=1 pioneer=<sha>[,<sha>];". location=1: it
 * reads cache_dir and cache_id (a chosen location). kept=1: it keeps prepared playlists' entries
 * (kept sets). New tokens go before pioneer=, whose list runs to ';'. */
__attribute__((used)) static const char rbstems_caps[] = "rbstems-cache: rebuild=" REBUILD_VERSION " location=1 kept=1 pioneer=" PIONEER_0002 ";";
/* dB: rebuilt stems must explain the chunk at least this well (chunks over -60 dBFS, FRAME_FLOOR).
 * Pioneer's model doesn't make its stems add up exactly: on rumble techno (Ignez, SMV015, 171
 * chunks) its worst chunk was -17.9 dB, most below -24; a wrong rebuild gives -6 dB or worse. */
#define REBUILT_RESIDUAL_MAX (-10.0)

#if !defined(_WIN32)
static const OrtApiBase *real_base;
static void *real_handle;               /* the real library, loaded privately */
#endif
static const OrtApi *R;                 /* the real library's functions */
#if !defined(_WIN32)
static OrtApi api;                      /* what rekordbox gets: R with three entries replaced */
static pthread_once_t once = PTHREAD_ONCE_INIT;
#endif
/* The performance timing lines (bridge/tests/perf.sh; bridge/build.sh --test-timing), test builds
 * only: timestamps in ns (clock_gettime_nsec_np, mach_absolute_time's clock) at hook entry, around
 * the real library's call and at return, taken before any log I/O; the "timing ..." line is
 * written afterwards. TIMING(x) marks the few statements that exist only there. */
#ifdef RBSTEMS_TEST_TIMING
#ifndef RBSTEMS_TEST_REAL_PATH
#error "RBSTEMS_TEST_TIMING is for a test build only (RBSTEMS_TEST_REAL_PATH)"
#endif
#define TIMING(x) x
static uint64_t t_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static __thread uint64_t t_real;        /* this hook's time inside the real Run or CreateSession */
static __thread const char *t_kind;     /* what this Run was: pass, hit, near, miss */
static __thread const char *t_memo;     /* the model's checksum: memo, hashed */
static __thread uint64_t t_dl;          /* load_real's dlopen of the real library */
#else
#define TIMING(x)
#endif
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hash_lock = PTHREAD_MUTEX_INITIALIZER;
static plat_queue *writer;              /* store_entry only */
static plat_queue *maint;               /* validate, index loads, prune: never on rekordbox's threads */
#if defined(_WIN32)
#define CACHE_TAIL "\\rbstemsplus\\Cache"
#define SEP "\\"
#else
#define CACHE_TAIL "/Library/Caches/rbstemsplus"
#define SEP "/"
#endif
/* a chosen location's folder (PATHS_CACHE_FOLDER) and its marker (MARKER_NAME): common/path_rules.h,
 * common/marker.h */
#define DEEPEST 160                     /* the longest name under the cache: <model>/<xx>/._<key>.desc.<pid>.tmp */
static char base_dir[PATH_MAX - DEEPEST]; /* where the stems are ("": none), short enough for every file in it */
static char memo_dir[PATH_MAX - DEEPEST]; /* the model-hash memos: always ~/Library/Caches/rbstemsplus */
static char config_path[PATH_MAX];
static char kept_dir[PATH_MAX];         /* the kept sets' folder ("": none) */
static char log_path[PATH_MAX];
static char state_path[PATH_MAX];       /* bridge-state, for the app's light */
static int chosen;                      /* base_dir is a chosen location, on an external drive */
static char cache_id[37];               /* ...whose marker carries this id */
static int enabled = 1;
static int rekordbox_model = 0;         /* this account caches rekordbox's own (listed) models too */
/* Near hits (a similar chunk's stems, verified) are off unless config.ini says near_hits=1: in real
 * use rekordbox decodes a file the same way every time, so its reloads are exact hits (the owner's
 * log: 72 hits, 0 near hits, 77 candidates rejected, all neighbouring chunks of the same track),
 * and the index costs a load at every launch, memory in rekordbox and reads before every miss. The
 * .desc files are still written, so a future pre-cache (inexact decodes) can turn it on with every
 * entry saved meanwhile. A test build takes RBSTEMS_TEST_NEAR_HITS as the default. */
static int near_on = 0;                /* config.ini near_hits */
static long long max_bytes = 20LL << 30;
static double max_days = 60;
static long long total_bytes = -1;      /* -1: not counted yet */
static int space_paused = -1;           /* under lock: -1 not measured yet (space_ok) */
static int writer_checked;              /* under lock: this writer batch checked the marker (store_entry) */
static long long hits, near_hits, misses, writes;
static void lose(const char *why);
#if !defined(_WIN32) && !defined(RBSTEMS_WIN_CACHE_TEST)
static int repin(plat_id was);
#endif

/* ---------------------------------------------------------------- log, config */

#define LOG_MAX (5 << 20)               /* bridge.log over this becomes bridge.log.1 (one kept) */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* snprintf for paths: a path that doesn't fit is "" and -1, never a shorter path (a truncated
 * cache path could name the home folder or ~/Library) */
__attribute__((format(printf, 3, 4)))
static int pathf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    if (r >= 0 && (size_t)r < n) return 0;
    out[0] = 0;
    return -1;
}

static void logf_(const char *fmt, ...) {
    if (!log_path[0]) return;
    pthread_mutex_lock(&log_lock);                          /* one rotation, whole lines */
    plat_stat st;
    if (plat_stat_path(log_path, 1, &st) == 0 && st.size > LOG_MAX) {
        char old[PATH_MAX + 2];
        if (!pathf(old, sizeof old, "%s.1", log_path)) plat_rename_path(log_path, old);
    }
    FILE *f = plat_fopen_w(log_path, 1);                    /* no link followed at the path */
    if (f) {
        time_t t = time(NULL);
        struct tm tmv;
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime_r(&t, &tmv));
        fprintf(f, "%s  ", ts);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    pthread_mutex_unlock(&log_lock);
}

/* config.ini, written by the app: its grammar is common/cfg_ini.h's (the same as the app's reader,
 * app/Sources/Config.swift); bridge/tests/run.sh and common/tests give both the same fixtures */
typedef cfg_config config;

/* reads config.ini into c; -1 when it exists but can't be read (common/cfg_ini.h) */
static int read_config(config *c) {
    if (!config_path[0]) return 0;
    char *b = NULL;
    size_t n = 0;
    int e = plat_read_file(config_path, CFG_MAX, &b, &n);
    if (e) return e == ENOENT ? 0 : -1;
    int ok = cfg_parse(b, n, c, logf_) == 0;
    free(b);
    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------- where the stems are */

/* This Mac (base_dir is the default folder, memo_dir) or a chosen location: PATHS_CACHE_FOLDER on an
 * external drive, whose marker's id is cache_id. A chosen location is checked off rekordbox's
 * threads (validate, the first job on the maintenance queue); until then it is pending and
 * rekordbox's stems pass through. Losing it (lose) is a failure of the location itself, never a
 * file missing below it. While it is unavailable it is checked again every RECHECK_EVERY seconds
 * (recheck: the same check as at launch; with the drive gone only the mount table is read), and
 * used again once it passes: a drive reconnected, repaired or allowed needs no restart. The
 * sessions rekordbox opens at launch are registered either way. The app learns the state from
 * bridge-state. */
enum { LOC_PENDING, LOC_AVAILABLE, LOC_UNAVAILABLE };
static int loc_state = LOC_UNAVAILABLE;     /* under lock; without base_dir never available */
static const char *loc_reason = "";         /* why unavailable: a code the app knows (bridge-state) */
static plat_id pin;                         /* under lock: base_dir as the last check found it, a cheap
                                             * same-folder check (the drive's identity is its marker) */
static char mount_dir[PLAT_MOUNT_MAX];      /* under lock: the chosen location's volume */
#define RECHECK_EVERY 10                    /* seconds between checks of an unavailable chosen location */
static int recheck_armed;                   /* under lock: a recheck is scheduled */

static int loc_now(void) {
    pthread_mutex_lock(&lock);
    int s = loc_state;
    pthread_mutex_unlock(&lock);
    return s;
}

static int test_any_volume(void) {          /* a test build may put a chosen location on any volume */
#ifdef RBSTEMS_TEST_REAL_PATH
    return getenv("RBSTEMS_TEST_ANY_VOLUME") != NULL;
#else
    return 0;
#endif
}

#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
#include "../win/c/volumes.h"
#ifdef RBSTEMS_WIN_CACHE_TEST
static int plat_mkdir_open_at(plat_dir, const char *, int, plat_dir *);
static int plat_dir_open_at_held(plat_dir, const char *, plat_dir *);
static int plat_dir_identity(plat_dir, plat_id *);
#endif
static unsigned long device_generation;                  /* under lock */

static void device_invalidated(void) {
    pthread_mutex_lock(&lock);
    device_generation++;
    int active = loc_state == LOC_AVAILABLE && !plat_id_same(pin, (plat_id){0});
    pin = (plat_id){0};
    pthread_mutex_unlock(&lock);
    if (active) plat_async_b(maint, ^{
        pthread_mutex_lock(&lock);
        int lost = plat_id_same(pin, (plat_id){0});
        pthread_mutex_unlock(&lock);
        if (lost) lose("not-connected");
    });
}

static const char *chosen_volume_why(void) {
    static vol_watch watch = {.invalidated = device_invalidated};
    if (test_any_volume()) return NULL;
    if (plat_win_drive_remote(base_dir)) return "not-local";
    switch (vol_saved_watch(base_dir, &watch)) {
    case VOL_OK: return NULL;
    case VOL_REFUSE_NETWORK: return "not-local";
    case VOL_REFUSE_READ_ONLY: return "read-only";
    default: return "not-connected";
    }
}
#endif

/* Test builds only: pauses and failures at chosen moments, for bridge/tests (a drive detached
 * mid-write, mid-prune, mid-index-load; a write failing with a given errno; an open failing once) */
#ifdef RBSTEMS_TEST_REAL_PATH
static void test_pause(const char *name) {
#ifdef RBSTEMS_WIN_CACHE_TEST
    extern void cache_test_pause(const char *);
    cache_test_pause(name);
#endif
    const char *t = getenv(name);
    if (t) {
        logf_("test: pause %s", name);                      /* bridge/tests can wait for it (RBSTEMS_HARNESS_WAIT_FOR) */
        usleep((useconds_t)(atof(t) * 1e6));
    }
}
static int test_errno(const char *name) {
    const char *t = getenv(name);
    return t ? atoi(t) : 0;
}
static _Thread_local int test_open_errno;   /* the next open_base on this thread fails with it */
#else
#define test_pause(name) ((void)0)
#define test_errno(name) 0
#endif

static const char *reason_text(const char *r) {
    return !strcmp(r, "config") ? "config.ini can't be read, or sets it wrongly"
         : !strcmp(r, "disabled") ? "Stems Cache is off (enabled=0), or isn't hooked into this rekordbox"
         : !strcmp(r, "not-connected") ? "its drive isn't connected"
         : !strcmp(r, "folder") ? "its folder is missing, or something on the way to it isn't a real folder"
         : !strcmp(r, "marker") ? "its folder's marker is missing or another folder's"
         : !strcmp(r, "read-only") ? "the drive is read-only"
         : !strcmp(r, "not-local") ? "the drive isn't connected to this Mac"
         : !strcmp(r, "no-permission") ? "no permission (macOS's privacy settings, or another account's folder)"
         : !strcmp(r, "write-failed") ? "writing failed"
         : "the drive doesn't answer (input/output error)";
}

/* when this process started (seconds since 1970): with the pid, the app tells this rekordbox from
 * a later process that got the same pid */
#if defined(_WIN32)
#define START_FMT "%lld"
static long long process_start(void) { return plat_process_start(); }   /* GetProcessTimes; long is 32-bit here */
#else
#define START_FMT "%ld"
static long process_start(void) {
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && len > 0 ? (long)kp.kp_proc.p_starttime.tv_sec : 0;
}
#endif

/* bridge-state, next to the log: what this rekordbox does with the location. The app's light
 * follows it while this process runs (app/Sources/Location.swift reads it: its keys are a contract). */
static void write_state(void) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static const char *names[] = {"pending", "available", "unavailable"};
    char tmp[PATH_MAX + 32];
    if (!state_path[0] || pathf(tmp, sizeof tmp, "%s.%d.tmp", state_path, getpid())) return;
    pthread_mutex_lock(&m);
    pthread_mutex_lock(&lock);
    int s = loc_state;
    const char *why = s == LOC_AVAILABLE ? "" : loc_reason;
    pthread_mutex_unlock(&lock);
    FILE *f = plat_fopen_w(tmp, 0);
    if (f) {
        int ok = fprintf(f, "pid=%d\nstart=" START_FMT "\nlocation=%s\nstate=%s\nreason=%s\n", getpid(), process_start(),
                         chosen ? base_dir : "", names[s], why) > 0;
        ok = fclose(f) == 0 && ok;
        if (!ok || plat_rename_path(tmp, state_path) != 0) plat_unlink_path(tmp);
    }
    pthread_mutex_unlock(&m);
}

#if !defined(_WIN32)
static const char *why_of(int e) {
    return e == ENOENT || e == ENOTDIR || e == ELOOP || e == EXDEV ? "folder"
         : e == EACCES || e == EPERM ? "no-permission"
         : e == EROFS ? "read-only" : "io-error";
}
#endif

/* why a chosen location's folder failed: its drive gone, or the folder itself */
static const char *gone_why(int e) {
    char mnt[PLAT_MOUNT_MAX];
    pthread_mutex_lock(&lock);
    strlcpy(mnt, mount_dir, sizeof mnt);
    pthread_mutex_unlock(&lock);
#if defined(_WIN32)
    return location_gone(mnt, test_any_volume(), e);       /* common/location.c */
#else
    if (!test_any_volume() && !plat_volume_mounted(mnt)) return "not-connected";
    return why_of(e);
#endif
}

/* Files are read through plat.h (common/plat_mac.c): plat_file_open_at opens one without blocking
 * on a FIFO or following a link, and only if it is a regular file of at most max bytes (else
 * PLAT_EFTYPE); plat_read_all reads exactly n bytes; plat_dir_name_is tells whether a folder is
 * stored under exactly its name (on a volume that ignores case, "ab" also opens "AB", which isn't
 * ours). */

/* <model> or <xx> in dir, made first if `create`: a real folder (no link), on the same volume and
 * named exactly name; not open with *err otherwise (EXDEV: another volume, or another name's case) */
static plat_dir sub_open(plat_dir dir, const char *name, int create, uint64_t vol, int *err) {
    static int said;
    plat_dir d;
    plat_stat st;
    int e;
    if (create && (e = plat_mkdir_at(dir, name)) && e != EEXIST) { *err = e; return NO_DIR; }
    if ((e = plat_dir_open_at(dir, name, &d))) { *err = e; return d; }
    if (plat_dir_stat(d, &st) || st.id.vol != vol || !plat_dir_name_is(d, name)) {
        if (!said) { said = 1; logf_("cache: a folder that matches %s only by ignoring case (or on another volume) is left alone", name); }
        *err = EXDEV;
        plat_dir_close(d);
        return NO_DIR;
    }
    return d;
}

/* base_dir, opened for one operation and closed at its end (short descriptors don't stop an
 * eject). A chosen location must be the folder the last check pinned: failing that is a loss. This
 * Mac's folder is made again when `create` (Clear deletes it). Not open with *err otherwise. */
static plat_dir open_base(int create, int *err) {
    plat_dir fd = NO_DIR;
    if (!base_dir[0]) { *err = ENOENT; return fd; }
#if !defined(_WIN32)
    if (!chosen && create) plat_mkdirs(base_dir);
#else
    (void)create;
#endif
    *err = plat_dir_open(base_dir, &fd);
#ifdef RBSTEMS_TEST_REAL_PATH
    if (test_open_errno && plat_dir_ok(fd)) { plat_dir_close(fd); fd = NO_DIR; *err = test_open_errno; }
    test_open_errno = 0;
#endif
    if (!chosen) return fd;
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    plat_id opened;
    int stat_error = plat_dir_ok(fd) ? plat_dir_identity(fd, &opened) : *err;
#else
    plat_stat st;
#endif
    pthread_mutex_lock(&lock);
    plat_id id = pin;
    pthread_mutex_unlock(&lock);
    if (!plat_dir_ok(fd)) {
        /* a failure of the location itself; a shortage of descriptors or memory in rekordbox is
         * only this lookup's (a miss), never a loss for the run */
        int e = *err;
        if (e == ENOENT || e == ENOTDIR || e == ELOOP || e == ENXIO || e == ENODEV || e == EIO || e == EACCES || e == EPERM || e == EROFS)
            lose(gone_why(e));
    }
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    else if (stat_error || !plat_id_same(opened, id)) {
        /* Leave discovery to the maintenance queue, including when the device notification
         * invalidated the pin without a change in the on-disk identity. */
        plat_dir_close(fd);
        fd = NO_DIR;
        *err = EXDEV;
        lose(plat_id_same(id, (plat_id){0}) ? "not-connected" : "folder");
#else
    else if (plat_dir_stat(fd, &st) || !plat_id_same(st.id, id)) {
        /* a drive connected again has new device numbers: checked at once (repin), and used
         * when the folder opened is the one that check pinned */
        int ok = 0;
        if (repin(id)) {
            pthread_mutex_lock(&lock);
            id = pin;
            pthread_mutex_unlock(&lock);
            ok = plat_dir_stat(fd, &st) == 0 && plat_id_same(st.id, id);
        }
        if (!ok) {
            plat_dir_close(fd);
            fd = NO_DIR;
            *err = EXDEV;
            lose("folder");
        }
#endif
    }
    return fd;
}

/* the prefix folder of an entry, base_dir/<model>/<key[0:2]>, made first if `create` */
static plat_dir entry_dir(const char *model, const char *key, int create, int *err) {
    plat_stat st;
    char xx[3] = {key[0], key[1], 0};
    plat_dir b = open_base(create, err), m = NO_DIR, x = NO_DIR;
    int e;
    if (!plat_dir_ok(b)) return x;
    if (!(e = plat_dir_stat(b, &st)) && plat_dir_ok(m = sub_open(b, model, create, st.id.vol, err))) x = sub_open(m, xx, create, st.id.vol, err);
    else if (e) *err = e;
    plat_dir_close(m);
    plat_dir_close(b);
    return x;
}

/* NULL when base_dir (open as fd) holds our marker, else why not; "" when it couldn't be told
 * for want of descriptors or memory (not the location's fault: no verdict). common/marker.c */
static const char *marker_check(plat_dir d) { return marker_check_at(d, cache_id); }

#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
#include "../common/cache_owner.h"

/* Only an exclusive directory creation can establish local cache ownership. */
static int local_cache_identity(plat_dir d, plat_id id, char out[CACHE_OWNER_IDENTITY_MAX]) {
    if (vol_dir_guid(d, out)) return -1;
    cache_owner_identity(out, VOL_GUID_LEN, id);
    return 0;
}

static int local_creation_identity(plat_dir d, const char *path, int created, char *identity) {
    (void)path;
    (void)created;
    plat_id id;
    int err = plat_dir_identity(d, &id);
    if (!err && local_cache_identity(d, id, identity)) err = EIO;
    return err;
}

static int make_local_cache(void) {
    char parent[sizeof memo_dir];
    memcpy(parent, memo_dir, sizeof parent);
    char *name = strrchr(parent, SEP[0]);
    if (!name) return ENOENT;
    *name++ = 0;
#if defined(_WIN32)
    plat_mkdirs(parent);
#endif
    plat_dir p;
    int err = plat_dir_open(parent, &p);
    if (!err) {
        err = cache_owner_init(p, name, memo_dir, local_creation_identity, plat_mkdir_open_at);
        plat_dir_close(p);
    }
    return err;
}

static void local_cache_status(int err) {
    const char *why = err ? cache_owner_reason(err) : "";
    int state = err ? LOC_UNAVAILABLE : LOC_AVAILABLE;
    pthread_mutex_lock(&lock);
    int changed = loc_state != state || strcmp(loc_reason, why);
    if (changed) space_paused = -1;
    loc_state = state;
    loc_reason = why;
    pthread_mutex_unlock(&lock);
    if (changed) {
        if (err) logf_("cache: This PC is unavailable (%s, error %d)", why, err);
        write_state();
    }
}

static plat_dir open_local_cache(int *err) {
    plat_dir d = NO_DIR;
    if (!memo_dir[0]) { *err = ENOENT; return d; }
    if ((*err = plat_dir_open(memo_dir, &d)) == ENOENT) {
        *err = make_local_cache();
        if (!*err) *err = plat_dir_open(memo_dir, &d);
    }
    return d;
}

static int local_marker_matches(plat_dir d, plat_id id) {
    char identity[CACHE_OWNER_IDENTITY_MAX];
    return !local_cache_identity(d, id, identity) && !cache_owner_check(d, identity);
}

static int mutation_allowed(plat_dir b, plat_id id) {
    if (!chosen) return local_marker_matches(b, id);
    pthread_mutex_lock(&lock);
    int pinned = loc_state == LOC_AVAILABLE && plat_id_same(pin, id);
    pthread_mutex_unlock(&lock);
    if (!pinned || (!test_any_volume() && !vol_dir_on_volume(b, base_dir))) return 0;
    const char *why = marker_check(b);
    if (why && *why) lose(why);
    return !why;
}

/* Keep intermediate folders from moving out of the checked cache during the mutation. */
static plat_dir mutation_sub_open(plat_dir dir, const char *name, int create, uint64_t vol, int *err) {
    plat_dir d;
    plat_stat st;
    if (create && (*err = plat_mkdir_at(dir, name)) && *err != EEXIST) return NO_DIR;
    if ((*err = plat_dir_open_at_held(dir, name, &d))) return NO_DIR;
    if (plat_dir_stat(d, &st) || st.id.vol != vol || !plat_dir_name_is(d, name)) {
        *err = EXDEV;
        plat_dir_close(d);
        return NO_DIR;
    }
    return d;
}

static plat_dir mutation_entry_dir(plat_dir b, uint64_t vol, const char *model, const char *key,
                                   int create, plat_dir *m, int *err) {
    char xx[3] = {key[0], key[1], 0};
    *m = mutation_sub_open(b, model, create, vol, err);
    return plat_dir_ok(*m) ? mutation_sub_open(*m, xx, create, vol, err) : NO_DIR;
}
#endif

/* A chosen location is still the folder validate pinned, with our marker (re-read at each prune,
 * each index load and the first write of each writer batch): else it is lost. 0 when it is. */
static int location_recheck(void) {
    if (!chosen) return 0;
    int err;
    plat_dir fd = open_base(0, &err);
    if (!plat_dir_ok(fd)) return -1;
    const char *why = marker_check(fd);
    plat_dir_close(fd);
    if (why && *why) { lose(why); return -1; }
    return why ? -1 : 0;                                    /* "": not now, but not lost */
}

/* Whether the chosen location can be used (NULL, with base_dir's identity in *id, its volume in
 * mnt), else why not. Its volume: the longest mount point holding base_dir's parent folder, so
 * never base_dir itself (plat_volume_holding: the table of mounts, no lookup that could follow a
 * link), local and not this Mac's own (a mount point left behind in /Volumes is on it: not
 * connected). From there down, every component a real folder on that volume (no link followed);
 * the folder holds our marker; the volume and the folder are writable. Nothing is ever created
 * here: the app makes the folder and the marker. */
#if defined(_WIN32)
/* Windows (bridge.md §1, check_location's row): common/location.c's location_check, the logic
 * common/tests/location.c, location_fake.c and location_win.c test (a drive path "X:\\..." or the volume-GUID path
 * cache_dir stores; not the volume holding %SystemRoot%; a network drive "not-local"; the folder
 * opened still on the volume walked) */
static const char *check_location(plat_id *id, char mnt[PLAT_MOUNT_MAX]) {
    location_pin lp = {{0, {0, 0}}, ""};
    const char *why = chosen_volume_why();
    if (!why) why = location_check(base_dir, cache_id, test_any_volume(), &lp);
    *id = lp.id;
    strlcpy(mnt, lp.mount, PLAT_MOUNT_MAX);
    return why;
}
#else
static const char *check_location(plat_id *id, char mnt[PLAT_MOUNT_MAX]) {
    int any = test_any_volume(), system = 0, e;
    char p[sizeof base_dir];
    strlcpy(p, base_dir, sizeof p);
    char *last = strrchr(p, '/');
    if (!last) return "not-connected";                      /* not absolute: on no volume */
    if (last == p) last[1] = 0;                             /* the parent is "/" */
    else *last = 0;
    if ((e = plat_volume_holding(p, mnt, PLAT_MOUNT_MAX, &system))) return e == ENOENT ? "not-connected" : "io-error";
    if (system && !any) return "not-connected";
    plat_stat st;
    plat_volume v;
    if ((e = plat_stat_path(mnt, 0, &st))) return e == ENOENT || e == ENOTDIR ? "not-connected" : why_of(e);
    uint64_t vol = st.id.vol;
    strlcpy(p, base_dir, sizeof p);
    for (char *s = p + (strcmp(mnt, "/") ? strlen(mnt) : 0) + 1;;) {
        char *c = strchr(s, '/');
        if (c) *c = 0;
        if ((e = plat_stat_path(p, 0, &st))) return e == ENOENT ? "folder" : why_of(e);
        if (st.kind != PLAT_DIR || (!any && st.id.vol != vol)) return "folder";
        if (!c) break;
        *c = '/';
        s = c + 1;
    }
    /* the volume as the folder sees it (no link on the way now): the same mount, local, writable */
    if ((e = plat_volume_info(base_dir, &v))) return why_of(e);
    if (!any && strcmp(v.mount, mnt)) return "folder";
    if (!v.local) return "not-local";
    if (v.readonly) return "read-only";
    plat_dir fd;
    test_pause("RBSTEMS_TEST_OPEN_DELAY");                 /* bridge/tests: an ancestor swapped after the walk */
    if ((e = plat_dir_open(base_dir, &fd))) return why_of(e);
    /* the folder opened is still on that volume: an ancestor swapped for a link after the walk
     * can't move it to another one (common/location.c's review TC1; BW3) */
    const char *why = plat_dir_stat(fd, &st) ? "io-error" : !any && st.id.vol != vol ? "folder" : marker_check(fd);
    if (why && !*why) why = "io-error";                     /* checked once: no descriptors is a failure here */
    *id = st.id;
    if (!why && (e = plat_dir_writable(fd))) why = e == EROFS ? "read-only" : "no-permission";
    plat_dir_close(fd);
    return why;
}
#endif

static void log_using(const char *how) {
    plat_volume v;
    if (plat_volume_info(base_dir, &v) == 0)
        logf_("cache: %s %s (%s, %s, %.1f GB free of %.1f GB)", how, base_dir, v.fstype, v.local ? "local" : "not local",
              (double)v.avail / 1e9, (double)v.total / 1e9);
    else logf_("cache: %s %s", how, base_dir);
}

static unsigned recheck_every(void) {
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_RECHECK_SECONDS");
    if (t) return (unsigned)atoi(t);
#endif
    return RECHECK_EVERY;
}

/* Schedule a recheck while the configured cache is unavailable and Stems Cache is on. */
static void recheck(void);
static void arm_recheck(void) {
    pthread_mutex_lock(&lock);
#if defined(_WIN32)
    int go = base_dir[0] && enabled && !recheck_armed && loc_state == LOC_UNAVAILABLE && strcmp(loc_reason, "disabled");
#else
    int go = chosen && !recheck_armed && loc_state == LOC_UNAVAILABLE && strcmp(loc_reason, "disabled");
#endif
    if (go) recheck_armed = 1;
    pthread_mutex_unlock(&lock);
    if (go && plat_after_b(maint, (uint64_t)recheck_every() * 1000000000u, ^{ recheck(); })) {
        pthread_mutex_lock(&lock);                          /* not queued (never on the Mac) */
        recheck_armed = 0;
        pthread_mutex_unlock(&lock);
    }
}

static void validate(void) {
    test_pause("RBSTEMS_TEST_VALIDATE_DELAY");
    plat_id id = {0, {0, 0}};
    char mnt[PLAT_MOUNT_MAX] = "";
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    pthread_mutex_lock(&lock);
    unsigned long generation = device_generation;
    pthread_mutex_unlock(&lock);
#endif
    const char *why = check_location(&id, mnt);
    pthread_mutex_lock(&lock);
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    if (generation != device_generation) why = "not-connected";
    if (why) id = (plat_id){0};
#endif
    loc_state = why ? LOC_UNAVAILABLE : LOC_AVAILABLE;
    loc_reason = why ? why : "";
    pin = id;
    strlcpy(mount_dir, mnt, sizeof mount_dir);
    pthread_mutex_unlock(&lock);
    write_state();
    if (why) logf_("cache: %s not available (%s): stems aren't saved or loaded until it is; checked again every %u seconds", base_dir, reason_text(why), recheck_every());
    else log_using("using");
    arm_recheck();
}
#ifdef RBSTEMS_TEST_TIMING
static void validate_timed(void) {
    uint64_t t0 = t_now();
    validate();
    uint64_t t1 = t_now();
    logf_("timing validate total_ns=%llu", (unsigned long long)(t1 - t0));
}
#define validate() validate_timed()
#endif

static void first_prune(int tries);

/* The chosen location, unavailable, checked again as at launch (maintenance queue). Once it
 * passes it is used again: the space it takes is counted afresh (first_prune), and with near hits
 * on the index is loaded again by the next miss. Logged when it's back, or when the reason changes. */
static void recheck(void) {
    pthread_mutex_lock(&lock);
    recheck_armed = 0;
    int still = loc_state == LOC_UNAVAILABLE;
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    unsigned long generation = device_generation;
#endif
    pthread_mutex_unlock(&lock);
    if (!still) return;
#if defined(_WIN32)
    if (!chosen) {
        int err = make_local_cache();
        local_cache_status(err);
        if (!err) first_prune(0);
        else arm_recheck();
        return;
    }
#endif
    plat_id id = {0, {0, 0}};
    char mnt[PLAT_MOUNT_MAX] = "";
    const char *why = check_location(&id, mnt);
    int back = 0, changed = 0;
    pthread_mutex_lock(&lock);
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    if (generation != device_generation) why = "not-connected";
#endif
    if (loc_state == LOC_UNAVAILABLE) {
        if (!why) {
            loc_state = LOC_AVAILABLE;
            loc_reason = "";
            pin = id;
            strlcpy(mount_dir, mnt, sizeof mount_dir);
            total_bytes = -1;
            space_paused = -1;
            writer_checked = 0;                             /* the next write checks the marker again */
            back = 1;
        } else if (strcmp(why, loc_reason)) {
            loc_reason = why;
            changed = 1;
        }
    }
    pthread_mutex_unlock(&lock);
    if (back) {
        write_state();
        log_using("back: using");
        first_prune(0);
        return;
    }
    if (changed) {
        write_state();
        logf_("cache: %s is still unavailable, now because %s", base_dir, reason_text(why));
    }
    arm_recheck();
}

/* ---------------------------------------------------------------- loading the real library */
#if !defined(_WIN32)

/* Pioneer's library, unmodified, outside the app (root-owned): the bundle then has no added
 * file, so a rekordbox update leaves Pioneer's app whole. Only this path, never the
 * environment: rekordbox may load unsigned code, so a path from the environment would let any
 * process put code into it. RBSTEMS_TEST_REAL_PATH exists for bridge/tests (build.sh --test)
 * only; a release build refuses it. */
#ifdef RBSTEMS_TEST_REAL_PATH
#define REAL_PATH RBSTEMS_TEST_REAL_PATH
#define BUILD_NOTE ", TEST BUILD"
#else
#define REAL_PATH "/Library/Application Support/rbstemsplus/ort/libonnxruntime.1.18.0.dylib"
#define BUILD_NOTE ""
#endif

static int hooks_ok; /* the real library is the ONNX Runtime 1.18 whose OrtApi layout we patch */

/* Whether only root can change the library: the file (after symlinks) and its two parent folders
 * are owned by root and not writable by group or others. Otherwise a process running as the
 * user could swap it, and rekordbox (which may load unsigned code) would run it. A test build
 * also accepts the user running the tests as the owner. */
static int owner_ok(const struct stat *st) {
#ifdef RBSTEMS_TEST_REAL_PATH
    if (st->st_uid == getuid()) return !(st->st_mode & (S_IWGRP | S_IWOTH));
#endif
    return st->st_uid == 0 && !(st->st_mode & (S_IWGRP | S_IWOTH));
}

static int only_root_can_change(const char *path) {
    char real[PATH_MAX], dir[PATH_MAX];
    struct stat st;
    if (!realpath(path, real)) return 1; /* missing: dlopen says so */
    if (stat(real, &st) != 0 || !owner_ok(&st)) {
        logf_("ERROR real library: %s is not owned by root, or others can change it: not loaded", real);
        return 0;
    }
    if (pathf(dir, sizeof dir, "%s", real)) return 0;
    for (int level = 0; level < 2; level++) {
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = 0;
        if (stat(dir, &st) != 0 || !owner_ok(&st)) {
            logf_("ERROR real library: its folder %s is not owned by root, or others can change it: not loaded", dir);
            return 0;
        }
    }
    return 1;
}

static void *open_real(const char *path) {
    if (!only_root_can_change(path)) return NULL;
    TIMING(uint64_t t0 = t_now();)
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    TIMING(t_dl = t_now() - t0;)
    if (!h) { logf_("real library: cannot load %s: %s", path, dlerror()); return NULL; }
    if (dlsym(h, "rbstems_marker")) {  /* dyld handed back this bridge (same install name) */
        logf_("ERROR real library: %s resolved to the bridge itself", path);
        return NULL;
    }
    if (!dlsym(h, "OrtGetApiBase")) { logf_("ERROR real library: %s has no OrtGetApiBase", path); return NULL; }
    return h;
}
#endif

/* The account's home, from the user database: $HOME is whatever started rekordbox says, and the
 * cleanup deletes files under it. A test build takes RBSTEMS_TEST_HOME instead when it is set
 * ("" = no home), so the tests never touch the real one. */
#if defined(_WIN32)
/* LocalAppData from the known folder (plat_home: the registry, never the environment), outside the
 * loader lock (phase 2); a test build takes RBSTEMS_TEST_HOME (an absolute X:\ path, "" = none),
 * read from Windows' own environment as UTF-16 (the CRT's getenv is the ANSI code page: a CJK
 * profile's home would not be UTF-8) */
static int home_dir(char *out, size_t n) {
#ifdef RBSTEMS_TEST_REAL_PATH
    wchar_t w[PATH_MAX];
    SetLastError(0);
    DWORD k = GetEnvironmentVariableW(L"RBSTEMS_TEST_HOME", w, PATH_MAX);
    if (k || GetLastError() != ERROR_ENVVAR_NOT_FOUND) {   /* set: "" (k 0) is no home */
        if (!k || k >= PATH_MAX || wr_utf16_to_8((const uint16_t *)w, k, out, n) < 0) return -1;
        return out[0] && out[1] == ':' && out[2] == '\\' ? 0 : -1;
    }
#endif
    return plat_home(out, n);
}
#else
static int home_dir(char *out, size_t n) {
    const char *home = NULL;
    struct passwd pw, *res = NULL;
    char buf[16384];
#ifdef RBSTEMS_TEST_REAL_PATH
    if ((home = getenv("RBSTEMS_TEST_HOME"))) return home[0] == '/' ? pathf(out, n, "%s", home) : -1;
#endif
    if (getpwuid_r(getuid(), &pw, buf, sizeof buf, &res) == 0 && res) home = res->pw_dir;
    return home && home[0] == '/' ? pathf(out, n, "%s", home) : -1;
}
#endif

/* the config, log and default cache paths under the user's home; a path that doesn't fit is ""
 * (off). Nothing is created here but the log's folder. */
#if defined(_WIN32)
static void set_paths(void) {
    char home[PATH_MAX], dir[PATH_MAX];
    if (home_dir(home, sizeof home)) return;                /* no home: no log, no config, no cache */
    if (!pathf(dir, sizeof dir, "%s\\rbstemsplus\\Logs", home) && !pathf(log_path, sizeof log_path, "%s\\bridge.log", dir)) {
        plat_mkdirs(dir);
        pathf(state_path, sizeof state_path, "%s\\bridge-state", dir);
    }
    pathf(config_path, sizeof config_path, "%s\\rbstemsplus\\config.ini", home);
    pathf(kept_dir, sizeof kept_dir, "%s\\rbstemsplus\\kept", home);
    size_t len, tail = strlen(CACHE_TAIL);
    if (pathf(memo_dir, sizeof memo_dir, "%s" CACHE_TAIL, home) || (len = strlen(memo_dir)) <= tail
        || strcmp(memo_dir + len - tail, CACHE_TAIL)) {
        memo_dir[0] = 0;
        logf_("ERROR the LocalAppData folder's path is too long for the cache (%zu characters): nothing is cached", strlen(home));
    }
}
#else
static void set_paths(void) {
    char home[PATH_MAX], dir[PATH_MAX];
    if (home_dir(home, sizeof home)) return;                /* no home: no log, no config, no cache */
    if (!pathf(dir, sizeof dir, "%s/Library/Logs/rbstemsplus", home) && !pathf(log_path, sizeof log_path, "%s/bridge.log", dir)) {
        plat_mkdirs(dir);
        pathf(state_path, sizeof state_path, "%s/bridge-state", dir);
    }
    pathf(config_path, sizeof config_path, "%s/Library/Application Support/rbstemsplus/config.ini", home);
    pathf(kept_dir, sizeof kept_dir, "%s/Library/Application Support/rbstemsplus/kept", home);
    size_t len, tail = strlen(CACHE_TAIL);
    if (pathf(memo_dir, sizeof memo_dir, "%s" CACHE_TAIL, home) || (len = strlen(memo_dir)) <= tail
        || strcmp(memo_dir + len - tail, CACHE_TAIL)) {
        memo_dir[0] = 0;
        logf_("ERROR the home folder's path is too long for the cache (%zu characters): nothing is cached", strlen(home));
    }
}
#endif

/* Where the stems go, from config.ini: this Mac's folder, a chosen location (pending until
 * validate has checked it), or nowhere (a config.ini that can't be read or sets the location
 * wrongly: never this Mac instead). Then the folders: the default one, which keeps the model-hash
 * memos in either case; a chosen location is never created by the bridge. */
static void set_location(void) {
    config c = {.enabled = 1, .rekordbox_model = 0, .near_hits = 0, .max_bytes = 20LL << 30, .max_days = 60};
#ifdef RBSTEMS_TEST_REAL_PATH
    if (getenv("RBSTEMS_TEST_NEAR_HITS")) c.near_hits = atoi(getenv("RBSTEMS_TEST_NEAR_HITS")) != 0;
#endif
    int unreadable = read_config(&c);
    enabled = c.enabled;
    rekordbox_model = c.rekordbox_model;
    near_on = c.near_hits;
    max_bytes = c.max_bytes;
    max_days = c.max_days;
    if (!memo_dir[0]) return;                               /* no home for the cache: nothing cached */
    if (unreadable || c.location < 0) {
        loc_reason = "config";
        logf_("ERROR config: %s: nothing is cached until it's fixed (RB Stems Plus › Settings)",
              unreadable ? "config.ini can't be read" : "where stems are saved (cache_dir, cache_id) is set wrongly");
    } else if (c.location) {
        chosen = 1;
        memcpy(base_dir, c.dir, strlen(c.dir) + 1);
        memcpy(cache_id, c.id, 37);
        loc_state = LOC_PENDING;
    } else {
        memcpy(base_dir, memo_dir, sizeof base_dir);
#if !defined(_WIN32) && !defined(RBSTEMS_WIN_CACHE_TEST)
        loc_state = LOC_AVAILABLE;
#endif
    }
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    int err = make_local_cache();
    if (!chosen && base_dir[0]) local_cache_status(err);
    else if (err) logf_("cache: This PC memo folder is unavailable (%s, error %d)", cache_owner_reason(err), err);
#else
    plat_mkdirs(memo_dir);
#endif
}

#if defined(_WIN32)
/* entry_win.c's phase 2, outside the loader lock: the paths, the buffered lines, then the rest of
 * the Mac's load_real (the real library is phase 1's, already loaded and checked) */
static void bridge_core_paths(void) { set_paths(); }
static void bridge_core_log(const char *line) { logf_("%s", line); }
static void bridge_core_start(void) {
    const char *ver = ew_real_base ? ew_real_base->GetVersionString() : "";
    set_location();
    writer = plat_queue_new("rbstems.writer", PLAT_WORK_DEFAULT);
    maint = plat_queue_new("rbstems.maintenance", PLAT_WORK_MAINT);   /* below normal, never background mode */
    int no_queue = !writer || !maint;                       /* BW5: then nothing is ever submitted */
    if (no_queue) {
        pthread_mutex_lock(&lock);
        enabled = 0;
        loc_state = LOC_UNAVAILABLE;
        loc_reason = "io-error";
        pthread_mutex_unlock(&lock);
        logf_("ERROR cannot create the bridge's queues: caching off");
    }
    logf_("bridge loaded (real library %s from %s%s; cache %s%s, enabled=%d, max_gb=%.1f, max_days=%g, rekordbox_model=%d, rebuild %s%s)",
          ew_real_base ? ver : "REFUSED", ew_real_path[0] ? ew_real_path : "?",
          ew_hooks_ok ? "" : ew_real_base ? ", NOT 1.18.x: passing everything through" : "", base_dir[0] ? base_dir : "NONE",
          !chosen ? "" : enabled && ew_hooks_ok ? " (chosen in Settings: checking it)" : " (chosen in Settings: not used, Stems Cache is off here)", enabled,
          max_bytes / (double)(1LL << 30), max_days, rekordbox_model, REBUILD_VERSION, EW_BUILD_NOTE);
    if (ew_refused && ew_refused[0]) logf_("rekordbox gets the stub OrtApi (%s): its stems fail cleanly", ew_refused);
    if (chosen) logf_("cache: the model's checksums are remembered in %s", memo_dir);
    if (!near_on) logf_("cache: near hits off (near_hits=0): only exact hits are served");
    write_state();
    if (no_queue) return;
    if (!chosen && ew_hooks_ok) arm_recheck();
    if (chosen && enabled && ew_hooks_ok) {
        if (plat_async_b(maint, ^{ validate(); })) {        /* BW6: not queued, so never pending */
            pthread_mutex_lock(&lock);
            loc_state = LOC_UNAVAILABLE;
            loc_reason = "io-error";
            pthread_mutex_unlock(&lock);
            write_state();
            logf_("cache: %s not checked (the check couldn't be queued): stems aren't saved or loaded; checked again every %u seconds",
                  base_dir, recheck_every());
            arm_recheck();
        }
    } else if (chosen) {
        pthread_mutex_lock(&lock);
        loc_state = LOC_UNAVAILABLE;
        loc_reason = "disabled";
        pthread_mutex_unlock(&lock);
        write_state();
    }
}
#else
static void load_real(void) {
    set_paths();
    void *h = open_real(REAL_PATH);
    if (!h) { logf_("ERROR no real ONNX Runtime library: rekordbox's analysis will fail; reinstall or uninstall"); return; }
    real_handle = h;
    Dl_info ri;
    const OrtApiBase *(*get)(void) = (const OrtApiBase *(*)(void))dlsym(h, "OrtGetApiBase");
    real_base = get();
    const char *ver = real_base ? real_base->GetVersionString() : "";
    hooks_ok = real_base && strncmp(ver, "1.18.", 5) == 0;
    set_location();
    writer = plat_queue_new("rbstems.writer", PLAT_WORK_DEFAULT);
    maint = plat_queue_new("rbstems.maintenance", PLAT_WORK_MAINT);   /* the Mac: QOS_CLASS_UTILITY */
    logf_("bridge loaded (real library %s from %s%s; cache %s%s, enabled=%d, max_gb=%.1f, max_days=%g, rekordbox_model=%d, rebuild %s%s)",
          real_base ? ver : "MISSING", dladdr((void *)get, &ri) ? ri.dli_fname : "?",
          hooks_ok ? "" : ", NOT 1.18.x: passing everything through", base_dir[0] ? base_dir : "NONE",
          !chosen ? "" : enabled && hooks_ok ? " (chosen in Settings: checking it)" : " (chosen in Settings: not used, Stems Cache is off here)", enabled,
          max_bytes / (double)(1LL << 30), max_days, rekordbox_model, REBUILD_VERSION, BUILD_NOTE);
    if (chosen) logf_("cache: the model's checksums are remembered in %s", memo_dir);
    if (!near_on) logf_("cache: near hits off (near_hits=0): only exact hits are served");
    write_state();
    /* a drive is touched only when Stems Cache will use it: off (enabled=0) or passing everything
     * through (not ORT 1.18), the chosen location is never checked, never woken */
    if (chosen && enabled && hooks_ok) plat_async_b(maint, ^{ validate(); });
    else if (chosen) {
        pthread_mutex_lock(&lock);
        loc_state = LOC_UNAVAILABLE;
        loc_reason = "disabled";
        pthread_mutex_unlock(&lock);
        write_state();
    }
}
#endif
#ifdef RBSTEMS_TEST_TIMING
/* the bridge's start (OrtGetApiBase's first call): own_ns leaves out the real library's dlopen,
 * which rekordbox would pay without the bridge too */
static void load_real_timed(void) {
    uint64_t t0 = t_now();
    load_real();
    uint64_t t1 = t_now();
    logf_("timing start total_ns=%llu dlopen_ns=%llu own_ns=%llu", (unsigned long long)(t1 - t0), (unsigned long long)t_dl,
          (unsigned long long)(t1 - t0 - t_dl));
}
#define load_real load_real_timed
#endif

/* ---------------------------------------------------------------- Demucs sessions */

#define MAX_SESSIONS 16
/* model: what names the session's cache folder and goes into its keys (the model's checksum, or
 * for a rebuilt model rebuilt_id); rebuild: the session's stems are rebuilt from x (rebuild_spec) */
static struct { OrtSession *s; char model[65]; int rebuild; } sessions[MAX_SESSIONS];

static const char *demucs_model(OrtSession *s, int *rebuild) {
    const char *m = NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].s == s) { m = sessions[i].model; *rebuild = sessions[i].rebuild; }
    pthread_mutex_unlock(&lock);
    return m;
}

static void hex(const unsigned char *d, int n, char *out) {
    for (int i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

#if defined(_WIN32)
static FILE *create_tmp(plat_dir x, const char *tmp, int rw, const char *mode, int *err);

static void save_model_memo(const char *name, const char *stamp, const char *digest) {
    int err;
    plat_dir d = open_local_cache(&err);
    if (!plat_dir_ok(d)) return;
    plat_stat st;
    char tmp[96];
    if (!plat_dir_stat(d, &st) && local_marker_matches(d, st.id) &&
        !pathf(tmp, sizeof tmp, "%s.%d.tmp", name, getpid())) {
        FILE *f = create_tmp(d, tmp, 0, "wb", &err);
        if (f) {
            int ok = fprintf(f, "%s %s\n", stamp, digest) > 0;
            ok = fclose(f) == 0 && ok;
            if (!ok || plat_rename_at(d, tmp, d, name)) plat_unlink_at(d, tmp);
        }
    }
    plat_dir_close(d);
}
#endif

static int file_sha256_locked(const char *path, char out[65]) {
    /* the checksum is remembered per (size, modification time) in model-<inode>.sha256 */
    plat_stat st;
    /* BX3: the memo's name, `want` and the hash all come from one handle, so a file swapped
     * between a stat and the open can't get the other's checksum. ORT still opens the path itself */
    plat_file mf = NO_FILE;
    if (plat_file_open_path(path, &mf)) return -1;          /* following links, as rekordbox names it */
    if (plat_file_stat(mf, &st)) {
        plat_file_close(mf);
        return -1;
    }
#if defined(_WIN32)
    char memo[PATH_MAX], want[128], line[200];
#else
    char memo[PATH_MAX], tmp[PATH_MAX], want[128], line[200];
    FILE *f;
#endif
    if (!memo_dir[0]) memo[0] = 0;                          /* no cache: no memo, hashed every time */
    else pathf(memo, sizeof memo, "%s" SEP "model-%llu.sha256", memo_dir, (unsigned long long)plat_id_number(st.id));
    snprintf(want, sizeof want, "%lld %ld", (long long)st.size, (long)st.mtime);
    test_pause("RBSTEMS_TEST_HASH_DELAY");                  /* the memo's name and `want` taken, nothing read yet */
    int64_t size = 0;
    plat_file fd = NO_FILE;
    if (memo[0] && !plat_file_open_at(plat_dir_cwd(), memo, sizeof line - 1, &size, &fd)) {   /* memo: an absolute path */
        int r = plat_read_all(fd, line, (size_t)size);
        plat_file_close(fd);
        line[r ? 0 : size] = 0;
        /* only 64 lowercase hex digits: the value becomes a folder's name and goes into every key */
        size_t wl = strlen(want);
        if (!strncmp(line, want, wl) && line[wl] == ' ' && entries_is_hex(line + wl + 1, 64) && strlen(line + wl + 1) >= 64
            && (line[wl + 65] == '\n' || !line[wl + 65])) {
            memcpy(out, line + wl + 1, 64);
            out[64] = 0;
            TIMING(t_memo = "memo";)
            plat_file_close(mf);
            return 0;
        }
    }
    TIMING(t_memo = "hashed";)
    fd = mf;
    unsigned char *buf = malloc(1 << 20);              /* per call: concurrent sessions hashed one shared buffer */
    plat_sha256_ctx c;
    if (!buf || plat_sha256_init(&c)) { free(buf); plat_file_close(fd); return -1; }
    size_t n;
    int e;
    while (!(e = plat_read(fd, buf, 1 << 20, &n)) && n > 0) plat_sha256_update(&c, buf, n);
    free(buf);
    plat_file_close(fd);
    unsigned char d[32];
    if (plat_sha256_final(&c, d) || e) return -1;
    hex(d, 32, out);
#if defined(_WIN32)
    if (memo[0]) save_model_memo(strrchr(memo, SEP[0]) + 1, want, out);
#else
    if (memo[0] && !pathf(tmp, sizeof tmp, "%s.%d.tmp", memo, getpid()) && (f = plat_fopen_w(tmp, 0))) {
        int ok = fprintf(f, "%s %s\n", want, out) > 0;
        ok = fclose(f) == 0 && ok;
        if (!ok || plat_rename_path(tmp, memo) != 0) plat_unlink_path(tmp);
    }
#endif
    return 0;
}

static int file_sha256(const char *path, char out[65]) {
    pthread_mutex_lock(&hash_lock);                     /* sessions opened together hash once and agree */
    int r = file_sha256_locked(path, out);
    pthread_mutex_unlock(&hash_lock);
    return r;
}

/* the cache folder (and key prefix) of a model whose stems are rebuilt: still 64 hex digits, so the
 * cleanup's layout() knows it, but never the folder of the model itself, and new for each
 * REBUILD_VERSION; nonzero when the OS's sha256 can't be had (Windows only) */
static int rebuilt_id(const char *fp, char out[65]) {
    unsigned char d[32];
    plat_sha256_ctx c;
    if (plat_sha256_init(&c)) return -1;
    plat_sha256_update(&c, "rbstems-rebuild-" REBUILD_VERSION ":", strlen("rbstems-rebuild-" REBUILD_VERSION ":"));
    plat_sha256_update(&c, fp, 64);
    if (plat_sha256_final(&c, d)) return -1;
    hex(d, 32, out);
    return 0;
}

/* ---------------------------------------------------------------- the cache's layout */

/* What a walk of the cache folder finds, by name and depth (common/entries.h): only what this file
 * writes, and the companions exFAT and FAT32 add; everything else is ENT_OTHER, never read, deleted
 * or entered. Memo temporary files belong to this Mac's folder only (memo_dir): in a chosen
 * location their names are ENT_OTHER. */
static int layout(const char *n, int level, int dir, int file, const char *parent) {
    return entries_layout(n, level, dir, file, parent, !chosen);
}

/* A folder's names are walked with plat_dir_each_b: f(name, st) for each (not "." or ".."), st
 * not following a link; -1 if the folder couldn't be read whole (f returning nonzero stops it
 * there, also -1). The kinds a walk tells apart, for layout(): */
#define IS_DIR(st) ((st)->kind == PLAT_DIR)
#define IS_FILE(st) ((st)->kind == PLAT_FILE)

/* The disk space an entry takes (the size limit counts it): <key>.flac, <key>.desc and, on exFAT
 * and FAT32, their companions (common/entries.c). x: its prefix folder. A missing file counts 0. */
static long long entry_size(plat_dir x, const char *key) { return entries_size_at(x, key); }

/* ---------------------------------------------------------------- chunk summaries */

/* The summary of a chunk: log10 RMS of the mono mix and of its first difference (the high
 * band) over 0.1 s frames. mix is planar [2][L]. Returns the number of frames. */
static int summarize(const float *mix, size_t L, float *out) {
    int n = (int)(L / DESC_FRAME);
    if (n > DESC_MAX_FRAMES) n = DESC_MAX_FRAMES;
    const float *l = mix, *r = mix + L;
    double prev = 0;
    for (int f = 0; f < n; f++) {
        double e0 = 0, e1 = 0;
        for (size_t i = (size_t)f * DESC_FRAME; i < (size_t)(f + 1) * DESC_FRAME; i++) {
            double m = 0.5 * ((double)l[i] + r[i]);
            e0 += m * m;
            e1 += (m - prev) * (m - prev);
            prev = m;
        }
        out[f] = (float)log10(sqrt(e0 / DESC_FRAME) + 1e-6);
        out[n + f] = (float)log10(sqrt(e1 / DESC_FRAME) + 1e-6);
    }
    return n;
}

/* The residual of a chunk against stems (pcm, 8-ch interleaved half level) shifted by d: per
 * channel, per frame and over the whole chunk, 10 log10(resid energy / mix energy). Samples the
 * shifted stems don't cover count as explained (serve() gives them to "other"). mixe receives
 * each frame's mean square (per channel), for the -60 dBFS floor. */
static void residual(const int16_t *pcm, const float *mix, size_t L, long d, int frames,
                     float *frame_db /* [2][frames] */, float *total_db /* [2] */, double *mixe /* [2][frames] */) {
    for (int c = 0; c < 2; c++) {
        const float *m = mix + (size_t)c * L;
        double rt = 0, mt = 0;
        for (int f = 0; f < frames; f++) {
            double re = 0, me = 0;
            for (size_t n = (size_t)f * DESC_FRAME; n < (size_t)(f + 1) * DESC_FRAME; n++) {
                long k = (long)n - d;
                double s = 0;
                if (k >= 0 && k < (long)L) {
                    const int16_t *p = pcm + (size_t)k * 8 + c;
                    s = (p[0] + p[2] + p[4] + p[6]) / 32767.0 * 2.0;
                } else {
                    s = m[n];
                }
                double e = m[n] - s;
                re += e * e;
                me += (double)m[n] * m[n];
            }
            frame_db[c * frames + f] = (float)(10 * log10((re + 1e-12) / (me + 1e-12)));
            if (mixe) mixe[c * frames + f] = me / DESC_FRAME;
            rt += re;
            mt += me;
        }
        total_db[c] = (float)(10 * log10((rt + 1e-12) / (mt + 1e-12)));
    }
}

/* the .desc file: "RBD2", frames, r0, g0, summary [2*frames], total residual [2] (dB, per
 * channel), frame residuals [2][frames] (dB). "RBD1" (no residuals, before 2026-10-06) is read
 * for completeness but never offered as a near candidate; an exact hit rewrites it as RBD2. */
typedef struct { char key[65]; int frames, version; float r0, g0; float *d, *rt, *rf; } summary;
static summary *index_;
static size_t index_n;
static char index_model[65];
static time_t index_time;
static int index_loading;               /* a load is queued or running: reloads are coalesced */
static double reload_every = 30;        /* seconds: max(30, 10 x the last load's time) */
/* the last few summaries add_to_index saw, for the load that was running meanwhile (it may have
 * walked past their files before they were written) */
#define RECENT 32
static summary recent[RECENT];
static char recent_model[RECENT][65];
static unsigned long long recent_seq[RECENT], add_seq;
/* The index's ceiling: entries for max_bytes at INDEX_ENTRY_BYTES each (well under a full chunk's
 * 2.4 MB), at most INDEX_HARD_MAX (about 1.9 KB each in rekordbox's memory). Past it a load keeps
 * a partial index: exact hits and separations aren't affected, only near hits. */
#define INDEX_ENTRY_BYTES (1LL << 20)
#define INDEX_HARD_MAX 50000
static size_t index_ceiling(void) {
    long long n = max_bytes / INDEX_ENTRY_BYTES;
    return n < 1000 ? 1000 : n > INDEX_HARD_MAX ? INDEX_HARD_MAX : (size_t)n;
}

/* A chosen location's entries can be written by anyone who can write the drive, so
 * the bars a .desc sets are capped, and an exact hit must add up to the chunk. Calibrated on the
 * 118 RBD2 entries saved on the owner's Mac (2026-11-01): whole-chunk residual at worst -16.0 dB
 * (median -33.4); |g0 - 1| at most 0.065; frame residuals over -10 dB only in frames under the
 * -60 dBFS floor, which aren't judged. Garbage and corruption are caught, not forgery: stems
 * swapped in a way that keeps their sum still pass. */
#define CHOSEN_RT_MAX (-10.0)
#define CHOSEN_RF_MAX (-10.0)
#define CHOSEN_G0_SLACK 0.15
#define EXACT_RESIDUAL_MAX (-10.0)

static int all_finite(const float *v, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!isfinite(v[i])) return 0;
    return 1;
}

static void free_summary(summary *s) {
    free(s->d);
    free(s->rt);
    free(s->rf);
    s->d = s->rt = s->rf = NULL;
}

static int copy_summary(summary *to, const summary *from) {        /* deep; 0 when done */
    size_t n2 = 2 * (size_t)from->frames;
    *to = *from;
    to->d = malloc(sizeof(float) * n2);
    to->rt = malloc(sizeof(float) * 2);
    to->rf = malloc(sizeof(float) * n2);
    if (!to->d || !to->rt || !to->rf) { free_summary(to); return -1; }
    memcpy(to->d, from->d, sizeof(float) * n2);
    memcpy(to->rt, from->rt, sizeof(float) * 2);
    memcpy(to->rf, from->rf, sizeof(float) * n2);
    return 0;
}

#define DESC_MAX (16 + 8 * (2 * DESC_MAX_FRAMES) + 8)

/* a .desc's bytes; in a chosen location its bars are capped (CHOSEN_*) */
static int parse_desc(const unsigned char *b, size_t n, summary *s) {
    char magic[4];
    s->d = s->rt = s->rf = NULL;
    if (n < 16) return -1;
    memcpy(magic, b, 4);
    memcpy(&s->frames, b + 4, 4);
    memcpy(&s->r0, b + 8, 4);
    memcpy(&s->g0, b + 12, 4);
    if ((memcmp(magic, "RBD1", 4) && memcmp(magic, "RBD2", 4)) || s->frames <= 0 || s->frames > DESC_MAX_FRAMES) return -1;
    s->version = magic[3] - '0';
    size_t n2 = 2 * (size_t)s->frames, want = 16 + 4 * n2 + (s->version == 2 ? 8 + 4 * n2 : 0);
    if (n != want) return -1;
    s->d = malloc(sizeof(float) * n2);
    int ok = s->d != NULL;
    if (ok) memcpy(s->d, b + 16, 4 * n2);
    if (ok && s->version == 2) {
        s->rt = malloc(sizeof(float) * 2);
        s->rf = malloc(sizeof(float) * n2);
        ok = s->rt && s->rf;
        if (ok) {
            memcpy(s->rt, b + 16 + 4 * n2, 8);
            memcpy(s->rf, b + 24 + 4 * n2, 4 * n2);
            ok = all_finite(s->rt, 2) && all_finite(s->rf, n2);
        }
    }
    ok = ok && isfinite(s->r0) && isfinite(s->g0) && all_finite(s->d, n2);
    if (ok && chosen && s->version == 2) {
        for (int c = 0; c < 2; c++) s->rt[c] = fminf(s->rt[c], (float)CHOSEN_RT_MAX);
        for (size_t i = 0; i < n2; i++) s->rf[i] = fminf(s->rf[i], (float)CHOSEN_RF_MAX);
    }
    if (!ok) free_summary(s);
    return ok ? 0 : -1;
}

/* <key>.desc in the prefix folder x */
static int read_desc(plat_dir x, const char *key, summary *s) {
    char name[80];
    unsigned char b[DESC_MAX];
    int64_t size;
    plat_file fd;
    snprintf(name, sizeof name, "%s.desc", key);
    if (plat_file_open_at(x, name, DESC_MAX, &size, &fd)) return -1;
    int r = plat_read_all(fd, b, (size_t)size);
    plat_file_close(fd);
    return r ? -1 : parse_desc(b, (size_t)size, s);
}

/* a new temporary file tmp in x (a stale one of this pid deleted first), as a stdio stream in
 * mode; NULL with *err, and nothing left behind, when it can't be */
static FILE *create_tmp(plat_dir x, const char *tmp, int rw, const char *mode, int *err) {
    plat_file fd;
    FILE *f = NULL;
    plat_unlink_at(x, tmp);
    if ((*err = plat_file_create_at(x, tmp, rw, &fd))) return NULL;
    if (!(f = plat_file_stdio(fd, mode))) {
        *err = errno;
        plat_file_close(fd);
        plat_unlink_at(x, tmp);
    }
    return f;
}

static int write_desc(plat_dir x, const char *key, const float *d, int frames, float r0, float g0, const float *rt,
                      const float *rf, int *err) {
    char name[80], tmp[96];
    snprintf(name, sizeof name, "%s.desc", key);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", name, getpid());
    FILE *f = create_tmp(x, tmp, 0, "wb", err);
    if (!f) return -1;
    size_t n = 2 * (size_t)frames;
    int ok = fwrite("RBD2", 1, 4, f) == 4 && fwrite(&frames, 4, 1, f) == 1 && fwrite(&r0, 4, 1, f) == 1
             && fwrite(&g0, 4, 1, f) == 1 && fwrite(d, sizeof(float), n, f) == n
             && fwrite(rt, sizeof(float), 2, f) == 2 && fwrite(rf, sizeof(float), n, f) == n;
    if (!ok) *err = errno;
    if (fclose(f) != 0 && ok) { ok = 0; *err = errno; }
    int e;
    if (ok && (e = plat_rename_at(x, tmp, x, name))) { ok = 0; *err = e; }
    if (!ok) { plat_unlink_at(x, tmp); return -1; }
    return 0;
}

/* whether a valid summary may be a near-hit candidate: in a chosen location, only one whose
 * level g0 is near 1 (a .desc there can't loosen the level bar; a quiet chunk's g0 is about 0
 * and it is never a near hit anyway). The file itself stays valid: it isn't rewritten. */
static int near_usable(const summary *s) {
    return s->version == 2 && (!chosen || fabs(s->g0 - 1.0) <= CHOSEN_G0_SLACK);
}

static void free_index(summary *ix, size_t n) {
    for (size_t i = 0; i < n; i++) free_summary(&ix[i]);
    free(ix);
}

/* (Re)reads every RBD2 summary of one model's folder, off rekordbox's threads (the maintenance
 * queue), and publishes it while the location is still available; up to the ceiling. The number
 * loaded, or -1 when the location isn't available. Clears index_loading in any case. */
static long load_index(const char *model) {
    summary *ix = NULL;
    size_t n = 0;
    __block _Atomic int capped = 0;
    long r = -1;
    double t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1e9;
    pthread_mutex_lock(&lock);
    unsigned long long seq0 = add_seq;
    pthread_mutex_unlock(&lock);
    size_t limit = index_ceiling();
    int err = 0;
    plat_dir b = NO_DIR, m = NO_DIR;
    plat_stat st;
    if (loc_now() == LOC_AVAILABLE && !location_recheck()) {
        b = open_base(0, &err);
        if (plat_dir_ok(b) || (!chosen && err == ENOENT)) r = 0;   /* this Mac's folder cleared: an empty index */
    }
    /* the prefix folders are read in parallel: on a USB drive each small read waits on the drive,
     * and several at once keep it busy (measured on an exFAT stick: 37 s for 10,000 entries one at a time, 8 s in parallel) */
    __block char (*pre)[3] = NULL;
    __block size_t np = 0;
    if (plat_dir_ok(b) && plat_dir_stat(b, &st) == 0 && plat_dir_ok(m = sub_open(b, model, 0, st.id.vol, &err)) && (pre = malloc(256 * 3)))
        plat_dir_each_b(m, ^int(const char *pn, const plat_stat *ps) {
            if (layout(pn, 2, IS_DIR(ps), IS_FILE(ps), NULL) == ENT_PREFIX_DIR && np < 256) memcpy(pre[np++], pn, 3);
            return 0;
        });
    test_pause("RBSTEMS_TEST_INDEX_DELAY");                 /* the prefix folders listed, none read yet */
    typedef struct { summary *s; size_t n, cap; } bucket;
    bucket *bk = np ? calloc(np, sizeof *bk) : NULL;
    __block _Atomic size_t taken = 0;
    uint64_t vol = st.id.vol;
    if (bk)
        plat_par_for_b(np, PLAT_WORK_MAINT, ^(size_t i) {   /* the Mac: dispatch_apply at QOS_CLASS_UTILITY */
            int e;
            plat_dir x = sub_open(m, pre[i], 0, vol, &e);
            if (!plat_dir_ok(x)) return;
            bucket *k = &bk[i];
            plat_dir_each_b(x, ^int(const char *en, const plat_stat *es) {
                if (layout(en, 3, IS_DIR(es), IS_FILE(es), pre[i]) != ENT_DESC) return 0;
                if (atomic_fetch_add(&taken, 1) >= limit) { capped = 1; return 1; }
                if (k->n == k->cap) {
                    summary *t = realloc(k->s, (k->cap ? k->cap * 2 : 64) * sizeof *t);
                    if (!t) return 1;                       /* a partial index: still correct */
                    k->s = t;
                    k->cap = k->cap ? k->cap * 2 : 64;
                }
                char key[65];
                memcpy(key, en, 64);
                key[64] = 0;
                if (read_desc(x, key, &k->s[k->n]) == 0) {
                    if (!near_usable(&k->s[k->n])) free_summary(&k->s[k->n]);
                    else { memcpy(k->s[k->n].key, key, 65); k->n++; }
                }
                return 0;
            });
            plat_dir_close(x);
        });
    for (size_t i = 0; bk && i < np; i++) n += bk[i].n;
    if (n && (ix = malloc(n * sizeof *ix))) {
        size_t at = 0;
        for (size_t i = 0; i < np; i++) { memcpy(ix + at, bk[i].s, bk[i].n * sizeof *ix); at += bk[i].n; }
    } else if (n) {                                         /* out of memory: no index this time */
        for (size_t i = 0; i < np; i++) free_index(bk[i].s, bk[i].n), bk[i].s = NULL;
        n = 0;
    }
    for (size_t i = 0; bk && i < np; i++) free(bk[i].s);
    free(bk);
    free(pre);
    plat_dir_close(m);
    plat_dir_close(b);
    summary *t = r >= 0 ? realloc(ix, (n + RECENT + 1) * sizeof *ix) : NULL;   /* room for the merge */
    if (t) ix = t;
    double took = clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1e9 - t0;
    pthread_mutex_lock(&lock);
    if (r >= 0 && t && loc_state == LOC_AVAILABLE) {
        for (int i = 0; i < RECENT; i++) {                   /* stored while this load ran */
            if (recent_seq[i] <= seq0 || strcmp(recent_model[i], model) || n >= limit) continue;
            size_t j = 0;
            while (j < n && strcmp(ix[j].key, recent[i].key)) j++;
            if (j == n && copy_summary(&ix[n], &recent[i]) == 0) n++;
        }
        summary *old = index_;
        size_t old_n = index_n;
        index_ = ix;
        index_n = n;
        memcpy(index_model, model, 65);
        index_time = time(NULL);
        reload_every = fmax(30, 10 * took);
        ix = old;
        n = old_n;
        r = (long)index_n;
    } else r = -1;
    index_loading = 0;
    pthread_mutex_unlock(&lock);
    free_index(ix, n);
    if (capped) logf_("summaries: stopped at %zu (the most kept for a %.1f GB limit): near hits use those; exact hits are unaffected",
                      limit, max_bytes / (double)(1LL << 30));
    if (took > 2) logf_("summaries: loading took %.1f s; reloaded at most every %.0f s", took, fmax(30, 10 * took));
    return r;
}
#ifdef RBSTEMS_TEST_TIMING
static long load_index_timed(const char *model) {
    uint64_t t0 = t_now();
    long r = load_index(model);
    uint64_t t1 = t_now();
    logf_("timing index entries=%ld total_ns=%llu", r, (unsigned long long)(t1 - t0));
    return r;
}
#define load_index(m) load_index_timed(m)
#endif

/* queues a load of model's summaries on the maintenance queue, unless one is queued already */
static void queue_index_load(const char *model) {
    pthread_mutex_lock(&lock);
    int go = !index_loading;
    index_loading = 1;
    pthread_mutex_unlock(&lock);
    char *m = go ? strdup(model) : NULL;
    if (m && !plat_async_b(maint, ^{
            load_index(m);
            free(m);
        })) return;
    free(m);                                                /* not queued (never on the Mac) */
    if (go) { pthread_mutex_lock(&lock); index_loading = 0; pthread_mutex_unlock(&lock); }
}

static void add_to_index(const char *model, const char *key, const float *d, int frames, float r0, float g0,
                         const float *rt, const float *rf) {
    summary src = {.frames = frames, .version = 2, .r0 = r0, .g0 = g0, .d = (float *)d, .rt = (float *)rt, .rf = (float *)rf}, s, r;
    memcpy(src.key, key, 65);
    if (!near_on || !near_usable(&src) || copy_summary(&s, &src)) return;
    int have_r = copy_summary(&r, &src) == 0, placed = 0;
    summary old_s = {0}, old_r = {0};
    pthread_mutex_lock(&lock);
    if (loc_state == LOC_AVAILABLE && !strcmp(index_model, model)) {
        for (size_t i = 0; i < index_n && !placed; i++)     /* the same entry again (an upgrade): replaced */
            if (!strcmp(index_[i].key, key)) { old_s = index_[i]; index_[i] = s; placed = 1; }
        summary *ix;
        if (!placed && index_n < index_ceiling() && (ix = realloc(index_, (index_n + 1) * sizeof *ix))) {
            index_ = ix;
            index_[index_n++] = s;
            placed = 1;
        }
    }
    if (have_r) {
        int slot = -1;
        for (int i = 0; i < RECENT && slot < 0; i++)
            if (recent_seq[i] && !strcmp(recent[i].key, key) && !strcmp(recent_model[i], model)) slot = i;
        if (slot < 0) slot = (int)(add_seq % RECENT);
        old_r = recent[slot];
        recent[slot] = r;
        memcpy(recent_model[slot], model, 65);
        recent_seq[slot] = ++add_seq;
    }
    pthread_mutex_unlock(&lock);
    if (!placed) free_summary(&s);
    free_summary(&old_s);
    free_summary(&old_r);
}

/* forgets the index (a lost location); under lock. The caller frees what it returns. */
static summary *forget_index_locked(size_t *n) {
    summary *old = index_;
    *n = index_n;
    index_ = NULL;
    index_n = 0;
    index_model[0] = 0;
    index_time = 0;
    return old;
}

/* the NEAR_CANDIDATES nearest RBD2 summaries of model closer than NEAR_MAX, copied out */
static int nearest(const char *model, const float *d, int frames, summary *out) {
    double best[NEAR_CANDIDATES];
    int found = 0;
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < index_n && !strcmp(index_model, model); i++) {
        if (index_[i].frames != frames) continue;
        double dist = 0;
        for (int j = 0; j < 2 * frames; j++) dist += fabs(index_[i].d[j] - d[j]);
        dist /= 2 * frames;
        if (!(dist <= NEAR_MAX)) continue;                 /* NaN never passes */
        int k;
        if (found < NEAR_CANDIDATES) k = found++;
        else if (dist >= best[NEAR_CANDIDATES - 1]) continue;
        else k = NEAR_CANDIDATES - 1;
        while (k > 0 && best[k - 1] > dist) {
            best[k] = best[k - 1];
            out[k] = out[k - 1];
            k--;
        }
        best[k] = dist;
        out[k] = index_[i];                                /* shallow: arrays copied below */
    }
    for (int k = 0; k < found; k++) {                       /* deep copies, valid after the lock */
        size_t n2 = 2 * (size_t)out[k].frames;
        float *rt = malloc(sizeof(float) * 2), *rf = malloc(sizeof(float) * n2);
        if (rt && rf) {
            memcpy(rt, out[k].rt, sizeof(float) * 2);
            memcpy(rf, out[k].rf, sizeof(float) * n2);
        } else {
            free(rt);
            free(rf);
            rt = rf = NULL;
        }
        out[k].d = NULL;
        out[k].rt = rt;
        out[k].rf = rf;
    }
    pthread_mutex_unlock(&lock);
    return found;
}

/* ---------------------------------------------------------------- verification */

/* mono stems sum (full level) from cached pcm, and mono mix */
static void monos(const int16_t *pcm, const float *mix, size_t L, float *s, float *m) {
    for (size_t i = 0; i < L; i++) {
        double v = 0;
        for (int ch = 0; ch < 8; ch++) v += pcm[i * 8 + ch];
        s[i] = (float)(v / 32767.0 * 2.0 * 0.5);                 /* stems doubled, L+R halved */
        m[i] = 0.5f * (mix[i] + mix[L + i]);
    }
}

/* correlation and level of mix[n] against stems[n - d] (normalised by the whole signals) */
static void corr_at(const float *s, const float *m, size_t L, long d, double ss, double mm, double *r, double *g) {
    double sm = 0;
    for (size_t n = 0; n < L; n++) {
        long k = (long)n - d;
        if (k >= 0 && k < (long)L) sm += (double)m[n] * s[k];
    }
    *r = sm / sqrt(ss * mm + 1e-30);
    *g = sm / (mm + 1e-30);
}

/* the shift d (|d| <= SHIFT_MAX) that best aligns stems with the mix: coarse on 16x averaged
 * signals, then sample-exact around it */
static long best_shift(const float *s, const float *m, size_t L, double ss, double mm) {
    size_t M = L / 16;
    float *a = malloc(M * sizeof(float)), *b = malloc(M * sizeof(float));
    long coarse = 0;
    if (a && b) {
        for (size_t i = 0; i < M; i++) {
            double x = 0, y = 0;
            for (int j = 0; j < 16; j++) { x += s[i * 16 + j]; y += m[i * 16 + j]; }
            a[i] = (float)x;
            b[i] = (float)y;
        }
        double best = -1e300;
        for (long c = -SHIFT_MAX / 16; c <= SHIFT_MAX / 16; c++) {
            double v = 0;
            for (size_t i = 0; i < M; i++) {
                long k = (long)i - c;
                if (k >= 0 && k < (long)M) v += (double)b[i] * a[k];
            }
            if (v > best) { best = v; coarse = c; }
        }
    }
    free(a);
    free(b);
    long d0 = coarse * 16, bestd = d0;
    double bestv = -1e300, r, g;
    for (long d = d0 - 24; d <= d0 + 24; d++) {
        if (d < -SHIFT_MAX || d > SHIFT_MAX) continue;
        corr_at(s, m, L, d, ss, mm, &r, &g);
        if (r > bestv) { bestv = r; bestd = d; }
    }
    return bestd;
}

/* does the requested chunk pass the entry's residual bars? worst: the largest excess in dB */
static int residual_ok(const int16_t *pcm, const float *mix, size_t L, long d, const summary *e, double *worst,
                       double *tot0, double *tot1) {
    int frames = e->frames;
    float *rf = malloc(sizeof(float) * 2 * frames), rt[2];
    double *me = malloc(sizeof(double) * 2 * frames);
    int ok = rf && me;
    *worst = -1e9;
    if (ok) {
        residual(pcm, mix, L, d, frames, rf, rt, me);
        long fs = lround((double)d / DESC_FRAME);           /* the entry's frame the content came from */
        for (int c = 0; c < 2; c++) {
            double ex = rt[c] - (e->rt[c] + M_TOTAL);
            if (ex > *worst) *worst = ex;
            if (ex > 0) ok = 0;
            for (int f = 0; f < frames; f++) {
                if (me[c * frames + f] < FRAME_FLOOR) continue;
                long fo = f - fs;
                if (fo < 0) fo = 0;
                if (fo >= frames) fo = frames - 1;
                double bar = fmax(e->rf[c * frames + fo] + M_FRAME, R_FLOOR);
                double exf = rf[c * frames + f] - bar;
                if (exf > *worst) *worst = exf;
                if (exf > 0) ok = 0;
            }
        }
        *tot0 = rt[0];
        *tot1 = rt[1];
    }
    free(rf);
    free(me);
    return ok;
}

/* ---------------------------------------------------------------- limits */

/* Kept sets: a prepared playlist's entries, kept until a date. The prepare helper writes one file
 * per set into kept_dir (<name>.kept, renamed into place whole), exactly:
 *     rbstemsplus-kept 1\n
 *     until <seconds since 1970, decimal, no leading zero>\n
 *     <model folder> <key>\n          (each 64 lowercase hex digits; any number of lines, 0 too)
 * A file that differs in any byte, is over KEPT_FILE_MAX, or isn't a plain file (a link) keeps
 * nothing, with a log line; one whose date has passed keeps nothing, silently. Over KEPT_SETS_MAX
 * files (the app makes at most 5) keeps nothing at all: never a choice of which. Names starting
 * with '.' are someone's temporary files. The bridge never writes or deletes them. */
#define KEPT_SETS_MAX 5
#define KEPT_FILE_MAX (8 << 20)                 /* ~64,000 entries: about 1 TB of stems */
#define KEPT_LINE 130                           /* "<64> <64>\n" */
typedef struct { unsigned char model[32], key[32]; } kept_entry;
static void unhex(const char *s, unsigned char *out);

static int by_kept(const void *a, const void *b) { return memcmp(a, b, sizeof(kept_entry)); }

/* a kept file's bytes: 0 with *until and *count (the entries, written to out unless it is NULL),
 * -1 if they aren't exactly the format above */
static int parse_kept(const char *b, size_t n, long long *until, kept_entry *out, size_t *count) {
    static const char head[] = "rbstemsplus-kept 1\nuntil ";
    size_t h = sizeof head - 1, i;
    long long u = 0;
    if (n < h || memcmp(b, head, h)) return -1;
    for (i = h; i < n && i - h < 12 && b[i] >= '0' && b[i] <= '9'; i++) u = u * 10 + (b[i] - '0');
    if (i == h || i >= n || b[i] != '\n' || (b[h] == '0' && i - h > 1)) return -1;
    i++;
    if ((n - i) % KEPT_LINE) return -1;
    size_t k = 0;
    for (; i < n; i += KEPT_LINE, k++) {
        if (!entries_is_hex(b + i, 64) || b[i + 64] != ' ' || !entries_is_hex(b + i + 65, 64) || b[i + 129] != '\n') return -1;
        if (out) { unhex(b + i, out[k].model); unhex(b + i + 65, out[k].key); }
    }
    *until = u;
    *count = k;
    return 0;
}

/* the entries of every kept set still in its time, sorted (duplicates are harmless), into *out;
 * returns their number and the sets' in *sets. Nothing (0) without the folder or on any doubt. */
static size_t load_kept(kept_entry **out, size_t *sets, time_t now) {
    __block kept_entry *v = NULL;
    __block size_t n = 0, files = 0, live = 0;
    *out = NULL;
    *sets = 0;
    if (!kept_dir[0]) return 0;
    plat_dir d;
    int e = plat_dir_open(kept_dir, &d);
    if (e) {
        if (e != ENOENT) logf_("kept: the folder can't be read (%s): nothing is kept", strerror(e));
        return 0;
    }
    /* one walk: past KEPT_SETS_MAX files it only counts */
    int ok = !plat_dir_each_b(d, ^int(const char *name, const plat_stat *lst) {
        size_t l = strlen(name), c = 0, got = 0, r = 0;
        long long until = 0;
        if (name[0] == '.' || l <= 5 || strcmp(name + l - 5, ".kept") || ++files > KEPT_SETS_MAX) return 0;
        const char *why = NULL;
        char *b = NULL;
        int64_t size = 0;
        plat_file fd = NO_FILE;
        int fe = IS_FILE(lst) ? plat_file_open_at(d, name, INT64_MAX, &size, &fd) : PLAT_EFTYPE;
        if (fe) why = fe == PLAT_EFTYPE ? "not a plain file" : strerror(fe);
        else if (size > KEPT_FILE_MAX) why = "too big";
        else if (!(b = malloc((size_t)size + 1))) why = "out of memory";
        else {
            while (got <= (size_t)size && !(fe = plat_read(fd, b + got, (size_t)size + 1 - got, &r)) && r > 0) got += r;
            if (fe || got != (size_t)size) why = "changed while read";
            else if (parse_kept(b, got, &until, NULL, &c)) why = "not a kept set";
        }
        plat_file_close(fd);
        if (!why && until > now && c) {
            kept_entry *t = c <= SIZE_MAX / sizeof *v - n ? realloc(v, (n + c) * sizeof *v) : NULL;
            if (!t) why = "out of memory";
            else {
                v = t;
                parse_kept(b, got, &until, v + n, &c);
                n += c;
            }
        }
        if (!why && until > now) live++;
        free(b);
        if (why) logf_("kept: %s ignored: %s", name, why);
        return 0;
    });
    plat_dir_close(d);
    if (!ok || files > KEPT_SETS_MAX) {
        logf_(ok ? "kept: %zu sets, over %d: nothing is kept" : "kept: the folder can't be read (%zu sets so far): nothing is kept", files, KEPT_SETS_MAX);
        free(v);
        return 0;
    }
    if (n) qsort(v, n, sizeof *v, by_kept);
    *out = v;
    *sets = live;
    return n;
}

/* one file of an entry as prune finds it: kept small (the model is an index into prune's table of
 * model folders, the key 32 bytes), so a big cache's list stays small too. kept: in a kept set
 * (live entries only) */
typedef struct { unsigned char key[32]; uint32_t model; uint8_t kind, kept; time_t mtime; long long bytes; } pfile;
enum { PF_FLAC, PF_DESC, PF_CFLAC, PF_CDESC };

static int by_key(const void *a, const void *b) {
    const pfile *x = a, *y = b;
    if (x->model != y->model) return x->model < y->model ? -1 : 1;
    int c = memcmp(x->key, y->key, 32);
    return c ? c : x->kind - y->kind;
}

static int by_mtime(const void *a, const void *b) {   /* entries not kept first, least recently used first */
    const pfile *p = a, *q = b;
    if (p->kept != q->kept) return p->kept - q->kept;
    return p->mtime < q->mtime ? -1 : p->mtime > q->mtime;
}

static void unhex(const char *s, unsigned char *out) {     /* 64 lowercase hex digits (entries_is_hex) */
    for (int i = 0; i < 32; i++) {
        int hi = s[2 * i] <= '9' ? s[2 * i] - '0' : s[2 * i] - 'a' + 10, lo = s[2 * i + 1] <= '9' ? s[2 * i + 1] - '0' : s[2 * i + 1] - 'a' + 10;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
}

/* deletes an entry's files (and companions) through its prefix folder; 0 if its FLAC is gone */
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
static int remove_entry(plat_dir b, uint64_t vol, const char *model, const char *key) {
    int err, r = -1, e;
    plat_dir m, x = mutation_entry_dir(b, vol, model, key, 0, &m, &err);
    if (!plat_dir_ok(x)) { plat_dir_close(m); return -1; }
#else
static int remove_entry(const char *model, const char *key) {
    int err, r = -1, e;
    plat_dir x = entry_dir(model, key, 0, &err);
    if (!plat_dir_ok(x)) return -1;
#endif
    for (int i = 0; i < ENTRIES_FILES; i++) {
        char name[80];
        entries_file(name, sizeof name, key, i);
        if ((e = plat_unlink_at(x, name)) == 0 || e == ENOENT) { if (!i) r = 0; }
    }
    plat_dir_close(x);
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    plat_dir_close(m);
#endif
    return r;
}

/* a companion ("._<name>") whose file or folder is gone, left for over an hour: deleted */
static void orphan(plat_dir dir, const char *name, const plat_stat *st, time_t now) {
    plat_stat o;
    if (now - st->mtime > 3600 && plat_stat_at(dir, name + 2, &o) == ENOENT) plat_unlink_at(dir, name);
}

/* The limits, off rekordbox's threads (maintenance queue): entries unused for max_days go, then
 * over max_bytes the least recently used, down to 90% of it. An entry in a kept set (load_kept)
 * never ages out, and over max_bytes goes only once every other entry has gone. Sizes are the disk space taken
 * (entry_size: companions too); each entry's files are grouped by key first. Only layout()'s
 * names are touched, through folders opened without following links (sub_open). A walk that
 * couldn't read everything (an error, out of memory) evicts nothing and sets no total.
 * Writes go on meanwhile (the writer queue): what they add while the walk runs is kept aside
 * (prune_pending) and added to the walk's total, so no write is lost from the count (one the walk
 * also found is counted twice until the next prune: too much, never too little). Returns whether
 * the walk was complete. */
static int prune_running;               /* under lock */
static long long prune_pending;
static int prune(void) {
    __block pfile *pf = NULL;
    __block size_t n = 0, cap = 0, nm = 0;
    __block char (*models)[65] = NULL;
    __block long long temps = 0;
    __block int complete = 1;
    long long total = 0, aged = 0, evicted = 0;
    time_t now = time(NULL);
    int err;
    plat_dir b = NO_DIR;
    plat_stat bs;
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    if (loc_now() != LOC_AVAILABLE) return 0;
    b = open_base(0, &err);
    if (!plat_dir_ok(b)) return !chosen && err == ENOENT;
    if (plat_dir_stat(b, &bs) || !mutation_allowed(b, bs.id)) { plat_dir_close(b); return 0; }
#else
    if (loc_now() != LOC_AVAILABLE || location_recheck() || !plat_dir_ok(b = open_base(0, &err))) return !chosen && !plat_dir_ok(b) && err == ENOENT;
    if (plat_dir_stat(b, &bs)) { plat_dir_close(b); return 0; }
#endif
    pthread_mutex_lock(&lock);
    prune_running = 1;
    prune_pending = 0;
    pthread_mutex_unlock(&lock);
    uint64_t vol = bs.id.vol;
    int (^add)(int, const char *, int, const plat_stat *) = ^int(int model, const char *name, int kind, const plat_stat *st) {
        if (n == cap) {
            pfile *t = realloc(pf, (cap ? cap * 2 : 4096) * sizeof *pf);
            if (!t) { complete = 0; return 1; }
            pf = t;
            cap = cap ? cap * 2 : 4096;
        }
        unhex(name, pf[n].key);
        pf[n].model = (uint32_t)model;
        pf[n].kind = (uint8_t)kind;
        pf[n].mtime = (time_t)st->mtime;
        pf[n].bytes = (long long)st->alloc;                 /* the disk space taken (st_blocks * 512) */
        n++;
        return 0;
    };
    if (plat_dir_each_b(b, ^int(const char *name, const plat_stat *st) {
            int k = layout(name, 1, IS_DIR(st), IS_FILE(st), NULL);
            if (k == ENT_MODEL_DIR) {
                if (!(nm & (nm - 1)) || !models) {
                    char (*t)[65] = realloc(models, (nm ? nm * 2 : 1) * 65);
                    if (!t) { complete = 0; return 1; }
                    models = t;
                }
                memcpy(models[nm++], name, 65);
            } else if (k == ENT_MEMO_TMP && now - st->mtime > 3600) {
                if (!plat_unlink_at(b, name)) temps++;
            } else if (k == ENT_COMP_DIR) orphan(b, name, st, now);
            return 0;
        })) complete = 0;
    test_pause("RBSTEMS_TEST_PRUNE_DELAY");                 /* the top level walked, the model folders not yet */
    for (size_t i = 0; i < nm && complete; i++) {
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
        plat_dir m = mutation_sub_open(b, models[i], 0, vol, &err);
#else
        plat_dir m = sub_open(b, models[i], 0, vol, &err);
#endif
        if (!plat_dir_ok(m)) { if (err != ENOENT && err != EXDEV) complete = 0; continue; }
        if (plat_dir_each_b(m, ^int(const char *pn, const plat_stat *ps) {
                int k = layout(pn, 2, IS_DIR(ps), IS_FILE(ps), NULL), e;
                if (k == ENT_COMP_DIR) orphan(m, pn, ps, now);
                if (k != ENT_PREFIX_DIR) return 0;
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
                plat_dir x = mutation_sub_open(m, pn, 0, vol, &e);
#else
                plat_dir x = sub_open(m, pn, 0, vol, &e);
#endif
                if (!plat_dir_ok(x)) { if (e != ENOENT && e != EXDEV) complete = 0; return !complete; }
                if (plat_dir_each_b(x, ^int(const char *en, const plat_stat *es) {
                        int kind = layout(en, 3, IS_DIR(es), IS_FILE(es), pn);
                        switch (kind) {
                        case ENT_FLAC: return add((int)i, en, PF_FLAC, es);
                        case ENT_DESC: return add((int)i, en, PF_DESC, es);
                        case ENT_COMP_FLAC: return add((int)i, en + 2, PF_CFLAC, es);
                        case ENT_COMP_DESC: return add((int)i, en + 2, PF_CDESC, es);
                        case ENT_COMP_TMP: orphan(x, en, es, now); return 0;
                        case ENT_TMP:
                            if (now - es->mtime > 3600 && !plat_unlink_at(x, en)) {
                                char c[96];
                                snprintf(c, sizeof c, "._%s", en);
                                plat_unlink_at(x, c);
                                temps++;
                            }
                            return 0;
                        default: return 0;
                        }
                    })) complete = 0;
                plat_dir_close(x);
                return !complete;
            })) complete = 0;
        plat_dir_close(m);
    }
#if !defined(_WIN32) && !defined(RBSTEMS_WIN_CACHE_TEST)
    plat_dir_close(b);
#endif
    plat_dir md;
    if (chosen && memo_dir[0] && !plat_dir_open(memo_dir, &md)) {   /* the memos' folder: its stale temporary files only */
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
        plat_stat ms;
        if (!plat_dir_stat(md, &ms) && local_marker_matches(md, ms.id))
#endif
        plat_dir_each_b(md, ^int(const char *name, const plat_stat *st) {
            if (IS_FILE(st) && entries_is_memo_tmp(name) && now - st->mtime > 3600 && !plat_unlink_at(md, name)) temps++;
            return 0;
        });
        plat_dir_close(md);
    }
    size_t live = 0, kn = 0, ksets = 0;
    long long kentries = 0, kbytes = 0, kevicted = 0;
    kept_entry *kv = NULL;
    unsigned char (*mbin)[32] = NULL;
    if (complete && (kn = load_kept(&kv, &ksets, now)) && !(mbin = malloc((nm ? nm : 1) * sizeof *mbin))) {
        logf_("kept: out of memory: nothing is kept");
        kn = 0;
    }
    for (size_t i = 0; i < nm && mbin; i++) unhex(models[i], mbin[i]);
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    /* Entry eviction is a new deletion pass. Re-read ownership through the base we walked. */
    if (complete && !mutation_allowed(b, bs.id)) complete = 0;
#define REMOVE_ENTRY(model, key) remove_entry(b, vol, (model), (key))
#else
#define REMOVE_ENTRY(model, key) remove_entry(model, key)
#endif
    if (complete) {
        qsort(pf, n, sizeof *pf, by_key);
        for (size_t i = 0; i < n;) {                        /* one entry: its files, grouped by key */
            size_t j = i;
            long long bytes = 0;
            time_t flac = -1, newest = 0;
            while (j < n && pf[j].model == pf[i].model && !memcmp(pf[j].key, pf[i].key, 32)) {
                bytes += pf[j].bytes;
                if (pf[j].kind == PF_FLAC) flac = pf[j].mtime;
                if (pf[j].mtime > newest) newest = pf[j].mtime;
                j++;
            }
            char key[65];
            hex(pf[i].key, 32, key);
            uint32_t model = pf[i].model;
            int kept = 0;
            if (kn && flac >= 0) {
                kept_entry probe;
                memcpy(probe.model, mbin[model], 32);
                memcpy(probe.key, pf[i].key, 32);
                kept = bsearch(&probe, kv, kn, sizeof *kv, by_kept) != NULL;
            }
            if (kept) { kentries++; kbytes += bytes; }
            if (flac >= 0 && !kept && now - flac > (time_t)(max_days * 86400)) {
                if (!REMOVE_ENTRY(models[model], key)) aged++;
                else total += bytes;
            } else if (flac < 0 && now - newest > 3600) {
                REMOVE_ENTRY(models[model], key);           /* a summary or companions without the stems */
            } else {
                total += bytes;
                if (flac >= 0) {                            /* the list of live entries reuses pf (live <= i) */
                    memcpy(pf[live].key, pf[i].key, 32);
                    pf[live].model = model;
                    pf[live].mtime = flac;
                    pf[live].bytes = bytes;
                    pf[live].kept = (uint8_t)kept;
                    live++;
                }
            }
            i = j;
        }
        if (total > max_bytes) {
            qsort(pf, live, sizeof *pf, by_mtime);          /* not kept, least recently used, first */
            for (size_t i = 0; i < live && total > max_bytes * 9 / 10; i++) {
                char key[65];
                hex(pf[i].key, 32, key);
                if (!REMOVE_ENTRY(models[pf[i].model], key)) {
                    total -= pf[i].bytes;
                    evicted++;
                    if (pf[i].kept) { kevicted++; kentries--; kbytes -= pf[i].bytes; }
                }
            }
        }
    }
#undef REMOVE_ENTRY
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    plat_dir_close(b);
#endif
    if (kn || ksets)
        logf_("kept: %zu sets, %lld of their entries in the cache (%.2f GB)", ksets, kentries, kbytes / (double)(1LL << 30));
    if (kevicted) logf_("kept: %lld kept entries removed: the cache was over %.1f GB with every other entry gone", kevicted, max_bytes / (double)(1LL << 30));
    free(kv);
    free(mbin);
    pthread_mutex_lock(&lock);
    if (complete && loc_state == LOC_AVAILABLE) total_bytes = total + prune_pending;
    prune_running = 0;
    pthread_mutex_unlock(&lock);
    free(pf);
    free(models);
    logf_("limits: %.2f GB in cache (%lld bytes on disk); removed %lld unused for %g days, %lld over %.1f GB, %lld stale temp files%s",
          total / (double)(1LL << 30), total, aged, max_days, evicted, max_bytes / (double)(1LL << 30), temps,
          complete ? "" : " (the folder couldn't be read whole: no eviction this time)");
    return complete;
}
#ifdef RBSTEMS_TEST_TIMING
static int prune_timed(void) {
    uint64_t t0 = t_now();
    int r = prune();
    uint64_t t1 = t_now();
    logf_("timing prune complete=%d total_ns=%llu", r, (unsigned long long)(t1 - t0));
    return r;
}
#define prune() prune_timed()
#endif

/* The limits at launch, once per load: retried (a minute apart, three times at most) when the walk
 * couldn't read everything, or the limits would go unenforced for the run (no total to grow). */
static void first_prune(int tries) {
    if (prune() || tries >= 3 || loc_now() != LOC_AVAILABLE) return;
    plat_after_b(maint, 60 * 1000000000ull, ^{ first_prune(tries + 1); });
}

/* queues prune on the maintenance queue, unless it is queued already */
static int prune_queued;                /* under lock */
static void queue_prune(void) {
    pthread_mutex_lock(&lock);
    int go = !prune_queued;
    prune_queued = 1;
    pthread_mutex_unlock(&lock);
    if (go && plat_async_b(maint, ^{
            pthread_mutex_lock(&lock);
            prune_queued = 0;
            pthread_mutex_unlock(&lock);
            prune();
        })) {
        pthread_mutex_lock(&lock);                          /* not queued (never on the Mac) */
        prune_queued = 0;
        pthread_mutex_unlock(&lock);
    }
}

/* ---------------------------------------------------------------- free space */

/* The floor (the user's decision, not a setting): under it no FLAC, .desc or index entry is
 * written. 10% of the size of the cache's disk, at most FREE_FLOOR_MAX; FREE_FLOOR_MAX when the size
 * can't be read. In bytes, GB as Finder counts them (10^9 bytes), like the app's status line, which
 * computes the same floor (cacheFreeFloor in app/Sources/State.swift). */
#define FREE_FLOOR_MAX 50000000000ULL
#define FREE_EVERY 60                   /* seconds a measurement is reused */

/* Free bytes on the cache's volume as Finder shows them (including purgeable space, which macOS
 * frees on demand), else statfs's smaller count; 0 if neither answers. A test build can fake it
 * (RBSTEMS_TEST_FREE_GB) or skip Finder's count (RBSTEMS_TEST_STATFS). */
static double free_bytes(const char **how) {
    int finder = 1;
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_FREE_GB");
    if (t) { *how = "test"; return atof(t) * 1e9; }
    finder = !getenv("RBSTEMS_TEST_STATFS");
#endif
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    if (!chosen) {
        int err = make_local_cache();
        local_cache_status(err);
        if (err) { *how = "This PC cache unavailable"; return 0; }
    }
#else
    if (!chosen) plat_mkdirs(base_dir);                     /* this Mac's: it may have been cleared since loading */
#endif
    return (double)plat_free_bytes(base_dir, finder, how);
}

/* The size of the cache's volume in bytes as statfs counts it (f_blocks * f_bsize, as the app
 * does); 0 if it can't be read. A test build can fake it (RBSTEMS_TEST_DISK_GB; 0: can't be read).
 * Called after free_bytes, which creates this Mac's cache folder. */
static uint64_t disk_bytes(void) {
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_DISK_GB");
    if (t) { double g = atof(t); return g > 0 ? (uint64_t)(g * 1e9 + 0.5) : 0; }
#endif
    plat_volume v;
    return plat_volume_info(base_dir, &v) == 0 ? v.total : 0;
}

/* the floor for a disk of `disk` bytes (0: unknown): 10% of it, at most FREE_FLOOR_MAX */
static uint64_t free_floor(uint64_t disk) {
    return disk > 0 && disk / 10 < FREE_FLOOR_MAX ? disk / 10 : FREE_FLOOR_MAX;
}

/* whether a cache write may go ahead: measured at most every FREE_EVERY seconds, logged when
 * saving pauses and when it resumes. Called on the writer queue only; its state is under lock
 * because a lost location resets it. */
static uint64_t space_checked;
static int space_ok(void) {
    uint64_t every = FREE_EVERY, now = clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000;
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_FREE_SECONDS");
    if (t) every = (uint64_t)atoi(t);
#endif
    pthread_mutex_lock(&lock);
    int paused = space_paused, fresh = paused >= 0 && now - space_checked < every;
    if (!fresh) space_checked = now;
    pthread_mutex_unlock(&lock);
    if (fresh) return !paused;
    const char *how;
    double b = free_bytes(&how);
    uint64_t disk = disk_bytes(), fl = free_floor(disk);
    char size[48];
    if (disk) snprintf(size, sizeof size, "disk %.1f GB", disk / 1e9);
    else snprintf(size, sizeof size, "disk size unknown");
    int p = !(b >= (double)fl);
    if (p && paused != 1)
        logf_("cache: saving paused, %.1f GB free on its disk (%s), under its floor of %.1f GB (%s); saved stems still load",
              b / 1e9, how, fl / 1e9, size);
    else if (!p && paused == 1)
        logf_("cache: saving resumed, %.1f GB free on its disk (%s), floor %.1f GB (%s)", b / 1e9, how, fl / 1e9, size);
    else if (paused < 0)
        logf_("cache: %.1f GB free on its disk (%s), floor %.1f GB (%s)", b / 1e9, how, fl / 1e9, size);
    pthread_mutex_lock(&lock);
    space_paused = p;
    pthread_mutex_unlock(&lock);
    return !p;
}

/* The chosen location failed (see "where the stems are"): unavailable until a recheck finds it
 * usable again. The index, the total and the space check are forgotten; bridge-state and the log
 * say why. */
static void lose(const char *why) {
    if (!chosen) return;
    size_t n = 0;
    summary *old = NULL;
    pthread_mutex_lock(&lock);
    int was = loc_state;
    if (was != LOC_UNAVAILABLE) {
        loc_state = LOC_UNAVAILABLE;
        loc_reason = why;
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
        pin = (plat_id){0};
#endif
        old = forget_index_locked(&n);
        total_bytes = -1;
        space_paused = -1;
    }
    pthread_mutex_unlock(&lock);
    if (was == LOC_UNAVAILABLE) return;
    free_index(old, n);
    write_state();
    logf_("cache: %s not available (%s): stems aren't saved or loaded until it is; checked again every %u seconds", base_dir, reason_text(why), recheck_every());
    arm_recheck();
}

/* The chosen location opened as another folder than the pinned one (was): its drive
 * connected again since the last use (a new mount has new device numbers), or the folder
 * replaced. Checked at once as at launch (check_location): when it passes, the folder found is
 * pinned and used, its space counted afresh and the index loaded again, as after a recheck.
 * 1 then (or when another thread pinned it meanwhile); 0 and nothing changed otherwise: the
 * caller loses it. Any thread: the space count runs on the maintenance queue. */
#if !defined(_WIN32) && !defined(RBSTEMS_WIN_CACHE_TEST)
static int repin(plat_id was) {
    plat_id id = {0, {0, 0}};
    char mnt[PLAT_MOUNT_MAX] = "";
    if (check_location(&id, mnt)) return 0;
    size_t n = 0;
    summary *old = NULL;
    int ours = 0, fresh = 0;
    pthread_mutex_lock(&lock);
    if (loc_state == LOC_AVAILABLE) {
        ours = 1;
        if (plat_id_same(pin, was)) {
            pin = id;
            strlcpy(mount_dir, mnt, sizeof mount_dir);
            old = forget_index_locked(&n);
            total_bytes = -1;
            space_paused = -1;
            writer_checked = 0;
            fresh = 1;
        }
    }
    pthread_mutex_unlock(&lock);
    if (!fresh) return ours;
    free_index(old, n);
    log_using("checked again: using");
    plat_async_b(maint, ^{ first_prune(0); });
    return 1;
}
#endif

/* ---------------------------------------------------------------- FLAC */

typedef struct { int16_t *pcm; size_t frames; } write_job;   /* pcm interleaved, 8 channels */

/* <key>.flac in the prefix folder x, through a temporary file renamed when complete; -1 with *err */
static int write_flac(plat_dir x, const char *key, write_job *job, int *err) {
    char name[80], tmp[96];
    snprintf(name, sizeof name, "%s.flac", key);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", name, getpid());
    FILE *f = create_tmp(x, tmp, 1, "w+b", err);            /* w+b: the encoder seeks back for STREAMINFO */
    if (!f) return -1;
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    if (!enc) { fclose(f); plat_unlink_at(x, tmp); *err = ENOMEM; return -1; }
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamMetadata_VorbisComment_Entry ent;
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&ent, "RBSTEMS_KEY", key);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, ent, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&ent, "RBSTEMS_FORMAT", FORMAT " drums,bass,other,vocals L/R half-level");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, ent, false);
    FLAC__stream_encoder_set_channels(enc, 8);
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, 44100);
    FLAC__stream_encoder_set_compression_level(enc, 5);
    FLAC__stream_encoder_set_total_samples_estimate(enc, job->frames);
    FLAC__stream_encoder_set_metadata(enc, &vc, 1);
    /* the encoder owns f from here: finish (or delete) closes it, even after a failed init */
    errno = 0;
    int ok = FLAC__stream_encoder_init_FILE(enc, f, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (ok) {
        enum { BLOCK = 4096 };
        FLAC__int32 buf[BLOCK * 8];
        for (size_t i = 0; ok && i < job->frames; i += BLOCK) {
            size_t n = job->frames - i < BLOCK ? job->frames - i : BLOCK;
            for (size_t j = 0; j < n * 8; j++) buf[j] = job->pcm[i * 8 + j];
            ok = FLAC__stream_encoder_process_interleaved(enc, buf, (unsigned)n);
        }
        ok = FLAC__stream_encoder_finish(enc) && ok;
    }
    *err = ok ? 0 : errno ? errno : EIO;                    /* the write's own errno, as far as it survives */
    test_pause("RBSTEMS_TEST_WRITE_DELAY");                 /* the temp file written, not yet renamed */
    if (ok && test_errno("RBSTEMS_TEST_WRITE_ERRNO")) { ok = 0; *err = test_errno("RBSTEMS_TEST_WRITE_ERRNO"); }
    FLAC__stream_encoder_delete(enc);
    FLAC__metadata_object_delete(vc);
    int e;
    if (ok && (e = plat_rename_at(x, tmp, x, name))) { ok = 0; *err = e; }
    if (ok) {
        pthread_mutex_lock(&lock);
        writes++;
        pthread_mutex_unlock(&lock);
        return 0;
    }
    plat_unlink_at(x, tmp);
    return -1;
}

typedef struct { int16_t *pcm; size_t frames, got; int ok; char key[65]; int key_ok; const unsigned char *b; size_t n, at; } read_ctx;

static FLAC__StreamDecoderReadStatus rd_read(const FLAC__StreamDecoder *d, FLAC__byte buf[], size_t *bytes, void *u) {
    read_ctx *c = u;
    (void)d;
    size_t left = c->n - c->at;
    if (!left) { *bytes = 0; return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM; }
    if (*bytes > left) *bytes = left;
    memcpy(buf, c->b + c->at, *bytes);
    c->at += *bytes;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus rd_write(const FLAC__StreamDecoder *d, const FLAC__Frame *fr,
                                               const FLAC__int32 *const buf[], void *u) {
    read_ctx *c = u;
    (void)d;
    if (fr->header.channels != 8 || fr->header.bits_per_sample != 16) { c->ok = 0; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
    for (unsigned i = 0; i < fr->header.blocksize; i++) {
        if (c->got >= c->frames) { c->ok = 0; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
        for (int ch = 0; ch < 8; ch++) c->pcm[c->got * 8 + ch] = (int16_t)buf[ch][i];
        c->got++;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void rd_meta(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *u) {
    read_ctx *c = u;
    (void)d;
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO && m->data.stream_info.total_samples != c->frames) c->ok = 0;
    if (m->type == FLAC__METADATA_TYPE_VORBIS_COMMENT)
        for (FLAC__uint32 i = 0; i < m->data.vorbis_comment.num_comments; i++) {
            const FLAC__StreamMetadata_VorbisComment_Entry *e = &m->data.vorbis_comment.comments[i];
            if (e->length == 12 + 64 && !memcmp(e->entry, "RBSTEMS_KEY=", 12) && !memcmp(e->entry + 12, c->key, 64)) c->key_ok = 1;
        }
}

static void rd_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *u) {
    (void)d; (void)s;
    ((read_ctx *)u)->ok = 0;
}

/* decodes an entry's FLAC from memory: exactly `frames` 8-channel 16-bit frames, and the key it
 * names; 0 when it is all there */
static int decode_flac(const unsigned char *b, size_t n, const char *key, int16_t *pcm, size_t frames) {
    read_ctx c = {pcm, frames, 0, 1, {0}, 0, b, n, 0};
    memcpy(c.key, key, 65);
    FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
    if (!d) return -1;
    FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_VORBIS_COMMENT);
    int ok = FLAC__stream_decoder_init_stream(d, rd_read, NULL, NULL, NULL, NULL, rd_write, rd_meta, rd_error, &c)
                 == FLAC__STREAM_DECODER_INIT_STATUS_OK
             && FLAC__stream_decoder_process_until_end_of_stream(d);
    FLAC__stream_decoder_finish(d);
    FLAC__stream_decoder_delete(d);
    return ok && c.ok && c.key_ok && c.got == frames ? 0 : -1;
}

/* the most bytes an entry's FLAC of `frames` frames may take: raw 16-bit audio and room for headers */
static int64_t flac_max(size_t frames) { return (int64_t)(frames * 16 + (1 << 20)); }

/* Reads an entry's stems (exact or near): its prefix folder (entry_dir), the FLAC opened with
 * plat_file_open_at, read whole, then decoded. 0 when done; *keep (if given) then receives the
 * open file for the hit's "last used" time (plat_file_touch). Otherwise -1 with *err: ENOENT and
 * the like for a miss; a failure of the location itself has been dealt with (lose). */
static int read_entry(const char *model, const char *key, int16_t *pcm, size_t frames, plat_file *keep, int *err) {
    char name[80];
    int64_t size = 0;
    snprintf(name, sizeof name, "%s.flac", key);
    *err = 0;
    plat_dir x = entry_dir(model, key, 0, err);
    plat_file fd = NO_FILE;
    int r = -1, e;
    if (plat_dir_ok(x)) {
        *err = plat_file_open_at(x, name, flac_max(frames), &size, &fd);
        plat_dir_close(x);
    }
    if (!plat_file_ok(fd)) return -1;
    unsigned char *b = malloc(size ? (size_t)size : 1);
    if (!b) *err = ENOMEM;
    else if ((e = plat_read_all(fd, b, (size_t)size))) *err = e;
    else if (decode_flac(b, (size_t)size, key, pcm, frames)) *err = PLAT_EFTYPE;   /* damaged or not ours: a miss */
    else r = 0;
    free(b);
    if (!r && keep) *keep = fd;
    else plat_file_close(fd);
    return r;
}

/* After a failed read or write of an entry: an I/O error may be the drive going. The location
 * is checked again (lose if it fails); otherwise it was that entry only, a miss. */
static void entry_failed(int err) {
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    if (chosen && (err == EIO || err == ENXIO || err == ENODEV)) lose("io-error");
#else
    if (chosen && (err == EIO || err == ENXIO || err == ENODEV)) location_recheck();
#endif
}

/* ---------------------------------------------------------------- tensors */

typedef struct { float *data; int64_t dims[8]; size_t ndim, count; } tensor;

static int view(const OrtValue *v, tensor *t) {
    OrtTensorTypeAndShapeInfo *info = NULL;
    if (R->GetTensorTypeAndShape(v, &info)) return -1;
    ONNXTensorElementDataType type;
    int bad = R->GetTensorElementType(info, &type) || type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
              || R->GetDimensionsCount(info, &t->ndim) || t->ndim > 8 || R->GetDimensions(info, t->dims, t->ndim);
    R->ReleaseTensorTypeAndShapeInfo(info);
    if (bad) return -1;
    t->count = 1;
    for (size_t i = 0; i < t->ndim; i++) t->count *= (size_t)t->dims[i];
    void *p = NULL;
#if defined(_WIN32)
    if (!ortw_on_cpu(R, v)) return -1;                      /* never a GPU pointer (BW1) */
#endif
    if (R->GetTensorMutableData((OrtValue *)v, &p)) return -1;
    t->data = p;
    return 0;
}

static int index_of(const char *const *names, size_t n, const char *want) {
    for (size_t i = 0; i < n; i++)
        if (names[i] && !strcmp(names[i], want)) return (int)i;
    return -1;
}

static void mean_std(const tensor *mix, double *mean, double *std) {
    double s = 0, ss = 0;
    for (size_t i = 0; i < mix->count; i++) s += mix->data[i];
    *mean = s / mix->count;
    for (size_t i = 0; i < mix->count; i++) { double d = mix->data[i] - *mean; ss += d * d; }
    *std = sqrt(ss / (mix->count - 1));                     /* unbiased, like torch.std */
}

/* our model (x all zero) returns NaN for constant input: rekordbox gets 0 instead (it then
 * rebuilds mean, which is right for constant input). Returns 1 if x was zero. */
static int sanitize_ours(OrtValue **outputs, int ix, int ixt) {
    tensor tx, txt;
    if (view(outputs[ix], &tx) || view(outputs[ixt], &txt)) return 0;
    for (size_t i = 0; i < tx.count; i++)
        if (tx.data[i] != 0.0f) return 0;
    for (size_t i = 0; i < txt.count; i++)
        if (!isfinite(txt.data[i])) txt.data[i] = 0.0f;
    return 1;
}

/* ---------------------------------------------------------------- rebuilding rekordbox's stems */

#define NFFT 4096
#define HOP 1024
#define BINS 2048                       /* NFFT / 2: the model's bins (the top one is dropped) */

/* The four stems' spectrogram part of one chunk, as rekordbox rebuilds it (Demucs's
 * HDemucs._ispec): x [4 sources][4: L.re L.im R.re R.im][BINS][F] -> out [4][2 channels][L],
 * before the time branch (xt * std + mean) is added. Inverse STFT: NFFT 4096, hop 1024, periodic
 * Hann, "normalized" (x sqrt(NFFT)), centred; the top bin and two empty frames on each side
 * restored, then the chunk's own samples taken out. Checked against torch.istft (max 2.4e-7) and
 * inside rekordbox (a null test: its stems played silence). 0 when done. */
static int rebuild_spec(const float *x, size_t L, size_t F, float *out) {
    const size_t pad = HOP / 2 * 3, start = NFFT / 2 + pad;  /* centre trim, then Demucs's padding */
    if (F != (L + HOP - 1) / HOP) return 1;
    const size_t frames = F + 4, full = NFFT + HOP * (frames - 1);
    if (start + L > full) return 1;
#if defined(_WIN32)
    plat_irfft *setup = plat_irfft_new(12);                 /* pocketfft: vDSP's packing and sum (irfft_cmp.c) */
#else
    FFTSetup setup = vDSP_create_fftsetup(12, kFFTRadix2);    /* per call: no shared state between threads */
#endif
    float *win = malloc(sizeof(float) * NFFT), *env = calloc(full, sizeof(float)), *y = malloc(sizeof(float) * full);
    float *re = malloc(sizeof(float) * BINS), *im = malloc(sizeof(float) * BINS), *frame = malloc(sizeof(float) * NFFT);
    int ok = setup && win && env && y && re && im && frame;
    if (ok) {
        for (int n = 0; n < NFFT; n++) win[n] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / NFFT);
        for (size_t t = 0; t < frames; t++)              /* the window envelope covers every frame, empty ones too */
            for (int n = 0; n < NFFT; n++) env[t * HOP + n] += win[n] * win[n];
        /* vDSP's inverse real FFT returns the plain sum (no 1/NFFT): times sqrt(NFFT) for
         * "normalized", over NFFT for the inverse DFT */
        const float scale = (float)(sqrt((double)NFFT) / NFFT);
#if !defined(_WIN32)
        DSPSplitComplex sc = {re, im};
#endif
        for (int s = 0; s < 4; s++)
            for (int c = 0; c < 2; c++) {
                memset(y, 0, sizeof(float) * full);
                const float *xr = x + (size_t)(s * 4 + 2 * c) * BINS * F, *xi = x + (size_t)(s * 4 + 2 * c + 1) * BINS * F;
                for (size_t f = 0; f < F; f++) {
                    /* packed: re[0] = DC, im[0] = Nyquist (the restored top bin, zero) */
                    for (size_t k = 0; k < BINS; k++) { re[k] = xr[k * F + f]; im[k] = xi[k * F + f]; }
                    im[0] = 0.0f;
#if defined(_WIN32)
                    plat_irfft_run(setup, re, im, frame);
#else
                    vDSP_fft_zrip(setup, &sc, 1, 12, kFFTDirection_Inverse);
                    vDSP_ztoc(&sc, 1, (DSPComplex *)frame, 2, BINS);
#endif
                    const size_t off = (f + 2) * HOP;
                    for (int n = 0; n < NFFT; n++) y[off + n] += frame[n] * scale * win[n];
                }
                float *o = out + (size_t)(s * 2 + c) * L;
                for (size_t n = 0; n < L; n++) o[n] = y[start + n] / env[start + n];
            }
    }
#if defined(_WIN32)
    if (setup) plat_irfft_free(setup);
#else
    if (setup) vDSP_destroy_fftsetup(setup);
#endif
    free(win); free(env); free(y); free(re); free(im); free(frame);
    return ok ? 0 : 1;
}

/* rekordbox's own models whose stems are rebuilt (see the top of the file). A test build also
 * takes RBSTEMS_TEST_PIONEER_MODEL. */
static int is_pioneer(const char *fp) {
    for (size_t i = 0; i < sizeof pioneer_models / sizeof *pioneer_models; i++)
        if (!strcmp(fp, pioneer_models[i])) return 1;
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_PIONEER_MODEL");
    if (t && !strcmp(fp, t)) return 1;
#endif
    return 0;
}

/* A rebuilt model's answer as the stems rekordbox makes of it, rebuild_spec(x) + xt * std + mean,
 * quantised the way the cache stores stems (pcm, 8 channels interleaved, half level). Never
 * writes to the model's outputs. Nonzero, and logged, when the answer can't be cached: shapes
 * other than rekordbox's, values that aren't finite, or stems beyond what the cache can hold. */
static int rebuilt_pcm(const tensor *tx, const tensor *txt, const tensor *mag, size_t L, double mean, double std,
                       int16_t *pcm, const char *key) {
    size_t F = mag->ndim == 4 ? (size_t)mag->dims[3] : 0;
    if (mag->ndim != 4 || mag->dims[0] != 1 || mag->dims[1] != 4 || mag->dims[2] != BINS || tx->ndim != 5
        || tx->dims[0] != 1 || tx->dims[1] != 4 || tx->dims[2] != 4 || tx->dims[3] != BINS || (size_t)tx->dims[4] != F
        || F != (L + HOP - 1) / HOP || txt->count != L * 8) {
        logf_("not cached %.12s: unexpected shapes for a rebuilt model (x %zu values, %zu frames, L %zu)", key, tx->count, F, L);
        return -1;
    }
    float *s = malloc(sizeof(float) * 8 * L);
    if (!s || rebuild_spec(tx->data, L, F, s)) {
        free(s);
        logf_("not cached %.12s: the rebuild failed", key);
        return -1;
    }
    size_t clamped = 0;
    int finite = 1;
    for (size_t ch = 0; ch < 8 && finite; ch++)
        for (size_t i = 0; i < L; i++) {
            double v = ((double)s[ch * L + i] + txt->data[ch * L + i] * std + mean) * 0.5 * 32767.0;
            if (!isfinite(v)) { finite = 0; break; }
            if (v > 32767 || v < -32767) { clamped++; v = v > 0 ? 32767 : -32767; }
            pcm[i * 8 + ch] = (int16_t)lrint(v);
        }
    free(s);
    if (!finite) { logf_("not cached %.12s: rebuilt stems aren't finite", key); return -1; }
    if (clamped) { logf_("not cached %.12s: %zu rebuilt samples beyond the cache's range", key, clamped); return -1; }
    return 0;
}

/* ---------------------------------------------------------------- the hooks */

/* Our models, by the SHA-256 of the file: opened with ONNX Runtime's memory pattern off. Measured
 * with rekordbox's ORT 1.18, one 485,100-sample chunk, 4 threads: peak 2.26 GB with it, 1.75 GB
 * without, same speed. Compiled in, never read from a file. The checksum itself may come from the
 * memo in the cache folder, so a forged memo could change another model's memory plan (never its
 * output), or have another model's stems rebuilt and cached: no more than writing wrong entries
 * into the cache folder directly, which the same account can always do, and rebuilt stems that
 * don't add up to the chunk are refused anyway (store_entry). A test build also takes
 * RBSTEMS_TEST_OUR_MODEL. */
static const char *const our_models[] = {
    "0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772",
};

static int is_ours(const char *fp) {
    for (size_t i = 0; i < sizeof our_models / sizeof *our_models; i++)
        if (!strcmp(fp, our_models[i])) return 1;
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_OUR_MODEL");
    if (t && !strcmp(fp, t)) return 1;
#endif
    return 0;
}

/* our model: a copy of rekordbox's options with the memory pattern off; if anything about that
 * fails, and for any other model, rekordbox's options unchanged. *how says which. */
static OrtStatus *create_demucs(const OrtEnv *env, const ORTCHAR_T *path, const OrtSessionOptions *options,
                                OrtSession **out, const char *fp, const char **how) {
    *how = "rekordbox's session options";
    if (!options || !is_ours(fp)) return R->CreateSession(env, path, options, out);
    OrtSessionOptions *o = NULL;
    OrtStatus *st = R->CloneSessionOptions(options, &o);
#ifdef RBSTEMS_TEST_REAL_PATH
    if (!st && getenv("RBSTEMS_TEST_CLONE_FAIL")) st = R->CreateStatus(ORT_FAIL, "test failure");
#endif
    if (!st) st = R->DisableMemPattern(o);
    if (!st) st = R->CreateSession(env, path, o, out);      /* the session keeps its own copy of o */
    if (o) R->ReleaseSessionOptions(o);
    if (!st) {
        *how = "memory pattern off";
        return NULL;
    }
    logf_("model %.12s: memory pattern off failed (%s), opening it with rekordbox's session options", fp, R->GetErrorMessage(st));
    R->ReleaseStatus(st);
    return R->CreateSession(env, path, options, out);
}

static OrtStatus *ORT_API_CALL hook_CreateSession(const OrtEnv *env, const ORTCHAR_T *model_path,
                                                  const OrtSessionOptions *options, OrtSession **out) {
#if defined(_WIN32)
    ew_phase2_now();                                        /* the core only once started (BW2) */
    if (!ew_phase2_done) return R->CreateSession(env, model_path, options, out);
    /* the wide path's tail, either separator (ort_win.h), and the path in UTF-8 for the reads and the log */
    char model8[PATH_MAX];
    if (!ortw_is_demucs16((const uint16_t *)model_path)
        || wr_utf16_to_8((const uint16_t *)model_path, wcslen(model_path), model8, sizeof model8) < 0)
        return R->CreateSession(env, model_path, options, out);
#define MODEL_PATH8 model8
#else
    size_t len = strlen(model_path);
    if (len < 13 || strcmp(model_path + len - 13, "/hdemucs.onnx")) return R->CreateSession(env, model_path, options, out);
#define MODEL_PATH8 model_path
#endif
    char fp[65];
    const char *how;
    /* hashed first: the checksum picks the options (the memo keeps it cheap) */
    if (file_sha256(MODEL_PATH8, fp)) {
        logf_("ERROR cannot read %s", MODEL_PATH8);
        return R->CreateSession(env, model_path, options, out);
    }
    OrtStatus *st = create_demucs(env, model_path, options, out, fp, &how);
    int cached = !st && enabled && base_dir[0];            /* a chosen location may come back later */
    /* rekordbox's own model: rebuilt and cached only where this account turned it on */
    int pioneer = is_pioneer(fp), rebuild = pioneer && rekordbox_model;
    char id[65];
    if (!rebuild) memcpy(id, fp, 65);
    else if (rebuilt_id(fp, id)) cached = 0;                /* no sha256 (Windows only): not cached */
    if (!st) logf_("Demucs session opened: model %.12s, %s%s%s", fp, how, cached ? "" : ", not cached",
                   !cached || !pioneer ? "" : rebuild ? ", rekordbox's own: stems rebuilt" : ", rekordbox's own: not cached here (rekordbox_model=0)");
    if (!cached) return st;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!sessions[i].s) { sessions[i].s = *out; memcpy(sessions[i].model, id, 65); sessions[i].rebuild = rebuild; break; }
    pthread_mutex_unlock(&lock);
    /* off rekordbox's threads, after a chosen location's check: this model's summaries, then
     * (once per load) the limits; a location that comes back later is counted by recheck */
    char *model = strdup(id);
    pthread_mutex_lock(&lock);
    index_loading = 1;                                      /* hook_Run doesn't queue another meanwhile */
    pthread_mutex_unlock(&lock);
    if (plat_async_b(maint, ^{
        static char loaded[65];                             /* the maintenance queue's own: once per load */
        static int pruned;
        long n = -1;
        if (!near_on) {                                     /* no index: the limits only */
            pthread_mutex_lock(&lock);
            index_loading = 0;
            int ok = model && loc_state == LOC_AVAILABLE;
            pthread_mutex_unlock(&lock);
            logf_(ok ? "summaries not loaded: near hits are off" : "summaries not loaded: %s isn't available", base_dir);
            if (!pruned && ok) { pruned = 1; first_prune(0); }
            free(model);
            return;
        }
        if (model && strcmp(loaded, model)) {
            if ((n = load_index(model)) >= 0) memcpy(loaded, model, 65);
        } else {                                            /* another session of a model already loaded */
            pthread_mutex_lock(&lock);
            index_loading = 0;
            if (model && loc_state == LOC_AVAILABLE) n = (long)index_n;
            pthread_mutex_unlock(&lock);
        }
        if (n >= 0) logf_("summaries loaded: %ld cached chunks", n);
        else logf_("summaries not loaded: %s isn't available", base_dir);
        if (!pruned && n >= 0) { pruned = 1; first_prune(0); }
        free(model);
    })) {
        free(model);                                        /* not queued (never on the Mac) */
        pthread_mutex_lock(&lock);
        index_loading = 0;
        pthread_mutex_unlock(&lock);
    }
    return st;
}

static void ORT_API_CALL hook_ReleaseSession(OrtSession *s) {
#if defined(_WIN32)
    /* BX1: at exit, ExitProcess has killed the other threads, one perhaps inside `lock` (an
     * SRWLOCK, no owner): don't take it. A stale sessions[] entry costs nothing then; a
     * mid-process FreeLibrary (loader lock held, threads alive) still clears its entry */
    if (ew_shutdown && ew_shutdown()) {
        R->ReleaseSession(s);
        return;
    }
    ew_phase2_now();                                        /* BW2 */
    if (!ew_phase2_done) {
        R->ReleaseSession(s);
        return;
    }
#endif
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].s == s) sessions[i].s = NULL;
    pthread_mutex_unlock(&lock);
    R->ReleaseSession(s);
}

static OrtValue *new_tensor(const int64_t *dims, size_t ndim) {
    OrtAllocator *a = NULL;
    OrtValue *v = NULL;
    if (R->GetAllocatorWithDefaultOptions(&a)) return NULL;
    if (R->CreateTensorAsOrtValue(a, dims, ndim, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v)) return NULL;
    return v;
}

/* fills the outputs from cached pcm, shifted by d samples; samples the shifted stems don't
 * cover go to "other" (the mix there), so the stems still add up to the mix; 0 when done */
static int serve(const int16_t *pcm, const float *mix, size_t L, long d, double mean, double std, const int64_t *xdims,
                 const int64_t *xtdims, int ix, int ixt, OrtValue **outputs) {
    OrtValue *ox = outputs[ix] ? outputs[ix] : new_tensor(xdims, 5);
    OrtValue *oxt = outputs[ixt] ? outputs[ixt] : new_tensor(xtdims, 3);
    tensor tx, txt;
    if (ox && oxt && !view(ox, &tx) && !view(oxt, &txt) && tx.count == (size_t)(xdims[1] * xdims[2] * xdims[3] * xdims[4])
        && txt.count == L * 8) {
        memset(tx.data, 0, tx.count * sizeof(float));
        for (size_t ch = 0; ch < 8; ch++)
            for (size_t i = 0; i < L; i++) {
                long k = (long)i - d;
                double v;
                if (k >= 0 && k < (long)L) v = pcm[(size_t)k * 8 + ch] / 32767.0 * 2.0;
                else v = (ch == 4 || ch == 5) ? mix[(ch - 4) * L + i] : 0.0;     /* other L/R = mix */
                txt.data[ch * L + i] = (float)((v - mean) / std);
            }
        outputs[ix] = ox;
        outputs[ixt] = oxt;
        return 0;
    }
    if (!outputs[ix] && ox) R->ReleaseValue(ox);
    if (!outputs[ixt] && oxt) R->ReleaseValue(oxt);
    return -1;
}

/* The writer queue is bounded: past WRITER_JOBS_MAX jobs or WRITER_BYTES_MAX bytes waiting (each
 * holds a chunk's stems and mix, about 11.6 MB for rekordbox's), new cache work is dropped (the
 * stems still reach rekordbox), logged once until the queue empties. */
#define WRITER_JOBS_MAX 4
#define WRITER_BYTES_MAX (64LL << 20)
static int writer_jobs, writer_full;                        /* under lock */
static long long writer_bytes;

static int writer_admit(long long bytes) {
    pthread_mutex_lock(&lock);
    int ok = writer_jobs < WRITER_JOBS_MAX && writer_bytes + bytes <= WRITER_BYTES_MAX, say = !ok && !writer_full;
    if (ok) { writer_jobs++; writer_bytes += bytes; }
    else writer_full = 1;
    pthread_mutex_unlock(&lock);
    if (say) logf_("cache: %d chunks are waiting to be saved: new stems aren't saved until they are", WRITER_JOBS_MAX);
    return ok;
}

static void writer_done(long long bytes) {
    pthread_mutex_lock(&lock);
    writer_bytes -= bytes;
    if (--writer_jobs == 0) writer_checked = writer_full = 0;   /* the next job starts a new batch */
    pthread_mutex_unlock(&lock);
}

/* the background work of a new entry or an RBD1 upgrade: summary, r0/g0 and residuals from the
 * stored pcm and the chunk, then (if write_pcm) the FLAC, then the .desc and the index. Rebuilt
 * stems (rebuilt) that don't add up to the chunk within REBUILT_RESIDUAL_MAX on either channel
 * aren't stored: a rebuild that doesn't match the model never reaches the cache. Chunks quieter
 * than FRAME_FLOOR (a track's silent end) aren't judged: there the residual means nothing.
 * The running total changes by the entry's disk space before and after (entry_size), whatever
 * happened; a chosen location's marker is checked at the first write of each batch. */
static void store_entry(const char *model, const char *key, int16_t *pcm, float *mix, size_t L, int write_pcm, int rebuilt) {
#ifdef RBSTEMS_TEST_REAL_PATH
    if (getenv("RBSTEMS_TEST_WRITER_DELAY")) usleep((useconds_t)(atof(getenv("RBSTEMS_TEST_WRITER_DELAY")) * 1e6));
#endif
    if (loc_now() != LOC_AVAILABLE) return;
#if !defined(_WIN32) && !defined(RBSTEMS_WIN_CACHE_TEST)
    pthread_mutex_lock(&lock);
    int check = chosen && !writer_checked;
    writer_checked = 1;
    pthread_mutex_unlock(&lock);
    if (check && location_recheck()) return;
#endif
    if (!space_ok()) return;                                /* under the floor: nothing written */
    int frames = 0;
    float *d = malloc(sizeof(float) * 2 * DESC_MAX_FRAMES), *s = malloc(L * sizeof(float)), *m = malloc(L * sizeof(float));
    float *rf = malloc(sizeof(float) * 2 * DESC_MAX_FRAMES), rt[2];
    double r = 0, g = 0;
    int ok = d && s && m && rf;
    if (ok) {
        frames = summarize(mix, L, d);
        double sse = 0, mme = 0;
        monos(pcm, mix, L, s, m);
        for (size_t i = 0; i < L; i++) { sse += (double)s[i] * s[i]; mme += (double)m[i] * m[i]; }
        corr_at(s, m, L, 0, sse, mme, &r, &g);
        residual(pcm, mix, L, 0, frames, rf, rt, NULL);
        ok = isfinite(r) && isfinite(g) && all_finite(d, 2 * (size_t)frames) && all_finite(rf, 2 * (size_t)frames);
        double me = 0;
        for (size_t i = 0; i < 2 * L; i++) me += (double)mix[i] * mix[i];
        if (ok && rebuilt && me / (2.0 * L) >= FRAME_FLOOR && !(rt[0] <= REBUILT_RESIDUAL_MAX && rt[1] <= REBUILT_RESIDUAL_MAX)) {
            logf_("not cached %.12s: rebuilt stems don't add up to the chunk (residual L %.1f R %.1f dB, at most %.0f)",
                  key, rt[0], rt[1], REBUILT_RESIDUAL_MAX);
            ok = 0;
        } else if (ok && chosen && me / (2.0 * L) >= FRAME_FLOOR && !(rt[0] <= EXACT_RESIDUAL_MAX && rt[1] <= EXACT_RESIDUAL_MAX)) {
            /* exact_adds_up would refuse it on every hit: written, it would only be rewritten */
            logf_("not cached %.12s: its stems don't add up to the chunk (residual L %.1f R %.1f dB, at most %.0f on another drive)",
                  key, rt[0], rt[1], EXACT_RESIDUAL_MAX);
            ok = 0;
        }
    }
    int err = 0;
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    plat_dir b = NO_DIR, md = NO_DIR, x = NO_DIR;
    if (ok) {
        b = chosen ? open_base(0, &err) : open_local_cache(&err);
        if (plat_dir_ok(b)) {
            plat_stat st;
            err = plat_dir_stat(b, &st);
            if (!err && !mutation_allowed(b, st.id)) err = chosen ? EACCES : ENOTEMPTY;
            if (!err) x = mutation_entry_dir(b, st.id.vol, model, key, 1, &md, &err);
        }
        if (!chosen && err && !plat_dir_ok(x)) local_cache_status(err);
    }
#else
    plat_dir x = ok ? entry_dir(model, key, 1, &err) : NO_DIR;
#endif
    int stored = 0;
    if (plat_dir_ok(x)) {
        long long before = entry_size(x, key);
        write_job job = {pcm, L};
        stored = (!write_pcm || write_flac(x, key, &job, &err) == 0)
                 && write_desc(x, key, d, frames, (float)r, (float)g, rt, rf, &err) == 0;
        long long after = entry_size(x, key);
        plat_dir_close(x);
        int over;
        pthread_mutex_lock(&lock);
        if (total_bytes >= 0) total_bytes += after - before;
        if (prune_running) prune_pending += after - before;
        over = total_bytes > max_bytes;
        pthread_mutex_unlock(&lock);
        if (stored) add_to_index(model, key, d, frames, (float)r, (float)g, rt, rf);
        if (over) queue_prune();
    }
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    plat_dir_close(md);
    plat_dir_close(b);
#endif
    if (ok && !stored) {
        logf_("ERROR writing %s/%s/%.2s/%s: %s", base_dir, model, key, key, strerror(err));
        if (err == EROFS) lose("read-only");
        else if (chosen && (err == EACCES || err == EPERM)) lose("no-permission");
        else entry_failed(err);
    }
    free(d); free(s); free(m); free(rf);
}
#ifdef RBSTEMS_TEST_TIMING
static void store_entry_timed(const char *model, const char *key, int16_t *pcm, float *mix, size_t L, int write_pcm, int rebuilt) {
    uint64_t t0 = t_now();
    store_entry(model, key, pcm, mix, L, write_pcm, rebuilt);
    uint64_t t1 = t_now();
    logf_("timing store pcm=%d total_ns=%llu", write_pcm, (unsigned long long)(t1 - t0));
}
#define store_entry(...) store_entry_timed(__VA_ARGS__)
#endif

/* whether an entry's .desc is a whole, valid RBD2 (an older or damaged one is rewritten on a hit) */
static int desc_is_rbd2(const char *model, const char *key) {
    summary sm;
    int err, ok = 0;
    plat_dir x = entry_dir(model, key, 0, &err);
    if (!plat_dir_ok(x)) return 0;
    if (read_desc(x, key, &sm) == 0) {
        ok = sm.version == 2;
        free_summary(&sm);
    }
    plat_dir_close(x);
    return ok;
}

/* An exact hit from a chosen location must add up to its chunk (EXACT_RESIDUAL_MAX on each
 * channel), unless the chunk is under FRAME_FLOOR (a track's silent end, as store_entry). */
static int exact_adds_up(const int16_t *pcm, const float *mix, size_t L) {
    double me = 0;
    for (size_t i = 0; i < 2 * L; i++) me += (double)mix[i] * mix[i];
    if (me / (2.0 * L) < FRAME_FLOOR) return 1;
    int frames = (int)(L / DESC_FRAME);
    if (frames > DESC_MAX_FRAMES) frames = DESC_MAX_FRAMES;
    float *rf = malloc(sizeof(float) * 2 * (size_t)frames), rt[2];
    if (!rf) return 0;
    residual(pcm, mix, L, 0, frames, rf, rt, NULL);
    free(rf);
    return rt[0] <= EXACT_RESIDUAL_MAX && rt[1] <= EXACT_RESIDUAL_MAX;
}

static OrtStatus *ORT_API_CALL hook_Run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names,
                                        const OrtValue *const *inputs, size_t n_in, const char *const *out_names,
                                        size_t n_out, OrtValue **outputs) {
#if defined(_WIN32)
    ew_phase2_now();                                        /* BW2 */
    if (!ew_phase2_done) return R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
#endif
    int rebuild = 0;
    const char *model = demucs_model(s, &rebuild);
#if defined(_WIN32)
    /* DirectML 1 (bridge.md §3, BW1): only CPU memory is read or written. A Demucs run with any
     * input or pre-allocated output elsewhere (DirectML's GPU memory) is passed through untouched:
     * nothing is cached or served, and our model's NaN for silence reaches rekordbox as it is */
    if (model && (!ortw_all_on_cpu(R, inputs, n_in) || !ortw_all_on_cpu(R, (const OrtValue *const *)outputs, n_out))) {
        static volatile LONG said;
        if (InterlockedExchange(&said, 1) == 0)
            logf_("cache: a Demucs run has tensors outside CPU memory (DirectML): passed through, not cached");
        return R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
    }
#endif
    int im = model ? index_of(in_names, n_in, "mix") : -1, ig = model ? index_of(in_names, n_in, "mag") : -1;
    int ix = model ? index_of(out_names, n_out, "x") : -1, ixt = model ? index_of(out_names, n_out, "xt") : -1;
    tensor mix, mag;
    if (!model || im < 0 || ig < 0 || ix < 0 || ixt < 0 || n_out != 2 || view(inputs[im], &mix) || view(inputs[ig], &mag)
        || mix.ndim != 3 || mix.dims[0] != 1 || mix.dims[1] != 2 || mag.ndim != 4)
        return R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
    size_t L = (size_t)mix.dims[2];
    double mean, std;
    mean_std(&mix, &mean, &std);
    /* silence or broken input: never cached; nor anything while a chosen location is pending or
     * lost (no drive I/O). Our model's NaN for silence becomes 0. */
    if (!isfinite(mean) || !isfinite(std) || std <= 0 || L < DESC_FRAME || loc_now() != LOC_AVAILABLE) {
#if defined(_WIN32)
        if (loc_now() != LOC_AVAILABLE) arm_recheck();      /* BX2: a recheck plat_after_b couldn't queue */
#endif
        OrtStatus *st = R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
        if (!st) sanitize_ours(outputs, ix, ixt);
        return st;
    }

    /* the exact key: format, model, shapes and the audio itself */
    unsigned char dg[32];
    char key[65];
    plat_sha256_ctx c;
    int keyed = plat_sha256_init(&c) == 0;
    if (keyed) {
        plat_sha256_update(&c, FORMAT, sizeof FORMAT);
        plat_sha256_update(&c, model, 64);
        plat_sha256_update(&c, mix.dims, sizeof(int64_t) * 3);
        plat_sha256_update(&c, mag.dims, sizeof(int64_t) * 4);
        plat_sha256_update(&c, mix.data, mix.count * sizeof(float));
        keyed = plat_sha256_final(&c, dg) == 0;
    }
    if (!keyed) {                                           /* no sha256 (Windows only): passed through */
        OrtStatus *st = R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
        if (!st) sanitize_ours(outputs, ix, ixt);
        return st;
    }
    hex(dg, 32, key);
#ifdef RBSTEMS_DUMP_DIR
    /* research builds only (bridge/build.sh with RBSTEMS_DUMP_DIR set; never a release): the
     * chunk rekordbox sends, as raw float32, <ms since 1970>-<key>.mix (2 x L, planar) and .mag */
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        long long ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        const tensor *t[2] = {&mix, &mag};
        const char *ext[2] = {"mix", "mag"};
        mkdir(RBSTEMS_DUMP_DIR, 0777);
        for (int k = 0; k < 2; k++) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%013lld-%s.%s", RBSTEMS_DUMP_DIR, ms, key, ext[k]);
            FILE *f = fopen(p, "wb");
            if (!f) { logf_("dump: can't write %s (%s)", p, strerror(errno)); continue; }
            size_t n = fwrite(t[k]->data, sizeof(float), t[k]->count, f);
            if (fclose(f) || n != t[k]->count) logf_("dump: %s is incomplete", p);
        }
        logf_("dump: %.12s (%zu samples)", key, L);
    }
#endif
    int64_t xdims[5] = {1, 4, mag.dims[1], mag.dims[2], mag.dims[3]}, xtdims[3] = {1, 8, (int64_t)L};
    int16_t *pcm = malloc(L * 8 * sizeof(int16_t));
    float *desc = malloc(sizeof(float) * 2 * DESC_MAX_FRAMES);
    float *ms = malloc(L * sizeof(float)), *mm = malloc(L * sizeof(float));
    if (!pcm || !desc || !ms || !mm) {
        free(pcm); free(desc); free(ms); free(mm);
        OrtStatus *st = R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
        if (!st) sanitize_ours(outputs, ix, ixt);
        return st;
    }
    int frames = summarize(mix.data, L, desc);

    /* 1. exact */
#ifdef RBSTEMS_TEST_REAL_PATH
    static int test_hit_used;
    if (!test_hit_used && test_errno("RBSTEMS_TEST_HIT_OPEN_ERRNO")) { test_hit_used = 1; test_open_errno = test_errno("RBSTEMS_TEST_HIT_OPEN_ERRNO"); }
#endif
    int err = 0;
    plat_file touch = NO_FILE;
    int got = read_entry(model, key, pcm, L, &touch, &err) == 0;
    if (!got) entry_failed(err);
    if (got && chosen && !exact_adds_up(pcm, mix.data, L)) {
        logf_("hit  %.12s refused: its stems don't add up to the chunk (a damaged or wrong entry): a miss", key);
        got = 0;
    }
    if (got && serve(pcm, mix.data, L, 0, mean, std, xdims, xtdims, ix, ixt, outputs) == 0) {
        plat_file_touch(touch);                             /* last used: now */
        plat_file_close(touch);
        pthread_mutex_lock(&lock);
        long long h = ++hits, nh = near_hits, m = misses;
        pthread_mutex_unlock(&lock);
        logf_("hit  %.12s (%zu samples)  [%lld hits, %lld near, %lld misses]", key, L, h, nh, m);
        if (near_on && !desc_is_rbd2(model, key)) {         /* an older or damaged entry: (re)write its summary */
            float *mixc = malloc(mix.count * sizeof(float));
            char *kk = strdup(key), *mdl = strdup(model);
            long long bytes = (long long)(L * 8 * sizeof(int16_t) + mix.count * sizeof(float));
            if (mixc && kk && mdl && writer_admit(bytes)) {
                memcpy(mixc, mix.data, mix.count * sizeof(float));
                int16_t *pc = pcm;
                if (!plat_async_b(writer, ^{
                        store_entry(mdl, kk, pc, mixc, L, 0, 0);
                        free(pc); free(mixc); free(kk); free(mdl);
                        writer_done(bytes);
                    })) pcm = NULL;
                else { free(mixc); free(kk); free(mdl); writer_done(bytes); }   /* not queued (never on the Mac) */
            } else {
                free(mixc); free(kk); free(mdl);
            }
        }
        free(pcm); free(desc); free(ms); free(mm);
        TIMING(t_kind = "hit";)
        return NULL;
    }
    plat_file_close(touch);

    /* 2. similar, verified per channel and frame; the index is reloaded off rekordbox's threads
     * (pre-cached entries, other Macs' and accounts' entries written since). Off: none. */
    pthread_mutex_lock(&lock);
    int stale = near_on && !index_loading && (strcmp(index_model, model) || difftime(time(NULL), index_time) > reload_every);
    pthread_mutex_unlock(&lock);
    if (stale) queue_index_load(model);
    summary cands[NEAR_CANDIDATES];
    int nc = near_on ? nearest(model, desc, frames, cands) : 0;
    double mmix = 0;
    for (size_t i = 0; i < L; i++) { float v = 0.5f * (mix.data[i] + mix.data[L + i]); mm[i] = v; mmix += (double)v * v; }
    for (int k = 0; k < nc; k++) {
        const char *why = NULL;
        long d = 0;
        double r = 0, g = 0, worst = 0, t0 = 0, t1 = 0;
        touch = NO_FILE;
        if (!cands[k].rt || !cands[k].rf || read_entry(model, cands[k].key, pcm, L, &touch, &err)) {
            why = "unreadable";
            if (cands[k].rt && cands[k].rf) entry_failed(err);
        } else {
            monos(pcm, mix.data, L, ms, mm);
            double sse = 0;
            for (size_t i = 0; i < L; i++) sse += (double)ms[i] * ms[i];
            d = best_shift(ms, mm, L, sse, mmix);
            corr_at(ms, mm, L, d, sse, mmix, &r, &g);
            if (labs(d) > SHIFT_ACCEPT) why = "shift";
            else if (!(fabs(g - cands[k].g0) <= GAIN_SLACK)) why = "level";
            else if (!residual_ok(pcm, mix.data, L, d, &cands[k], &worst, &t0, &t1)) why = "residual";
        }
        if (!why && serve(pcm, mix.data, L, d, mean, std, xdims, xtdims, ix, ixt, outputs) == 0) {
            plat_file_touch(touch);
            plat_file_close(touch);
            pthread_mutex_lock(&lock);
            long long h = hits, nh = ++near_hits, m = misses;
            pthread_mutex_unlock(&lock);
            logf_("near %.12s for %.12s: shift %ld, level %.3f (cached %.3f), residual L %.1f R %.1f dB (cached %.1f %.1f, worst frame %+.1f dB to its bar)  [%lld hits, %lld near, %lld misses]",
                  cands[k].key, key, d, g, cands[k].g0, t0, t1, cands[k].rt[0], cands[k].rt[1], worst, h, nh, m);
            for (int j = 0; j < nc; j++) free_summary(&cands[j]);
            free(pcm); free(desc); free(ms); free(mm);
            TIMING(t_kind = "near";)
            return NULL;
        }
        plat_file_close(touch);
        logf_("near %.12s rejected (%s) for %.12s: shift %ld, level %.3f (cached %.3f), residual L %.1f R %.1f dB, worst frame %+.1f dB over its bar",
              cands[k].key, why ? why : "serve", key, d, g, cands[k].g0, t0, t1, worst);
    }
    for (int j = 0; j < nc; j++) free_summary(&cands[j]);

    /* 3. the model, then cache its stems and the chunk's summary in the background */
    TIMING(t_kind = "miss";)
    OrtStatus *st = R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
    tensor tx, txt;
    free(ms); free(mm); free(desc);
    if (st || view(outputs[ix], &tx) || view(outputs[ixt], &txt) || txt.count != L * 8) {
        free(pcm);
        return st;
    }
    int xzero = 1;
    for (size_t i = 0; i < tx.count && xzero; i++) xzero = tx.data[i] == 0.0f;
    if (!xzero && !rebuild) {                                   /* neither ours nor a rebuilt model: don't cache */
        free(pcm);
        return st;
    }
    if (!xzero) {
        /* a rebuilt model: rekordbox gets its answer untouched; the cache gets the stems rebuilt */
        if (rebuilt_pcm(&tx, &txt, &mag, L, mean, std, pcm, key)) {
            free(pcm);
            return st;
        }
    } else {
        int finite = 1;
        for (size_t i = 0; i < txt.count; i++)
            if (!isfinite(txt.data[i])) { txt.data[i] = 0.0f; finite = 0; }
        if (!finite) {                                          /* never cache NaN stems */
            logf_("model returned non-finite stems for %.12s: not cached", key);
            free(pcm);
            return st;
        }
        for (size_t ch = 0; ch < 8; ch++)
            for (size_t i = 0; i < L; i++) {
                double v = (txt.data[ch * L + i] * std + mean) * 0.5 * 32767.0;
                pcm[i * 8 + ch] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : lrint(v));
            }
    }
    int rebuilt = !xzero;
    float *mixc = malloc(mix.count * sizeof(float));
    char *kk = strdup(key), *mdl = strdup(model);
    long long bytes = (long long)(L * 8 * sizeof(int16_t) + mix.count * sizeof(float));
    pthread_mutex_lock(&lock);
    long long h = hits, nh = near_hits, m = ++misses;
    pthread_mutex_unlock(&lock);
    logf_("miss %.12s (%zu samples%s)  [%lld hits, %lld near, %lld misses]", key, L, rebuilt ? ", rebuilt" : "", h, nh, m);
    if (!mixc || !kk || !mdl || !writer_admit(bytes)) {
        free(mixc); free(kk); free(mdl); free(pcm);
        return st;
    }
    memcpy(mixc, mix.data, mix.count * sizeof(float));
    if (plat_async_b(writer, ^{
            store_entry(mdl, kk, pcm, mixc, L, 1, rebuilt);
            free(pcm); free(mixc); free(kk); free(mdl);
            writer_done(bytes);
        })) {
        free(mixc); free(kk); free(mdl); free(pcm);         /* not queued (never on the Mac) */
        writer_done(bytes);
    }
    return st;
}

#ifdef RBSTEMS_TEST_TIMING
/* R becomes a copy of the real table whose Run and CreateSession add their time to t_real; what
 * rekordbox calls are the hooks wrapped in the timing lines (bridge_GetApi, timing_hooks) */
static const OrtApi *R_real;
static OrtApi timed_real;

static OrtStatus *ORT_API_CALL real_Run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names,
                                        const OrtValue *const *inputs, size_t n_in, const char *const *out_names,
                                        size_t n_out, OrtValue **outputs) {
    uint64_t t0 = t_now();
    OrtStatus *st = R_real->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
    t_real += t_now() - t0;
    return st;
}

static OrtStatus *ORT_API_CALL real_CreateSession(const OrtEnv *env, const ORTCHAR_T *model_path,
                                                  const OrtSessionOptions *options, OrtSession **out) {
    uint64_t t0 = t_now();
    OrtStatus *st = R_real->CreateSession(env, model_path, options, out);
    t_real += t_now() - t0;
    return st;
}

static OrtStatus *ORT_API_CALL timed_Run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names,
                                         const OrtValue *const *inputs, size_t n_in, const char *const *out_names,
                                         size_t n_out, OrtValue **outputs) {
    t_real = 0;
    t_kind = "pass";
    uint64_t t0 = t_now();
    OrtStatus *st = hook_Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
    uint64_t t1 = t_now(), real = t_real;
    logf_("timing run=%s total_ns=%llu real_ns=%llu own_ns=%llu", t_kind, (unsigned long long)(t1 - t0),
          (unsigned long long)real, (unsigned long long)(t1 - t0 - real));
    return st;
}

static OrtStatus *ORT_API_CALL timed_CreateSession(const OrtEnv *env, const ORTCHAR_T *model_path,
                                                   const OrtSessionOptions *options, OrtSession **out) {
    t_real = 0;
    t_memo = "-";
    uint64_t t0 = t_now();
    OrtStatus *st = hook_CreateSession(env, model_path, options, out);
    uint64_t t1 = t_now(), real = t_real;
    logf_("timing session memo=%s total_ns=%llu real_ns=%llu own_ns=%llu", t_memo, (unsigned long long)(t1 - t0),
          (unsigned long long)real, (unsigned long long)(t1 - t0 - real));
    return st;
}

static void timing_hooks(const OrtApi *real) {          /* bridge_GetApi, under its lock, once */
    R_real = real;
    memcpy(&timed_real, real, sizeof timed_real);
    timed_real.Run = real_Run;
    timed_real.CreateSession = real_CreateSession;
    R = &timed_real;
    api.Run = timed_Run;
    api.CreateSession = timed_CreateSession;
}
#endif

/* ---------------------------------------------------------------- the library's entry points */
#if defined(_WIN32)
/* entry_win.c's bridge_GetApi, once, under its lock: a (a copy of the real table) gets the three
 * hooks; R, the table the hooks call, is the copy's original slots */
static void bridge_core_hook(OrtApi *a) {
    static OrtApi real_slots;
    memcpy(&real_slots, a, sizeof real_slots);
    R = &real_slots;
    a->CreateSession = hook_CreateSession;
    a->ReleaseSession = hook_ReleaseSession;
    a->Run = hook_Run;
}
#else

static const OrtApi *ORT_API_CALL bridge_GetApi(uint32_t version) {
    const OrtApi *real = real_base ? real_base->GetApi(version) : NULL;
    if (!real) return NULL;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    if (!R) {
        R = real;
        memcpy(&api, real, sizeof api);
        api.CreateSession = hook_CreateSession;
        api.ReleaseSession = hook_ReleaseSession;
        api.Run = hook_Run;
        TIMING(timing_hooks(real);)
    }
    pthread_mutex_unlock(&m);
#ifdef RBSTEMS_TEST_TIMING
    return real == R_real ? &api : real;
#else
    return real == R ? &api : real;
#endif
}

static const char *ORT_API_CALL bridge_GetVersionString(void) {
    return real_base ? real_base->GetVersionString() : "";
}

static OrtApiBase bridge_base = {bridge_GetApi, bridge_GetVersionString};

EXPORT const OrtApiBase *ORT_API_CALL OrtGetApiBase(void) NO_EXCEPTION {
    pthread_once(&once, load_real);
    return hooks_ok ? &bridge_base : real_base;
}

/* the two other functions rekordbox imports from the library, passed through */
EXPORT OrtStatus *OrtSessionOptionsAppendExecutionProvider_CPU(OrtSessionOptions *o, int use_arena) {
    pthread_once(&once, load_real);
    static OrtStatus *(*f)(OrtSessionOptions *, int);
    if (!f && real_handle) f = (OrtStatus * (*)(OrtSessionOptions *, int)) dlsym(real_handle, "OrtSessionOptionsAppendExecutionProvider_CPU");
    if (f) return f(o, use_arena);
    return R ? R->CreateStatus(ORT_FAIL, "rbstems bridge: real CPU provider missing") : NULL;
}

EXPORT OrtStatus *OrtSessionOptionsAppendExecutionProvider_CoreML(OrtSessionOptions *o, uint32_t flags) {
    pthread_once(&once, load_real);
    static OrtStatus *(*f)(OrtSessionOptions *, uint32_t);
    if (!f && real_handle) f = (OrtStatus * (*)(OrtSessionOptions *, uint32_t)) dlsym(real_handle, "OrtSessionOptionsAppendExecutionProvider_CoreML");
    if (f) return f(o, flags);
    return R ? R->CreateStatus(ORT_FAIL, "rbstems bridge: real CoreML provider missing") : NULL;
}
#endif
