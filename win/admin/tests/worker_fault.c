/* worker.c's write_new (the staged, saved and runtime copies, the live DLL) with its flushes
 * observed and made to fail (review AF4/WF4): this file includes worker.c after plat.h, with
 * plat_map_rw, plat_map_flush, plat_unmap, plat_fsync and plat_file_close renamed to the stubs
 * below for worker.c only (they call the real ones), and calls write_new directly. Each file is
 * mapped, written, its whole view flushed, unmapped, the file flushed, then closed (F,U,S,C); a
 * failed flush of either kind comes back, the file is closed and removed. win/admin/tests/run.sh
 * runs it, plain and with ASan. Also forces directory listing failures and an oversized
 * list of rekordbox versions.
 *   worker-fault SCRATCH      exits 1 on a failure */
#include "plat.h"
#include "fake_os.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int worker_test_map_rw(plat_file f, int64_t size, void **addr);
int worker_test_map_flush(void *addr, int64_t size);
void worker_test_unmap(void *addr, int64_t size);
int worker_test_fsync(plat_file f);
void worker_test_file_close(plat_file f);
int worker_test_file_create_at(plat_dir d, const char *name, int rw, plat_file *out);
int worker_test_dir_each(plat_dir d, plat_each_fn fn, void *ctx);
int worker_test_find_rekordbox(const compat_os *os, const char *root, compat_rekordbox *out, int max, int *current, int *state);
int worker_test_file_facts(const compat_os *os, const char *path, int flags, compat_file *out);

#define plat_map_rw(f, s, a) worker_test_map_rw(f, s, a)
#define plat_map_flush(a, s) worker_test_map_flush(a, s)
#define plat_file_close(f) worker_test_file_close(f)
#define plat_unmap(a, s) worker_test_unmap(a, s)
#define plat_fsync(f) worker_test_fsync(f)
#define plat_file_create_at(d, n, rw, o) worker_test_file_create_at(d, n, rw, o)
#define plat_dir_each(d, fn, ctx) worker_test_dir_each(d, fn, ctx)
#define compat_find_rekordbox(os, r, o, m, c, s) worker_test_find_rekordbox(os, r, o, m, c, s)
#define compat_file_facts(os, p, f, o) worker_test_file_facts(os, p, f, o)
#include "../worker.c"
#undef plat_map_rw
#undef plat_map_flush
#undef plat_file_close
#undef plat_unmap
#undef plat_fsync
#undef plat_file_create_at
#undef plat_dir_each
#undef compat_find_rekordbox
#undef compat_file_facts

static char order[64];   /* F: the view flushed, U: unmapped, S: the file flushed, C: closed */
static int fail_map_rw, fail_map_flush, fail_fsync, checks, failures, bad_call;
static void *mapped;
static int64_t mapped_size;
static plat_file created;   /* the handle plat_file_create_at gave */
static const char *expect;   /* the bytes write_new was given */
static struct stat list_failure, version_list;
static int failed_fd = -1, self_removals;
static const char current_version[] = "rekordbox 7.2.20", older_version[] = "rekordbox 7.2.19";

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
int worker_test_dir_each(plat_dir d, plat_each_fn fn, void *ctx) {
    struct stat st;
    if (fstat(d.fd, &st)) return errno;
    if (st.st_dev == list_failure.st_dev && st.st_ino == list_failure.st_ino) {
        failed_fd = d.fd;
        return EIO;
    }
    if (st.st_dev == version_list.st_dev && st.st_ino == version_list.st_ino) {
        plat_stat item = {.kind = PLAT_FILE};
        for (int i = 0; i < 127; i++) {
            char name[32];
            snprintf(name, sizeof name, "extra-%03d", i);
            fn(ctx, name, &item);
        }
        item.kind = PLAT_DIR;
        fn(ctx, current_version, &item);
        fn(ctx, older_version, &item);
        return 0;
    }
    return plat_dir_each(d, fn, ctx);
}

/* The current version is already known. Give only its exe canned facts. */
int worker_test_find_rekordbox(const compat_os *os, const char *root, compat_rekordbox *out, int max, int *current, int *state) {
    (void)os;
    (void)max;
    snprintf(out->folder, sizeof out->folder, "%s/%s", root, current_version);
    snprintf(out->folder_version, sizeof out->folder_version, "7.2.20");
    *current = 0;
    *state = COMPAT_RB_OK;
    return 1;
}

