/* readable.c on Windows (helper.md §1, T14, the UNC ruling), for the VM: rbsp-win-readable-test.exe,
 * built with readable.c's test gate (RBSP_READABLE_TEST: readable_test_step, defined here).
 *
 *   rbsp-win-readable-test.exe DIR                 the cases a scratch folder DIR (on the system
 *                                                  drive) can give: a file, a missing one, a
 *                                                  folder, a drive that isn't there, the "another
 *                                                  drive" rule; then, through the gate, the timed
 *                                                  job's paths (review B8): give-up and a late
 *                                                  finish freeing its slot, the download sentence,
 *                                                  a placeholder not remembered, the caps by kind
 *                                                  with a system-drive track still read, the
 *                                                  remembered keys' spellings and widening, a
 *                                                  network error remembered; and the decode's reads
 *                                                  bounded (a pipe read nobody answers)
 *   rbsp-win-readable-test.exe --path P [SECS]     readable(P, SECS: default 2) once, then again,
 *                                                  each state, its time and its sentence, and
 *                                                  readable_may_wait(P); exits 1 unless both are
 *                                                  ok or asking
 *   rbsp-win-readable-test.exe --seq SECS P...     readable() on each P in turn, in one process (what
 *                                                  is remembered carries over): "N: STATE after MS ms:
 *                                                  WHY" per path
 *
 * The gate touches only made-up paths (servers named blk* and neterr*, the drive Q:, a volume GUID
 * with no volume) and scratch files whose names say what to do (-slow-: the first step waits;
 * -ph-: looks like a placeholder, its download waits): it never reaches the network. Exits 1 on a
 * failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "path_rules.h"
#include "plat_helper.h"
#include "readable.h"

static int fails;
#define CHECK(c, ...)                      \
    do {                                   \
        if (c) printf("ok   ");            \
        else { printf("FAIL "); fails++; } \
        printf(__VA_ARGS__);               \
        printf("\n");                      \
    } while (0)

static const char *name(read_state s) {
    return s == READ_OK ? "ok" : s == READ_ABSENT ? "absent" : s == READ_ASKING ? "asking" : s == READ_DENIED ? "denied" : "failed";
}

/* ---------------------------------------------------------------- readable.c's test gate */

static HANDLE gate;                             /* manual reset: blocked steps wait for it; NULL: the gate is off */
static volatile LONG looks;                     /* the gate's first-step calls: a job started */
static volatile LONG fetches;                   /* RD_FETCH steps: a download about to start */
static volatile LONG bigslow_hyd, off_hyd;      /* RD_HYDRATE steps of -bigslow- and -off- files */
static volatile LONG gone_hyd, mv_hyd;          /* RD_HYDRATE steps of -offgone- and -mv- files */

static int ci_prefix(const char *p, const char *pre) {
    for (; *pre; p++, pre++)
        if ((*p >= 'A' && *p <= 'Z' ? *p | 0x20 : *p) != *pre && !((*p == '/' || *p == '\\') && *pre == '\\')) return 0;
    return 1;
}

static int made_up(const char *p) {             /* a path the gate blocks at its first step, then ENOENT */
    return ci_prefix(p, "\\\\blk") || ci_prefix(p, "\\\\?\\unc\\blk") || ci_prefix(p, "q:") || ci_prefix(p, "\\\\?\\q:")
           || ci_prefix(p, "\\\\?\\volume{");
}

