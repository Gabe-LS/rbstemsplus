/* bridge/tests harness: linked against the bridge by @rpath, as rekordbox is, and run by run.sh
 * with HOME pointed at a scratch folder. Prints "ok ..." or "FAIL ..." lines; exits 1 on a FAIL.
 *
 *   harness load REAL           OrtGetApiBase is the bridge's: 1.18.x, Run/CreateSession/
 *                               ReleaseSession hooked, everything else REAL's; the CPU provider works
 *   harness passthrough FAKE    a library that isn't 1.18.x is handed back untouched
 *   harness missing             no real library: NULL, and no crash
 *   harness run MODEL WHAT...   runs chunks through a session on MODEL and says, for each, whether
 *                               the answer is the model's own ("model") or a cached one ("cache")
 *                               WHAT: a (the base chunk), b (a shifted 300 samples later),
 *                               c (a with a loud tone burst), d (unrelated audio), wait (until the
 *                               background writer has stored every miss so far), sleep (1 s, for
 *                               the writer to finish when nothing is expected to be stored),
 *                               drain (as wait, but polled every 10 ms, no extra pause, no line:
 *                               misses paced so that the writer queue never fills; a miss not
 *                               stored within 10 s ends the run: "FAIL drain", exit 1),
 *                               free=GB (the free space the test bridge sees from now on),
 *                               e, f, g (more unrelated audio), nN (unrelated audio number N: n1, n2, ...),
 *                               sh=CMD (runs CMD with system())
 *                               After creating the session it waits (10 s, or $RBSTEMS_HARNESS_READY) for the bridge's
 *                               (or for $RBSTEMS_HARNESS_WAIT_FOR anywhere in the log, e.g. "cache: using")
 *                               "summaries" line: the index loads off rekordbox's threads. Stored
 *                               entries are counted in $RBSTEMS_HARNESS_CACHE (a chosen location),
 *                               else in the default folder.
 *   harness prun MODEL WHAT...  the same with a model shaped like rekordbox's own (make_model.py
 *                               --pioneer): mag [1, 4, 2048, F] with bin 0 of the real parts at 1,
 *                               x non-zero. Each answer is "model" (exactly the model's), "cache" (x
 *                               zero, xt the stems rebuilt as rekordbox would, within the cache's
 *                               16-bit rounding; for b, a near hit, only x zero is checked) or
 *                               "wrong". More WHAT: w=W0,W1,W2,W3 (the model's weights, needed to
 *                               check answers), frames=N (F from now on; default ceil(L / 1024)),
 *                               gain=G (each chunk from now on scaled by G; answers then unchecked)
 *                               In both, WHAT*N repeats WHAT N times (a*200: 200 runs of a), and m*N
 *                               is N chunks of unrelated audio, each new (n<seed>, seeds counting up
 *                               from 0, or from K after mseed=K)
 *
 *   The bench group (bridge/tests/perf.sh; the numbers come from a timing build's log lines):
 *   harness bench run|prun MODEL WHAT...   as run and prun, quiet: no line per chunk, at the end
 *                               "ran N: X model, Y cache, Z wrong"
 *   harness bench sessions MODEL N [nomemo]   N sessions on MODEL one after the other, each
 *                               released once the bridge has dealt with its index; nomemo deletes
 *                               the model checksums' memos before each. After each: "session I
 *                               outer_ns=T memo_ino=INO" (the memo's inode, 0 if none)
 *   harness bench start         OrtGetApiBase and GetApi, timed from here: "start outer_ns=T hooked=1"
 *
 * On Windows (bridge.md §4 "Port harness.c", T6) the same groups, built by bridge/win/build.sh
 * --test as harness.exe and linked through a NONAME import library, so it imports the bridge by
 * ordinal only, as rekordbox.exe does. The differences are the Windows bridge's own: the base is
 * always the bridge's (it forwards GetVersionString), "missing" gets the refusal stub instead of
 * NULL (r4:I46), the providers are CPU and DirectML (ordinals 2-4), and the bridge's folders are
 * under the home (RBSTEMS_TEST_HOME, else %LOCALAPPDATA%): rbstemsplus\Logs and rbstemsplus\Cache.
 * One more WHAT in run (BW1, the TEST bridge's device check): gpu=in marks each later chunk's mix
 * as GPU memory, gpu=out pre-allocates x (the shape of an earlier run's, filled with -12345) and
 * marks it, gpu=off stops (the TEST bridge reads the marked value's address from
 * RBSTEMS_TEST_GPU_VALUE). A marked run must be passed through: one served prints "FAIL run C: a
 * GPU-marked run was served", and an entry stored since the first marked run (beyond the unmarked
 * misses since then) fails the next drain, wait or sleep (review BX5). Drain or wait for the
 * unmarked misses before the first gpu= mark: they are counted from there.
 * Windows' argv, RBSTEMS_HARNESS_CACHE and sh= are read as UTF-16 (review BX4), and a cache
 * folder that exists but can't be listed fails the count (a missing one counts 0).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>                      /* CommandLineToArgvW */
#undef near                               /* windows.h's 16-bit leftovers */
#undef far
#else
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
/* dllimport: an import's address is then the bridge's own function, read from the import table.
 * Without it, &OrtGetApiBase is harness.exe's import thunk, and owner() of it names harness.exe
 * (bridge/win/build.sh refuses an exe with such a thunk) */
#define ORT_DLL_IMPORT
#endif
#include "onnxruntime_c_api.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* the bridge's other exports (declared in ONNX Runtime's provider headers) */
#if defined(_WIN32)
__declspec(dllimport) OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProviderEx_DML(OrtSessionOptions *o, void *dml, void *queue);
__declspec(dllimport) OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_CPU(OrtSessionOptions *o, int use_arena);
__declspec(dllimport) OrtStatus *ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(OrtSessionOptions *o, int device);
#else
OrtStatus *OrtSessionOptionsAppendExecutionProvider_CPU(OrtSessionOptions *o, int use_arena);
OrtStatus *OrtSessionOptionsAppendExecutionProvider_CoreML(OrtSessionOptions *o, uint32_t flags);
#endif

