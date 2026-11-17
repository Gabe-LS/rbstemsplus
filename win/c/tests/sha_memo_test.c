/* sha_memo.c (the core DLL's slow_fact sha256 memo, review NW6) on plat_mac.c: a hit hashes
 * nothing; a file replaced or with a new stamp is hashed again (a same-size rewrite that keeps
 * the stamp is a hit: the stamp is a cache key, while sha_file sees it); a file written while it
 * is hashed fails and is never memoised; the oldest path goes when the memo is full; two threads at once. */
#include "check.h"

#include "../sha_memo.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>

static const char HELLO[] = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";   /* sha256("hello") */
static const char WORLD[] = "486ea46224d1bb4fb680f34f7c9ad96a8f24ec88be73ea8e5a6c65260e9cb8a7";   /* sha256("world") */

static void put(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    fputs(s, f);
    fclose(f);
}

/* a stat hook whose last-write time is the test's (a finer clock than plat_stat's seconds) */
static int64_t fake_mtime, fake_ctime;
static int fake_stat(void *ctx, plat_file f, sha_memo_stamp *out) {
    (void)ctx;
    plat_stat st;
    int e = plat_file_stat(f, &st);
    if (e) return e;
    out->size = st.size;
    out->mtime = fake_mtime;
    out->ctime = fake_ctime;
    out->id[0] = st.id.vol;
    out->id[1] = st.id.file[0];
    return 0;
}
static void touch_during(void *ctx) {
    (void)ctx;
    fake_mtime++;
}

/* a stat hook with the file's real nanosecond last-write time (fstat), as fine as Windows' 100 ns */
static int ns_stat(void *ctx, plat_file f, sha_memo_stamp *out) {
    (void)ctx;
    struct stat s;
    if (fstat(f.fd, &s)) return errno;
    out->size = s.st_size;
    out->mtime = (int64_t)s.st_mtimespec.tv_sec * 1000000000 + s.st_mtimespec.tv_nsec;
    out->id[0] = (uint64_t)s.st_dev;
    out->id[1] = (uint64_t)s.st_ino;
    return 0;
}

/* a file of two 1 MiB chunks (the hash's), all one byte */
enum { TWO_CHUNKS = 2 << 20 };
static void fill(const char *path, const char *mode, int c) {
    FILE *f = fopen(path, mode);
    if (!f) abort();
    for (int i = 0; i < TWO_CHUNKS; i++) fputc(c, f);
    fclose(f);
}
/* between the hash's reads: the first time, both chunks rewritten in place with 'C' (same size),
 * so the hash reads the old first chunk and the new second one */
static const char *torn_path;
static int torn_calls;
static void rewrite_both(void *ctx) {
    (void)ctx;
    if (!torn_calls++) fill(torn_path, "r+b", 'C');
}

typedef struct { sha_memo *m; const char *path; int ok; } th_arg;
static void *hash_thread(void *p) {
    th_arg *a = p;
    char out[65];
    a->ok = 1;
    for (int i = 0; i < 200; i++) a->ok &= !sha_memo_file(a->m, a->path, out) && !strcmp(out, HELLO);
    return NULL;
}

