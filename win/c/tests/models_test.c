/* models.c on plat_mac.c in a scratch folder: Models.swift's cases (SignedReleases' and the model
 * tests' shape): saving Pioneer's model before ours replaces it, putting it back, the order of
 * the saved originals, the refusals (a model RB Stems Plus doesn't know, a link, a change
 * meanwhile, a damaged copy), and that nothing but our own temporary files is ever removed. */
#include "check.h"

#include "../models.h"
#include "plat.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static char logged[4096];
static void m_log(void *ctx, const char *line) {
    (void)ctx;
    size_t have = strlen(logged);
    if (have + strlen(line) + 2 <= sizeof logged) snprintf(logged + have, sizeof logged - have, "%s|", line);
}
static const char *meanwhile;   /* will_replace writes this into rekordbox's model (NULL: nothing) */
static char model_path[1200];
static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    fputs(text, f);
    fclose(f);
}
/* CriticalWork (review MC3/MC4): the tests' own, so the process's stays open for the others; the
 * replace runs inside it (count 1), the save outside (count 0) */
static critical_work mwork;
static int replace_calls, replace_outside, save_calls, save_inside;
static void m_will(void *ctx) {
    (void)ctx;
    replace_calls++;
    if (mwork.count != 1) replace_outside++;
    if (meanwhile) put(model_path, meanwhile);
}
static void m_save_check(void *ctx) {
    (void)ctx;
    save_calls++;
    if (mwork.count != 0) save_inside++;
}
static int same(const char *path, const char *text) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char b[256];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return !strcmp(b, text);
}
static void sha_of(const char *text, char out[65]) {
    plat_sha256_ctx c;
    unsigned char s[32];
    plat_sha256_init(&c);
    plat_sha256_update(&c, text, strlen(text));
    plat_sha256_final(&c, s);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", s[i]);
}
static int exists(const char *dir, const char *name) {
    char p[1300];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return !lstat(p, &st);
}

/* a fresh layout under scratch/name: rekordbox's model folder with its engine version, our
 * payload folder with our model, and the support folder's originals path (not made yet) */
static char dir[1100], originals[1100], payload[1100], support_dir[1100];
static void layout(const char *scratch, const char *name, const char *engine) {
    char root[1100], p[1200];
    snprintf(root, sizeof root, "%s/%s", scratch, name);
    mkdir(root, 0755);
    snprintf(dir, sizeof dir, "%s/demucs3_model", root);
    mkdir(dir, 0755);
    snprintf(originals, sizeof originals, "%s/originals", root);
    snprintf(support_dir, sizeof support_dir, "%s", root);
    snprintf(payload, sizeof payload, "%s/payload", root);
    mkdir(payload, 0755);
    snprintf(p, sizeof p, "%s/model.onnx", payload);
    put(p, "OURS");
    if (engine) {
        snprintf(p, sizeof p, "%s/demucs3_ver.txt", dir);
        put(p, engine);
    }
    snprintf(model_path, sizeof model_path, "%s/hdemucs.onnx", dir);
    logged[0] = 0;
    meanwhile = NULL;
}

static void test_models_review(const char *scratch, models *m, const char *ours, const char *pioneer);
static void test_models_vu1(const char *scratch, models *m, const char *ours, const char *pioneer);
static void test_models_critical(const char *scratch, models *m, const char *ours, const char *pioneer);
static void test_models_md3(const char *scratch, models *m, const char *ours, const char *pioneer);
static void test_models_uninstall(const char *scratch, models *m);
static void test_models_engine_tag(const char *scratch, models *m, const char *ours);

