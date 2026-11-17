/* Phase 0 probe (helper.md Q5): the shape of djmdContent.FolderPath in a Windows rekordbox
 * library, read on the Mac from a copy of the VM's master.db. Prints only rows whose FolderPath
 * is a local file by the Windows predicate (a drive letter, or a UNC path); other rows (streaming
 * entries) are neither printed nor counted. The library key comes on stdin and is never printed.
 * Built with the helper's SQLCipher amalgamation (CommonCrypto):
 *   clang -DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_CC -DSQLITE_TEMP_STORE=2 \
 *     -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown \
 *     -I SQLC folderpath.c SQLC/sqlite3.c -framework Security -framework CoreFoundation
 *   folderpath DB < keyfile
 *   folderpath --check DB < keyfile   PRAGMA integrity_check only (the stub-crash investigation) */
#include <stdio.h>
#include <string.h>
#include "sqlite3.h"

int main(int argc, char **argv) {
    int check = argc == 3 && strcmp(argv[1], "--check") == 0;
    if (argc != 2 && !check) { fprintf(stderr, "usage: folderpath [--check] DB < keyfile\n"); return 2; }
    const char *path = argv[argc - 1];
    char key[256];
    if (!fgets(key, sizeof key, stdin)) { fprintf(stderr, "no key on stdin\n"); return 2; }
    key[strcspn(key, "\r\n")] = 0;
    sqlite3 *db;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) { fprintf(stderr, "open failed\n"); return 1; }
    sqlite3_key(db, key, (int)strlen(key));
    memset(key, 0, sizeof key);
    if (check) {
        sqlite3_stmt *c;
        if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &c, NULL) != SQLITE_OK) { fprintf(stderr, "check: %s\n", sqlite3_errmsg(db)); return 1; }
        /* passes only on exactly one row, "ok", followed by the end of the result */
        int rows = 0, ok = 0, rc;
        while ((rc = sqlite3_step(c)) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(c, 0);
            printf("integrity_check: %s\n", v ? v : "(null)");
            rows++;
            ok = rows == 1 && v && strcmp(v, "ok") == 0;
        }
        if (rc != SQLITE_DONE) fprintf(stderr, "check: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(c);
        sqlite3_close(db);
        return rc == SQLITE_DONE && rows == 1 && ok ? 0 : 1;
    }
    const char *q =
        "SELECT FolderPath, length(FolderPath) FROM djmdContent "
        "WHERE (FolderPath GLOB '[A-Za-z]:[/\\]*' OR FolderPath GLOB '[/\\][/\\]*') ORDER BY FolderPath";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, q, -1, &st, NULL) != SQLITE_OK) { fprintf(stderr, "query: %s\n", sqlite3_errmsg(db)); return 1; }
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        printf("%s\n", (const char *)sqlite3_column_text(st, 0));
        n++;
    }
    printf("(%d local rows)\n", n);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}
