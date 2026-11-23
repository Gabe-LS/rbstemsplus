/* Runs the Windows bridge's cache policy with the host's directory operations.
 * Drive discovery is counted, and the prune race is injected at the walk checkpoint. */
#define RBSTEMS_WIN_CACHE_TEST
#define RBSTEMS_TEST_REAL_PATH "/unused"
static int plat_win_drive_remote(const char *path);
#ifndef CACHE_BRIDGE_SOURCE
#define CACHE_BRIDGE_SOURCE "../../rbstems_bridge.c"
#endif
#include CACHE_BRIDGE_SOURCE
#include "../../../win/c/volumes.h"

static int fail_owner_write;
static int discoveries;
static int remove_marker_during_walk;
static char swap_from[PATH_MAX], swap_to[PATH_MAX];
static int failures;
static const char model[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char key[] = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
static const char fixture_id[] = "ABCD1234-1111-2222-3333-123456789ABC";

static int plat_win_drive_remote(const char *path) {
    (void)path;
    return 0;
}

static int plat_mkdir_open_at(plat_dir d, const char *name, int not_indexed, plat_dir *out) {
    (void)not_indexed;
    int err = plat_mkdir_at(d, name);
    if (!err) err = plat_dir_open_at(d, name, out);
    if (!err && fail_owner_write) {
        if (fchmod(out->fd, 0555)) abort();
        fail_owner_write--;
    }
    return err;
}

static int plat_dir_open_at_held(plat_dir d, const char *name, plat_dir *out) {
    return plat_dir_open_at(d, name, out);
}

static int plat_dir_identity(plat_dir d, plat_id *id) {
    struct stat st;
    if (fstat(d.fd, &st)) return errno;
    *id = (plat_id){st.st_dev, {st.st_ino, 0}};
    return 0;
}

int vol_dir_guid(plat_dir dir, char out[VOL_GUID_MAX]) {
    (void)dir;
    snprintf(out, VOL_GUID_MAX, "\\\\?\\Volume{abcd1234-1111-2222-3333-123456789abc}\\");
    return 0;
}

int vol_dir_on_volume(plat_dir dir, const char *path) {
    (void)dir;
    (void)path;
    return 1;
}

int vol_saved_refusal(const char *path) {
    (void)path;
    discoveries++;
    return VOL_OK;
}

int vol_saved_watch(const char *path, vol_watch *watch) {
    (void)watch;
    return vol_saved_refusal(path);
}

void cache_test_pause(const char *name) {
    if (strcmp(name, "RBSTEMS_TEST_PRUNE_DELAY")) return;
    if (swap_from[0]) {
        if (rename(base_dir, swap_to) || rename(swap_from, base_dir)) abort();
        swap_from[0] = 0;
    }
    if (remove_marker_during_walk) {
        plat_dir d;
        if (plat_dir_open(base_dir, &d) || plat_unlink_at(d, MARKER_NAME)) abort();
        plat_dir_close(d);
    }
}

static void expect(int yes, const char *description) {
    if (!yes) {
        fprintf(stderr, "FAIL %s\n", description);
        failures++;
    }
}

static void put(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f || fputs(s, f) < 0 || fclose(f)) abort();
}

static void fixture(const char *path, int external, char leaf[PATH_MAX]) {
    char model_dir[PATH_MAX], prefix[PATH_MAX], marker[PATH_MAX], text[96];
    if (pathf(base_dir, sizeof base_dir, "%s", path) ||
        pathf(model_dir, sizeof model_dir, "%s/%s", path, model) ||
        pathf(prefix, sizeof prefix, "%s/%s/%.2s", path, model, key) ||
        pathf(leaf, PATH_MAX, "%s/%s.flac", prefix, key) ||
        pathf(marker, sizeof marker, "%s/%s", path, MARKER_NAME)) abort();
    if ((mkdir(path, 0755) && errno != EEXIST) ||
        (mkdir(model_dir, 0755) && errno != EEXIST) ||
        (mkdir(prefix, 0755) && errno != EEXIST)) abort();
    put(leaf, "saved stems");
    struct timeval past[2] = {{1, 0}, {1, 0}};
    if (utimes(leaf, past)) abort();
    snprintf(text, sizeof text, "id=%s\nformat=%s\n", fixture_id, MARKER_FORMAT);
    if (external) put(marker, text);
    memcpy(cache_id, fixture_id, sizeof cache_id);
    chosen = external;
    loc_state = LOC_AVAILABLE;
    plat_dir d;
    plat_stat st;
    if (plat_dir_open(base_dir, &d) || plat_dir_stat(d, &st)) abort();
    pin = st.id;
    plat_dir_close(d);
}

