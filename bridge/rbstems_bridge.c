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
 *         half level (stems can peak above full scale) and doubled on reading.
 * Config: ~/Library/Application Support/rbstemsplus/config.ini   enabled=1  max_gb=20  max_days=60
 *         rekordbox_model=0 (written by the app, read here when rekordbox loads the library;
 *         missing = defaults)
 * Log:    ~/Library/Logs/rbstemsplus/bridge.log (over 5 MB it becomes bridge.log.1)
 * Home:   the account's home from the user database, never $HOME; without one (or with a path too
 *         long to hold the cache's files) there is no cache, and nothing is written elsewhere.
 * Limits: an entry not used for max_days is deleted, and over max_gb the least recently used
 *         go first (a cache hit stamps the file's modification time). Checked when rekordbox
 *         opens the Demucs model, and after writes that take the cache over the limit. The
 *         cleanup touches only names and depths this file writes (layout()); anything else in
 *         the folder is left alone.
 * Space:  nothing is written to the cache while its disk has less free than its floor: 10% of the
 *         disk's size, at most 50 GB (free_floor); cached stems still load.
 * Memory: our own model (our_models) is opened with ONNX Runtime's memory pattern off; any other
 *         model gets rekordbox's session options untouched.
 * Safety: any failure falls back to running the model; writes go to a temporary file in a
 *         background queue and are renamed when complete.
 */
#include <Accelerate/Accelerate.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <ctype.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
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
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "onnxruntime_c_api.h"

#define FORMAT "rbstems-v1"
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
 * pioneer_models): "rbstems-cache: rebuild=<v> pioneer=<sha>[,<sha>];" */
__attribute__((used)) static const char rbstems_caps[] = "rbstems-cache: rebuild=" REBUILD_VERSION " pioneer=" PIONEER_0002 ";";
/* dB: rebuilt stems must explain the chunk at least this well (chunks over -60 dBFS, FRAME_FLOOR).
 * Pioneer's model doesn't make its stems add up exactly: on rumble techno (Ignez, SMV015, 171
 * chunks) its worst chunk was -17.9 dB, most below -24; a wrong rebuild gives -6 dB or worse. */
#define REBUILT_RESIDUAL_MAX (-10.0)

static const OrtApiBase *real_base;
static void *real_handle;               /* the real library, loaded privately */
static const OrtApi *R;                 /* the real library's functions */
static OrtApi api;                      /* what rekordbox gets: R with three entries replaced */
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hash_lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_queue_t writer;
#define CACHE_TAIL "/Library/Caches/rbstemsplus"
#define DEEPEST 160                     /* the longest name under the cache: <model>/<xx>/<key>.desc.<pid>.tmp */
static char base_dir[PATH_MAX - DEEPEST]; /* the cache ("": none), short enough for every file in it */
static char config_path[PATH_MAX];
static char log_path[PATH_MAX];
static int enabled = 1;
static int rekordbox_model = 0;         /* this account caches rekordbox's own (listed) models too */
static long long max_bytes = 20LL << 30;
static double max_days = 60;
static long long total_bytes = -1;      /* -1: not counted yet */
static long long hits, near_hits, misses, writes;

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

/* fopen for writing that won't follow a symlink planted at the path */
static FILE *open_w(const char *path, int flags, const char *mode) {
    int fd = open(path, flags | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f) close(fd);
    return f;
}