int readable_test_step(int step, const char *path, plat_where *w) {
    if (!gate) return 0;                        /* --path and --seq: real paths only (review HR3) */
    if (step == 0 && !w) {                      /* RD_LOOK, before the attributes */
        InterlockedIncrement(&looks);
        if (ci_prefix(path, "\\\\neterr")) return ENETUNREACH;
        if (made_up(path)) { WaitForSingleObject(gate, INFINITE); return ENOENT; }
        if (strstr(path, "-slow-")) WaitForSingleObject(gate, INFINITE);
        return 0;
    }
    if (step == 0 && w && (strstr(path, "-ph-") || strstr(path, "-big-") || strstr(path, "-bigslow-") || strstr(path, "-mv-")))
        w->placeholder = 1;                    /* after the attributes */
    if (step == 0 && w && (strstr(path, "-off-") || strstr(path, "-offgone-"))) w->offline = 1;
    if (step == 2) {                            /* RD_FETCH */
        InterlockedIncrement(&fetches);
        if (strstr(path, "-ph-")) WaitForSingleObject(gate, INFINITE);
    }
    if (step == 3 && w) {                       /* RD_HYDRATE: the read-on loop's attribute checks */
        if (strstr(path, "-big-")) w->placeholder = 1;   /* still flagged: read to its end */
        if (strstr(path, "-bigslow-")) {
            w->placeholder = 1;
            if (InterlockedIncrement(&bigslow_hyd) == 2) WaitForSingleObject(gate, INFINITE);
        }
        if (strstr(path, "-off-")) { w->offline = 1; InterlockedIncrement(&off_hyd); }
        if (strstr(path, "-offgone-")) { InterlockedIncrement(&gone_hyd); return ENOENT; }
        if (strstr(path, "-mv-")) {
            w->placeholder = 1;
            if (InterlockedIncrement(&mv_hyd) == 1) {
                char moved[1120];
                snprintf(moved, sizeof moved, "%s.moved", path);
                if (!MoveFileExA(path, moved, MOVEFILE_REPLACE_EXISTING)) return EIO;
            }
        }
    }
    return 0;
}

void readable_test_counts(long *o, long *f, long *l);

static int counts_are(long o, long f, long l) {
    long a, b, c;
    readable_test_counts(&a, &b, &c);
    return a == o && b == f && c == l;
}

/* the gate opened: every blocked job finishes; their counters back to 0 within 5 s; closed again */
static void release(const char *what) {
    SetEvent(gate);
    int zero = 0;
    for (int i = 0; i < 500 && !(zero = counts_are(0, 0, 0)); i++) Sleep(10);
    long a, b, c;
    readable_test_counts(&a, &b, &c);
    CHECK(zero, "%s: released, every slot free again (%ld %ld %ld)", what, a, b, c);
    ResetEvent(gate);
}

typedef struct { read_state s; double ms; long jobs; char why[512]; } try_t;
static try_t try_read(const char *path, double secs) {
    try_t t;
    plat_finfo st;
    const char *folder;
    LONG before = InterlockedCompareExchange(&looks, 0, 0);
    uint64_t t0 = plat_now_mono_ns();
    t.s = readable(path, secs, &st, t.why, sizeof t.why, &folder);
    t.ms = (double)(plat_now_mono_ns() - t0) / 1e6;
    t.jobs = InterlockedCompareExchange(&looks, 0, 0) - before;
    return t;
}

static void file_at(char *out, size_t n, const char *dir, const char *name) {
    snprintf(out, n, "%s\\%s", dir, name);
    FILE *o = fopen(out, "wb");
    if (o) { fputs("RIFF", o); fclose(o); }
}

static void file_sized(char *out, size_t n, const char *dir, const char *name, size_t bytes) {   /* BYTES of zeros */
    snprintf(out, n, "%s\\%s", dir, name);
    FILE *o = fopen(out, "wb");
    if (!o) return;
    static char z[65536];
    for (size_t left = bytes; left;) {
        size_t k = left < sizeof z ? left : sizeof z;
        fwrite(z, 1, k, o);
        left -= k;
    }
    fclose(o);
}

