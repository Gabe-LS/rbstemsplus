/* The after-exit copy's folder check (afterexit_self_win.c's ax_win_dir_ok; reviews UN1, UQ5,
 * U51-U54) on Windows' own folders, in the Windows test exe (wintest.c, run in the VM, W13a): a
 * plain folder in %TEMP% passes, also by %TEMP%'s long and 8.3 forms and in another case; a folder in a
 * subfolder of %TEMP%, a junction, a file and odd paths don't. Its fixtures are this run's own
 * (named by the process id and the tick count, made with CREATE_NEW semantics and checked), and it
 * removes only what this run made. */
#include "check.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include "../afterexit.h"

#include <wchar.h>
#include <wctype.h>

/* the folder link (already made, empty) turned into a mount point (junction) to target (an
 * absolute "C:\..." path), as mklink /J makes it: 0, or -1 */
static int make_junction(const wchar_t *link, const wchar_t *target) {
    HANDLE h = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    static wchar_t sub[AX_PATH_MAX];
    _snwprintf(sub, AX_PATH_MAX, L"\\??\\%ls", target);
    sub[AX_PATH_MAX - 1] = 0;
    USHORT sn = (USHORT)(wcslen(sub) * sizeof(wchar_t)), pn = (USHORT)(wcslen(target) * sizeof(wchar_t));
    /* REPARSE_DATA_BUFFER's MountPointReparseBuffer, laid out by hand (it lives in the DDK's ntifs.h) */
    static BYTE buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    ZeroMemory(buf, sizeof buf);
    DWORD *tag = (DWORD *)buf;
    USHORT *f = (USHORT *)(buf + 4);   /* ReparseDataLength, Reserved, then the four offsets and lengths */
    *tag = IO_REPARSE_TAG_MOUNT_POINT;
    f[2] = 0;                          /* SubstituteNameOffset */
    f[3] = sn;                         /* SubstituteNameLength */
    f[4] = (USHORT)(sn + 2);           /* PrintNameOffset */
    f[5] = pn;                         /* PrintNameLength */
    BYTE *path = buf + 16;
    memcpy(path, sub, sn);
    memcpy(path + sn + 2, target, pn);
    f[0] = (USHORT)(8 + sn + 2 + pn + 2);   /* ReparseDataLength: the four fields and the two names */
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf, (DWORD)(8 + f[0]), NULL, 0, &got, NULL);
    CloseHandle(h);
    return ok ? 0 : -1;
}