static void logf_(const char *fmt, ...) {
    if (!log_path[0]) return;
    pthread_mutex_lock(&log_lock);                          /* one rotation, whole lines */
    struct stat st;
    if (stat(log_path, &st) == 0 && st.st_size > LOG_MAX) {
        char old[PATH_MAX + 2];
        if (!pathf(old, sizeof old, "%s.1", log_path)) rename(log_path, old);
    }
    FILE *f = open_w(log_path, O_WRONLY | O_APPEND, "a");
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

static void mkdirs(const char *path) {
    char tmp[PATH_MAX];
    if (pathf(tmp, sizeof tmp, "%s", path)) return;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

/* 1 or 0 for a yes/no value, -1 when it's neither */
static int yes_no(const char *v) {
    if (!strcmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcasecmp(v, "false")) return 0;
    if (!strcmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcasecmp(v, "true")) return 1;
    return -1;
}

/* config.ini, written by the app: key=value lines, '#' starts a comment line. The bridge reads
 * enabled, rekordbox_model, max_gb and max_days at the top (before any [section]) and in [cache]; other sections
 * are someone else's ([watcher] has an enabled of its own). Keys and section names ignore case,
 * spaces around '=' are allowed, and a value is either quoted ("..." or '...') or ends at the
 * first space or '#'. Unknown keys are ignored; a bad or out-of-range value is ignored with a log
 * line, and so is a line too long to read whole (its tail must not pass for a line of its own). */
static void read_config(void) {
    FILE *f = fopen(config_path, "r");
    if (!f) return;                                         /* none: the defaults */
    char line[256];
    int first = 1, tail = 0, ours = 1;                      /* ours: top level or [cache] */
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        int whole = (len > 0 && line[len - 1] == '\n') || feof(f);
        if (tail) { tail = !whole; continue; }              /* the rest of an over-long line */
        char *s = line;
        if (first && !strncmp(s, "\xEF\xBB\xBF", 3)) s += 3; /* a UTF-8 byte order mark */
        first = 0;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '#' || !*s) { tail = !whole; continue; }
        if (!whole) { tail = 1; logf_("config: ignored a line over %d characters: %.40s", (int)sizeof line - 2, s); continue; }
        if (*s == '[') {                                    /* [section] */
            char name[32] = {0};
            ours = sscanf(s, "[ %31[A-Za-z0-9_-] ]", name) == 1 && !strcasecmp(name, "cache");
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || !ours) continue;
        char *ke = eq;
        while (ke > s && isspace((unsigned char)ke[-1])) ke--;
        *ke = 0;                                            /* s: the key */
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *q = (*v == '"' || *v == '\'') ? strchr(v + 1, *v) : NULL;
        if (q) { *q = 0; v++; }                             /* v: the value */
        else v[strcspn(v, " \t\r\n#")] = 0;
        if (!strcasecmp(s, "enabled") || !strcasecmp(s, "rekordbox_model")) {
            int b = yes_no(v), *dst = !strcasecmp(s, "enabled") ? &enabled : &rekordbox_model;
            if (b >= 0) *dst = b;
            else logf_("config: ignored %s=%.40s (allowed 1 or 0)", dst == &enabled ? "enabled" : "rekordbox_model", v);
        } else if (!strcasecmp(s, "max_gb") || !strcasecmp(s, "max_days")) {
            int gb = tolower((unsigned char)s[4]) == 'g';
            double lo = gb ? 0.1 : 1, hi = gb ? 100000 : 36500;
            char *end;
            double x = strtod(v, &end);
            if (!*v || *end || !(x >= lo && x <= hi)) logf_("config: ignored %s=%.40s (allowed %g to %g)", gb ? "max_gb" : "max_days", v, lo, hi);
            else if (gb) max_bytes = (long long)(x * (1LL << 30));
            else max_days = x;
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- loading the real library */

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
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { logf_("real library: cannot load %s: %s", path, dlerror()); return NULL; }
    if (dlsym(h, "rbstems_marker")) {  /* dyld handed back this bridge (same install name) */
        logf_("ERROR real library: %s resolved to the bridge itself", path);
        return NULL;
    }
    if (!dlsym(h, "OrtGetApiBase")) { logf_("ERROR real library: %s has no OrtGetApiBase", path); return NULL; }
    return h;
}

/* The account's home, from the user database: $HOME is whatever started rekordbox says, and the
 * cleanup deletes files under it. A test build takes RBSTEMS_TEST_HOME instead when it is set
 * ("" = no home), so the tests never touch the real one. */
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

/* the cache, config and log paths under the user's home; a path that doesn't fit is "" (off) */
static void set_paths(void) {
    char home[PATH_MAX], dir[PATH_MAX];
    if (home_dir(home, sizeof home)) return;                /* no home: no log, no config, no cache */
    if (!pathf(dir, sizeof dir, "%s/Library/Logs/rbstemsplus", home) && !pathf(log_path, sizeof log_path, "%s/bridge.log", dir))
        mkdirs(dir);
    pathf(config_path, sizeof config_path, "%s/Library/Application Support/rbstemsplus/config.ini", home);
    size_t len, tail = strlen(CACHE_TAIL);
    if (pathf(base_dir, sizeof base_dir, "%s" CACHE_TAIL, home) || (len = strlen(base_dir)) <= tail
        || strcmp(base_dir + len - tail, CACHE_TAIL)) {
        base_dir[0] = 0;
        logf_("ERROR the home folder's path is too long for the cache (%zu characters): nothing is cached", strlen(home));
        return;
    }
    mkdirs(base_dir);
}

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
    read_config();
    writer = dispatch_queue_create("rbstems.writer", DISPATCH_QUEUE_SERIAL);
    logf_("bridge loaded (real library %s from %s%s; cache %s, enabled=%d, max_gb=%.1f, max_days=%g, rekordbox_model=%d, rebuild %s%s)",
          real_base ? ver : "MISSING", dladdr((void *)get, &ri) ? ri.dli_fname : "?",
          hooks_ok ? "" : ", NOT 1.18.x: passing everything through", base_dir[0] ? base_dir : "NONE", enabled,
          max_bytes / (double)(1LL << 30), max_days, rekordbox_model, REBUILD_VERSION, BUILD_NOTE);
}

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

static int file_sha256_locked(const char *path, char out[65]) {
    /* the checksum is remembered per (size, modification time) in model-<inode>.sha256 */
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    char memo[PATH_MAX], tmp[PATH_MAX], want[128];
    if (!base_dir[0]) memo[0] = 0;                          /* no cache: no memo, hashed every time */
    else pathf(memo, sizeof memo, "%s/model-%llu.sha256", base_dir, (unsigned long long)st.st_ino);
    snprintf(want, sizeof want, "%lld %ld", (long long)st.st_size, (long)st.st_mtimespec.tv_sec);
    FILE *f = memo[0] ? fopen(memo, "r") : NULL;
    if (f) {
        char line[200] = {0};
        fgets(line, sizeof line, f);
        fclose(f);
        size_t wl = strlen(want);
        if (!strncmp(line, want, wl) && line[wl] == ' ' && strlen(line + wl + 1) >= 64) {
            memcpy(out, line + wl + 1, 64);
            out[64] = 0;
            return 0;
        }
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char *buf = malloc(1 << 20);              /* per call: concurrent sessions hashed one shared buffer */
    if (!buf) { close(fd); return -1; }
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    ssize_t n;
    while ((n = read(fd, buf, 1 << 20)) > 0) CC_SHA256_Update(&c, buf, (CC_LONG)n);
    free(buf);
    close(fd);
    if (n < 0) return -1;
    unsigned char d[32];
    CC_SHA256_Final(d, &c);
    hex(d, 32, out);
    if (memo[0] && !pathf(tmp, sizeof tmp, "%s.%d.tmp", memo, getpid()) && (f = open_w(tmp, O_WRONLY | O_TRUNC, "w"))) {
        int ok = fprintf(f, "%s %s\n", want, out) > 0;
        ok = fclose(f) == 0 && ok;
        if (!ok || rename(tmp, memo) != 0) unlink(tmp);
    }
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
 * REBUILD_VERSION */
static void rebuilt_id(const char *fp, char out[65]) {
    unsigned char d[32];
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, "rbstems-rebuild-" REBUILD_VERSION ":", (CC_LONG)strlen("rbstems-rebuild-" REBUILD_VERSION ":"));
    CC_SHA256_Update(&c, fp, 64);
    CC_SHA256_Final(d, &c);
    hex(d, 32, out);
}

/* ---------------------------------------------------------------- the cache's layout */

/* What fts found under the cache folder, by name and depth: only what this file writes. Everything
 * else (another app's files, a symlink, a folder someone made) is OTHER and is never read, deleted
 * or entered. */
enum { OTHER, MODEL_DIR, PREFIX_DIR, ENTRY_FLAC, ENTRY_DESC, ENTRY_TMP, MEMO_TMP };

static int is_hex(const char *s, size_t n) {               /* lowercase, as hex() writes */
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

static int is_pid_tmp(const char *s) {                      /* ".<pid>.tmp", the whole rest */
    if (*s++ != '.' || !isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s)) s++;
    return !strcmp(s, ".tmp");
}

/* level: e's depth below the cache folder (fts levels from base_dir; +1 from a model folder) */
static int layout(const FTSENT *e, int level) {
    const char *n = e->fts_name;
    size_t len = e->fts_namelen;
    int dir = e->fts_info == FTS_D, file = e->fts_info == FTS_F;
    if (level == 1) {                                       /* <model sha256>/, model-<inode>.sha256.<pid>.tmp */
        if (dir) return len == 64 && is_hex(n, 64) ? MODEL_DIR : OTHER;
        if (!file || strncmp(n, "model-", 6) || !isdigit((unsigned char)n[6])) return OTHER;
        for (n += 6; isdigit((unsigned char)*n); n++) {}
        return !strncmp(n, ".sha256", 7) && is_pid_tmp(n + 7) ? MEMO_TMP : OTHER;
    }
    if (level == 2) return dir && len == 2 && is_hex(n, 2) ? PREFIX_DIR : OTHER;   /* <key[0:2]>/ */
    if (level != 3 || !file || len < 69 || !is_hex(n, 64) || strncmp(n, e->fts_parent->fts_name, 2) || e->fts_parent->fts_namelen != 2)
        return OTHER;
    if (!strcmp(n + 64, ".flac")) return ENTRY_FLAC;        /* <key>.flac, <key>.desc, either's .<pid>.tmp */
    if (!strcmp(n + 64, ".desc")) return ENTRY_DESC;
    return (!strncmp(n + 64, ".flac", 5) || !strncmp(n + 64, ".desc", 5)) && is_pid_tmp(n + 69) ? ENTRY_TMP : OTHER;
}

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

static int read_desc(const char *path, summary *s) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[4];
    s->d = s->rt = s->rf = NULL;
    int ok = fread(magic, 1, 4, f) == 4 && (!memcmp(magic, "RBD1", 4) || !memcmp(magic, "RBD2", 4))
             && fread(&s->frames, 4, 1, f) == 1 && s->frames > 0 && s->frames <= DESC_MAX_FRAMES
             && fread(&s->r0, 4, 1, f) == 1 && fread(&s->g0, 4, 1, f) == 1;
    if (ok) {
        s->version = magic[3] - '0';
        size_t n = 2 * (size_t)s->frames;
        s->d = malloc(sizeof(float) * n);
        ok = s->d && fread(s->d, sizeof(float), n, f) == n;
        if (ok && s->version == 2) {
            s->rt = malloc(sizeof(float) * 2);
            s->rf = malloc(sizeof(float) * n);
            ok = s->rt && s->rf && fread(s->rt, sizeof(float), 2, f) == 2 && fread(s->rf, sizeof(float), n, f) == n
                 && all_finite(s->rt, 2) && all_finite(s->rf, n);
        }
        ok = ok && isfinite(s->r0) && isfinite(s->g0) && all_finite(s->d, n);
    }
    fclose(f);
    if (!ok) free_summary(s);
    return ok ? 0 : -1;
}

static int desc_path(const char *flac_path, char *out, size_t n) {
    return pathf(out, n, "%.*s.desc", (int)(strlen(flac_path) - 5), flac_path);
}

static int write_desc(const char *flac_path, const float *d, int frames, float r0, float g0, const float *rt, const float *rf) {
    char path[PATH_MAX], tmp[PATH_MAX];
    if (desc_path(flac_path, path, sizeof path) || pathf(tmp, sizeof tmp, "%s.%d.tmp", path, getpid())) return -1;
    FILE *f = open_w(tmp, O_WRONLY | O_TRUNC, "wb");
    if (!f) return -1;
    size_t n = 2 * (size_t)frames;
    int ok = fwrite("RBD2", 1, 4, f) == 4 && fwrite(&frames, 4, 1, f) == 1 && fwrite(&r0, 4, 1, f) == 1
             && fwrite(&g0, 4, 1, f) == 1 && fwrite(d, sizeof(float), n, f) == n
             && fwrite(rt, sizeof(float), 2, f) == 2 && fwrite(rf, sizeof(float), n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static void free_index(summary *ix, size_t n) {
    for (size_t i = 0; i < n; i++) free_summary(&ix[i]);
    free(ix);
}

/* (re)reads every RBD2 summary of one model's cache */
static void load_index(const char *model) {
    char dir[PATH_MAX];
    if (!base_dir[0] || pathf(dir, sizeof dir, "%s/%s", base_dir, model)) return;
    char *roots[] = {dir, NULL};
    size_t cap = 1024, n = 0;
    summary *ix = malloc(cap * sizeof *ix);
    if (!ix) return;
    FTS *fts = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    FTSENT *e;
    while (fts && (e = fts_read(fts))) {
        if (e->fts_level == 0) continue;
        int k = layout(e, e->fts_level + 1);
        if (e->fts_info == FTS_D && k != PREFIX_DIR) fts_set(fts, e, FTS_SKIP);
        if (k != ENTRY_DESC) continue;
        if (n == cap) {
            summary *t = realloc(ix, cap * 2 * sizeof *ix);
            if (!t) break;                                  /* a partial index: still correct */
            ix = t;
            cap *= 2;
        }
        if (read_desc(e->fts_path, &ix[n]) == 0) {
            if (ix[n].version != 2) { free_summary(&ix[n]); continue; }
            memcpy(ix[n].key, e->fts_name, 64);
            ix[n].key[64] = 0;
            n++;
        }
    }
    if (fts) fts_close(fts);
    pthread_mutex_lock(&lock);
    summary *old = index_;
    size_t old_n = index_n;
    index_ = ix;
    index_n = n;
    memcpy(index_model, model, 65);
    index_time = time(NULL);
    pthread_mutex_unlock(&lock);
    free_index(old, old_n);
}

static void add_to_index(const char *model, const char *key, const float *d, int frames, float r0, float g0,
                         const float *rt, const float *rf) {
    size_t n2 = 2 * (size_t)frames;
    summary s = {.frames = frames, .version = 2, .r0 = r0, .g0 = g0};
    memcpy(s.key, key, 65);
    s.d = malloc(sizeof(float) * n2);
    s.rt = malloc(sizeof(float) * 2);
    s.rf = malloc(sizeof(float) * n2);
    if (!s.d || !s.rt || !s.rf) { free_summary(&s); return; }
    memcpy(s.d, d, sizeof(float) * n2);
    memcpy(s.rt, rt, sizeof(float) * 2);
    memcpy(s.rf, rf, sizeof(float) * n2);
    pthread_mutex_lock(&lock);
    summary *ix = NULL;
    if (!strcmp(index_model, model) && (ix = realloc(index_, (index_n + 1) * sizeof *ix))) {
        index_ = ix;
        index_[index_n++] = s;
    }
    pthread_mutex_unlock(&lock);
    if (!ix) free_summary(&s);
}

/* the NEAR_CANDIDATES nearest RBD2 summaries closer than NEAR_MAX, copied out */
static int nearest(const float *d, int frames, summary *out) {
    double best[NEAR_CANDIDATES];
    int found = 0;
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < index_n; i++) {
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

typedef struct { char path[PATH_MAX]; time_t mtime; off_t size; } entry;

static int by_mtime(const void *a, const void *b) {
    time_t x = ((const entry *)a)->mtime, y = ((const entry *)b)->mtime;
    return x < y ? -1 : x > y;
}

static void unlink_entry(const char *flac) {
    char desc[PATH_MAX];
    unlink(flac);
    if (!desc_path(flac, desc, sizeof desc)) unlink(desc);
}

/* FTS_PHYSICAL: a symlink is never followed, so nothing outside the cache folder is reached */
static void prune(void) {
    if (!base_dir[0]) return;
    char *roots[] = {base_dir, NULL};
    size_t cap = 4096, n = 0;
    entry *es = malloc(cap * sizeof *es);
    if (!es) return;
    FTS *fts = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!fts) { free(es); return; }
    time_t now = time(NULL);
    long long total = 0, aged = 0, temps = 0;
    int complete = 1;
    FTSENT *e;
    while ((e = fts_read(fts))) {
        if (e->fts_level == 0) continue;
        int k = layout(e, e->fts_level);
        if (e->fts_info == FTS_D && k != MODEL_DIR && k != PREFIX_DIR) fts_set(fts, e, FTS_SKIP);
        size_t len = strlen(e->fts_path);
        if (k == ENTRY_TMP || k == MEMO_TMP) {
            if (now - e->fts_statp->st_mtime > 3600) { unlink(e->fts_path); temps++; }
            continue;
        }
        if (k == ENTRY_DESC) {                                  /* a summary without its stems */
            char f[PATH_MAX];
            if (!pathf(f, sizeof f, "%.*s.flac", (int)(len - 5), e->fts_path) && access(f, F_OK) != 0
                && now - e->fts_statp->st_mtime > 3600)
                unlink(e->fts_path);
            continue;
        }
        if (k != ENTRY_FLAC) continue;
        if (now - e->fts_statp->st_mtime > (time_t)(max_days * 86400)) { unlink_entry(e->fts_path); aged++; continue; }
        if (n == cap) {
            entry *t = realloc(es, cap * 2 * sizeof *es);
            if (!t) { complete = 0; break; }                    /* no eviction on a partial list */
            es = t;
            cap *= 2;
        }
        if (pathf(es[n].path, sizeof es[n].path, "%s", e->fts_path)) continue;
        es[n].mtime = e->fts_statp->st_mtime;
        es[n].size = e->fts_statp->st_size;
        total += es[n].size;
        n++;
    }
    fts_close(fts);
    long long evicted = 0;
    if (complete && total > max_bytes) {
        qsort(es, n, sizeof *es, by_mtime);                 /* least recently used first */
        for (size_t i = 0; i < n && total > max_bytes * 9 / 10; i++) {
            if (unlink(es[i].path) == 0) { unlink_entry(es[i].path); total -= es[i].size; evicted++; }
        }
    }
    free(es);
    if (complete) {
        pthread_mutex_lock(&lock);
        total_bytes = total;
        pthread_mutex_unlock(&lock);
    }
    logf_("limits: %.2f GB in cache; removed %lld unused for %g days, %lld over %.1f GB, %lld stale temp files%s",
          total / (double)(1LL << 30), aged, max_days, evicted, max_bytes / (double)(1LL << 30), temps,
          complete ? "" : " (out of memory: no eviction this time)");
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
    mkdirs(base_dir);                                       /* it may have been cleared since loading */
    double b = 0;
    CFURLRef u = finder ? CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)base_dir, (CFIndex)strlen(base_dir), true) : NULL;
    CFTypeRef v = NULL;
    if (u && CFURLCopyResourcePropertyForKey(u, kCFURLVolumeAvailableCapacityForImportantUsageKey, &v, NULL) && v
        && CFGetTypeID(v) == CFNumberGetTypeID()) {
        int64_t n = 0;
        if (CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &n) && n > 0) b = (double)n;
    }
    if (v) CFRelease(v);
    if (u) CFRelease(u);
    if (b > 0) { *how = "Finder's count"; return b; }
    struct statfs fs;
    *how = "statfs";
    return statfs(base_dir, &fs) == 0 ? (double)fs.f_bavail * fs.f_bsize : 0;
}

