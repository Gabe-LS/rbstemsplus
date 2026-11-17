/* plat_helper.h on Windows: the prepare helper's calls beyond plat.h (helper.md §1, T4). Paths are
 * UTF-8, converted strictly (win_rules.h), and handed to Win32 as "\\?\" paths, so long paths need
 * no LongPathsEnabled. */
#include "plat_helper.h"
#include "win_rules.h"

#include <shlobj.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

/* FOLDERID_LocalAppData {F1B32785-6FBA-4FCF-9D55-7B8E7F157091} and FOLDERID_RoamingAppData
 * {3EB685DB-65F9-4CF6-A03A-E3EF65729F3D} (knownfolders.h) */
static const GUID ph_local = {0xf1b32785, 0x6fba, 0x4fcf, {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}};
static const GUID ph_roaming = {0x3eb685db, 0x65f9, 0x4cf6, {0xa0, 0x3a, 0xe3, 0xef, 0x65, 0x72, 0x9f, 0x3d}};

/* the "\\?\X:\..." wide form of an absolute UTF-8 path (wr_nt_path's, with the Win32 prefix);
 * malloc'd, NULL when it isn't a path wr_nt_path takes */
static wchar_t *ph_wide(const char *path) {
    wchar_t *w = malloc(sizeof(wchar_t) * (WR_NT_MAX + 1));
    if (!w) return NULL;
    long n = wr_nt_path(path, (uint16_t *)w, WR_NT_MAX + 1);
    if (n < 4) { free(w); return NULL; }
    w[1] = L'\\';                                           /* "\??\" -> "\\?\" */
    return w;
}

COMMON_API int plat_known_folder(int which, char *out, size_t n) {
    PWSTR p = NULL;
    int r = -1;
    if (n) out[0] = 0;
    const GUID *id = which == PLAT_FOLDER_ROAMING_APPDATA ? &ph_roaming : which == PLAT_FOLDER_LOCAL_APPDATA ? &ph_local : NULL;
    if (id && SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, NULL, &p) == S_OK && p) {
        const char *rest;
        if (wr_utf16_to_8((const uint16_t *)p, wcslen(p), out, n) >= 0 && wr_path_kind(out, &rest) == 1) r = 0;
    }
    if (p) CoTaskMemFree(p);
    if (r && n) out[0] = 0;
    return r;
}

COMMON_API int plat_temp_dir(char *out, size_t n) {
    char b[1024];
    if (plat_known_folder(PLAT_FOLDER_LOCAL_APPDATA, b, sizeof b)) return -1;
    if ((size_t)snprintf(out, n, "%s\\Temp", b) >= n) { if (n) out[0] = 0; return -1; }
    return 0;
}

COMMON_API int plat_temp_file(plat_file *out) {
    static volatile LONG seq;
    char dir[1024], p[1100];
    out->h = NULL;
    if (plat_temp_dir(dir, sizeof dir)) return ENOENT;
    plat_mkdirs(dir);                                       /* normally there already */
    for (int tries = 0; tries < 64; tries++) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        snprintf(p, sizeof p, "%s\\rbsp-pcm-%lu-%ld-%llx.tmp", dir, (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&seq),
                 (unsigned long long)t.QuadPart);
        wchar_t *w = ph_wide(p);
        if (!w) return EINVAL;
        /* shared with no one; gone when the last handle closes, also if the process dies; never
         * inherited (no SECURITY_ATTRIBUTES) */
        HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        DWORD le = GetLastError();
        free(w);
        if (h != INVALID_HANDLE_VALUE) {
            out->h = h;
            return 0;
        }
        if (le != ERROR_FILE_EXISTS && le != ERROR_ALREADY_EXISTS) return wr_errno_win32((uint32_t)le, WR_OP_OPEN);
    }
    return EEXIST;
}

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#ifndef FILE_ATTRIBUTE_PINNED
#define FILE_ATTRIBUTE_PINNED 0x00080000
#endif
#ifndef FILE_ATTRIBUTE_UNPINNED
#define FILE_ATTRIBUTE_UNPINNED 0x00100000
#endif

