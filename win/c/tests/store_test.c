/* payload_store.c on plat_mac.c: SignedReleases.swift's saved-copy and floor cases, plus the
 * Windows rules (no news, a suffixed version refused), in a scratch folder. */
#include "check.h"

#include "../fs_write.h"
#include "../payload.h"
#include "sig_mac.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>   /* chflags */

static char dirs[2][1024];

static int exists(int which, const char *name) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", dirs[which], name);
    return access(p, F_OK) == 0;
}

static void put(int which, const char *name, const void *d, size_t n) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", dirs[which], name);
    FILE *f = fopen(p, "wb");
    if (!f) abort();
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

static void rm(int which, const char *name) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", dirs[which], name);
    unlink(p);
}

/* the descriptors open in this process (0..4095) */
static int open_fds(void) {
    int n = 0;
    for (int i = 0; i < 4096; i++) n += fcntl(i, F_GETFD) != -1;
    return n;
}

/* whether the folder holds exactly these names (a temp file left behind fails it) */
static int only(int which, int want) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "ls -A '%s' | wc -l", dirs[which]);
    FILE *p = popen(cmd, "r");
    int n = -1;
    if (p) {
        if (fscanf(p, "%d", &n) != 1) n = -1;
        pclose(p);
    }
    return n == want;
}

static char *payload(const char *pv, int windows) {
    char *o = malloc(2048);
    if (!o) abort();
    snprintf(o, 2048,
             "{\"payload_version\": \"%s\", %s"
             "\"model\": {\"file\": \"m.onnx\", \"sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\", \"size\": 9}%s}",
             pv,
             "\"app\": {\"version\": \"1.0.0\", \"zip\": \"a.zip\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"bridge\": {\"file\": \"b.dylib\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"compatible\": {\"rekordbox\": [], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": []}, ",
             windows ? ", \"windows\": {\"app\": {\"version\": \"1.0.0\", \"zip\": \"w.zip\", "
                       "\"sha256\": \"1111111111111111111111111111111111111111111111111111111111111111\", "
                       "\"admin_sha256\": \"2222222222222222222222222222222222222222222222222222222222222222\", "
                       "\"stub_sha256\": {\"x64\": \"3333333333333333333333333333333333333333333333333333333333333333\", "
                       "\"arm64\": \"4444444444444444444444444444444444444444444444444444444444444444\"}}, "
                       "\"bridge\": {\"file\": \"b.dll\", \"sha256\": \"5555555555555555555555555555555555555555555555555555555555555555\"}, "
                       "\"compatible\": {\"rekordbox\": [\"7.2.19\"], \"ort_sha256\": [], \"stems_engine\": [\"0002\"]}}"
                     : "");
    return o;
}

static payload_manifest m;

/* C6: two accepts at once */
typedef struct {
    const payload_files *f;
    const char *data;
    const unsigned char *sig;
    size_t sn;
    int r;
} racer;
static void *race(void *arg) {
    racer *x = arg;
    static payload_manifest mm[2];
    x->r = payload_accept(x->f, x->data, strlen(x->data), x->sig, x->sn, &mm[x->data[23] == '2']);
    return NULL;
}

