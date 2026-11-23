/* The resident watcher's WATCH_KEPT adapter on a real scratch folder. */
#include "check.h"
#include "../fs_write.h"
#include "../watch_kept.h"

#define RBSP_CORE_PLAT_SHIM_H
#include "internal.h"

#include <sys/stat.h>
#include <unistd.h>

int checks, failures;

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f || fputs(text, f) == EOF || fclose(f)) abort();
}

static void date_text(void *ctx, double t, char *out, size_t n) {
    (void)ctx;
    snprintf(out, n, "%.0f", t);
}

static int session_id = 123;

static void session(char *out, size_t n) {
    snprintf(out, n, "%d-0000000000000001", session_id);
}

static int writes, blocked;

static int write_support(const watch_env *e, const char *name, const char *s, size_t n) {
    if (!strcmp(name, "kept.checked")) writes++;
    return win_write_atomic(e->support, name, s, n);
}

static void log_line(void *ctx, const char *line) {
    (void)ctx;
    if (strstr(line, "prepared playlists not checked:")) blocked++;
}

static void expect_sets(watch_kept *k, const char *expected, const char *case_name) {
    arena a = {0};
    core_facts out = {.a = &a};
    const char *args[] = {"sets"};
    int r = watch_kept_step(k, CORE_STEP_WATCH_KEPT, args, 1, &out);
    CHECK(r == 0 && out.set[FACT_KEPT_SETS] && !strcmp(out.s[FACT_KEPT_SETS], expected),
          "%s: WATCH_KEPT sets wanted <%s>, status %d got <%s>", case_name, expected, r,
          out.set[FACT_KEPT_SETS] ? out.s[FACT_KEPT_SETS] : "(unset)");
    arena_free(&a);
}

static void expect_session(watch_kept *k, int wants_facts, const char *case_name) {
    int prior_writes = writes, prior_blocked = blocked;
    session_id++;
    watch_kept_check(k);
    CHECK(writes == prior_writes + 1 && blocked == prior_blocked + wants_facts,
          "%s: record the fresh session and %s the Prepare prerequisites", case_name, wants_facts ? "read" : "skip");
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char kept[1024], set[1100], manifest[1100], fixture[512], expected[128];
    snprintf(kept, sizeof kept, "%s/kept", argv[1]);
    snprintf(set, sizeof set, "%s/42.kept", kept);
    snprintf(manifest, sizeof manifest, "%s/42.manifest", kept);
    watch_kept k = {.kept = kept, .date = date_text};

    expect_sets(&k, "", "missing folder");
    if (mkdir(kept, 0700)) abort();
    write_file(manifest, "file A\nfile B\n");
    expect_sets(&k, "", "no kept sets (a manifest alone)");

    long long until = (long long)(plat_now_wall_ns() / 1000000000) + 86400;
    const char *entry = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    snprintf(fixture, sizeof fixture, "rbstemsplus-kept 1\nuntil %lld\n%s %s\n", until, entry, entry);
    write_file(set, fixture);
    snprintf(expected, sizeof expected, "42\t%lld\t2", until);
    expect_sets(&k, expected, "current set");

    /* The actual core reaches the prerequisites, then remembers the session across restarts. */
    watch_env env = {.support = argv[1], .log = log_line};
    k.env = &env;
    k.facts = (host_facts_env){.support = env.support, .model = "/unused/model.onnx", .prepare = 1};
    k.cache = "/unused/Cache";
    k.session = session;
    k.write = write_support;
    watch_kept_check(&k);
    k.checked[0] = 0;
    watch_kept_check(&k);
    CHECK(writes == 1 && blocked == 1, "current set reaches the core's prerequisites once per session (%d writes, %d logs)", writes, blocked);
    char checked[1100];
    snprintf(checked, sizeof checked, "%s/kept.checked", env.support);
    if (writes && unlink(checked)) abort();

    write_file(set, "rbstemsplus-kept 1\nuntil 1\n");
    expect_sets(&k, "", "expired set");
    expect_session(&k, 0, "expired sets only");

    write_file(set, "rbstemsplus-kept 1\nuntil never\n");
    expect_sets(&k, "42\t\t2", "malformed set (unknown date)");
    expect_session(&k, 1, "unknown date");
    if (unlink(set) || unlink(manifest)) abort();
    expect_session(&k, 0, "empty kept folder");
    if (rmdir(kept)) abort();
    expect_session(&k, 0, "missing kept folder");
    if (unlink(checked)) abort();

    printf("watch-kept: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