/* The size of the cache's volume in bytes as statfs counts it (f_blocks * f_bsize, as the app
 * does); 0 if it can't be read. A test build can fake it (RBSTEMS_TEST_DISK_GB; 0: can't be read).
 * Called after free_bytes, which creates the cache folder. */
static uint64_t disk_bytes(void) {
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_DISK_GB");
    if (t) { double g = atof(t); return g > 0 ? (uint64_t)(g * 1e9 + 0.5) : 0; }
#endif
    struct statfs fs;
    return statfs(base_dir, &fs) == 0 ? (uint64_t)fs.f_blocks * fs.f_bsize : 0;
}

/* the floor for a disk of `disk` bytes (0: unknown): 10% of it, at most FREE_FLOOR_MAX */
static uint64_t free_floor(uint64_t disk) {
    return disk > 0 && disk / 10 < FREE_FLOOR_MAX ? disk / 10 : FREE_FLOOR_MAX;
}

/* whether a cache write may go ahead: measured at most every FREE_EVERY seconds, logged when
 * saving pauses and when it resumes. Called on the writer queue only (its statics are unlocked). */
static int space_ok(void) {
    static int paused = -1;                                 /* -1: not measured yet */
    static uint64_t checked;
    uint64_t every = FREE_EVERY, now = clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000;
#ifdef RBSTEMS_TEST_REAL_PATH
    const char *t = getenv("RBSTEMS_TEST_FREE_SECONDS");
    if (t) every = (uint64_t)atoi(t);
#endif
    if (paused >= 0 && now - checked < every) return !paused;
    checked = now;
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
    paused = p;
    return !p;
}

