/* update_win.c's import gate (run.sh): update_win.c linked with the pure rules it calls, the
 * payload code they need and common/plat_win.c (its tree's deletion), as the core DLL links them,
 * so the import table must stay within the core DLL's allowlist (KERNEL32, ADVAPI32, USER32, ntdll,
 * SHELL32, ole32 and the UCRT; winhttp.dll is loaded at run time). Never run.
 *
 * Built again with UPDATE_WIN_TEST_HOOKS, it is winc-update-wintest.exe (run in the VM, review K2's
 * undefined4): update_win.c's NT path match on Windows itself. listed_image of this process is its
 * PROCESS_NAME_NATIVE image path; its own folder matches that image through form[0] (the folder as
 * spelled, its drive mapped) and form[1] (its final path by handle); a pid that isn't listed is 0;
 * the 8.3 spelling of its own exe's NT path matches its folder, and the 8.3 spelling of a watcher
 * in a long-named folder of this run's matches that folder only through the alias step (its final
 * path), not another folder; an 8.3 spelling that can't be opened can't be told (-1); a folder
 * that doesn't exist matches nothing (0). Its fixtures are this run's own (named by the process
 * id and the tick count) and it removes only what it made.
 *
 * With --get it is update_win.c's WinHTTP download instead (win_get, app.md 5.4; platform.suite's
 * winhttp steps): URL from byte FROM (Range: bytes=FROM- when FROM > 0) into the file OUT, written
 * from the byte the answer starts at (OUT is made when FROM is 0, else written over in place), at
 * most MAX bytes; its outcome on one line, "get result R status S err E at A total T bytes B"
 * (R an UPDATE_NET_*, A and T the sink's begin, -1 when it never began), exit 0 (2: bad arguments
 * or OUT can't be opened). The step judges the line and OUT.
 *
 *   winc-update-wintest.exe
 *   winc-update-wintest.exe --get URL FROM MAX OUT */
#include "../update.h"

#ifndef UPDATE_WIN_TEST_HOOKS

int main(void) {
    const update_net *n = update_net_windows();
    const update_os *o = update_os_windows();
    return n && o && n->get && o->spawn ? 0 : 1;
}

#else

#  include "check.h"

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

#  include <errno.h>
#  include <wchar.h>

int checks, failures;

/* --get's sink: OUT written from the answer's first byte on */
typedef struct {
    FILE *f;
    int64_t at, total, bytes;
} get_sink;
static int gs_begin(void *ctx, int64_t at, int64_t total) {
    get_sink *s = ctx;
    s->at = at;
    s->total = total;
    return _fseeki64(s->f, at, SEEK_SET) ? EIO : 0;
}
static int gs_write(void *ctx, const void *p, size_t n) {
    get_sink *s = ctx;
    if (fwrite(p, 1, n, s->f) != n) return EIO;
    s->bytes += (int64_t)n;
    return 0;
}
static int get_main(char **a) {
    char *e1, *e2;
    long long from = strtoll(a[3], &e1, 10), max = strtoll(a[4], &e2, 10);
    if (*e1 || *e2 || from < 0 || max < 0) {
        printf("usage: --get URL FROM MAX OUT (FROM and MAX bytes)\n");
        return 2;
    }
    FILE *f = fopen(a[5], from > 0 ? "r+b" : "wb");
    if (!f) {
        printf("can't open %s (errno %d)\n", a[5], errno);
        return 2;
    }
    get_sink s = {f, -1, -1, 0};
    update_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.begin = gs_begin;
    sink.write = gs_write;
    sink.ctx = &s;
    const update_net *net = update_net_windows();
    int status = 0, err = 0;
    int r = net->get(net->ctx, a[2], (int64_t)from, (int64_t)max, &sink, &status, &err);
    if (fclose(f)) printf("note: closing %s failed (errno %d)\n", a[5], errno);
    printf("get result %d status %d err %d at %lld total %lld bytes %lld\n", r, status, err, (long long)s.at, (long long)s.total,
           (long long)s.bytes);
    return 0;
}

enum { N = UPDATE_WIN_NT_MAX };