/* ---------------------------------------------------------------- the platform's parts */

#if defined(_WIN32)
#define LOG_TAIL "\\rbstemsplus\\Logs\\bridge.log"
#define CACHE_TAIL "\\rbstemsplus\\Cache"

/* The file layer: paths are UTF-8 in here, as the bridge's are, and UTF-16 to Windows. The CRT's
 * narrow calls (getenv, fopen, opendir, stat, remove) take the ANSI code page instead, which
 * can't spell a CJK profile's home (T7) */

/* UTF-8 to UTF-16 into w (n units), strict: 0 if it isn't UTF-8 or doesn't fit */
static int wide(const char *s, wchar_t *w, int n) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) > 0;
}
/* UTF-16 to UTF-8 into s (n bytes), strict: 0 if it has an unpaired surrogate or doesn't fit */
static int narrow(const wchar_t *w, char *s, int n) {
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, s, n, NULL, NULL) > 0;
}

/* the bridge's home: RBSTEMS_TEST_HOME (the test bridge's), else LocalAppData; NULL if neither
 * is set or the value isn't valid UTF-16 */
static const char *home(void) {
    static char h[3 * 1024];
    const wchar_t *w = _wgetenv(L"RBSTEMS_TEST_HOME");
    if (!w) w = _wgetenv(L"LOCALAPPDATA");
    return w && narrow(w, h, sizeof h) ? h : NULL;
}
#define CACHE_HOME home()

static FILE *u8_fopen(const char *path, const char *mode) {
    wchar_t w[2048], m[8];
    return wide(path, w, 2048) && wide(mode, m, 8) ? _wfopen(w, m) : NULL;
}
static int u8_remove(const char *path) {
    wchar_t w[2048];
    return wide(path, w, 2048) ? _wremove(w) : -1;
}
static int u8_is_dir(const char *path) {               /* a folder (links followed) */
    wchar_t w[2048];
    struct _stat st;
    return wide(path, w, 2048) && !_wstat(w, &st) && (st.st_mode & _S_IFMT) == _S_IFDIR;
}

/* a folder's entries (FindFirstFileW), names in UTF-8; "." and ".." included, as readdir's; a
 * name that isn't valid UTF-16 is skipped */
struct u8_dirent { char d_name[3 * MAX_PATH]; };
typedef struct { HANDLE h; int pending; WIN32_FIND_DATAW f; struct u8_dirent e; } u8_dir;
static int u8_missing;                    /* u8_opendir's last NULL: the folder isn't there */
static u8_dir *u8_opendir(const char *dir) {
    char pat[2048];
    wchar_t w[2048];
    u8_missing = 0;
    if (snprintf(pat, sizeof pat, "%s\\*", dir) >= (int)sizeof pat || !wide(pat, w, 2048)) return NULL;
    u8_dir *d = malloc(sizeof *d);
    if (!d) return NULL;
    d->h = FindFirstFileW(w, &d->f);
    if (d->h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        u8_missing = e == ERROR_PATH_NOT_FOUND || e == ERROR_FILE_NOT_FOUND;
        free(d);
        return NULL;
    }
    d->pending = 1;
    return d;
}
static struct u8_dirent *u8_readdir(u8_dir *d) {
    for (;;) {
        if (!d->pending && !FindNextFileW(d->h, &d->f)) return NULL;
        d->pending = 0;
        if (narrow(d->f.cFileName, d->e.d_name, sizeof d->e.d_name)) return &d->e;
    }
}
static void u8_closedir(u8_dir *d) { FindClose(d->h); free(d); }

static char *utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = n > 0 ? malloc((size_t)n) : NULL;
    if (!s || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) { free(s); return NULL; }
    return s;
}

/* the file an address belongs to (its module's path, UTF-8; leaks: the harness exits soon) */
static const char *owner(const void *p) {
    HMODULE m = NULL;
    wchar_t w[1024];
    if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &m))
        return "?";
    DWORD n = GetModuleFileNameW(m, w, 1024);
    char *s = n && n < 1024 ? utf8(w) : NULL;
    return s ? s : "?";
}

/* the file's final path (GetFinalPathNameByHandleW: links, short names and case resolved) */
static int final_path(const char *path, wchar_t *out, DWORD n) {
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return 0;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD got = GetFinalPathNameByHandleW(h, out, n, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    return got > 0 && got < n;
}

static int same_file(const char *a, const char *b) {
    wchar_t ra[1024], rb[1024];
    return final_path(a, ra, 1024) && final_path(b, rb, 1024) && !_wcsicmp(ra, rb);
}

/* a model path as ONNX Runtime takes it: UTF-16 on Windows (one at a time) */
static const ORTCHAR_T *ort_path(const char *path) {
    static wchar_t w[1024];
    return MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) ? w : L"";
}

