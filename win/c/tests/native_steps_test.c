/* native_steps.c (the core DLL's PAYLOAD and CACHE_PREPARE) on a fake env: which payload becomes
 * the flow's, the facts PAYLOAD gives, manifestDue, and CACHE_PREPARE handing only a verified
 * bridge to the admin; the support folder's small files, PRUNE_PAYLOADS and STATE_LINE. */
#include "check.h"

#include "../admin_proto.h"
#include "../native_config.h"
#include "../native_facts.h"
#include "../models.h"
#include "../native_steps.h"
#include "../native_location.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

static struct {
    int fetch_answer;                 /* NS_FETCH_* */
    payload_manifest fresh, saved;
    int has_saved;
    int64_t age;
    const char *bridge_sha;           /* what sha() says of any existing file ("" : unreadable) */
    const char *ort;
    int set_bridge_rc;
    int dl_answer;
    int procs_rc, running, updating, windows;
    int64_t free_support, free_root, free_vf;   /* -1 unknown */
    int one_volume, vol_root, vol_vf; /* one_volume: every path on one; else the support folder's is 0 */
    /* recorded */
    int seen, seen_null, fetches, set_bridges, cleared, downloads;
    int registered;                   /* a bridge is registered now (after the last set or clear) */
    char dl_dir[1100], dl_name[64], dl_sha[65];
    int64_t dl_max;
    int blocked, blocked_asks;
    char seen_version[32], bridge_path[1024], bridge_name[64], logged[512];
} F;

static int f_fetch(void *ctx, payload_manifest *out) {
    (void)ctx;
    F.fetches++;
    if (F.fetch_answer == NS_FETCH_OK) *out = F.fresh;
    return F.fetch_answer;
}
static int f_saved(void *ctx, payload_manifest *out) {
    (void)ctx;
    if (F.has_saved) *out = F.saved;
    return F.has_saved;
}
static void f_seen(void *ctx, const payload_manifest *m) {
    (void)ctx;
    F.seen++;
    if (!m) F.seen_null++;
    else snprintf(F.seen_version, sizeof F.seen_version, "%s", m->payload_version);
}
static int64_t f_age(void *ctx) {
    (void)ctx;
    return F.age;
}
static int f_sha(void *ctx, const char *path, char out[65]) {
    (void)ctx;
    FILE *x = fopen(path, "rb");
    if (!x || !F.bridge_sha[0]) {
        if (x) fclose(x);
        return -1;
    }
    fclose(x);
    snprintf(out, 65, "%s", F.bridge_sha);
    return 0;
}
static void f_ort(void *ctx, char out[65]) {
    (void)ctx;
    snprintf(out, 65, "%s", F.ort);
}
static int f_set_bridge(void *ctx, const char *path, const char *name) {
    (void)ctx;
    if (!path) {
        F.cleared++;
        F.registered = 0;
        return 0;
    }
    F.set_bridges++;
    F.registered = F.set_bridge_rc == 0;
    snprintf(F.bridge_path, sizeof F.bridge_path, "%s", path);
    snprintf(F.bridge_name, sizeof F.bridge_name, "%s", name);
    return F.set_bridge_rc;
}
static void f_log(void *ctx, const char *line) {
    (void)ctx;
    size_t have = strlen(F.logged);
    if (have + strlen(line) + 2 <= sizeof F.logged) snprintf(F.logged + have, sizeof F.logged - have, "%s|", line);
}

static int f_download(void *ctx, const payload_manifest *m, const char *dir, const char *name, const char *sha, int64_t max) {
    (void)ctx;
    (void)m;
    F.downloads++;
    snprintf(F.dl_dir, sizeof F.dl_dir, "%s", dir);
    snprintf(F.dl_name, sizeof F.dl_name, "%s", name);
    snprintf(F.dl_sha, sizeof F.dl_sha, "%s", sha);
    F.dl_max = max;
    return F.dl_answer;
}
static int f_procs(void *ctx, int *running, int *updating, int *windows) {
    (void)ctx;
    *running = F.running;
    *updating = F.updating;
    *windows = F.windows;
    return F.procs_rc;
}
static int64_t f_size(void *ctx, const char *path) {
    (void)ctx;
    struct stat st;
    return stat(path, &st) ? -1 : (int64_t)st.st_size;
}
/* the version folder ("rekordbox 7.2.19"), the root ("RB Stems Plus"), else the support folder */
static int64_t f_free(void *ctx, const char *path) {
    (void)ctx;
    return strstr(path, "rekordbox") ? F.free_vf : strstr(path, "RB Stems Plus") ? F.free_root : F.free_support;
}
static int f_vol(const char *path) { return strstr(path, "rekordbox") ? F.vol_vf : strstr(path, "RB Stems Plus") ? F.vol_root : 0; }
static int f_same(void *ctx, const char *a, const char *b) {
    (void)ctx;
    return F.one_volume || f_vol(a) == f_vol(b);
}
static int f_blocked(void *ctx) {
    (void)ctx;
    F.blocked_asks++;
    return F.blocked;
}
static char removed[1024];
static int rm_rc;
static const char *rm_swap_loc;      /* at the first delete: move this folder away and replace it (review MQ3) */
static const char *rm_swap_file;     /* optional file in the replacement */
static const char *rm_fail_prefix;   /* names starting so fail with EBUSY (NULL: none) */
static int rm_calls;
/* plat_remove_tree_at on the Mac, for the fake: name in dfd and everything under it, never
 * following a link (one is unlinked itself). 0, or an errno */
static int rm_tree_at(int dfd, const char *name) {
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
        if (strcmp(x->d_name, ".") && strcmp(x->d_name, "..")) rm_tree_at(dirfd(dir), x->d_name);
    closedir(dir);
    return unlinkat(dfd, name, AT_REMOVEDIR) ? errno : 0;
}
static int f_rm(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    rm_calls++;
    size_t have = strlen(removed);
    if (have + strlen(name) + 2 < sizeof removed) snprintf(removed + have, sizeof removed - have, "%s|", name);
    if (rm_rc) return rm_rc;
    if (rm_swap_loc) {
        char mv[1400];
        snprintf(mv, sizeof mv, "%s-moved", rm_swap_loc);
        if (rename(rm_swap_loc, mv) || mkdir(rm_swap_loc, 0755)) abort();
        if (rm_swap_file) {
            char file[1400];
            snprintf(file, sizeof file, "%s/%s", rm_swap_loc, rm_swap_file);
            int fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd < 0 || close(fd)) abort();
        }
        rm_swap_loc = NULL;
    }
    if (rm_fail_prefix && !strncmp(name, rm_fail_prefix, strlen(rm_fail_prefix))) return EBUSY;
    return rm_tree_at(d.fd, name);   /* relative to the handle */
}
/* plat_remove_empty_dir_at on the Mac, for the fake: race_do (when set) runs first, as something
 * changing the folder between REMOVE_LOCATION's checks and its delete (review MT1); then the name
 * must be a folder (not followed), the one expected, and empty */
static void (*race_do)(int dfd, const char *name);
static void race_empty(int dfd, const char *name);
static void race_support(int dfd, const char *name) {
    if (!strcmp(name, "rbstemsplus")) race_empty(dfd, name);
}
static int f_rmdir(void *ctx, plat_dir d, const char *name, plat_id expect) {
    (void)ctx;
    if (race_do) race_do(d.fd, name);
    plat_stat st;
    int e = plat_stat_at(d, name, &st);
    if (e) return e;
    if (st.kind != PLAT_DIR) return st.kind == PLAT_OTHER ? ELOOP : ENOTDIR;
    if (!plat_id_same(st.id, expect)) return ESTALE;
    return unlinkat(d.fd, name, AT_REMOVEDIR) ? errno : 0;
}
/* a folder's files removed (the earlier cases' leftovers) */
static void empty_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    char p[1400];
    for (struct dirent *x; (x = readdir(d));)
        if (x->d_name[0] != '.' || (x->d_name[1] && strcmp(x->d_name, ".."))) {
            snprintf(p, sizeof p, "%s/%s", path, x->d_name);
            unlink(p);
        }
    closedir(d);
}
static void spit_n(const char *path, size_t n) {
    FILE *x = fopen(path, "wb");
    if (!x) abort();
    for (size_t i = 0; i < n; i++) fputc('x', x);
    fclose(x);
}

static void manifest(payload_manifest *m, const char *version, const char *app, char shac) {
    memset(m, 0, sizeof *m);
    snprintf(m->payload_version, sizeof m->payload_version, "%s", version);
    m->signed_by = "release";
    memset(m->model.sha256, 'b', 64);
    snprintf(m->model.file, sizeof m->model.file, "model.onnx");
    m->model.size = 1000;
    m->has_windows = 1;
    snprintf(m->win_app.version, sizeof m->win_app.version, "%s", app);
    snprintf(m->win_bridge.file, sizeof m->win_bridge.file, "rbstemsplus-bridge-win-x64.dll");
    memset(m->win_bridge.sha256, shac, 64);
}

/* scratch/a/b made (a folder; errors ignored) into out */
static void mkp(char *out, size_t n, const char *a, const char *b) {
    snprintf(out, n, "%s/%s", a, b);
    mkdir(out, 0755);
}
static int count_of(const char *hay, const char *needle) {
    int k = 0;
    for (const char *c = hay; (c = strstr(c, needle)); c += strlen(needle)) k++;
    return k;
}

/* PRUNE_PAYLOADS with its flow payload 1.5.2 (model.onnx): stuck folders tried and logged once
 * (review SR1), and no link followed at payload, at the kept version or at its model (SR2) */
static void test_prune_more(const char *scratch, const native_steps_env *base, native_flow *fl) {
    char sup[1100], p[1200], q[1200];
    struct stat st;
    native_steps_env e = *base;
    e.support = sup;

    /* SR1: 1.4.0 stuck, 1.4.1 deletable: 1.4.1 goes, one line for 1.4.0 */
    mkp(sup, sizeof sup, scratch, "s1");
    mkp(p, sizeof p, sup, "payload");
    mkp(p, sizeof p, sup, "payload/1.5.2");
    mkp(p, sizeof p, sup, "payload/1.4.0");
    mkp(q, sizeof q, sup, "payload/1.4.1");
    snprintf(p, sizeof p, "%s/payload/1.5.2/model.onnx", sup);
    spit_n(p, 1000);
    rm_fail_prefix = "1.4.0";
    removed[0] = 0;
    F.logged[0] = 0;
    snprintf(p, sizeof p, "%s/payload/1.4.0", sup);
    CHECK(!native_step_prune_payloads(&e, fl) && stat(q, &st) && !stat(p, &st) && count_of(F.logged, "1.4.0 couldn't be deleted") == 1,
          "a stuck folder: tried and logged once, the other gone (%s)", F.logged);
    /* 40 stuck, 9.0.0 deletable: 9.0.0 goes, 41 removes asked */
    mkp(sup, sizeof sup, scratch, "s1b");
    mkp(p, sizeof p, sup, "payload");
    mkp(p, sizeof p, sup, "payload/1.5.2");
    snprintf(p, sizeof p, "%s/payload/1.5.2/model.onnx", sup);
    spit_n(p, 1000);
    for (int i = 0; i < 40; i++) {
        char name[32];
        snprintf(name, sizeof name, "payload/stuck-%02d", i);
        mkp(p, sizeof p, sup, name);
    }
    mkp(q, sizeof q, sup, "payload/9.0.0");
    rm_fail_prefix = "stuck";
    rm_calls = 0;
    F.logged[0] = 0;
    CHECK(!native_step_prune_payloads(&e, fl) && stat(q, &st) && rm_calls == 41, "40 stuck: 9.0.0 still goes, each tried once (%d)", rm_calls);
    rm_fail_prefix = NULL;

    /* SR2 1: payload itself a link to a folder with 1.5.2/model.onnx and 9.9.9: nothing deleted, one line */
    char real[1100];
    mkp(real, sizeof real, scratch, "s2a-real");
    mkp(p, sizeof p, real, "1.5.2");
    mkp(q, sizeof q, real, "9.9.9");
    snprintf(p, sizeof p, "%s/1.5.2/model.onnx", real);
    spit_n(p, 1000);
    mkp(sup, sizeof sup, scratch, "s2a");
    snprintf(p, sizeof p, "%s/payload", sup);
    CHECK(!symlink(real, p), "a linked payload folder");
    removed[0] = 0;
    F.logged[0] = 0;
    CHECK(!native_step_prune_payloads(&e, fl) && !removed[0] && !stat(q, &st) && count_of(F.logged, "isn't a plain folder") == 1,
          "payload a link: nothing deleted, one line (%s)", F.logged);
    /* SR2 2: the kept version folder a link (to a folder with model.onnx), a real 1.4.0 beside it */
    mkp(real, sizeof real, scratch, "s2b-real");
    snprintf(p, sizeof p, "%s/model.onnx", real);
    spit_n(p, 1000);
    mkp(sup, sizeof sup, scratch, "s2b");
    mkp(p, sizeof p, sup, "payload");
    mkp(q, sizeof q, sup, "payload/1.4.0");
    snprintf(p, sizeof p, "%s/payload/1.5.2", sup);
    CHECK(!symlink(real, p), "a linked kept version");
    removed[0] = 0;
    F.logged[0] = 0;
    CHECK(!native_step_prune_payloads(&e, fl) && !removed[0] && !stat(q, &st) && !F.logged[0], "the kept version a link: nothing deleted or logged");
    /* SR2 3: the model a link to a real file, a real 1.4.0 beside it */
    char file[1200];
    snprintf(file, sizeof file, "%s/s2c-model", scratch);
    spit_n(file, 1000);
    mkp(sup, sizeof sup, scratch, "s2c");
    mkp(p, sizeof p, sup, "payload");
    mkp(p, sizeof p, sup, "payload/1.5.2");
    mkp(q, sizeof q, sup, "payload/1.4.0");
    snprintf(p, sizeof p, "%s/payload/1.5.2/model.onnx", sup);
    CHECK(!symlink(file, p), "a linked model");
    removed[0] = 0;
    CHECK(!native_step_prune_payloads(&e, fl) && !removed[0] && !stat(q, &st), "the model a link: nothing deleted");
}

/* REMOVE_OWN_FOLDERS' one-shot faults (NATIVE_STEPS_TEST_HOOKS): the next open of a path, or stat
 * of a name, equal to fault_name fails with fault_rc once */
static const char *fault_name;
static int fault_op, fault_rc;
static int f_fault(int op, const char *name) {
    if (!fault_name || op != fault_op || strcmp(name, fault_name)) return 0;
    fault_name = NULL;
    return fault_rc;
}

#define SAVED_MODEL "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.onnx"

/* a support folder at scratch/name with a saved model, installed and a payload */

static void own_tree(char *sup, size_t n, const char *scratch, const char *name, const char *originals) {
    char p[1300];
    mkp(sup, n, scratch, name);
    mkp(p, sizeof p, sup, originals);
    snprintf(p, sizeof p, "%s/%s/" SAVED_MODEL, sup, originals);
    spit_n(p, 3);
    snprintf(p, sizeof p, "%s/installed", sup);
    spit_n(p, 3);
    mkp(p, sizeof p, sup, "payload");
    mkp(p, sizeof p, sup, "payload/1.5.2");
    snprintf(p, sizeof p, "%s/payload/1.5.2/payload.json", sup);
    spit_n(p, 3);
}

/* whether sup/name exists (a link not followed) */
static int there(const char *sup, const char *name) {
    char p[1300];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", sup, name);
    return !lstat(p, &st);
}

/* REMOVE_OWN_FOLDERS: support with or without its originals, the other folders; only the expected
 * support (review S53), originals by id (S51), fail closed (S52), links at Cache and Logs left
 * (S54), roots must be folders (S59), other links unlinked as themselves (S60), failed deletes
 * (S61) */