/* ---------------------------------------------------------------- FLAC */

typedef struct { int16_t *pcm; size_t frames; } write_job;   /* pcm interleaved, 8 channels */

static int write_flac(const char *final, write_job *job, const char *key) {
    char tmp[PATH_MAX], dbuf[MAXPATHLEN];
    if (pathf(tmp, sizeof tmp, "%s.%d.tmp", final, getpid()) || !dirname_r(final, dbuf)) return -1;
    mkdirs(dbuf);
    FILE *f = open_w(tmp, O_RDWR | O_TRUNC, "w+b");           /* w+b: the encoder seeks back for STREAMINFO */
    if (!f) { logf_("ERROR writing %s", final); return -1; }
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    if (!enc) { fclose(f); unlink(tmp); return -1; }
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
    int ok = FLAC__stream_encoder_init_FILE(enc, f, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (ok) {
        enum { BLOCK = 4096 };
        FLAC__int32 buf[BLOCK * 8];
        for (size_t f = 0; ok && f < job->frames; f += BLOCK) {
            size_t n = job->frames - f < BLOCK ? job->frames - f : BLOCK;
            for (size_t i = 0; i < n * 8; i++) buf[i] = job->pcm[f * 8 + i];
            ok = FLAC__stream_encoder_process_interleaved(enc, buf, (unsigned)n);
        }
        ok = FLAC__stream_encoder_finish(enc) && ok;
    }
    FLAC__stream_encoder_delete(enc);
    FLAC__metadata_object_delete(vc);
    struct stat st;
    if (ok && rename(tmp, final) == 0 && stat(final, &st) == 0) {
        int over;
        pthread_mutex_lock(&lock);
        writes++;
        if (total_bytes >= 0) total_bytes += st.st_size;
        over = total_bytes > max_bytes;
        pthread_mutex_unlock(&lock);
        if (over) prune();
        return 0;
    }
    unlink(tmp);
    logf_("ERROR writing %s", final);
    return -1;
}

typedef struct { int16_t *pcm; size_t frames, got; int ok; char key[65]; int key_ok; } read_ctx;

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
            const char *s = (const char *)m->data.vorbis_comment.comments[i].entry;
            if (!strncmp(s, "RBSTEMS_KEY=", 12) && !strncmp(s + 12, c->key, 64)) c->key_ok = 1;
        }
}