static uint64_t now_ns(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
static void sleep_ms(unsigned ms) { Sleep(ms); }
/* RBSTEMS_HARNESS_CACHE as UTF-8 (read as UTF-16: a CJK path), NULL if unset, "" if invalid */
static const char *harness_cache(void) {
    static char c[3 * 1024];
    const wchar_t *w = _wgetenv(L"RBSTEMS_HARNESS_CACHE");
    return !w ? NULL : narrow(w, c, sizeof c) ? c : "";      /* "": run() refuses it */
}
/* sh=CMD through _wsystem: CMD is UTF-8 here; -1 if it isn't */
static int u8_system(const char *cmd) {
    static wchar_t w[8192];
    return wide(cmd, w, 8192) ? _wsystem(w) : -1;
}
#define COUNT_UNOPENED (u8_missing ? 0 : -1)
/* both the CRT's environment (getenv, the bridge's) and Windows' own */
static void set_env(const char *k, const char *v) { _putenv_s(k, v); SetEnvironmentVariableA(k, v); }

/* the entry carries no type: the path's own */
static int is_dir(const struct u8_dirent *e, const char *path) { (void)e; return u8_is_dir(path); }

/* the file's id (NTFS: its file index; exFAT gives one too), 0 if none */
static unsigned long long file_id(const char *path) {
    wchar_t w[1024];
    BY_HANDLE_FILE_INFORMATION i;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return 0;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = GetFileInformationByHandle(h, &i);
    CloseHandle(h);
    return ok ? ((unsigned long long)i.nFileIndexHigh << 32) | i.nFileIndexLow : 0;
}

/* not in the UCRT */
static char *strndup_(const char *s, size_t n) {
    size_t l = strnlen(s, n);
    char *d = malloc(l + 1);
    if (d) { memcpy(d, s, l); d[l] = 0; }
    return d;
}
#define strndup strndup_
static void *memmem_(const void *h, size_t hn, const void *nd, size_t nn) {
    for (size_t i = 0; nn <= hn && i <= hn - nn; i++)
        if (!memcmp((const char *)h + i, nd, nn)) return (void *)((const char *)h + i);
    return NULL;
}
#define memmem memmem_
#else
#define LOG_TAIL "/Library/Logs/rbstemsplus/bridge.log"
#define CACHE_TAIL "/Library/Caches/rbstemsplus"

static const char *home(void) { return getenv("RBSTEMS_TEST_HOME") ? getenv("RBSTEMS_TEST_HOME") : getenv("HOME"); }
#define CACHE_HOME getenv("HOME")

/* the file layer: the C library's own (paths are UTF-8 already) */
#define u8_fopen fopen
#define u8_remove remove
#define u8_dirent dirent
typedef DIR u8_dir;
#define u8_opendir opendir
#define u8_readdir readdir
#define u8_closedir closedir
#define harness_cache() getenv("RBSTEMS_HARNESS_CACHE")
#define u8_system system
#define COUNT_UNOPENED 0                          /* a folder opendir refuses counts 0 */

/* the file an address belongs to */
static const char *owner(const void *p) {
    Dl_info i;
    return p && dladdr(p, &i) ? i.dli_fname : "?";
}

static int same_file(const char *a, const char *b) {
    char ra[1024], rb[1024];
    return realpath(a, ra) && realpath(b, rb) && !strcmp(ra, rb);
}

static const ORTCHAR_T *ort_path(const char *path) { return path; }
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void sleep_ms(unsigned ms) { usleep(ms * 1000); }
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
static int is_dir(const struct u8_dirent *e, const char *path) { (void)path; return e->d_type == DT_DIR; }
static unsigned long long file_id(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : (unsigned long long)st.st_ino;
}
#endif

static int failed;
static int quiet;                         /* bench: no line per chunk */

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed = 1;
}

/* ---------------------------------------------------------------- load, passthrough, missing */

static int load(const char *real) {
    const OrtApiBase *b = OrtGetApiBase();
    check(b != NULL, "OrtGetApiBase returns a base");
    if (!b) return 1;
    const char *v = b->GetVersionString();
    check(!strncmp(v, "1.18.", 5), "the version is the real library's 1.18.x");
    check(same_file(owner((const void *)b), owner((const void *)OrtGetApiBase)), "the base is the bridge's");
    const OrtApi *a = b->GetApi(ORT_API_VERSION);
    check(a != NULL, "GetApi(ORT_API_VERSION) returns an OrtApi");
    if (!a) return 1;
    const char *bridge = owner((const void *)OrtGetApiBase);
    check(same_file(owner((const void *)a->Run), bridge), "Run is hooked");
    check(same_file(owner((const void *)a->CreateSession), bridge), "CreateSession is hooked");
    check(same_file(owner((const void *)a->ReleaseSession), bridge), "ReleaseSession is hooked");
    check(same_file(owner((const void *)a->CreateEnv), real), "CreateEnv is the real library's");
    check(same_file(owner((const void *)a->CreateTensorAsOrtValue), real), "CreateTensorAsOrtValue is the real library's");
    check(b->GetApi(ORT_API_VERSION) == a, "GetApi answers the same table twice");
    OrtEnv *env = NULL;
    OrtSessionOptions *so = NULL;
    OrtStatus *st = a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "rbstems-tests", &env);
    check(!st && env, "CreateEnv works");
    st = a->CreateSessionOptions(&so);
    check(!st && so, "CreateSessionOptions works");
    if (so) {
        st = OrtSessionOptionsAppendExecutionProvider_CPU(so, 1);
        check(!st, "the CPU provider passes through to the real library");
        if (st) a->ReleaseStatus(st);
        a->ReleaseSessionOptions(so);
    }
    if (env) a->ReleaseEnv(env);
    return failed;
}

#if defined(_WIN32)
/* the Windows bridge always answers with its own base (bridge_base, forwarding the version) */
static int passthrough(const char *fake) {
    const OrtApiBase *b = OrtGetApiBase();
    check(b != NULL, "OrtGetApiBase returns a base");
    if (!b) return 1;
    const char *bridge = owner((const void *)OrtGetApiBase);
    check(same_file(owner((const void *)b), bridge), "the base is the bridge's, forwarding");
    check(!strcmp(b->GetVersionString(), "1.17.0"), "the version is the fake's 1.17.0");
    const OrtApi *a = b->GetApi(ORT_API_VERSION);
    check(a && same_file(owner((const void *)a), fake), "GetApi is the fake's, untouched");
    OrtStatus *s = OrtSessionOptionsAppendExecutionProvider_CPU(NULL, 1);
    check(s && same_file(owner((const void *)s), fake), "the CPU provider passes through");
    s = OrtSessionOptionsAppendExecutionProvider_DML(NULL, 0);
    check(s && same_file(owner((const void *)s), fake), "the DirectML provider passes through");
    s = OrtSessionOptionsAppendExecutionProviderEx_DML(NULL, NULL, NULL);
    check(s && same_file(owner((const void *)s), fake), "the DirectML Ex provider passes through");
    return failed;
}