static void test_remove_own(const char *scratch, const native_steps_env *base) {
    char sup[1100], p[1200], q[1300], outside[1100];
    struct stat st;
    native_steps_env e = *base;
    e.support = sup;
    rm_rc = 0;
    rm_fail_prefix = NULL;
    native_steps_test_fault = f_fault;
    mkp(outside, sizeof outside, scratch, "own-outside");
    snprintf(p, sizeof p, "%s/keep-me", outside);
    spit_n(p, 1);
    /* Support’s app artifacts and saved originals. */
    own_tree(sup, sizeof sup, scratch, "own-support", "originals");
    CHECK(native_remove_own_folders(&e, sup, 1) == 0, "keep the originals: all went");
    CHECK(there(sup, "originals/" SAVED_MODEL), "the originals were kept");
    CHECK(!there(sup, "installed") && !there(sup, "payload") && !there(sup, "Cache"), "support's other entries gone");
    /* An unknown link and its target stay. */
    snprintf(p, sizeof p, "%s/linked", sup);
    CHECK(!symlink(outside, p), "a link in support");
    F.logged[0] = 0;
    CHECK(native_remove_own_folders(&e, sup, 1) == 1 && there(sup, "linked"), "an unknown link in support is kept");
    if (unlink(p)) abort();
    snprintf(p, sizeof p, "%s/keep-me", outside);
    CHECK(!stat(p, &st), "what it points to untouched");
    /* without keeping: support goes whole */
    CHECK(native_remove_own_folders(&e, sup, 0) == 0 && stat(sup, &st), "support gone whole");
    CHECK(native_remove_own_folders(&e, sup, 1) == 0, "nothing there: all went");

    /* a mixed-case Originals (a volume that ignores case finds it as "originals"): kept, by id */
    own_tree(sup, sizeof sup, scratch, "own-case", "Originals");
    snprintf(p, sizeof p, "%s/oRIGINALS", sup);
    {
        CHECK(native_remove_own_folders(&e, sup, 1) == 0 && there(sup, "Originals/" SAVED_MODEL) && !there(sup, "installed"),
              "a mixed-case Originals: kept, the rest gone");
    }
    CHECK(native_remove_own_folders(&e, sup, 0) == 0 && stat(sup, &st), "without keeping: all gone");

    /* end to end (review VU3): MODELS_INFO's verified decides REMOVE_OWN_FOLDERS' keep; a saved
     * copy that can't be read is verified (unknown), so it stays. The DLL's own dispatch
     * (core_native_steps.c's CORE_STEP_MODELS_INFO, then REMOVE_OWN_FOLDERS "1") isn't host-linked:
     * it is checked on the VM only (W-row for the next Windows round, review VQ5) */
    own_tree(sup, sizeof sup, scratch, "own-e2e", "originals");
    char locked[1300], rbdir[1200];
    snprintf(locked, sizeof locked, "%s/originals/%064d.onnx", sup, 7);
    spit_n(locked, 4);
    chmod(locked, 0);
    snprintf(rbdir, sizeof rbdir, "%s/rb-model", scratch);
    models mm = {.dir = rbdir, .originals = "", .support = sup};
    int verified = models_any_verified(&mm);
    CHECK(verified == 1 && native_remove_own_folders(&e, sup, verified) == 0 && !lstat(locked, &st) && !there(sup, "installed"),
          "e2e: a copy that can't be read: verified, REMOVE_OWN_FOLDERS keeps it, the rest gone");
    chmod(locked, 0644);
    CHECK(native_remove_own_folders(&e, sup, 0) == 0 && stat(sup, &st), "e2e: then without keeping: all gone");

    /* support that can't be listed (0300): 1, nothing deleted, the originals kept */
    own_tree(sup, sizeof sup, scratch, "own-locked", "originals");
    chmod(sup, 0300);
    removed[0] = 0;
    F.logged[0] = 0;
    int r = native_remove_own_folders(&e, sup, 1), r0 = native_remove_own_folders(&e, sup, 0);
    chmod(sup, 0755);
    CHECK(r == 1 && r0 == 1 && !removed[0] && strstr(F.logged, "couldn't open"), "support unreadable: 1 either way, logged, nothing tried");
    CHECK(there(sup, "originals/" SAVED_MODEL) && there(sup, "installed"), "the originals and the rest kept");

    /* A one-shot EBUSY on opening support stops all cleanup. */
    fault_rc = EBUSY;
    const struct {
        int op, keep;
        const char *name;
    } faults[] = {{NS_FAULT_OPEN, 1, sup}, {NS_FAULT_OPEN, 0, sup}};
    for (int i = 0; i < (int)(sizeof faults / sizeof faults[0]); i++) {
        fault_op = faults[i].op;
        fault_name = faults[i].name;
        removed[0] = 0;
        CHECK(native_remove_own_folders(&e, sup, faults[i].keep) == 1 && !fault_name && !removed[0], "EBUSY %d: 1, nothing tried", i);
        CHECK(there(sup, "originals/" SAVED_MODEL) && there(sup, "installed") && there(sup, "payload/1.5.2/payload.json"), "EBUSY %d: everything kept", i);
    }
    /* an entry's own stat failing in the walk: 1, the rest still go, support stays */
    fault_op = NS_FAULT_STAT;
    fault_name = "installed";
    CHECK(native_remove_own_folders(&e, sup, 0) == 1 && there(sup, "installed") && !there(sup, "payload") && !there(sup, "originals"),
          "an entry that can't be looked at: 1, kept with support, the rest gone");
    fault_name = NULL;
    /* a link at Cache (and at Logs): left, 1, what it points to untouched, support stays */
    for (int keep = 0; keep < 2; keep++) {
        own_tree(sup, sizeof sup, scratch, "own-linked-cache", "originals");
        snprintf(p, sizeof p, "%s/Cache", sup);
        CHECK(!symlink(outside, p), "a linked Cache");
        snprintf(p, sizeof p, "%s/Logs", sup);
        CHECK(!symlink(outside, p), "a linked Logs");
        F.logged[0] = 0;
        CHECK(native_remove_own_folders(&e, sup, keep) == 1 && there(sup, "Cache") && there(sup, "Logs") && !there(sup, "installed")
                  && strstr(F.logged, "refused to delete"),
              "keep=%d: the links at Cache and Logs left, logged, 1, the rest gone", keep);
        CHECK(!stat(sup, &st) && (keep ? there(sup, "originals/" SAVED_MODEL) : !there(sup, "originals")), "keep=%d: support stays", keep);
        snprintf(p, sizeof p, "%s/keep-me", outside);
        CHECK(!stat(p, &st), "keep=%d: what they point to untouched", keep);
        snprintf(p, sizeof p, "%s/Cache", sup);
        unlink(p);
        snprintf(p, sizeof p, "%s/Logs", sup);
        unlink(p);
        CHECK(native_remove_own_folders(&e, sup, 0) == 0 && stat(sup, &st), "keep=%d: then all of it goes", keep);
    }

    /* support a link: left, nothing deleted where it points */
    snprintf(sup, sizeof sup, "%s/own-linked-support", scratch);
    CHECK(!symlink(outside, sup), "a linked support folder");
    removed[0] = 0;
    CHECK(native_remove_own_folders(&e, sup, 0) == 1 && native_remove_own_folders(&e, sup, 1) == 1 && !lstat(sup, &st)
              && !removed[0],
          "support a link: left either way, not all went");
    snprintf(p, sizeof p, "%s/keep-me", outside);
    CHECK(!stat(p, &st), "what it points to untouched");
    /* a file at support: refused, left, logged */
    snprintf(sup, sizeof sup, "%s/own-file", scratch);
    spit_n(sup, 2);
    F.logged[0] = 0;
    removed[0] = 0;
    CHECK(native_remove_own_folders(&e, sup, 0) == 1 && native_remove_own_folders(&e, sup, 1) == 1 && !stat(sup, &st)
              && S_ISREG(st.st_mode) && !removed[0] && strstr(F.logged, "refused to delete"),
          "a file at support: refused, left, logged");
    /* support not the expected one: the scratch parent, a sibling, none, empty: refused, logged,
     * nothing touched */
    char sib[1100];
    own_tree(sup, sizeof sup, scratch, "own-wrong", "originals");
    own_tree(sib, sizeof sib, scratch, "own-wrong-sibling", "originals");
    const char *wrong[4] = {scratch, sib, NULL, ""};
    for (int i = 0; i < 4; i++)
        for (int keep = 0; keep < 2; keep++) {
            e.support = wrong[i];
            F.logged[0] = 0;
            removed[0] = 0;
            CHECK(native_remove_own_folders(&e, sup, keep) == 1 && !removed[0] && strstr(F.logged, "refused to delete"),
                  "support %s keep=%d: refused, logged, nothing tried", wrong[i] ? wrong[i] : "NULL", keep);
        }
    e.support = sup;
    for (int i = 0; i < 4; i++) {
        F.logged[0] = 0;
        removed[0] = 0;
        CHECK(native_remove_own_folders(&e, wrong[i], 0) == 1 && !removed[0] && strstr(F.logged, "refused to delete"),
              "expected %s: refused, logged, nothing tried", wrong[i] ? wrong[i] : "NULL");
    }
    CHECK(there(sup, "originals/" SAVED_MODEL) && there(sup, "installed") && there(sib, "installed") && !stat(scratch, &st), "nothing touched");
    /* the case of the expected path's letters ignored */
    char upper[1100];
    snprintf(upper, sizeof upper, "%s", sup);
    for (char *c = strrchr(upper, '/'); c && *c; c++)
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    CHECK(native_remove_own_folders(&e, upper, 0) == 0 && stat(sup, &st), "the expected path in another case: all gone");

    /* review SF2 and its follow-up: support under a linked parent (a junction at LocalAppData):
     * everything goes, support itself through the parent opened following the link, by id */
    char real[1100], lparent[1100];
    mkp(real, sizeof real, scratch, "real-local");
    snprintf(lparent, sizeof lparent, "%s/linked-local", scratch);
    CHECK(!symlink(real, lparent), "a linked LocalAppData");
    own_tree(sup, sizeof sup, lparent, "rbstemsplus", "originals");
    F.logged[0] = 0;
    snprintf(p, sizeof p, "%s/rbstemsplus", real);
    CHECK(native_remove_own_folders(&e, sup, 0) == 0 && stat(p, &st) && !lstat(lparent, &st) && S_ISLNK(st.st_mode),
          "support under a linked parent: 0, support gone, the link kept (%s)", F.logged);
    /* support swapped for another empty folder between its walk and its delete: that one stays, 1 */
    own_tree(sup, sizeof sup, lparent, "rbstemsplus", "originals");
    race_do = race_support;
    F.logged[0] = 0;
    int sr = native_remove_own_folders(&e, sup, 0);
    race_do = NULL;
    CHECK(sr == 1 && !stat(p, &st) && S_ISDIR(st.st_mode) && strstr(F.logged, "couldn't remove"), "support swapped before its delete: 1, the other folder stays (%s)",
          F.logged);
    rmdir(p);
    snprintf(q, sizeof q, "%s/moved-away", real);
    rm_tree_at(AT_FDCWD, q);
    /* the parent can't be opened: 1, logged */
    own_tree(sup, sizeof sup, lparent, "rbstemsplus", "originals");
    fault_name = lparent;
    fault_op = NS_FAULT_OPEN;
    fault_rc = EACCES;
    F.logged[0] = 0;
    sr = native_remove_own_folders(&e, sup, 0);
    CHECK(sr == 1 && !fault_name && !stat(p, &st) && strstr(F.logged, "couldn't open"), "the parent can't be opened: 1, support left empty, logged (%s)", F.logged);
    rmdir(p);

    /* Originals must be a directory before its recognized files can be removed. */
    mkp(sup, sizeof sup, scratch, "own-sf3");
    snprintf(p, sizeof p, "%s/originals", sup);
    CHECK(!symlink(outside, p), "originals a link");
    CHECK(native_remove_own_folders(&e, sup, 1) == 1 && !lstat(p, &st), "originals a link: kept");
    unlink(p);
    spit_n(p, 2);
    CHECK(native_remove_own_folders(&e, sup, 0) == 1 && !lstat(p, &st), "originals a file: kept");

    /* review SF4: what stays is decided by stat ids, never the listing's */
    own_tree(sup, sizeof sup, scratch, "own-sf4", "originals");
    snprintf(p, sizeof p, "%s/Cache", sup);
    CHECK(!symlink(outside, p), "a linked Cache");
    fault_op = NS_FAULT_LIST_ID;
    fault_name = "originals";
    fault_rc = 1;
    CHECK(native_remove_own_folders(&e, sup, 1) == 1 && !fault_name && there(sup, "originals/" SAVED_MODEL) && there(sup, "Cache"),
          "a listing id other than the stat id: the originals and the linked Cache still kept");
    fault_name = NULL;

    /* S61's m4: a listing that couldn't be read whole: 1, support stays (what it held may be more) */
    own_tree(sup, sizeof sup, scratch, "own-m4", "originals");
    fault_op = NS_FAULT_LIST;
    fault_name = "";
    fault_rc = 1;
    CHECK(native_remove_own_folders(&e, sup, 0) == 1 && !fault_name && !stat(sup, &st), "a listing not read whole: 1, support kept");
    fault_name = NULL;
    native_steps_test_fault = NULL;
    CHECK(native_remove_own_folders(&e, sup, 0) == 0 && stat(sup, &st), "read whole again: all gone");

    /* Unknown names are kept regardless of their length. */
    own_tree(sup, sizeof sup, scratch, "own-m3", "originals");
    char longname[300] = "";
    for (int k = 0; k < 86; k++) strcat(longname, "\xe9\x9f\xb3");
    snprintf(p, sizeof p, "%s/%s", sup, longname);
    spit_n(p, 1);
    CHECK(strlen(longname) == 258 && !stat(p, &st), "m3: the 258-byte name was made");
    CHECK(native_remove_own_folders(&e, sup, 0) == 1 && !stat(p, &st), "m3: the unknown 258-byte name is kept");
}

/* LEFTOVER (review S55): a fake process list, naps counted */
static int64_t lo_started;   /* what start() answers for any pid */
static int lo_asks, lo_naps;
static int64_t lo_start(void *ctx, int32_t pid) {
    (void)ctx;
    (void)pid;
    lo_asks++;
    return lo_started;
}
static void lo_nap(void *ctx) {
    (void)ctx;
    lo_naps++;
}

static void test_leftover(const char *scratch) {
    char sup[1100], p[1200], id[64], label[64];
    mkp(sup, sizeof sup, scratch, "leftover");
    snprintf(p, sizeof p, "%s/busy", sup);
    FILE *x = fopen(p, "wb");
    fputs("4242\n100\nuninstall-all\nRemove everything\n", x);
    fclose(x);
    /* this process's own marker, live: none at once */
    lo_started = 100;
    lo_asks = lo_naps = 0;
    native_leftover(sup, 4242, lo_start, lo_nap, NULL, id, sizeof id, label, sizeof label);
    CHECK(!id[0] && !label[0] && lo_asks == 1 && !lo_naps, "own and live: none, no wait");
    /* this process's pid with another start (a crash, then the pid reused): offered */
    lo_started = 200;
    lo_asks = lo_naps = 0;
    native_leftover(sup, 4242, lo_start, lo_nap, NULL, id, sizeof id, label, sizeof label);
    CHECK(!strcmp(id, "uninstall-all") && !strcmp(label, "Remove everything") && !lo_naps, "own pid, another start: the action");
    /* a start that can't be asked: live, waited for 20 naps, then none */
    lo_started = -2;
    lo_asks = lo_naps = 0;
    native_leftover(sup, 4243, lo_start, lo_nap, NULL, id, sizeof id, label, sizeof label);
    CHECK(!id[0] && lo_naps == 20 && lo_asks == 21, "-2: none after 20 naps (%d, %d)", lo_naps, lo_asks);
    /* another live writer: waited for, then none */
    lo_started = 100;
    lo_asks = lo_naps = 0;
    native_leftover(sup, 4243, lo_start, lo_nap, NULL, id, sizeof id, label, sizeof label);
    CHECK(!id[0] && lo_naps == 20, "another live writer: none after 20 naps");
    /* another process, gone: the action */
    lo_started = -1;
    lo_asks = lo_naps = 0;
    native_leftover(sup, 4243, lo_start, lo_nap, NULL, id, sizeof id, label, sizeof label);
    CHECK(!strcmp(id, "uninstall-all") && !strcmp(label, "Remove everything") && !lo_naps, "another process, dead: the action");
    /* a marker without an action: none */
    x = fopen(p, "wb");
    fputs("4243\n100\n", x);
    fclose(x);
    native_leftover(sup, 4242, lo_start, lo_nap, NULL, id, sizeof id, label, sizeof label);
    CHECK(!id[0] && !label[0], "a marker without an action: none");
    /* no marker, no support: none */
    unlink(p);
    native_leftover(sup, 4242, lo_start, lo_nap, NULL, id, sizeof id, label, sizeof label);
    CHECK(!id[0], "no marker: none");
    native_leftover(NULL, 4242, lo_start, lo_nap, NULL, id, sizeof id, label, sizeof label);
    CHECK(!id[0], "no support: none");
}

/* REMOVE_LOCATION: the marker gate, no link followed, only the model folders (Clear) or all of
 * it, the marker last and the folder only once empty (the uninstall) */
