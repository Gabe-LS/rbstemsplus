/* The mapped view's flush failing (review WF6): run.sh builds fs_write.c, models.c and shell.c
 * with -Dplat_map_flush=test_map_flush, so their plat_map_flush calls land here (plat.h declares
 * it for that build instead of the Mac's inline 0). A write whose view flush fails returns that
 * error and leaves the old file and no temp file; install's copy whose flush fails is
 * MODELS_COPY_BAD, restore's MODELS_NOT_BACK, rekordbox's model as it was and no temp left.
 *
 *   flush-fault SCRATCH        exit 0 when every check passed */
#include "../fs_write.h"
#include "../models.h"
#include "../native_steps.h"
#include "../shell.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int armed, checks, failures;

int test_map_flush(void *addr, int64_t size);
int test_map_flush(void *addr, int64_t size) {
    (void)addr;
    (void)size;
    return armed ? EIO : 0;
}

/* models.c's engine file, as native_steps.c writes it (atomically, through the folder's handle) */
int native_support_write_at(plat_dir d, const char *name, const char *text, size_t n) { return win_write_at(d, name, text, n); }

#define CHECK(cond, what)                                                               \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            failures++;                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s: %s\n", __FILE__, __LINE__, #cond, what);  \
        }                                                                               \
    } while (0)

static void put(const char *path, const char *text) {
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
/* the entries in dir, and how many of them start with prefix */
static int entries(const char *dir, const char *prefix, int *with) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    *with = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            n++;
            if (!strncmp(e->d_name, prefix, strlen(prefix))) (*with)++;
        }
    closedir(d);
    return n;
}
static void sha_of(const char *text, char out[65]) {
    plat_sha256_ctx c;
    unsigned char s[32];
    plat_sha256_init(&c);
    plat_sha256_update(&c, text, strlen(text));
    plat_sha256_final(&c, s);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", s[i]);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char root[1024], dir[1100], originals[1100], payload[1100], p[1300], model[1300], ours[65], pioneer[65], why[128];
    int with;
    snprintf(root, sizeof root, "%s/flush-fault", argv[1]);
    if (mkdir(root, 0755) && errno != EEXIST) return 2;

    /* fs_write.c: by path and through a handle */
    snprintf(dir, sizeof dir, "%s/fs", root);
    mkdir(dir, 0755);
    CHECK(!win_write_atomic(dir, "w", "old", 3), "written");
    armed = 1;
    int e = win_write_atomic(dir, "w", "new!", 4);
    armed = 0;
    snprintf(p, sizeof p, "%s/w", dir);
    CHECK(e == EIO && holds(p, "old") && entries(dir, "", &with) == 1, "fs_write by path: the view's flush error, the old file, no temp file");
    plat_dir d;
    CHECK(!plat_dir_open(dir, &d), "the folder opens");
    armed = 1;
    e = win_write_at(d, "w", "new!", 4);
    armed = 0;
    plat_dir_close(d);
    CHECK(e == EIO && holds(p, "old") && entries(dir, "", &with) == 1, "fs_write through a handle: the view's flush error, the old file, no temp file");

    /* models.c: install's copy and restore's */
    snprintf(dir, sizeof dir, "%s/demucs3_model", root);
    snprintf(originals, sizeof originals, "%s/originals", root);
    snprintf(payload, sizeof payload, "%s/payload", root);
    mkdir(dir, 0755);
    mkdir(originals, 0755);
    mkdir(payload, 0755);
    snprintf(p, sizeof p, "%s/demucs3_ver.txt", dir);
    put(p, "0002");
    snprintf(p, sizeof p, "%s/model.onnx", payload);
    put(p, "OURS");
    snprintf(model, sizeof model, "%s/hdemucs.onnx", dir);
    sha_of("OURS", ours);
    sha_of("PIONEER", pioneer);
    critical_work w;
    critical_init(&w);
    models m = {.dir = dir, .originals = originals, .support = root, .critical = &w};
    models_add_ours(&m, ours);
    armed = 1;
    int r = models_install_ours(&m, payload, "model.onnx", ours, why);
    armed = 0;
    entries(dir, ".hdemucs.new", &with);
    CHECK(r == MODELS_COPY_BAD && access(model, F_OK) != 0 && with == 0, "install: the copy's view flush failing: COPY_BAD, no model put in, no temp left");
    CHECK(models_install_ours(&m, payload, "model.onnx", ours, why) == MODELS_OK && holds(model, "OURS"), "install: disarmed: ours in place");
    snprintf(p, sizeof p, "%s/%s.onnx", originals, pioneer);
    put(p, "PIONEER");
    armed = 1;
    r = models_restore(&m, why);
    armed = 0;
    entries(dir, ".hdemucs.restore", &with);
    CHECK(r == MODELS_NOT_BACK && holds(model, "OURS") && with == 0, "restore: the copy's view flush failing: NOT_BACK, ours stays, no temp left");
    CHECK(models_restore(&m, why) == MODELS_OK && holds(model, "PIONEER"), "restore: disarmed: Pioneer's back");
    printf("flush-fault: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