void test_store(const char *scratch) {
    snprintf(dirs[0], sizeof dirs[0], "%s/payload", scratch);
    snprintf(dirs[1], sizeof dirs[1], "%s/support", scratch);
    mkdir(dirs[0], 0755);
    mkdir(dirs[1], 0755);

    /* win_write_atomic, win/c's one copy: made, replaced whole, empty, no temp file left; a
     * folder that isn't there, or a name that is a folder, an error and nothing left either */
    char got[64], p0[1100];
    snprintf(p0, sizeof p0, "%s/w", dirs[1]);
    FILE *x;
    CHECK(!win_write_atomic(dirs[1], "w", "first text", 10) && only(1, 1), "written, no temp file");
    CHECK(!win_write_atomic(dirs[1], "w", "2nd", 3) && (x = fopen(p0, "rb")) && fread(got, 1, sizeof got, x) == 3 && !fclose(x)
              && !memcmp(got, "2nd", 3) && only(1, 1),
          "replaced whole by a shorter text");
    CHECK(!win_write_atomic(dirs[1], "w", NULL, 0) && (x = fopen(p0, "rb")) && fread(got, 1, sizeof got, x) == 0 && !fclose(x), "empty");
    snprintf(p0, sizeof p0, "%s/none", dirs[1]);
    CHECK(win_write_atomic(p0, "w", "x", 1) == ENOENT, "no such folder: ENOENT");
    snprintf(p0, sizeof p0, "%s/dir", dirs[1]);
    mkdir(p0, 0755);
    CHECK(win_write_atomic(dirs[1], "dir", "x", 1) != 0 && only(1, 2), "over a folder: an error, no temp file left");
    rmdir(p0);
    /* win_write_at: through the caller's handle, which stays open (written twice through it) */
    plat_dir wd;
    CHECK(!plat_dir_open(dirs[1], &wd) && !win_write_at(wd, "w", "via d", 5) && !win_write_at(wd, "w", "again", 5), "through a handle, twice");
    plat_dir_close(wd);
    snprintf(p0, sizeof p0, "%s/w", dirs[1]);
    CHECK((x = fopen(p0, "rb")) && fread(got, 1, sizeof got, x) == 5 && !fclose(x) && !memcmp(got, "again", 5) && only(1, 1), "its text, no temp file");
    /* no handle left open: 64 writes by path leave as many open descriptors as before (review AW3) */
    int fds_before = open_fds(), all_ok = 1;
    for (int i = 0; i < 64; i++) all_ok &= !win_write_atomic(dirs[1], "w", "fd", 2);
    int fds_after = open_fds();
    CHECK(all_ok && fds_after == fds_before && only(1, 1), "64 writes: no descriptor left open (%d before, %d after)", fds_before, fds_after);
    rm(1, "w");
    CHECK(only(1, 0), "the support folder empty again");

    test_key rel, bak, oth;
    CHECK(test_key_new(&rel) == 0 && test_key_new(&bak) == 0 && test_key_new(&oth) == 0, "keys");
    sig_pubkey keys[2] = {{"release", {0}}, {"backup", {0}}};
    memcpy(keys[0].xy, rel.xy, 64);
    memcpy(keys[1].xy, bak.xy, 64);
    payload_files f = {dirs[0], dirs[1], keys, 2, "1.0.0"};
    unsigned char s[SIG_DER_MAX], s2[SIG_DER_MAX];

    char *p = payload("1.1.0", 1);
    size_t pn = strlen(p), sn = test_sign(&rel, p, pn, s);
    CHECK(payload_saved(&f, &m) == PAYLOAD_NONE, "nothing saved: none");
    put(0, "payload.json", p, pn);
    CHECK(payload_saved(&f, &m) == PAYLOAD_NO_SIGNATURE, "an unsigned saved payload.json is ignored");
    rm(0, "payload.json");

    /* a download that doesn't verify changes nothing */
    size_t on = test_sign(&oth, p, pn, s2);
    CHECK(payload_accept(&f, p, pn, s2, on, &m) == PAYLOAD_BAD_SIGNATURE, "another key's signature: refused");
    CHECK(payload_accept(&f, p, pn, s, 20, &m) == PAYLOAD_BAD_SIGNATURE, "a truncated signature: refused");
    CHECK(payload_accept(&f, p, pn, s, 0, &m) == PAYLOAD_NO_SIGNATURE, "an empty signature: refused");
    CHECK(only(0, 0) && only(1, 0), "and nothing saved");

    /* a good one */
    CHECK(payload_accept(&f, p, pn, s, sn, &m) == PAYLOAD_OK && !strcmp(m.signed_by, "release"), "a signed payload.json is accepted");
    CHECK(exists(0, "payload.json") && exists(0, "payload.json.sig") && only(0, 2), "saved with its signature, no temp file left");
    char v[64];
    CHECK(payload_read_floor(&f, v) && !strcmp(v, "1.1.0") && only(1, 1), "its version is the newest accepted");
    CHECK(payload_saved(&f, &m) == PAYLOAD_OK && !strcmp(m.payload_version, "1.1.0") && m.has_windows, "the saved copy verifies when read");
    put(0, "payload.json", p, pn - 1);
    CHECK(payload_saved(&f, &m) == PAYLOAD_BAD_SIGNATURE, "changed after the download: refused on read");
    put(0, "payload.json", p, pn);
    rm(0, "payload.json.sig");
    CHECK(payload_saved(&f, &m) == PAYLOAD_NO_SIGNATURE, "its signature gone: refused");
    put(0, "payload.json.sig", "", 0);
    CHECK(payload_saved(&f, &m) == PAYLOAD_NO_SIGNATURE, "an empty signature: refused");
    put(0, "payload.json.sig", s, sn);
    CHECK(payload_saved(&f, &m) == PAYLOAD_OK, "put back, it verifies again");

    /* no going back */
    char *newer = payload("1.2.0", 1);
    size_t nn = strlen(newer), nsn = test_sign(&bak, newer, nn, s2);
    CHECK(payload_accept(&f, newer, nn, s2, nsn, &m) == PAYLOAD_OK && payload_read_floor(&f, v) && !strcmp(v, "1.2.0"), "a newer one raises the floor");
    CHECK(payload_accept(&f, p, pn, s, sn, &m) == PAYLOAD_OLDER, "an older one, validly signed, is refused");
    CHECK(payload_saved(&f, &m) == PAYLOAD_OK && !strcmp(m.payload_version, "1.2.0"), "and the newer one stays saved");
    put(0, "payload.json", p, pn);
    put(0, "payload.json.sig", s, sn);
    CHECK(payload_saved(&f, &m) == PAYLOAD_OLDER, "an older signed pair put in place is refused on read");
    rm(1, "payload-version");
    CHECK(payload_saved(&f, &m) == PAYLOAD_OK && !strcmp(m.payload_version, "1.1.0"), "(the record deleted: an older one above the minimum reads again)");
    put(1, "payload-version", "not a version!\n", 15);
    CHECK(!payload_read_floor(&f, v), "a record that isn't a version is ignored");

    /* the minimum */
    f.minimum = "1.2.0";
    rm(1, "payload-version");
    CHECK(payload_saved(&f, &m) == PAYLOAD_OLDER, "an older pair with the record deleted: still refused (the app's version)");
    rm(0, "payload.json"), rm(0, "payload.json.sig");
    CHECK(payload_accept(&f, p, pn, s, sn, &m) == PAYLOAD_OLDER && only(0, 0) && only(1, 0), "older than the app, none accepted: nothing saved");

    /* a minimum that isn't a version refuses everything (C4) */
    static const char *const bad_min[] = {"v1.5.0", "1.5", "1.5.0 x", "garbage_"};
    for (size_t i = 0; i < sizeof bad_min / sizeof bad_min[0]; i++) {
        f.minimum = bad_min[i];
        CHECK(payload_accept(&f, p, pn, s, sn, &m) == PAYLOAD_NOT_VALID && only(0, 0) && only(1, 0), "minimum %s: payload 1.1.0 refused", bad_min[i]);
    }
    f.minimum = "1.2.0";

    /* no news: a valid payload without the windows block */
    char *mac = payload("1.3.0", 0);
    size_t mn = strlen(mac), msn = test_sign(&rel, mac, mn, s2);
    CHECK(payload_accept(&f, mac, mn, s2, msn, &m) == PAYLOAD_NO_NEWS && only(0, 0) && only(1, 0), "a Mac-only payload: no news, nothing saved");

    /* a suffixed version (there is no beta channel): refused, nothing saved */
    char *b2 = payload("1.5.0-beta.2", 1), *fin = payload("1.5.0", 1);
    unsigned char sb2[SIG_DER_MAX], sf[SIG_DER_MAX];
    size_t b2n = test_sign(&rel, b2, strlen(b2), sb2), fn = test_sign(&rel, fin, strlen(fin), sf);
    CHECK(payload_accept(&f, b2, strlen(b2), sb2, b2n, &m) == PAYLOAD_NOT_VALID && only(0, 0) && only(1, 0),
          "a signed payload with a suffixed version: refused, nothing saved");
    CHECK(payload_accept(&f, fin, strlen(fin), sf, fn, &m) == PAYLOAD_OK && payload_read_floor(&f, v) && !strcmp(v, "1.5.0"), "the final is accepted");
    CHECK(payload_saved(&f, &m) == PAYLOAD_OK, "and reads");

    free(p), free(newer), free(mac), free(b2), free(fin);

    /* C5: the floor first, its write checked; a fault in either write fails closed */
    snprintf(dirs[0], sizeof dirs[0], "%s/c5-payload", scratch);
    snprintf(dirs[1], sizeof dirs[1], "%s/c5-support", scratch);
    mkdir(dirs[0], 0755);
    mkdir(dirs[1], 0755);
    payload_files g = {dirs[0], dirs[1], keys, 2, "1.0.0"};
    char *v11 = payload("1.1.0", 1), *v12 = payload("1.2.0", 1), *v13 = payload("1.3.0", 1), *v105 = payload("1.0.5", 1);
    unsigned char s11[SIG_DER_MAX], s12[SIG_DER_MAX], s13[SIG_DER_MAX], s105[SIG_DER_MAX];
    size_t n11 = test_sign(&rel, v11, strlen(v11), s11), n12 = test_sign(&rel, v12, strlen(v12), s12);
    size_t n13 = test_sign(&rel, v13, strlen(v13), s13), n105 = test_sign(&rel, v105, strlen(v105), s105);
    CHECK(payload_accept(&g, v11, strlen(v11), s11, n11, &m) == PAYLOAD_OK, "1.1.0 accepted");
    chmod(dirs[1], 0500);   /* the floor's rename fails */
    CHECK(payload_accept(&g, v12, strlen(v12), s12, n12, &m) == PAYLOAD_NOT_SAVED, "the floor can't be written: not saved");
    chmod(dirs[1], 0755);
    CHECK(payload_read_floor(&g, v) && !strcmp(v, "1.1.0") && only(1, 1), "the floor unchanged, no temp file");
    CHECK(payload_saved(&g, &m) == PAYLOAD_OK && !strcmp(m.payload_version, "1.1.0"), "the pair unchanged");
    CHECK(payload_accept(&g, v105, strlen(v105), s105, n105, &m) == PAYLOAD_OLDER, "an older one still refused");
    chmod(dirs[0], 0500);   /* the pair's write fails after the floor's */
    CHECK(payload_accept(&g, v13, strlen(v13), s13, n13, &m) == PAYLOAD_NOT_SAVED, "the pair can't be written: not saved");
    chmod(dirs[0], 0755);
    CHECK(payload_read_floor(&g, v) && !strcmp(v, "1.3.0"), "the floor was raised first");
    CHECK(payload_saved(&g, &m) == PAYLOAD_OLDER, "the old pair under the newer floor is refused: fails closed");
    CHECK(payload_accept(&g, v13, strlen(v13), s13, n13, &m) == PAYLOAD_OK && payload_saved(&g, &m) == PAYLOAD_OK, "the next download saves it");
    /* C12: the signature is saved before payload.json: when payload.json can't be replaced, the
     * new signature sits beside the old payload.json, and the pair doesn't verify */
    char *v14 = payload("1.4.0", 1);
    unsigned char s14[SIG_DER_MAX];
    size_t n14 = test_sign(&rel, v14, strlen(v14), s14);
    char jp[1100], sp[1100];
    snprintf(jp, sizeof jp, "%s/payload.json", dirs[0]);
    snprintf(sp, sizeof sp, "%s/payload.json.sig", dirs[0]);
    CHECK(chflags(jp, UF_IMMUTABLE) == 0, "payload.json made immutable");
    CHECK(payload_accept(&g, v14, strlen(v14), s14, n14, &m) == PAYLOAD_NOT_SAVED, "payload.json can't be replaced: not saved");
    chflags(jp, 0);
    size_t jn = 0, sn2 = 0;
    FILE *fj = fopen(jp, "rb"), *fs = fopen(sp, "rb");
    char jb[4096], sb[128];
    if (fj) jn = fread(jb, 1, sizeof jb, fj), fclose(fj);
    if (fs) sn2 = fread(sb, 1, sizeof sb, fs), fclose(fs);
    CHECK(sn2 == n14 && !memcmp(sb, s14, n14), "the signature was replaced first");
    CHECK(jn == strlen(v13) && !memcmp(jb, v13, jn), "payload.json kept its old bytes");
    CHECK(payload_saved(&g, &m) == PAYLOAD_BAD_SIGNATURE, "the mixed pair doesn't verify: refused");
    free(v14);

    /* C6: overlapping accepts never lower the floor */
    rm(1, "payload-version");
    int lowered = 0;
    for (int i = 0; i < 200; i++) {
        rm(1, "payload-version");
        racer a = {&g, v11, s11, n11, -1}, b = {&g, v12, s12, n12, -1};
        pthread_t ta, tb;
        pthread_create(&ta, NULL, race, i & 1 ? &a : &b);
        pthread_create(&tb, NULL, race, i & 1 ? &b : &a);
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        if (!payload_read_floor(&g, v) || strcmp(v, "1.2.0") || b.r != PAYLOAD_OK) lowered++;
    }
    CHECK(lowered == 0, "two accepts at once, 200 rounds: the floor always ends at 1.2.0 (%d didn't)", lowered);
    free(v11), free(v12), free(v13), free(v105);
    test_key_free(&rel), test_key_free(&bak), test_key_free(&oth);
}