void test_models(const char *scratch) {
    char ours[65], pioneer[65], pioneer2[65], other[65], why[128], p[1300];
    sha_of("OURS", ours);
    sha_of("PIONEER", pioneer);
    sha_of("PIONEER2", pioneer2);
    sha_of("SOMEONE ELSE", other);
    critical_init(&mwork);
    models m = {.dir = dir, .originals = originals, .log = m_log, .will_replace = m_will, .support = support_dir, .will_save = m_save_check,
                .critical = &mwork};
    models_add_ours(&m, MODELS_OURS_BUILTIN);
    models_add_ours(&m, ours);
    models_add_ours(&m, ours);
    models_add_ours(&m, "NOT-A-SHA");
    CHECK(m.nours == 2 && models_is_sha256(ours) && !models_is_sha256("ABC"), "ours: once each, sha256s only");

    /* no model yet: ours copied in, nothing saved */
    layout(scratch, "a", "0002\n");
    CHECK(models_install_ours(&m, payload, "model.onnx", ours, why) == MODELS_OK && same(model_path, "OURS")
              && strstr(logged, "Pioneer's model: none to save") && !exists(dir, ".hdemucs.new") && models_left_behind(&m) == 2,
          "no model: ours in place (%s)", logged);
    CHECK(models_restore(&m, why) == MODELS_NO_SAVED_COPY && same(model_path, "OURS") && !models_any_saved(&m),
          "restore with nothing saved over ours: no saved copy, ours stays (%s)", logged);

    /* Pioneer's model: saved with its engine version, then ours in its place; restore puts it back */
    layout(scratch, "b", "0002\n");
    put(model_path, "PIONEER");
    CHECK(models_install_ours(&m, payload, "model.onnx", ours, why) == MODELS_OK && same(model_path, "OURS"), "Pioneer's replaced (%s)", logged);
    snprintf(p, sizeof p, "%s/%s.onnx", originals, pioneer);
    CHECK(same(p, "PIONEER") && strstr(logged, "save Pioneer's model: saved"), "Pioneer's saved first");
    snprintf(p, sizeof p, "%s/%s.engine", originals, pioneer);
    CHECK(same(p, "0002\n"), "with its STEMS Engine version");
    CHECK(models_any_saved(&m) && models_any_verified(&m), "MODELS_INFO: saved and verified");
    logged[0] = 0;
    CHECK(models_install_ours(&m, payload, "model.onnx", ours, why) == MODELS_OK && !strstr(logged, "save Pioneer"), "ours again: nothing saved");
    CHECK(models_restore(&m, why) == MODELS_OK && same(model_path, "PIONEER") && !exists(dir, ".hdemucs.restore") && models_left_behind(&m) == 3,
          "Pioneer's back");
    logged[0] = 0;
    CHECK(models_restore(&m, why) == MODELS_OK && strstr(logged, "already in place"), "Pioneer's already in place");
    snprintf(p, sizeof p, "%s/%s.onnx", originals, pioneer);
    CHECK(same(p, "PIONEER"), "the saved copy kept");

    /* a model this app doesn't know (not ours, saved ones exist with the same engine): left alone */
    put(model_path, "SOMEONE ELSE");
    logged[0] = 0;
    CHECK(models_install_ours(&m, payload, "model.onnx", ours, why) == MODELS_UNKNOWN && same(model_path, "SOMEONE ELSE"), "unknown: left");
    CHECK(models_restore(&m, why) == MODELS_UNKNOWN && same(model_path, "SOMEONE ELSE"), "unknown: restore leaves it");
    snprintf(p, sizeof p, "%s/%s.onnx", originals, other);
    CHECK(!same(p, "SOMEONE ELSE"), "never saved as Pioneer's");
    /* a new STEMS Engine version: then it can be Pioneer's (rekordbox downloaded a new one) */
    snprintf(p, sizeof p, "%s/demucs3_ver.txt", dir);
    put(p, "0003");
    CHECK(models_restore(&m, why) == MODELS_OK && strstr(logged, "already in place"), "a new engine: taken for Pioneer's");

    /* the order: the current engine's first */
    layout(scratch, "c", "0002");
    put(model_path, "PIONEER");
    models_install_ours(&m, payload, "model.onnx", ours, why);   /* saves PIONEER (0002) */
    snprintf(p, sizeof p, "%s/demucs3_ver.txt", dir);
    put(p, "0010");
    char name[1300];
    snprintf(name, sizeof name, "%s/%s.onnx", originals, pioneer2);
    put(name, "PIONEER2");
    snprintf(name, sizeof name, "%s/%s.engine", originals, pioneer2);
    put(name, "0010\n");
    CHECK(models_restore(&m, why) == MODELS_OK && same(model_path, "PIONEER2"), "the current engine's original first");
    put(model_path, "OURS");
    put(p, "0001");
    CHECK(models_restore(&m, why) == MODELS_OK && same(model_path, "PIONEER2"), "else the highest engine version (0010 over 0002)");

    /* a damaged saved copy: the next one; all damaged: said so, ours stays, no temp left */
    put(model_path, "OURS");
    snprintf(name, sizeof name, "%s/%s.onnx", originals, pioneer2);
    put(name, "BROKEN");
    logged[0] = 0;
    CHECK(models_restore(&m, why) == MODELS_OK && same(model_path, "PIONEER") && strstr(logged, "didn't verify: trying the next one"),
          "a damaged copy skipped (%s)", logged);
    put(model_path, "OURS");
    snprintf(name, sizeof name, "%s/%s.onnx", originals, pioneer);
    put(name, "BROKEN TOO");
    CHECK(models_restore(&m, why) == MODELS_SAVED_DAMAGED && same(model_path, "OURS") && !exists(dir, ".hdemucs.restore"), "all damaged");
    CHECK(models_any_saved(&m) && !models_any_verified(&m), "MODELS_INFO: saved, none verified");
    /* saving over a damaged copy sets it aside first */
    put(model_path, "PIONEER");
    CHECK(models_install_ours(&m, payload, "model.onnx", ours, why) == MODELS_OK && same(name, "PIONEER") && strstr(logged, "is damaged: set aside"),
          "a damaged copy set aside, the good one saved");
    snprintf(p, sizeof p, "%s.damaged", name);
    CHECK(same(p, "BROKEN TOO"), "the damaged one kept aside");

    /* changed meanwhile: nothing replaced, no temp left */
    put(model_path, "OURS");
    meanwhile = "PIONEER";
    CHECK(models_restore(&m, why) == MODELS_CHANGED && same(model_path, "PIONEER") && !exists(dir, ".hdemucs.restore"), "restore: changed meanwhile");
    put(model_path, "OURS");
    meanwhile = "SOMETHING NEW";
    CHECK(models_install_ours(&m, payload, "model.onnx", ours, why) == MODELS_CHANGED && same(model_path, "SOMETHING NEW")
              && !exists(dir, ".hdemucs.new"),
          "install: changed meanwhile");
    meanwhile = NULL;

    /* our model not what the payload says: refused, rekordbox's untouched */
    put(model_path, "OURS");
    CHECK(models_install_ours(&m, payload, "model.onnx", pioneer, why) == MODELS_COPY_BAD && same(model_path, "OURS") && !exists(dir, ".hdemucs.new"),
          "a copy that doesn't verify");
    CHECK(models_install_ours(&m, payload, "missing.onnx", ours, why) == MODELS_NOT_COPIED_WHY && why[0], "no source: the OS error (%s)", why);

    /* a link where the model is: never followed */
    layout(scratch, "d", "0002");
    char target[1300];
    snprintf(target, sizeof target, "%s/elsewhere.onnx", payload);
    put(target, "PIONEER");
    CHECK(!symlink(target, model_path), "a linked model");
    CHECK(models_install_ours(&m, payload, "model.onnx", ours, why) == MODELS_NOT_A_FILE && same(target, "PIONEER"), "install: a link isn't a file");
    CHECK(models_restore(&m, why) == MODELS_UNREADABLE && models_left_behind(&m) == 1, "restore: a link can't be read");
    unlink(model_path);
    /* a linked originals folder: nothing read or saved through it */
    char real[1200];
    snprintf(real, sizeof real, "%s/d/real-originals", scratch);
    mkdir(real, 0755);
    snprintf(name, sizeof name, "%s/%s.onnx", real, pioneer);
    put(name, "PIONEER");
    CHECK(!symlink(real, originals), "linked originals");
    put(model_path, "OURS");
    CHECK(!models_any_saved(&m) && models_restore(&m, why) == MODELS_NO_SAVED_COPY && same(model_path, "OURS"), "linked originals: not used");

    /* REMOVE_LEFTOVERS: only the temporary files, only regular files */
    layout(scratch, "f", "0002");
    snprintf(p, sizeof p, "%s/.hdemucs.new", dir);
    put(p, "half");
    snprintf(p, sizeof p, "%s/.hdemucs.restore", dir);
    mkdir(p, 0755);   /* a folder of that name: not ours to delete */
    mkdir(originals, 0755);
    snprintf(name, sizeof name, "%s/%s.onnx.part", originals, pioneer);
    put(name, "half");
    snprintf(p, sizeof p, "%s/%s.onnx", originals, pioneer2);
    put(p, "PIONEER2");
    char odd[1300];
    snprintf(odd, sizeof odd, "%s/notasha.onnx.part", originals);
    put(odd, "x");
    char lnk[1300];
    snprintf(lnk, sizeof lnk, "%s/%s.onnx.part", originals, other);
    CHECK(!symlink(p, lnk), "a linked part");
    logged[0] = 0;
    CHECK(models_remove_leftovers(&m) == 2 && !exists(dir, ".hdemucs.new") && exists(dir, ".hdemucs.restore") && access(name, F_OK) != 0
              && same(p, "PIONEER2") && same(odd, "x") && !access(lnk, F_OK) && strstr(logged, "leftover .hdemucs.new removed: ok"),
          "leftovers: the two temporary files went, nothing else (%s)", logged);
    CHECK(models_remove_leftovers(&m) == 0, "again: nothing");
    /* WF1: a run's own names too (base.<digits>.<digits>, the longest part name included), regular
     * files only; a link by such a name stays and its target is unchanged (WF8) */
    char wf[1400], wf_target[1300];
    snprintf(wf_target, sizeof wf_target, "%s/outside-target", payload);
    put(wf_target, "TARGET");
    const char *wf_gone[] = {".hdemucs.new.4242.987654321", ".hdemucs.restore.3.4"};
    for (int i = 0; i < 2; i++) {
        snprintf(wf, sizeof wf, "%s/%s", dir, wf_gone[i]);
        put(wf, "STALE");
    }
    char longest[1400];
    snprintf(longest, sizeof longest, "%s/%s.onnx.part.4294967295.18446744073709551615", originals, pioneer);
    put(longest, "STALE");
    snprintf(wf, sizeof wf, "%s/.hdemucs.new.1.2x", dir);
    put(wf, "KEEP");
    snprintf(wf, sizeof wf, "%s/.hdemucs.new.7.8", dir);
    mkdir(wf, 0755);
    const char *wf_links[] = {".hdemucs.restore.5.6", ".hdemucs.new.3.4"};
    int linked = 1;
    for (int i = 0; i < 2; i++) {
        snprintf(wf, sizeof wf, "%s/%s", dir, wf_links[i]);
        linked &= !symlink(wf_target, wf);
    }
    snprintf(wf, sizeof wf, "%s/%s.onnx.part.7.8", originals, pioneer);
    linked &= !symlink(wf_target, wf);
    snprintf(wf, sizeof wf, "%s/%s.onnx.part.1.2.3", originals, pioneer);
    put(wf, "KEEP");
    int wf_n = models_remove_leftovers(&m);
    char lp[1400];
    snprintf(lp, sizeof lp, "%s.onnx.part.7.8", pioneer);
    snprintf(wf, sizeof wf, "%s.onnx.part.1.2.3", pioneer);
    CHECK(linked && wf_n == 3 && !exists(dir, wf_gone[0]) && !exists(dir, wf_gone[1]) && access(longest, F_OK) != 0
              && exists(dir, ".hdemucs.new.1.2x") && exists(dir, ".hdemucs.new.7.8") && exists(dir, wf_links[0]) && exists(dir, wf_links[1])
              && exists(originals, lp) && exists(originals, wf) && same(wf_target, "TARGET"),
          "leftovers: a run's own names went (%d), other names, a folder and links (their target unchanged) kept", wf_n);

    /* no model folder: install fails with the OS error, restore has nothing to do */
    layout(scratch, "e", NULL);
    rmdir(dir);
    CHECK(models_install_ours(&m, payload, "model.onnx", ours, why) == MODELS_NOT_COPIED_WHY && models_restore(&m, why) == MODELS_OK
              && models_left_behind(&m) == 0,
          "no model folder");
    test_models_review(scratch, &m, ours, pioneer);
}