static void ordinary_opens(int n, int benchmark) {
    discoveries = 0;
    uint64_t start = plat_now_mono_ns();
    for (int i = 0; i < n; i++) {
        int err;
        plat_dir d = open_base(0, &err);
        if (!plat_dir_ok(d)) abort();
        plat_dir_close(d);
    }
    uint64_t elapsed = plat_now_mono_ns() - start;
    printf("I: %d ordinary opens, %d discoveries, %.1f ns/open\n", n, discoveries, (double)elapsed / n);
    if (!benchmark) expect(discoveries == 0, "I: ordinary opens only check the pinned folder identity");
}

/* Lane A, F1: initialization and free-space recovery must establish ownership. */
static int marked_local_slot(void) {
    plat_dir d;
    if (plat_dir_open(memo_dir, &d)) return 0;
    plat_id id;
    int marked = !plat_dir_identity(d, &id) && local_marker_matches(d, id);
    plat_dir_close(d);
    return marked;
}

static void t_this_pc_creation(const char *root) {
    char slot[PATH_MAX], marker[PATH_MAX], data[PATH_MAX], entry[PATH_MAX];
    if (pathf(slot, sizeof slot, "%s/this-pc", root) ||
        pathf(memo_dir, sizeof memo_dir, "%s/Cache", slot) ||
        pathf(marker, sizeof marker, "%s/this-pc-owner", memo_dir) ||
        pathf(data, sizeof data, "%s/keep.txt", memo_dir) ||
        pathf(entry, sizeof entry, "%s/%s/%.2s/%s.desc", memo_dir, model, key, key) ||
        mkdir(slot, 0755) || mkdir(memo_dir, 0755)) abort();
    config_path[0] = 0;
    chosen = 0;
    set_location();
    expect(marked_local_slot() && loc_now() == LOC_AVAILABLE, "F1: startup recovers an empty unmarked slot");

    if ((unlink(marker) && errno != ENOENT) || rmdir(memo_dir)) abort();
    enum { samples = 44100 };
    int16_t *pcm = calloc(samples * 8, sizeof *pcm);
    float *mix = calloc(samples * 2, sizeof *mix);
    if (!pcm || !mix) abort();
    setenv("RBSTEMS_TEST_DISK_GB", "1", 1);
    space_paused = -1;
    store_entry(model, key, pcm, mix, samples, 1, 0);
    expect(marked_local_slot() && !access(entry, F_OK), "F1: free-space recovery before a store recreates a marked slot");

    if (pathf(memo_dir, sizeof memo_dir, "%s/occupied", slot) ||
        pathf(marker, sizeof marker, "%s/this-pc-owner", memo_dir) ||
        pathf(data, sizeof data, "%s/keep.txt", memo_dir) ||
        pathf(entry, sizeof entry, "%s/%s", memo_dir, model) || mkdir(memo_dir, 0755)) abort();
    put(data, "keep these bytes");
    put(marker, "partial marker");
    set_location();
    space_paused = -1;
    store_entry(model, key, pcm, mix, samples, 1, 0);
    char got[32] = "";
    FILE *f = fopen(data, "rb");
    if (!f) abort();
    fread(got, 1, sizeof got - 1, f);
    fclose(f);
    char owner[32] = "";
    f = fopen(marker, "rb");
    if (!f) abort();
    fread(owner, 1, sizeof owner - 1, f);
    fclose(f);
    expect(loc_now() == LOC_UNAVAILABLE && !strcmp(loc_reason, "marker") &&
           !strcmp(got, "keep these bytes") && !strcmp(owner, "partial marker") &&
           access(entry, F_OK) && errno == ENOENT, "F1: populated unmarked slot is unavailable and its files are untouched");
    unsetenv("RBSTEMS_TEST_DISK_GB");
    free(mix);
    free(pcm);

    if (pathf(memo_dir, sizeof memo_dir, "%s/fault", slot)) abort();
    fail_owner_write = 1;
    set_location();
    expect(!fail_owner_write && loc_now() == LOC_UNAVAILABLE && access(memo_dir, F_OK) && errno == ENOENT,
           "F1: a failed marker write rolls back the folder and reports unavailable");
    set_location();
    expect(marked_local_slot() && loc_now() == LOC_AVAILABLE, "F1: a retry establishes the slot after a marker failure");
}