static void test_remove_location_review(const char *top, const native_steps_env *e, const char *id);
static void test_remove_location(const char *scratch, const native_steps_env *base) {
    char top[1100], loc[1300], p[1400], name[256], err[128];
    native_steps_env e = *base;
    struct stat st;
    static const char id[] = "0E8A1D84-0C4B-4F6B-9E4B-2D5E1C1C9A11";
    static const char rel[] = "drive/Stems/RB Stems Plus Cache.noindex";
    rm_rc = 0;
    rm_fail_prefix = NULL;
    if (!realpath(scratch, top)) abort();   /* no link on the way to the scratch "drive" */
    mkp(p, sizeof p, top, "drive");
    mkp(p, sizeof p, top, "drive/Stems");
    mkp(loc, sizeof loc, top, rel);
    char hexd[65];
    memset(hexd, 'a', 64);
    hexd[64] = 0;
    snprintf(p, sizeof p, "%s/%s", loc, hexd);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/%s/stem", loc, hexd);
    spit_n(p, 3);
    snprintf(p, sizeof p, "%s/._stray", loc);
    spit_n(p, 1);
    snprintf(p, sizeof p, "%s/not-a-model", loc);
    spit_n(p, 1);
    /* no marker, or another id: refused, nothing touched */
    CHECK(native_remove_location_under(&e, top, rel, id, 0, name, err, NULL) == 6, "no marker: 6");
    snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
    FILE *x = fopen(p, "w");
    fprintf(x, "id=%s\nformat=rbstems-v1\n", "11111111-2222-3333-4444-555555555555");
    fclose(x);
    CHECK(native_remove_location_under(&e, top, rel, id, 0, name, err, NULL) == 6, "another location's marker: 6");
    x = fopen(p, "w");
    fprintf(x, "id=%s\nformat=rbstems-v1\n", id);
    fclose(x);
    /* not our folder's name, or "..": refused as not connected */
    CHECK(native_remove_location_under(&e, top, "drive/Stems", id, 0, name, err, NULL) == 1
              && native_remove_location_under(&e, top, "drive/../drive/Stems/RB Stems Plus Cache.noindex", id, 0, name, err, NULL) == 1,
          "not a location's path: 1");
    /* Clear: the model folders and "._" files go; the marker, the folder and other files stay */
    snprintf(p, sizeof p, "%s/%s", loc, hexd);
    CHECK(!native_remove_location_under(&e, top, rel, id, 0, name, err, NULL) && stat(p, &st), "Clear: the model folder went");
    snprintf(p, sizeof p, "%s/._stray", loc);
    CHECK(stat(p, &st), "and the ._ file");
    snprintf(p, sizeof p, "%s/not-a-model", loc);
    CHECK(!stat(p, &st), "another file stays");
    snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
    CHECK(!stat(p, &st), "the marker stays");
    /* a link on the way: never followed */
    char real[1300], lk[1300];
    mkp(real, sizeof real, top, "real-drive");
    snprintf(lk, sizeof lk, "%s/linked", top);
    CHECK(!symlink(real, lk), "a linked folder on the way");
    CHECK(native_remove_location_under(&e, top, "linked/RB Stems Plus Cache.noindex", id, 1, name, err, NULL) == 4 && !strcmp(name, "linked") && err[0],
          "a link on the way: 4, its name and the OS error (%s: %s)", name, err);
    /* the uninstall: everything in the cache, the marker last, then the folder */
    snprintf(p, sizeof p, "%s/drive/Stems/._RB Stems Plus Cache.noindex", top);
    spit_n(p, 1);
    rm_rc = EACCES;
    CHECK(native_remove_location_under(&e, top, rel, id, 1, name, err, NULL) == 7 && !stat(loc, &st), "a delete that fails: 7, the folder and marker stay");
    snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
    CHECK(!stat(p, &st), "the marker still there: still found");
    rm_rc = 0;
    CHECK(!native_remove_location_under(&e, top, rel, id, 1, name, err, NULL) && stat(loc, &st), "the uninstall: the folder gone");
    snprintf(p, sizeof p, "%s/drive/Stems/._RB Stems Plus Cache.noindex", top);
    CHECK(!stat(p, &st), "test_whole_remove_preserves_parent_sibling: the unrelated regular file beside the cache stays");
    snprintf(p, sizeof p, "%s/drive/Stems", top);
    CHECK(!stat(p, &st), "its parent stays");
    CHECK(native_remove_location_under(&e, top, rel, id, 1, name, err, NULL) == 4, "gone: its folder can't be opened (4)");
    test_remove_location_review(top, &e, id);
}

/* review MT1's races: what happens to the folder between the checks and its delete */
static void put_at(int dfd, const char *dir, const char *file, const char *text) {
    int d = openat(dfd, dir, O_RDONLY | O_DIRECTORY);
    if (d < 0) abort();
    int f = openat(d, file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) abort();
    if (write(f, text, strlen(text)) < 0) abort();
    close(f);
    close(d);
}
static void race_file(int dfd, const char *name) { put_at(dfd, name, "late-file", "late"); }
static void race_away(int dfd, const char *name) {
    if (renameat(dfd, name, dfd, "moved-away")) abort();
}
static void race_plain(int dfd, const char *name) {
    race_away(dfd, name);
    mkdirat(dfd, name, 0755);
    put_at(dfd, name, "user-document.txt", "mine");
}
static void race_marker(int dfd, const char *name) {
    race_away(dfd, name);
    mkdirat(dfd, name, 0755);
    put_at(dfd, name, ".rbstemsplus-cache", "id=11111111-2222-3333-4444-555555555555\nformat=rbstems-v1\n");
}
static void race_empty(int dfd, const char *name) {
    race_away(dfd, name);
    mkdirat(dfd, name, 0755);
}
static void race_put_file(int dfd, const char *name) {
    race_away(dfd, name);
    int f = openat(dfd, name, O_WRONLY | O_CREAT, 0644);
    close(f);
}
static void race_put_link(int dfd, const char *name) {
    race_away(dfd, name);
    if (symlinkat("moved-away", dfd, name)) abort();
}

/* a fresh location at top/rel's parent: its folder, a model folder and the marker of id */
static void make_location(const char *top, const char *parent, const char *id, char *loc, size_t n) {
    char p[1400];
    mkp(p, sizeof p, top, parent);
    snprintf(loc, n, "%s/%s/RB Stems Plus Cache.noindex", top, parent);
    mkdir(loc, 0755);
    snprintf(p, sizeof p, "%s/%064d", loc, 0);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
    FILE *x = fopen(p, "w");
    fprintf(x, "id=%s\nformat=rbstems-v1\n", id);
    fclose(x);
}

static void test_reassigned_letter(const char *top, const char *id) {
    char replacement[1300];
    make_location(top, "reassigned-letter", id, replacement, sizeof replacement);
    const char *letter = "E:\\Music\\" PATHS_CACHE_FOLDER;
    native_location snap = {.kind = LOC_CHOSEN, .probe = PROBE_AVAILABLE, .has_in_use = 1};
    /* The current E: leads to the replacement, whose marker was copied from the old volume. */
    plat_stat st;
    if (plat_stat_path(replacement, 0, &st)) abort();
    native_location_identity(st.id, snap.identity);
    snprintf(snap.in_use, sizeof snap.in_use, "%s", letter);
    native_location_target target;
    int64_t size;
    native_settings settings;
    native_settings_in in = {.location = 1, .dir = letter, .id = id, .snap = &snap};
    native_settings_facts(&in, &settings);
    CHECK(!native_location_now_facts(1, &snap, letter, id, "", NULL, &target, &size)
              && !target.gate_id[0] && settings.clear == 0,
          "reassigned_letter_with_copied_marker: neither uninstall nor Clear gets a gate");
}

static void test_replaced_confirmed_folder(const char *top, const native_steps_env *e, const char *id) {
    for (int whole = 0; whole <= 1; whole++) {
        const char *parent = whole ? "confirmed-remove" : "confirmed-clear";
        char rel[128], loc[1300], moved[1400], model[1400], name[256], err[128], identity[NL_IDENTITY_MAX];
        snprintf(rel, sizeof rel, "%s/%s", parent, PATHS_CACHE_FOLDER);
        make_location(top, parent, id, loc, sizeof loc);
        plat_stat confirmed, replacement;
        if (plat_stat_path(loc, 0, &confirmed)) abort();
        native_location_identity(confirmed.id, identity);
        snprintf(model, sizeof model, "%s/%064d", loc, 0);
        struct stat st;
        plat_id wrong_volume = confirmed.id;
        wrong_volume.vol ^= 1;
        native_location_identity(wrong_volume, identity);
        int r = native_remove_location_under(e, top, rel, id, whole, name, err, identity);
        CHECK(r == 6 && !stat(model, &st), "confirmed_volume_changed: deletion requires the confirmed volume");
        if (r != 6) continue;
        native_location_identity(confirmed.id, identity);
        snprintf(moved, sizeof moved, "%s-moved", loc);
        if (rename(loc, moved)) abort();
        make_location(top, parent, id, loc, sizeof loc);
        if (plat_stat_path(loc, 0, &replacement) || plat_id_same(confirmed.id, replacement.id)) abort();
        r = native_remove_location_under(e, top, rel, id, whole, name, err, identity);
        CHECK(r == 6 && !stat(model, &st), "replaced_confirmed_folder: %s preserves the replacement's model",
              whole ? "whole removal" : "Clear");
        if (r != 6) continue;
        native_location_identity(replacement.id, identity);
        r = native_remove_location_under(e, top, rel, id, whole, name, err, identity);
        CHECK(!r && stat(model, &st), "%s can remove a confirmed, marked folder's models", whole ? "whole removal" : "Clear");
    }
}

/* the marker's (or its companion's) name in uppercase as the same file: a hard link on a
 * case-sensitive drive, the entry renamed on a case-insensitive one */
static int same_file_upper(const char *loc, const char *lower, const char *upper, int sensitive) {
    char a[1500], b[1500];
    snprintf(a, sizeof a, "%s/%s", loc, lower);
    snprintf(b, sizeof b, "%s/%s", loc, upper);
    return sensitive ? link(a, b) : rename(a, b);
}

/* review MS6, MP3, MP4: the marker and its companion kept until last by identity, not by name */
static void test_remove_location_ms6(const char *top, const native_steps_env *e, const char *id) {
    char loc[1300], p[1500], q[1500], name[256], err[128];
    struct stat st;
    /* the marker in another case, the same file: the uninstall still deletes it, last */
    make_location(top, "ms6a", id, loc, sizeof loc);
    int sensitive = 0;
#ifdef _PC_CASE_SENSITIVE
    sensitive = pathconf(loc, _PC_CASE_SENSITIVE) == 1;
#endif
    native_steps_test_fault = f_fault;
    fault_op = NS_FAULT_STAT;
    CHECK(!same_file_upper(loc, ".rbstemsplus-cache", ".RBSTEMSPLUS-CACHE", sensitive), "MS6: the marker in uppercase");
    CHECK(!native_remove_location_under(e, top, "ms6a/RB Stems Plus Cache.noindex", id, 1, name, err, NULL) && stat(loc, &st),
          "MS6: the marker in another case: 0, the folder gone");

    /* hard links to the marker and to its companion under other names: plain entries, all goes */
    make_location(top, "ms6b", id, loc, sizeof loc);
    snprintf(p, sizeof p, "%s/._.rbstemsplus-cache", loc);
    spit_n(p, 1);
    snprintf(q, sizeof q, "%s/alias2", loc);
    CHECK(!link(p, q), "MS6: the companion linked to alias2");
    snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
    snprintf(q, sizeof q, "%s/alias", loc);
    CHECK(!link(p, q), "MS6: the marker linked to alias");
    CHECK(!native_remove_location_under(e, top, "ms6b/RB Stems Plus Cache.noindex", id, 1, name, err, NULL) && stat(loc, &st),
          "MS6: links to the marker and its companion: 0, the folder gone");

    /* a case variant whose stat fails: kept, and so is the marker (7) */
    make_location(top, "ms6c", id, loc, sizeof loc);
    CHECK(!same_file_upper(loc, ".rbstemsplus-cache", ".RBSTEMSPLUS-CACHE", sensitive), "MS6: the marker in uppercase (c)");
    snprintf(p, sizeof p, "%s/.RBSTEMSPLUS-CACHE", loc);
    fault_name = ".RBSTEMSPLUS-CACHE";
    fault_rc = EIO;
    int r = native_remove_location_under(e, top, "ms6c/RB Stems Plus Cache.noindex", id, 1, name, err, NULL);
    CHECK(r == 7 && !fault_name && !lstat(p, &st), "MS6: a variant that can't be looked at: 7 (%d), the marker kept", r);

    /* the marker's or the companion's stat fails: 6, nothing touched; a companion that isn't there is fine */
    make_location(top, "ms6d", id, loc, sizeof loc);
    snprintf(p, sizeof p, "%s/%064d", loc, 0);
    snprintf(q, sizeof q, "%s/._.rbstemsplus-cache", loc);
    spit_n(q, 1);
    fault_name = ".rbstemsplus-cache";
    fault_rc = EIO;
    r = native_remove_location_under(e, top, "ms6d/RB Stems Plus Cache.noindex", id, 1, name, err, NULL);
    CHECK(r == 6 && !fault_name && !stat(p, &st), "MS6: the marker can't be looked at: 6 (%d), nothing touched", r);
    fault_name = "._.rbstemsplus-cache";
    r = native_remove_location_under(e, top, "ms6d/RB Stems Plus Cache.noindex", id, 1, name, err, NULL);
    CHECK(r == 6 && !fault_name && !stat(p, &st) && !lstat(q, &st), "MS6: the companion can't be looked at: 6 (%d), nothing touched", r);
    fault_name = "._.rbstemsplus-cache";
    fault_rc = ENOENT;
    r = native_remove_location_under(e, top, "ms6d/RB Stems Plus Cache.noindex", id, 1, name, err, NULL);
    CHECK(!r && !fault_name && stat(loc, &st), "MS6: no companion: 0 (%d), the folder gone", r);
    fault_name = NULL;
    native_steps_test_fault = NULL;

    /* review RF1 (the Mac's MQ1): every entry by identity, not only ASCII case variants. The long-s
     * (U+017F) alias resolves to the same name on a case-insensitive drive only; a "._" hard link
     * to the marker is the marker's file on both kinds */
    static const char long_s_marker[] = ".rb\xc5\xbftemsplus-cache", long_s_companion[] = "._.rb\xc5\xbftemsplus-cache";
    char blocked[1500], marker[1500];
    if (!sensitive) {
        /* the marker under the alias, an entry that can't go: 7, the alias and the marker kept */
        make_location(top, "rf1a", id, loc, sizeof loc);
        snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
        snprintf(q, sizeof q, "%s/%s", loc, long_s_marker);
        CHECK(!rename(p, q) && !lstat(p, &st), "RF1: the marker under its long-s alias, which resolves to it");
        snprintf(blocked, sizeof blocked, "%s/blocked", loc);
        spit_n(blocked, 1);
        rm_fail_prefix = "blocked";
        r = native_remove_location_under(e, top, "rf1a/RB Stems Plus Cache.noindex", id, 1, name, err, NULL);
        rm_fail_prefix = NULL;
        CHECK(r == 7 && !lstat(q, &st) && !lstat(p, &st), "RF1: the marker's long-s alias with an entry that can't go: 7, alias and marker kept (%d)", r);
        /* the companion under the alias: the same */
        make_location(top, "rf1b", id, loc, sizeof loc);
        snprintf(p, sizeof p, "%s/._.rbstemsplus-cache", loc);
        spit_n(p, 1);
        snprintf(q, sizeof q, "%s/%s", loc, long_s_companion);
        CHECK(!rename(p, q), "RF1: the companion under its long-s alias");
        snprintf(blocked, sizeof blocked, "%s/blocked", loc);
        spit_n(blocked, 1);
        snprintf(marker, sizeof marker, "%s/.rbstemsplus-cache", loc);
        rm_fail_prefix = "blocked";
        r = native_remove_location_under(e, top, "rf1b/RB Stems Plus Cache.noindex", id, 1, name, err, NULL);
        rm_fail_prefix = NULL;
        CHECK(r == 7 && !lstat(q, &st) && !lstat(marker, &st), "RF1: the companion's long-s alias with an entry that can't go: 7, alias and marker kept (%d)", r);
        /* the marker under the alias alone: the uninstall deletes it last, the folder gone */
        make_location(top, "rf1c", id, loc, sizeof loc);
        snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
        snprintf(q, sizeof q, "%s/%s", loc, long_s_marker);
        CHECK(!rename(p, q), "RF1: the marker under its long-s alias (c)");
        r = native_remove_location_under(e, top, "rf1c/RB Stems Plus Cache.noindex", id, 1, name, err, NULL);
        CHECK(r == 0 && stat(loc, &st), "RF1: the marker's long-s alias alone: 0, the folder gone (%d)", r);
    }
    /* a "._" hard link to the marker, an entry that can't go: kept with the marker by the
     * uninstall (7) and by Clear (0), on both kinds of volume */
    for (int whole = 1; whole >= 0; whole--) {
        make_location(top, whole ? "rf1d1" : "rf1d0", id, loc, sizeof loc);
        snprintf(marker, sizeof marker, "%s/.rbstemsplus-cache", loc);
        snprintf(q, sizeof q, "%s/._marker-hardlink", loc);
        CHECK(!link(marker, q), "RF1: a ._ hard link to the marker (whole %d)", whole);
        snprintf(blocked, sizeof blocked, "%s/blocked", loc);
        spit_n(blocked, 1);
        rm_fail_prefix = "blocked";
        r = native_remove_location_under(e, top, whole ? "rf1d1/RB Stems Plus Cache.noindex" : "rf1d0/RB Stems Plus Cache.noindex", id, whole,
                                         name, err, NULL);
        rm_fail_prefix = NULL;
        CHECK(r == (whole ? 7 : 0) && !lstat(q, &st) && !lstat(marker, &st), "RF1: a ._ hard link to the marker, whole %d: %d (%d), link and marker kept",
              whole, whole ? 7 : 0, r);
    }

    /* a case-sensitive drive: another file in another case goes as any other (MP4); the uppercase
     * hard link (MP7's case) is covered above */
    if (!sensitive) {
        printf("MS6: the scratch drive ignores case; a separate case variant isn't tested here (skipped)\n");
        return;
    }
    make_location(top, "ms6e", id, loc, sizeof loc);
    snprintf(p, sizeof p, "%s/.RBSTEMSPLUS-CACHE", loc);
    spit_n(p, 2);
    snprintf(q, sizeof q, "%s/._.RBSTEMSPLUS-CACHE", loc);
    spit_n(q, 2);
    CHECK(!native_remove_location_under(e, top, "ms6e/RB Stems Plus Cache.noindex", id, 0, name, err, NULL) && !stat(p, &st) && stat(q, &st),
          "MS6, case-sensitive: Clear keeps another file named as the marker, deletes a ._ file named as the companion");
    CHECK(!native_remove_location_under(e, top, "ms6e/RB Stems Plus Cache.noindex", id, 1, name, err, NULL) && stat(loc, &st),
          "MS6, case-sensitive: the uninstall deletes another file named as the marker: 0, the folder gone");
}

