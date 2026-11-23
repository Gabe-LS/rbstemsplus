/* Windows kept rules with real files and injected platform failures. */
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "plat.h"

static int fail_alloc, fail_replace, fail_expiry;

static void *test_realloc(void *p, size_t n) {
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    return realloc(p, n);
}

static int test_rename(plat_dir d, const char *src, plat_dir dst, const char *name) {
    return fail_replace ? EBUSY : plat_rename_at(d, src, dst, name);
}

static int test_unlink(plat_dir d, const char *name) {
    return fail_expiry && !strcmp(name, "expired.kept") ? EBUSY : plat_unlink_at(d, name);
}

#define realloc test_realloc
#define plat_rename_at test_rename
#define plat_unlink_at test_unlink
#include "../kept.c"
#undef plat_unlink_at
#undef plat_rename_at
#undef realloc

#undef KEPT_FILE_MAX
#include "kept-parser.inc"

static int passed, failed;
static const char model[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static char why[160];

static void check(int ok, const char *name) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (ok) passed++;
    else failed++;
}

static void file(plat_dir d, const char *name, const char *data, size_t n) {
    plat_file f;
    assert(!plat_file_create_at(d, name, 0, &f));
    FILE *out = plat_file_stdio(f, "w");
    assert(out && fwrite(data, 1, n, out) == n && !fclose(out));
}

static size_t bridge_count(plat_dir d, const char *name, long long until) {
    size_t n = 0, count = 0;
    long long u;
    char *b = slurp(d, name, &n);
    assert(b && !parse_kept(b, n, &u, NULL, &count) && u == until);
    free(b);
    return count;
}

static void writes(const char *path, plat_dir d) {
    kept k = {.name = "set", .until = 2000000000};
    assert(strlcpy(k.dir, path, sizeof k.dir) < sizeof k.dir);
    line earlier = {{1}, {1}}, current = {{1}, {2}};
    k.earlier = (lines){&earlier, 1, 1};
    k.now = (lines){&current, 1, 1};
    assert(!kept_write(&k, 0, why, sizeof why));
    size_t len = 0, count = 0;
    long long u = 0;
    kept_entry rows[2];
    char *saved = slurp(d, "set.kept", &len);
    check(saved && !memchr(saved, '\r', len) && !parse_kept(saved, len, &u, NULL, &count) &&
          u == k.until && count == 2 && !parse_kept(saved, len, &u, rows, &count) &&
          rows[0].key[0] == 1 && rows[1].key[0] == 2,
          "bridge reads both sorted rows after an interrupted write");
    free(saved);

    fail_alloc = 1;
    k.now.n = 0;                        /* fail specifically on the append of an earlier row */
    check(kept_write(&k, 0, why, sizeof why) && !strcmp(why, "out of memory") && !fail_alloc &&
          bridge_count(d, "set.kept", k.until) == 2, "allocation failure leaves the previous set published");
    k.now.n = 1;

    fail_replace = 1;
    int rc = kept_write(&k, 1, why, sizeof why);
    fail_replace = 0;
    char tmp[120];
    snprintf(tmp, sizeof tmp, ".set.kept.%d.tmp", plat_pid());
    plat_stat st;
    check(rc && plat_stat_at(d, tmp, &st) == ENOENT && bridge_count(d, "set.kept", k.until) == 2,
          "replace failure preserves the old set and removes the temporary file");

    k.until = 1000000000000LL;
    check(kept_write(&k, 1, why, sizeof why) && bridge_count(d, "set.kept", 2000000000) == 2,
          "a 13-digit expiry cannot replace a readable set");
    k.until = 2000000000;
    k.now.n = KEPT_FILE_MAX / KEPT_LINE + 1;
    k.now.v = calloc(k.now.n, sizeof *k.now.v);
    assert(k.now.v);
    check(kept_write(&k, 1, why, sizeof why) && bridge_count(d, "set.kept", k.until) == 2,
          "an oversized set cannot replace a readable set");
    free(k.now.v);
    k.now = (lines){&current, 1, 1};
    check(!kept_write(&k, 1, why, sizeof why) && bridge_count(d, "set.kept", k.until) == 1,
          "a completed write replaces the set and drops the earlier row");
    assert(!plat_rename_at(d, "set.kept", d, ".published.kept"));
}

static void set_file(plat_dir d, const char *name, time_t until, const char *key) {
    char buf[256];
    int n = snprintf(buf, sizeof buf, "%s%lld\n%s %s\n", HEAD, (long long)until, model, key);
    file(d, name, buf, (size_t)n);
}