int main(int argc, char **argv) {
    char path[PATH_MAX], leaf[PATH_MAX];
    if (argc < 2 || pathf(path, sizeof path, "%s/chosen", argv[1])) return 2;
    maint = plat_queue_new("cache-test", PLAT_WORK_MAINT);
    fixture(path, 1, leaf);
    if (argc == 3 && !strcmp(argv[2], "--bench")) {
        ordinary_opens(100000, 1);
        return 0;
    }
    ordinary_opens(100, 0);
#ifdef CACHE_OWNER_MARKER
    int err;
    device_invalidated();
    plat_dir invalid = open_base(0, &err);
    expect(!plat_dir_ok(invalid), "I: a disk change with the same folder identity blocks opens");
    plat_dir_close(invalid);
    plat_queue_wait(maint);
    fixture(path, 1, leaf);
    entry_failed(EIO);
    invalid = open_base(0, &err);
    expect(!plat_dir_ok(invalid), "I: device I/O loss invalidates eligibility until rechecked");
    plat_dir_close(invalid);
    fixture(path, 1, leaf);
#endif
    remove_marker_during_walk = 1;
    prune();
    expect(access(leaf, F_OK) == 0, "J: a fresh eviction pass requires the marker on its base handle");
    remove_marker_during_walk = 0;

    if (pathf(swap_from, sizeof swap_from, "%s/replacement", argv[1]) ||
        pathf(swap_to, sizeof swap_to, "%s/original", argv[1]) ||
        pathf(path, sizeof path, "%s/replaced", argv[1])) abort();
    fixture(swap_from, 1, leaf);
    fixture(path, 1, leaf);
    /* The earlier repin logic can accept a copied marker even on the host's volume. */
    dispatch_semaphore_t resume = dispatch_semaphore_create(0);
    plat_async_b(maint, ^{ dispatch_semaphore_wait(resume, DISPATCH_TIME_FOREVER); });
    setenv("RBSTEMS_TEST_ANY_VOLUME", "1", 1);
    prune();
    unsetenv("RBSTEMS_TEST_ANY_VOLUME");
    swap_from[0] = 0;
    expect(access(leaf, F_OK) == 0, "J: eviction never reopens a replacement at the configured path");
    if (pathf(leaf, sizeof leaf, "%s/%s/%.2s/%s.flac", swap_to, model, key, key)) abort();
    expect(access(leaf, F_OK) != 0 && errno == ENOENT, "J: eviction uses the original marked base");
    dispatch_semaphore_signal(resume);
    plat_queue_wait(maint);
    dispatch_release(resume);

    if (pathf(path, sizeof path, "%s/local", argv[1])) abort();
    fixture(path, 0, leaf);
    prune();
    expect(access(leaf, F_OK) == 0, "J: an unmarked default cache is left alone");
#ifdef CACHE_OWNER_MARKER
    if (pathf(memo_dir, sizeof memo_dir, "%s/created", argv[1])) abort();
    make_local_cache();
    fixture(memo_dir, 0, leaf);
    prune();
    expect(access(leaf, F_OK) != 0 && errno == ENOENT, "J: the bridge's marked local cache can be pruned");

    if (pathf(path, sizeof path, "%s/core-owned", argv[1])) abort();
    fixture(path, 0, leaf);
    char owner[PATH_MAX], identity[100];
    if (pathf(owner, sizeof owner, "%s/this-pc-owner", path)) abort();
    snprintf(identity, sizeof identity, "\\\\?\\Volume{abcd1234-1111-2222-3333-123456789abc}\\%016llx%016llx%016llx",
             (unsigned long long)pin.vol, (unsigned long long)pin.file[0], (unsigned long long)pin.file[1]);
    put(owner, identity);
    prune();
    expect(access(leaf, F_OK) != 0 && errno == ENOENT, "J: the core's This PC marker authorizes bridge prune");
#endif
    t_this_pc_creation(argv[1]);
    printf("cache policy: %d failed\n", failures);
    return failures != 0;
}
