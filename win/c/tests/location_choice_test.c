/* The location choice's steps (native_steps.c, plan N3) on a scratch tree: LOCATION_PROBE's
 * refusals in the Mac's order, DISCOVER's candidates, CREATE, WRITE (config.ini read back, the
 * hint, the pending flag), WRITABLE, SIZE, THIS_MAC_DELETE's parts, Settings' Clear for a chosen
 * folder and LocationStatus.code. The Windows-only answers (the volume's facts and GUID root, the
 * drive's name, the id) are fakes: a scratch folder stands for the volume, its path for the root. */
#include "check.h"

#include "../native_steps.h"
#include "../native_location.h"
#include "../volumes.h"
#include "marker.h"
#include "path_rules.h"
#ifndef RBSP_CORE_PLAT_SHIM_H
#  define RBSP_CORE_PLAT_SHIM_H
#endif
#include "internal.h"   /* LOC_*, PROBE_* */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char vol[PATH_MAX];   /* the fake volume's root */
static int fake_refusal, fake_errno, id_fails, indexed, remembered;
static int readback_faults;
static const char *fake_real;   /* NULL: the folder itself */
static const char *fake_mount;  /* NULL: vol */

static int f_volume(void *ctx, const char *folder, int *refusal, char *root, size_t rn, char *real, size_t n) {
    (void)ctx;
    if (fake_errno) return fake_errno;
    *refusal = fake_refusal;
    snprintf(root, rn, "%s", fake_mount ? fake_mount : vol);
    snprintf(real, n, "%s", fake_real ? fake_real : folder);
    return 0;
}
static void f_drive(void *ctx, const char *dir, int remember, char *out, size_t n) {
    (void)ctx;
    (void)dir;
    remembered += remember;
    snprintf(out, n, "STICK");
}
static int f_id(void *ctx, char out[40]) {
    (void)ctx;
    static int k;
    if (id_fails) return -1;
    snprintf(out, 40, "0000000%d-AAAA-4BBB-8CCC-DDDDDDDDDDDD", ++k % 10);
    return 0;
}
static void f_indexed(void *ctx, const char *dir) {
    (void)ctx;
    (void)dir;
    indexed++;
}
static const native_choice_ops ops = {NULL, f_volume, f_drive, f_id, f_indexed, NULL};

static int f_readback(int op, const char *name) {
    const char *base = strrchr(name, '/');
    if (op != NS_FAULT_OPEN || !base || strcmp(base + 1, "config.ini")) return 0;
    readback_faults++;
    return EIO;
}

static void lc_log(void *ctx, const char *line) {
    (void)ctx;
    (void)line;
}
/* plat_remove_tree_at's stand-in: name in dfd and its tree, never following a link */
static int rm_at(int dfd, const char *name) {
    struct stat st;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW)) return errno;
    if (!S_ISDIR(st.st_mode)) return unlinkat(dfd, name, 0) ? errno : 0;
    int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    DIR *dir = fd < 0 ? NULL : fdopendir(fd);
    if (!dir) {
        if (fd >= 0) close(fd);
        return errno;
    }
    for (struct dirent *x; (x = readdir(dir));)
        if (strcmp(x->d_name, ".") && strcmp(x->d_name, "..")) rm_at(dirfd(dir), x->d_name);
    closedir(dir);
    return unlinkat(dfd, name, AT_REMOVEDIR) ? errno : 0;
}
static int lc_rm(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    return rm_at(d.fd, name);
}
static int lc_rm_partial(void *ctx, plat_dir d, const char *name) {
    int *calls = ctx;
    return ++*calls == 2 ? EIO : lc_rm(NULL, d, name);
}
static int lc_rmdir(void *ctx, plat_dir d, const char *name, plat_id expect) {
    (void)ctx;
    plat_stat st;
    if (plat_stat_at(d, name, &st)) return ENOENT;
    if (!plat_id_same(st.id, expect)) return ESTALE;
    return unlinkat(d.fd, name, AT_REMOVEDIR) ? errno : 0;
}

static void mk(const char *a, const char *b, char *out, size_t n) {
    snprintf(out, n, "%s/%s", a, b);
    mkdir(out, 0755);
}
static void put(const char *dir, const char *name, const char *text) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) abort();
    fputs(text, f);
    fclose(f);
}
static char *slurp(const char *dir, const char *name) {
    static char b[8192];
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    b[0] = 0;
    if (!f) return NULL;
    size_t k = fread(b, 1, sizeof b - 1, f);
    b[k] = 0;
    fclose(f);
    return b;
}
static int there(const char *dir, const char *name) {
    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return !lstat(p, &st);
}
#define ID1 "11111111-2222-4333-8444-555555555555"
#define MARK(id) "id=" id "\nformat=" MARKER_FORMAT "\n"

static void t_probe(const char *root) {
    char a[PATH_MAX], b[PATH_MAX], c[PATH_MAX], d[PATH_MAX];
    mk(vol, "music", a, sizeof a);
    mk(a, "sets", b, sizeof b);
    native_choice_probe p;
    CHECK(native_choice_probe_folder(&ops, "", &p) != 0, "no folder");
    static const struct { int refusal; int64_t code; } map[] = {{VOL_REFUSE_UNREADABLE, 9}, {VOL_REFUSE_NETWORK, 2}, {VOL_REFUSE_INTERNAL, 1},
                                                              {VOL_REFUSE_FILE_SYSTEM, 7}, {VOL_REFUSE_READ_ONLY, 3}, {VOL_OK, 0}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        fake_refusal = map[i].refusal;
        CHECK(!native_choice_probe_folder(&ops, b, &p) && p.code == map[i].code, "refusal %d: code %lld", map[i].refusal, (long long)p.code);
    }
    CHECK(!strcmp(p.real, b) && !strcmp(p.mount, vol) && !strcmp(p.drive, "STICK"), "real %s mount %s drive %s", p.real, p.mount, p.drive);
    fake_errno = EIO;
    CHECK(!native_choice_probe_folder(&ops, b, &p) && p.code == 9 && !p.real[0], "unreadable: %lld", (long long)p.code);
    fake_errno = 0;
    /* the volume's root itself */
    CHECK(!native_choice_probe_folder(&ops, vol, &p) && p.code == 0, "the root: %lld", (long long)p.code);
    /* a link on the way */
    snprintf(c, sizeof c, "%s/linked", vol);
    symlink(a, c);
    snprintf(d, sizeof d, "%s/sets", c);
    CHECK(!native_choice_probe_folder(&ops, d, &p) && p.code == 6, "a link: %lld", (long long)p.code);
    /* a real path not below the volume's root */
    fake_real = root;
    CHECK(!native_choice_probe_folder(&ops, b, &p) && p.code == 6, "outside the root: %lld", (long long)p.code);
    fake_real = NULL;
    char sibling[PATH_MAX];
    mk(root, "vol-sibling", sibling, sizeof sibling);
    CHECK(!native_choice_probe_folder(&ops, sibling, &p) && p.code == 6, "sibling with the mount's prefix: %lld", (long long)p.code);
    /* inside our folder: a component above named so (any case), a marker above, a marker at the root */
    mk(vol, "rb stems plus cache.noindex", c, sizeof c);
    mk(c, "in", d, sizeof d);
    CHECK(!native_choice_probe_folder(&ops, d, &p) && p.code == 5, "inside our folder: %lld", (long long)p.code);
    put(a, MARKER_NAME, "x");
    CHECK(!native_choice_probe_folder(&ops, b, &p) && p.code == 5, "a marker above: %lld", (long long)p.code);
    unlink(c);
    snprintf(c, sizeof c, "%s/%s", a, MARKER_NAME);
    unlink(c);
    put(vol, MARKER_NAME, "x");
    CHECK(!native_choice_probe_folder(&ops, b, &p) && p.code == 5, "a marker at the root: %lld", (long long)p.code);
    snprintf(c, sizeof c, "%s/%s", vol, MARKER_NAME);
    unlink(c);
    /* our folder itself, its own marker: may be used */
    mk(b, PATHS_CACHE_FOLDER, c, sizeof c);
    put(c, MARKER_NAME, MARK(ID1));
    CHECK(!native_choice_probe_folder(&ops, c, &p) && p.code == 0, "our folder: %lld", (long long)p.code);
    /* a path over cache_dir's limit */
    char longp[PATH_MAX];
    snprintf(longp, sizeof longp, "%s", b);
    while (strlen(longp) < PATHS_LOCATION_MAX - 20) {
        size_t k = strlen(longp);
        snprintf(longp + k, sizeof longp - k, "/%s", "abcdefghijklmnopqrstuvwxyz");
        mkdir(longp, 0755);
    }
    CHECK(!native_choice_probe_folder(&ops, longp, &p) && p.code == 8, "too long: %lld", (long long)p.code);
}