/* review MD1: a look at rekordbox's model that fails is never "none" */
static int stat_calls, stat_fail_mask;   /* bit k: the (k+1)th look fails with EACCES */
static int f_stat_fault(const char *name) {
    if (strcmp(name, "hdemucs.onnx")) return 0;
    int k = stat_calls++;
    return k < 30 && (stat_fail_mask >> k & 1) ? EACCES : 0;
}
static void m_save_changes(void *ctx) {
    (void)ctx;
    put(model_path, "CHANGED BEFORE THE COPY");
}
static void m_corrupt(void *ctx) {
    (void)ctx;
    put(model_path, "CORRUPTED AFTER THE RENAME");
}

static void m_move_restored_dir(void *ctx) {
    char moved[1200];
    snprintf(moved, sizeof moved, "%s.moved", dir);
    if (rename(dir, moved)) abort();
    if (ctx) {
        if (symlink(moved, dir)) abort();
    } else {
        if (mkdir(dir, 0755)) abort();
        put(model_path, "PIONEER");
    }
}

/* MD3: the regular files in d named base or base.<digits>.<digits> (models.c's temporary names) */
static int temps_of(const char *d, const char *base) {
    DIR *dp = opendir(d);
    if (!dp) return -1;
    int n = 0;
    size_t bl = strlen(base);
    struct dirent *e;
    while ((e = readdir(dp))) {
        const char *q = e->d_name;
        if (strncmp(q, base, bl)) continue;
        q += bl;
        int ok = 1;
        if (*q)
            for (int k = 0; k < 2; k++) {   /* exactly .<digits>.<digits> */
                if (*q != '.' || !isdigit((unsigned char)q[1])) {
                    ok = 0;
                    break;
                }
                q++;
                while (isdigit((unsigned char)*q)) q++;
            }
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        if (ok && !*q && !lstat(p, &st) && S_ISREG(st.st_mode)) n++;
    }
    closedir(dp);
    return n;
}
/* will_replace for MD3: the regular file named .hdemucs.new.<this pid>.<...> or .hdemucs.restore.<this
 * pid>.<...> in rekordbox's model folder just before the rename (none: seen_temp stays empty) */
static char seen_temp[256];
static void m_see_temp(void *ctx) {
    (void)ctx;
    char a[64], b[64], p[1400];
    snprintf(a, sizeof a, ".hdemucs.new.%d.", (int)getpid());
    snprintf(b, sizeof b, ".hdemucs.restore.%d.", (int)getpid());
    DIR *dp = opendir(dir);
    struct dirent *e;
    struct stat st;
    while (dp && (e = readdir(dp))) {
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if ((!strncmp(e->d_name, a, strlen(a)) || !strncmp(e->d_name, b, strlen(b))) && !lstat(p, &st) && S_ISREG(st.st_mode))
            snprintf(seen_temp, sizeof seen_temp, "%s", e->d_name);
    }
    if (dp) closedir(dp);
}

static char swap_from[1300], swap_to[1300];   /* MD5: will_save swaps support for a link to swap_to */
static void m_swap_support(void *ctx) {
    (void)ctx;
    char moved[1400];
    snprintf(moved, sizeof moved, "%s.moved", swap_from);
    if (rename(swap_from, moved) || symlink(swap_to, swap_from)) put(model_path, "SWAP FAILED");
}

