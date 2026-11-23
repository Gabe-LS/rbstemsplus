/* CNG's PBKDF2 for SQLCipher on Windows (sqlc_kdf.h; helper.md §2a P-H5, T13) */
#include "sqlc_kdf.h"
#include "plat.h"

#include <windows.h>
#include <bcrypt.h>
#include <string.h>

typedef NTSTATUS(WINAPI *kw_open_fn)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS(WINAPI *kw_derive_fn)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, PUCHAR, ULONG, ULONGLONG, PUCHAR, ULONG, ULONG);
static kw_derive_fn kw_derive;
static BCRYPT_ALG_HANDLE kw_alg[3];     /* HMAC-SHA1, -SHA256, -SHA512: opened once, kept for the process */

#define KW_PROC(dst, mod, name)                     \
    do {                                            \
        FARPROC f_ = GetProcAddress((mod), (name)); \
        memcpy(&(dst), &f_, sizeof f_);             \
    } while (0)

static void kw_load(void) {
    /* bcrypt.dll isn't a KnownDLL: only its full System32 path (plat_win.c's sha256 does the same) */
    wchar_t path[MAX_PATH + 16];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    memcpy(path + n, L"\\bcrypt.dll", sizeof L"\\bcrypt.dll");
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return;                     /* never freed: it serves the process's life */
    kw_open_fn open = NULL;
    kw_derive_fn derive = NULL;
    KW_PROC(open, m, "BCryptOpenAlgorithmProvider");
    KW_PROC(derive, m, "BCryptDeriveKeyPBKDF2");
    if (!open || !derive) return;
    static const LPCWSTR names[3] = {BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA512_ALGORITHM};
    for (int i = 0; i < 3; i++)
        if (open(&kw_alg[i], names[i], NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) kw_alg[i] = NULL;
    kw_derive = derive;
}

int kdf_cng(int algorithm, const unsigned char *pass, int pass_sz, const unsigned char *salt, int salt_sz, int rounds,
            int key_sz, unsigned char *key) {
    static plat_once_t o = PLAT_ONCE_INIT;
    plat_once(&o, kw_load);
    if (algorithm < 0 || algorithm > 2 || !kw_derive || !kw_alg[algorithm]) return -1;
    if (pass_sz <= 0 || salt_sz <= 0 || rounds <= 0 || key_sz <= 0) return -1;   /* SQLCipher's own takes these */
    NTSTATUS s = kw_derive(kw_alg[algorithm], (PUCHAR)pass, (ULONG)pass_sz, (PUCHAR)salt, (ULONG)salt_sz, (ULONGLONG)rounds, key,
                           (ULONG)key_sz, 0);
    if (s != 0) { SecureZeroMemory(key, (size_t)key_sz); return -1; }
    return 0;
}

static int kw_installed = -1;
static void kw_install(void) { kw_installed = sqlc_kdf_install(kdf_cng); }

int kdf_cng_install(void) {
    static plat_once_t o = PLAT_ONCE_INIT;
    plat_once(&o, kw_install);
    return kw_installed;
}