/* review MT1, MT2, MT3, MT5 */
static void test_remove_location_review(const char *top, const native_steps_env *e, const char *id) {
    test_reassigned_letter(top, id);
    test_replaced_confirmed_folder(top, e, id);
    char loc[1300], rel[300], p[1500], name[256], err[128];
    struct stat st;
    static const struct {
        void (*race)(int, const char *);
        const char *what, *kept;
    } races[] = {
        {race_file, "a file written in after the check", "late-file"},
        {race_plain, "the folder swapped for one holding a document", "user-document.txt"},
        {race_marker, "the folder swapped for another location's", ".rbstemsplus-cache"},
        {race_empty, "the folder swapped for an empty one", NULL},
        {race_put_file, "a file put at its name", NULL},
        {race_put_link, "a link put at its name", NULL},
    };
    for (size_t i = 0; i < sizeof races / sizeof races[0]; i++) {
        char parent[32];
        snprintf(parent, sizeof parent, "mt1-%zu", i);
        make_location(top, parent, id, loc, sizeof loc);
        snprintf(rel, sizeof rel, "%s/RB Stems Plus Cache.noindex", parent);
        race_do = races[i].race;
        int r = native_remove_location_under(e, top, rel, id, 1, name, err, NULL);
        race_do = NULL;
        int kept = 1;
        if (races[i].kept) {
            snprintf(p, sizeof p, "%s/%s", loc, races[i].kept);
            kept = !lstat(p, &st);
        } else
            kept = !lstat(loc, &st);
        CHECK(r == 7 && kept, "MT1, %s: 7 (%d), left", races[i].what, r);
    }

    /* the folder swapped between the marker check and the id taken: the id is the checked one's */
    make_location(top, "mt1-early", id, loc, sizeof loc);
    rm_swap_loc = loc;
    {
        int r = native_remove_location_under(e, top, "mt1-early/RB Stems Plus Cache.noindex", id, 1, name, err, NULL);
        rm_swap_loc = NULL;
        CHECK(r == 7 && !lstat(loc, &st), "MT1 early swap: 7 (%d), the replacement kept", r);
    }
    /* MT2: a marker renamed in another case is still the marker: never deleted first */
    make_location(top, "mt2", id, loc, sizeof loc);
    snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
    char upper[1500];
    snprintf(upper, sizeof upper, "%s/.RBSTEMSPLUS-CACHE", loc);
    int sensitive = 0;   /* review RF2: the same file in another case on either kind of volume */
#ifdef _PC_CASE_SENSITIVE
    sensitive = pathconf(loc, _PC_CASE_SENSITIVE) == 1;
#endif
    if (!same_file_upper(loc, ".rbstemsplus-cache", ".RBSTEMSPLUS-CACHE", sensitive)) {
        snprintf(p, sizeof p, "%s/blocked", loc);
        mkdir(p, 0755);
        rm_fail_prefix = "blocked";
        int r = native_remove_location_under(e, top, "mt2/RB Stems Plus Cache.noindex", id, 1, name, err, NULL);
        rm_fail_prefix = NULL;
        CHECK(r == 7 && !lstat(upper, &st), "MT2: an entry that can't go: 7, the marker in another case kept (%d)", r);
    } else
        CHECK(0, "MT2: the marker in another case couldn't be made (%d)", errno);
    test_remove_location_ms6(top, e, id);

    /* MT3: an id with a suffix, or a short one: 6, nothing touched */
    make_location(top, "mt3", id, loc, sizeof loc);
    char longer[64];
    snprintf(longer, sizeof longer, "%s-wrong", id);
    snprintf(p, sizeof p, "%s/%064d", loc, 0);
    CHECK(native_remove_location_under(e, top, "mt3/RB Stems Plus Cache.noindex", longer, 1, name, err, NULL) == 6
              && native_remove_location_under(e, top, "mt3/RB Stems Plus Cache.noindex", "0E8A1D84", 1, name, err, NULL) == 6 && !stat(p, &st),
          "MT3: a suffixed or short id: 6, nothing touched");

    /* MT5: a 258-byte name (86 CJK characters; APFS and NTFS take it, the old 256-byte list refused it): Clear keeps it, the uninstall deletes it */
    make_location(top, "mt5", id, loc, sizeof loc);
    char longname[300];
    longname[0] = 0;
    for (int k = 0; k < 86; k++) strcat(longname, "\xe9\x9f\xb3");
    snprintf(p, sizeof p, "%s/%s", loc, longname);
    spit_n(p, 1);
    CHECK(strlen(longname) == 258 && !stat(p, &st), "MT5: the 258-byte name was made");
    CHECK(!native_remove_location_under(e, top, "mt5/RB Stems Plus Cache.noindex", id, 0, name, err, NULL) && !stat(p, &st), "MT5: Clear keeps a long name");
    CHECK(!native_remove_location_under(e, top, "mt5/RB Stems Plus Cache.noindex", id, 1, name, err, NULL) && stat(loc, &st),
          "MT5: the uninstall deletes it, the folder gone");

    /* MT6: Clear keeps uppercase hex and the marker's companion; a model-named link goes as itself */
    make_location(top, "mt6", id, loc, sizeof loc);
    char uhex[65];
    memset(uhex, 'A', 64);
    uhex[64] = 0;
    snprintf(p, sizeof p, "%s/%s", loc, uhex);
    mkdir(p, 0755);
    char comp[1500];
    snprintf(comp, sizeof comp, "%s/._.rbstemsplus-cache", loc);
    spit_n(comp, 1);
    char target[1300], lk[1500];
    snprintf(target, sizeof target, "%s/mt6/outside", top);
    mkdir(target, 0755);
    char lhex[65];
    memset(lhex, 'b', 64);
    lhex[64] = 0;
    snprintf(lk, sizeof lk, "%s/%s", loc, lhex);
    CHECK(!symlink(target, lk), "MT6: a model-named link");
    char ulk[1500], plain[1500];
    snprintf(ulk, sizeof ulk, "%s/._linked", loc);
    CHECK(!symlink(target, ulk), "MT6: a ._ link");
    snprintf(plain, sizeof plain, "%s/notes.txt", loc);
    spit_n(plain, 1);
    CHECK(!native_remove_location_under(e, top, "mt6/RB Stems Plus Cache.noindex", id, 0, name, err, NULL) && !stat(p, &st) && !lstat(comp, &st)
              && !stat(plain, &st) && lstat(lk, &st) && lstat(ulk, &st) && !stat(target, &st),
          "MT6: Clear keeps uppercase hex, a non-hex name and ._ of the marker; the links went as themselves, their target kept");
    /* a marker that is a link: 6; our folder a link: 4 */
    make_location(top, "mt6b", id, loc, sizeof loc);
    snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
    char mreal[1300];
    snprintf(mreal, sizeof mreal, "%s/mt6b/marker-real", top);
    if (rename(p, mreal) || symlink(mreal, p)) abort();
    CHECK(native_remove_location_under(e, top, "mt6b/RB Stems Plus Cache.noindex", id, 1, name, err, NULL) == 6, "MT6: a linked marker: 6");
    char lfold[1300];
    mkp(lfold, sizeof lfold, top, "mt6c");
    snprintf(p, sizeof p, "%s/RB Stems Plus Cache.noindex", lfold);
    CHECK(!symlink(loc, p), "MT6: our folder a link");
    CHECK(native_remove_location_under(e, top, "mt6c/RB Stems Plus Cache.noindex", id, 1, name, err, NULL) == 4, "MT6: our folder a link: 4");
    CHECK(native_remove_location(e, "relative/RB Stems Plus Cache.noindex", id, 1, name, err, NULL) == 1, "MT6: a bad root: 1");
}

