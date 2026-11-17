/* Phase 0 probe (the Windows plan §3; helper.md Q1, Q6): what CPU an x64 program sees (under Prism
 * on ARM64), which synth rekordbox's libmpg123.dll picks, and rekordbox's own FMA3 flag.
 *
 *   cpuprobe.exe                       CPUID leaves 1 and 7, XGETBV, IsProcessorFeaturePresent
 *   cpuprobe.exe --mpg123 DLL          also: the DLL's decoders and the one it picks
 *   cpuprobe.exe --rekordbox           also: the running rekordbox.exe's flag at RVA 0x619ece8
 *                                      (rekordbox 7.2.19's __use_fma3_lib, helper.md §2)
 * Builds for x64 (CPUID) and arm64 (IsProcessorFeaturePresent only). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

static void features(void) {
#if defined(__x86_64__)
    unsigned a, b, c, d;
    __cpuid(0, a, b, c, d);
    char vendor[13];
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4); vendor[12] = 0;
    printf("cpuid vendor %s, max leaf %u\n", vendor, a);
    __cpuid(1, a, b, c, d);
    printf("leaf1 ecx=%08x edx=%08x: SSE2=%u SSE3=%u SSSE3=%u SSE4.1=%u SSE4.2=%u POPCNT=%u FMA=%u OSXSAVE=%u AVX=%u F16C=%u\n", c, d,
           (d >> 26) & 1, c & 1, (c >> 9) & 1, (c >> 19) & 1, (c >> 20) & 1, (c >> 23) & 1, (c >> 12) & 1, (c >> 27) & 1, (c >> 28) & 1, (c >> 29) & 1);
    unsigned osx = (c >> 27) & 1;
    if (a >= 7) {
        unsigned a7, b7, c7, d7;
        __cpuid_count(7, 0, a7, b7, c7, d7);
        printf("leaf7 ebx=%08x: AVX2=%u BMI1=%u BMI2=%u AVX512F=%u\n", b7, (b7 >> 5) & 1, (b7 >> 3) & 1, (b7 >> 8) & 1, (b7 >> 16) & 1);
    }
    if (osx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        printf("xgetbv(0)=%08x%08x: XMM=%u YMM=%u\n", hi, lo, (lo >> 1) & 1, (lo >> 2) & 1);
    }
#endif
    printf("IsProcessorFeaturePresent: SSE3=%d SSSE3=%d SSE4.1=%d SSE4.2=%d AVX=%d AVX2=%d AVX512F=%d ARM_V8_CRC32=%d ARM_V81_ATOMIC=%d\n",
           IsProcessorFeaturePresent(13), IsProcessorFeaturePresent(36), IsProcessorFeaturePresent(37), IsProcessorFeaturePresent(38),
           IsProcessorFeaturePresent(39), IsProcessorFeaturePresent(40), IsProcessorFeaturePresent(41), IsProcessorFeaturePresent(31),
           IsProcessorFeaturePresent(34));
    USHORT pm = 0, nm = 0;
    if (IsWow64Process2(GetCurrentProcess(), &pm, &nm)) printf("IsWow64Process2: process 0x%04x native 0x%04x\n", pm, nm);
}

typedef int (*init_f)(void);
typedef const char **(*list_f)(void);
typedef void *(*new_f)(const char *, int *);
typedef const char *(*cur_f)(void *);

static void mpg123(const char *path) {
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH);
    HMODULE m = LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) { printf("mpg123: load failed %lu\n", GetLastError()); return; }
    init_f init = (init_f)GetProcAddress(m, "mpg123_init");
    list_f dec = (list_f)GetProcAddress(m, "mpg123_decoders");
    list_f sup = (list_f)GetProcAddress(m, "mpg123_supported_decoders");
    new_f nw = (new_f)GetProcAddress(m, "mpg123_new");
    cur_f cur = (cur_f)GetProcAddress(m, "mpg123_current_decoder");
    printf("mpg123: init=%d\n", init ? init() : -99);
    if (dec) { printf("mpg123 built-in decoders:"); for (const char **p = dec(); p && *p; p++) printf(" %s", *p); printf("\n"); }
    if (sup) { printf("mpg123 supported here:"); for (const char **p = sup(); p && *p; p++) printf(" %s", *p); printf("\n"); }
    if (nw && cur) { int err = 0; void *h = nw(NULL, &err); printf("mpg123 default decoder: %s (err %d)\n", h ? cur(h) : "?", err); }
}

static void rekordbox_flag(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e; memset(&e, 0, sizeof e); e.dwSize = sizeof e;
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, L"rekordbox.exe") == 0) { pid = e.th32ProcessID; break; }
    CloseHandle(snap);
    if (!pid) { printf("rekordbox: not running\n"); return; }
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) { printf("rekordbox: open failed %lu\n", GetLastError()); return; }
    HMODULE mods[1]; DWORD need;
    if (!K32EnumProcessModulesEx(p, mods, sizeof mods, &need, LIST_MODULES_64BIT)) { printf("rekordbox: modules failed %lu\n", GetLastError()); return; }
    unsigned char *base = (unsigned char *)mods[0];
    int v = -1; SIZE_T got = 0;
    BOOL ok = ReadProcessMemory(p, base + 0x619ece8, &v, sizeof v, &got);
    printf("rekordbox pid %lu base %p: __use_fma3_lib (RVA 0x619ece8) = %d (%s)\n", pid, (void *)base, v, ok ? "read" : "read FAILED");
    CloseHandle(p);
}

int main(int argc, char **argv) {
    features();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mpg123") && i + 1 < argc) mpg123(argv[++i]);
        else if (!strcmp(argv[i], "--rekordbox")) rekordbox_flag();
    }
    return 0;
}