static void t_gate(const char *dir) {
    char f[1100], p[200];
    gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!gate) { CHECK(0, "the gate's event"); return; }
    try_t t;
    /* give-up, then the late finish frees its slot */
    file_at(f, sizeof f, dir, "a-slow-1.wav");
    t = try_read(f, 0.3);
    CHECK(t.s == READ_ASKING && strstr(t.why, "didn't open it") && counts_are(0, 0, 1), "a system-drive read given up: asking (%s), one local slot",
          t.why);
    release("the late finish");
    /* a placeholder's download: the sentence, and it isn't remembered */
    file_at(f, sizeof f, dir, "b-ph-1.wav");
    t = try_read(f, 0.3);
    CHECK(t.s == READ_ASKING && strstr(t.why, "still downloading") && counts_are(0, 1, 0), "a download given up: %s", t.why);
    t = try_read(f, 0.3);
    CHECK(t.s == READ_ASKING && t.jobs == 1 && t.ms >= 250 && counts_are(0, 2, 0), "a placeholder isn't remembered: waited again (%.0f ms)", t.ms);
    release("the downloads");
    /* given up before its download (review HR1): it never starts one once released */
    file_at(f, sizeof f, dir, "e-slow-ph-1.wav");
    LONG before = InterlockedCompareExchange(&fetches, 0, 0);
    t = try_read(f, 0.3);
    CHECK(t.s == READ_ASKING && counts_are(0, 0, 1), "given up at its first step: one local slot (%s)", t.why);
    release("the read given up before its download");
    CHECK(InterlockedCompareExchange(&fetches, 0, 0) == before, "and it never reached its download (%ld download steps since)",
          (long)(InterlockedCompareExchange(&fetches, 0, 0) - before));
    /* the read-on loop (review HR7): still flagged, read to its end; blocked inside, given up; an
     * OFFLINE-only file keeps its one 64 KB read (HR4) */
    file_sized(f, sizeof f, dir, "f-big-1.wav", 3u << 20);
    t = try_read(f, 5);
    CHECK(t.s == READ_OK && counts_are(0, 0, 0), "a placeholder still flagged at each check: read to its end, ok (%s)", t.s == READ_OK ? "ok" : t.why);
    file_sized(f, sizeof f, dir, "g-bigslow-1.wav", 3u << 20);
    t = try_read(f, 0.3);
    CHECK(t.s == READ_ASKING && strstr(t.why, "still downloading") && counts_are(0, 1, 0), "a download held inside the read-on loop: %s", t.why);
    release("the held download");
    file_sized(f, sizeof f, dir, "h-off-1.wav", 3u << 20);
    LONG off0 = InterlockedCompareExchange(&off_hyd, 0, 0);
    t = try_read(f, 5);
    CHECK(t.s == READ_OK && InterlockedCompareExchange(&off_hyd, 0, 0) - off0 == 1, "an OFFLINE-only file: one check after its 64 KB read, ok (%ld)",
          (long)(InterlockedCompareExchange(&off_hyd, 0, 0) - off0));
    file_sized(f, sizeof f, dir, "i-offgone-1.wav", 3u << 20);
    t = try_read(f, 5);
    CHECK(t.s == READ_ABSENT && InterlockedCompareExchange(&gone_hyd, 0, 0) == 1, "an OFFLINE-only file gone after its first read: absent (%s)", t.why);
    file_sized(f, sizeof f, dir, "j-mv-1.wav", 3u << 20);
    t = try_read(f, 5);
    CHECK(t.s == READ_ABSENT && InterlockedCompareExchange(&mv_hyd, 0, 0) == 1, "a placeholder renamed with its handle open: absent (%s)", t.why);
    /* the caps by kind: 8 other drives and 8 downloads blocked, the system drive still read */
    for (int i = 1; i <= 8; i++) {
        snprintf(p, sizeof p, "\\\\blk%d\\m\\x.wav", i);
        try_read(p, 0.2);
    }
    CHECK(counts_are(8, 0, 0), "8 other drives' reads given up");
    t = try_read("\\\\blk9\\m\\x.wav", 0.2);
    CHECK(t.s == READ_ASKING && t.jobs == 0 && strstr(t.why, "other drives"), "a ninth other drive isn't started: %s", t.why);
    for (int i = 1; i <= 8; i++) {
        snprintf(p, sizeof p, "c-ph-%d.wav", i);
        file_at(f, sizeof f, dir, p);
        try_read(f, 0.2);
    }
    CHECK(counts_are(8, 8, 0), "8 downloads given up too");
    file_at(f, sizeof f, dir, "c-ph-9.wav");
    t = try_read(f, 2);
    CHECK(t.s == READ_ASKING && t.ms < 1500 && strstr(t.why, "downloading earlier tracks"), "a ninth download doesn't start: %s (%.0f ms)", t.why, t.ms);
    file_at(f, sizeof f, dir, "plain.wav");
    t = try_read(f, 2);
    CHECK(t.s == READ_OK, "and a system-drive track is still read: %s", t.s == READ_OK ? "ok" : t.why);
    release("the caps");
    /* the remembered keys: spellings, then the share widened at its second folder */
    t = try_read("\\\\BLKX\\Music\\a\\1.wav", 0.2);
    CHECK(t.s == READ_ASKING && strstr(t.why, "\\\\blkx\\music\\a didn't answer"), "a share's folder given up: %s", t.why);
    t = try_read("//blkx/music/A/2.wav", 0.2);
    CHECK(t.s == READ_ASKING && !t.jobs, "the same folder as //, another case: not read again (%s)", t.why);
    t = try_read("\\\\?\\UNC\\blkx\\MUSIC\\a\\3.wav", 0.2);
    CHECK(t.s == READ_ASKING && !t.jobs, "the same folder as \\\\?\\UNC\\: not read again");
    t = try_read("\\\\blkx\\music\\b\\1.wav", 0.2);
    CHECK(t.s == READ_ASKING && t.jobs == 1 && strstr(t.why, "\\\\blkx\\music didn't answer"), "a second folder given up: the share widened (%s)",
          t.why);
    t = try_read("\\\\blkx\\music\\c\\1.wav", 0.2);
    CHECK(t.s == READ_ASKING && !t.jobs, "a third folder: not read (%s)", t.why);
    t = try_read("\\\\blkx\\other\\a\\1.wav", 0.2);
    CHECK(t.s == READ_ASKING && t.jobs == 1, "another share of that server: read (it may answer)");
    t = try_read("\\\\blky\\m\\x.wav", 0.2);
    t = try_read("\\\\blky\\m\\d\\y.wav", 0.2);
    CHECK(t.s == READ_ASKING && !t.jobs, "a file directly under a share given up: the whole share (%s)", t.why);
    t = try_read("Q:\\Music\\1.wav", 0.2);
    CHECK(t.s == READ_ASKING && strstr(t.why, "q:\\ didn't answer"), "a drive letter given up: %s", t.why);
    t = try_read("q:/music/2.wav", 0.2);
    CHECK(t.s == READ_ASKING && !t.jobs, "the same drive, another case and '/': not read again");
    t = try_read("\\\\?\\Q:\\x.wav", 0.2);
    CHECK(t.s == READ_ASKING && !t.jobs, "the same drive as \\\\?\\Q:\\: not read again");
    t = try_read("\\\\?\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\a\\1.wav", 0.2);
    t = try_read("\\\\?\\volume{0A1B2C3D-0000-1111-2222-333344445555}\\b\\2.wav", 0.2);
    CHECK(t.s == READ_ASKING && !t.jobs, "a volume GUID path: its volume remembered, not a share's folder (%s)", t.why);
    release("the remembered keys");
    /* a network error remembers like a timeout (review B3) */
    t = try_read("\\\\neterr\\m\\a\\x.wav", 2);
    CHECK(t.s == READ_ASKING && t.ms < 1000 && strstr(t.why, "didn't answer"), "a network error: %s", t.why);
    t = try_read("\\\\neterr\\m\\a\\y.wav", 2);
    CHECK(t.s == READ_ASKING && !t.jobs, "its folder remembered: not read again");
}