/* CLEAR_THIS_MAC (review S43, S47): only support/Cache, through a no-follow handle on support */
static void test_clear_cache(const char *scratch, const native_steps_env *base) {
    char sup[1100], cache[1200], p[1300], outside[1100];
    struct stat st;
    int link = -1;
    char identity[NS_CACHE_IDENTITY_MAX] = "";
    native_steps_env e = *base;
    e.support = sup;
    rm_rc = 0;
    rm_fail_prefix = NULL;
    mkp(outside, sizeof outside, scratch, "cc-outside");
    snprintf(p, sizeof p, "%s/keep-me", outside);
    spit_n(p, 1);
    /* a real tree goes */
    mkp(sup, sizeof sup, scratch, "cc1");
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    native_pc_cache_create(&e, cache);
    CHECK(!native_pc_cache_identity(&e, cache, identity), "marked new cache");
    mkp(p, sizeof p, cache, "ab");
    snprintf(p, sizeof p, "%s/ab/stem.m4a", cache);
    spit_n(p, 3);
    F.logged[0] = 0;
    CHECK(!native_clear_cache(&e, cache, identity, 1, &link) && !link && stat(cache, &st) && !stat(sup, &st) && !F.logged[0],
          "a real tree: gone, support stays, nothing logged");
    /* no Cache, no support: 0 */
    F.logged[0] = 0;
    CHECK(!native_clear_cache(&e, cache, identity, 1, &link) && !link && !F.logged[0], "no Cache: 0, nothing logged");
    snprintf(sup, sizeof sup, "%s/cc-none", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    F.logged[0] = 0;
    CHECK(!native_clear_cache(&e, cache, identity, 1, &link) && !F.logged[0], "no support folder: 0, nothing logged");
    /* a link at Cache: left, *link; a file at Cache: refused, not a link */
    mkp(sup, sizeof sup, scratch, "cc2");
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    CHECK(!symlink(outside, cache), "a linked Cache");
    F.logged[0] = 0;
    CHECK(native_clear_cache(&e, cache, identity, 1, &link) == 1 && link == 1 && !lstat(cache, &st) && strstr(F.logged, "refused to delete"),
          "a link at Cache: left, said so, logged");
    snprintf(p, sizeof p, "%s/keep-me", outside);
    CHECK(!stat(p, &st), "what it points to untouched");
    unlink(cache);
    spit_n(cache, 2);
    F.logged[0] = 0;
    CHECK(native_clear_cache(&e, cache, identity, 1, &link) == 1 && link == 0 && !stat(cache, &st) && strstr(F.logged, "refused to delete"),
          "a file at Cache: refused, left, logged");
    unlink(cache);
    /* support a link (to a folder with a Cache): nothing deleted */
    char real[1100];
    mkp(real, sizeof real, scratch, "cc-real");
    mkp(p, sizeof p, real, "Cache");
    snprintf(sup, sizeof sup, "%s/cc-linked", scratch);
    CHECK(!symlink(real, sup), "a linked support folder");
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    F.logged[0] = 0;
    CHECK(native_clear_cache(&e, cache, identity, 1, &link) != 0 && !stat(p, &st) && strstr(F.logged, "refused to delete"), "support a link: nothing deleted, logged");
    /* support at mode 0000 with a Cache inside: 1, logged, Cache left */
    mkp(sup, sizeof sup, scratch, "cc-locked");
    mkp(cache, sizeof cache, sup, "Cache");
    chmod(sup, 0);
    F.logged[0] = 0;
    CHECK(native_clear_cache(&e, cache, identity, 1, &link) == 1 && strstr(F.logged, "couldn't clear"), "support unreadable: 1, logged");
    chmod(sup, 0755);
    CHECK(!stat(cache, &st), "its Cache still there");
    /* cache_default not support/Cache: refused, logged, nothing touched */
    mkp(sup, sizeof sup, scratch, "cc3");
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    native_pc_cache_create(&e, cache);
    CHECK(!native_pc_cache_identity(&e, cache, identity), "marked new cache");
    char sibling[1200], other[1200];
    mkp(sibling, sizeof sibling, scratch, "Documents");
    mkp(other, sizeof other, scratch, "cc-other");
    mkp(other, sizeof other, scratch, "cc-other/Cache");
    const char *wrong[3] = {sup, sibling, other};
    for (int i = 0; i < 3; i++) {
        F.logged[0] = 0;
        removed[0] = 0;
        CHECK(native_clear_cache(&e, wrong[i], identity, 1, &link) == 1 && !removed[0] && strstr(F.logged, "refused to delete") && !stat(wrong[i], &st)
                  && !stat(cache, &st),
              "cache_default %s: refused, logged, nothing touched", wrong[i]);
    }
    CHECK(native_clear_cache(&e, NULL, identity, 1, &link) == 1, "no cache_default: refused");
    /* a delete that fails: 1, logged */
    snprintf(p, sizeof p, "%s/stem", cache);
    spit_n(p, 1);
    rm_rc = EACCES;
    F.logged[0] = 0;
    CHECK(native_clear_cache(&e, cache, identity, 1, &link) == 1 && strstr(F.logged, "couldn't clear") && !stat(cache, &st), "a failed delete: 1, logged");
    rm_rc = 0;
    rm_swap_loc = cache;
    rm_swap_file = "personal-file";
    int r = native_clear_cache(&e, cache, identity, 1, &link);
    rm_swap_loc = rm_swap_file = NULL;
    snprintf(p, sizeof p, "%s/personal-file", cache);
    CHECK(r == 1 && !stat(p, &st), "test_clear_preserves_replacement: another tree moved into Cache stays");
    snprintf(p, sizeof p, "%s-moved/stem", cache);
    CHECK(stat(p, &st), "test_clear_preserves_replacement: only the verified directory's original contents go");
}

/* DOWNLOAD, REKORDBOX, SPACE */
static void test_stop_keeping_unlink_failure(const char *kept) {
    char entry[1200], manifest[1200];
    snprintf(entry, sizeof entry, "%s/immutable.kept", kept);
    snprintf(manifest, sizeof manifest, "%s/immutable.manifest", kept);
    spit_n(entry, 3);
    spit_n(manifest, 3);
    CHECK(chflags(manifest, UF_IMMUTABLE) == 0, "NW83: make the manifest immutable");
    CHECK(native_stop_keeping(kept, "immutable") != 0, "NW83: a failed manifest unlink reports failure");
    CHECK(access(entry, F_OK) != 0 && errno == ENOENT && access(manifest, F_OK) == 0,
          "NW83: the .kept file is gone and the manifest remains");
    CHECK(chflags(manifest, 0) == 0, "NW83: clear the manifest's immutable flag");

    spit_n(entry, 3);
    CHECK(chflags(entry, UF_IMMUTABLE) == 0, "review 3: make the .kept file immutable");
    int failed = native_stop_keeping(kept, "immutable");
    CHECK(chflags(entry, 0) == 0, "review 3: clear the .kept file's immutable flag");
    CHECK(failed && access(entry, F_OK) == 0 && access(manifest, F_OK) == 0,
          "review 3: failed .kept unlink reports failure and preserves both files");
    CHECK(!native_stop_keeping(kept, "immutable"), "review 3: Stop Keeping can be retried");
}

static void test_native_steps_more(const char *scratch) {
    char vf[1100], root[1100], p[1200], model[1200];
    snprintf(vf, sizeof vf, "%s/rekordbox 7.2.19", scratch);
    snprintf(root, sizeof root, "%s/RB Stems Plus", scratch);
    snprintf(model, sizeof model, "%s/hdemucs.onnx", scratch);
    mkdir(vf, 0755);
    mkdir(root, 0755);
    native_steps_env e = {f_fetch, f_saved, f_seen, f_age, f_sha, f_ort, f_set_bridge, f_log, f_download, f_procs, f_size, f_free, f_same,
                          NULL, scratch, "1.5.0", model, vf, root, f_blocked, f_rm, f_rmdir};
    native_flow fl;
    native_flow_init(&fl);
    native_payload_facts o;
    memset(&F, 0, sizeof F);
    F.bridge_sha = "";
    F.ort = "";
    F.age = -1;

    /* DOWNLOAD: no flow payload; then the model and the bridge into the payload's folder */
    CHECK(native_step_download(&e, &fl, "model") == 1 && F.downloads == 0, "no flow payload: 1");
    manifest(&F.fresh, "1.5.1", "1.5.1", 'a');
    F.fetch_answer = NS_FETCH_OK;
    native_step_payload(&e, &fl, "latest", &o);
    F.dl_answer = NS_DL_OK;
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/payload/1.5.1", scratch);
    CHECK(native_step_download(&e, &fl, "model") == NS_DL_OK && !strcmp(F.dl_dir, dir) && !strcmp(F.dl_name, "model.onnx") && F.dl_sha[0] == 'b'
              && F.dl_max == 1000,
          "the model: into its payload's folder, at most its size");
    F.dl_answer = NS_DL_NO_SPACE;
    CHECK(native_step_download(&e, &fl, "bridge") == NS_DL_NO_SPACE && !strcmp(F.dl_name, "rbstemsplus-bridge-win-x64.dll") && F.dl_sha[0] == 'a'
              && F.dl_max == ADMIN_BRIDGE_MAX,
          "the bridge: its code as is");
    int before = F.downloads;
    CHECK(native_step_download(&e, &fl, "other") == 1 && native_step_download(&e, &fl, NULL) == 1 && F.downloads == before, "another arg: 1");

    /* REKORDBOX */
    int open, closing, updating;
    CHECK(!native_step_rekordbox(&e, &open, &closing, &updating) && !open && !closing && !updating, "nothing runs");
    F.running = 1;
    F.windows = 2;
    CHECK(!native_step_rekordbox(&e, &open, &closing, &updating) && open && !closing, "open with its windows");
    F.windows = 0;
    CHECK(!native_step_rekordbox(&e, &open, &closing, &updating) && open && closing, "running with no window: still closing (and open)");
    /* rekordbox open only in another session (another account): EnumWindows sees none of its
     * windows, but it counts as one: open, never closing */
    F.windows = native_rb_windows(1, 0);
    CHECK(!native_step_rekordbox(&e, &open, &closing, &updating) && open && !closing, "another session only: open, not closing");
    CHECK(native_rb_windows(0, 0) == 0 && native_rb_windows(0, 2) == 2 && native_rb_windows(0, -1) == -1 && native_rb_windows(2, 1) == 3
              && native_rb_windows(1, -1) == 1,
          "the count: another session's process (or an unknown one) is a window; this session's windows unlisted: -1 unless another is open");
    F.windows = 0;
    F.updating = 1;
    CHECK(!native_step_rekordbox(&e, &open, &closing, &updating) && updating, "Pioneer's updater");
    F.procs_rc = -1;
    CHECK(native_step_rekordbox(&e, &open, &closing, &updating) == 1, "couldn't be listed: 1");

    /* SPACE: the model 1000 bytes, 400 of them downloaded; rekordbox's model file 300 */
    int64_t need;
    snprintf(p, sizeof p, "%s/payload", scratch);
    mkdir(p, 0755);
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/model.onnx.bbbbbbbbbbbb.part", dir);
    spit_n(p, 400);
    spit_n(model, 300);
    F.free_support = F.free_root = F.free_vf = (int64_t)1 << 40;
    F.vol_root = 1;
    F.vol_vf = 2;
    CHECK(!native_step_space(&e, &fl, 1, 0, &need) && need == 0, "room: 0");
    F.free_support = 100;
    const int64_t spare = (int64_t)200 << 20;
    CHECK(!native_step_space(&e, &fl, 1, 0, &need) && need == (1000 - 400) + 1000 + 300 + spare, "the model: its download left, its copy, the old one (%lld)",
          (long long)need);
    snprintf(p, sizeof p, "%s/model.onnx.bbbbbbbbbbbb.part", dir);
    spit_n(p, 1200);   /* a part past the model's size: dropped by the download, never credited */
    CHECK(!native_step_space(&e, &fl, 1, 0, &need) && need == 1000 + 1000 + 300 + spare, "a part past the size: the whole download (%lld)",
          (long long)need);
    spit_n(p, 400);
    F.free_support = -1;
    CHECK(!native_step_space(&e, &fl, 1, 0, &need) && need == 0, "unknown free bytes: skipped");
    /* the cache: the live DLL 500, the bridge 50; no saved set yet: pioneer\ 500 and ort\ 500 on
     * the root's volume, the bridge 50 on the version folder's */
    snprintf(p, sizeof p, "%s/onnxruntime.dll", vf);
    spit_n(p, 500);
    snprintf(p, sizeof p, "%s/rbstemsplus-bridge-win-x64.dll", dir);
    spit_n(p, 50);
    F.free_support = (int64_t)1 << 40;
    F.free_root = 10;
    CHECK(!native_step_space(&e, &fl, 0, 1, &need) && need == 500 + 500 + spare, "the root on its own full volume: its need (%lld)", (long long)need);
    F.free_root = (int64_t)1 << 40;
    F.free_vf = 10;
    CHECK(!native_step_space(&e, &fl, 0, 1, &need) && need == 50 + spare, "the version folder's volume: the bridge (%lld)", (long long)need);
    F.vol_vf = F.vol_root;
    F.free_root = 10;
    F.free_vf = (int64_t)1 << 40;   /* asked by the root's path, the first of the two */
    CHECK(!native_step_space(&e, &fl, 0, 1, &need) && need == 500 + 500 + 50 + spare, "the root and the version folder on one volume: one sum (%lld)",
          (long long)need);
    F.vol_vf = 2;
    F.free_vf = (int64_t)1 << 40;
    /* a complete saved set: the original isn't saved again */
    snprintf(p, sizeof p, "%s/pioneer", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/pioneer/7.2.19", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/pioneer/7.2.19/SHA256SUMS", root);
    spit_n(p, 10);
    CHECK(!native_step_space(&e, &fl, 0, 1, &need) && need == 500 + spare, "saved already: not again, the ort copy alone (%lld)", (long long)need);
    /* one volume: the two needs add up, checked against the support folder's free bytes */
    F.one_volume = 1;
    F.free_support = 100;
    CHECK(!native_step_space(&e, &fl, 1, 1, &need) && need == (600 + 1000 + 300) + (500 + 50) + spare, "one volume: summed (%lld)", (long long)need);
    F.one_volume = 0;
    CHECK(!native_step_space(&e, &fl, 1, 1, &need) && need == 600 + 1000 + 300 + spare, "two volumes: the support folder's first");
    /* the model downloaded whole: nothing left to download */
    snprintf(p, sizeof p, "%s/model.onnx", dir);
    spit_n(p, 1000);
    CHECK(!native_step_space(&e, &fl, 1, 0, &need) && need == 1000 + 300 + spare, "downloaded already: its copy and the old one");
    /* no flow payload: 1 for the model, the cache still measured */
    F.fetch_answer = NS_FETCH_UNVERIFIED;
    native_step_payload(&e, &fl, "latest", &o);
    CHECK(native_step_space(&e, &fl, 1, 0, &need) == 1, "the model with no flow payload: 1");
    F.free_root = 10;
    CHECK(!native_step_space(&e, &fl, 0, 1, &need) && need == 500 + spare, "the cache with no flow payload: no bridge counted");

    /* PRUNE_PAYLOADS: nothing with no flow payload, nor before the flow's model is here */
    char other[1200];
    snprintf(other, sizeof other, "%s/payload/1.4.0", scratch);
    mkdir(other, 0755);
    snprintf(p, sizeof p, "%s/payload/payload.json", scratch);
    spit_n(p, 3);
    removed[0] = 0;
    CHECK(!native_step_prune_payloads(&e, &fl) && !removed[0], "no flow payload: nothing deleted");
    manifest(&F.fresh, "1.5.2", "1.5.2", 'a');
    F.fetch_answer = NS_FETCH_OK;
    native_step_payload(&e, &fl, "latest", &o);
    CHECK(!native_step_prune_payloads(&e, &fl) && !removed[0], "the flow's model not here: nothing deleted");
    /* 1.5.2's model here: the other versions' folders go, its own and payload.json stay */
    snprintf(dir, sizeof dir, "%s/payload/1.5.2", scratch);
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/model.onnx", dir);
    spit_n(p, 1000);
    snprintf(p, sizeof p, "%s/payload/1.5.1/model.onnx", scratch);
    remove(p);
    snprintf(p, sizeof p, "%s/payload/1.5.1", scratch);
    empty_dir(p);
    /* S34: a link to a folder outside and a plain file among the versions: never deleted */
    char outside[1100], ln[1200];
    snprintf(outside, sizeof outside, "%s/outside", scratch);
    mkdir(outside, 0755);
    snprintf(ln, sizeof ln, "%s/payload/1.3.0", scratch);
    CHECK(!symlink(outside, ln), "a link among the versions");
    snprintf(p, sizeof p, "%s/payload/1.2.0", scratch);
    spit_n(p, 1);
    rm_rc = EACCES;
    F.logged[0] = 0;
    CHECK(!native_step_prune_payloads(&e, &fl) && strstr(removed, "1.4.0|") && strstr(removed, "1.5.1|") && !strstr(removed, "1.5.2")
              && !strstr(removed, "payload.json") && strstr(F.logged, "couldn't be deleted (error") && !strstr(removed, "1.3.0")
              && !strstr(removed, "1.2.0"),
          "the others asked to go once (%s); a failure logged, the pass ends", removed);
    rm_rc = 0;
    removed[0] = 0;
    struct stat st;
    CHECK(!native_step_prune_payloads(&e, &fl) && stat(other, &st) && !stat(dir, &st) && !strstr(removed, "1.3.0") && !strstr(removed, "1.2.0")
              && !lstat(ln, &st) && !stat(outside, &st) && !stat(p, &st),
          "1.4.0 gone, 1.5.2 kept; the link, what it points to and the file left (%s)", removed);
    /* S33: more than one pass's worth (33), all go; the kept one stays */
    for (int i = 0; i < 33; i++) {
        snprintf(p, sizeof p, "%s/payload/0.0.%d", scratch, i);
        mkdir(p, 0755);
    }
    removed[0] = 0;
    native_step_prune_payloads(&e, &fl);
    int left = 0, named = 0;
    for (int i = 0; i < 33; i++) {
        snprintf(p, sizeof p, "%s/payload/0.0.%d", scratch, i);
        left += !stat(p, &st);
    }
    for (const char *c = removed; (c = strchr(c, '|')); c++) named++;
    CHECK(!left && named == 33 && !stat(dir, &st), "33 old versions: all gone in passes (%d left, %d asked), 1.5.2 kept", left, named);
    /* S31: support itself a link (to a folder with payload\1.5.2\model.onnx and payload\9.9.9\):
     * nothing deleted, one line logged */
    char real[1100], sl[1100];
    snprintf(real, sizeof real, "%s/real-support", scratch);
    mkdir(real, 0755);
    snprintf(p, sizeof p, "%s/payload", real);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/payload/1.5.2", real);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/payload/1.5.2/model.onnx", real);
    spit_n(p, 1000);
    snprintf(p, sizeof p, "%s/payload/9.9.9", real);
    mkdir(p, 0755);
    snprintf(sl, sizeof sl, "%s/linked-support", scratch);
    CHECK(!symlink(real, sl), "a linked support folder");
    native_steps_env linked = e;
    linked.support = sl;
    removed[0] = 0;
    F.logged[0] = 0;
    CHECK(!native_step_prune_payloads(&linked, &fl) && !removed[0] && !stat(p, &st) && strstr(F.logged, "isn't a plain folder")
              && strchr(F.logged, '|') == strrchr(F.logged, '|'),
          "support a link: nothing deleted, one line (%s)", F.logged);
    test_prune_more(scratch, &e, &fl);
    test_remove_own(scratch, &e);
    test_clear_cache(scratch, &e);
    test_remove_location(scratch, &e);
    test_leftover(scratch);
    native_flow_free(&fl);
}