/* the review's cases (MD1, MD7, MD9): outcomes and files, never only codes */
static void test_models_review(const char *scratch, models *m, const char *ours, const char *pioneer) {
    char why[128], p[1300], name[1300], before[1300];
    /* MD1: install (ours live) and restore (ours live, Pioneer's saved): a failing first, last or
     * both looks: never OK, the live bytes and the originals unchanged */
    for (int op = 0; op < 2; op++)
        for (int mask = 1; mask <= 3; mask++) {
            char sub[16];
            snprintf(sub, sizeof sub, "md1-%d-%d", op, mask);
            layout(scratch, sub, "0002");
            put(model_path, "OURS");
            mkdir(originals, 0755);
            snprintf(name, sizeof name, "%s/%s.onnx", originals, pioneer);
            put(name, "PIONEER");
            models_test_stat_fault = f_stat_fault;
            stat_calls = 0;
            stat_fail_mask = mask;
            int r = op ? models_restore(m, why) : models_install_ours(m, payload, "model.onnx", ours, why);
            models_test_stat_fault = NULL;
            CHECK(r != MODELS_OK && same(model_path, "OURS") && same(name, "PIONEER") && !exists(dir, ".hdemucs.new") && !exists(dir, ".hdemucs.restore"),
                  "%s, look mask %d: %d, nothing changed", op ? "restore" : "install", mask, r);
        }

    /* MD7: the model folder linked after an install: NOT_BACK, nothing changed, unreadable */
    layout(scratch, "md7", "0002");
    put(model_path, "PIONEER");
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_OK, "md7: installed");
    snprintf(before, sizeof before, "%s-real", dir);
    CHECK(!rename(dir, before) && !symlink(before, dir), "md7: the folder linked");
    snprintf(p, sizeof p, "%s/hdemucs.onnx", before);
    CHECK(models_restore(m, why) == MODELS_NOT_BACK && same(p, "OURS") && models_left_behind(m) == 1, "md7: NOT_BACK, nothing changed, unreadable");

    /* MD9: Pioneer's live, a linked originals folder: NOT_SAVED, the model untouched */
    layout(scratch, "md9a", "0002");
    put(model_path, "PIONEER");
    char outside[1300];
    snprintf(outside, sizeof outside, "%s/md9a/outside", scratch);
    mkdir(outside, 0755);
    CHECK(!symlink(outside, originals), "md9a: linked originals");
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_NOT_SAVED && same(model_path, "PIONEER"), "md9a: NOT_SAVED, untouched");
    snprintf(p, sizeof p, "%s/%s.onnx", outside, pioneer);
    CHECK(!same(p, "PIONEER"), "md9a: nothing saved through the link");
    /* the model changed between its hash and its copy: NOT_SAVED, no copy left */
    layout(scratch, "md9b", "0002");
    put(model_path, "PIONEER");
    m->will_save = m_save_changes;
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_NOT_SAVED && same(model_path, "CHANGED BEFORE THE COPY"),
          "md9b: changed before the copy: NOT_SAVED, ours not put in");
    m->will_save = m_save_check;
    snprintf(p, sizeof p, "%s.onnx", pioneer);
    snprintf(name, sizeof name, "%s.onnx.part", pioneer);
    CHECK(!exists(originals, p) && !exists(originals, name), "md9b: nothing saved, no part left");
    /* <sha>.onnx and <sha>.onnx.damaged both non-empty folders: NOT_SAVED, the model untouched */
    layout(scratch, "md9c", "0002");
    put(model_path, "PIONEER");
    mkdir(originals, 0755);
    snprintf(p, sizeof p, "%s/%s.onnx", originals, pioneer);
    mkdir(p, 0755);
    snprintf(name, sizeof name, "%s/x", p);
    put(name, "x");
    snprintf(p, sizeof p, "%s/%s.onnx.damaged", originals, pioneer);
    mkdir(p, 0755);
    snprintf(name, sizeof name, "%s/y", p);
    put(name, "y");
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_NOT_SAVED && same(model_path, "PIONEER") && same(name, "y"),
          "md9c: folders in the way: NOT_SAVED, nothing of them removed");
    /* MD5: a linked support folder: NOT_SAVED, the model untouched, nothing deleted or saved there */
    layout(scratch, "md5", "0002");
    put(model_path, "PIONEER");
    char real_sup[1300], part[1400];
    snprintf(real_sup, sizeof real_sup, "%s/md5/outside-support", scratch);
    mkdir(real_sup, 0755);
    snprintf(p, sizeof p, "%s/originals", real_sup);
    mkdir(p, 0755);
    snprintf(part, sizeof part, "%s/%s.onnx.part", p, pioneer);
    put(part, "SOMEONE'S FILE");
    snprintf(support_dir, sizeof support_dir, "%s/md5/linked-support", scratch);
    CHECK(!symlink(real_sup, support_dir), "md5: a linked support");
    snprintf(originals, sizeof originals, "%s/originals", support_dir);   /* as models_of: support's */
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_NOT_SAVED && same(model_path, "PIONEER"), "md5: NOT_SAVED, untouched");
    snprintf(name, sizeof name, "%s/%s.onnx", p, pioneer);
    CHECK(same(part, "SOMEONE'S FILE") && access(name, F_OK) != 0, "md5: the outside part kept, nothing placed there");
    CHECK(!models_any_saved(m) && !models_any_verified(m), "md5: nothing read through it either");
    /* MD5 read side: a good saved copy behind the link: never listed, never put back */
    layout(scratch, "md5r", "0002");
    put(model_path, "OURS");
    snprintf(real_sup, sizeof real_sup, "%s/md5r/outside-support", scratch);
    mkdir(real_sup, 0755);
    snprintf(p, sizeof p, "%s/originals", real_sup);
    mkdir(p, 0755);
    snprintf(name, sizeof name, "%s/%s.onnx", p, pioneer);
    put(name, "PIONEER");
    snprintf(support_dir, sizeof support_dir, "%s/md5r/linked-support", scratch);
    CHECK(!symlink(real_sup, support_dir), "md5r: a linked support");
    snprintf(originals, sizeof originals, "%s/originals", support_dir);
    CHECK(!models_any_saved(m) && !models_any_verified(m), "md5r: the copy behind the link isn't counted");
    CHECK(models_restore(m, why) == MODELS_NO_SAVED_COPY && same(model_path, "OURS"), "md5r: restore: not put back from behind the link");

    /* MD5 swap: support swapped for a link after it was opened: the engine file still goes through the
     * handle, the recheck refuses the link (a good copy is waiting behind it), nothing outside changes */
    layout(scratch, "md5s", "0002");
    put(model_path, "PIONEER");
    snprintf(support_dir, sizeof support_dir, "%s/md5s/sup", scratch);
    mkdir(support_dir, 0755);
    snprintf(originals, sizeof originals, "%s/originals", support_dir);
    snprintf(swap_from, sizeof swap_from, "%s", support_dir);
    snprintf(swap_to, sizeof swap_to, "%s/md5s/outside", scratch);
    mkdir(swap_to, 0755);
    snprintf(p, sizeof p, "%s/originals", swap_to);
    mkdir(p, 0755);
    snprintf(name, sizeof name, "%s/%s.onnx", p, pioneer);
    put(name, "PIONEER");
    snprintf(part, sizeof part, "%s/%s.engine", p, pioneer);
    put(part, "SENTINEL");
    m->will_save = m_swap_support;
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_NOT_SAVED && same(model_path, "PIONEER"), "md5s: swapped: NOT_SAVED, untouched");
    m->will_save = m_save_check;
    CHECK(same(part, "SENTINEL") && same(name, "PIONEER"), "md5s: nothing outside written");
    snprintf(name, sizeof name, "%s.moved/originals/%s.engine", support_dir, pioneer);
    CHECK(same(name, "0002\n"), "md5s: the engine file went through the handle");
    /* MD5 listing: a linked support's saved copies don't decide whether rekordbox's model is Pioneer's */
    layout(scratch, "md5l", "0002");
    put(model_path, "PIONEER");
    snprintf(real_sup, sizeof real_sup, "%s/md5l/outside-support", scratch);
    mkdir(real_sup, 0755);
    snprintf(p, sizeof p, "%s/originals", real_sup);
    mkdir(p, 0755);
    char other2[65];
    sha_of("SOMEONE ELSE", other2);
    snprintf(name, sizeof name, "%s/%s.onnx", p, other2);
    put(name, "SOMEONE ELSE");
    snprintf(name, sizeof name, "%s/%s.engine", p, other2);
    put(name, "0002");
    snprintf(support_dir, sizeof support_dir, "%s/md5l/linked-support", scratch);
    CHECK(!symlink(real_sup, support_dir), "md5l: a linked support");
    snprintf(originals, sizeof originals, "%s/originals", support_dir);
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_NOT_SAVED && same(model_path, "PIONEER"), "md5l: NOT_SAVED (not UNKNOWN from the copies behind the link)");

    /* MD13: equals ordered newest first, by distinct mtimes (not by name or listing order) */
    layout(scratch, "md13", "0002");
    put(model_path, "OURS");
    mkdir(originals, 0755);
    char older[1300], newer[1300], c_sha[65];
    sha_of("PIONEER C", c_sha);
    snprintf(older, sizeof older, "%s/%s.onnx", originals, pioneer);
    put(older, "PIONEER");
    snprintf(newer, sizeof newer, "%s/%s.onnx", originals, c_sha);
    put(newer, "PIONEER C");
    struct timeval t1[2] = {{1000, 0}, {1000, 0}}, t2[2] = {{2000, 0}, {2000, 0}};
    utimes(older, t1);
    utimes(newer, t2);
    CHECK(models_restore(m, why) == MODELS_OK && same(model_path, "PIONEER C"), "md13: the newest saved first");
    utimes(older, t2);
    utimes(newer, t1);
    put(model_path, "OURS");
    CHECK(models_restore(m, why) == MODELS_OK && same(model_path, "PIONEER"), "md13: the other way round too");
    /* install, restore, install: each outcome, Pioneer's kept once */
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_OK && same(model_path, "OURS") && same(older, "PIONEER"),
          "md13: install over it");
    CHECK(models_restore(m, why) == MODELS_OK && same(model_path, "PIONEER"), "md13: restore");
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_OK && same(model_path, "OURS") && same(older, "PIONEER"),
          "md13: install again: Pioneer's still the one saved");

    /* corrupted right after restore's rename: NOT_VERIFIED */
    layout(scratch, "md9d", "0002");
    put(model_path, "PIONEER");
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_OK, "md9d: installed");
    m->after_rename = m_corrupt;
    CHECK(models_restore(m, why) == MODELS_NOT_VERIFIED && same(model_path, "CORRUPTED AFTER THE RENAME"), "md9d: NOT_VERIFIED");
    m->after_rename = NULL;

    for (int link = 0; link < 2; link++) {
        layout(scratch, link ? "umr1-link" : "umr1-replacement", "0002");
        put(model_path, "PIONEER");
        CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_OK, "UMR1: installed");
        m->after_rename = m_move_restored_dir;
        m->ctx = link ? &link : NULL;
        CHECK(models_restore(m, why) == MODELS_NOT_BACK && same(model_path, "PIONEER"),
              "UMR1: NOT_BACK when the restored folder is replaced with %s", link ? "a link to the moved folder" : "a different folder with the same model");
        m->after_rename = NULL;
        m->ctx = NULL;
    }

    /* J1: a saved copy that can't be read is never "none verified" (Remove Anyway keeps it) */
    layout(scratch, "j1", "0002");
    put(model_path, "OURS");
    CHECK(!models_any_verified(m), "j1: no originals: none verified");
    mkdir(originals, 0755);
    char damaged[1300], locked[1300], other[65];
    snprintf(damaged, sizeof damaged, "%s/%s.onnx", originals, pioneer);
    put(damaged, "DAMAGED");
    CHECK(models_any_saved(m) && !models_any_verified(m), "j1: only a damaged copy: none verified");
    memset(other, 'a', 64);
    other[64] = 0;
    snprintf(locked, sizeof locked, "%s/%s.onnx", originals, other);
    put(locked, "CAN'T BE READ");
    chmod(locked, 0);
    CHECK(models_any_verified(m), "j1: a copy that can't be opened: counted (unknown, kept)");
    chmod(locked, 0644);
    unlink(locked);
    chmod(originals, 0);
    CHECK(models_any_verified(m), "j1: originals that can't be opened: counted (unknown, kept)");
    chmod(originals, 0755);
    CHECK(!models_any_verified(m), "j1: readable again: the damaged one alone, none verified");
    /* VU2: a <sha>.onnx that isn't a regular file (a link here; Windows lists reparse points as
     * the same kind): counted (unknown, kept); a folder of that name: not */
    char linked[1300], target[1300];
    snprintf(linked, sizeof linked, "%s/%s.onnx", originals, other);
    snprintf(target, sizeof target, "%s/j1/elsewhere", scratch);
    put(target, "SOMEWHERE");
    CHECK(!symlink(target, linked) && models_any_verified(m), "vu2: a <sha>.onnx that is a link: counted (unknown, kept)");
    unlink(linked);
    mkdir(linked, 0755);
    CHECK(!models_any_verified(m), "vu2: a folder named <sha>.onnx: none verified");
    rmdir(linked);
    /* VU3: a file named originals: none (it isn't a folder) */
    layout(scratch, "vu3", "0002");
    put(originals, "NOT A FOLDER");
    CHECK(!models_any_verified(m), "vu3: a file named originals: none verified");
    test_models_vu1(scratch, m, ours, pioneer);
}