void test_sha_memo(const char *scratch) {
    char a[1100], b[1100], tmp[1100];
    snprintf(a, sizeof a, "%s/hdemucs.onnx", scratch);
    snprintf(b, sizeof b, "%s/bridge.dll", scratch);
    snprintf(tmp, sizeof tmp, "%s/new.part", scratch);
    sha_memo m;
    char out[65];
    sha_memo_init(&m);
    put(a, "hello");
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, HELLO) && m.hashed == 1, "hashed: %s", out);
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, HELLO) && m.hashed == 1, "the same stamp: a hit, nothing hashed");
    /* replaced by a rename over it (a new file id), the same size */
    put(tmp, "world");
    CHECK(rename(tmp, a) == 0, "renamed");
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, WORLD) && m.hashed == 2, "replaced: hashed again (%s)", out);
    CHECK(!sha_memo_file(&m, a, out) && m.hashed == 2, "then a hit");
    /* another size */
    put(a, "hello!");
    CHECK(!sha_memo_file(&m, a, out) && strcmp(out, WORLD) && m.hashed == 3, "another size: hashed again");
    CHECK(sha_memo_file(&m, scratch, out) != 0, "a folder: not hashed");
    snprintf(tmp, sizeof tmp, "%s/missing", scratch);
    CHECK(sha_memo_file(&m, tmp, out) != 0, "missing: not hashed");
    CHECK(sha_file(scratch, out) != 0 && sha_file(tmp, out) != 0, "sha_file: a folder, missing: no hash");
    sha_memo_free(&m);

    /* the finer stamp: a rewrite in place with the same size and a new last-write time */
    sha_memo_init(&m);
    m.stat = fake_stat;
    put(a, "hello");
    fake_mtime = 100;
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, HELLO) && m.hashed == 1, "stamped");
    put(a, "world");
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, HELLO) && m.hashed == 1, "a same-size rewrite, stamp unchanged: the memo's (a cache key, core.h)");
    CHECK(!sha_file(a, out) && !strcmp(out, WORLD) && m.hashed == 1, "sha_file (uncached) sees that rewrite (%s)", out);
    fake_mtime = 101;
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, WORLD) && m.hashed == 2, "a new last-write time: hashed again");
    /* the last-write time restored (SetFileTime), the change time not: a miss */
    put(a, "hello");
    fake_ctime = 1;
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, HELLO) && m.hashed == 3, "a new change time: hashed again");
    put(a, "world");
    fake_ctime = 2;
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, WORLD) && m.hashed == 4, "and again");
    /* written while hashed: not memoised, so the next ask hashes again */
    m.between_reads = touch_during;
    put(b, "hello");
    CHECK(sha_memo_file(&m, b, out) != 0 && m.hashed == 5, "hashed while it changed: no hash");
    m.between_reads = NULL;
    CHECK(!sha_memo_file(&m, b, out) && !strcmp(out, HELLO) && m.hashed == 6, "not memoised then: hashed again");
    CHECK(!sha_memo_file(&m, b, out) && m.hashed == 6, "now memoised");
    /* a: memoised at 101; changing during its hash drops it, even its old stamp */
    fake_mtime = 102;
    m.between_reads = touch_during;
    CHECK(sha_memo_file(&m, a, out) != 0 && m.hashed == 7, "a rehashed while it changed: no hash");
    m.between_reads = NULL;
    fake_mtime = 101;
    CHECK(!sha_memo_file(&m, a, out) && m.hashed == 8, "its older stamp is gone too");
    sha_memo_free(&m);

    /* a real change of both chunks during the hash, under a real fine stamp: no hash, nothing
     * memoised; the next ask hashes what the file now holds */
    char c[1100];
    snprintf(c, sizeof c, "%s/c", scratch);
    fill(c, "wb", 'C');
    sha_memo_init(&m);
    char want[65];
    CHECK(!sha_memo_file(&m, c, want) && m.hashed == 1, "all C: hashed");
    sha_memo_free(&m);
    sha_memo_init(&m);
    m.stat = ns_stat;
    fill(a, "wb", 'A');
    torn_path = a;
    torn_calls = 0;
    m.between_reads = rewrite_both;
    CHECK(sha_memo_file(&m, a, out) != 0 && m.hashed == 1 && torn_calls >= 2, "both chunks changed while hashed: no hash");
    m.between_reads = NULL;
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, want) && m.hashed == 2, "then hashed again: all C (%s)", out);
    CHECK(!sha_memo_file(&m, a, out) && !strcmp(out, want) && m.hashed == 2, "and memoised");
    sha_memo_free(&m);

    /* full: the oldest path goes */
    sha_memo_init(&m);
    char p[SHA_MEMO_MAX + 1][1100];
    for (int i = 0; i <= SHA_MEMO_MAX; i++) {
        snprintf(p[i], sizeof p[i], "%s/f%d", scratch, i);
        put(p[i], "hello");
        sha_memo_file(&m, p[i], out);
    }
    CHECK(m.hashed == SHA_MEMO_MAX + 1, "every path hashed once");
    sha_memo_file(&m, p[SHA_MEMO_MAX], out);
    sha_memo_file(&m, p[1], out);
    CHECK(m.hashed == SHA_MEMO_MAX + 1, "the newest paths: hits");
    sha_memo_file(&m, p[0], out);
    CHECK(m.hashed == SHA_MEMO_MAX + 2, "the oldest: evicted, hashed again");
    sha_memo_free(&m);

    /* two workers at once */
    sha_memo_init(&m);
    put(a, "hello");
    th_arg t1 = {&m, a, 0}, t2 = {&m, a, 0};
    pthread_t x, y;
    pthread_create(&x, NULL, hash_thread, &t1);
    pthread_create(&y, NULL, hash_thread, &t2);
    pthread_join(x, NULL);
    pthread_join(y, NULL);
    CHECK(t1.ok && t2.ok && m.hashed >= 1 && m.hashed <= 2, "two threads: the right sha, hashed at most twice (%ld)", m.hashed);
    sha_memo_free(&m);
}
