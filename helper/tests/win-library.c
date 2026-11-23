/* SQLCipher with LibTomCrypt, the Windows helper's (helper.md §1 "Build", §2a P-H5; T3), and the
 * Mac's CommonCrypto build of the same amalgamation, from one source:
 *
 *   win-library-test --make DB       a SQLCipher 4 database with a test key (never rekordbox's)
 *   win-library-test --open DB       opens it with that key and reads it back; prints the key
 *                                    derivation's time (PBKDF2-HMAC-SHA512, 256,000 rounds: what
 *                                    every --playlists and --tracks pays, P-H5)
 *   win-library-test --library [DB]  rekordbox's master.db (default: library_default_path) through
 *                                    library.c and its built-in key: the time to open it and
 *                                    the number of playlists, nothing else (no names, no paths)
 *   win-library-test --open-alt DB   as --open, with the derivation through the other PBKDF2
 *                                    (sqlc_kdf.h: CNG on Windows, CommonCrypto in the Mac's test),
 *                                    each call through the hook counted: it fails unless SQLCipher
 *                                    derived through it and no call fell back (review F1)
 *   win-library-test --kdf-check     that PBKDF2 against SQLCipher's provider (LibTomCrypt on
 *                                    Windows): known answers, then 256,000 rounds of SHA-512 on a
 *                                    test password, compared byte for byte (never printed), each
 *                                    timed (P-H5: the VM's and the PC's numbers decide); on Windows
 *                                    also kdf_cng_install's result, as library_open makes it
 *
 * helper/tests/win-sqlcipher.sh builds the Mac ones (CommonCrypto and LibTomCrypt) and checks each
 * opens the other's database; helper/build-win.sh builds rbsp-win-library-test.exe for the VM. The
 * library key is never printed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "plat.h"
#include "library.h"
#include "plat_helper.h"
#include "sqlc_kdf.h"

#if defined(_WIN32)
#define ALT_NAME "CNG"
#define alt_kdf kdf_cng
#else
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#define ALT_NAME "CommonCrypto"
static int alt_kdf(int algorithm, const unsigned char *pass, int pass_sz, const unsigned char *salt, int salt_sz, int rounds,
                   int key_sz, unsigned char *key) {
    CCPseudoRandomAlgorithm prf = algorithm == SQLC_PBKDF2_SHA1     ? kCCPRFHmacAlgSHA1
                                  : algorithm == SQLC_PBKDF2_SHA256 ? kCCPRFHmacAlgSHA256
                                  : algorithm == SQLC_PBKDF2_SHA512 ? kCCPRFHmacAlgSHA512
                                                                    : 0;
    if (!prf) return -1;
    return CCKeyDerivationPBKDF(kCCPBKDF2, (const char *)pass, (size_t)pass_sz, salt, (size_t)salt_sz, prf, (unsigned)rounds, key,
                                (size_t)key_sz) == kCCSuccess ? 0 : -1;
}
#endif

int sqlite3_key(sqlite3 *db, const void *key, int n);

static const char TEST_KEY[] = "rbsp test key, not rekordbox's";

static double ms_since(uint64_t t0) { return (double)(plat_now_mono_ns() - t0) / 1e6; }

static int make(const char *path) {
    sqlite3 *db = NULL;
    remove(path);
    int rc = sqlite3_open(path, &db);
    if (rc == SQLITE_OK) rc = sqlite3_key(db, TEST_KEY, (int)strlen(TEST_KEY));
    if (rc == SQLITE_OK)
        rc = sqlite3_exec(db, "CREATE TABLE t(a INTEGER, b TEXT); INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'thrée');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) fprintf(stderr, "make: %s\n", db ? sqlite3_errmsg(db) : "out of memory");
    sqlite3_close(db);
    return rc != SQLITE_OK;
}

static int sum_cb(void *ctx, int n, char **v, char **names) {
    (void)names;
    if (n == 2 && v[0] && v[1]) { *(long *)ctx += atol(v[0]); *(long *)ctx += (long)strlen(v[1]); }
    return 0;
}

static int open_(const char *path) {
    sqlite3 *db = NULL;
    double best = 1e30;
    long sum = 0;
    int rc = SQLITE_OK;
    for (int i = 0; i < 5 && rc == SQLITE_OK; i++) {         /* the best of 5: the derivation alone */
        sqlite3_close(db);
        db = NULL;
        rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_key(db, TEST_KEY, (int)strlen(TEST_KEY));
        uint64_t t0 = plat_now_mono_ns();
        if (rc == SQLITE_OK) rc = sqlite3_exec(db, "SELECT count(*) FROM sqlite_master", NULL, NULL, NULL);
        double ms = ms_since(t0);
        if (ms < best) best = ms;
    }
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, "SELECT a, b FROM t", sum_cb, &sum, NULL);
    if (rc != SQLITE_OK) { fprintf(stderr, "open: %s\n", db ? sqlite3_errmsg(db) : "out of memory"); sqlite3_close(db); return 1; }
    sqlite3_close(db);
    if (sum != 18) {                                        /* 1 + 3, 2 + 3, 3 + 6 (é is two bytes) */ fprintf(stderr, "open: read back %ld, not the rows written\n", sum); return 1; }
    printf("opened and read back; key derivation %.1f ms (best of 5)\n", best);
    return 0;
}

