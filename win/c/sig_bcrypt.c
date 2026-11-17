/* sig_p256_verify on Windows: BCrypt (app.md §5.1, §2 rule 4). bcrypt.dll isn't a KnownDLL and
 * isn't imported: it is loaded at the first call by its full System32 path with
 * LOAD_LIBRARY_SEARCH_SYSTEM32 (the worker's rule, app.md §5.5; the core DLL does the same), and
 * its functions are taken with GetProcAddress. The pseudo-handles BCRYPT_SHA256_ALG_HANDLE and
 * BCRYPT_ECDSA_P256_ALG_HANDLE (Windows 10+) need no provider to be opened or closed. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include "sig.h"

#include <string.h>

typedef NTSTATUS(WINAPI *hash_fn)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, PUCHAR, ULONG, PUCHAR, ULONG);
typedef NTSTATUS(WINAPI *import_fn)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE *, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *verify_fn)(BCRYPT_KEY_HANDLE, VOID *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *destroy_fn)(BCRYPT_KEY_HANDLE);

static struct {
    hash_fn hash;
    import_fn import;
    verify_fn verify;
    destroy_fn destroy;
} bc;
static INIT_ONCE bc_once = INIT_ONCE_STATIC_INIT;

/* GetProcAddress's FARPROC to the function's own type, through a generic function pointer */
#define PROC(m, name) ((void (*)(void))GetProcAddress((m), (name)))

static BOOL CALLBACK bc_load(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    wchar_t path[MAX_PATH];
    static const wchar_t name[] = L"\\bcrypt.dll";
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + sizeof name / sizeof name[0] > MAX_PATH) return TRUE;
    memcpy(path + n, name, sizeof name);
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return TRUE;
    hash_fn h = (hash_fn)PROC(m, "BCryptHash");
    import_fn i = (import_fn)PROC(m, "BCryptImportKeyPair");
    verify_fn v = (verify_fn)PROC(m, "BCryptVerifySignature");
    destroy_fn d = (destroy_fn)PROC(m, "BCryptDestroyKey");
    if (h && i && v && d) {   /* all or none: the module stays loaded for the process's life */
        bc.hash = h;
        bc.import = i;
        bc.verify = v;
        bc.destroy = d;
    }
    return TRUE;
}

int sig_p256_verify(const unsigned char xy[64], const void *data, size_t n, const unsigned char rs[64]) {
    InitOnceExecuteOnce(&bc_once, bc_load, NULL, NULL);
    if (!bc.hash) return -1;
    if (n > 0xffffffffu) return 0;   /* payload.json is capped far below (payload.c) */

    unsigned char digest[32];
    if (!BCRYPT_SUCCESS(bc.hash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)(uintptr_t)data, (ULONG)n, digest, sizeof digest)))
        return -1;

    struct {
        BCRYPT_ECCKEY_BLOB h;
        unsigned char xy[64];
    } blob;
    blob.h.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.h.cbKey = 32;
    memcpy(blob.xy, xy, 64);
    BCRYPT_KEY_HANDLE key = NULL;
    /* a point that isn't on the curve is refused here: that key verifies nothing */
    if (!BCRYPT_SUCCESS(bc.import(BCRYPT_ECDSA_P256_ALG_HANDLE, NULL, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)&blob,
                                  (ULONG)sizeof blob, 0)))
        return 0;
    unsigned char sig[64];
    memcpy(sig, rs, 64);
    NTSTATUS st = bc.verify(key, NULL, digest, sizeof digest, sig, sizeof sig, 0);
    bc.destroy(key);
    return BCRYPT_SUCCESS(st) ? 1 : 0;
}
