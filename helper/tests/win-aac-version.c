/* Exercise the production System32 version lookup and its cache on the host. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "aac_mf.h"

#define plat_once pthread_once
#define MAX_PATH 260
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800
#define VS_FFI_SIGNATURE 0xfeef04bd
#define HIWORD(n) ((n) >> 16)
#define LOWORD(n) ((n) & 0xffff)

typedef uint32_t DWORD;
typedef unsigned UINT;
typedef void *HMODULE;
typedef void (*FARPROC)(void);
typedef struct {
    DWORD dwSignature, dwFileVersionMS, dwFileVersionLS;
} VS_FIXEDFILEINFO;

static int lookups, unavailable;
static int version_module;
static VS_FIXEDFILEINFO version = {VS_FFI_SIGNATURE, (10u << 16) | 3u, (65535u << 16) | 7u};

static UINT GetSystemDirectoryW(wchar_t *path, UINT cap) {
    const wchar_t *system = L"C:\\Windows\\System32";
    assert(cap > wcslen(system));
    wcscpy(path, system);
    return (UINT)wcslen(path);
}

static HMODULE LoadLibraryExW(const wchar_t *path, void *file, DWORD flags) {
    assert(!wcscmp(path, L"C:\\Windows\\System32\\version.dll"));
    assert(!file && flags == LOAD_LIBRARY_SEARCH_SYSTEM32);
    return &version_module;
}

static DWORD GetFileVersionInfoSizeW(const wchar_t *path, DWORD *handle) {
    assert(!wcscmp(path, L"C:\\Windows\\System32\\msauddecmft.dll"));
    *handle = 0;
    lookups++;
    return unavailable ? 0 : sizeof version;
}

static int GetFileVersionInfoW(const wchar_t *path, DWORD handle, DWORD len, void *data) {
    assert(!wcscmp(path, L"C:\\Windows\\System32\\msauddecmft.dll"));
    assert(handle == 0 && len == sizeof version);
    memcpy(data, &version, len);
    return 1;
}

static int VerQueryValueW(void *data, const wchar_t *name, void **value, UINT *len) {
    assert(!wcscmp(name, L"\\"));
    *value = data;
    *len = sizeof version;
    return 1;
}

static FARPROC GetProcAddress(HMODULE module, const char *name) {
    assert(module == &version_module);
    if (!strcmp(name, "GetFileVersionInfoSizeW")) return (FARPROC)GetFileVersionInfoSizeW;
    if (!strcmp(name, "GetFileVersionInfoW")) return (FARPROC)GetFileVersionInfoW;
    assert(!strcmp(name, "VerQueryValueW"));
    return (FARPROC)VerQueryValueW;
}

#include "aac-version.inc"

int main(int argc, char **argv) {
    assert(argc == 2);
    unavailable = !strcmp(argv[1], "unavailable");
    const char *expected = unavailable ? "" : "10.3.65535.7";
    assert(!strcmp(aac_mf_decoder_version(), expected));
    assert(!strcmp(aac_mf_decoder_version(), expected));
    if (lookups != 1) {
        fprintf(stderr, "FAIL AAC version (%s): %d lookups, expected 1\n", argv[1], lookups);
        return 1;
    }
    printf("AAC version (%s): 1 lookup across repeated calls\n", argv[1]);
    return 0;
}