void test_afterexit_win(void) {
    static wchar_t tmp[AX_PATH_MAX], dir[AX_PATH_MAX], sub[AX_PATH_MAX], deep[AX_PATH_MAX], target[AX_PATH_MAX], jun[AX_PATH_MAX],
        file[AX_PATH_MAX], longp[AX_PATH_MAX], shortp[AX_PATH_MAX], dlong[AX_PATH_MAX], dshort[AX_PATH_MAX], tcase[AX_PATH_MAX], dcase[AX_PATH_MAX];
    DWORD tn = GetTempPathW(AX_PATH_MAX, tmp);   /* ends with '\' */
    CHECK(tn > 0 && tn < AX_PATH_MAX, "GetTempPathW");
    if (tn == 0 || tn >= AX_PATH_MAX) return;
    /* this run's own names (review U51): the process id and the tick count in the 16 hex digits */
    unsigned long id = GetCurrentProcessId(), t = GetTickCount() & 0xfffffffu;
    _snwprintf(dir, AX_PATH_MAX, L"%lsrbsp-uninstall-%08lx%07lx1", tmp, id, t);
    _snwprintf(sub, AX_PATH_MAX, L"%lsrbsp-ax-wintest-%08lx", tmp, id);
    _snwprintf(deep, AX_PATH_MAX, L"%ls\\rbsp-uninstall-%08lx%07lx2", sub, id, t);
    _snwprintf(target, AX_PATH_MAX, L"%lsrbsp-ax-wintest-target-%08lx", tmp, id);
    _snwprintf(jun, AX_PATH_MAX, L"%lsrbsp-uninstall-%08lx%07lx3", tmp, id, t);
    _snwprintf(file, AX_PATH_MAX, L"%lsrbsp-uninstall-%08lx%07lx4", tmp, id, t);
    int mk_dir = CreateDirectoryW(dir, NULL) != 0, mk_sub = CreateDirectoryW(sub, NULL) != 0;
    int mk_deep = mk_sub && CreateDirectoryW(deep, NULL) != 0, mk_target = CreateDirectoryW(target, NULL) != 0;
    int mk_jun = CreateDirectoryW(jun, NULL) != 0;
    int junction = mk_jun && mk_target && make_junction(jun, target) == 0;
    HANDLE fh = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    int mk_file = fh != INVALID_HANDLE_VALUE;
    if (mk_file) CloseHandle(fh);
    CHECK(mk_dir && mk_sub && mk_deep && mk_target && mk_jun && mk_file, "the fixtures made, each new");

    CHECK(ax_win_dir_ok(dir), "a plain folder directly in %%TEMP%%: the copy's own");
    /* %TEMP% by its long and its 8.3 form (review U54): %TEMP% may already be the 8.3 form (the
     * VM's is C:\Users\GABRIE~1\...), so the long form is GetLongPathNameW's and the short one
     * GetShortPathNameW's of that; the folder passes by both. The VM's account has a long name,
     * so the two differ; only when they don't (no 8.3 form at all) is it noted, and W13a counts
     * that note as a failure */
    DWORD ll = GetLongPathNameW(tmp, longp, AX_PATH_MAX);
    CHECK(ll > 0 && ll < AX_PATH_MAX, "GetLongPathNameW of %%TEMP%%");
    DWORD sl = ll > 0 && ll < AX_PATH_MAX ? GetShortPathNameW(longp, shortp, AX_PATH_MAX) : 0;
    CHECK(sl > 0 && sl < AX_PATH_MAX, "GetShortPathNameW of %%TEMP%%'s long form");
    if (sl > 0 && sl < AX_PATH_MAX) {
        if (wcscmp(shortp, longp) == 0) {
            printf("note: %%TEMP%% has no distinct 8.3 form; short-name case not exercised\n");
        } else {
            _snwprintf(dlong, AX_PATH_MAX, L"%lsrbsp-uninstall-%08lx%07lx1", longp, id, t);
            _snwprintf(dshort, AX_PATH_MAX, L"%lsrbsp-uninstall-%08lx%07lx1", shortp, id, t);
            CHECK(ax_win_dir_ok(dlong), "the folder by %%TEMP%%'s long form: the same parent");
            CHECK(ax_win_dir_ok(dshort), "the folder by %%TEMP%%'s 8.3 form: the same parent");
        }
    }
    /* %TEMP% in another case (review U54): every letter swapped (the drive letter is one, so the
     * string always changes), with the CRT's towupper/towlower, not USER32's. If GetLongPathNameW
     * gives the on-disk case back, this pins the accepting result, not CompareStringOrdinal's
     * ignore-case flag */
    wcscpy(tcase, tmp);
    for (wchar_t *c = tcase; *c; c++) *c = iswlower(*c) ? towupper(*c) : towlower(*c);
    _snwprintf(dcase, AX_PATH_MAX, L"%lsrbsp-uninstall-%08lx%07lx1", tcase, id, t);
    CHECK(wcscmp(tcase, tmp) && ax_win_dir_ok(dcase), "%%TEMP%% in another case: the same parent");
    CHECK(!ax_win_dir_ok(deep), "a folder in a subfolder of %%TEMP%%: refused");
    CHECK(junction, "the junction made (FSCTL_SET_REPARSE_POINT)");
    CHECK(!ax_win_dir_ok(jun), "a junction directly in %%TEMP%%: refused (a reparse point)");
    CHECK(!ax_win_dir_ok(file), "a file of that name: refused");
    CHECK(!ax_win_dir_ok(L"C:\\x") && !ax_win_dir_ok(NULL) && !ax_win_dir_ok(L"") && !ax_win_dir_ok(L"x"), "another parent, none: refused");

    /* only what this run made: the junction itself first (never its target) */
    if (mk_file) DeleteFileW(file);
    if (mk_jun) RemoveDirectoryW(jun);
    if (mk_target) RemoveDirectoryW(target);
    if (mk_deep) RemoveDirectoryW(deep);
    if (mk_sub) RemoveDirectoryW(sub);
    if (mk_dir) RemoveDirectoryW(dir);
}