/* the support folder's small files, STATE_LINE */
static void test_native_state(const char *scratch) {
    char sup[1100], p[1200];
    snprintf(sup, sizeof sup, "%s/support/inner", scratch);
    CHECK(!native_support_exists(sup, "welcomed") && native_support_age(sup, "request", 1000) == -1 && !native_support_remove(sup, "busy"),
          "no support folder: nothing there, nothing to remove");
    CHECK(!native_support_write(sup, "welcomed", "", 0) && native_support_exists(sup, "welcomed"), "an empty marker, the folder made");
    CHECK(!native_support_write(sup, "installed", "model=1\ncache=0\n", 16), "installed written");
    snprintf(p, sizeof p, "%s/installed", sup);
    char *b = NULL;
    size_t n = 0;
    CHECK(!plat_read_file(p, 100, &b, &n) && n == 16 && !memcmp(b, "model=1\ncache=0\n", 16), "its text");
    free(b);
    CHECK(!native_support_write(sup, "installed", "model=0\ncache=1\n", 16) && !plat_read_file(p, 100, &b, &n) && !memcmp(b, "model=0", 7),
          "written over");
    free(b);
    snprintf(p, sizeof p, "%s/request", sup);
    FILE *x = fopen(p, "w");
    fputs("reinstall\n", x);
    fclose(x);
    plat_stat st;
    CHECK(!plat_stat_path(p, 0, &st), "the request's stat");
    CHECK(native_support_age(sup, "request", st.mtime + 42) == 42 && native_support_age(sup, "request", st.mtime - 5) == 0, "its age; 0 in the future");
    CHECK(!native_support_remove(sup, "request") && native_support_age(sup, "request", st.mtime) == -1 && !native_support_remove(sup, "request"),
          "taken; again: done");
    snprintf(p, sizeof p, "%s/busy", sup);
    mkdir(p, 0755);
    CHECK(native_support_remove(sup, "busy") != 0 && native_support_exists(sup, "busy"), "a folder named busy: never removed");
    rmdir(p);
    CHECK(native_support_write(NULL, "x", "", 0) != 0, "no support folder: an error");
    /* support a dangling link: refused, nothing made where it points (models' MR3) */
    char dl[1100], dt[1100];
    snprintf(dl, sizeof dl, "%s/support/dangling", scratch);
    snprintf(dt, sizeof dt, "%s/support/never-made", scratch);
    CHECK(!symlink(dt, dl), "a dangling link at support");
    struct stat ds;
    CHECK(native_support_write(dl, "x", "", 0) == ELOOP && lstat(dt, &ds) && errno == ENOENT, "support a dangling link: ELOOP, nothing made");
    unlink(dl);

    char line[256];
    native_state_line(line, sizeof line, "7.2.19.0342", "0002", "0123456789abcdef", 1, 1, 0);
    CHECK(!strcmp(line, "rekordbox 7.2.19.0342, engine 0002, model 0123456789ab, bridge yes, chosen (model: true, cache: false)"), "%s", line);
    native_state_line(line, sizeof line, "", "", "", 0, 0, 1);
    CHECK(!strcmp(line, "rekordbox , engine , model , bridge no, chosen (model: false, cache: true)"), "%s", line);

    /* STOP_KEEPING */
    char kept[1100];
    snprintf(kept, sizeof kept, "%s/kept", scratch);
    mkdir(kept, 0755);
    snprintf(p, sizeof p, "%s/My_Set-1.kept", kept);
    x = fopen(p, "w");
    fclose(x);
    snprintf(p, sizeof p, "%s/My_Set-1.manifest", kept);
    x = fopen(p, "w");
    fclose(x);
    snprintf(p, sizeof p, "%s/odd.kept", kept);
    mkdir(p, 0755);
    CHECK(native_stop_keeping(kept, "My Set") && native_stop_keeping(kept, "") && native_stop_keeping(kept, "../x") && native_stop_keeping(NULL, "a"),
          "not a kept name: refused");
    CHECK(!native_stop_keeping(kept, "My_Set-1") && access(p, F_OK) == 0, "removed; the other set's left");
    snprintf(p, sizeof p, "%s/My_Set-1.manifest", kept);
    CHECK(access(p, F_OK) != 0 && !native_stop_keeping(kept, "My_Set-1"), "its manifest too; again: gone");
    test_stop_keeping_unlink_failure(kept);
    CHECK(native_stop_keeping(kept, "odd") != 0, "a .kept that isn't a file: left, still there");
    /* review S41: "gone" only when it surely is */
    char kreal[1100], klink[1100];
    snprintf(kreal, sizeof kreal, "%s/kept-real", scratch);
    mkdir(kreal, 0755);
    snprintf(p, sizeof p, "%s/A.kept", kreal);
    x = fopen(p, "w");
    fclose(x);
    snprintf(klink, sizeof klink, "%s/kept-link", scratch);
    CHECK(!symlink(kreal, klink), "a linked kept folder");
    CHECK(native_stop_keeping(klink, "A") != 0 && access(p, F_OK) == 0, "a linked kept folder: still there, nothing removed");
    chmod(kreal, 0100);
    CHECK(native_stop_keeping(kreal, "A") != 0, "a kept folder that can't be read: still there");
    chmod(kreal, 0400);
    CHECK(native_stop_keeping(kreal, "A") != 0, "a kept folder whose names can't be looked at: still there");
    chmod(kreal, 0755);
    snprintf(p, sizeof p, "%s/kept-file", scratch);
    x = fopen(p, "w");
    fclose(x);
    CHECK(native_stop_keeping(p, "A") == 0, "kept a plain file: nothing kept (0)");

    /* config.ini's writer, by hand (fixtures_test.c compares it with Swift's where Swift can write) */
    static const native_config_pair limits[2] = {{"max_gb", "1.5"}, {"max_days", "30"}}, model1[1] = {{"rekordbox_model", "1"}};
    struct {
        const char *in, *want;
        const native_config_pair *v;
        int nv;
    } cases[] = {
        {NULL, "# RB Stems Plus settings (RB Stems Plus \xe2\x80\xba Settings\xe2\x80\xa6), read by Stems Cache when rekordbox starts.\nmax_gb=1.5\nmax_days=30\n",
         limits, 2},
        {"[cache]\nmax_gb=2\n", "max_days=30\n[cache]\nmax_gb=1.5\n", limits, 2},
        {"\xef\xbb\xbfx=1", "\xef\xbb\xbfx=1\nrekordbox_model=1\n", model1, 1},
        {"[other]\nrekordbox_model=0\n", "rekordbox_model=1\n[other]\nrekordbox_model=0\n", model1, 1},
        {"", "rekordbox_model=1\n", model1, 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *o = NULL;
        size_t on = 0;
        int r = native_config_rewrite(cases[i].in, cases[i].in ? strlen(cases[i].in) : 0, cases[i].v, cases[i].nv, NULL, 0, &o, &on);
        CHECK(!r && on == strlen(cases[i].want) && !memcmp(o, cases[i].want, on), "config case %zu: \"%.*s\"", i, (int)on, o ? o : "");
        free(o);
    }
    CHECK(!native_config_sets("[other]\nmax_gb=1\n", 17, "max_gb") && native_config_sets("MAX_GB = 7\n", 11, "max_gb")
              && !native_config_sets(NULL, 0, "max_gb") && native_config_readable("x=1\n", 4) && !native_config_readable("x\0", 2)
              && !native_config_readable("\xff", 1),
          "configSets, readable");

    /* review S46: CONFIG_WRITE's arguments as pairs */
    {
        native_config_pair pr[8];
        char nums[8][32];
        const char *good[] = {"max_gb", "1.50", "max_days", "30", "rekordbox_model", "1"};
        CHECK(native_config_pairs(good, 6, pr, nums) == 3 && !strcmp(pr[0].key, "max_gb") && !strcmp(pr[0].value, "1.5")
                  && !strcmp(pr[1].value, "30") && !strcmp(pr[2].key, "rekordbox_model") && !strcmp(pr[2].value, "1"),
              "pairs: numbers as configNumber writes them (1.50 -> 1.5, 30), the model as given");
        const char *hex[] = {"max_gb", "0x10"}, *exp1[] = {"max_days", "1e1"};
        CHECK(native_config_pairs(hex, 2, pr, nums) == 1 && !strcmp(pr[0].value, "16") && native_config_pairs(exp1, 2, pr, nums) == 1
                  && !strcmp(pr[0].value, "10"),
              "pairs: hex and an exponent read as Double(value) reads them (16, 10)");
        const char *odd[] = {"max_gb", "1", "max_days"}, *unknown[] = {"cache_dir", "/x"}, *space[] = {"max_gb", " 5"},
                   *trail[] = {"max_gb", "5 "}, *nan_[] = {"max_gb", "nan"}, *inf_[] = {"max_days", "1e999"}, *empty[] = {"max_gb", ""},
                   *upper[] = {"MAX_GB", "1"}, *nil[] = {"max_gb", NULL};
        CHECK(native_config_pairs(odd, 3, pr, nums) == -1 && native_config_pairs(odd, 0, pr, nums) == -1 && native_config_pairs(NULL, 2, pr, nums) == -1,
              "pairs: an odd count, none, no arguments: -1");
        CHECK(native_config_pairs(unknown, 2, pr, nums) == -1 && native_config_pairs(upper, 2, pr, nums) == -1, "pairs: a key not written so: -1");
        CHECK(native_config_pairs(space, 2, pr, nums) == -1 && native_config_pairs(trail, 2, pr, nums) == -1 && native_config_pairs(empty, 2, pr, nums) == -1,
              "pairs: a number with a space before or after, or none: -1");
        CHECK(native_config_pairs(nan_, 2, pr, nums) == -1 && native_config_pairs(inf_, 2, pr, nums) == -1, "pairs: nan, and 1e999 (infinite): -1");
        CHECK(native_config_pairs(nil, 2, pr, nums) == -1, "pairs: a missing value: -1");
        const char *eight[16], *nine[18];
        for (int i = 0; i < 18; i += 2) {
            if (i < 16) eight[i] = "rekordbox_model", eight[i + 1] = "1";
            nine[i] = "max_days", nine[i + 1] = "3";
        }
        CHECK(native_config_pairs(eight, 16, pr, nums) == 8 && native_config_pairs(nine, 18, pr, nums) == -1, "pairs: 8 at most, 9: -1");
    }

    /* review S42: never a config.ini over CFG_MAX (65,536 bytes) */
    char *big = malloc(65536 + 64), *o = NULL;
    size_t on = 0;
    if (!big) abort();
    for (int i = 0; i < 65536; i += 2) memcpy(big + i, "#\n", 2);
    CHECK(native_config_rewrite(big, 65536, model1, 1, NULL, 0, &o, &on) == -1 && !o, "65,536 bytes and an insert: -1");
    memcpy(big, "max_gb=1\n", 9);
    for (int i = 9; i < 65535; i++) big[i] = '#';
    big[65535] = '\n';
    CHECK(native_config_rewrite(big, 65536, limits, 1, NULL, 0, &o, &on) == -1, "a replacement growing it past the limit: -1");
    memset(big, '#', 65534);
    memcpy(big + 65534, "\r\n", 2);
    static const char *const loc[1] = {"cache_dir"};
    CHECK(native_config_rewrite(big, 65536, NULL, 0, loc, 1, &o, &on) == -1, "nothing changed but Swift's extra newline after a CRLF: -1");
    memset(big, '#', 65517);
    big[65517] = '\n';
    CHECK(native_config_rewrite(big, 65518, model1, 1, NULL, 0, &o, &on) == 0 && on == 65536, "exactly 65,536 after the write: 0 (%zu)", on);
    free(o);
    free(big);
}

/* LAUNCH_AGENT_UNLOAD (native_watcher_unload) on fakes, over a real apps folder */
typedef struct {
    char calls[2048];
    int run_rc, off;
    const char *list_fails;   /* stop_watchers answers -1 for this folder */
} unload_fake;
static unload_fake UF;
static void uf_add(const char *s) {
    strncat(UF.calls, s, sizeof UF.calls - strlen(UF.calls) - 1);
    strncat(UF.calls, ";", sizeof UF.calls - strlen(UF.calls) - 1);
}
static int uf_run_remove(void *ctx) {
    (void)ctx;
    uf_add("run");
    return UF.run_rc;
}
static int uf_stop(void *ctx, const char *dir) {
    (void)ctx;
    const char *b = strrchr(dir, '/');
    b = b ? b + 1 : dir;
    char line[256];
    snprintf(line, sizeof line, "stop %s", b);
    uf_add(line);
    if (UF.list_fails && !strcmp(b, UF.list_fails)) return -1;
    return !strcmp(b, "1.5.0");
}
static int uf_off(void *ctx) {
    (void)ctx;
    uf_add("off");
    return UF.off;
}
static void uf_log(void *ctx, const char *line) {
    (void)ctx;
    char b[600];
    snprintf(b, sizeof b, "log %s", line);
    uf_add(b);
}

/* a folder under scratch whose path is about 780 bytes, holding one folder with a 250-byte name
 * (made through mkdirat: its full path is longer than PATH_MAX), for the paths that don't fit (review
 * UN16). 0 and its path in out, or -1 */
int test_long_dir(const char *scratch, const char *tag, char *out, size_t n) {
    char seg[201], name[251];
    memset(seg, 'd', 200);
    seg[200] = 0;
    memset(name, 'v', 250);
    name[250] = 0;
    int k = snprintf(out, n, "%s/%s", scratch, tag);
    if (k < 0 || (size_t)k >= n || (mkdir(out, 0755) && errno != EEXIST)) return -1;
    while (strlen(out) < 780) {
        size_t l = strlen(out), take = 780 - l < 200 ? 780 - l : 200;
        if (l + 1 + take >= n) return -1;
        out[l] = '/';
        memcpy(out + l + 1, seg, take);
        out[l + 1 + take] = 0;
        if (mkdir(out, 0755) && errno != EEXIST) return -1;
    }
    int fd = open(out, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int r = mkdirat(fd, name, 0755);
    int e = errno;
    close(fd);
    return r && e != EEXIST ? -1 : 0;
}

static void test_watcher_unload(const char *scratch) {
    char apps[1200], p[1300];
    snprintf(apps, sizeof apps, "%s/unload-apps", scratch);
    mkdir(apps, 0755);
    snprintf(p, sizeof p, "%s/1.5.0", apps);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/1.4.0", apps);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/current", apps);
    FILE *x = fopen(p, "w");
    if (x) {
        fputs("1.5.0\n", x);
        fclose(x);
    }
    snprintf(p, sizeof p, "%s/link", apps);
    (void)symlink("1.5.0", p);
    native_unload_ops ops = {uf_run_remove, uf_stop, uf_off, NULL, NULL};
    int off = 7;

    memset(&UF, 0, sizeof UF);
    CHECK(native_watcher_unload(&ops, apps, &off) == 0 && off == 0, "unloaded: 0, the reminders on (%d)", off);
    CHECK(!strncmp(UF.calls, "run;", 4), "the Run value first, so a logon meanwhile starts nothing: %s", UF.calls);
    CHECK(strstr(UF.calls, "stop 1.5.0;") && strstr(UF.calls, "stop 1.4.0;") && !strstr(UF.calls, "stop current") && !strstr(UF.calls, "stop link"),
          "every version folder, never a file or a link: %s", UF.calls);
    CHECK(strstr(UF.calls, ";off;") && !strcmp(UF.calls + strlen(UF.calls) - 4, "off;"), "the reminders read last: %s", UF.calls);

    memset(&UF, 0, sizeof UF);
    UF.run_rc = EACCES;
    UF.off = 1;
    CHECK(native_watcher_unload(&ops, apps, &off) == 1 && off == 1 && strstr(UF.calls, "stop 1.5.0;"),
          "the Run value stays: 1, the watchers still stopped, off read");
    memset(&UF, 0, sizeof UF);
    UF.list_fails = "1.4.0";
    UF.off = -1;
    CHECK(native_watcher_unload(&ops, apps, &off) == 1 && off == -1, "a process list unreadable: 1, the state unknown: -1");
    memset(&UF, 0, sizeof UF);
    UF.off = 3;
    CHECK(native_watcher_unload(&ops, apps, &off) == 0 && off == -1, "an odd answer for the state: unknown");
    memset(&UF, 0, sizeof UF);
    CHECK(native_watcher_unload(&ops, NULL, &off) == 0 && !strcmp(UF.calls, "run;off;"), "no apps folder: only the Run value: %s", UF.calls);
    memset(&UF, 0, sizeof UF);
    snprintf(p, sizeof p, "%s/none", scratch);
    CHECK(native_watcher_unload(&ops, p, &off) == 0 && !strcmp(UF.calls, "run;off;"), "an apps folder not there: nothing to stop");

    /* review UN16: an apps folder there but unreadable, and a version folder whose path doesn't fit */
    native_unload_ops lops = {uf_run_remove, uf_stop, uf_off, uf_log, NULL};
    if (geteuid() != 0) {
        memset(&UF, 0, sizeof UF);
        chmod(apps, 0);
        CHECK(native_watcher_unload(&lops, apps, &off) == 1 && strstr(UF.calls, "log watcher: ") && strstr(UF.calls, " couldn't be read;")
                  && !strstr(UF.calls, "stop "),
              "an apps folder that can't be opened: 1, logged: %s", UF.calls);
        chmod(apps, 0755);
    }
    char deep[1100];
    if (!test_long_dir(scratch, "unload-long", deep, sizeof deep)) {
        memset(&UF, 0, sizeof UF);
        CHECK(native_watcher_unload(&lops, deep, &off) == 1 && strstr(UF.calls, "log watcher: the running watchers couldn't all be checked;")
                  && !strstr(UF.calls, "stop "),
              "a version folder whose path doesn't fit: not stopped, counted as unchecked: %s", UF.calls);
    } else CHECK(0, "the overlong version folder couldn't be made");
}

/* LAUNCH_AGENT_LOAD, WATCHER_CHECK, WATCHER_STATE (native_watcher_*) on fakes */
static struct {
    char calls[1024];
    int read_rc, present, ours, off, write_rc, running, start_rc;
} WF;
static void wf_add(const char *s) {
    strncat(WF.calls, s, sizeof WF.calls - strlen(WF.calls) - 1);
    strncat(WF.calls, ";", sizeof WF.calls - strlen(WF.calls) - 1);
}
static int wf_read(void *ctx, int *present, int *ours, int *off) {
    (void)ctx;
    wf_add("read");
    *present = WF.present;
    *ours = WF.ours;
    *off = WF.off;
    return WF.read_rc;
}
static int wf_write(void *ctx) {
    (void)ctx;
    wf_add("write");
    if (!WF.write_rc) WF.present = WF.ours = 1;   /* StartupApproved (off) stays the user's */
    return WF.write_rc;
}
static int wf_running(void *ctx) {
    (void)ctx;
    wf_add("running");
    return WF.running;
}
static int wf_start(void *ctx) {
    (void)ctx;
    wf_add("start");
    return WF.start_rc;
}
static void wf_log(void *ctx, const char *line) {
    (void)ctx;
    char b[600];
    snprintf(b, sizeof b, "log %s", line);
    wf_add(b);
}

static void test_watcher_load(void) {
    native_watcher_ops ops = {wf_read, wf_write, wf_running, wf_start, wf_log, NULL};
    int off = 7;
    /* the complete uninstall's or Demucs v4's uninstall removed the Run value: an install puts it back
     * and starts the watcher (the bug: LOAD wasn't there, so no watcher until the next bootstrap) */
    memset(&WF, 0, sizeof WF);
    CHECK(native_watcher_load(&ops, &off) == 0 && off == 0, "no Run value: written, started, reminders on (%d)", off);
    CHECK(!strncmp(WF.calls, "read;write;log watcher: installed", 33) && strstr(WF.calls, "read;running;start;log watcher: started;"),
          "the Run value first, then the watcher started: %s", WF.calls);
    memset(&WF, 0, sizeof WF);
    WF.present = WF.ours = 1;
    WF.running = 1;
    CHECK(native_watcher_load(&ops, &off) == 0 && !strstr(WF.calls, "write") && !strstr(WF.calls, "start"), "ours and running: nothing to do: %s",
          WF.calls);
    memset(&WF, 0, sizeof WF);
    WF.present = WF.ours = WF.off = 1;
    CHECK(native_watcher_load(&ops, &off) == 0 && off == 1 && !strstr(WF.calls, "write;") && !strstr(WF.calls, "start;")
              && strstr(WF.calls, "log watcher: turned off in Startup apps: not started;"),
          "ours and off: left alone: %s", WF.calls);
    memset(&WF, 0, sizeof WF);
    WF.present = 1;   /* another stub's */
    WF.off = 1;
    CHECK(native_watcher_load(&ops, &off) == 0 && off == 1 && strstr(WF.calls, "write;") && !strstr(WF.calls, "start;"),
          "a value not ours is rewritten; the user's off still prevents starting: %s", WF.calls);
    memset(&WF, 0, sizeof WF);
    WF.write_rc = EACCES;
    WF.running = -1;
    CHECK(native_watcher_load(&ops, &off) == 0 && strstr(WF.calls, "COULD NOT BE INSTALLED") && !strstr(WF.calls, "start;") && off == 0,
          "a refused write is logged, never fails the flow; a list unread starts none: %s", WF.calls);
    memset(&WF, 0, sizeof WF);
    WF.present = WF.ours = 1;
    WF.start_rc = ENOENT;
    CHECK(native_watcher_load(&ops, &off) == 0 && strstr(WF.calls, "log watcher: started FAILED: "), "a start that fails is logged: %s", WF.calls);
    memset(&WF, 0, sizeof WF);
    WF.read_rc = EIO;
    CHECK(native_watcher_load(&ops, &off) == 0 && strstr(WF.calls, "write;") && strstr(WF.calls, "start;") && off == -1,
          "an unreadable value is written and the watcher still starts; the state unknown");

    /* checkWatcher: only a value of our name that starts something else is repaired */
    memset(&WF, 0, sizeof WF);
    CHECK(native_watcher_check(&ops, &off) == 0 && !strcmp(WF.calls, "read;read;") && off == 0, "no value: left alone (%s)", WF.calls);
    memset(&WF, 0, sizeof WF);
    WF.present = 1;
    WF.off = 1;
    CHECK(native_watcher_check(&ops, &off) == 0 && strstr(WF.calls, "write;log watcher: the Run value started something else") && off == 1,
          "another stub's: pointed at ours, the user's off kept: %s", WF.calls);
    memset(&WF, 0, sizeof WF);
    WF.present = WF.ours = 1;
    CHECK(native_watcher_check(&ops, &off) == 0 && !strstr(WF.calls, "write") && !strstr(WF.calls, "start"), "ours: kept, nothing started");
    memset(&WF, 0, sizeof WF);
    WF.present = 1;
    WF.ours = 1;
    WF.off = 1;
    CHECK(native_watcher_off(&ops) == 1, "turned off in Startup apps: off");
    WF.present = 0;
    CHECK(native_watcher_off(&ops) == 0, "no value: not off (nothing to turn off)");
    WF.read_rc = EIO;
    CHECK(native_watcher_off(&ops) == -1, "unreadable: unknown");
}

static void test_settings_facts(void) {
    native_settings o;
    native_settings_in in;
    memset(&in, 0, sizeof in);
    in.max_gb = 20;
    in.max_days = 60;
    in.this_pc_folder = 1;
    native_settings_facts(&in, &o);
    CHECK(!strcmp(o.gb, "20") && !strcmp(o.days, "60") && !o.unreadable && o.size_kind == 0 && o.clear == 1 && o.location == 0 && !o.label_path[0]
              && !o.drive[0],
          "This PC: default limits, no path or drive label, size measuring without a snapshot, local Clear available");
    in.this_pc_folder = 0;
    in.max_gb = 1.5;
    native_settings_facts(&in, &o);
    CHECK(o.clear == 2 && !strcmp(o.gb, "1.5"), "This PC without its folder: 2; 1.5 GB as typed");
    in.unreadable = 1;
    native_settings_facts(&in, &o);
    CHECK(o.unreadable && o.location == 2 && o.size_kind == -1 && o.clear == 0, "config.ini unreadable: 2, no size, nothing to clear");
    in.unreadable = 0;
    in.location = 2;
    in.this_pc_folder = 1;
    native_settings_facts(&in, &o);
    CHECK(o.location == 3 && o.size_kind == -1 && o.clear == 1, "set wrongly: 3, no size, This PC's old stems may be cleared");
    in.location = 1;
    in.dir = "E:\\Music\\DJ\\RB Stems Plus";
    in.drive = "SAMSUNG";
    in.can_choose = 1;
    native_settings_facts(&in, &o);
    CHECK(o.location == 1 && !strcmp(o.label_path, "Music/DJ") && !strcmp(o.drive, "SAMSUNG") && o.clear == 0 && o.size_kind == 0 && o.can_choose,
          "a chosen folder: parent label (%s) and drive name, size measuring without a snapshot, Choose available and Clear disabled", o.label_path);
    in.dir = "E:\\RB Stems Plus\\";
    native_settings_facts(&in, &o);
    CHECK(!o.label_path[0], "a folder at the drive's root: no folders between (%s)", o.label_path);
    in.dir = "C:\\mnt\\usb\\Stems\\RB Stems Plus";
    in.mount = "C:\\mnt\\usb\\";
    native_settings_facts(&in, &o);
    CHECK(!strcmp(o.label_path, "Stems"), "a volume mounted in a folder: below its mount point (%s)", o.label_path);
    in.dir = "\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}\\Stems\\RB Stems Plus";
    in.mount = "";
    native_settings_facts(&in, &o);
    CHECK(!strcmp(o.label_path, "Stems"), "a GUID path without a mount: below its volume root (%s)", o.label_path);
    in.dir = "E:/Music/DJ/RB Stems Plus";
    in.mount = NULL;
    native_settings_facts(&in, &o);
    CHECK(!strcmp(o.label_path, "Music/DJ"), "a forward-slash path still skips the drive letter (%s)", o.label_path);
}

static void t_r1_assessed(void) {
    static inv_list inventory = {.n = 2};
    strcpy(inventory.it[0].folder, "/rekordbox/rekordbox 7.2.20");
    strcpy(inventory.it[1].folder, "/rekordbox/rekordbox 7.2.19");
    inv_assess_row rows[] = {{.r1 = NF_R1_NOTHING}, {.r1 = NF_R1_ORT_MISSING}};
    CHECK(native_r1_assessed(&inventory, rows, inventory.it[1].folder) == NF_R1_ORT_MISSING,
          "R1 explicitly selects the older broken installation");
    CHECK(native_r1_assessed(&inventory, rows, "/other/rekordbox 7.2.19") == -1, "R1 rejects an unlisted path");
    inventory.it[1].trust = INV_STALE;
    CHECK(native_r1_assessed(&inventory, rows, inventory.it[1].folder) == -1, "R1 skips stale entries");
}

static void t_lane_a_pc_cache_create(const char *scratch);
static void t_pc_cache_identity_deadline(const char *scratch);
static void lane_b_support_artifacts(const char *scratch);
static void lane_b_pending_cleanup(const char *scratch);
static void lane_b_whole_removal(const char *scratch);
static void lane_b_held_descendant(const char *scratch);

void test_native_steps(const char *scratch) {
    t_lane_a_pc_cache_create(scratch);
    t_pc_cache_identity_deadline(scratch);
    t_r1_assessed();
    test_watcher_unload(scratch);
    test_watcher_load();
    test_settings_facts();
    static const char A64[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    native_steps_env e = {f_fetch, f_saved, f_seen, f_age, f_sha, f_ort, f_set_bridge, f_log, f_download, f_procs, f_size, f_free, f_same,
                          NULL, scratch, "1.5.0", "C:\\m\\hdemucs.onnx", NULL, NULL, f_blocked, f_rm, f_rmdir};
    native_flow fl;
    native_flow_init(&fl);
    native_payload_facts o;
    memset(&F, 0, sizeof F);
    F.bridge_sha = A64;
    F.ort = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    F.age = 120;
    manifest(&F.fresh, "1.5.1", "1.5.1", 'a');
    manifest(&F.saved, "1.5.0", "1.5.0", 'a');

    /* the bridge's place */
    char p[1200], want[1200];
    CHECK(!native_bridge_path(scratch, &F.fresh, p, sizeof p), "the bridge's path");
    snprintf(want, sizeof want, "%s/payload/1.5.1/rbstemsplus-bridge-win-x64.dll", scratch);
    CHECK(!strcmp(p, want), "support/payload/<version>/<file>: %s", p);
    payload_manifest none;
    memset(&none, 0, sizeof none);
    CHECK(native_bridge_path(scratch, &none, p, sizeof p) && native_bridge_path(scratch, &F.fresh, p, 10), "no windows block, too long: -1");

    /* latest: downloaded and verified; its bridge not here yet */
    F.fetch_answer = NS_FETCH_OK;
    CHECK(!native_step_payload(&e, &fl, "latest", &o) && o.fetch == 0 && o.found && !strcmp(o.version, "1.5.1") && !strcmp(o.app_version, "1.5.1")
              && !strcmp(o.signed_by, "release") && o.bridge_sha[0] == 'a' && o.model_sha[0] == 'b' && !o.bridge_here && o.has_age && o.age == 120
              && !strcmp(o.step_path, "C:\\m\\hdemucs.onnx") && !o.has_flag,
          "latest: the fresh one's facts");
    CHECK(fl.has && !strcmp(fl.m.payload_version, "1.5.1") && F.seen == 1 && !strcmp(F.seen_version, "1.5.1"), "it is the flow's, and seen");
    /* its bridge downloaded: here */
    snprintf(p, sizeof p, "%s/payload", scratch);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/payload/1.5.1", scratch);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/payload/1.5.1/rbstemsplus-bridge-win-x64.dll", scratch);
    FILE *x = fopen(p, "wb");
    fputs("MZ bridge", x);
    fclose(x);
    CHECK(!native_step_payload(&e, &fl, "latest", &o) && o.bridge_here, "latest: its bridge here and verified");
    F.bridge_sha = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    CHECK(!native_step_payload(&e, &fl, "latest", &o) && !o.bridge_here, "a bridge that doesn't verify: not here");
    F.bridge_sha = A64;
    /* offline: the saved one becomes the flow's */
    F.fetch_answer = NS_FETCH_OFFLINE;
    F.has_saved = 1;
    CHECK(!native_step_payload(&e, &fl, "latest", &o) && o.fetch == 2 && o.found && !strcmp(o.version, "1.5.0") && !strcmp(fl.m.payload_version, "1.5.0"),
          "offline: the saved one");
    /* unverified, nothing saved: none; the flow has none */
    F.fetch_answer = NS_FETCH_UNVERIFIED;
    F.has_saved = 0;
    CHECK(!native_step_payload(&e, &fl, "latest", &o) && o.fetch == 1 && !o.found && !fl.has, "unverified, nothing saved: none, and no flow payload");
    /* verified but nothing for this app: 0, the saved one */
    F.fetch_answer = NS_FETCH_NO_NEWS;
    F.has_saved = 1;
    CHECK(!native_step_payload(&e, &fl, "latest", &o) && o.fetch == 2 && o.found && !strcmp(o.version, "1.5.0"), "no news: the saved one, fetch 2");
    /* saved */
    F.has_saved = 0;
    CHECK(!native_step_payload(&e, &fl, "saved", &o) && o.fetch == 2 && !o.found && !fl.has, "saved, none: none");
    F.has_saved = 1;
    CHECK(!native_step_payload(&e, &fl, "saved", &o) && o.found && fl.has && !strcmp(fl.m.payload_version, "1.5.0"), "saved: the flow's");
    /* fetch: never the flow's */
    F.fetch_answer = NS_FETCH_OK;
    CHECK(!native_step_payload(&e, &fl, "fetch", &o) && o.fetch == 0 && o.found && !strcmp(o.version, "1.5.1") && !strcmp(fl.m.payload_version, "1.5.0"),
          "fetch: its facts, the flow's unchanged");
    F.fetch_answer = NS_FETCH_OFFLINE;
    CHECK(!native_step_payload(&e, &fl, "fetch", &o) && o.fetch == 2 && !o.found, "fetch offline: none");
    F.fetch_answer = NS_FETCH_NO_NEWS;
    CHECK(!native_step_payload(&e, &fl, "fetch", &o) && o.fetch == 2 && !o.found, "fetch, no news: 2, none");
    F.fetch_answer = NS_FETCH_UNVERIFIED;
    CHECK(!native_step_payload(&e, &fl, "fetch", &o) && o.fetch == 1 && !o.found, "fetch unverified (or older): 1, none");
    /* check: manifestDue */
    int seen_null = F.seen_null;
    F.has_saved = 0;
    CHECK(!native_step_payload(&e, &fl, "check", &o) && o.has_flag && o.flag && F.seen_null == seen_null + 1, "check, none saved: due, the record's rule");
    F.has_saved = 1;
    CHECK(!native_step_payload(&e, &fl, "check", &o) && o.has_flag && !o.flag, "check, saved for this app, 2 min old: not due");
    F.blocked = 1;
    CHECK(!native_step_payload(&e, &fl, "check", &o) && o.has_flag && o.flag, "a feature off only for an unlisted version: due (blockedByVersionList)");
    F.blocked = 0;
    F.age = 86401;
    CHECK(!native_step_payload(&e, &fl, "check", &o) && o.flag, "a day old: due");
    F.age = 120;
    manifest(&F.saved, "1.4.0", "1.4.0", 'a');
    CHECK(!native_step_payload(&e, &fl, "check", &o) && o.flag, "from before this app: due");
    /* a payload.json written in the future: no age, and due (never clamped to 0) */
    F.age = -5;
    F.fetch_answer = NS_FETCH_OFFLINE;
    CHECK(!native_step_payload(&e, &fl, "latest", &o) && o.found && !o.has_age, "latest offline, age in the future: no age");
    CHECK(!native_step_payload(&e, &fl, "check", &o) && o.has_flag && o.flag, "check, age in the future: due");
    F.age = -1;
    F.blocked = 0;
    CHECK(native_manifest_due(-1, "1.5.0", "1.5.0", NULL, NULL) && !native_manifest_due(10, "1.5.0", "1.5.0", NULL, NULL)
              && !native_manifest_due(10, "1.5.0", "1.5.0", f_blocked, NULL) && !native_manifest_due(10, "1.6.0", "1.5.0", NULL, NULL)
              && native_manifest_due(10, "", "1.5.0", NULL, NULL),
          "manifestDue's rule");
    F.blocked = 1;
    CHECK(native_manifest_due(10, "1.5.0", "1.5.0", f_blocked, NULL), "manifestDue: blocked by the list");
    /* blocked is asked only when the other gates say "not due" (S30) */
    F.blocked_asks = 0;
    CHECK(native_manifest_due(-1, "1.5.0", "1.5.0", f_blocked, NULL) && native_manifest_due(86401, "1.5.0", "1.5.0", f_blocked, NULL)
              && native_manifest_due(10, "1.4.0", "1.5.0", f_blocked, NULL) && native_manifest_due(10, NULL, "1.5.0", f_blocked, NULL)
              && F.blocked_asks == 0,
          "due already: blocked never asked");
    CHECK(native_manifest_due(10, "1.5.0", "1.5.0", f_blocked, NULL) && F.blocked_asks == 1, "not due otherwise: asked once");
    F.blocked = 0;
    CHECK(native_step_payload(&e, &fl, "nope", &o) != 0 && native_step_payload(&e, &fl, NULL, &o) != 0, "another arg: refused");

    /* CACHE_PREPARE */
    char ort[81];
    F.fetch_answer = NS_FETCH_UNVERIFIED;
    F.has_saved = 0;
    native_step_payload(&e, &fl, "latest", &o);
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 1 && F.set_bridges == 0, "no flow payload: 1, nothing handed over");
    F.fetch_answer = NS_FETCH_OK;
    native_step_payload(&e, &fl, "latest", &o);
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 0 && F.set_bridges == 1 && !strcmp(F.bridge_path, p)
              && !strcmp(F.bridge_name, "rbstemsplus-bridge-win-x64.dll") && ort[0] == 'c' && strstr(F.logged, "Pioneer's ONNX Runtime: cccccccccccc"),
          "the verified bridge handed to the admin, ORT's sha (%s)", ort);
    F.bridge_sha = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 1 && F.set_bridges == 1, "a bridge that changed after its check: 1, not handed over");
    F.bridge_sha = A64;
    F.ort = "";
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 0 && !strcmp(ort, "unreadable"), "ORT unreadable: said so");
    F.set_bridge_rc = ENOMEM;
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 1, "the admin couldn't take it: 1");
    remove(p);
    F.set_bridge_rc = 0;
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 1, "the bridge gone: 1");

    /* ST8: a registered bridge never outlives its flow payload or a failed prepare */
    x = fopen(p, "wb");
    fputs("MZ bridge", x);
    fclose(x);
    F.fetch_answer = NS_FETCH_OK;
    native_step_payload(&e, &fl, "latest", &o);
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 0 && F.registered, "A prepared: registered");
    manifest(&F.fresh, "1.5.2", "1.5.2", 'a');
    native_step_payload(&e, &fl, "latest", &o);
    CHECK(!F.registered, "latest B: A's bridge cleared");
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 1 && !F.registered, "B's bridge not there: a failed prepare, nothing registered");
    manifest(&F.fresh, "1.5.1", "1.5.1", 'a');
    native_step_payload(&e, &fl, "latest", &o);
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 0 && F.registered, "A again: registered");
    F.has_saved = 1;
    native_step_payload(&e, &fl, "saved", &o);
    CHECK(!F.registered, "saved: cleared too");
    native_step_payload(&e, &fl, "latest", &o);
    native_step_cache_prepare(&e, &fl, ort);
    int cleared = F.cleared;
    native_step_payload(&e, &fl, "check", &o);
    native_step_payload(&e, &fl, "fetch", &o);
    CHECK(F.registered && F.cleared == cleared, "check and fetch never touch it");

    /* ST9: latest A, then fetch B: CACHE_PREPARE still hands over A */
    manifest(&F.fresh, "1.5.2", "1.5.2", 'a');
    native_step_payload(&e, &fl, "fetch", &o);
    CHECK(native_step_cache_prepare(&e, &fl, ort) == 0 && strstr(F.bridge_path, "1.5.1"), "fetch B leaves the flow's A: %s", F.bridge_path);
    /* check with a saved payload: seen(saved) once, never seen(NULL) */
    int seen = F.seen;
    seen_null = F.seen_null;
    native_step_payload(&e, &fl, "check", &o);
    CHECK(F.seen == seen + 1 && F.seen_null == seen_null && !strcmp(F.seen_version, F.saved.payload_version), "check: the saved one seen once, never NULL");
    remove(p);
    native_flow_free(&fl);
    test_native_steps_more(scratch);
    test_native_state(scratch);
    lane_b_support_artifacts(scratch);
    lane_b_pending_cleanup(scratch);
    lane_b_whole_removal(scratch);
    lane_b_held_descendant(scratch);
}