static void rd_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *u) {
    (void)d; (void)s;
    ((read_ctx *)u)->ok = 0;
}

static int read_flac(const char *path, const char *key, int16_t *pcm, size_t frames) {
    read_ctx c = {pcm, frames, 0, 1, {0}, 0};
    memcpy(c.key, key, 65);
    FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
    if (!d) return -1;
    FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_VORBIS_COMMENT);
    int ok = FLAC__stream_decoder_init_file(d, path, rd_write, rd_meta, rd_error, &c) == FLAC__STREAM_DECODER_INIT_STATUS_OK
             && FLAC__stream_decoder_process_until_end_of_stream(d);
    FLAC__stream_decoder_finish(d);
    FLAC__stream_decoder_delete(d);
    return ok && c.ok && c.key_ok && c.got == frames ? 0 : -1;
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

static int cache_path(const char *model, const char *key, char *path, size_t n) {
    return base_dir[0] ? pathf(path, n, "%s/%s/%.2s/%s.flac", base_dir, model, key, key) : -1;
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
    FFTSetup setup = vDSP_create_fftsetup(12, kFFTRadix2);    /* per call: no shared state between threads */
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
        DSPSplitComplex sc = {re, im};
        for (int s = 0; s < 4; s++)
            for (int c = 0; c < 2; c++) {
                memset(y, 0, sizeof(float) * full);
                const float *xr = x + (size_t)(s * 4 + 2 * c) * BINS * F, *xi = x + (size_t)(s * 4 + 2 * c + 1) * BINS * F;
                for (size_t f = 0; f < F; f++) {
                    /* packed: re[0] = DC, im[0] = Nyquist (the restored top bin, zero) */
                    for (size_t k = 0; k < BINS; k++) { re[k] = xr[k * F + f]; im[k] = xi[k * F + f]; }
                    im[0] = 0.0f;
                    vDSP_fft_zrip(setup, &sc, 1, 12, kFFTDirection_Inverse);
                    vDSP_ztoc(&sc, 1, (DSPComplex *)frame, 2, BINS);
                    const size_t off = (f + 2) * HOP;
                    for (int n = 0; n < NFFT; n++) y[off + n] += frame[n] * scale * win[n];
                }
                float *o = out + (size_t)(s * 2 + c) * L;
                for (size_t n = 0; n < L; n++) o[n] = y[start + n] / env[start + n];
            }
    }
    if (setup) vDSP_destroy_fftsetup(setup);
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
    size_t len = strlen(model_path);
    if (len < 13 || strcmp(model_path + len - 13, "/hdemucs.onnx")) return R->CreateSession(env, model_path, options, out);
    char fp[65];
    const char *how;
    /* hashed first: the checksum picks the options (the memo keeps it cheap) */
    if (file_sha256(model_path, fp)) {
        logf_("ERROR cannot read %s", model_path);
        return R->CreateSession(env, model_path, options, out);
    }
    OrtStatus *st = create_demucs(env, model_path, options, out, fp, &how);
    int cached = !st && enabled && base_dir[0];
    /* rekordbox's own model: rebuilt and cached only where this account turned it on */
    int pioneer = is_pioneer(fp), rebuild = pioneer && rekordbox_model;
    char id[65];
    if (rebuild) rebuilt_id(fp, id);
    else memcpy(id, fp, 65);
    if (!st) logf_("Demucs session opened: model %.12s, %s%s%s", fp, how, cached ? "" : ", not cached",
                   !cached || !pioneer ? "" : rebuild ? ", rekordbox's own: stems rebuilt" : ", rekordbox's own: not cached here (rekordbox_model=0)");
    if (!cached) return st;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!sessions[i].s) { sessions[i].s = *out; memcpy(sessions[i].model, id, 65); sessions[i].rebuild = rebuild; break; }
    pthread_mutex_unlock(&lock);
    char *model = strdup(id);
    if (model)
        dispatch_async(writer, ^{
            prune();
            load_index(model);
            logf_("summaries loaded: %zu cached chunks", index_n);
            free(model);
        });
    return st;
}