static int read_library(const char *path) {
    char def[2048], why[512];
    if (!path) { library_default_path(def, sizeof def); path = def; }
    uint64_t t0 = plat_now_mono_ns();
    library *l = library_open(path, why, sizeof why);
    double ms = ms_since(t0);
    if (!l) { printf("library: %s\n", why); return 1; }
    /* the listing into a scratch file, counted: nothing of it is printed */
    plat_file tf;
    FILE *f = plat_temp_file(&tf) ? NULL : plat_file_stdio(tf, "w+");
    if (!f && plat_file_ok(tf)) plat_file_close(tf);
    int rc = f ? library_list(l, f, why, sizeof why) : -1;
    long lines = 0;
    if (!rc) {
        rewind(f);
        for (int c; (c = getc(f)) != EOF;) lines += c == '\n';
    }
    if (f) fclose(f);
    library_close(l);
    if (rc) { printf("library: opened in %.1f ms; listing failed: %s\n", ms, why); return 1; }
    printf("library: opened in %.1f ms; %ld playlists and folders\n", ms, lines);
    return 0;
}

static int unhex(const char *h, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (unsigned char)v;
    }
    return 0;
}

/* PBKDF2's known answers (Python's hashlib.pbkdf2_hmac), then the round count SQLCipher 4 uses */
static int kdf_check(void) {
    static const struct { int alg, rounds, n; const char *pass, *salt, *hex; } kat[] = {
        {SQLC_PBKDF2_SHA512, 1, 64, "password", "salt",
         "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce"},
        {SQLC_PBKDF2_SHA512, 2, 64, "password", "salt",
         "e1d9c16aa681708a45f5c7c4e215ceb66e011a2e9f0040713f18aefdb866d53cf76cab2868a39b9f7840edce4fef5a82be67335c77a6068e04112754f27ccf4e"},
        {SQLC_PBKDF2_SHA512, 4096, 40, "passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt",
         "8c0511f4c6e597c6ac6315d8f0362e225f3c501495ba23b868c005174dc4ee71115b59f9e60cd953"},
        {SQLC_PBKDF2_SHA256, 4096, 40, "passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt",
         "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9"},
        {SQLC_PBKDF2_SHA1, 4096, 40, "passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt",
         "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038b6b89a48612c5a25284e6605e12329"},
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof kat / sizeof *kat; i++) {
        unsigned char want[64], a[64], b[64];
        int pl = (int)strlen(kat[i].pass), sl = (int)strlen(kat[i].salt);
        unhex(kat[i].hex, want, (size_t)kat[i].n);
        int ra = alt_kdf(kat[i].alg, (const unsigned char *)kat[i].pass, pl, (const unsigned char *)kat[i].salt, sl, kat[i].rounds, kat[i].n, a);
        int rb = sqlc_kdf_provider(kat[i].alg, (const unsigned char *)kat[i].pass, pl, (const unsigned char *)kat[i].salt, sl, kat[i].rounds,
                                   kat[i].n, b);
        int ok = !ra && !rb && !memcmp(a, want, (size_t)kat[i].n) && !memcmp(b, want, (size_t)kat[i].n);
        printf("%s known answer %zu (algorithm %d, %d rounds): " ALT_NAME " %s, SQLCipher's provider %s\n", ok ? "ok  " : "FAIL", i,
               kat[i].alg, kat[i].rounds, ra ? "failed" : memcmp(a, want, (size_t)kat[i].n) ? "differs" : "same",
               rb ? "failed" : memcmp(b, want, (size_t)kat[i].n) ? "differs" : "same");
        bad += !ok;
    }
    /* SQLCipher 4's page key: 256,000 rounds of HMAC-SHA512, a 16-byte salt, 32 bytes out */
    static const unsigned char salt[16] = {0x52, 0x42, 0x53, 0x50, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    unsigned char a[32], b[32];
    uint64_t t0 = plat_now_mono_ns();
    int ra = alt_kdf(SQLC_PBKDF2_SHA512, (const unsigned char *)TEST_KEY, (int)strlen(TEST_KEY), salt, 16, 256000, 32, a);
    double ma = ms_since(t0);
    t0 = plat_now_mono_ns();
    int rb = sqlc_kdf_provider(SQLC_PBKDF2_SHA512, (const unsigned char *)TEST_KEY, (int)strlen(TEST_KEY), salt, 16, 256000, 32, b);
    double mb = ms_since(t0);
    int ok = !ra && !rb && !memcmp(a, b, sizeof a);
    printf("%s 256,000 rounds: " ALT_NAME " and SQLCipher's provider %s; " ALT_NAME " %.1f ms, the provider %.1f ms\n", ok ? "ok  " : "FAIL",
           ra || rb ? "failed" : ok ? "give the same key" : "differ", ma, mb);
#if defined(_WIN32)
    int inst = kdf_cng_install();               /* what library_open calls first */
    printf("%s kdf_cng_install: %d\n", inst == 0 ? "ok  " : "FAIL", inst);
    bad += inst != 0;
#endif
    return bad || !ok;
}

/* alt_kdf through SQLCipher's hook, counted: calls and the ones that failed (SQLCipher's own
 * derivation then) */
static int hook_calls, hook_fails;
static int counted_kdf(int algorithm, const unsigned char *pass, int pass_sz, const unsigned char *salt, int salt_sz, int rounds,
                       int key_sz, unsigned char *key) {
    int r = alt_kdf(algorithm, pass, pass_sz, salt, salt_sz, rounds, key_sz, key);
    hook_calls++;
    hook_fails += r != 0;
    return r;
}

static int open_alt(const char *db) {
    if (sqlc_kdf_install(counted_kdf)) { fprintf(stderr, "can't install " ALT_NAME "'s PBKDF2\n"); return 1; }
    int r = open_(db);
    printf("%s " ALT_NAME " through SQLCipher's hook: %d derivation(s), %d fell back\n", !r && hook_calls > 0 && !hook_fails ? "ok  " : "FAIL",
           hook_calls, hook_fails);
    return r || hook_calls == 0 || hook_fails != 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    if (plat_exe_start()) { fprintf(stderr, "plat_exe_start failed\n"); return 1; }
#endif
    if (argc == 3 && !strcmp(argv[1], "--make")) return make(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--open")) return open_(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--open-alt")) return open_alt(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--kdf-check")) return kdf_check();
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "--library")) return read_library(argc == 3 ? argv[2] : NULL);
    fprintf(stderr, "usage: %s --make DB | --open DB | --open-alt DB | --kdf-check | --library [DB]\n", argv[0]);
    return 2;
}
