/* life_os_version (lifecycle.h) on its own: RtlGetVersion from ntdll (a static import, on the core
 * DLL's allowlist, app.md §7) and nothing else, for the app and --swap. The start stub (stub.c) and
 * the watcher (watch_win.c) don't link it: each reads RtlGetVersion itself within its own import
 * allowlist; app_test.c checks at compile time that the three minimums agree. RtlGetVersion
 * reports the real build whatever the exe's manifest says (I73); GetVersionEx and .NET's
 * Environment.OSVersion don't. Unverified until the VM run. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "lifecycle.h"

#include <string.h>

/* ntdll's export, as wdm.h declares it (mingw-w64's user-mode headers don't) */
NTSYSAPI LONG NTAPI RtlGetVersion(PRTL_OSVERSIONINFOW info);

int life_os_version(uint32_t *major, uint32_t *minor, uint32_t *build) {
    RTL_OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    *major = *minor = *build = 0;
    if (RtlGetVersion(&v) != 0) return -1;                 /* STATUS_SUCCESS is 0 */
    *major = v.dwMajorVersion;
    *minor = v.dwMinorVersion;
    *build = v.dwBuildNumber;
    return 0;
}