/* one-shot faults (review VQ5, VQ2): only the listing or the originals open numbered so fails */
static int each_fail_at, open_fail_at;
static int f_each_fault(int call) { return call == each_fail_at; }
static int f_open_fault(int call) { return call == open_fail_at ? EIO : 0; }
static int f_loop_fault(int call) { return call == open_fail_at ? ELOOP : 0; }

/* VU1, VU4 and VU3's fault-hook cases: saved copies that can't be read are never decided on */
static void test_models_vu1(const char *scratch, models *m, const char *ours, const char *pioneer) {
    char why[128], saved_copy[1300], p[1300];
    /* originals that can't be opened, an unknown model on the saved copy's engine: restore fails, the copy stays */
    layout(scratch, "vu1a", "0002");
    put(model_path, "SOMEONE ELSE'S");
    mkdir(originals, 0755);
    snprintf(saved_copy, sizeof saved_copy, "%s/%s.onnx", originals, pioneer);
    put(saved_copy, "PIONEER");
    snprintf(p, sizeof p, "%s/%s.engine", originals, pioneer);
    put(p, "0002\n");
    chmod(originals, 0);
    int r = models_restore(m, why);
    CHECK(r == MODELS_NOT_BACK && models_any_saved(m) && models_any_verified(m), "vu1: originals unreadable: NOT_BACK (%d); saved and verified (unknown, kept)", r);
    chmod(originals, 0755);
    CHECK(same(saved_copy, "PIONEER") && same(model_path, "SOMEONE ELSE'S"), "vu1: the copy and the model untouched");
    /* ours live, originals unreadable: never "no saved copy" */
    put(model_path, "OURS");
    chmod(originals, 0);
    r = models_restore(m, why);
    chmod(originals, 0755);
    CHECK(r == MODELS_NOT_BACK && same(model_path, "OURS"), "vu1: ours live, originals unreadable: NOT_BACK, not NO_SAVED_COPY (%d)", r);
    /* a saved copy that can't be opened: NOT_BACK, nothing changed */
    chmod(saved_copy, 0);
    r = models_restore(m, why);
    chmod(saved_copy, 0644);
    CHECK(r == MODELS_NOT_BACK && same(model_path, "OURS") && same(saved_copy, "PIONEER"), "vu1: a copy that can't be opened: NOT_BACK (%d)", r);
    /* a <sha>.onnx that is a link: unknown, NOT_BACK */
    char other[65], linked[1300];
    memset(other, 'c', 64);
    other[64] = 0;
    snprintf(linked, sizeof linked, "%s/%s.onnx", originals, other);
    CHECK(!symlink(saved_copy, linked), "vu1: a linked <sha>.onnx");
    r = models_restore(m, why);
    unlink(linked);
    CHECK(r == MODELS_OK && same(model_path, "PIONEER"), "vu1: a good copy next to a linked <sha>.onnx: back (%d)", r);
    put(model_path, "OURS");
    /* VQ5: one listing per decision, and a one-shot failure on it: restore NOT_BACK, install over
     * Pioneer's NOT_SAVED, info saved and verified */
    models_test_each_fault = f_each_fault;
    int calls = models_test_each_calls;
    r = models_restore(m, why);
    CHECK(models_test_each_calls == calls + 1, "vq5: restore lists originals exactly once (%d)", models_test_each_calls - calls);
    put(model_path, "OURS");
    each_fail_at = models_test_each_calls + 1;
    r = models_restore(m, why);
    CHECK(r == MODELS_NOT_BACK && same(model_path, "OURS"), "vq5: the one listing fails: restore NOT_BACK (%d)", r);
    put(model_path, "PIONEER");
    calls = models_test_each_calls;
    each_fail_at = calls + 1;
    r = models_install_ours(m, payload, "model.onnx", ours, why);
    CHECK(r == MODELS_NOT_SAVED && same(model_path, "PIONEER") && models_test_each_calls == calls + 1,
          "vq5: the one listing fails: install NOT_SAVED, nothing changed, one listing (%d)", r);
    int saved = 0, verified = 0;
    calls = models_test_each_calls;
    models_originals_info(m, &saved, &verified);
    CHECK(models_test_each_calls == calls + 1, "vq5: MODELS_INFO lists originals exactly once (%d)", models_test_each_calls - calls);
    each_fail_at = 0;
    models_test_each_fault = NULL;
    /* MP6: a good copy next to one that can't be opened: back; with no live model too */
    char locked[1300], other2[65];
    memset(other2, 'd', 64);
    other2[64] = 0;
    snprintf(locked, sizeof locked, "%s/%s.onnx", originals, other2);
    put(locked, "LOCKED");
    chmod(locked, 0);
    put(model_path, "OURS");
    r = models_restore(m, why);
    CHECK(r == MODELS_OK && same(model_path, "PIONEER"), "mp6: ours live, a good copy and one that can't be opened: back (%d)", r);
    unlink(model_path);
    r = models_restore(m, why);
    chmod(locked, 0644);
    CHECK(r == MODELS_OK && same(model_path, "PIONEER") && same(locked, "LOCKED"), "mp6: no live model, good + unreadable: back, the other untouched (%d)", r);
    /* P9: an unknown live model with good + unreadable copies on another engine: NOT_BACK */
    snprintf(p, sizeof p, "%s/%s.engine", originals, pioneer);
    put(p, "0001\n");
    put(model_path, "SOMEONE ELSE'S");
    chmod(locked, 0);
    r = models_restore(m, why);
    CHECK(r == MODELS_NOT_BACK && same(model_path, "SOMEONE ELSE'S"), "p9: unknown live model, good + unreadable on another engine: NOT_BACK (%d)", r);
    /* VQ2 (P5, P7): install over an unknown model with an unreadable saved copy: NOT_SAVED, model unchanged */
    r = models_install_ours(m, payload, "model.onnx", ours, why);
    CHECK(r == MODELS_NOT_SAVED && same(model_path, "SOMEONE ELSE'S"), "p5/p7: unknown live, an unreadable copy: install NOT_SAVED (%d)", r);
    chmod(locked, 0644);
    unlink(locked);
    /* P6: the only other copy a non-regular <sha>.onnx: NOT_SAVED */
    CHECK(!symlink(saved_copy, locked), "p6: a linked <sha>.onnx");
    r = models_install_ours(m, payload, "model.onnx", ours, why);
    CHECK(r == MODELS_NOT_SAVED && same(model_path, "SOMEONE ELSE'S"), "p6: unknown live, a linked <sha>.onnx: install NOT_SAVED (%d)", r);
    /* VQ4: a lone non-regular <sha>.onnx is saved and verified (unknown, kept) */
    unlink(saved_copy);
    saved = verified = 0;
    models_originals_info(m, &saved, &verified);
    int saved_only = 0;
    models_originals_info(m, &saved_only, NULL);
    CHECK(saved && verified && saved_only && models_any_saved(m), "vq4: only a linked <sha>.onnx: saved and verified, saved alone too");
    unlink(locked);
    /* VQ2: a one-shot failure on the first originals open: install over an unknown model on another engine NOT_SAVED */
    put(saved_copy, "PIONEER");
    models_test_originals_fault = f_open_fault;
    open_fail_at = models_test_originals_calls + 1;
    r = models_install_ours(m, payload, "model.onnx", ours, why);
    CHECK(r == MODELS_NOT_SAVED && same(model_path, "SOMEONE ELSE'S"), "vq2: the first originals open fails: install NOT_SAVED (%d)", r);
    models_test_originals_fault = NULL;
    open_fail_at = 0;
    /* RG1: ELOOP at support's open while support is a plain folder (Redirection Guard refusing a
     * junction above it): unknown, not "none", so saved and verified */
    models_test_originals_fault = f_loop_fault;
    open_fail_at = models_test_originals_calls + 1;
    saved = verified = 0;
    models_originals_info(m, &saved, &verified);
    CHECK(saved && verified, "rg1: ELOOP above a plain support folder: saved and verified (unknown)");
    models_test_originals_fault = NULL;
    open_fail_at = 0;
    models_test_each_fault = f_each_fault;
    each_fail_at = models_test_each_calls + 1;
    r = models_install_ours(m, payload, "model.onnx", ours, why);
    CHECK(r == MODELS_NOT_SAVED && same(model_path, "SOMEONE ELSE'S"), "vq2: the first listing fails: install NOT_SAVED (%d)", r);
    models_test_each_fault = NULL;
    each_fail_at = 0;
    /* VQ5: an empty originals with its one listing failing: info saved and verified */
    layout(scratch, "vq5i", "0002");
    mkdir(originals, 0755);
    models_test_each_fault = f_each_fault;
    each_fail_at = models_test_each_calls + 1;
    saved = verified = 0;
    models_originals_info(m, &saved, &verified);
    CHECK(saved && verified, "vq5: empty originals, its listing fails: saved and verified (unknown)");
    models_test_each_fault = NULL;
    each_fail_at = 0;
    /* back to the vu1a layout for what follows */
    layout(scratch, "vu1a", "0002");
    snprintf(p, sizeof p, "%s/%s.engine", originals, pioneer);
    put(p, "0002\n");
    put(saved_copy, "PIONEER");
    put(model_path, "PIONEER");
    /* readable again: restore puts Pioneer's back after an install, as before */
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_OK && models_restore(m, why) == MODELS_OK && same(model_path, "PIONEER"),
          "vu1: readable again: install, then restore, as before");
    /* VU4: originals that can't be opened: "saved" (unknown), never "no saved copy" */
    layout(scratch, "vu4", "0002");
    mkdir(originals, 0755);
    chmod(originals, 0);
    CHECK(models_any_saved(m), "vu4: originals unreadable: saved (unknown)");
    chmod(originals, 0755);
    CHECK(!models_any_saved(m), "vu4: readable and empty: none saved");
    test_models_uninstall(scratch, m);
    test_models_md3(scratch, m, ours, pioneer);   /* before the critical cases: they close mwork */
    test_models_engine_tag(scratch, m, ours);
    test_models_critical(scratch, m, ours, pioneer);
}

