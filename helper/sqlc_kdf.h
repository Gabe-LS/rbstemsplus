/* SQLCipher's key derivation through another PBKDF2 (helper.md §2a, P-H5; T13). Every
 * --playlists and --tracks opens rekordbox's library, and SQLCipher's first read derives the
 * page key: PBKDF2-HMAC-SHA512, 256,000 rounds. The Windows helper's SQLCipher uses LibTomCrypt
 * (CommonCrypto doesn't exist there), whose derivation took 226 ms against CommonCrypto's 93 ms
 * on the host Mac (helper/tests/win-sqlcipher.sh), past P-H5's "Mac + 100 ms" before any Windows
 * overhead. So on Windows only the derivation goes through CNG (BCryptDeriveKeyPBKDF2, the OS's),
 * and everything else (AES, the page HMACs, the random bytes) stays LibTomCrypt's.
 *
 * sqlc_kdf.c is SQLCipher's amalgamation plus sqlc_kdf_install, in one translation unit (it
 * includes sqlite3.c), so the provider's struct is SQLCipher's own, never a copy of its layout.
 * The Mac helper doesn't link it: its sqlite3.o is the amalgamation alone, as before. Nothing here
 * logs or keeps a password, a salt or a derived key. */
#ifndef RBSP_SQLC_KDF_H
#define RBSP_SQLC_KDF_H

/* SQLCipher's algorithm numbers (SQLCIPHER_PBKDF2_HMAC_*) */
enum { SQLC_PBKDF2_SHA1 = 0, SQLC_PBKDF2_SHA256 = 1, SQLC_PBKDF2_SHA512 = 2 };

/* PBKDF2 with HMAC over the algorithm's hash: key_sz bytes into key. 0, or -1 to have SQLCipher's
 * own provider derive it instead (an algorithm or a size this one doesn't take, or a failure) */
typedef int (*sqlc_pbkdf2)(int algorithm, const unsigned char *pass, int pass_sz, const unsigned char *salt, int salt_sz,
                           int rounds, int key_sz, unsigned char *key);

/* from now on SQLCipher derives its keys with fn (NULL: its provider's own again). Initializes
 * SQLite. 0, or -1 when SQLCipher has no provider */
int sqlc_kdf_install(sqlc_pbkdf2 fn);

/* the derivation by SQLCipher's provider itself (LibTomCrypt's on Windows), whatever is
 * installed: the tests' reference (helper/tests/win-library.c --kdf-check). 0 or -1 */
int sqlc_kdf_provider(int algorithm, const unsigned char *pass, int pass_sz, const unsigned char *salt, int salt_sz, int rounds,
                      int key_sz, unsigned char *key);

#if defined(_WIN32)
/* kdf_win.c: CNG's PBKDF2 (bcrypt.dll loaded once by its System32 path, never the search order:
 * I31), an sqlc_pbkdf2 */
int kdf_cng(int algorithm, const unsigned char *pass, int pass_sz, const unsigned char *salt, int salt_sz, int rounds,
            int key_sz, unsigned char *key);
/* sqlc_kdf_install(kdf_cng), once per process; 0, or -1 (SQLCipher's own then, LibTomCrypt's) */
int kdf_cng_install(void);
#endif

#endif