static int one(const char *path, double secs) {
    int good = 1;
    for (int i = 0; i < 2; i++) {
        plat_finfo st = {0};
        char why[512];
        const char *folder;
        uint64_t t0 = plat_now_mono_ns();
        read_state s = readable(path, secs, &st, why, sizeof why, &folder);
        double ms = (double)(plat_now_mono_ns() - t0) / 1e6;
        printf("%s %s after %.0f ms%s%s (size %lld)\n", i ? "again:" : "first:", name(s), ms, why[0] ? ": " : "", why, (long long)st.size);
        good &= s == READ_OK || s == READ_ASKING;
    }
    printf("may wait (--quick doesn't look): %d; another drive's root: %zu bytes\n", readable_may_wait(path), paths_other_drive(path));
    return !good;
}

static int seq(int argc, char **argv) {
    double secs = atof(argv[2]);
    for (int i = 3; i < argc; i++) {
        plat_finfo st = {0};
        char why[512];
        const char *folder;
        uint64_t t0 = plat_now_mono_ns();
        read_state s = readable(argv[i], secs, &st, why, sizeof why, &folder);
        printf("%d: %s after %.0f ms%s%s\n", i - 2, name(s), (double)(plat_now_mono_ns() - t0) / 1e6, why[0] ? ": " : "", why);
    }
    return 0;
}