/* the complete uninstall's MODEL_RESTORE in the start config's layout (VM bug c: the shell gave no
 * paths.model, so every uninstall stopped there): rekordbox's folder under roaming AppData
 * (Pioneer\rekordbox6\models\demucs3_model, hdemucs.onnx's folder as models_of takes it) and
 * LocalAppData's rbstemsplus\originals. Nothing to put back is done (MODELS_OK, nothing made); a
 * real "not back" stays a failure, which stops the uninstall (flows_test.c's restore_rc cases). */
static void test_models_uninstall(const char *scratch, models *m) {
    char why[128], root[1100], pioneer_dir[1200], rb6[1200], p[1300];
    snprintf(root, sizeof root, "%s/uninstall", scratch);
    mkdir(root, 0755);
    snprintf(pioneer_dir, sizeof pioneer_dir, "%s/Roaming/Pioneer", root);
    snprintf(rb6, sizeof rb6, "%s/rekordbox6", pioneer_dir);
    snprintf(dir, sizeof dir, "%s/models/demucs3_model", rb6);
    snprintf(model_path, sizeof model_path, "%s/hdemucs.onnx", dir);
    snprintf(support_dir, sizeof support_dir, "%s/Local/rbstemsplus", root);
    snprintf(originals, sizeof originals, "%s/originals", support_dir);
    snprintf(p, sizeof p, "%s/Roaming", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Local", root);
    mkdir(p, 0755);
    mkdir(support_dir, 0755);
    meanwhile = NULL;

    /* no rekordbox: no Pioneer folder at all */
    logged[0] = 0;
    CHECK(models_restore(m, why) == MODELS_OK && strstr(logged, "nothing to put back") && !exists(root, "Roaming/Pioneer")
              && !exists(support_dir, "originals"),
          "uninstall: no rekordbox: nothing to put back, nothing made (%s)", logged);
    /* rekordbox without a stems model folder yet */
    mkdir(pioneer_dir, 0755);
    mkdir(rb6, 0755);
    logged[0] = 0;
    CHECK(models_restore(m, why) == MODELS_OK && strstr(logged, "nothing to put back") && !exists(rb6, "models"),
          "uninstall: rekordbox, no model folder: nothing to put back (%s)", logged);
    /* rekordbox with an empty model folder, ours never placed */
    snprintf(p, sizeof p, "%s/models", rb6);
    mkdir(p, 0755);
    mkdir(dir, 0755);
    logged[0] = 0;
    CHECK(models_restore(m, why) == MODELS_OK && strstr(logged, "no stems model and none was saved") && !exists(dir, "hdemucs.onnx")
              && !exists(support_dir, "originals"),
          "uninstall: rekordbox, no model and none saved: left as it is (%s)", logged);
    /* rekordbox with its own model, ours never placed (no originals\) */
    put(model_path, "PIONEER");
    snprintf(p, sizeof p, "%s/demucs3_ver.txt", dir);
    put(p, "0002\n");
    logged[0] = 0;
    CHECK(models_restore(m, why) == MODELS_OK && strstr(logged, "already in place") && same(model_path, "PIONEER") && !exists(support_dir, "originals"),
          "uninstall: rekordbox's own model, none of ours ever placed: already in place, untouched (%s)", logged);

    /* a real "not back" still fails: ours in place and no saved copy of Pioneer's */
    put(model_path, "OURS");
    CHECK(models_restore(m, why) == MODELS_NO_SAVED_COPY && same(model_path, "OURS"), "uninstall: ours in place, nothing saved: NO_SAVED_COPY, ours stays");
    /* ... or originals\ that can't be read */
    mkdir(originals, 0755);
    chmod(originals, 0);
    int r = models_restore(m, why);
    chmod(originals, 0755);
    CHECK(r == MODELS_NOT_BACK && same(model_path, "OURS"), "uninstall: ours in place, originals unreadable: NOT_BACK, ours stays (%d)", r);
}

static void m_save_and_close(void *ctx) {
    m_save_check(ctx);
    critical_close(&mwork);
}

static void m_close_on_replace(void *ctx) {
    critical_work *w = ctx;
    critical_close(w);
    CHECK(!critical_wait_idle(w, 0), "wc1: the admitted copy still counts after closing admission");
}

static void test_models_inflight(const char *scratch, const models *m, const char *ours) {
    critical_work install, restore;
    critical_init(&install);
    critical_init(&restore);
    models active = *m;
    active.will_replace = m_close_on_replace;
    active.will_save = NULL;
    active.log = NULL;
    active.critical = &install;
    active.ctx = &install;
    layout(scratch, "wc1-inflight", "0002");
    put(model_path, "PIONEER");

    char why[128];
    int r = models_install_ours(&active, payload, "model.onnx", ours, why);
    CHECK(r == MODELS_OK && same(model_path, "OURS") && !critical_admit(&install) && critical_wait_idle(&install, 0),
          "wc1: install admitted before close finishes the rename and releases its count (%d)", r);

    active.critical = &restore;
    active.ctx = &restore;
    active.will_replace = NULL;
    active.after_rename = m_close_on_replace;
    r = models_restore(&active, why);
    CHECK(r == MODELS_OK && same(model_path, "PIONEER") && !critical_admit(&restore) && critical_wait_idle(&restore, 0),
          "wc1: restore admitted before close finishes the rename and releases its count (%d)", r);
}

/* review MC3/MC4: the copy through the rename in CriticalWork, the save outside; a closed
 * admission begins neither (MODELS_STOPPED), nothing changed; NULL means the process's one */
static void test_models_critical(const char *scratch, models *m, const char *ours, const char *pioneer) {
    char why[128], name[1300], stale[1300];
    CHECK(replace_calls > 0 && !replace_outside, "mc: every replace ran inside CriticalWork (%d of %d outside)", replace_outside, replace_calls);
    CHECK(save_calls > 0 && !save_inside, "mc: every save ran outside it (%d of %d inside)", save_inside, save_calls);
    CHECK(!critical_running(&mwork), "mc: after every install and restore above, none left running");
    test_models_inflight(scratch, m, ours);
    /* Closing during the save refuses the copy, including cleanup of a stale temporary file. */
    layout(scratch, "mc1", "0002");
    put(model_path, "PIONEER");
    snprintf(stale, sizeof stale, "%s/.hdemucs.new.123.456", dir);
    put(stale, "LEFTOVER");
    m->will_save = m_save_and_close;
    int saves = save_calls, replaces = replace_calls;
    int r = models_install_ours(m, payload, "model.onnx", ours, why);
    m->will_save = m_save_check;
    snprintf(name, sizeof name, "%s/%s.onnx", originals, pioneer);
    CHECK(r == MODELS_STOPPED && same(model_path, "PIONEER") && same(stale, "LEFTOVER") && temps_of(dir, ".hdemucs.new") == 1 && !critical_running(&mwork)
              && strstr(logged, "Demucs v4: not copied: the app is quitting") && replace_calls == replaces,
          "wc1: close during the save: install STOPPED (%d), model and stale file untouched, nothing running", r);
    CHECK(save_calls == saves + 1 && !save_inside && same(name, "PIONEER"), "mc: the save before the copy still ran, outside (as on the Mac)");
    /* closed: restore stops before the candidates, ours stays */
    put(model_path, "OURS");
    snprintf(stale, sizeof stale, "%s/.hdemucs.restore.123.456", dir);
    put(stale, "LEFTOVER");
    r = models_restore(m, why);
    CHECK(r == MODELS_STOPPED && same(model_path, "OURS") && same(stale, "LEFTOVER") && temps_of(dir, ".hdemucs.restore") == 1 && !critical_running(&mwork)
              && strstr(logged, "not put back: the app is quitting") && replace_calls == replaces,
          "wc1: closed: restore STOPPED (%d), model and stale file untouched, nothing running", r);
    /* the default: m->critical NULL is the process's CriticalWork (closed last: the tests after
     * this one use their own) */
    m->critical = NULL;
    critical_close(critical_process());
    r = models_restore(m, why);
    CHECK(r == MODELS_STOPPED && same(model_path, "OURS") && !critical_running(critical_process()), "mc: critical NULL: the process's, closed: STOPPED (%d)", r);
    m->critical = &mwork;
}

/* WF5: the next unlink of a restore copy fails once (EBUSY): the copy stays */
static int unlink_faults;
static int f_unlink_fault(const char *name) {
    (void)name;
    if (!unlink_faults) return 0;
    unlink_faults--;
    return EBUSY;
}
/* WF7: a file at the saved copy's name just before the rename */
static char planted[1400];
static void f_plant(void) { put(planted, "PLANTED"); }
/* WF11: install's final rename refused */
static int f_rename_fault(void) { return EACCES; }
/* the regular files temps_of counts, removed (a test's own leftovers) */
static void clear_temps(const char *d, const char *base) {
    DIR *dp = opendir(d);
    struct dirent *e;
    char p[1400];
    size_t bl = strlen(base);
    while (dp && (e = readdir(dp)))
        if (!strncmp(e->d_name, base, bl) && e->d_name[bl] == '.') {
            snprintf(p, sizeof p, "%s/%s", d, e->d_name);
            unlink(p);
        }
    if (dp) closedir(dp);
}

/* review MD3 (and WF1, WF5, WF7-WF11): the per-run temporary names and what clears them */
static void test_models_md3(const char *scratch, models *m, const char *ours, const char *pioneer) {
    char why[128], p[1300];
    /* MD3: each run copies to its own name (base.<pid>.<ns>); a stopped run's temporary files (the
     * fixed name of before, and base.<digits>.<digits>, regular files) are cleared, nothing else;
     * none is left after a success or a failure */
    layout(scratch, "md3", "0002");
    put(model_path, "PIONEER");
    mkdir(originals, 0755);
    /* the first 9: install's (more than the 8 a call clear_stale once took, WF10); the last 2: restore's */
    const char *stale[] = {".hdemucs.new", ".hdemucs.new.123.456", ".hdemucs.new.9.1", ".hdemucs.new.1.1", ".hdemucs.new.2.2", ".hdemucs.new.3.3",
                           ".hdemucs.new.4.4", ".hdemucs.new.5.5", ".hdemucs.new.6.6", ".hdemucs.restore", ".hdemucs.restore.1.2"};
    enum { INSTALL_STALE = 9 };
    const char *keep[] = {".hdemucs.new.x", ".hdemucs.new.1", ".hdemucs.newer", ".hdemucs.new.1.2.3", "hdemucs.new.1.2", ".hdemucs.new.1.2x", ".hdemucs.restore.old",
                          ".hdemucs.new..", ".hdemucs.new.1."};
    for (size_t i = 0; i < sizeof stale / sizeof *stale; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, stale[i]);
        put(p, "STALE");
    }
    for (size_t i = 0; i < sizeof keep / sizeof *keep; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, keep[i]);
        put(p, "KEEP");
    }
    char name[1300], folder_in[1400], part_bak[1400], part_stale[1400], part_fixed[1400], part_base[100], want[100];
    snprintf(p, sizeof p, "%s/.hdemucs.new.7.8", dir);   /* a folder by a temporary name: kept */
    mkdir(p, 0755);
    snprintf(folder_in, sizeof folder_in, "%s/inside", p);
    put(folder_in, "IN");
    snprintf(part_base, sizeof part_base, "%s.onnx.part", pioneer);
    snprintf(part_fixed, sizeof part_fixed, "%s/%s", originals, part_base);
    put(part_fixed, "STALE");
    snprintf(part_stale, sizeof part_stale, "%s/%s.5.6", originals, part_base);
    put(part_stale, "STALE");
    snprintf(part_bak, sizeof part_bak, "%s/%s.bak", originals, part_base);
    put(part_bak, "KEEP");
    char target[1300], link_new[1400], link_part[1400];
    snprintf(target, sizeof target, "%s/outside-target", payload);
    put(target, "TARGET");
    snprintf(link_new, sizeof link_new, "%s/.hdemucs.new.3.4", dir);
    snprintf(link_part, sizeof link_part, "%s/%s.7.8", originals, part_base);
    CHECK(!symlink(target, link_new) && !symlink(target, link_part), "md3: links by a temporary name");
    m->will_replace = m_see_temp;
    seen_temp[0] = 0;
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_OK && same(model_path, "OURS"), "md3: installed (%s)", logged);
    snprintf(want, sizeof want, ".hdemucs.new.%d.", (int)getpid());
    CHECK(!strncmp(seen_temp, want, strlen(want)), "md3: install copied to its own name (%s)", seen_temp);
    int stale_gone = 1, kept = 1;
    for (size_t i = 0; i < INSTALL_STALE; i++) stale_gone &= !exists(dir, stale[i]);
    for (size_t i = 0; i < sizeof keep / sizeof *keep; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, keep[i]);
        kept &= same(p, "KEEP");
    }
    CHECK(stale_gone && kept && same(folder_in, "IN") && temps_of(dir, ".hdemucs.new") == 0,
          "md3: install: the stopped runs' .hdemucs.new files cleared, other names and the folder kept, none left");
    snprintf(name, sizeof name, "%s/%s.onnx", originals, pioneer);
    CHECK(same(name, "PIONEER") && access(part_fixed, F_OK) && access(part_stale, F_OK) && same(part_bak, "KEEP") && temps_of(originals, part_base) == 0,
          "md3: save: the stopped runs' parts cleared, another name kept, none left");
    struct stat lst;
    CHECK(!lstat(link_new, &lst) && S_ISLNK(lst.st_mode) && !lstat(link_part, &lst) && S_ISLNK(lst.st_mode) && same(target, "TARGET"),
          "md3: links by a temporary name stay, their target unchanged (WF8)");
    seen_temp[0] = 0;
    CHECK(models_restore(m, why) == MODELS_OK && same(model_path, "PIONEER"), "md3: restored (%s)", logged);
    snprintf(want, sizeof want, ".hdemucs.restore.%d.", (int)getpid());
    CHECK(!strncmp(seen_temp, want, strlen(want)), "md3: restore copied to its own name (%s)", seen_temp);
    snprintf(p, sizeof p, "%s/.hdemucs.restore.old", dir);
    CHECK(!exists(dir, stale[INSTALL_STALE]) && !exists(dir, stale[INSTALL_STALE + 1]) && same(p, "KEEP") && temps_of(dir, ".hdemucs.restore") == 0,
          "md3: restore: the stopped runs' .hdemucs.restore files cleared, another name kept, none left");
    m->will_replace = m_will;
    /* failures leave none either: a copy that doesn't verify, a change meanwhile, every saved copy damaged */
    put(model_path, "OURS");
    CHECK(models_install_ours(m, payload, "model.onnx", pioneer, why) == MODELS_COPY_BAD && temps_of(dir, ".hdemucs.new") == 0, "md3: copy bad: none left");
    meanwhile = "SOMETHING NEW";
    CHECK(models_install_ours(m, payload, "model.onnx", ours, why) == MODELS_CHANGED && temps_of(dir, ".hdemucs.new") == 0, "md3: changed: none left");
    meanwhile = NULL;
    put(model_path, "OURS");
    put(name, "BROKEN");
    CHECK(models_restore(m, why) == MODELS_SAVED_DAMAGED && same(model_path, "OURS") && temps_of(dir, ".hdemucs.restore") == 0,
          "md3: all damaged: none left");

    /* WF5: a damaged first candidate whose copy can't be removed (EBUSY, once): the second, good
     * one is still put back, under its own name */
    layout(scratch, "wf5", "0002");
    put(model_path, "OURS");
    mkdir(originals, 0755);
    char bad_sha[65], good[1400], bad[1400];
    sha_of("PIONEER B", bad_sha);
    snprintf(good, sizeof good, "%s/%s.onnx", originals, pioneer);
    put(good, "PIONEER");
    snprintf(bad, sizeof bad, "%s/%s.onnx", originals, bad_sha);
    put(bad, "NOT WHAT ITS NAME SAYS");
    struct timeval t_old[2] = {{1000, 0}, {1000, 0}}, t_new[2] = {{2000, 0}, {2000, 0}};
    utimes(good, t_old);
    utimes(bad, t_new);   /* the damaged one first (newest first among equals) */
    unlink_faults = 1;
    models_test_unlink_fault = f_unlink_fault;
    int r5 = models_restore(m, why);
    models_test_unlink_fault = NULL;
    CHECK(r5 == MODELS_OK && same(model_path, "PIONEER") && unlink_faults == 0, "wf5: the stuck copy doesn't block the next candidate (%d: %s)", r5, logged);
    clear_temps(dir, ".hdemucs.restore");   /* the copy the fault left */

    /* WF7: a saved copy appearing at <sha>.onnx just before the rename: NOT_SAVED, it and the
     * live model unchanged, no part left */
    layout(scratch, "wf7", "0002");
    put(model_path, "PIONEER");
    mkdir(originals, 0755);
    snprintf(planted, sizeof planted, "%s/%s.onnx", originals, pioneer);
    models_test_before_place = f_plant;
    int r7 = models_install_ours(m, payload, "model.onnx", ours, why);
    models_test_before_place = NULL;
    CHECK(r7 == MODELS_NOT_SAVED && same(planted, "PLANTED") && same(model_path, "PIONEER") && temps_of(originals, part_base) == 0,
          "wf7: a copy planted before the rename: NOT_SAVED, it and rekordbox's model unchanged, no part left (%d)", r7);

    /* WF11: install's final rename failing (EACCES): NOT_PLACED_WHY, rekordbox's model and the
     * saved original unchanged, no temp left */
    layout(scratch, "wf11", "0002");
    put(model_path, "PIONEER");
    models_test_rename_fault = f_rename_fault;
    int r11 = models_install_ours(m, payload, "model.onnx", ours, why);
    models_test_rename_fault = NULL;
    snprintf(name, sizeof name, "%s/%s.onnx", originals, pioneer);
    CHECK(r11 == MODELS_NOT_PLACED_WHY && same(model_path, "PIONEER") && same(name, "PIONEER") && temps_of(dir, ".hdemucs.new") == 0,
          "wf11: the rename failing: NOT_PLACED_WHY (%d, %s), nothing changed, no temp left", r11, why);
}

