/* fs_write.c with its flush failing (review AW3): run.sh builds fs_write.c a second time with
 * -Dplat_fsync=fs_test_fsync, so its plat_fsync calls land here; this file calls the real one
 * (plat_mac.c) unless a failure is armed. A write whose flush fails must return that error,
 * leave the old file as it was and leave no temp file, by path and through a handle.
 *
 *   fs-write-fault SCRATCH        exit 0 when every check passed */
#include "../fs_write.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int fs_test_fsync(plat_file f);

static int armed, checks, failures;

int fs_test_fsync(plat_file f) {
    if (armed) return EIO;
    return plat_fsync(f);
}

#define CHECK(cond, what)                                                          \
    do {                                                                           \
        checks++;                                                                  \
        if (!(cond)) {                                                             \
            failures++;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s: %s\n", __FILE__, __LINE__, #cond, what); \
        }                                                                          \
    } while (0)

/* the number of entries in dir */
static int entries(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
    return n;
}

/* whether dir/name holds exactly want */
static int holds(const char *dir, const char *name, const char *want) {
    char p[1100], got[64];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(got, 1, sizeof got, f);
    fclose(f);
    return n == strlen(want) && !memcmp(got, want, n);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/fs-write-fault", argv[1]);
    if (mkdir(dir, 0755) && errno != EEXIST) return 2;

    CHECK(!win_write_atomic(dir, "w", "old", 3) && holds(dir, "w", "old") && entries(dir) == 1, "written");
    armed = 1;
    int e = win_write_atomic(dir, "w", "new!", 4);
    armed = 0;
    CHECK(e == EIO, "by path: the flush's error comes back");
    CHECK(holds(dir, "w", "old") && entries(dir) == 1, "by path: the old file stays, no temp file left");

    plat_dir d;
    CHECK(!plat_dir_open(dir, &d), "the folder opens");
    armed = 1;
    e = win_write_at(d, "w", "new!", 4);
    armed = 0;
    plat_dir_close(d);
    CHECK(e == EIO, "through a handle: the flush's error comes back");
    CHECK(holds(dir, "w", "old") && entries(dir) == 1, "through a handle: the old file stays, no temp file left");

    CHECK(!win_write_atomic(dir, "w", "new!", 4) && holds(dir, "w", "new!") && entries(dir) == 1, "disarmed: replaced");
    printf("fs-write-fault: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
