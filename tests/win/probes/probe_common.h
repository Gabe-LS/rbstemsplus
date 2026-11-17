/* Shared by the Phase 0 VM probes (tests/win/probes): an allocation-free logger, the loader-lock
 * check, the calling module and the loaded-module list. x64 only (rekordbox is x64). Probe code,
 * never shipped. */
#ifndef PROBE_COMMON_H
#define PROBE_COMMON_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <intrin.h>

static SRWLOCK plog_lock = SRWLOCK_INIT;
static const wchar_t *plog_path; /* set by the probe before the first line */

/* One line per call: open, append, close. CreateFileW/WriteFile only, no CRT allocation, so it
 * is safe while this thread holds the loader lock (no DLL is loaded by it). */
static inline void plog(const char *fmt, ...) {
    char line[1536];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int k = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, GetCurrentThreadId());
    va_list a;
    va_start(a, fmt);
    k += vsnprintf(line + k, sizeof line - k, fmt, a);
    va_end(a);
    if (k > (int)sizeof line - 3) k = (int)sizeof line - 3;
    line[k++] = '\r';
    line[k++] = '\n';
    AcquireSRWLockExclusive(&plog_lock);
    HANDLE h = CreateFileW(plog_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, line, (DWORD)k, &w, NULL);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&plog_lock);
}

/* Does this thread own the loader lock? PEB (gs:[0x60]) -> LoaderLock (PEB+0x110, an
 * RTL_CRITICAL_SECTION*) -> OwningThread (the owner's thread id). Offsets from the public x64
 * PEB layout. Validated in each run: DllMain (always under the lock) must read 1. */
static inline int owns_loader_lock(void) {
    unsigned char *peb = (unsigned char *)__readgsqword(0x60);
    RTL_CRITICAL_SECTION *cs = *(RTL_CRITICAL_SECTION **)(peb + 0x110);
    if (!cs) return -1;
    return (DWORD)(ULONG_PTR)cs->OwningThread == GetCurrentThreadId();
}

/* The base name of the module holding address a ("?" if none). */
static inline void module_of(const void *a, char *out, size_t n) {
    HMODULE m = NULL;
    wchar_t w[MAX_PATH];
    snprintf(out, n, "?");
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)a, &m))
        return;
    DWORD k = GetModuleFileNameW(m, w, MAX_PATH);
    if (!k) return;
    const wchar_t *b = wcsrchr(w, L'\\');
    b = b ? b + 1 : w;
    WideCharToMultiByte(CP_UTF8, 0, b, -1, out, (int)n, NULL, NULL);
    out[n - 1] = 0;
}

#define MAXMOD 512
typedef struct { HMODULE m[MAXMOD]; DWORD n; } modset;

static inline void modules_now(modset *s) {
    DWORD need = 0;
    s->n = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), s->m, sizeof s->m, &need)) s->n = need / sizeof(HMODULE);
    if (s->n > MAXMOD) s->n = MAXMOD;
}

/* Logs every module in `after` that isn't in `before` (before may be NULL: logs all). */
static inline void modules_log(const char *what, const modset *before, const modset *after) {
    char line[1400];
    int k = snprintf(line, sizeof line, "%s:", what);
    int any = 0;
    for (DWORD i = 0; i < after->n; i++) {
        int seen = 0;
        if (before)
            for (DWORD j = 0; j < before->n; j++)
                if (before->m[j] == after->m[i]) { seen = 1; break; }
        if (seen) continue;
        char name[MAX_PATH];
        module_of(after->m[i], name, sizeof name);
        if (k > (int)sizeof line - 80) { plog("%s ...", line); k = snprintf(line, sizeof line, "%s (cont.):", what); }
        k += snprintf(line + k, sizeof line - k, " %s", name);
        any = 1;
    }
    if (!any) k += snprintf(line + k, sizeof line - k, " (none)");
    plog("%s", line);
}
#endif