/* no real library: the refusal stub (never NULL to rekordbox, r4:I46), every slot the bridge's */
static int missing(void) {
    const OrtApiBase *b = OrtGetApiBase();
    const char *bridge = owner((const void *)OrtGetApiBase);
    check(b && same_file(owner((const void *)b), bridge), "OrtGetApiBase returns the bridge's stub without a real library");
    if (!b) return 1;
    check(!strcmp(b->GetVersionString(), "1.18.0"), "the stub says 1.18.0");
    const OrtApi *a = b->GetApi(ORT_API_VERSION);
    check(a && same_file(owner((const void *)a->Run), bridge) && same_file(owner((const void *)a->CreateEnv), bridge),
          "GetApi gives the stub table (every slot the bridge's)");
    OrtStatus *s = OrtSessionOptionsAppendExecutionProvider_CPU(NULL, 1);
    check(s && same_file(owner((const void *)s), bridge), "the CPU provider returns the stub's failed status");
    s = OrtSessionOptionsAppendExecutionProvider_DML(NULL, 0);
    check(s && same_file(owner((const void *)s), bridge), "the DirectML provider returns the stub's failed status");
    return failed;
}
#else
static int passthrough(const char *fake) {
    const OrtApiBase *b = OrtGetApiBase();
    check(b != NULL, "OrtGetApiBase returns a base");
    if (!b) return 1;
    check(same_file(owner((const void *)b), fake), "the base is the fake library's own");
    check(!strcmp(b->GetVersionString(), "1.17.0"), "the version is the fake's 1.17.0");
    const OrtApi *a = b->GetApi(ORT_API_VERSION);
    check(a && same_file(owner((const void *)a), fake), "GetApi is the fake's, untouched");
    OrtStatus *s = OrtSessionOptionsAppendExecutionProvider_CPU(NULL, 1);
    check(s && same_file(owner((const void *)s), fake), "the CPU provider passes through");
    s = OrtSessionOptionsAppendExecutionProvider_CoreML(NULL, 0);
    check(s && same_file(owner((const void *)s), fake), "the CoreML provider passes through");
    return failed;
}

static int missing(void) {
    check(OrtGetApiBase() == NULL, "OrtGetApiBase returns NULL without a real library");
    check(OrtSessionOptionsAppendExecutionProvider_CPU(NULL, 1) == NULL, "the CPU provider returns without crashing");
    check(OrtSessionOptionsAppendExecutionProvider_CoreML(NULL, 0) == NULL, "the CoreML provider returns without crashing");
    return failed;
}
#endif

/* ---------------------------------------------------------------- run */

#define K 2.475f                          /* the test model's factor (make_model.py) */
#ifndef L
#define L 88200                                 /* 2 s: 20 summary frames (perfgate.sh: rekordbox's 485,100) */
#endif
#define SRC (L + 1000)
#ifndef BURST
#define BURST 0.05
#endif

/* deterministic stereo "music": filtered noise and a tone under a slowly moving envelope */
static void source(unsigned seed, float *l, float *r) {
    uint32_t s = seed * 2654435761u + 1;
    double lp = 0, lp2 = 0;
    for (int i = 0; i < SRC; i++) {
        s = s * 1664525u + 1013904223u;
        double n1 = (s >> 8) / 8388608.0 - 1.0;
        s = s * 1664525u + 1013904223u;
        double n2 = (s >> 8) / 8388608.0 - 1.0;
        lp += 0.2 * (n1 - lp);
        lp2 += 0.2 * (n2 - lp2);
        double t = i / 44100.0;
        double env = 0.6 + 0.4 * sin(2 * M_PI * (0.7 + 0.1 * seed) * t);
        double tone = 0.3 * sin(2 * M_PI * (220.0 + 20 * seed) * t);
        l[i] = (float)(0.3 * env * (lp + tone));
        r[i] = (float)(0.3 * env * (0.8 * lp + 0.3 * lp2 + tone));
    }
}

/* the chunk "what" as planar [2][L]. The source is shifted and scaled so that its first L samples
 * have mean 0 and std 0.1 (as the bridge measures them): the test model's stems, xt * std + mean
 * = 0.2475 * mix each, then add up to 0.99 * mix, a residual of -40 dB, like a good model. */
static unsigned nseed;                    /* "n<N>": unrelated audio N (a miss and a write each) */

static void chunk(char what, float *mix) {
    static float l[SRC], r[SRC];
    source(what == 'n' ? 1000 + nseed : what == 'd' ? 7 : what >= 'e' && what <= 'g' ? 11 + 2 * (unsigned)(what - 'e') : 1, l, r);
    double sum = 0, ss = 0;
    for (int i = 0; i < L; i++) sum += (double)l[i] + r[i];
    double mean = sum / (2 * L);
    for (int i = 0; i < L; i++) ss += (l[i] - mean) * (l[i] - mean) + (r[i] - mean) * (r[i] - mean);
    double scale = 0.1 / sqrt(ss / (2 * L - 1));
    for (int i = 0; i < SRC; i++) {
        l[i] = (float)((l[i] - mean) * scale);
        r[i] = (float)((r[i] - mean) * scale);
    }
    int off = what == 'b' ? 300 : 0;
    memcpy(mix, l + off, L * sizeof(float));
    memcpy(mix + L, r + off, L * sizeof(float));
    if (what == 'c')                                    /* 0.5-1.5 s: a quiet 1 kHz tone, left only */
        for (int i = 22050; i < 66150; i++) mix[i] += (float)(BURST * sin(2 * M_PI * 1000.0 * i / 44100.0));
}

