/* admin.c's write_file (the source folder's files) with its flushes observed and made to fail:
 * this file includes admin.c after plat.h, with plat_map_flush, plat_unmap and plat_fsync
 * renamed to the stubs below for admin.c only (they call the real ones). Each file is mapped,
 * written, its view flushed, unmapped, then the file flushed (MD4's order); a failed flush of
 * either kind comes back, and the file is removed. win/admin/tests/run.sh runs it.
 *   write-fault SCRATCH      exits 1 on a failure */
#include "plat.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int admin_test_map_rw(plat_file f, int64_t size, void **addr);
int admin_test_map_flush(void *addr, int64_t size);
void admin_test_unmap(void *addr, int64_t size);
int admin_test_fsync(plat_file f);
void admin_test_file_close(plat_file f);

#define plat_map_rw(f, s, a) admin_test_map_rw(f, s, a)
#define plat_map_flush(a, s) admin_test_map_flush(a, s)
#define plat_file_close(f) admin_test_file_close(f)
#define plat_unmap(a, s) admin_test_unmap(a, s)
#define plat_fsync(f) admin_test_fsync(f)
#include "../../c/admin.c"
#undef plat_map_rw
#undef plat_map_flush
#undef plat_file_close
#undef plat_unmap
#undef plat_fsync

static char order[64];   /* F: the view flushed, U: unmapped, S: the file flushed */
static int fail_map_flush, fail_fsync, checks, failures, bad_call;
static void *mapped;
static int64_t mapped_size;

static void note(char c) {
    size_t n = strlen(order);
    if (n + 1 < sizeof order) order[n] = c, order[n + 1] = 0;
}
int admin_test_map_rw(plat_file f, int64_t size, void **addr) {
    int e = plat_map_rw(f, size, addr);
    mapped = e ? NULL : *addr, mapped_size = e ? 0 : size;
    return e;
}
int admin_test_map_flush(void *addr, int64_t size) {
    note('F');
    /* the whole view, after its bytes are in: "{}" for payload.json, "sig" for its .sig */
    if (!addr || addr != mapped || size != mapped_size
        || !((size == 2 && !memcmp(addr, "{}", 2)) || (size == 3 && !memcmp(addr, "sig", 3)))) bad_call++;
    return fail_map_flush ? EIO : plat_map_flush(addr, size);
}
void admin_test_unmap(void *addr, int64_t size) {
    note('U');
    if (addr != mapped || size != mapped_size) bad_call++;
    plat_unmap(addr, size);
}
int admin_test_fsync(plat_file f) {
    note('S');
    return fail_fsync ? EIO : plat_fsync(f);
}

#define CHECK(cond, what)                                                               \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            failures++;                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s: %s (%s)\n", __FILE__, __LINE__, #cond, what, order); \
        }                                                                               \
    } while (0)

static void spit(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) fputs(text, f), fclose(f);
}
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

void admin_test_file_close(plat_file f) {
    note('C');
    plat_file_close(f);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char rel[1100], src[1100], pj[1200], sig[1200], out[1200];
    snprintf(rel, sizeof rel, "%s/rel", argv[1]);
    snprintf(src, sizeof src, "%s/source", argv[1]);
    mkdir(rel, 0755);
    snprintf(pj, sizeof pj, "%s/payload.json", rel);
    snprintf(sig, sizeof sig, "%s/payload.json.sig", rel);
    spit(pj, "{}");
    spit(sig, "sig");
    snprintf(out, sizeof out, "%s/payload.json", src);

    order[0] = 0;
    CHECK(!admin_source_prepare(src, rel, NULL, NULL, NULL) && holds(out, "{}"), "the pair copied");
    CHECK(!strcmp(order, "FUSCFUSC"), "each file: its view flushed, unmapped, then the file flushed");

    order[0] = 0;
    fail_map_flush = 1;
    int e = admin_source_prepare(src, rel, NULL, NULL, NULL);
    fail_map_flush = 0;
    CHECK(e == EIO && !there(out), "the view's flush failing: its error, the file removed");
    CHECK(!strcmp(order, "FUC"), "the view's flush failing: unmapped, the file not flushed, nothing else written");

    order[0] = 0;
    fail_fsync = 1;
    e = admin_source_prepare(src, rel, NULL, NULL, NULL);
    fail_fsync = 0;
    CHECK(e == EIO && !there(out), "the file's flush failing: its error, the file removed");
    CHECK(!strcmp(order, "FUSC"), "the file's flush failing: after the view's");

    order[0] = 0;
    CHECK(!admin_source_prepare(src, rel, NULL, NULL, NULL) && holds(out, "{}") && !strcmp(order, "FUSCFUSC"), "disarmed: copied again");
    CHECK(!bad_call, "each flush and unmap: the mapped view, whole, its bytes written");
    printf("write-fault: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
