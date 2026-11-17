/* The Windows helper's platform calls (common/plat_helper.h, T4) and big_plat.c (T7), on a scratch
 * folder. The same source runs on the Mac (helper/tests/win-plat.sh, on plat_mac.c: the parts that
 * are one code on both) and in the Windows VM (built by helper/build-win.sh as
 * rbsp-win-plat-test.exe, with the Windows-only checks):
 *
 *   win-plat-test SCRATCH [DLL_DIR]   SCRATCH: an empty folder (in the VM: on NTFS, e.g.
 *                                     C:\rbtest\scratch); DLL_DIR (Windows): the folder holding
 *                                     rbsp-planted.dll, which plat_exe_start must keep out of
 *                                     every search; exits 1 when a check fails */
#include "plat_helper.h"
#include "big.h"
#include "../../common/tests/check.h"

#include <stdint.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <winioctl.h>
#define SEP "\\"
#else
#include <unistd.h>
#define SEP "/"
#endif

static char root[1024];

static void path(char *out, size_t n, const char *name) { snprintf(out, n, "%s" SEP "%s", root, name); }

static void t_strlcat(void) {
    char b[8] = "ab";
    CHECK(plat_strlcat(b, "cd", sizeof b) == 4 && !strcmp(b, "abcd"), "append: %s", b);
    CHECK(plat_strlcat(b, "efghij", sizeof b) == 10 && !strcmp(b, "abcdefg"), "truncated: %s", b);
    char u[4] = {'x', 'y', 'z', 'w'};                      /* not terminated within n */
    CHECK(plat_strlcat(u, "a", sizeof u) == 5 && u[3] == 'w', "unterminated dst left alone");
    char e[1] = "";
    CHECK(plat_strlcat(e, "abc", sizeof e) == 3 && e[0] == 0, "n = 1");
}

static void t_getline(void) {
    char p[1100];
    path(p, sizeof p, "lines.txt");
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL, "make %s", p);
    if (!f) return;
    fputs("one\ntwo\r\n\n", f);
    for (int i = 0; i < 1000; i++) fputc('x', f);            /* longer than the first buffer, no '\n' */
    fclose(f);
    f = fopen(p, "rb");
    char *line = NULL;
    size_t cap = 0;
    long r = plat_getline(&line, &cap, f);
    CHECK(r == 4 && !strcmp(line, "one\n"), "line 1: %ld", r);
    r = plat_getline(&line, &cap, f);
    CHECK(r == 5 && !strcmp(line, "two\r\n"), "line 2 keeps its CR: %ld", r);
    r = plat_getline(&line, &cap, f);
    CHECK(r == 1 && !strcmp(line, "\n"), "an empty line: %ld", r);
    r = plat_getline(&line, &cap, f);
    CHECK(r == 1000 && strlen(line) == 1000 && cap > 1000, "the last line, grown: %ld", r);
    CHECK(plat_getline(&line, &cap, f) == -1, "then the end");
    fclose(f);
    free(line);
}

typedef struct { int files; int64_t bytes, alloc; int sub, deep; } walked;
static int walk_fn(void *ctx, const char *rel, const plat_stat *st) {
    walked *w = ctx;
    w->files++;
    w->bytes += st->size;
    w->alloc += st->alloc;
    if (!strcmp(rel, "a" SEP "b" SEP "two.bin")) w->sub = 1;
    if (strstr(rel, "linked")) w->deep = 1;                 /* never reached through a link */
    return 0;
}
static int stop_fn(void *ctx, const char *rel, const plat_stat *st) {
    (void)rel, (void)st;
    return ++*(int *)ctx >= 2;
}

static void make(const char *rel, size_t n) {
    char p[1100];
    path(p, sizeof p, rel);
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL, "make %s", p);
    if (!f) return;
    for (size_t i = 0; i < n; i++) fputc((int)(i & 0xFF), f);
    fclose(f);
}

#if defined(_WIN32)
/* a junction (a mount-point reparse point) at link, to the folder target: FSCTL_SET_REPARSE_POINT
 * with REPARSE_DATA_BUFFER's mount-point form (ntifs.h), written out here; 0 or -1 */