/* the files named *EXT under dir (5-letter EXT), or -1: a folder there that can't be listed
 * (Windows: one that exists; review BX4). Every caller fails on -1 */
static int count_ext(const char *dir, const char *ext) {
    int n = 0;
    u8_dir *d = u8_opendir(dir);
    if (!d) return COUNT_UNOPENED;
    struct u8_dirent *e;
    while ((e = u8_readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t len = strlen(e->d_name);
        if (is_dir(e, p)) {
            int k = count_ext(p, ext);
            if (k < 0) { n = -1; break; }
            n += k;
        } else if (len > 5 && !strcmp(e->d_name + len - 5, ext)) n++;
    }
    u8_closedir(d);
    return n;
}
static int count_flac(const char *dir) { return count_ext(dir, ".flac"); }
#if defined(_WIN32)
/* BX5: whether cache holds more entries than at the first GPU mark (at_mark; -1: none yet) and
 * the unmarked misses since then: a marked run was stored. Says so */
static int marked_stored(const char *cache, int at_mark, int unmarked) {
    if (at_mark < 0) return 0;
    int n = count_flac(cache);
    if (n >= 0 && n <= at_mark + unmarked) return 0;
    printf("FAIL gpu: a GPU-marked run was stored (%d entries; %d at the first mark, %d unmarked misses since)\n", n, at_mark, unmarked);
    return 1;
}
#define MARKED_STORED() marked_stored(cache, flac_marked, misses - misses_marked)
#else
#define MARKED_STORED() 0
#endif

/* the spectrogram part of a cached answer for a source whose bin 0 is c in every frame (the rest
 * zero), as Demucs's _ispec rebuilds it: c / 64 (normalized inverse FFT) times, at each kept
 * sample, the sum of the Hann windows of the F real frames over the sum of the squared windows
 * of all F + 4 frames (two empty ones on each side) */
static void spec_shape(int F, double *out) {
    const int N = 4096, H = 1024, start = N / 2 + H / 2 * 3;
    for (int n = 0; n < L; n++) {
        double s1 = 0, s2 = 0;
        long at = start + n;
        for (int t = 0; t < F + 4; t++) {
            long m = at - (long)t * H;
            if (m < 0 || m >= N) continue;
            double w = 0.5 - 0.5 * cos(2 * M_PI * m / N);
            s2 += w * w;
            if (t >= 2 && t < F + 2) s1 += w;
        }
        out[n] = s1 / s2 / 64.0;
    }
}

/* the bridge's log, as a string ("" if none) */
static char *log_text(void) {
    const char *h = home();
    char p[1200];
    snprintf(p, sizeof p, "%s" LOG_TAIL, h ? h : "");
    FILE *f = u8_fopen(p, "rb");
    if (!f) return strdup("");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    n = (long)fread(b, 1, (size_t)n, f);
    b[n] = 0;
    fclose(f);
    return b;
}

/* waits until the bridge has dealt with the new session's index (its "summaries" line), unless it
 * didn't take the session (", not cached") */
static void wait_ready(void) {
    int tenths = getenv("RBSTEMS_HARNESS_READY") ? atoi(getenv("RBSTEMS_HARNESS_READY")) * 10 : 100;
    for (int t = 0; t < tenths; t++) {
        char *b = log_text(), *s = NULL, *p = b;
        while ((p = strstr(p, "Demucs session opened"))) s = p++;
        char *eol = s ? strchr(s, '\n') : NULL;
        const char *want = getenv("RBSTEMS_HARNESS_WAIT_FOR") ? getenv("RBSTEMS_HARNESS_WAIT_FOR") : "  summaries ";
        int done = !s || (eol && memmem(s, (size_t)(eol - s), ", not cached", 12)) || (eol && strstr(eol, want))
                   || (getenv("RBSTEMS_HARNESS_WAIT_FOR") && strstr(b, want));
        free(b);
        if (done) return;
        sleep_ms(100);
    }
    printf("FAIL no \"summaries\" line from the bridge in time\n");
}

/* WHAT*N and m*N expanded, mseed=K taken; a new array (its strings leak: the harness exits soon) */
static char **expand(int *argc, char **argv) {
    size_t n = 0, cap = 64;
    char **out = malloc(cap * sizeof *out);
    unsigned mseed = 0;
    for (int k = 0; k < *argc; k++) {
        const char *a = argv[k], *star = strrchr(a, '*');
        long times = 1;
        if (!strncmp(a, "mseed=", 6)) { mseed = (unsigned)atoi(a + 6); continue; }
        if (!strcmp(a, "quiet")) { quiet = 1; continue; }
        if (strncmp(a, "sh=", 3) && star && star[1] && strspn(star + 1, "0123456789") == strlen(star + 1)) times = atol(star + 1);
        else star = NULL;                               /* the token as it is, whatever its length */
        char *base = star ? strndup(a, (size_t)(star - a)) : strdup(a);
        for (long t = 0; t < times; t++) {
            if (n + 1 >= cap) out = realloc(out, (cap *= 2) * sizeof *out);
            if (!strcmp(base, "m") && star) {
                char nn[32];
                snprintf(nn, sizeof nn, "n%u", mseed++);
                out[n++] = strdup(nn);
            } else out[n++] = strdup(base);
        }
        free(base);
    }
    out[n] = NULL;
    *argc = (int)n;
    return out;
}

static int run(const char *model, int argc, char **argv, int pioneer) {
    argv = expand(&argc, argv);
    int said_model = 0, said_cache = 0, said_wrong = 0;
    const OrtApiBase *b = OrtGetApiBase();
    const OrtApi *a = b ? b->GetApi(ORT_API_VERSION) : NULL;
    if (!a) { check(0, "an OrtApi"); return 1; }
    OrtEnv *env = NULL;
    OrtSessionOptions *so = NULL;
    OrtSession *s = NULL;
    OrtMemoryInfo *mi = NULL;
    OrtStatus *st = a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "rbstems-tests", &env);
    if (!st) st = a->CreateSessionOptions(&so);
    if (!st) st = a->SetIntraOpNumThreads(so, 1);
    if (!st) st = a->CreateSession(env, ort_path(model), so, &s);
    if (!st) st = a->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi);
    if (st) { printf("FAIL session: %s\n", a->GetErrorMessage(st)); return 1; }
    wait_ready();
    char cache[1200];
    const char *hc = harness_cache();
    if (hc && !hc[0]) { printf("FAIL run: RBSTEMS_HARNESS_CACHE is empty or not valid UTF-16\n"); return 1; }
    if (hc) snprintf(cache, sizeof cache, "%s", hc);
    else snprintf(cache, sizeof cache, "%s" CACHE_TAIL, CACHE_HOME);
    static float mix[2 * L];
    static double shape[L];
    float mag1 = 1.0f, *mag = &mag1;
    int64_t mdims[3] = {1, 2, L}, gdims[4] = {1, 1, 1, 1};
    int misses = 0, F = (L + 1023) / 1024;
    double w[4] = {0, 0, 0, 0}, gain = 1;
    int desc0 = 0;                                       /* drain: the entries there before this run */
