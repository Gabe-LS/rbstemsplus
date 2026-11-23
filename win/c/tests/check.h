/* CHECK for win/c's tests: counts every check and failure across the test files (main.c). */
#ifndef RBSP_WIN_TEST_CHECK_H
#define RBSP_WIN_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int checks, failures;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

/* a stored zip made by the tests (unzip_test.c), with one entry bent when bend is set: index (-1
 * none), its flags, its method, a Unix symbolic link, a CRC that doesn't match */
typedef struct {
    const char *name;
    const void *data;
    size_t n;
} test_zfile;
typedef struct {
    int index;
    unsigned flags, method;
    int symlink, bad_crc;
} test_zbend;
unsigned char *test_zip(const test_zfile *f, int nf, size_t *n);
unsigned char *test_zip_ex(const test_zfile *f, int nf, size_t *n, const test_zbend *bend);

/* the test files' entry points; main.c runs them in this order */
void test_fixtures(const char *path);   /* fixtures.txt, from today's Swift (gen-fixtures.sh) */
void test_sig(void);
void test_payload(void);
void test_store(const char *scratch);   /* payload_store.c on plat_mac.c */
void test_host_facts(const char *scratch, const char *pe);   /* host_facts.c on a scratch tree and the PE fixtures */
void test_compat(const char *scratch, const char *pedir);   /* pedir: run.sh's PE fixtures */
void test_unzip(const char *scratch, const char *zips);     /* zips: run.sh's zips from other tools */
void test_update_rules(void);                               /* the pure rules; also in the Windows test exe */
void test_update(const char *scratch, const char *pedir);   /* the updater on fakes */
void test_native_facts(const char *scratch);               /* the core DLL's DRIVE, SIZE_WALK, ALPHATHETA */
void test_native_steps(const char *scratch);                /* the core DLL's PAYLOAD and CACHE_PREPARE */
void test_models(const char *scratch);                     /* models.c: rekordbox's model and the saved originals */
int test_model_fixtures(const char *fixtures, const char *scratch);   /* models.c against Models.swift: the cases run */
void test_sha_memo(const char *scratch);                    /* the core DLL's sha256 memo on plat_mac.c */
void test_afterexit(const char *scratch);                   /* afterexit.c: the uninstall's after-exit on fakes */
void test_native_location(const char *scratch);             /* native_location.c: CORE_SLOW_LOCATION's measure (HF5) */
void test_location_choice(const char *scratch);             /* native_steps.c: the location choice's steps (plan N3) */
void test_afterexit_win(void);                              /* afterexit_self_win.c on Windows' own folders (wintest.c) */
/* native_steps_test.c: a folder under scratch (its path about 780 bytes) holding one folder with a
 * 250-byte name, whose full path doesn't fit 1024 bytes. 0 and its path in out, or -1 */
int test_long_dir(const char *scratch, const char *tag, char *out, size_t n);

#endif