/* a Win32 path (C:\...) in its NT form, its drive mapped to its device: 0, or -1 */
static int nt_of(const wchar_t *win32, wchar_t *out) {
    if (!wcsncmp(win32, L"\\\\?\\", 4)) win32 += 4;
    if (!win32[0] || win32[1] != L':' || win32[2] != L'\\') return -1;
    wchar_t drive[3] = {win32[0], L':', 0}, dev[MAX_PATH];
    DWORD n = QueryDosDeviceW(drive, dev, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return -1;
    return _snwprintf(out, N, L"%ls%ls", dev, win32 + 2) < 0 ? -1 : 0;
}

/* path's folder, in place (its last separator cut) */
static void cut_name(wchar_t *path) {
    wchar_t *s = wcsrchr(path, L'\\');
    if (s) *s = 0;
}

int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "--get")) return get_main(argv);
    if (argc != 1) {
        printf("usage: winc-update-wintest.exe [--get URL FROM MAX OUT]\n");
        return 2;
    }
    static wchar_t native[N], listed[N], win32[N], dir[N], shortp[N], short_nt[N], tmp[N], longd[N], other[N], file[N], img[N];
    int by[2] = {0, 0};

    /* listed_image of this process: its PROCESS_NAME_NATIVE image path */
    DWORD len = N;
    BOOL qn = QueryFullProcessImageNameW(GetCurrentProcess(), PROCESS_NAME_NATIVE, native, &len);
    CHECK(qn, "QueryFullProcessImageNameW(PROCESS_NAME_NATIVE) of this process: error %lu", GetLastError());
    int li = update_win_test_listed_image(GetCurrentProcessId(), listed);
    CHECK(li == 1 && qn && CompareStringOrdinal(listed, -1, native, -1, TRUE) == CSTR_EQUAL,
          "listed_image of this process (%d): %ls, its native image %ls", li, li == 1 ? listed : L"-", native);

    /* its own folder: both forms match its image */
    len = N;
    BOOL qw = QueryFullProcessImageNameW(GetCurrentProcess(), 0, win32, &len);
    CHECK(qw, "QueryFullProcessImageNameW of this process: error %lu", GetLastError());
    wcscpy(dir, win32);
    cut_name(dir);
    int m = update_win_test_nt_matches(dir, native, 0, by);
    CHECK(m == 1 && by[0], "its own folder %ls matches its image %ls (%d), through form[0] (%d)", dir, native, m, by[0]);
    if (wcschr(native, L'~')) printf("note: this exe's own path is 8.3-spelled: form[1] not checked on it\n");
    else CHECK(by[1], "its own folder %ls matches its image through form[1] (the final path)", dir);

    /* a pid that isn't listed: 0 (gone) */
    DWORD gone = 0x0ffffffc;
    for (; gone > 0x0fff0000; gone -= 4) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, gone);
        if (!h && GetLastError() == ERROR_INVALID_PARAMETER) break;
        if (h) CloseHandle(h);
    }
    CHECK(update_win_test_listed_image(gone, listed) == 0, "a pid that isn't listed (%lu): 0", gone);

    /* its own exe's 8.3 spelling, in NT form: matches its folder */
    DWORD sn = GetShortPathNameW(win32, shortp, N);
    if (sn == 0 || sn >= N || nt_of(shortp, short_nt)) {
        CHECK(0, "the 8.3 spelling of %ls in NT form: error %lu", win32, GetLastError());
    } else if (!wcschr(short_nt, L'~')) {
        printf("note: %ls has no 8.3 component (8.3 names off on its volume?): its own exe's alias not checked\n", shortp);
    } else {
        m = update_win_test_nt_matches(dir, short_nt, 0, by);
        CHECK(m == 1, "its own exe's 8.3 spelling %ls matches its folder %ls: %d", short_nt, dir, m);
    }

    /* a watcher in a long-named folder of this run's, by its 8.3 spelling: the alias step alone
     * matches it (review K2's undefined2); against another folder it is elsewhere (GH1) */
    DWORD tn = GetTempPathW(N, tmp);
    CHECK(tn > 0 && tn < N, "GetTempPathW: error %lu", GetLastError());
    DWORD tick = GetTickCount();
    _snwprintf(longd, N, L"%lsrbsp update wintest long folder %lu-%lu", tmp, GetCurrentProcessId(), tick);
    _snwprintf(other, N, L"%lsrbsp update wintest other folder %lu-%lu", tmp, GetCurrentProcessId(), tick);
    _snwprintf(file, N, L"%ls\\rbsp-watch-x64.exe", longd);
    int made_d = CreateDirectoryW(longd, NULL) != 0, made_o = CreateDirectoryW(other, NULL) != 0, made_f = 0;
    CHECK(made_d && made_o, "this run's folders %ls and %ls: error %lu", longd, other, GetLastError());
    if (made_d) {
        HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        made_f = f != INVALID_HANDLE_VALUE;
        if (made_f) CloseHandle(f);
        CHECK(made_f, "this run's watcher file %ls: error %lu", file, GetLastError());
    }
    if (made_d && made_o && made_f) {
        sn = GetShortPathNameW(file, shortp, N);
        DWORD ln = GetLongPathNameW(longd, img, N);   /* the folder's long spelling, %TEMP% included */
        if (sn == 0 || sn >= N || ln == 0 || ln >= N || nt_of(shortp, short_nt)) {
            CHECK(0, "the 8.3 spelling of %ls in NT form: error %lu", file, GetLastError());
        } else if (!wcschr(short_nt, L'~')) {
            printf("note: %ls has no 8.3 component (8.3 names off on %%TEMP%%'s volume?): the alias step not checked\n", shortp);
        } else {
            m = update_win_test_nt_matches(img, short_nt, 1, by);
            CHECK(m == 1 && !by[0] && !by[1], "the watcher's 8.3 spelling %ls matches %ls through the alias only: %d (forms %d %d)", short_nt, img,
                  m, by[0], by[1]);
            m = update_win_test_nt_matches(img, short_nt, 0, by);
            CHECK(m == 1, "the same under the folder (running_under's match): %d", m);
            m = update_win_test_nt_matches(other, short_nt, 1, by);
            CHECK(m == 0, "the watcher's 8.3 spelling against another folder: elsewhere (0): %d", m);
            m = update_win_test_nt_matches(other, short_nt, 0, by);
            CHECK(m == 0, "the same under another folder: elsewhere (0): %d", m);
        }
        /* an 8.3 spelling that can't be opened: can't be told */
        if (!nt_of(longd, img)) {
            wcscat(img, L"\\NOSUCH~1\\RBSP-W~1.EXE");
            m = update_win_test_nt_matches(other, img, 1, by);
            CHECK(m == -1, "an 8.3 spelling that can't be opened (%ls): -1: %d", img, m);
        }
    }
    /* a folder that doesn't exist: nothing runs from it (0) */
    _snwprintf(img, N, L"%ls\\missing", other);
    m = update_win_test_nt_matches(img, native, 0, by);
    CHECK(m == 0, "a folder that doesn't exist (%ls): 0: %d", img, m);

    if (made_f) DeleteFileW(file);
    if (made_d) RemoveDirectoryW(longd);
    if (made_o) RemoveDirectoryW(other);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

#endif