static void ORT_API_CALL hook_ReleaseSession(OrtSession *s) {
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

/* the background work of a new entry or an RBD1 upgrade: summary, r0/g0 and residuals from the
 * stored pcm and the chunk, then (if write_pcm) the FLAC, then the .desc and the index. Rebuilt
 * stems (rebuilt) that don't add up to the chunk within REBUILT_RESIDUAL_MAX on either channel
 * aren't stored: a rebuild that doesn't match the model never reaches the cache. Chunks quieter
 * than FRAME_FLOOR (a track's silent end) aren't judged: there the residual means nothing. */
static void store_entry(const char *model, const char *key, const char *path, int16_t *pcm, float *mix, size_t L,
                        int write_pcm, int rebuilt) {
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
        }
    }
    if (ok && write_pcm) {
        write_job job = {pcm, L};
        ok = write_flac(path, &job, key) == 0;
    }
    if (ok && write_desc(path, d, frames, (float)r, (float)g, rt, rf) == 0)
        add_to_index(model, key, d, frames, (float)r, (float)g, rt, rf);
    free(d); free(s); free(m); free(rf);
}

static int desc_is_rbd2(const char *flac_path) {
    char path[PATH_MAX], magic[4] = {0};
    if (desc_path(flac_path, path, sizeof path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "RBD2", 4);
    fclose(f);
    return ok;
}

static OrtStatus *ORT_API_CALL hook_Run(OrtSession *s, const OrtRunOptions *ro, const char *const *in_names,
                                        const OrtValue *const *inputs, size_t n_in, const char *const *out_names,
                                        size_t n_out, OrtValue **outputs) {
    int rebuild = 0;
    const char *model = demucs_model(s, &rebuild);
    int im = model ? index_of(in_names, n_in, "mix") : -1, ig = model ? index_of(in_names, n_in, "mag") : -1;
    int ix = model ? index_of(out_names, n_out, "x") : -1, ixt = model ? index_of(out_names, n_out, "xt") : -1;
    tensor mix, mag;
    if (!model || im < 0 || ig < 0 || ix < 0 || ixt < 0 || n_out != 2 || view(inputs[im], &mix) || view(inputs[ig], &mag)
        || mix.ndim != 3 || mix.dims[0] != 1 || mix.dims[1] != 2 || mag.ndim != 4)
        return R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
    size_t L = (size_t)mix.dims[2];
    double mean, std;
    mean_std(&mix, &mean, &std);
    if (!isfinite(mean) || !isfinite(std) || std <= 0 || L < DESC_FRAME) {
        /* silence or broken input: never cached; our model's NaN for silence becomes 0 */
        OrtStatus *st = R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
        if (!st) sanitize_ours(outputs, ix, ixt);
        return st;
    }

    /* the exact key: format, model, shapes and the audio itself */
    unsigned char dg[32];
    char key[65], path[PATH_MAX];
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, FORMAT, sizeof FORMAT);
    CC_SHA256_Update(&c, model, 64);
    CC_SHA256_Update(&c, mix.dims, sizeof(int64_t) * 3);
    CC_SHA256_Update(&c, mag.dims, sizeof(int64_t) * 4);
    CC_SHA256_Update(&c, mix.data, (CC_LONG)(mix.count * sizeof(float)));
    CC_SHA256_Final(dg, &c);
    hex(dg, 32, key);
    if (cache_path(model, key, path, sizeof path)) return R->Run(s, ro, in_names, inputs, n_in, out_names, n_out, outputs);
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
    if (read_flac(path, key, pcm, L) == 0 && serve(pcm, mix.data, L, 0, mean, std, xdims, xtdims, ix, ixt, outputs) == 0) {
        utimes(path, NULL);                                 /* last used: now */
        pthread_mutex_lock(&lock);
        long long h = ++hits, nh = near_hits, m = misses;
        pthread_mutex_unlock(&lock);
        logf_("hit  %.12s (%zu samples)  [%lld hits, %lld near, %lld misses]", key, L, h, nh, m);
        if (!desc_is_rbd2(path)) {                          /* an older entry: add its residuals */
            float *mixc = malloc(mix.count * sizeof(float));
            char *pp = strdup(path), *kk = strdup(key), *mdl = strdup(model);
            if (mixc && pp && kk && mdl) {
                memcpy(mixc, mix.data, mix.count * sizeof(float));
                int16_t *pc = pcm;
                pcm = NULL;
                dispatch_async(writer, ^{
                    store_entry(mdl, kk, pp, pc, mixc, L, 0, 0);
                    free(pc); free(mixc); free(pp); free(kk); free(mdl);
                });
            } else {
                free(mixc); free(pp); free(kk); free(mdl);
            }
        }
        free(pcm); free(desc); free(ms); free(mm);
        return NULL;
    }

    /* 2. similar, verified per channel and frame */
    pthread_mutex_lock(&lock);
    int stale = strcmp(index_model, model) || time(NULL) - index_time > 30;
    pthread_mutex_unlock(&lock);
    if (stale) load_index(model);                           /* pre-cached entries written since */
    summary cands[NEAR_CANDIDATES];
    int nc = nearest(desc, frames, cands);
    double mmix = 0;
    for (size_t i = 0; i < L; i++) { float v = 0.5f * (mix.data[i] + mix.data[L + i]); mm[i] = v; mmix += (double)v * v; }
    for (int k = 0; k < nc; k++) {
        char cpath[PATH_MAX];
        const char *why = NULL;
        long d = 0;
        double r = 0, g = 0, worst = 0, t0 = 0, t1 = 0;
        if (cache_path(model, cands[k].key, cpath, sizeof cpath) || !cands[k].rt || !cands[k].rf
            || read_flac(cpath, cands[k].key, pcm, L))
            why = "unreadable";
        else {
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
            utimes(cpath, NULL);
            pthread_mutex_lock(&lock);
            long long h = hits, nh = ++near_hits, m = misses;
            pthread_mutex_unlock(&lock);
            logf_("near %.12s for %.12s: shift %ld, level %.3f (cached %.3f), residual L %.1f R %.1f dB (cached %.1f %.1f, worst frame %+.1f dB to its bar)  [%lld hits, %lld near, %lld misses]",
                  cands[k].key, key, d, g, cands[k].g0, t0, t1, cands[k].rt[0], cands[k].rt[1], worst, h, nh, m);
            for (int j = 0; j < nc; j++) free_summary(&cands[j]);
            free(pcm); free(desc); free(ms); free(mm);
            return NULL;
        }
        logf_("near %.12s rejected (%s) for %.12s: shift %ld, level %.3f (cached %.3f), residual L %.1f R %.1f dB, worst frame %+.1f dB over its bar",
              cands[k].key, why ? why : "serve", key, d, g, cands[k].g0, t0, t1, worst);
    }
    for (int j = 0; j < nc; j++) free_summary(&cands[j]);

    /* 3. the model, then cache its stems and the chunk's summary in the background */
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
    char *pp = strdup(path), *kk = strdup(key), *mdl = strdup(model);
    if (!mixc || !pp || !kk || !mdl) {
        free(mixc); free(pp); free(kk); free(mdl); free(pcm);
        return st;
    }
    memcpy(mixc, mix.data, mix.count * sizeof(float));
    pthread_mutex_lock(&lock);
    long long h = hits, nh = near_hits, m = ++misses;
    pthread_mutex_unlock(&lock);
    logf_("miss %.12s (%zu samples%s)  [%lld hits, %lld near, %lld misses]", key, L, rebuilt ? ", rebuilt" : "", h, nh, m);
    dispatch_async(writer, ^{
        store_entry(mdl, kk, pp, pcm, mixc, L, 1, rebuilt);
        free(pcm); free(mixc); free(pp); free(kk); free(mdl);
    });
    return st;
}

/* ---------------------------------------------------------------- the library's entry points */

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
    }
    pthread_mutex_unlock(&m);
    return real == R ? &api : real;
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
