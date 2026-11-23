/* The Windows location choice's volume probe, folder id and not-indexed attribute, and the
 * measure's rekordbox ops (nl_rekordbox_running, nl_process_start): shared by the DLL and
 * winc-location-wintest. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include "compat.h"
#include "native_choice_win.h"
#include "native_steps.h"   /* NS_PATH_MAX */
#include "volumes.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the folder chosen's volume: vol_facts_of (links followed; a network path, a mapped letter or a
 * share behind a link: the network's facts), the location verdict, then its GUID root and the folder's final
 * path as a volume GUID path (vol_guid_path: cache_dir's form, the contract with the bridge) */
int nc_volume(void *ctx, const char *folder, int *refusal, char *root, size_t rn, char *real, size_t n,
              void (*log)(void *, const char *)) {
    vol_facts *f = malloc(sizeof *f);
    if (!f) return ENOMEM;
    int e = vol_facts_of(folder, f);
    *refusal = e ? VOL_REFUSE_UNREADABLE : vol_location_refusal(f);
    if (!e && *refusal == VOL_OK) {
        if (snprintf(root, rn, "%s", f->guid) >= (int)rn || !vol_guid_root(root)) e = ENAMETOOLONG;
        else if ((e = vol_guid_path(folder, real, n)) == 0 && strncmp(real, root, VOL_GUID_LEN) && _strnicmp(real, root, VOL_GUID_LEN))
            e = EXDEV;   /* the final path is on another volume than the one asked about */
    }
    char line[VOL_PATH_MAX + 64];
    if (!e) {   /* the external rule's facts for the log (J28's real-hardware round reads them) */
        vol_facts_line(f, line, sizeof line);
        log(ctx, line);
    }
    free(f);
    return e;
}

/* a version 4 GUID (CoCreateGuid: the system's random source), 8-4-4-4-12 upper-case hex */
int nc_new_id(void *ctx, char out[40]) {
    (void)ctx;
    GUID g;
    if (CoCreateGuid(&g) != S_OK) return -1;
    snprintf(out, 40, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
             g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return 0;
}

/* ".noindex" on Windows: the search indexer skips our folder (app.md §5.8) */
void nc_not_indexed(void *ctx, const char *dir) {
    (void)ctx;
    wchar_t w[NS_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dir, -1, w, NS_PATH_MAX)) return;
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT))
        SetFileAttributesW(w, (a & ~(DWORD)FILE_ATTRIBUTE_DIRECTORY) | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
}

/* rekordboxOpen(): a rekordbox.exe in any session (compat_processes) */
int nl_rekordbox_running(void *ctx) {
    (void)ctx;
    compat_procs p;
    return compat_processes(compat_os_windows(), NULL, &p) ? -1 : p.running;
}
/* the running process's creation time in seconds since 1970, as bridge-state's start=;
 * -1 if the process is gone or its start time cannot be read */
int64_t nl_process_start(void *ctx, int32_t pid) {
    (void)ctx;
    if (pid <= 0) return -1;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return -1;
    FILETIME c, x, k, u;
    /* A handle can outlive the process, and STILL_ACTIVE is also a valid exit code. */
    BOOL ok = WaitForSingleObject(h, 0) == WAIT_TIMEOUT && GetProcessTimes(h, &c, &x, &k, &u);
    CloseHandle(h);
    if (!ok) return -1;
    ULARGE_INTEGER t = {.LowPart = c.dwLowDateTime, .HighPart = c.dwHighDateTime};
    return (int64_t)((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