static void cap(const char *path, plat_dir d) {
    time_t until = time(NULL) + 3600;
    char key[65];
    snprintf(key, sizeof key, "%064d", 1);
    set_file(d, "expired.kept", until - 7200, key);
    file(d, "expired.manifest", "x", 1);
    file(d, "bad.kept", "bad", 3);
    file(d, ".temporary.kept", "bad", 3);
    for (int i = 1; i <= 4; i++) {
        char name[16];
        snprintf(name, sizeof name, "s%d.kept", i);
        set_file(d, name, until, key);
    }
    fail_expiry = 1;
    kept *k = kept_open(path, "s4", until, plat_dir_cwd(), why, sizeof why);
    plat_stat st;
    check(!k && strstr(why, "5 playlists") && !plat_stat_at(d, "expired.manifest", &st),
          "an expired set stuck on disk still occupies a bridge slot and retains its manifest");
    kept_close(k);
    fail_expiry = 0;
    k = kept_open(path, "s4", until, plat_dir_cwd(), why, sizeof why);
    check(k && plat_stat_at(d, "expired.kept", &st) == ENOENT &&
          plat_stat_at(d, "expired.manifest", &st) == ENOENT,
          "expiry frees a slot, dot files are ignored, and an existing set can be renewed");
    kept_close(k);
    k = kept_open(path, "new", until, plat_dir_cwd(), why, sizeof why);
    check(!k && strstr(why, "5 playlists") && !plat_stat_at(d, "bad.kept", &st),
          "an unreadable fifth set occupies a bridge slot");
    kept_close(k);
    assert(!plat_unlink_at(d, "bad.kept"));
    for (int i = 1; i <= 4; i++) {
        char name[16];
        snprintf(name, sizeof name, "s%d.kept", i);
        assert(!plat_unlink_at(d, name));
    }
}

static void room(const char *path, plat_dir d) {
    char key[5][65];
    plat_dir prefix;
    time_t until = time(NULL) + 3600;
    char data[4096] = {1};
    long long size[5];
    for (int i = 0; i < 5; i++) {
        snprintf(key[i], sizeof key[i], "%064d", i + 1);
        assert(!kept_prefix_dir(d, model, key[i], 1, &prefix));
        for (int f = 0; f < ENTRIES_FILES; f++) {
            char name[80];
            entries_file(name, sizeof name, key[i], f);
            file(prefix, name, data, sizeof data);
            struct timespec time[2] = {{100 + i, 0}, {100 + i, 0}};
            assert(!utimensat(prefix.fd, name, time, AT_SYMLINK_NOFOLLOW));
        }
        plat_dir_close(prefix);
        size[i] = kept_entry_bytes(d, model, key[i]);
        assert(size[i] >= (long long)(sizeof data * ENTRIES_FILES));
    }
    set_file(d, "other.kept", until, key[0]);
    set_file(d, "duplicate.kept", until, key[0]);
    set_file(d, "own.kept", until, key[1]);
    kept *k = kept_open(path, "own", until, d, why, sizeof why);
    assert(k);
    check(kept_bytes(k) == size[0], "other sets count each entry once, including allocated companions");
    kept_hold(k, model, key[1], 1);
    kept_release(k);
    kept_add(k, model, key[1], size[1]);
    check(kept_has(k, key[1]) && kept_bytes(k) == size[0] + size[1],
          "holding and then keeping an earlier entry charges the share once");
    long long used = size[0] + size[1] + size[2] + size[3] + size[4], target = used - size[2];
    long long removed = make_room(d, k, &used, target);
    check(removed == 1 && used == target && kept_entry_bytes(d, model, key[2]) < 0 &&
          kept_entry_bytes(d, model, key[0]) >= 0 && kept_entry_bytes(d, model, key[1]) >= 0 &&
          kept_entry_bytes(d, model, key[3]) >= 0 && kept_entry_bytes(d, model, key[4]) >= 0,
          "room removes the oldest unprotected entry and charges its allocated companions");
    kept_close(k);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    plat_dir d;
    assert(!plat_dir_open(argv[1], &d));
    writes(argv[1], d);
    cap(argv[1], d);
    room(argv[1], d);
    plat_dir_close(d);
    printf("kept-host: %d passed, %d failed\n", passed, failed);
    return !!failed;
}