static int junction(const char *link, const char *target) {
    wchar_t wl[1100], wt[1100];
    if (!MultiByteToWideChar(CP_UTF8, 0, link, -1, wl, 1100) || !MultiByteToWideChar(CP_UTF8, 0, target, -1, wt + 4, 1090)) return -1;
    memcpy(wt, L"\\??\\", 4 * sizeof(wchar_t));
    if (!CreateDirectoryW(wl, NULL)) return -1;
    HANDLE h = CreateFileW(wl, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size_t sub = wcslen(wt) * sizeof(wchar_t), print = wcslen(wt + 4) * sizeof(wchar_t);
    struct {
        DWORD tag;
        WORD len, reserved, sub_off, sub_len, print_off, print_len;
        WCHAR path[2200];
    } r;
    memset(&r, 0, sizeof r);
    r.tag = IO_REPARSE_TAG_MOUNT_POINT;
    r.sub_off = 0;
    r.sub_len = (WORD)sub;
    r.print_off = (WORD)(sub + sizeof(wchar_t));
    r.print_len = (WORD)print;
    memcpy(r.path, wt, sub);
    memcpy((char *)r.path + sub + sizeof(wchar_t), wt + 4, print);
    r.len = (WORD)(4 * sizeof(WORD) + sub + print + 2 * sizeof(wchar_t));
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &r, (DWORD)(8 + r.len), NULL, 0, &got, NULL);
    CloseHandle(h);
    return ok ? 0 : -1;
}
#endif

static void t_walk(void) {
    char top[1100], p[1100];
    path(top, sizeof top, "tree");
    path(p, sizeof p, "tree" SEP "a" SEP "b");
    plat_mkdirs(p);
    path(p, sizeof p, "tree" SEP "empty");
    plat_mkdirs(p);
    make("tree" SEP "one.bin", 1000);
    make("tree" SEP "a" SEP "b" SEP "two.bin", 5000);
    make("tree" SEP "a" SEP "three.bin", 0);
    char l[1100];
    path(l, sizeof l, "tree" SEP "a" SEP "linked");
#if !defined(_WIN32)
    CHECK(symlink(root, l) == 0, "a link to the scratch folder");   /* would loop if followed */
#else
    CHECK(junction(l, root) == 0, "a junction to the scratch folder");   /* no privilege needed */
#endif
    walked w = {0, 0, 0, 0, 0};
    CHECK(plat_walk(top, walk_fn, &w) == 0, "walked whole");
    CHECK(w.files == 3 && w.bytes == 6000 && w.sub && !w.deep, "%d files, %lld bytes, sub %d, link followed %d", w.files,
          (long long)w.bytes, w.sub, w.deep);
    CHECK(w.alloc >= 6000, "allocated %lld", (long long)w.alloc);
    int n = 0;
    CHECK(plat_walk(top, stop_fn, &n) == -1 && n == 2, "fn's nonzero stops it: %d", n);
    path(p, sizeof p, "nowhere");
    CHECK(plat_walk(p, walk_fn, &w) == -1, "a missing folder");
}

static void t_finfo(void) {
    char p[1100];
    plat_finfo fi;
    make("finfo.bin", 1234);
    path(p, sizeof p, "finfo.bin");
    int64_t now = plat_now_wall_ns();
    CHECK(plat_finfo_path(p, &fi) == 0 && fi.kind == PLAT_FILE && fi.size == 1234, "a file: %d, %lld", fi.kind, (long long)fi.size);
    CHECK(fi.mtime_ns > now - 600000000000LL && fi.mtime_ns < now + 600000000000LL, "its time in ns since 1970: %lld, now %lld",
          (long long)fi.mtime_ns, (long long)now);
    CHECK(plat_finfo_path(root, &fi) == 0 && fi.kind == PLAT_DIR, "a folder");
    path(p, sizeof p, "nowhere.bin");
    CHECK(plat_finfo_path(p, &fi) == ENOENT, "missing: ENOENT");
}

static void t_stdio_sync(void) {
    plat_dir d;
    plat_file f;
    CHECK(plat_dir_open(root, &d) == 0, "the scratch folder");
    CHECK(plat_file_create_at(d, "sync.txt", 0, &f) == 0, "create");
    FILE *s = plat_file_stdio(f, "w");
    CHECK(s != NULL, "stdio");
    if (s) {
        CHECK(fprintf(s, "rbstemsplus-kept 1\n") > 0, "written");
        CHECK(plat_stdio_sync(s) == 0, "synced through the stream");
        CHECK(fclose(s) == 0, "closed");
    }
    plat_stat st;
    CHECK(plat_stat_at(d, "sync.txt", &st) == 0 && st.size == 19, "its bytes there, no CR: %lld", (long long)st.size);
    plat_dir_close(d);
}

static void t_temp(void) {
    char d[1024];
    CHECK(plat_temp_dir(d, sizeof d) == 0 && d[0], "the temporary folder: %s", d);
    plat_file f;
    CHECK(plat_temp_file(&f) == 0 && plat_file_ok(f), "a temporary file");
    if (!plat_file_ok(f)) return;
    void *m = NULL;
    CHECK(plat_map_rw(f, 1 << 20, &m) == 0 && m, "mapped");
    if (m) {
        memset(m, 0x5a, 1 << 20);
        plat_unmap(m, 1 << 20);
    }
    unsigned char b[16];
    CHECK(plat_pread(f, b, sizeof b, 4096, &(size_t){0}) == 0 && b[0] == 0x5a && b[15] == 0x5a, "written through the map");
    plat_file_close(f);
}