int worker_test_file_facts(const compat_os *os, const char *path, int flags, compat_file *out) {
    const char *name = strrchr(path, '/');
    if (!name || strcmp(name + 1, "rekordbox.exe")) return compat_file_facts(os, path, flags, out);
    *out = (compat_file){.readable = 1, .pin = COMPAT_PIN_OK, .pe.product_version = "7.2.20"};
    return 0;
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

static int delete_self(void *ctx, const char *self, const char *const folders[3], int *reboot) {
    (void)ctx;
    (void)self;
    (void)folders;
    self_removals++;
    *reboot = ADMIN_GONE_AT_RESTART;
    return 0;
}

static void remove_with_list_error(const char *root, const char *fault) {
    admin_env env = {.root = root};
    admin_os os = {.delete_self = delete_self};
    run r = {.e = &env, .os = &os, .self_in_bin = 1, .self12 = "0123456789ab"};
    r.pioneer = r.ort = r.bin = r.stage = r.receipts = plat_dir_cwd();
    if (stat(fault, &list_failure) || plat_dir_open(root, &r.root)) abort();
    failed_fd = -1;
    CHECK(remove_root(&r) == ADMIN_EXIT_FAILED, "a listing error stops root removal");
    CHECK(!self_removals, "a listing error stops before scheduling self-removal");
    int closed = failed_fd >= 0 && fcntl(failed_fd, F_GETFD) == -1 && errno == EBADF;
    CHECK(closed, "the directory whose listing failed was closed");
    if (!closed && failed_fd >= 0) plat_dir_close((plat_dir){failed_fd});
}

static void version_list_overflow(const char *scratch) {
    char rb[1100], root[1100], saved[1200], ort[1200], old[1200], current[1200];
    char original[1300], runtime[1300], bridge[1300], sums[160], sha[65];
    snprintf(rb, sizeof rb, "%s/rekordbox", scratch);
    snprintf(root, sizeof root, "%s/support", scratch);
    snprintf(saved, sizeof saved, "%s/pioneer", root);
    snprintf(ort, sizeof ort, "%s/ort", root);
    snprintf(old, sizeof old, "%s/%s", rb, older_version);
    snprintf(current, sizeof current, "%s/%s", rb, current_version);
    if (mkdir(rb, 0755) || mkdir(root, 0755) || mkdir(saved, 0755) || mkdir(ort, 0755) ||
        mkdir(old, 0755) || mkdir(current, 0755) || stat(rb, &version_list)) abort();
    plat_dir d, v;
    if (plat_dir_open(saved, &d) || plat_mkdir_at(d, "7.2.19") || plat_dir_open_at(d, "7.2.19", &v)) abort();
    if (sha_hex("original", 8, sha) || !admin_sums_line(sha, sums, sizeof sums) || put(v, SUMS, sums) || put(v, LIVE, "original")) abort();
    plat_dir_close(v);
    plat_dir_close(d);
    if (plat_dir_open(ort, &d) || put(d, LIVE, "original")) abort();
    plat_dir_close(d);
    if (plat_dir_open(old, &d) || put(d, LIVE, "rbstems bridge: 1")) abort();
    plat_dir_close(d);
    snprintf(original, sizeof original, "%s/7.2.19/%s", saved, LIVE);
    snprintf(runtime, sizeof runtime, "%s/%s", ort, LIVE);
    snprintf(bridge, sizeof bridge, "%s/%s", old, LIVE);

    static fake f;
    fake_init(&f);
    admin_env env = {.root = root, .rekordbox = rb};
    /* (the install reads the inventory, not this listing: no "one bridged version at a time" scan, D1) */
    admin_args a = {.verb = ADMIN_UNINSTALL, .all = 1};
    CHECK(admin_run(&env, &f.os, &a) == ADMIN_EXIT_CHECK_FAILED, "uninstall refuses a truncated list");
    CHECK(holds(original, "original") && holds(runtime, "original") && holds(bridge, "rbstems bridge: 1"),
          "the omitted version's saved original, runtime and live bridge stay intact");
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

    char root[1024], bin[1100], own[1200], left[1300];
    snprintf(root, sizeof root, "%s/remove-root", argv[1]);
    snprintf(bin, sizeof bin, "%s/bin", root);
    snprintf(own, sizeof own, "%s/0123456789ab", bin);
    snprintf(left, sizeof left, "%s/leftover", own);
    if (mkdir(root, 0755) || mkdir(bin, 0755) || mkdir(own, 0755) || plat_dir_open(own, &d)) return 2;
    if (write_new(d, "leftover", NULL, 0)) return 2;
    plat_dir_close(d);
    remove_with_list_error(root, bin);
    remove_with_list_error(root, own);
    CHECK(there(left), "files in an unlisted worker folder are still there");
    version_list_overflow(argv[1]);

    printf("worker-fault: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