#if defined(_WIN32)
    char gpu = '-';                                      /* gpu=in|out|off: BW1's device check */
    int64_t xdims[8];
    size_t xnd = 0;                                      /* x's shape, from the last run ORT allocated it */
    int flac_marked = -1, misses_marked = 0;             /* BX5: the entries and misses at the first mark */
#endif
    for (int k = 0; k < argc; k++)
        if (!strcmp(argv[k], "drain")) {
            if ((desc0 = count_ext(cache, ".desc")) < 0) { printf("FAIL drain: can't list %s\n", cache); return 1; }
            break;
        }
    for (int k = 0; k < argc; k++) {
        if (!strncmp(argv[k], "gain=", 5)) {
            gain = atof(argv[k] + 5);
            continue;
        }
        if (!strncmp(argv[k], "w=", 2)) {
            sscanf(argv[k] + 2, "%lf,%lf,%lf,%lf", &w[0], &w[1], &w[2], &w[3]);
            continue;
        }
        if (!strncmp(argv[k], "frames=", 7)) {
            F = atoi(argv[k] + 7);
            continue;
        }
#if defined(_WIN32)
        if (!strncmp(argv[k], "gpu=", 4)) {              /* in, out (x pre-allocated) or off */
            gpu = !strcmp(argv[k] + 4, "in") ? 'i' : !strcmp(argv[k] + 4, "out") ? 'o' : '-';
            continue;
        }
#endif
        if (pioneer) {                                   /* mag [1, 4, 2048, F]: bin 0 of L.re and R.re at 1 */
            static float *buf;
            free(buf);
            buf = malloc(sizeof(float) * 4 * 2048 * (size_t)F);
            for (size_t i = 0; i < 4 * 2048 * (size_t)F; i++) buf[i] = (float)(0.37 * sin(0.001 * (double)i + 1.0));
            for (int f = 0; f < F; f++) buf[(size_t)0 * 2048 * F + f] = buf[(size_t)2 * 2048 * F + f] = 1.0f;
            mag = buf;
            gdims[1] = 4; gdims[2] = 2048; gdims[3] = F;
            spec_shape(F, shape);
        }
        if (!strcmp(argv[k], "wait")) {               /* the writer stores misses in the background */
            int n = 0, nd = 0, nf = 0;
            for (int t = 0; t < 300 && (n = count_flac(cache)) >= 0 && n < misses; t++) sleep_ms(100);
            /* ...and then its .desc (each FLAC's, polled: a slow disk image under load can take
             * longer than a fixed pause), and the index entry the writer adds after it */
            for (int t = 0; t < 50 && n >= 0 && (nd = count_ext(cache, ".desc")) >= 0 && (nf = count_flac(cache)) >= 0 && nd < nf; t++)
                sleep_ms(100);
            if (n < 0 || nd < 0 || nf < 0) { printf("FAIL wait: can't list %s\n", cache); return 1; }
            sleep_ms(300);
            printf("stored %d\n", n);
            if (MARKED_STORED()) return 1;
            continue;
        }
        if (!strcmp(argv[k], "drain")) {              /* the writer done with every miss so far (its .desc, */
            int t = 0, nd;                                  /* written last), polled often: misses paced */
            while ((nd = count_ext(cache, ".desc")) >= 0 && nd - desc0 < misses && t++ < 1000) sleep_ms(10);
            if (nd < 0) { printf("FAIL drain: can't list %s\n", cache); return 1; }
            if (t > 1000) { printf("FAIL drain: the writer didn't store miss %d in 10 s\n", misses); return 1; }
            if (MARKED_STORED()) return 1;
            continue;
        }
        if (!strncmp(argv[k], "free=", 5)) {             /* after a sleep or wait: the writer is idle */
            set_env("RBSTEMS_TEST_FREE_GB", argv[k] + 5);
            continue;
        }
        if (!strncmp(argv[k], "sh=", 3)) {               /* a shell command between chunks (moving the folder...) */
            if (u8_system(argv[k] + 3) != 0) printf("FAIL sh: %s\n", argv[k] + 3);
            continue;
        }
        if (!strcmp(argv[k], "sleep")) {
            sleep_ms(1000);
            int n = count_flac(cache);
            if (n < 0) { printf("FAIL sleep: can't list %s\n", cache); return 1; }
            printf("stored %d\n", n);
            if (MARKED_STORED()) return 1;
            continue;
        }
        char what = argv[k][0];
        if (what == 'n') nseed = (unsigned)atoi(argv[k] + 1);
        chunk(what, mix);
        if (gain != 1)
            for (int i = 0; i < 2 * L; i++) mix[i] = (float)(mix[i] * gain);
        OrtValue *in[2] = {NULL, NULL}, *out[2] = {NULL, NULL};
        const char *in_names[] = {"mix", "mag"}, *out_names[] = {"x", "xt"};
        st = a->CreateTensorWithDataAsOrtValue(mi, mix, sizeof mix, mdims, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[0]);
        if (!st) st = a->CreateTensorWithDataAsOrtValue(mi, mag, sizeof(float) * (size_t)(gdims[1] * gdims[2] * gdims[3]), gdims, 4,
                                                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1]);