static void t_probe_trailing_separator(const char *root) {
    char mount[PATH_MAX], parent[PATH_MAX], inside[PATH_MAX], marked[PATH_MAX], sibling[PATH_MAX], plain[PATH_MAX];
    mk(root, "vol-trailing", mount, sizeof mount);
    mk(mount, PATHS_CACHE_FOLDER, parent, sizeof parent);
    mk(parent, "inside", inside, sizeof inside);
    mk(mount, "marked", parent, sizeof parent);
    put(parent, MARKER_NAME, "x");
    mk(parent, "inside", marked, sizeof marked);
    mk(root, "vol-trailing-sibling", sibling, sizeof sibling);
    mk(mount, "plain", plain, sizeof plain);
    size_t n = strlen(mount);
    if (n + 1 >= sizeof mount) abort();
    memcpy(mount + n, "/", 2);
    fake_mount = mount;
    const struct { const char *folder; int64_t code; } rows[] = {
        {inside, 5}, {marked, 5}, {sibling, 6}, {mount, 0}, {plain, 0},
    };
    native_choice_probe p;
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        CHECK(!native_choice_probe_folder(&ops, rows[i].folder, &p) && p.code == rows[i].code,
              "trailing separator: %s: expected %lld, got %lld", rows[i].folder, (long long)rows[i].code, (long long)p.code);
    fake_mount = NULL;
}

static void t_probe_host_roots(void) {
    native_choice_probe p;
    /* devfs supplies a real mount boundary even when the sandbox can't attach an image. */
    fake_mount = "/dev";
    CHECK(!native_choice_probe_folder(&ops, "/dev", &p) && p.code == 0, "a mounted root: %lld", (long long)p.code);
    fake_mount = fake_real = "/private/var";
    CHECK(!native_choice_probe_folder(&ops, "/var", &p) && p.code == 0, "the host's /var alias: %lld", (long long)p.code);
    fake_mount = fake_real = NULL;
}

static void t_discover_create_write(const char *root) {
    char sup[PATH_MAX], a[PATH_MAX], ours[PATH_MAX], other[PATH_MAX];
    mk(root, "support", sup, sizeof sup);
    mk(vol, "dj", a, sizeof a);
    native_steps_env e;
    memset(&e, 0, sizeof e);
    e.support = sup;
    e.log = lc_log;
    e.remove_tree_at = lc_rm;
    e.remove_empty_dir_at = lc_rmdir;
    native_choice_found f;
    CHECK(native_choice_discover(sup, "", vol, &f) != 0, "no real path");
    CHECK(!native_choice_discover(sup, a, vol, &f) && f.code == 0 && !f.current[0], "nothing there: %lld", (long long)f.code);
    /* a folder of our name without a marker: not ours */
    mk(a, PATHS_CACHE_FOLDER, ours, sizeof ours);
    CHECK(!native_choice_discover(sup, a, vol, &f) && f.code == 1 && !strcmp(f.found, ours), "not ours: %lld", (long long)f.code);
    put(ours, MARKER_NAME, "id=" ID1 "\nformat=other\n");
    CHECK(!native_choice_discover(sup, a, vol, &f) && f.code == 1, "a wrong marker: %lld", (long long)f.code);
    snprintf(other, sizeof other, "%s/%s", ours, MARKER_NAME);
    unlink(other);
    rmdir(ours);
    /* CREATE: our folder made with a marker, the attribute asked */
    char dir[NS_PATH_MAX], id[40], err[128], identity[NS_CACHE_IDENTITY_MAX];
    indexed = 0;
    CHECK(native_choice_create(&ops, &e, a, dir, id, err, identity) == 0 && !strcmp(dir, ours) && indexed == 1, "create: %s %s", dir, err);
    CHECK(!native_choice_discover(sup, a, vol, &f) && f.code == 2 && !strcmp(f.found, ours) && !strcmp(f.id, id), "found: %lld %s", (long long)f.code, f.id);
    CHECK(!native_choice_discover(sup, ours, vol, &f) && f.code == 2 && !strcmp(f.found, ours), "found by itself: %lld", (long long)f.code);
    CHECK(native_choice_create(&ops, &e, a, dir, id, err, identity) == 1, "a folder appeared meanwhile");
    /* No UUID was obtained, so the folder is never created. */
    char b[PATH_MAX], bo[PATH_MAX];
    mk(vol, "other", b, sizeof b);
    snprintf(bo, sizeof bo, "%s/%s", b, PATHS_CACHE_FOLDER);
    id_fails = 1;
    CHECK(native_choice_create(&ops, &e, b, dir, id, err, identity) == 3 && !there(b, PATHS_CACHE_FOLDER), "no id: %s", err);
    id_fails = 0;
    chmod(b, 0555);
    CHECK(native_choice_create(&ops, &e, b, dir, id, err, identity) == 2 && err[0], "mkdir refused: %s", err);
    chmod(b, 0755);
    /* WRITE: config.ini names it, read back; the hint with the id made; the label remembered */
    char made[NS_PATH_MAX], made_id[40], made_identity[NS_CACHE_IDENTITY_MAX];
    CHECK(native_choice_create(&ops, &e, b, made, made_id, err, made_identity) == 0, "create 2");
    put(sup, "config.ini", "# mine\nmax_gb = 2\n");
    remembered = 0;
    CHECK(native_choice_write(&ops, &e, made, made_id, vol, 1, made_identity) == 0 && remembered == 1, "write");
    char *cfg = slurp(sup, "config.ini");
    CHECK(cfg && strstr(cfg, "# mine") && strstr(cfg, "max_gb = 2") && strstr(cfg, made_id) && strstr(cfg, "cache_dir"), "config.ini: %s", cfg ? cfg : "");
    char *hint = slurp(sup, NS_LOCATION_HINT);
    char want[PATH_MAX];
    snprintf(want, sizeof want, "mount=%s\nrelative=other/%s\nown=%s\n", vol, PATHS_CACHE_FOLDER, made_id);
    CHECK(hint && !strcmp(hint, want), "hint: %s", hint ? hint : "");
    /* DISCOVER now: current is it; a folder chosen elsewhere on the drive finds it through the hint */
    char c[PATH_MAX];
    mk(vol, "elsewhere", c, sizeof c);
    CHECK(!native_choice_discover(sup, c, vol, &f) && f.code == 2 && !strcmp(f.found, made) && !strcmp(f.current, made)
              && !strcmp(f.current_id, made_id),
          "through the hint: %lld %s", (long long)f.code, f.found);
    /* the hint's path through a link: not taken */
    char moved[PATH_MAX];
    snprintf(moved, sizeof moved, "%s/other-real", vol);
    rename(b, moved);
    symlink(moved, b);
    CHECK(!native_choice_discover(sup, c, vol, &f) && f.code == 0, "the hint through a link: %lld", (long long)f.code);
    unlink(b);
    rename(moved, b);
    /* The dot component reaches a marked folder if accepted. */
    put(sup, NS_LOCATION_HINT, "relative=other/./" PATHS_CACHE_FOLDER "\n");
    CHECK(!native_choice_discover(sup, c, vol, &f) && f.code == 0 && !f.found[0], "invalid relative hint: %lld", (long long)f.code);
    /* The absolute config.ini path is relative to the host filesystem root. */
    CHECK(!native_support_remove(sup, NS_LOCATION_HINT) && !native_choice_discover(sup, c, "/", &f) && f.code == 2
              && !strcmp(f.found, made) && !strcmp(f.id, made_id),
          "config.ini fallback: %lld %s", (long long)f.code, f.found);
    int bad_path = native_choice_write(&ops, &e, c, made_id, vol, 0, made_identity);
    cfg = slurp(sup, "config.ini");
    CHECK(bad_path == 2 && cfg && strstr(cfg, made) && strstr(cfg, made_id), "invalid WRITE path left config.ini intact");

    char d[PATH_MAX], dd[NS_PATH_MAX], did[40];
    mk(vol, "third", d, sizeof d);
    CHECK(native_choice_create(&ops, &e, d, dd, did, err, identity) == 0, "create 3");
    /* an unreadable config.ini: nothing written */
    char e2[PATH_MAX];
    snprintf(e2, sizeof e2, "%s/config.ini", sup);
    FILE *x = fopen(e2, "ab");
    fputc(0, x);
    fclose(x);
    CHECK(native_choice_write(&ops, &e, made, made_id, vol, 0, made_identity) == 1, "an unreadable config.ini");
    char marker_want[96];
    snprintf(marker_want, sizeof marker_want, "id=%s\nformat=%s\n", made_id, MARKER_FORMAT);
    char *marker = slurp(made, MARKER_NAME);
    CHECK(there(b, PATHS_CACHE_FOLDER) && marker && !strcmp(marker, marker_want), "failed WRITE kept the existing folder and its marker");
    put(dd, MARKER_NAME, MARK(ID1));
    CHECK(native_choice_write(&ops, &e, dd, did, vol, 1, identity) == 2, "rewrite failed with a different marker");
    marker = slurp(dd, MARKER_NAME);
    CHECK(marker && !strcmp(marker, MARK(ID1)), "rollback kept the folder and its replacement marker");
    put(sup, "config.ini", "max_gb = 2\ncache_dir = \"/x\"\n");
    /* the replace went through, then reading config.ini failed */
    char fourth[PATH_MAX];
    mk(vol, "fourth", fourth, sizeof fourth);
    CHECK(native_choice_create(&ops, &e, fourth, dir, id, err, identity) == 0, "create 4");
    readback_faults = 0;
    native_steps_test_fault = f_readback;
    CHECK(native_choice_write(&ops, &e, dir, id, vol, 1, identity) == 1 && readback_faults == 1, "WRITE failed at read-back");
    native_steps_test_fault = NULL;
    cfg = slurp(sup, "config.ini");
    CHECK(cfg && strstr(cfg, dir) && strstr(cfg, id), "config.ini replaced before read-back failed");
    CHECK(!native_choice_discover(sup, fourth, vol, &f) && f.code == 2 && !strcmp(f.found, dir) && !strcmp(f.id, id),
          "read-back failure kept the folder and its marker");
    /* This PC: both keys go, the pending flag goes */
    put(sup, NS_PENDING_DELETION, "");
    readback_faults = 0;
    native_steps_test_fault = f_readback;
    int this_pc = native_choice_write(&ops, &e, "", "", "", 0, NULL);
    native_steps_test_fault = NULL;
    cfg = slurp(sup, "config.ini");
    CHECK(this_pc == 1 && readback_faults == 1 && cfg && !strstr(cfg, "cache_dir") && !strstr(cfg, "cache_id")
              && there(sup, NS_PENDING_DELETION),
          "This PC failed at read-back with the pending flag still set");
    CHECK(native_choice_write(&ops, &e, "", "", "", 0, NULL) == 0, "This PC");
    cfg = slurp(sup, "config.ini");
    CHECK(cfg && !strstr(cfg, "cache_dir") && !strstr(cfg, "cache_id") && strstr(cfg, "max_gb = 2"), "This PC's config.ini: %s", cfg ? cfg : "");
    CHECK(!there(sup, NS_PENDING_DELETION), "the pending flag removed");
}

