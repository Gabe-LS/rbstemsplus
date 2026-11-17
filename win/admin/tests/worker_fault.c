/* worker.c's write_new (the staged, saved and runtime copies, the live DLL) with its flushes
 * observed and made to fail (review AF4/WF4): this file includes worker.c after plat.h, with
 * plat_map_rw, plat_map_flush, plat_unmap, plat_fsync and plat_file_close renamed to the stubs
 * below for worker.c only (they call the real ones), and calls write_new directly. Each file is
 * mapped, written, its whole view flushed, unmapped, the file flushed, then closed (F,U,S,C); a
 * failed flush of either kind comes back, the file is closed and removed. win/admin/tests/run.sh
 * runs it, plain and with ASan.
 *   worker-fault SCRATCH      exits 1 on a failure */
#include "plat.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int worker_test_map_rw(plat_file f, int64_t size, void **addr);
int worker_test_map_flush(void *addr, int64_t size);
void worker_test_unmap(void *addr, int64_t size);
int worker_test_fsync(plat_file f);
void worker_test_file_close(plat_file f);
int worker_test_file_create_at(plat_dir d, const char *name, int rw, plat_file *out);

#define plat_map_rw(f, s, a) worker_test_map_rw(f, s, a)
#define plat_map_flush(a, s) worker_test_map_flush(a, s)
#define plat_file_close(f) worker_test_file_close(f)
#define plat_unmap(a, s) worker_test_unmap(a, s)
#define plat_fsync(f) worker_test_fsync(f)
#define plat_file_create_at(d, n, rw, o) worker_test_file_create_at(d, n, rw, o)
#include "../worker.c"
#undef plat_map_rw
#undef plat_map_flush
#undef plat_file_close
#undef plat_unmap
#undef plat_fsync
#undef plat_file_create_at

static char order[64];   /* F: the view flushed, U: unmapped, S: the file flushed, C: closed */
static int fail_map_rw, fail_map_flush, fail_fsync, checks, failures, bad_call;
static void *mapped;
static int64_t mapped_size;
static plat_file created;   /* the handle plat_file_create_at gave */
static const char *expect;   /* the bytes write_new was given */

static void note(char c) {
    size_t n = strlen(order);
    if (n + 1 < sizeof order) order[n] = c, order[n + 1] = 0;
}
int worker_test_map_rw(plat_file f, int64_t size, void **addr) {
    note('M');
    if (memcmp(&f, &created, sizeof f)) bad_call++;
    int e = fail_map_rw ? (*addr = NULL, ENOSPC) : plat_map_rw(f, size, addr);
    mapped = e ? NULL : *addr, mapped_size = e ? 0 : size;
    return e;
}
int worker_test_map_flush(void *addr, int64_t size) {
    note('F');
    /* the whole view, after its bytes are in */
    if (!addr || addr != mapped || size != mapped_size || size != (int64_t)strlen(expect) || memcmp(addr, expect, (size_t)size)) bad_call++;
    return fail_map_flush ? ENOSPC : plat_map_flush(addr, size);
}
void worker_test_unmap(void *addr, int64_t size) {
    note('U');
    if (addr != mapped || size != mapped_size) bad_call++;
    plat_unmap(addr, size);
}
int worker_test_fsync(plat_file f) {
    note('S');
    if (memcmp(&f, &created, sizeof f)) bad_call++;
    return fail_fsync ? EACCES : plat_fsync(f);
}
void worker_test_file_close(plat_file f) {
    note('C');
    if (memcmp(&f, &created, sizeof f)) bad_call++;
    plat_file_close(f);
}
int worker_test_file_create_at(plat_dir d, const char *name, int rw, plat_file *out) {
    int e = plat_file_create_at(d, name, rw, out);
    if (!e) created = *out;
    return e;
}

#define CHECK(cond, what)                                                                       \
    do {                                                                                        \
        checks++;                                                                               \
        if (!(cond)) {                                                                          \
            failures++;                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s: %s (%s)\n", __FILE__, __LINE__, #cond, what, order); \
        }                                                                                       \
    } while (0)

static int holds(const char *path, const char *want) {
    char got[64];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(got, 1, sizeof got, f);
    fclose(f);
    return n == strlen(want) && !memcmp(got, want, n);
}
static int there(const char *path) {
    struct stat st;
    return !lstat(path, &st);
}
/* write_new of text as name in d, the order recorded afresh */
static int put(plat_dir d, const char *name, const char *text) {
    order[0] = 0;
    expect = text;
    return write_new(d, name, text, strlen(text));
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char dir[1024], a[1100], b[1100];
    snprintf(dir, sizeof dir, "%s/worker-fault", argv[1]);
    if (mkdir(dir, 0755) && errno != EEXIST) return 2;
    snprintf(a, sizeof a, "%s/a.dll", dir);
    snprintf(b, sizeof b, "%s/b.dll", dir);
    plat_dir d;
    if (plat_dir_open(dir, &d)) return 2;

    CHECK(!put(d, "a.dll", "MZ") && holds(a, "MZ") && !strcmp(order, "MFUSC"), "a file: its view flushed, unmapped, the file flushed, closed");
    CHECK(!put(d, "b.dll", "MZ!") && holds(b, "MZ!") && !strcmp(order, "MFUSC"), "another length: the same order");

    fail_map_flush = 1;
    int e = put(d, "c.dll", "MZc");
    fail_map_flush = 0;
    snprintf(a, sizeof a, "%s/c.dll", dir);
    CHECK(e == ENOSPC && !there(a), "the view's flush failing: its error, the file removed");
    CHECK(!strcmp(order, "MFUC"), "the view's flush failing: unmapped and closed, the file not flushed");

    fail_fsync = 1;
    e = put(d, "d.dll", "MZd");
    fail_fsync = 0;
    snprintf(a, sizeof a, "%s/d.dll", dir);
    CHECK(e == EACCES && !there(a), "the file's flush failing: its error, the file removed");
    CHECK(!strcmp(order, "MFUSC"), "the file's flush failing: after the view's, then closed");

    snprintf(a, sizeof a, "%s/d.dll", dir);
    CHECK(!put(d, "d.dll", "MZd") && holds(a, "MZd") && !strcmp(order, "MFUSC"), "disarmed: written");
    /* the write probe (worker.c's Controlled Folder Access check, NULL, 0): nothing mapped, the file flushed and closed */
    snprintf(a, sizeof a, "%s/probe", dir);
    order[0] = 0;
    CHECK(!write_new(d, "probe", NULL, 0) && holds(a, "") && !strcmp(order, "SC"), "the write probe (NULL, 0): no view, flushed, closed");
    fail_map_rw = 1;
    e = put(d, "e.dll", "MZe");
    fail_map_rw = 0;
    snprintf(a, sizeof a, "%s/e.dll", dir);
    CHECK(e == ENOSPC && !there(a) && !strcmp(order, "MC"), "the map failing: its error, closed, the file removed");
    snprintf(a, sizeof a, "%s/a.dll", dir);
    CHECK(put(d, "a.dll", "XX") == EEXIST && holds(a, "MZ") && !strcmp(order, ""), "the name taken: EEXIST, the file there untouched");
    CHECK(!bad_call, "each flush and unmap: the mapped view, whole, its bytes written");
    plat_dir_close(d);
    printf("worker-fault: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