#if defined(_WIN32)
        /* BW1: the TEST bridge sees the value named in RBSTEMS_TEST_GPU_VALUE as GPU memory: mix
         * (gpu=in) or a pre-allocated x (gpu=out), filled with a pattern no answer has. The run
         * must be the model's own, untouched: never served, never stored */
        static float *xbuf;
        size_t xcount = 1;
        for (size_t i = 0; i < xnd; i++) xcount *= (size_t)xdims[i];
        if (!st && gpu == 'o' && !xnd) { printf("FAIL run %c: gpu=out before a run gave x's shape\n", what); return 1; }
        if (!st && gpu == 'o') {
            free(xbuf);
            xbuf = malloc(sizeof(float) * xcount);
            for (size_t i = 0; i < xcount; i++) xbuf[i] = -12345.0f;
            st = a->CreateTensorWithDataAsOrtValue(mi, xbuf, sizeof(float) * xcount, xdims, xnd, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &out[0]);
        }
        if (gpu != '-' && flac_marked < 0) {             /* BX5: what is stored before the first mark */
            if ((flac_marked = count_flac(cache)) < 0) { printf("FAIL run %c: can't list %s\n", what, cache); return 1; }
            misses_marked = misses;
        }
        char gv[32] = "";
        if (gpu != '-') snprintf(gv, sizeof gv, "%llx", (unsigned long long)(uintptr_t)(gpu == 'i' ? in[0] : out[0]));
        set_env("RBSTEMS_TEST_GPU_VALUE", gv);
#endif
        if (!st) st = a->Run(s, NULL, in_names, (const OrtValue *const *)in, 2, out_names, 2, out);
        if (st) { printf("FAIL run %c: %s\n", what, a->GetErrorMessage(st)); return 1; }
#if defined(_WIN32)
        if (gpu != 'o') {                                /* x's shape, for a later gpu=out */
            OrtTensorTypeAndShapeInfo *ti = NULL;
            if (!a->GetTensorTypeAndShape(out[0], &ti)) {
                if (a->GetDimensionsCount(ti, &xnd) || xnd > 8 || a->GetDimensions(ti, xdims, xnd)) xnd = 0;
                a->ReleaseTensorTypeAndShapeInfo(ti);
            }
        }
#endif
        float *x = NULL, *xt = NULL;
        if (a->GetTensorMutableData(out[0], (void **)&x) || a->GetTensorMutableData(out[1], (void **)&xt)) {
            printf("FAIL run %c: no output data\n", what);
            return 1;
        }
        size_t xn = pioneer ? (size_t)16 * 2048 * F : 4;
        int xzero = 1;
        for (size_t i = 0; i < xn; i++) xzero &= x[i] == 0.0f;
        if (pioneer) {
            /* the model's own answer: x = mag times the mask (w at bin 0 of the real parts), xt = tile(mix) * K */
            int xmodel = 1, xtmodel = 1, near = 1;
            for (int s = 0; s < 4 && xmodel; s++)
                for (int ch = 0; ch < 4 && xmodel; ch++)
                    for (int b = 0; b < 2048 && xmodel; b++)
                        for (int f = 0; f < F; f++) {
                            float want = (b == 0 && (ch == 0 || ch == 2)) ? (float)w[s] * mag[(size_t)ch * 2048 * F + f] : 0.0f;
                            float got = x[(((size_t)s * 4 + ch) * 2048 + b) * F + f];
                            if (!(got == want || (isnan(got) && isnan(want)))) { xmodel = 0; break; }
                        }
            double worst = 0;
            for (int ch = 0; ch < 8; ch++)
                for (int i = 0; i < L; i++) {
                    float m = mix[(ch & 1) * L + i] * K, got = xt[ch * L + i];
                    if (got != m) xtmodel = 0;
                    double want = m + w[ch / 2] * shape[i] / 0.1;   /* the cached stems, (stems - mean) / std */
                    double e = fabs(got - want);
                    if (!(e <= worst)) worst = e;
                }
            near = what == 'b' ? 1 : worst < 1e-3;
            const char *says = xmodel && xtmodel ? "model" : xzero && near ? "cache" : "wrong";
            if (!quiet) printf("%c %s (x zero %d, worst %.2e)\n", what, says, xzero, worst);
            said_model += says[0] == 'm';
            said_cache += says[0] == 'c';
            said_wrong += says[0] == 'w';
#if defined(_WIN32)
            if (gpu != '-' && !(xmodel && xtmodel)) { printf("FAIL run %c: a GPU-marked run was served\n", what); return 1; }
            if (gpu == '-' && xmodel && xtmodel) misses++;      /* a marked run is never stored (BX5) */
#else
            if (xmodel && xtmodel) misses++;
#endif
            a->ReleaseValue(in[0]); a->ReleaseValue(in[1]); a->ReleaseValue(out[0]); a->ReleaseValue(out[1]);
            continue;
        }
        /* the model's answer is exactly tile(mix) * K; a cached one differs by the 16-bit
         * rounding; a near one (b) is the shifted entry's */
        int exact = 1, finite = 1;
        double err = 0, ref = 0;
        for (int ch = 0; ch < 8; ch++)
            for (int i = 0; i < L; i++) {
                float want = mix[(ch & 1) * L + i] * K, got = xt[ch * L + i];
                if (got != want) exact = 0;
                if (!isfinite(got)) finite = 0;
                err += (double)(got - want) * (got - want);
                ref += (double)want * want;
            }
        double rel = sqrt(err / ref);
        if (!quiet) printf("%c %s (x zero %d, finite %d, error %.2e)\n", what, exact ? "model" : "cache", xzero, finite, rel);
        said_model += exact;
        said_cache += !exact;