static HANDLE drip_w;
static DWORD WINAPI drip(void *arg) {   /* a byte every 300 ms, 8 times */
    (void)arg;
    for (int i = 0; i < 8; i++) {
        DWORD put;
        Sleep(300);
        WriteFile(drip_w, "x", 1, &put, NULL);
    }
    return 0;
}

static void t_watch(void) {
    HANDLE r, w;
    char b[16];
    DWORD got = 0;
    if (!CreatePipe(&r, &w, NULL, 0)) { CHECK(0, "a pipe"); return; }
    if (readable_watch_start("\\\\blkw\\music\\a.wav", 1)) {   /* else the read below would wait for good (review HR9) */
        CHECK(0, "the decode's watch starts");
        CloseHandle(r);
        CloseHandle(w);
        return;
    }
    CHECK(1, "the decode's watch starts");
    ULONGLONG t0 = GetTickCount64();
    BOOL ok = ReadFile(r, b, sizeof b, &got, NULL);
    DWORD le = GetLastError();
    ULONGLONG el = GetTickCount64() - t0;
    int c = readable_watch_stop(!ok);
    CHECK(!ok && le == ERROR_OPERATION_ABORTED && c && el >= 900 && el < 5000,
          "a read that moves nothing for 1 s is cancelled (%s, error %lu, after %llu ms, stop %d)", ok ? "read" : "failed", (unsigned long)le,
          (unsigned long long)el, c);
    try_t t = try_read("\\\\blkw\\music\\b.wav", 2);
    CHECK(t.s == READ_ASKING && !t.jobs, "its share is remembered for the run: not read again (%s)", t.why);
    /* a cancel whose call worked anyway (review B5): nothing remembered */
    if (readable_watch_start("\\\\blkv\\music\\a.wav", 1)) {
        CHECK(0, "the watch starts again");
        CloseHandle(r);
        CloseHandle(w);
        return;
    }
    CHECK(1, "the watch starts again");
    ok = ReadFile(r, b, sizeof b, &got, NULL);
    c = readable_watch_stop(0);
    CHECK(!ok && !c, "cancelled, but the call didn't fail: stop gives 0");
    t = try_read("\\\\blkv\\music\\b.wav", 0.2);
    CHECK(t.jobs == 1, "and its share isn't remembered: read again");
    release("the watch's shares");
    /* a pipe that keeps moving: never cancelled */
    drip_w = w;
    HANDLE th = CreateThread(NULL, 0, drip, NULL, 0, NULL);
    readable_watch_start("D:\\Music\\a.wav", 1);
    int reads = 0;
    for (int i = 0; i < 8; i++)
        if (ReadFile(r, b, 1, &got, NULL) && got == 1) reads++;
    c = readable_watch_stop(reads != 8);
    CHECK(reads == 8 && !c, "reads that keep moving aren't cancelled (%d of 8, stop %d)", reads, c);
    if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
    CloseHandle(r);
    CloseHandle(w);
}