COMMON_API int plat_path_where(const char *path, plat_where *w) {
    memset(w, 0, sizeof *w);
    wchar_t *p = ph_wide(path);
    if (!p) return EINVAL;
    /* attributes only: no handle to the data, so no placeholder is hydrated */
    WIN32_FILE_ATTRIBUTE_DATA a;
    int e = 0;
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &a)) {
        DWORD x = a.dwFileAttributes;
        w->exists = 1;
        w->attributes = x;
        w->dir = (x & FILE_ATTRIBUTE_DIRECTORY) != 0;
        w->placeholder = (x & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0;
        w->offline = (x & FILE_ATTRIBUTE_OFFLINE) != 0;
        w->pinned = (x & FILE_ATTRIBUTE_PINNED) != 0;
        w->unpinned = (x & FILE_ATTRIBUTE_UNPINNED) != 0;
    } else {
        e = wr_errno_win32((uint32_t)GetLastError(), WR_OP_OPEN);
    }
    /* the drive, also for a name that isn't there (the drive may be what is missing) */
    wchar_t root[MAX_PATH + 1];
    if (GetVolumePathNameW(p, root, MAX_PATH + 1)) {
        /* "\\?\X:\" as "X:\", the form GetDriveTypeW documents; a volume GUID root as it is */
        const wchar_t *r = !wcsncmp(root, L"\\\\?\\", 4) && root[4] && root[5] == L':' ? root + 4 : root;
        UINT t = GetDriveTypeW(r);
        w->remote = t == DRIVE_REMOTE;
        w->removable = t == DRIVE_REMOVABLE || t == DRIVE_CDROM;
    }
    free(p);
    return e;
}

COMMON_API int plat_stdio_sync(FILE *s) {
    if (fflush(s)) return errno ? errno : EIO;
    int fd = _fileno(s);
    HANDLE h = fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return EBADF;
    return FlushFileBuffers(h) ? 0 : wr_errno_win32((uint32_t)GetLastError(), WR_OP_OPEN);   /* as plat_fsync */
}

COMMON_API int plat_finfo_path(const char *path, plat_finfo *fi) {
    wchar_t *p = ph_wide(path);
    if (!p) return EINVAL;
    WIN32_FILE_ATTRIBUTE_DATA a;                            /* attributes only: nothing hydrated */
    BOOL ok = GetFileAttributesExW(p, GetFileExInfoStandard, &a);
    DWORD le = GetLastError();
    free(p);
    if (!ok) return wr_errno_win32((uint32_t)le, WR_OP_OPEN);
    fi->kind = a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? PLAT_DIR : a.dwFileAttributes & FILE_ATTRIBUTE_DEVICE ? PLAT_OTHER : PLAT_FILE;
    fi->size = (int64_t)((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow);
    /* 100 ns since 1601 to ns since 1970, as manifest.c records the Mac's st_mtimespec */
    uint64_t u = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    if (u > (uint64_t)INT64_MAX) return EOVERFLOW;
    int64_t t = (int64_t)u - 116444736000000000LL;
    if (t > INT64_MAX / 100 || t < INT64_MIN / 100) return EOVERFLOW;
    fi->mtime_ns = t * 100;
    return 0;
}

COMMON_API int plat_exe_start(void) {
    /* System32 only, for every later load without a full path (and for the full-path loads'
     * dependencies: plat_lib_open adds the DLL's own folder) */
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) return -1;
    /* the UCRT opens the standard streams in text mode: \n would become \r\n */
    FILE *std[3] = {stdin, stdout, stderr};
    for (int i = 0; i < 3; i++) {
        int fd = _fileno(std[i]);
        if (fd < 0) continue;                               /* not connected (no console, no pipe) */
        if (_setmode(fd, _O_BINARY) == -1) return -1;
    }
    return 0;
}