/* Lane B */
static void lane_b_support_artifacts(const char *scratch) {
    static const char *const dirs[] = {"kept", "originals", "admin-source", "admin-source/admin", "update", "Logs", "Logs/reports"};
    static const char *const files[] = {
        "kept/Set-1_a.kept", "kept/Set-1_a.manifest", ("originals/" SAVED_MODEL),
        "originals/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.engine",
        "originals/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.onnx.damaged",
        "admin-source/rbstemsplus-bridge-win-x64.dll", "admin-source/admin/RB Stems Plus.exe",
        "update/payload.json", "update/payload.json.sig", "Logs/bridge.log.1",
        "Logs/reports/RB Stems Plus report 2026-11-20 120003.zip",
    };
    static const char *const unknown[] = {
        "kept/My Notes.kept", "originals/personal.engine", "admin-source/other.dll",
        "admin-source/admin/personal.exe", "update/other.json", "Logs/watcher.log.1",
        "Logs/reports/other.zip", "Logs/reports/RB Stems Plus report 2026-11-20 notes.zip",
    };
    char sup[1100], p[1400];
    for (int keep = 0; keep < 2; keep++) {
        snprintf(sup, sizeof sup, "%s/lane-b-support-%d", scratch, keep);
        if (mkdir(sup, 0755)) abort();
        for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
            snprintf(p, sizeof p, "%s/%s", sup, dirs[i]);
            if (mkdir(p, 0755)) abort();
        }
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
            snprintf(p, sizeof p, "%s/%s", sup, files[i]);
            spit_n(p, 1);
        }
        if (keep) {
            for (size_t i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
                snprintf(p, sizeof p, "%s/%s", sup, unknown[i]);
                spit_n(p, 1);
            }
        }
        native_steps_env e = {.support = sup, .log = f_log, .remove_empty_dir_at = f_rmdir};
        CHECK(native_remove_own_folders(&e, sup, 0) == keep && there(scratch, keep ? "lane-b-support-1" : "lane-b-support-0") == keep,
              "F5: support remains exactly when unknown artifacts are present");
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
            CHECK(!there(sup, files[i]), "F5: uninstall removes %s", files[i]);
        if (keep) {
            for (size_t i = 0; i < sizeof unknown / sizeof unknown[0]; i++)
                CHECK(there(sup, unknown[i]), "F5: uninstall keeps %s", unknown[i]);
        }
    }
}