int main(int argc, char **argv) {
    if (plat_exe_start()) { fprintf(stderr, "plat_exe_start failed\n"); return 1; }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--path")) return one(argv[2], argc == 4 ? atof(argv[3]) : 2);
    if (argc >= 4 && !strcmp(argv[1], "--seq")) return seq(argc, argv);
    if (argc != 2) { fprintf(stderr, "usage: %s DIR | --path P [SECS] | --seq SECS P...\n", argv[0]); return 2; }
    const char *dir = argv[1];
    char f[1100], why[512], sys[MAX_PATH + 1];
    plat_finfo st;
    const char *folder;
    snprintf(f, sizeof f, "%s\\a file.wav", dir);
    FILE *o = fopen(f, "wb");
    if (o) { fputs("RIFF", o); fclose(o); }
    read_state s = readable(f, 2, &st, why, sizeof why, &folder);
    CHECK(s == READ_OK && st.kind == PLAT_FILE && st.size == 4 && !folder[0], "a file: ok, its size (%s %s)", name(s), why);
    CHECK(!readable_may_wait(f), "a file on the system drive: --quick looks at it");
    snprintf(f, sizeof f, "%s/a file.wav", dir);
    s = readable(f, 2, &st, why, sizeof why, &folder);
    CHECK(s == READ_OK, "the same with '/' (%s %s)", name(s), why);
    snprintf(f, sizeof f, "%s\\not there.wav", dir);
    s = readable(f, 2, &st, why, sizeof why, &folder);
    CHECK(s == READ_ABSENT && strstr(why, "isn't there"), "a missing file: absent (%s: %s)", name(s), why);
    s = readable(dir, 2, &st, why, sizeof why, &folder);
    CHECK(s == READ_FAILED && !strcmp(why, "not a file"), "a folder: not a file (%s: %s)", name(s), why);
    /* a drive letter that isn't there (the first free one from Z: down) */
    DWORD drives = GetLogicalDrives();
    char letter = 0;
    for (char c = 'Z'; c >= 'D' && !letter; c--)
        if (!(drives & (1u << (c - 'A')))) letter = c;
    if (letter) {
        snprintf(f, sizeof f, "%c:\\Music\\x.mp3", letter);
        s = readable(f, 2, &st, why, sizeof why, &folder);
        CHECK(s == READ_ABSENT, "%c: (no such drive): absent (%s: %s)", letter, name(s), why);
        CHECK(paths_other_drive(f) == 3 && readable_may_wait(f), "%c: is another drive, root \"%c:\\\", --quick doesn't look", letter, letter);
    }
    UINT n = GetSystemWindowsDirectoryA(sys, sizeof sys);
    if (n >= 3) {
        char p[64];
        snprintf(p, sizeof p, "%c:/Users/x.mp3", sys[0]);
        CHECK(!paths_other_drive(p), "the system drive %c: isn't another drive", sys[0]);
        p[0] = (char)(sys[0] ^ 0x20);
        CHECK(!paths_other_drive(p), "nor in the other case (%c:)", p[0]);
    }
    CHECK(paths_other_drive("\\\\nas\\share\\m\\x.mp3") == strlen("\\\\nas\\share"), "UNC: another drive, \\\\server\\share its root");
    t_gate(dir);
    if (gate) t_watch();                        /* it relies on the gate for its made-up shares */
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