/* big_plat.c: a mapped buffer from 32 MB, its realloc keeping the bytes, calloc's zeros */
static void t_big(void) {
    size_t n = (size_t)40 << 20;
    unsigned char *p = big_alloc(n);
    CHECK(p != NULL, "40 MB");
    if (!p) return;
    for (size_t i = 0; i < n; i += 4096) p[i] = (unsigned char)(i >> 12);
    p[n - 1] = 0xA5;
    unsigned char *q = big_realloc(p, 2 * n);
    CHECK(q != NULL, "grown to 80 MB");
    if (q) {
        int same = q[n - 1] == 0xA5;
        for (size_t i = 0; i < n; i += 4096) same &= q[i] == (unsigned char)(i >> 12);
        CHECK(same, "the bytes kept");
        q[2 * n - 1] = 1;
        p = q;
    }
    big_free(p);
    uint32_t *z = big_calloc((size_t)10 << 20, 4);
    CHECK(z != NULL, "calloc 40 MB");
    if (z) {
        int zero = 1;
        for (size_t i = 0; i < ((size_t)10 << 20); i += 1021) zero &= z[i] == 0;
        CHECK(zero, "zeros");
        big_free(z);
    }
    unsigned char *s = big_alloc(1000);                     /* malloc's, then grown into a mapping */
    CHECK(s != NULL, "small");
    if (s) {
        memset(s, 7, 1000);
        unsigned char *t = big_realloc(s, n);
        CHECK(t && t[0] == 7 && t[999] == 7, "small to mapped, the bytes kept");
        big_free(t ? t : s);
    }
    void *many[70];
    int got = 0;
    for (int i = 0; i < 70; i++) if ((many[i] = big_alloc((size_t)32 << 20))) got++;   /* past the table: malloc */
    CHECK(got == 70, "70 buffers of 32 MB: %d", got);
    for (int i = 0; i < 70; i++) big_free(many[i]);
}

#if defined(_WIN32)
static void t_windows(const char *dll_dir) {
    char l[1024], r[1024];
    CHECK(plat_known_folder(PLAT_FOLDER_LOCAL_APPDATA, l, sizeof l) == 0, "LocalAppData: %s", l);
    CHECK(plat_known_folder(PLAT_FOLDER_ROAMING_APPDATA, r, sizeof r) == 0, "RoamingAppData: %s", r);
    CHECK(strcmp(l, r) != 0, "two folders");
    CHECK(plat_known_folder(99, r, sizeof r) == -1 && !r[0], "an unknown one");
    /* the temporary folder ignores the environment */
    char before[1024], after[1024];
    plat_temp_dir(before, sizeof before);
    _putenv("TMP=C:\\nowhere");
    _putenv("TEMP=C:\\nowhere");
    plat_temp_dir(after, sizeof after);
    CHECK(!strcmp(before, after) && !strstr(after, "nowhere"), "TMP/TEMP not read: %s", after);
    plat_where w;
    char p[1100];
    path(p, sizeof p, "lines.txt");
    CHECK(plat_path_where(p, &w) == 0 && w.exists && !w.dir && !w.placeholder && !w.offline && !w.remote, "a local file: %#x",
          (unsigned)w.attributes);
    CHECK(plat_path_where(root, &w) == 0 && w.dir, "a folder");
    path(p, sizeof p, "missing.bin");
    CHECK(plat_path_where(p, &w) == ENOENT && !w.exists, "missing");
    CHECK(plat_path_where("relative\\x", &w) == EINVAL, "relative refused");
    /* plat_exe_start ran first (main): binary streams, and no DLL from the exe's or the current folder */
    CHECK(_setmode(_fileno(stdout), _O_BINARY) == _O_BINARY, "stdout already binary");
    if (dll_dir) {
        SetCurrentDirectoryA(dll_dir);
        HMODULE m = LoadLibraryW(L"rbsp-planted.dll");
        CHECK(m == NULL, "a DLL in the current folder isn't found by name");
        if (m) FreeLibrary(m);
    }
}
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
    if (plat_exe_start()) { fprintf(stderr, "plat_exe_start failed\n"); return 1; }
#endif
    if (argc < 2) { fprintf(stderr, "usage: %s SCRATCH [DLL_DIR]\n", argv[0]); return 2; }
    snprintf(root, sizeof root, "%s", argv[1]);
    t_strlcat();
    t_getline();
    t_walk();
    t_finfo();
    t_stdio_sync();
    t_temp();
    t_big();
#if defined(_WIN32)
    t_windows(argc > 2 ? argv[2] : NULL);
#endif
    printf("win-plat: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
