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

int main(int argc, char **argv) {
#ifdef _WIN32
    if (plat_exe_start()) { fprintf(stderr, "plat_exe_start failed\n"); return 1; }
#endif
    if (argc == 3 && !strcmp(argv[1], "--make")) return make(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--open")) return open_(argv[2]);
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "--library")) return read_library(argc == 3 ? argv[2] : NULL);
    fprintf(stderr, "usage: %s --make DB | --open DB | --library [DB]\n", argv[0]);
    return 2;
}