static void t_writable_size(void) {
    char a[PATH_MAX], m[PATH_MAX], pre[PATH_MAX];
    mk(vol, "w", a, sizeof a);
    CHECK(native_choice_writable(a) == 1, "writable");
    mk(a, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", m, sizeof m);
    mk(m, "01", pre, sizeof pre);
    put(pre, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.flac", "stems");
    CHECK(native_choice_size(a) > 0, "size %lld", (long long)native_choice_size(a));
    CHECK(native_choice_size("") == 0, "no folder");
    chmod(m, 0555);
    CHECK(native_choice_writable(a) == 0, "a model folder read-only");
    chmod(m, 0755);
    chmod(a, 0555);
    CHECK(native_choice_writable(a) == 0, "read-only");
    chmod(a, 0755);
    CHECK(native_choice_writable("/no/such/folder") == 0, "missing");
}

static int f_replaced_at_create(int op, const char *path) {
    if (op != NS_FAULT_OPEN_CREATED) return 0;
    char moved[PATH_MAX];
    snprintf(moved, sizeof moved, "%s-moved", path);
    if (rename(path, moved) || mkdir(path, 0755)) abort();
    return EACCES;
}

static int marker_arrived_at_create(int op, const char *dir) {
    if (op == NS_FAULT_OPEN_CREATED) put(dir, MARKER_NAME, "preexisting contents");
    return 0;
}

static int fail_created_folder_probe(int op, const char *dir) {
    (void)dir;
    return op == NS_FAULT_OPEN_CREATED ? EACCES : 0;
}

static void t_rollback_preserves_replacements(const char *root) {
    char sup[PATH_MAX], parent[PATH_MAX], dir[NS_PATH_MAX], id[40], err[128], identity[NS_CACHE_IDENTITY_MAX], moved[PATH_MAX], path[PATH_MAX];
    mk(root, "rollback-support", sup, sizeof sup);
    mk(sup, "config.ini", path, sizeof path);   /* config rewrite fails */
    native_steps_env e = {.support = sup, .log = lc_log, .remove_empty_dir_at = lc_rmdir};

    mk(vol, "rollback-create", parent, sizeof parent);
    native_steps_test_fault = f_replaced_at_create;
    int r = native_choice_create(&ops, &e, parent, dir, id, err, identity);
    native_steps_test_fault = NULL;
    CHECK(r != 0 && there(parent, PATHS_CACHE_FOLDER), "t_rollback_preserves_replacements: failed create keeps the replacement");

    mk(vol, "marker-arrival", parent, sizeof parent);
    native_steps_test_fault = marker_arrived_at_create;
    r = native_choice_create(&ops, &e, parent, dir, id, err, identity);
    native_steps_test_fault = NULL;
    snprintf(path, sizeof path, "%s/%s", parent, PATHS_CACHE_FOLDER);
    char *arrived = slurp(path, MARKER_NAME);
    CHECK(r != 0 && arrived && !strcmp(arrived, "preexisting contents"),
          "D: CREATE cannot overwrite a marker that appeared inside the new folder");

    mk(vol, "rollback-write", parent, sizeof parent);
    if (native_choice_create(&ops, &e, parent, dir, id, err, identity)) abort();
    snprintf(moved, sizeof moved, "%s-moved", dir);
    if (rename(dir, moved) || mkdir(dir, 0755)) abort();
    CHECK(native_choice_write(&ops, &e, dir, id, vol, 1, identity) == 2 && there(parent, PATHS_CACHE_FOLDER) && there(moved, MARKER_NAME),
          "t_rollback_preserves_replacements: failed write keeps an unmarked replacement");
    char copied[96];
    snprintf(copied, sizeof copied, "id=%s\nformat=%s\n", id, MARKER_FORMAT);
    put(dir, MARKER_NAME, copied);
    CHECK(native_choice_write(&ops, &e, dir, id, vol, 1, identity) != 0 && there(dir, MARKER_NAME) && there(moved, MARKER_NAME),
          "A: failed write leaves a replacement with a copied marker untouched");

    char ancestor[PATH_MAX], target[PATH_MAX], target_parent[PATH_MAX], target_cache[PATH_MAX], marker[96];
    mk(vol, "rollback-ancestor", ancestor, sizeof ancestor);
    mk(ancestor, "parent", parent, sizeof parent);
    if (native_choice_create(&ops, &e, parent, dir, id, err, identity)) abort();
    mk(vol, "rollback-target", target, sizeof target);
    mk(target, "parent", target_parent, sizeof target_parent);
    mk(target_parent, PATHS_CACHE_FOLDER, target_cache, sizeof target_cache);
    snprintf(marker, sizeof marker, "id=%s\nformat=%s\n", id, MARKER_FORMAT);
    put(target_cache, MARKER_NAME, marker);
    snprintf(moved, sizeof moved, "%s-moved", ancestor);
    if (rename(ancestor, moved) || symlink(target, ancestor)) abort();
    CHECK(native_choice_write(&ops, &e, dir, id, vol, 1, identity) == 2 && there(target_cache, MARKER_NAME),
          "t_rollback_preserves_replacements: failed write leaves a marked folder beyond a replaced ancestor");

    mk(vol, "rollback-intact", parent, sizeof parent);
    if (native_choice_create(&ops, &e, parent, dir, id, err, identity)) abort();
    CHECK(native_choice_write(&ops, &e, dir, id, vol, 1, identity) == 1 && !there(parent, PATHS_CACHE_FOLDER),
          "failed write removes the intact empty cache it created");
}

static void t_support_tree_overlap(const char *root) {
    char physical[PATH_MAX], redirected[PATH_MAX], support[PATH_MAX], support_alias[PATH_MAX];
    char inside[PATH_MAX], outside[PATH_MAX], cache[PATH_MAX], config[2 * PATH_MAX], dir[NS_PATH_MAX], id[40], err[128], identity[NS_CACHE_IDENTITY_MAX];
    mk(vol, "local-app-data", physical, sizeof physical);
    mk(physical, "rbstemsplus", support, sizeof support);
    mk(support, "payload", inside, sizeof inside);
    mk(support, "payload/1.2.3", inside, sizeof inside);
    mk(vol, "outside-support", outside, sizeof outside);
    snprintf(redirected, sizeof redirected, "%s/local-app-data-alias", root);
    if (symlink(physical, redirected)) abort();
    snprintf(support_alias, sizeof support_alias, "%s/rbstemsplus", redirected);
    native_choice_ops scoped = ops;
    scoped.support = support_alias;
    native_steps_env e = {.support = support_alias, .log = lc_log, .remove_tree_at = lc_rm, .remove_empty_dir_at = lc_rmdir};
    native_choice_probe p;
    CHECK(!native_choice_probe_folder(&scoped, support, &p) && p.code && !native_choice_probe_folder(&scoped, inside, &p) && p.code,
          "t_support_tree_overlap: picker refuses the resolved support tree");
    int made = native_choice_create(&scoped, &e, inside, dir, id, err, identity);
    CHECK(made == 2 && !there(inside, PATHS_CACHE_FOLDER), "t_support_tree_overlap: create refuses support");
    mk(inside, PATHS_CACHE_FOLDER, cache, sizeof cache);
    put(cache, MARKER_NAME, MARK(ID1));
    put(cache, "personal-file", "keep");
    put(support, NS_LOCATION_HINT, "relative=local-app-data/rbstemsplus/payload/1.2.3/" PATHS_CACHE_FOLDER "\n");
    native_choice_found f;
    CHECK(!native_choice_discover(support_alias, outside, vol, &f) && f.code == 0,
          "t_support_tree_overlap: discovery ignores a marked support-tree candidate from the hint");
    CHECK(native_choice_write(&scoped, &e, cache, ID1, vol, 0, identity) == 2 && !there(support, "config.ini"),
          "t_support_tree_overlap: write refuses support");
    snprintf(config, sizeof config, "cache_dir = \"%s\"\ncache_id = %s\n", cache, ID1);
    put(support, "config.ini", config);
    if (native_support_remove(cache, MARKER_NAME)) abort();
    CHECK(native_remove_own_folders(&e, support_alias, 0) == 1 && there(cache, "personal-file"),
          "t_support_tree_overlap: uninstall reports kept files and preserves the unmarked configured cache");
    put(support, "config.ini", config);
    CHECK(!native_choice_write(&ops, &e, "", "", "", 0, NULL), "switch to This PC");
    CHECK(native_remove_own_folders(&e, support_alias, 0) == 1 && there(cache, "personal-file") &&
          !there(support, "config.ini"), "B: without location keys, uninstall removes config and preserves the legacy cache");
    if (lc_rm(NULL, plat_dir_cwd(), cache)) abort();
    put(support, "config.ini", config);
    CHECK(!native_remove_own_folders(&e, support_alias, 0) && !there(physical, "rbstemsplus"),
          "B: uninstall finishes when the separately removed legacy cache is already gone");
}

static void t_commit_revalidates_destination(const char *root) {
    const char *model = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *original = "# original settings\nmax_gb = 2\n";
    char sup[PATH_MAX], parent[PATH_MAX], cache[PATH_MAX], moved[PATH_MAX], model_dir[PATH_MAX], cfg[2 * PATH_MAX];
    mk(root, "commit-support", sup, sizeof sup);
    mk(vol, "commit-parent", parent, sizeof parent);
    mk(parent, PATHS_CACHE_FOLDER, cache, sizeof cache);
    native_steps_env e = {.support = sup, .log = lc_log, .remove_empty_dir_at = lc_rmdir};
    put(cache, MARKER_NAME, MARK(ID1));
    put(sup, "config.ini", original);
    native_choice_found found;
    if (native_choice_discover(sup, parent, vol, &found) || found.code != 2) abort();
    snprintf(moved, sizeof moved, "%s-moved", cache);
    if (rename(cache, moved) || mkdir(cache, 0755)) abort();
    put(cache, MARKER_NAME, MARK(ID1));
    CHECK(native_choice_write(&ops, &e, cache, ID1, vol, 0, found.identity) != 0 && !strcmp(slurp(sup, "config.ini"), original),
          "D: a rediscovered cache replaced with a copied marker cannot change settings");
    if (lc_rm(NULL, plat_dir_cwd(), cache) || rename(moved, cache)) abort();

    put(sup, "config.ini", original);
    fake_refusal = VOL_REFUSE_INTERNAL;
    CHECK(native_choice_write(&ops, &e, cache, ID1, vol, 0, found.identity) != 0 && !strcmp(slurp(sup, "config.ini"), original),
          "D: a volume that became internal cannot be committed");
    char create[PATH_MAX], dir[NS_PATH_MAX], id[40], err[128], identity[NS_CACHE_IDENTITY_MAX];
    mk(vol, "commit-ineligible", create, sizeof create);
    CHECK(native_choice_create(&ops, &e, create, dir, id, err, identity) != 0 && !there(create, PATHS_CACHE_FOLDER),
          "D: a volume that became internal cannot get a new cache");
    fake_refusal = VOL_OK;

    put(sup, "config.ini", original);
    snprintf(moved, sizeof moved, "%s-moved", cache);
    if (rename(cache, moved)) abort();
    int r = native_choice_write(&ops, &e, cache, ID1, vol, 0, found.identity);
    CHECK(r == 2 && !strcmp(slurp(sup, "config.ini"), original),
          "t_commit_revalidates_destination: missing destination cannot change settings");

    if (rename(moved, cache)) abort();
    put(cache, MARKER_NAME, MARK("22222222-3333-4444-8555-666666666666"));
    put(sup, "config.ini", original);
    r = native_choice_write(&ops, &e, cache, ID1, vol, 0, found.identity);
    CHECK(r == 2 && !strcmp(slurp(sup, "config.ini"), original),
          "t_commit_revalidates_destination: a different marker cannot change settings");

    put(cache, MARKER_NAME, MARK(ID1));
    mk(cache, model, model_dir, sizeof model_dir);
    snprintf(cfg, sizeof cfg, "cache_dir = \"%s/old-parent/%s\"\ncache_id = %s\n", vol, PATHS_CACHE_FOLDER, ID1);
    put(sup, "config.ini", cfg);
    if (chmod(model_dir, 0555)) abort();
    r = native_choice_write(&ops, &e, cache, ID1, vol, 0, found.identity);
    CHECK(r == 2 && !strcmp(slurp(sup, "config.ini"), cfg),
          "t_commit_revalidates_destination: a rediscovered matching cache with read-only models cannot change settings");
    if (chmod(model_dir, 0755)) abort();
}

static void t_remove_deep(void) {
    char parent[PATH_MAX], dir[PATH_MAX], name[256], err[128];
    snprintf(parent, sizeof parent, "%s", vol);
    for (int i = 0; i < 69; i++) {
        mk(parent, "d", dir, sizeof dir);
        snprintf(parent, sizeof parent, "%s", dir);
    }
    mk(parent, PATHS_CACHE_FOLDER, dir, sizeof dir);
    put(dir, MARKER_NAME, MARK(ID1));
    native_steps_env e = {.log = lc_log, .remove_tree_at = lc_rm, .remove_empty_dir_at = lc_rmdir};
    CHECK(native_remove_location_under(&e, vol, dir + strlen(vol) + 1, ID1, 1, name, err, NULL) == 0 && !there(parent, PATHS_CACHE_FOLDER),
          "removed a 70-component location: %s %s", name, err);
}

static void t_this_pc(const char *root) {
    const char *model = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char sup[PATH_MAX], cache[PATH_MAX], x[PATH_MAX], link[PATH_MAX], real[PATH_MAX];
    mk(root, "s2", sup, sizeof sup);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    native_steps_env e;
    memset(&e, 0, sizeof e);
    e.support = sup;
    e.log = lc_log;
    e.remove_tree_at = lc_rm;
    e.remove_empty_dir_at = lc_rmdir;
    char identity[NS_CACHE_IDENTITY_MAX], measured[NS_CACHE_IDENTITY_MAX];
    native_pc_cache_create(&e, cache);
    CHECK(!native_pc_cache_identity(&e, cache, identity), "made a marked local cache");
    mk(cache, model, x, sizeof x);
    mk(cache, "FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210", x, sizeof x);   /* upper case: not a model folder */
    put(cache, "memo", "keep");
    CHECK(!native_pc_cache_identity(&e, cache, measured) && !strcmp(identity, measured), "marked cache identity is stable");
    CHECK(native_this_pc_remove_stems(&e, cache, identity) == 0, "removed");
    CHECK(!there(cache, model)
              && there(cache, "FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210") && there(cache, "memo"),
          "only the model folders");
    mk(root, "s2-real", real, sizeof real);
    mk(real, "Cache", cache, sizeof cache);
    mk(cache, model, x, sizeof x);
    CHECK(native_pc_cache_identity(&e, cache, measured) && native_this_pc_remove_stems(&e, cache, identity) == 1 && there(cache, model),
          "Cache outside support: none listed or deleted");
    mk(root, "s2-with-link", sup, sizeof sup);
    snprintf(link, sizeof link, "%s/Cache", sup);
    CHECK(!native_this_pc_remove_stems(&e, link, identity), "no folder: none");
    /* a link at Cache: nothing through it */
    CHECK(!symlink(cache, link), "linked Cache fixture");
    CHECK(native_this_pc_remove_stems(&e, link, identity) == 1 && there(cache, model), "a link at Cache: none deleted, retry retained");
    CHECK(native_pc_cache_identity(&e, link, measured), "a link at Cache: none listed");
    /* a link at support: even a plain Cache in its target holds no models for this PC */
    snprintf(sup, sizeof sup, "%s/s2-linked", root);
    CHECK(!symlink(real, sup), "linked support fixture");
    snprintf(link, sizeof link, "%s/Cache", sup);
    CHECK(native_pc_cache_identity(&e, link, measured), "linked support: none listed");
    CHECK(native_this_pc_remove_stems(&e, link, identity) == 1, "linked support: refused and still pending");
    CHECK(there(cache, model), "linked support: target model folder survives");
    CHECK(!strcmp(native_location_code(LOC_CHOSEN, PROBE_AVAILABLE), "available") && !strcmp(native_location_code(LOC_THIS_MAC, -1), "thisMac")
              && !strcmp(native_location_code(LOC_CHOSEN, PROBE_NOT_CONNECTED), "notConnected")
              && !strcmp(native_location_code(LOC_BRIDGE_TOO_OLD, -1), "bridgeTooOld") && !strcmp(native_location_code(9, 0), "?"),
          "LocationStatus.code");
}

static void t_unmarked_this_pc(const char *root) {
    const char *model = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char sup[PATH_MAX], cache[PATH_MAX], moved[PATH_MAX], identity[NS_CACHE_IDENTITY_MAX];
    mk(root, "unmarked-support", sup, sizeof sup);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    snprintf(moved, sizeof moved, "%s/old-cache", sup);
    native_steps_env e = {.support = sup, .log = lc_log, .remove_tree_at = lc_rm, .remove_empty_dir_at = lc_rmdir};
    native_pc_cache_create(&e, cache);
    if (native_pc_cache_identity(&e, cache, identity) || rename(cache, moved) || mkdir(cache, 0755)) abort();
    put(cache, model, "personal");
    native_this_pc_remove_stems(&e, cache, identity);
    CHECK(there(cache, model), "C: switch and startup cleanup preserve an unmarked Cache");
    put(cache, "personal-file", "keep");
    int link;
    CHECK(native_clear_cache(&e, cache, identity, 0, &link) != 0 && there(cache, "personal-file"),
          "C: Clear preserves a plain replacement in the Cache slot");
    put(cache, NS_PC_MARKER, identity);
    CHECK(native_clear_cache(&e, cache, identity, 0, &link) != 0 && there(cache, "personal-file"),
          "C: Clear refuses a marker copied into another folder");
    if (lc_rm(NULL, plat_dir_cwd(), cache)) abort();
    native_pc_cache_create(&e, cache);
    put(cache, model, "new cache");
    native_this_pc_remove_stems(&e, cache, identity);
    CHECK(there(cache, model), "C: a retry never deletes models from a newly marked cache");
    CHECK(native_clear_cache(&e, cache, identity, 1, &link) != 0 && there(cache, model),
          "C: uninstall leaves a newly marked cache that replaced the confirmed one");
}

static void t_this_pc_partial_delete(const char *root) {
    const char *a = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *b = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
    char sup[PATH_MAX], cache[PATH_MAX], model[PATH_MAX];
    mk(root, "s-partial", sup, sizeof sup);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    int calls = 0;
    native_steps_env e = {.support = sup, .ctx = &calls, .log = lc_log, .remove_tree_at = lc_rm_partial};
    /* review N3R3: one delete went through and the next failed; retry at the next start */
    char identity[NS_CACHE_IDENTITY_MAX];
    native_pc_cache_create(&e, cache);
    if (native_pc_cache_identity(&e, cache, identity)) abort();
    mk(cache, a, model, sizeof model);
    mk(cache, b, model, sizeof model);
    CHECK(native_this_pc_remove_stems(&e, cache, identity) == 1 && calls == 2 && there(cache, a) != there(cache, b)
              && !strcmp(slurp(sup, NS_PENDING_DELETION), identity),
          "THIS_MAC_DELETE: a partial delete records the confirmed identity for retry");
}

static void t_location_now(void) {
    const char *dir = "E:\\Music\\" PATHS_CACHE_FOLDER;
    native_location l = {.kind = LOC_THIS_MAC, .has_in_use = 1, .in_use = "C:\\support\\Cache"};
    native_location_target o;
    int64_t size;
    /* review N3R3: with the gate off, even the This PC snapshot must not name a chosen folder as This PC */
    CHECK(!native_location_now_facts(0, &l, dir, ID1, "Remembered", l.in_use, &o, &size)
              && !strcmp(o.dir, dir) && !strcmp(o.drive, "Remembered") && !o.gate_id[0] && size == -1,
          "LOCATION_NOW: gate off with a chosen folder");
    CHECK(!native_location_now_facts(1, &l, NULL, "", "", "c:\\support\\cache", &o, &size)
              && !o.dir[0] && !o.drive[0] && !o.gate_id[0] && size == 4,
          "LOCATION_NOW: This PC with a matching memo");
}

static void t_settings_clear(void) {
    native_settings_in in;
    native_settings o;
    memset(&in, 0, sizeof in);
    in.location = 1;
    in.dir = "\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\X\\" PATHS_CACHE_FOLDER;
    in.id = ID1;
    in.drive = "Named";
    in.this_pc_folder = 1;
    native_settings_facts(&in, &o);
    CHECK(o.clear == 0, "no measure: no Clear");
    native_location snap = {.kind = LOC_CHOSEN, .probe = PROBE_AVAILABLE, .has_in_use = 1};
    native_location_identity((plat_id){1, {2, 3}}, snap.identity);
    in.snap = &snap;
    snprintf(snap.in_use, sizeof snap.in_use, "/Volumes/Y/" PATHS_CACHE_FOLDER);
    native_settings_facts(&in, &o);
    CHECK(o.clear == 0, "available for another folder: no Clear (review N3R3)");
    snprintf(snap.in_use, sizeof snap.in_use, "%s", in.dir);
    snap.has_status_drive = 1;
    snprintf(snap.status_drive, sizeof snap.status_drive, "Measured");
    native_settings_facts(&in, &o);
    CHECK(o.clear == 3 && !strcmp(o.clear_target.dir, in.dir) && !strcmp(o.clear_target.gate_id, ID1)
              && !strcmp(o.clear_target.drive, "Measured") && !strcmp(o.clear_target.identity, snap.identity),
          "available: Clear's folder, gate id and drive (review N3R3)");
    snap.probe = PROBE_NOT_CONNECTED;
    native_settings_facts(&in, &o);
    CHECK(o.clear == 0, "not connected: none");
    snap.kind = LOC_BRIDGE_TOO_OLD;
    snprintf(snap.in_use, sizeof snap.in_use, "C:\\support\\Cache");
    native_settings_facts(&in, &o);
    CHECK(o.clear == 1 && !o.clear_target.gate_id, "bridge too old: this PC's folder without a gate (review N3R3)");
    in.this_pc_folder = 0;
    native_settings_facts(&in, &o);
    CHECK(o.clear == 2, "bridge too old, no folder");
}

typedef struct {
    int kind;
    char path[NS_PATH_MAX];
    unsigned delay_ms;
    native_choice_probe probe;
} blocked_choice;

static void blocking_picker_label(void *ctx, const char *dir, int remember, char *out, size_t n) {
    (void)dir; (void)remember;
    const blocked_choice *b = ctx;
    plat_sleep_ms(b->delay_ms);
    snprintf(out, n, "STICK");
}

static int picker_volume(void *ctx, const char *folder, int *refusal, char *root, size_t rn, char *real, size_t n) {
    (void)ctx; (void)folder; (void)root; (void)rn; (void)real; (void)n;
    *refusal = VOL_REFUSE_INTERNAL;
    return 0;
}

static void read_picker_label(void *ctx) {
    blocked_choice *b = ctx;
    native_choice_ops o = {.ctx = b, .volume = picker_volume, .drive_name = blocking_picker_label};
    native_choice_probe_folder(&o, b->path, &b->probe);
}

static void t_picker_label_deadline(void) {
    blocked_choice b = {.delay_ms = 400};
    snprintf(b.path, sizeof b.path, "%s", vol);
    uint64_t end = plat_now_mono_ns() + 50000000;
    int rc = native_choice_read(read_picker_label, &b, sizeof b, end);
    CHECK(rc == NL_TIMEOUT && !b.probe.drive[0],
          "t_picker_label_deadline: blocked label must time out without publishing eligibility");
}

static void t_picker_shared_deadline(void) {
    blocked_choice b = {0};
    snprintf(b.path, sizeof b.path, "%s", vol);
    uint64_t end = plat_now_mono_ns() + 150000000;
    int first = native_choice_read(read_picker_label, &b, sizeof b, end);
    CHECK(!first && !strcmp(b.probe.drive, "STICK"), "first picker check completed");
    b.delay_ms = 60;
    plat_sleep_ms(160);
    int next = native_choice_read(read_picker_label, &b, sizeof b, end);
    CHECK(next == NL_TIMEOUT, "t_picker_shared_deadline: a later blocked check must use the first deadline");
}

static atomic_int commit_blocked, commit_release, commit_fault, commit_creates;

static void wait_for_commit(const char *support) {
    for (int i = 0; i < 500 && native_location_test_commit_pending(support); i++) plat_sleep_ms(1);
    if (native_location_test_commit_pending(support)) abort();
}

static int block_commit_io(void) {
    atomic_store(&commit_blocked, 1);
    for (int i = 0; i < 1000 && !atomic_load(&commit_release); i++) plat_sleep_ms(1);
    atomic_store(&commit_blocked, 0);
    return 0;
}

static int blocked_commit_volume(void *ctx, const char *folder, int *refusal, char *mount, size_t mn, char *real, size_t rn) {
    block_commit_io();
    return f_volume(ctx, folder, refusal, mount, mn, real, rn);
}

static int blocked_commit_fault(int op, const char *path) {
    if (op == NS_FAULT_OPEN_CREATED) atomic_fetch_add(&commit_creates, 1);
    if (op == NS_FAULT_OPEN && !strstr(path, "/config.ini")) return 0;
    if (op == atomic_load(&commit_fault)) return block_commit_io();
    if (op == NS_FAULT_LOCATION_CONFIG_REPLACE && atomic_load(&commit_fault) == -op) plat_sleep_ms(180);
    return 0;
}

static atomic_int support_test_picker_blocked;

static void block_support_test_picker(void *ctx) {
    (void)ctx;
    atomic_store(&support_test_picker_blocked, 1);
    for (int i = 0; i < 1000 && !atomic_load(&commit_release); i++) plat_sleep_ms(1);
    atomic_store(&support_test_picker_blocked, 0);
}

static int support_test_bridge(void *ctx, int *present, int *says) {
    (void)ctx;
    *present = *says = 0;
    return 0;
}

static int support_test_running(void *ctx) {
    (void)ctx;
    return 0;
}

static int64_t support_test_process_start(void *ctx, int32_t pid) {
    (void)ctx;
    (void)pid;
    return -1;
}

static int support_test_drive_offline(void *ctx, const char *dir, char *label, size_t ln, char *letter, size_t lt) {
    (void)ctx; (void)dir; (void)label; (void)ln; (void)letter; (void)lt;
    return ENOENT;
}

static void t_support_while_drives_are_blocked(native_choice_commit *write) {
    const char *support = write->support;
    native_choice_commit local = {0};
    snprintf(local.support, sizeof local.support, "%s", support);
    if (snprintf(local.cache_default, sizeof local.cache_default, "%s/Cache", support) >= (int)sizeof local.cache_default) abort();
    native_steps_env e = {.support = support};
    native_pc_cache_create(&e, local.cache_default);
    native_choice_ops blocked = ops;
    blocked.volume = blocked_commit_volume;
    atomic_store(&commit_release, 0);
    int unused = 0;
    int picker = native_choice_read_keyed(block_support_test_picker, &unused, sizeof unused,
                                          plat_now_mono_ns() + 80000000, 0, "/hung-drive");
    int drive = native_choice_commit_run(write, &blocked, lc_rmdir, plat_now_mono_ns() + 80000000);
    CHECK(picker == NL_TIMEOUT && drive == NS_CHOICE_TIMED_OUT && atomic_load(&support_test_picker_blocked) &&
          atomic_load(&commit_blocked), "support test: both drive jobs still occupy their workers (%d, %d)", picker, drive);

    static native_location_runner runner = NATIVE_LOCATION_RUNNER_INIT;
    static const native_location_ops location_ops = {.live_bridge = support_test_bridge, .rekordbox_running = support_test_running,
        .process_start = support_test_process_start, .volume_name = support_test_drive_offline};
    native_location_env location_env = {.support = support, .cache_default = local.cache_default, .ops = &location_ops};
    native_location location;
    int measured = native_location_measure_bounded(&runner, &location_env, NULL, 150000000, &location);
    CHECK(!measured && native_location_matches_config(&location, write->path, write->id),
          "support test: config read returns the selected folder while both drive jobs are hung (%d)", measured);
    char identity[NS_CACHE_IDENTITY_MAX];
    int checked = native_pc_cache_identity_bounded(support, local.cache_default, plat_now_mono_ns() + 150000000, identity);
    CHECK(!checked && identity[0], "support test: This PC identity is available while both drive jobs are hung (%d)", checked);
    int saved = native_choice_commit_run(&local, &ops, lc_rmdir, plat_now_mono_ns() + 150000000);
    CHECK(!saved && atomic_load(&support_test_picker_blocked) && atomic_load(&commit_blocked),
          "support test: Use This PC saves while both drive jobs are hung (%d)", saved);

    atomic_store(&commit_release, 1);
    wait_for_commit(support);
    const char *config = slurp(support, "config.ini");
    CHECK(config && !strstr(config, "cache_dir") && !strstr(config, "cache_id"),
          "support test: the expired drive commit leaves This PC selected after it resumes");
}

static void t_confirmed_commit(const char *root) {
    char support[PATH_MAX], parent[PATH_MAX];
    mk(root, "bounded-commit-support", support, sizeof support);
    mk(vol, "bounded-commit-parent", parent, sizeof parent);
    const char *original = "# previous settings\n";
    put(support, "config.ini", original);
    native_choice_commit create = {.create = 1};
    snprintf(create.support, sizeof create.support, "%s", support);
    snprintf(create.path, sizeof create.path, "%s", parent);
    native_choice_ops blocked = ops;
    blocked.volume = blocked_commit_volume;
    atomic_store(&commit_release, 0);
    native_steps_test_fault = blocked_commit_fault;
    int rc = native_choice_commit_run(&create, &blocked, lc_rmdir, plat_now_mono_ns() + 80000000);
    CHECK(rc == NS_CHOICE_TIMED_OUT && atomic_load(&commit_blocked) && !create.left_folder,
          "R2: blocked CREATE eligibility returns while the device is still blocked (%d)", rc);
    atomic_store(&commit_release, 1);
    wait_for_commit(support);
    native_steps_test_fault = NULL;
    CHECK(!atomic_load(&commit_creates) && !there(parent, PATHS_CACHE_FOLDER),
          "R2: expired eligibility never attempts CREATE after the device resumes");
    rc = native_choice_commit_run(&create, &ops, lc_rmdir, plat_now_mono_ns() + 500000000);
    CHECK(!rc && create.dir[0] && create.id[0] && create.identity[0],
          "R2: the same parent can be used on a new attempt (%d)", rc);
    if (rc) return;

    native_choice_commit write = {0};
    snprintf(write.support, sizeof write.support, "%s", support);
    snprintf(write.path, sizeof write.path, "%s", create.dir);
    snprintf(write.id, sizeof write.id, "%s", create.id);
    snprintf(write.identity, sizeof write.identity, "%s", create.identity);
    snprintf(write.mount, sizeof write.mount, "%s", vol);
    snprintf(write.label, sizeof write.label, "%s", "PICKER LABEL");
    write.has_label = 1;
    atomic_store(&commit_blocked, 0);
    atomic_store(&commit_release, 0);
    rc = native_choice_commit_run(&write, &blocked, lc_rmdir, plat_now_mono_ns() + 80000000);
    CHECK(rc == NS_CHOICE_TIMED_OUT && atomic_load(&commit_blocked) && !strcmp(slurp(support, "config.ini"), original),
          "R2: blocked WRITE eligibility returns with the previous config selected (%d)", rc);
    atomic_store(&commit_release, 1);
    wait_for_commit(support);

    native_steps_test_fault = blocked_commit_fault;
    atomic_store(&commit_fault, NS_FAULT_LOCATION_CONFIG_READ);
    atomic_store(&commit_blocked, 0);
    atomic_store(&commit_release, 0);
    rc = native_choice_commit_run(&write, &ops, lc_rmdir, plat_now_mono_ns() + 80000000);
    CHECK(rc == NS_CHOICE_TIMED_OUT && atomic_load(&commit_blocked),
          "R2: a redirected config read shares the WRITE deadline (%d)", rc);
    atomic_store(&commit_release, 1);
    wait_for_commit(support);
    CHECK(!strcmp(slurp(support, "config.ini"), original), "R2: a timed-out config read cannot publish after it resumes");
    native_steps_test_fault = NULL;

    int lookup_count = remembered;
    rc = native_choice_commit_run(&write, &ops, lc_rmdir, plat_now_mono_ns() + 500000000);
    const char *hint = slurp(support, "location-drive");
    CHECK(!rc && remembered == lookup_count && hint && strstr(hint, "label=PICKER LABEL\n"),
          "R2: WRITE remembers the picker label without calling the drive lookup (%d)", rc);

    mk(vol, "commit-mutating", parent, sizeof parent);
    snprintf(create.path, sizeof create.path, "%s", parent);
    atomic_store(&commit_blocked, 0);
    atomic_store(&commit_release, 0);
    atomic_store(&commit_fault, NS_FAULT_OPEN_CREATED);
    native_steps_test_fault = blocked_commit_fault;
    rc = native_choice_commit_run(&create, &ops, lc_rmdir, plat_now_mono_ns() + 80000000);
    CHECK(rc == NS_CHOICE_TIMED_OUT && atomic_load(&commit_blocked) && create.left_folder &&
          strstr(slurp(support, "config.ini"), write.path),
          "R2: blocked CREATE mutation reports a possible leftover and keeps the previous location (%d)", rc);
    atomic_store(&commit_release, 1);
    wait_for_commit(support);
    CHECK(!there(parent, PATHS_CACHE_FOLDER), "R2: a timed-out CREATE disposes its own unfinished folder after the device resumes");

    put(support, "config.ini", original);
    atomic_store(&commit_fault, -NS_FAULT_LOCATION_CONFIG_REPLACE);
    rc = native_choice_commit_run(&write, &ops, lc_rmdir, plat_now_mono_ns() + 80000000);
    CHECK(rc == NS_CHOICE_SAVED_UNCHECKED && strstr(slurp(support, "config.ini"), write.path),
          "R2: WRITE stays pending through an in-flight replacement and reports its actual outcome (%d)", rc);
    wait_for_commit(support);

    put(support, "config.ini", original);
    atomic_store(&commit_fault, NS_FAULT_OPEN);
    atomic_store(&commit_blocked, 0);
    atomic_store(&commit_release, 0);
    rc = native_choice_commit_run(&write, &ops, lc_rmdir, plat_now_mono_ns() + 80000000);
    CHECK(rc == NS_CHOICE_SAVED_UNCHECKED && atomic_load(&commit_blocked) && strstr(slurp(support, "config.ini"), write.path),
          "R2: a blocked readback after replacement returns a saved but unchecked result (%d)", rc);
    atomic_store(&commit_release, 1);
    wait_for_commit(support);
    native_steps_test_fault = NULL;
    t_support_while_drives_are_blocked(&write);
}

static void t_lane_a_failed_external_marker(const char *root);
static void lane_b_rollback_keeps_marker(const char *root);
static void lane_b_picker_reuses_slot(void);
/* Lane C */
static void t_settings_size_snapshot(void) {
    native_settings_in in = {.location = 1, .dir = "E:\\Cache", .id = ID1};
    native_settings out;
    native_settings_facts(&in, &out);
    CHECK(out.size_kind == 0, "Settings measures while there is no snapshot");
    native_location snap = {.kind = LOC_CHOSEN, .probe = PROBE_AVAILABLE};
    snprintf(snap.config_dir, sizeof snap.config_dir, "%s", in.dir);
    snprintf(snap.config_id, sizeof snap.config_id, "66666666-7777-4888-8999-aaaaaaaaaaaa");
    in.snap = &snap;
    native_settings_facts(&in, &out);
    CHECK(out.size_kind == 0, "Settings rejects a snapshot for a different marker");
    snprintf(snap.config_id, sizeof snap.config_id, "%s", in.id);
    native_settings_facts(&in, &out);
    CHECK(out.size_kind == 4, "Settings can use the memo for a matching snapshot");
    in.location = 0;
    in.snap = NULL;
    native_settings_facts(&in, &out);
    CHECK(out.size_kind == 0, "Settings measures while This PC has no snapshot");
    snap = (native_location){.kind = LOC_THIS_MAC, .probe = -1, .has_in_use = 1, .in_use = "C:\\support\\Cache"};
    in.snap = &snap;
    native_settings_facts(&in, &out);
    CHECK(out.size_kind == 0, "Settings measures while This PC has no local probe or live bridge result");
    snap.rekordbox_running = snap.has_bridge = 1;
    native_settings_facts(&in, &out);
    CHECK(out.size_kind == 4, "Settings can use the memo when This PC has a matching live result");
}

static void t_choice_size_unreadable(const char *root) {
    char cache[PATH_MAX], model[PATH_MAX];
    mk(root, "unreadable-cache", cache, sizeof cache);
    CHECK(native_choice_size(cache) == 0, "empty cache measures zero");
    CHECK(chmod(cache, 0000) == 0, "make the cache unreadable");
    CHECK(native_choice_size(cache) == -1, "unreadable cache size is unknown");
    CHECK(chmod(cache, 0755) == 0, "restore cache permissions");

    mk(cache, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", model, sizeof model);
    CHECK(chmod(model, 0000) == 0, "make the model unreadable while the cache root is readable");
    int64_t size = native_choice_size(cache);
    CHECK(size == -1, "picker must receive unknown size for an unreadable model, got %lld", (long long)size);
    CHECK(chmod(model, 0755) == 0, "restore model permissions");
    CHECK(native_choice_size(cache) == 0, "readable empty model measures zero");
}

void test_location_choice(const char *scratch) {
    char root[PATH_MAX];
    if (!realpath(scratch, root)) abort();
    char v[PATH_MAX];
    mk(root, "vol", v, sizeof v);
    snprintf(vol, sizeof vol, "%s", v);
    remembered = 0;
    t_lane_a_failed_external_marker(root);
    t_probe(root);
    t_probe_trailing_separator(root);
    t_probe_host_roots();
    CHECK(remembered == 0, "probe never writes location-drive");
    t_discover_create_write(root);
    t_rollback_preserves_replacements(root);
    t_support_tree_overlap(root);
    t_commit_revalidates_destination(root);
    t_writable_size();
    t_remove_deep();
    t_this_pc(root);
    t_unmarked_this_pc(root);
    t_this_pc_partial_delete(root);
    t_picker_shared_deadline();
    lane_b_picker_reuses_slot();
    t_picker_label_deadline();
    t_confirmed_commit(root);
    t_location_now();
    t_settings_clear();
    lane_b_rollback_keeps_marker(root);
}

/* Lane B */
static void lane_b_rollback_keeps_marker(const char *root) {
    char sup[PATH_MAX], config[PATH_MAX], parent[PATH_MAX], dir[NS_PATH_MAX], id[40], err[128], identity[NS_CACHE_IDENTITY_MAX];
    mk(root, "lane-b-rollback-support", sup, sizeof sup);
    mk(sup, "config.ini", config, sizeof config);
    mk(vol, "lane-b-rollback-parent", parent, sizeof parent);
    native_steps_env e = {.support = sup, .log = lc_log, .remove_empty_dir_at = lc_rmdir};
    if (native_choice_create(&ops, &e, parent, dir, id, err, identity)) abort();
    put(dir, "extra-file", "keep this");
    int rc = native_choice_write(&ops, &e, dir, id, vol, 1, identity);
    plat_dir d;
    if (plat_dir_open(dir, &d)) abort();
    CHECK(rc == 1 && there(dir, "extra-file") && !marker_check_at(d, id),
          "F2: failed rollback of a nonempty cache keeps the extra file and restores the correct marker");
    plat_dir_close(d);
}

static void lane_b_picker_reuses_slot(void) {
    blocked_choice slow = {.kind = 1, .path = "/slow-picker", .delay_ms = 400};
    int first = native_choice_read(read_picker_label, &slow, sizeof slow, plat_now_mono_ns() + 50000000);
    int second = native_choice_read(read_picker_label, &slow, sizeof slow, plat_now_mono_ns() + 50000000);
    blocked_choice different = {.kind = 1, .path = "/other-picker"};
    int other = native_choice_read(read_picker_label, &different, sizeof different, plat_now_mono_ns() + 100000000);
    blocked_choice different_kind = {.kind = 2, .path = "/slow-picker"};
    int other_kind = native_choice_read(read_picker_label, &different_kind, sizeof different_kind, plat_now_mono_ns() + 100000000);
    CHECK(first == NL_TIMEOUT && second == NL_TIMEOUT && other == 0 && !strcmp(different.probe.drive, "STICK"),
          "F8: retrying the same hung picker path leaves a worker for a different path (%d, %d, %d)", first, second, other);
    CHECK(other_kind == 0 && !strcmp(different_kind.probe.drive, "STICK"),
          "F8: a different kind of check can run at the same path (%d)", other_kind);
    plat_sleep_ms(400);
}

/* A failed create disposes only the empty slot it opened. */
static void t_lane_a_failed_external_marker(const char *root) {
    char support[PATH_MAX], parent[PATH_MAX], slot[2 * PATH_MAX];
    char dir[NS_PATH_MAX], id[40], err[128], identity[NS_CACHE_IDENTITY_MAX];
    mk(root, "lane-a-choice-support", support, sizeof support);
    native_steps_env e = {.support = support, .log = lc_log, .remove_empty_dir_at = lc_rmdir};

    mk(vol, "lane-a-choice-empty", parent, sizeof parent);
    native_steps_test_fault = fail_created_folder_probe;
    int r = native_choice_create(&ops, &e, parent, dir, id, err, identity);
    native_steps_test_fault = NULL;
    CHECK(r == 3 && !there(parent, PATHS_CACHE_FOLDER),
          "N7: CREATE disposes the empty slot when the new folder's identity check fails");
    CHECK(!native_choice_create(&ops, &e, parent, dir, id, err, identity) && there(dir, MARKER_NAME),
          "N7: CREATE can retry on the same parent after disposal");

    mk(vol, "lane-a-choice-conflict", parent, sizeof parent);
    native_steps_test_fault = marker_arrived_at_create;
    r = native_choice_create(&ops, &e, parent, dir, id, err, identity);
    native_steps_test_fault = NULL;
    snprintf(slot, sizeof slot, "%s/%s", parent, PATHS_CACHE_FOLDER);
    const char *marker = slurp(slot, MARKER_NAME);
    CHECK(r == NS_CHOICE_CREATE_LEFT_FOLDER && !dir[0] && !id[0] && marker && !strcmp(marker, "preexisting contents"),
          "N7: CREATE reports a left folder when disposal cannot remove a stray marker");
    mk(vol, "lane-a-choice-alternative", parent, sizeof parent);
    CHECK(!native_choice_create(&ops, &e, parent, dir, id, err, identity) && there(dir, MARKER_NAME),
          "N7: another parent can be used after the marker failure");
    t_choice_size_unreadable(root);
    t_settings_size_snapshot();
}
