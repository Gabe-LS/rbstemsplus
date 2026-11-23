/* SQLCipher's amalgamation with sqlc_kdf_install (sqlc_kdf.h): compiled in place of sqlite3.c,
 * with its options (helper/build-win.sh, helper/tests/win-sqlcipher.sh) */
#include "sqlite3.c"
#include "sqlc_kdf.h"

typedef int (*sk_kdf_fn)(void *ctx, int algorithm, const unsigned char *pass, int pass_sz, const unsigned char *salt, int salt_sz,
                         int workfactor, int key_sz, unsigned char *key);
static sk_kdf_fn sk_own;                /* the provider's own derivation */
static sqlc_pbkdf2 sk_fn;

static int sk_kdf(void *ctx, int algorithm, const unsigned char *pass, int pass_sz, const unsigned char *salt, int salt_sz,
                  int workfactor, int key_sz, unsigned char *key) {
    sqlc_pbkdf2 fn = sk_fn;
    if (fn && fn(algorithm, pass, pass_sz, salt, salt_sz, workfactor, key_sz, key) == 0) return SQLITE_OK;
    return sk_own(ctx, algorithm, pass, pass_sz, salt, salt_sz, workfactor, key_sz, key);
}

int sqlc_kdf_install(sqlc_pbkdf2 fn) {
    if (sqlite3_initialize() != SQLITE_OK) return -1;
    int r = -1;
    sqlite3_mutex_enter(sqlcipher_mutex(SQLCIPHER_MUTEX_PROVIDER));
    sqlcipher_provider *p = sqlcipher_get_provider();
    if (p && p->kdf) {
        if (p->kdf != sk_kdf) {
            sk_own = p->kdf;
            p->kdf = sk_kdf;
        }
        sk_fn = fn;
        r = 0;
    }
    sqlite3_mutex_leave(sqlcipher_mutex(SQLCIPHER_MUTEX_PROVIDER));
    return r;
}

int sqlc_kdf_provider(int algorithm, const unsigned char *pass, int pass_sz, const unsigned char *salt, int salt_sz, int rounds,
                      int key_sz, unsigned char *key) {
    if (sqlite3_initialize() != SQLITE_OK) return -1;
    sqlite3_mutex_enter(sqlcipher_mutex(SQLCIPHER_MUTEX_PROVIDER));
    sqlcipher_provider *p = sqlcipher_get_provider();
    sk_kdf_fn own = p ? (p->kdf == sk_kdf ? sk_own : p->kdf) : NULL;
    sqlite3_mutex_leave(sqlcipher_mutex(SQLCIPHER_MUTEX_PROVIDER));
    void *ctx = NULL;
    if (!own || p->ctx_init(&ctx) != SQLITE_OK) return -1;
    int rc = own(ctx, algorithm, pass, pass_sz, salt, salt_sz, rounds, key_sz, key);
    p->ctx_free(&ctx);
    return rc == SQLITE_OK ? 0 : -1;
}