/* review b-a2a-F1: the saved model's <sha>.engine. Its write failing makes the save NOT_SAVED
 * (the copy removed, ours not placed), never an untagged copy that an older engine's tagged one
 * outranks at restore; an untagged copy "already saved" gets its tag (or the save fails) */
static int engine_faults;
static int f_engine_fault(void) {
    engine_faults++;
    return ENOSPC;
}
static void test_models_engine_tag(const char *scratch, models *m, const char *ours) {
    (void)ours;
    char why[128], p[1300], e1[65], e2[65], part_base[80];
    sha_of("PIONEER_ENGINE1", e1);
    sha_of("PIONEER_ENGINE2", e2);
    snprintf(part_base, sizeof part_base, "%s.onnx.part", e2);
    /* engine 0001's model saved and tagged earlier; rekordbox now has engine 0002 and its model */
    layout(scratch, "et", "0002\n");
    mkdir(originals, 0755);
    snprintf(p, sizeof p, "%s/%s.onnx", originals, e1);
    put(p, "PIONEER_ENGINE1");
    snprintf(p, sizeof p, "%s/%s.engine", originals, e1);
    put(p, "0001\n");
    put(model_path, "PIONEER_ENGINE2");
    char onnx2[80], engine2[80];
    snprintf(onnx2, sizeof onnx2, "%s.onnx", e2);
    snprintf(engine2, sizeof engine2, "%s.engine", e2);
    models_test_engine_fault = f_engine_fault;
    engine_faults = 0;
    int r = models_install_ours(m, payload, "model.onnx", ours, why);
    models_test_engine_fault = NULL;
    CHECK(r == MODELS_NOT_SAVED && engine_faults == 1 && same(model_path, "PIONEER_ENGINE2") && !exists(originals, onnx2) && !exists(originals, engine2)
              && temps_of(originals, part_base) == 0,
          "et: the engine file not written: NOT_SAVED (%d), rekordbox's model untouched, no untagged copy, no part left (%s)", r, logged);
    /* the fault gone: the copy is saved with its tag, ours placed */
    logged[0] = 0;
    r = models_install_ours(m, payload, "model.onnx", ours, why);
    snprintf(p, sizeof p, "%s/%s", originals, engine2);
    CHECK(r == MODELS_OK && same(model_path, "OURS") && exists(originals, onnx2) && same(p, "0002\n"), "et: the next install tags the copy (%d, %s)", r, logged);
    /* restore with engines 0001 and 0002 saved: 0002's model */
    logged[0] = 0;
    r = models_restore(m, why);
    CHECK(r == MODELS_OK && same(model_path, "PIONEER_ENGINE2") && strstr(logged, "STEMS Engine 0002"), "et: restore puts engine 0002's model back (%d, %s)", r, logged);

    /* an untagged copy left by an older run (the live model's sha): "already saved" writes the tag */
    snprintf(p, sizeof p, "%s/%s", originals, engine2);
    unlink(p);
    logged[0] = 0;
    models_test_engine_fault = f_engine_fault;
    engine_faults = 0;
    r = models_install_ours(m, payload, "model.onnx", ours, why);
    models_test_engine_fault = NULL;
    CHECK(r == MODELS_NOT_SAVED && engine_faults == 1 && same(model_path, "PIONEER_ENGINE2") && !exists(originals, engine2),
          "et: already saved, its tag not writable: NOT_SAVED (%d), rekordbox's model untouched (%s)", r, logged);
    logged[0] = 0;
    r = models_install_ours(m, payload, "model.onnx", ours, why);
    CHECK(r == MODELS_OK && same(model_path, "OURS") && same(p, "0002\n") && strstr(logged, "already saved"), "et: already saved, untagged: the tag written (%d, %s)", r,
          logged);
    r = models_restore(m, why);
    CHECK(r == MODELS_OK && same(model_path, "PIONEER_ENGINE2"), "et: then restore picks engine 0002's model (%d)", r);
}