#if defined(_WIN32)
        if (gpu != '-' && !exact) { printf("FAIL run %c: a GPU-marked run was served\n", what); return 1; }
        if (gpu == '-' && exact) misses++;                  /* a marked run is never stored (BX5) */
#else
        if (exact) misses++;
#endif
        a->ReleaseValue(in[0]);
        a->ReleaseValue(in[1]);
        a->ReleaseValue(out[0]);
        a->ReleaseValue(out[1]);
    }
    a->ReleaseSession(s);
    a->ReleaseMemoryInfo(mi);
    a->ReleaseSessionOptions(so);
    a->ReleaseEnv(env);
    if (quiet) printf("ran %d: %d model, %d cache, %d wrong\n", said_model + said_cache + said_wrong, said_model, said_cache, said_wrong);
    return 0;
}

/* ---------------------------------------------------------------- bench: sessions, start */

/* the model checksums' memo folder: the bridge's home's default cache folder */
static void memo_folder(char *out, size_t n) {
    const char *h = home();
    snprintf(out, n, "%s" CACHE_TAIL, h ? h : "");
}

/* the inode (Windows: the file id) of the (one) memo there, 0 if none; nomemo: deletes them all */
static unsigned long long memos(int forget) {
    char dir[1200], p[1500];
    memo_folder(dir, sizeof dir);
    u8_dir *d = u8_opendir(dir);
    unsigned long long ino = 0;
    if (!d) return 0;
    struct u8_dirent *e;
    while ((e = u8_readdir(d))) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, "model-", 6) || len < 7 + 6 || strcmp(e->d_name + len - 7, ".sha256")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (forget) u8_remove(p);
        else ino = file_id(p);
    }
    u8_closedir(d);
    return ino;
}

static int sessions(const char *model, int n, int nomemo) {
    const OrtApiBase *b = OrtGetApiBase();
    const OrtApi *a = b ? b->GetApi(ORT_API_VERSION) : NULL;
    if (!a) { check(0, "an OrtApi"); return 1; }
    OrtEnv *env = NULL;
    OrtSessionOptions *so = NULL;
    OrtStatus *st = a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "rbstems-tests", &env);
    if (!st) st = a->CreateSessionOptions(&so);
    if (!st) st = a->SetIntraOpNumThreads(so, 1);
    if (st) { printf("FAIL setup: %s\n", a->GetErrorMessage(st)); return 1; }
    for (int i = 0; i < n; i++) {
        OrtSession *s = NULL;
        if (nomemo) memos(1);
        uint64_t t0 = now_ns();
        st = a->CreateSession(env, ort_path(model), so, &s);
        uint64_t t1 = now_ns();
        if (st) { printf("FAIL session %d: %s\n", i, a->GetErrorMessage(st)); return 1; }
        wait_ready();
        printf("session %d outer_ns=%llu memo_ino=%llu\n", i, (unsigned long long)(t1 - t0), memos(0));
        a->ReleaseSession(s);
    }
    a->ReleaseSessionOptions(so);
    a->ReleaseEnv(env);
    return failed;
}

static int start(void) {
    uint64_t t0 = now_ns();
    const OrtApiBase *b = OrtGetApiBase();
    const OrtApi *a = b ? b->GetApi(ORT_API_VERSION) : NULL;
    uint64_t t1 = now_ns();
    int hooked = a && same_file(owner((const void *)a->Run), owner((const void *)OrtGetApiBase));
    printf("start outer_ns=%llu hooked=%d\n", (unsigned long long)(t1 - t0), hooked);
    return !hooked;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
#if defined(_WIN32)
    /* BX4: argv in UTF-8, from the UTF-16 command line (the CRT's argv is in the ANSI code page,
     * which can't spell a CJK path), strict: an unpaired surrogate is refused */
    int wn = 0;
    wchar_t **wa = CommandLineToArgvW(GetCommandLineW(), &wn);
    char **u8 = wa && wn == argc ? calloc((size_t)wn + 1, sizeof *u8) : NULL;
    for (int i = 0; u8 && i < wn; i++) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wa[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0 || !(u8[i] = malloc((size_t)n)) || !narrow(wa[i], u8[i], n)) u8 = NULL;   /* leaks: exits */
    }
    if (wa) LocalFree(wa);
    if (!u8) {
        fprintf(stderr, "harness: the command line isn't valid UTF-16\n");
        return 2;
    }
    argv = u8;
#endif
    if (argc == 3 && !strcmp(argv[1], "load")) return load(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "passthrough")) return passthrough(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "missing")) return missing();
    if (argc >= 3 && !strcmp(argv[1], "run")) return run(argv[2], argc - 3, argv + 3, 0);
    if (argc >= 3 && !strcmp(argv[1], "prun")) return run(argv[2], argc - 3, argv + 3, 1);
    if (argc >= 4 && !strcmp(argv[1], "bench") && (!strcmp(argv[2], "run") || !strcmp(argv[2], "prun"))) {
        quiet = 1;
        return run(argv[3], argc - 4, argv + 4, argv[2][0] == 'p');
    }
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "bench") && !strcmp(argv[2], "sessions"))
        return sessions(argv[3], atoi(argv[4]), argc == 6 && !strcmp(argv[5], "nomemo"));
    if (argc == 3 && !strcmp(argv[1], "bench") && !strcmp(argv[2], "start")) return start();
    fprintf(stderr, "usage: see the top of harness.c\n");
    return 2;
}