static void lane_b_pending_cleanup(const char *scratch) {
    char sup[1100], cache[1200], moved[1200], p[1400], pending[1400], identity[NS_CACHE_IDENTITY_MAX];
    snprintf(sup, sizeof sup, "%s/lane-b-pending", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    snprintf(moved, sizeof moved, "%s/moved", sup);
    snprintf(pending, sizeof pending, "%s/%s", sup, NS_PENDING_DELETION);
    if (mkdir(sup, 0755)) abort();
    native_steps_env e = {.support = sup, .log = f_log, .remove_tree_at = f_rm};
    native_pc_cache_create(&e, cache);
    if (native_pc_cache_identity(&e, cache, identity)) abort();
    native_steps_test_fault = f_fault;
    fault_op = NS_FAULT_OPEN;
    const int retry[] = {EACCES, EBUSY, ELOOP};
    for (size_t i = 0; i < sizeof retry / sizeof retry[0]; i++) {
        fault_name = cache;
        fault_rc = retry[i];
        int rc = native_this_pc_remove_stems(&e, cache, identity);
        char *saved = NULL;
        size_t len = 0;
        CHECK(rc == 1 && !fault_name && !plat_read_file(pending, sizeof identity, &saved, &len) &&
                  len == strlen(identity) && !memcmp(saved, identity, len),
              "F6: refused or transient open (%d) records the confirmed identity and retries", retry[i]);
        free(saved);
        fault_name = NULL;
        if (native_support_remove(sup, NS_PENDING_DELETION)) abort();
    }
    native_steps_test_fault = NULL;

    if (native_support_write(sup, NS_PENDING_DELETION, identity, strlen(identity))) abort();
    snprintf(p, sizeof p, "%s/%s", cache, NS_PC_MARKER);
    if (unlink(p)) abort();
    plat_stat before, after;
    if (plat_stat_path(pending, 0, &before)) abort();
    CHECK(native_this_pc_remove_stems(&e, cache, identity) == 1 &&
              !plat_stat_path(pending, 0, &after) && plat_id_same(before.id, after.id),
          "F6: an unverifiable identity leaves the existing retry record in place");
    if (rename(cache, moved) || symlink(moved, cache)) abort();
    CHECK(native_this_pc_remove_stems(&e, cache, identity) == 1 && there(sup, NS_PENDING_DELETION),
          "F6: a refused cache link leaves the retry record");
    if (unlink(cache)) abort();
    CHECK(native_this_pc_remove_stems(&e, cache, identity) == 0 && !there(sup, NS_PENDING_DELETION),
          "F6: a missing cache clears the retry record");

    native_pc_cache_create(&e, cache);
    char replacement[NS_CACHE_IDENTITY_MAX];
    if (native_pc_cache_identity(&e, cache, replacement) || !strcmp(replacement, identity)) abort();
    snprintf(p, sizeof p, "%s/%.*s", cache, 64, SAVED_MODEL);
    if (mkdir(p, 0755) || native_support_write(sup, NS_PENDING_DELETION, identity, strlen(identity))) abort();
    struct stat st;
    CHECK(native_this_pc_remove_stems(&e, cache, identity) == 0 && !there(sup, NS_PENDING_DELETION) && !stat(p, &st),
          "F6: a verified replacement clears the retry record and keeps the replacement's models");
}

static void lane_b_whole_removal(const char *scratch) {
    static const char id[] = "0e8a1d84-4672-4f70-8b0a-2c356a48d563";
    static const char rel[] = "lane-b-external/" PATHS_CACHE_FOLDER;
    char external[1300], sup[1100], cache[1200], path[1500], name[256], err[128], identity[NS_CACHE_IDENTITY_MAX];
    native_steps_env e = {.support = sup, .log = f_log, .remove_tree_at = f_rm, .remove_empty_dir_at = f_rmdir};
    make_location(scratch, "lane-b-external", id, external, sizeof external);
    plat_stat st;
    if (plat_stat_path(external, 0, &st)) abort();
    native_location_identity(st.id, identity);
    race_do = race_file;
    int rc = native_remove_location_under(&e, scratch, rel, id, 1, name, err, identity);
    race_do = NULL;
    CHECK(rc == 7 && there(external, "late-file") && there(external, ".rbstemsplus-cache"),
          "F2: external removal failure keeps the late entry and restores ownership");
    snprintf(path, sizeof path, "%s/late-file", external);
    if (unlink(path) || plat_stat_path(external, 0, &st)) abort();
    native_location_identity(st.id, identity);
    CHECK(!native_remove_location_under(&e, scratch, rel, id, 1, name, err, identity) && !there(scratch, rel),
          "F2: external removal can be authorized and retried");

    snprintf(sup, sizeof sup, "%s/lane-b-clear", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    if (mkdir(sup, 0755)) abort();
    native_pc_cache_create(&e, cache);
    if (native_pc_cache_identity(&e, cache, identity)) abort();
    int link;
    race_do = race_file;
    rc = native_clear_cache(&e, cache, identity, 1, &link);
    race_do = NULL;
    CHECK(rc == 1 && there(cache, "late-file") && !native_pc_cache_identity(&e, cache, identity),
          "F2: This PC removal failure keeps the late entry and restores ownership");
    snprintf(path, sizeof path, "%s/late-file", cache);
    if (unlink(path)) abort();
    CHECK(!native_clear_cache(&e, cache, identity, 1, &link) && !there(sup, "Cache"),
          "F2: This PC removal can be authorized and retried");

    snprintf(sup, sizeof sup, "%s/lane-b-unlink", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    if (mkdir(sup, 0755)) abort();
    native_pc_cache_create(&e, cache);
    if (native_pc_cache_identity(&e, cache, identity)) abort();
    native_steps_test_fault = f_fault;
    fault_op = NS_FAULT_UNLINK_MARKER;
    fault_name = NS_PC_MARKER;
    fault_rc = ENOENT;
    rc = native_clear_cache(&e, cache, identity, 1, &link);
    native_steps_test_fault = NULL;
    CHECK(rc == 1 && !fault_name && there(sup, "Cache"), "F2: a missing marker during unlink cannot report successful removal");
    fault_name = NULL;
}

static void lane_b_held_descendant(const char *scratch) {
    static const char id[] = "0e8a1d84-4672-4f70-8b0a-2c356a48d563";
    static const char rel[] = "lane-b-held-tree/" PATHS_CACHE_FOLDER;
    char loc[1300], p[1500], name[256], err[128];
    make_location(scratch, "lane-b-held-tree", id, loc, sizeof loc);
    snprintf(p, sizeof p, "%s/ancestor", loc);
    if (mkdir(p, 0755)) abort();
    snprintf(p, sizeof p, "%s/ancestor/in-use", loc);
    if (mkdir(p, 0755)) abort();
    snprintf(p, sizeof p, "%s/ancestor/in-use/stems", loc);
    spit_n(p, 1);
    snprintf(p, sizeof p, "%s/sibling", loc);
    spit_n(p, 1);
    native_steps_env e = {.log = f_log, .remove_tree_at = f_rm, .remove_empty_dir_at = f_rmdir};
    fault_op = NS_FAULT_OPEN;
    fault_name = "in-use";
    fault_rc = EBUSY;
    native_steps_test_hold_cache_tree = 1;
    native_steps_test_fault = f_fault;
    int rc = native_remove_location_under(&e, scratch, rel, id, 1, name, err, NULL);
    native_steps_test_fault = NULL;
    CHECK(rc == 7 && !fault_name && there(loc, "ancestor/in-use/stems") && there(loc, ".rbstemsplus-cache") && !there(loc, "sibling"),
          "L1: a failed hold keeps the entire descendant and its ancestors; siblings can still be removed");
    fault_name = NULL;
    native_steps_test_hold_cache_tree = 0;
}

/* Lane A, F1: native This PC initialization shares the bridge's ownership policy. */
static void t_lane_a_pc_cache_create(const char *scratch) {
    char support[NS_PATH_MAX], cache[NS_PATH_MAX], marker[NS_PATH_MAX], tmp[NS_PATH_MAX], data[NS_PATH_MAX];
    char identity[NS_CACHE_IDENTITY_MAX];
    struct stat st;
    mkp(support, sizeof support, scratch, "lane-a-support");
    snprintf(cache, sizeof cache, "%s/Cache", support);
    snprintf(marker, sizeof marker, "%s/this-pc-owner", cache);
    snprintf(tmp, sizeof tmp, "%s/.this-pc-owner.tmp", cache);
    snprintf(data, sizeof data, "%s/keep.txt", cache);
    native_steps_env e = {.support = support, .log = f_log, .remove_empty_dir_at = f_rmdir};

    if (mkdir(cache, 0755)) abort();
    native_pc_cache_create(&e, cache);
    CHECK(!native_pc_cache_identity(&e, cache, identity), "F1: native init recovers an empty slot");
    char part[NS_CACHE_IDENTITY_MAX];
    snprintf(part, sizeof part, "%s", identity);
    if (unlink(marker) && errno != ENOENT) abort();
    FILE *f = fopen(marker, "wb");
    if (!f || fwrite(part, 1, 5, f) != 5 || fclose(f)) abort();
    f = fopen(tmp, "wb");
    if (!f || fwrite(part, 1, 8, f) != 8 || fclose(f)) abort();
    native_pc_cache_create(&e, cache);
    CHECK(!native_pc_cache_identity(&e, cache, identity) && stat(tmp, &st) && errno == ENOENT,
          "F1: native init recovers a slot containing only partial marker files");

    if (unlink(marker) && errno != ENOENT) abort();
    spit_n(marker, 5);
    native_pc_cache_create(&e, cache);
    CHECK(native_pc_cache_identity(&e, cache, identity) && !stat(marker, &st) && st.st_size == 5,
          "F1: a reserved filename alone does not authorize recovery");
    if (unlink(marker) && errno != ENOENT) abort();
    spit_n(data, 12);
    spit_n(marker, 5);
    native_pc_cache_create(&e, cache);
    CHECK(native_pc_cache_identity(&e, cache, identity) && !stat(data, &st) && st.st_size == 12 &&
          !stat(marker, &st) && st.st_size == 5, "F1: native init leaves a populated unowned slot alone");

    if (unlink(data) || unlink(marker) || (unlink(tmp) && errno != ENOENT) || rmdir(cache)) abort();
    fault_op = NS_FAULT_OPEN_CREATED;
    fault_name = cache;
    fault_rc = EIO;
    native_steps_test_fault = f_fault;
    native_pc_cache_create(&e, cache);
    native_steps_test_fault = NULL;
    CHECK(!fault_name && stat(cache, &st) && errno == ENOENT, "F1: native init rolls back a newly created slot if ownership cannot be recorded");
    native_pc_cache_create(&e, cache);
    CHECK(!native_pc_cache_identity(&e, cache, identity), "F1: native init can retry after the failure");
}

static atomic_int pc_probe_started, pc_probe_release;

static int block_pc_cache_open(int op, const char *name) {
    if (op != NS_FAULT_OPEN || !strstr(name, "/pc-deadline-support/Cache")) return 0;
    atomic_store(&pc_probe_started, 1);
    for (int i = 0; i < 1000 && !atomic_load(&pc_probe_release); i++) plat_sleep_ms(1);
    atomic_store(&pc_probe_started, 0);
    return 0;
}

static void t_pc_cache_identity_deadline(const char *scratch) {
    char support[NS_PATH_MAX], cache[NS_PATH_MAX], identity[NS_CACHE_IDENTITY_MAX];
    snprintf(support, sizeof support, "%s/pc-deadline-support", scratch);
    if (mkdir(support, 0755)) abort();
    snprintf(cache, sizeof cache, "%s/Cache", support);
    native_steps_env e = {.support = support};
    native_pc_cache_create(&e, cache);
    if (native_pc_cache_identity(&e, cache, identity)) abort();
    native_steps_test_fault = block_pc_cache_open;
    int rc = native_pc_cache_identity_bounded(support, cache, plat_now_mono_ns() + 80000000, identity);
    CHECK(rc == NL_TIMEOUT && atomic_load(&pc_probe_started) && !identity[0],
          "R2: a blocked local ownership probe times out without granting a deletion identity (%d)", rc);
    atomic_store(&pc_probe_release, 1);
    native_steps_test_fault = NULL;
}
