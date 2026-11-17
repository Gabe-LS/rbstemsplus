/* The worker's runs on the Mac (app.md §7: RootScripts' scenarios become the worker's test list,
 * plus the admin floor, the --version pattern and the protocol mismatch): real files in a scratch
 * tree through plat_mac.c, compat.c and payload.c, with the fake OS (fake_os.c) and the test
 * crypto shim's keys. */
#include "../../c/tests/check.h"

#include "../../c/tests/sig_mac.h"
#include "fake_os.h"

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

void test_worker(const char *scratch, const char *pedir);

static char S[1024], RB[1100], V19[1200], V20[1200], ROOT[1100], SRC[1100], APP[1100];
static unsigned char *exe;   /* rekordbox.exe: the PE fixture with rekordbox's version resource, "signed" */
static size_t exe_n;
static test_key key;
static sig_pubkey keys[1];
static fake F;

static const char BRIDGE1[] = "MZ bridge one rbstems bridge: 1 rbstems_marker";
static const char BRIDGE2[] = "MZ bridge two rbstems bridge: 2 rbstems_marker";
static const char WORKER_A[] = "MZ worker A rbsp-admin worker: proto=1 minimum=1.5.0\n";
static const char WORKER_B[] = "MZ worker B rbsp-admin worker: proto=1 minimum=1.5.0\n";

/* -- files */

static void spit(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "can't write %s\n", path);
        abort();
    }
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}
static void spits(const char *path, const char *s) { spit(path, s, strlen(s)); }

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    *n = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(len > 0 ? (size_t)len + 1 : 1);
    if (b && len > 0 && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) b[len > 0 ? len : 0] = 0;
    *n = len > 0 ? (size_t)len : 0;
    return b;
}

/* whether the file at path holds exactly s */
static int holds(const char *path, const char *s) {
    size_t n;
    unsigned char *b = slurp(path, &n);
    int r = b && n == strlen(s) && !memcmp(b, s, n);
    free(b);
    return r;
}

static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static const char *P(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static const char *P(const char *fmt, ...) {
    static char ring[8][1400];
    static int at;
    char *o = ring[at++ & 7];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o, 1400, fmt, ap);
    va_end(ap);
    return o;
}

static void sha_hex(const void *d, size_t n, char out[65]) {
    unsigned char h[32];
    CC_SHA256(d, (CC_LONG)n, h);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}
static const char *sha_of(const char *s) {
    static char o[4][65];
    static int at;
    char *r = o[at++ & 3];
    sha_hex(s, strlen(s), r);
    return r;
}

static void sh(const char *cmd) {
    if (system(cmd)) {
        fprintf(stderr, "failed: %s\n", cmd);
        abort();
    }
}

/* Pioneer's ORT of version v: "signed", unique per version */
static const char *ort_of(const char *v) { return P("MZ onnxruntime %s ALPHATHETA-SIGNED", v); }

/* a rekordbox version folder: its exe (the fixture's ProductVersion, UTF-16, made v's: same
 * length) and Pioneer's ORT; an uninstall entry for it */
static void version_folder(const char *dir, const char *v, int entry) {
    static const unsigned char v19[12] = {'7', 0, '.', 0, '2', 0, '.', 0, '1', 0, '9', 0};
    mkdir(dir, 0755);
    unsigned char *e = malloc(exe_n);
    if (!e) abort();
    memcpy(e, exe, exe_n);
    if (strlen(v) == 6)
        for (size_t i = 0; i + 12 <= exe_n; i++)
            if (!memcmp(e + i, v19, 12))
                for (size_t k = 0; k < 6; k++) e[i + 2 * k] = (unsigned char)v[k];
    spit(P("%s/rekordbox.exe", dir), e, exe_n);
    free(e);
    spits(P("%s/onnxruntime.dll", dir), ort_of(v));
    if (entry) fake_entry(&F, dir, P("%s.0342", v));
}

/* the source folder: a signed payload.json (payload_version pv) naming bridge, worker, the ORTs
 * listed; windows 0: no windows block */
static void source(const char *pv, const char *bridge, const char *worker, int windows, const char *bridge_name) {
    char bsha[65], wsha[65], o19[65], o20[65];
    sha_hex(bridge, strlen(bridge), bsha);
    sha_hex(worker, strlen(worker), wsha);
    sha_hex(ort_of("7.2.19"), strlen(ort_of("7.2.19")), o19);
    sha_hex(ort_of("7.2.20"), strlen(ort_of("7.2.20")), o20);
    sh(P("rm -rf '%s' && mkdir -p '%s/admin'", SRC, SRC));
    char json[4096];
    snprintf(json, sizeof json,
             "{\"payload_version\": \"%s\", "
             "\"model\": {\"file\": \"m.onnx\", \"sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\", \"size\": 9}%s",
             pv, windows ? ", \"windows\": {" : "}");
    if (windows)
        snprintf(json + strlen(json), sizeof json - strlen(json),
                 "\"app\": {\"version\": \"%s\", \"zip\": \"w.zip\", "
                 "\"sha256\": \"1111111111111111111111111111111111111111111111111111111111111111\", \"admin_sha256\": \"%s\", "
                 "\"stub_sha256\": {\"x64\": \"3333333333333333333333333333333333333333333333333333333333333333\", "
                 "\"arm64\": \"4444444444444444444444444444444444444444444444444444444444444444\"}}, "
                 "\"bridge\": {\"file\": \"%s\", \"sha256\": \"%s\"}, "
                 "\"compatible\": {\"rekordbox\": [\"7.2.19\", \"7.2.20\"], \"ort_sha256\": [\"%s\", \"%s\"], \"stems_engine\": [\"0002\"]}}}",
                 pv, wsha, bridge_name, bsha, o19, o20);
    {
        /* a payload carries the Mac part too (payload.c's rule) */
        char *w = strstr(json, "\"model\"");
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%.*s\"app\": {\"version\": \"1.0.0\", \"zip\": \"a.zip\", "
                                  "\"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
                                  "\"bridge\": {\"file\": \"b.dylib\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
                                  "\"compatible\": {\"rekordbox\": [], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": []}, %s",
                 (int)(w - json), json, w);
        snprintf(json, sizeof json, "%s", tmp);
    }
    spits(P("%s/payload.json", SRC), json);
    unsigned char sig[SIG_DER_MAX];
    size_t sn = test_sign(&key, json, strlen(json), sig);
    spit(P("%s/payload.json.sig", SRC), sig, sn);
    if (admin_plain_name_ok(bridge_name)) spits(P("%s/%s", SRC, bridge_name), bridge);
    spits(P("%s/admin/RB Stems Plus.exe", SRC), worker);
}

static const char *sha12_of(const char *s) {
    static char o[13];
    memcpy(o, sha_of(s), 12);
    o[12] = 0;
    return o;
}

/* a fresh scratch tree: rekordbox 7.2.19 as Pioneer's installer leaves it, no root, the source
 * for payload 1.5.0 with BRIDGE1 and WORKER_A, the worker running from the app folder */
static void fresh(void) {
    fake_init(&F);
    sh(P("rm -rf '%s'/* && mkdir -p '%s' '%s/admin'", S, RB, APP));
    version_folder(V19, "7.2.19", 1);
    source("1.5.0", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    spits(P("%s/admin/RB Stems Plus.exe", APP), WORKER_A);
    snprintf(F.self, sizeof F.self, "%s/admin/RB Stems Plus.exe", APP);
}

static int run_verb(int verb, const char *version, int force) {
    admin_args a;
    memset(&a, 0, sizeof a);
    a.verb = verb;
    a.proto = ADMIN_PROTO;
    snprintf(a.version, sizeof a.version, "%s", version);
    if (verb == ADMIN_INSTALL || verb == ADMIN_INSTALL_WORKER) snprintf(a.source, sizeof a.source, "%s", SRC);
    a.force = force;
    snprintf(a.pipe, sizeof a.pipe, "\\\\.\\pipe\\rbstemsplus-admin-0123456789abcdef0123456789abcdef");
    admin_env e = {ROOT, RB, keys, 1, "1.5.0", 75000, 1000};
    fake_clear_said(&F);
    return admin_run(&e, &F.os, &a);
}
static int install(void) { return run_verb(ADMIN_INSTALL, "7.2.19", 0); }

/* the state after a good install of BRIDGE1 into 7.2.19 */
static int installed(const char *vdir, const char *v, const char *bridge) {
    return holds(P("%s/onnxruntime.dll", vdir), bridge) && holds(P("%s/pioneer/%s/onnxruntime.dll", ROOT, v), ort_of(v))
           && holds(P("%s/pioneer/%s/SHA256SUMS", ROOT, v), P("%s  onnxruntime.dll\n", sha_of(ort_of(v))))
           && holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of(v)) && holds(P("%s/installed-bridge", ROOT), P("%s\n", sha_of(bridge)))
           && !exists(P("%s/onnxruntime.dll.rbsp-new", vdir)) && !exists(P("%s/onnxruntime.dll.rbsp-probe", vdir));
}

static int no_stage(void) {
    char cmd[1300];
    snprintf(cmd, sizeof cmd, "ls -d '%s'/stage.* > /dev/null 2>&1", ROOT);
    return system(cmd) != 0;
}

static int pioneers(const char *vdir, const char *v) { return holds(P("%s/onnxruntime.dll", vdir), ort_of(v)); }

/* -- cases */

static void t_first_install(void) {
    fresh();
    int x = install();
    CHECK(x == ADMIN_EXIT_OK, "the first install: %d\n%s", x, F.said);
    CHECK(installed(V19, "7.2.19", BRIDGE1), "the bridge in, Pioneer's file saved with SHA256SUMS, ort\\'s copy, the record");
    CHECK(holds(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A)), WORKER_A), "the first install puts the worker into bin\\<sha12>\\ (I75, I78)");
    CHECK(holds(P("%s/payload-version", ROOT), "1.5.0\n") || holds(P("%s/payload-version", ROOT), "1.5.0"), "the floor written at the first verified install");
    CHECK(no_stage(), "no staging folder left");
    CHECK(F.load_calls == 2, "the load check of the bridge and of ort\\ (%d)", F.load_calls);
    CHECK(fake_said(&F, P("@bridge %s", sha_of(BRIDGE1))) && fake_said(&F, "@floor 1.5.0"), "the records for the app");
    CHECK(exists(P("%s/lock", ROOT)), "the lock file stays (only the handle on it is the lock)");

    x = install();
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "already in place") && installed(V19, "7.2.19", BRIDGE1), "again: already in place (%d)", x);
    CHECK(F.load_calls == 4, "already in place: its load checks run again (A3)");
    CHECK(holds(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A)), WORKER_A), "a run from the app folder's copy never removes bin\\'s worker");

    /* Reinstall (force) with a newer bridge: the older installed bridge counts as ours (I6) */
    source("1.5.1", BRIDGE2, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE2), "an update of the bridge over ours: %d\n%s", x, F.said);
    CHECK(!fake_said(&F, "saving Pioneer's file") && holds(P("%s/pioneer/7.2.19/onnxruntime.dll", ROOT), ort_of("7.2.19")),
          "the saved set kept as it was, not saved again over ours");
    CHECK(F.load_calls == 6, "the reinstall's load checks (%d)", F.load_calls);

    /* A3: a run stopped between the swap and its load check: the next run's "already in place"
     * load-checks it, and a bridge that doesn't load is rolled back */
    F.load_fail = 1;
    x = install();
    CHECK(x == ADMIN_EXIT_ROLLED_BACK && pioneers(V19, "7.2.19") && F.load_calls == 7, "already in place, but it doesn't load: rolled back, 6 (%d)", x);
    F.load_fail = 0;

    /* B1: already in place, but ort\'s copy isn't AlphaTheta's, or is a link: never loaded, 4 */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    spits(P("%s/ort/onnxruntime.dll", ROOT), "MZ not AlphaTheta's, not listed");
    F.load_calls = 0;
    x = install();
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && F.load_calls == 0 && !fake_said(&F, "already in place") && holds(P("%s/onnxruntime.dll", V19), BRIDGE1),
          "an unsigned ort\\ copy: refused before any load (%d, %d loads)", x, F.load_calls);
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    sh(P("mv '%s/ort/onnxruntime.dll' '%s/ort-real.dll' && ln -s '%s/ort-real.dll' '%s/ort/onnxruntime.dll'", ROOT, S, S, ROOT));
    F.load_calls = 0;
    x = install();
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && F.load_calls == 0 && !fake_said(&F, "already in place"), "a linked ort\\ copy: refused before any load (%d, %d loads)", x, F.load_calls);
    sh(P("rm -f '%s/ort-real.dll'", S));
}

static void t_refusals_before_change(void) {
    /* the lock */
    fresh();
    mkdir(ROOT, 0755);
    int fd = open(P("%s/lock", ROOT), O_RDWR | O_CREAT, 0644);
    flock(fd, LOCK_EX);
    int x = install();
    CHECK(x == ADMIN_EXIT_ALREADY_RUNNING && pioneers(V19, "7.2.19"), "another run holds the lock: 7 (%d)", x);
    close(fd);

    /* Pioneer's updater, rekordbox open, the linger (PLAN R1) */
    fresh();
    fake_proc(&F, "Upmgr rekordbox.exe", "");
    CHECK(install() == ADMIN_EXIT_INSTALLER_RUNNING && pioneers(V19, "7.2.19"), "Pioneer's updater runs: 8");
    fresh();
    fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V19));
    F.windows = 1;
    x = install();
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && F.sleeps == 0 && !fake_said(&F, "@still-closing") && !fake_said(&F, "@closed"),
          "rekordbox open with a window: 5 at once (%d)", x);
    F.windows = 0;
    F.rekordbox_exits_after = 3;
    x = install();
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "@still-closing") && fake_said(&F, "@closed") && F.sleeps == 3,
          "rekordbox still closing: waited for, @closed, then installed (%d, %d sleeps)", x, F.sleeps);
    fresh();
    fake_proc(&F, "rekordbox.exe", "");
    x = install();
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && F.clock >= 75000 && pioneers(V19, "7.2.19"), "a rekordbox still there after the linger: 5 (%d)", x);
    fresh();
    F.procs_fail = 1;
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED, "the process list can't be had: 4");
    fresh();
    F.mapped = 1;
    x = install();
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && pioneers(V19, "7.2.19") && !exists(P("%s/onnxruntime.dll.rbsp-new", V19)),
          "the live DLL mapped in another session: 5, nothing changed (%d)", x);

    /* rekordbox missing, not Pioneer's, another version than checked */
    fresh();
    F.n_entries = 0;
    CHECK(install() == ADMIN_EXIT_REKORDBOX_MISSING, "no rekordbox: 9");
    fresh();
    CHECK(run_verb(ADMIN_INSTALL, "7.2.18", 0) == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "not 7.2.18 as checked"), "another version than checked: 4");
    fresh();
    spits(P("%s/rekordbox.exe", V19), "MZ not signed");
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS, "rekordbox.exe not AlphaTheta's: 10");
    fresh();
    F.pin_error = 1;
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED, "the pin can't answer: 4");
    fresh();
    F.bad_acl[F.n_bad_acl++] = V19;
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS && pioneers(V19, "7.2.19"), "rekordbox's folder writable by others: 10");
    fresh();
    F.bad_acl[F.n_bad_acl++] = P("%s/onnxruntime.dll", V19);
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS, "the live DLL writable by others: 10");
    fresh();
    sh(P("mv '%s' '%s.real' && ln -s '%s.real' '%s'", V19, V19, V19, V19));
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS || install() == ADMIN_EXIT_REKORDBOX_MISSING, "a link in place of the version folder: refused");

    /* the root */
    fresh();
    mkdir(ROOT, 0755);
    F.root_not_exact = 1;
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && pioneers(V19, "7.2.19"), "an existing root whose DACL isn't exactly ours: 4");
    fresh();
    sh(P("mkdir -p '%s.elsewhere' && ln -s '%s.elsewhere' '%s'", ROOT, ROOT, ROOT));
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED, "a link in place of the root: 4");
    sh(P("rm -f '%s'", ROOT));
    fresh();
    sh(P("mkdir -p '%s/pioneer.real' && ln -s '%s/pioneer.real' '%s/pioneer'", ROOT, ROOT, ROOT));
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && pioneers(V19, "7.2.19"), "a link in place of pioneer\\: 4");

    /* the write probe */
    fresh();
    chmod(V19, 0555);
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "couldn't write into rekordbox"), "rekordbox's folder refuses a write: 4");
    chmod(V19, 0755);
}

static void t_payload(void) {
    /* the order: size cap, signature, JSON, the floor (payload.c), all on the staged copy */
    fresh();
    spits(P("%s/payload.json.sig", SRC), "not a signature");
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && pioneers(V19, "7.2.19") && !exists(P("%s/payload-version", ROOT)), "a bad signature: 4, no floor");
    fresh();
    char *big = malloc(PAYLOAD_MAX + 2);
    memset(big, ' ', PAYLOAD_MAX + 1);
    big[PAYLOAD_MAX + 1] = 0;
    spits(P("%s/payload.json", SRC), big);
    free(big);
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "over"), "payload.json over the cap: refused before anything else");
    fresh();
    source("1.5.0", BRIDGE1, WORKER_A, 0, "rbstemsplus-bridge-win-x64.dll");
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "no Windows part"), "no windows block: 4");
    fresh();
    source("1.4.9", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    int x = install();
    CHECK(x == ADMIN_EXIT_OLDER && fake_said(&F, "@floor 1.5.0"), "below the compiled minimum (J18): the floor's own exit (%d)", x);
    fresh();
    source("1.5.9-beta.1", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && pioneers(V19, "7.2.19") && !exists(P("%s/payload-version", ROOT)),
          "a signed suffixed payload is refused: 4, nothing written, no floor");
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "1.5.0 installs");
    source("1.5.8", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    CHECK(run_verb(ADMIN_INSTALL, "7.2.19", 1) == ADMIN_EXIT_OK && fake_said(&F, "@floor 1.5.8"), "a newer payload raises the floor");
    source("1.5.5", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    CHECK(run_verb(ADMIN_INSTALL, "7.2.19", 1) == ADMIN_EXIT_OLDER, "then an older final is refused by the floor: 14 (J23)");
    CHECK(run_verb(ADMIN_INSTALL_WORKER, "", 0) == ADMIN_EXIT_OLDER, "install-worker too");

    fresh();
    source("1.5.0", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    spits(P("%s/rbstemsplus-bridge-win-x64.dll", SRC), BRIDGE2);   /* not the bridge payload.json names */
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "checksum doesn't match") && pioneers(V19, "7.2.19"),
          "the bridge's sha256 from the verified payload, never argv: 4");
    fresh();
    source("1.5.0", BRIDGE1, WORKER_A, 1, "..");
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED, "a bridge name that isn't a plain name: 4");
    fresh();
    sh(P("mv '%s/rbstemsplus-bridge-win-x64.dll' '%s/real.dll' && ln -s '%s/real.dll' '%s/rbstemsplus-bridge-win-x64.dll'", SRC, SRC, SRC, SRC));
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && pioneers(V19, "7.2.19"), "a link in the source folder is never followed");
    fresh();
    sh(P("mv '%s/payload.json' '%s/p.json' && ln -s '%s/p.json' '%s/payload.json'", SRC, SRC, SRC, SRC));
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED, "payload.json a link: 4");
    fresh();
    sh(P("dd if=/dev/zero of='%s/rbstemsplus-bridge-win-x64.dll' bs=1048576 count=33 2>/dev/null", SRC));
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "over 32 MB"), "a bridge over 32 MB: 4");
    fresh();
    spits(P("%s/admin/RB Stems Plus.exe", SRC), WORKER_B);   /* not the worker payload.json names */
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && pioneers(V19, "7.2.19") && !exists(P("%s/bin/%s", ROOT, sha12_of(WORKER_B))),
          "the first install's worker checked against admin_sha256: 4");
}

static void t_classification(void) {
    /* ORT not listed */
    fresh();
    spits(P("%s/onnxruntime.dll", V19), "MZ onnxruntime other ALPHATHETA-SIGNED");
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "isn't one this release supports"), "Pioneer's ORT not listed: 4");
    /* foreign, no saved set */
    fresh();
    spits(P("%s/onnxruntime.dll", V19), "MZ somebody else's");
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS && !exists(P("%s/bin/%s", ROOT, sha12_of(WORKER_A))), "neither Pioneer's nor ours, no saved set: 10, no worker put in bin\\");
    /* a bridge-like file with no saved set */
    fresh();
    spits(P("%s/onnxruntime.dll", V19), BRIDGE1);
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS, "a bridge with no saved set: 10");
    /* the live DLL missing, no saved set (R1, Defender) */
    fresh();
    unlink(P("%s/onnxruntime.dll", V19));
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS, "no live DLL and no saved set: 10");

    /* a half-run: the saved set complete, the live file a stranger's: put back first, then in */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    spits(P("%s/onnxruntime.dll", V19), "MZ half written");
    int x = install();
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "stopped half-way") && installed(V19, "7.2.19", BRIDGE1), "a stopped run's half state is repaired, then installed (%d)\n%s", x,
          F.said);
    /* the live DLL gone (Defender), the set complete: put back by a no-replace rename (I71), then in */
    unlink(P("%s/onnxruntime.dll", V19));
    x = install();
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "is missing") && installed(V19, "7.2.19", BRIDGE1), "the live DLL absent: put back, then installed (%d)", x);
    /* an unlisted bridge of ours with a complete set (an older build): a half-run, put back */
    spits(P("%s/installed-bridge", ROOT), P("%s\n", sha_of("x")));
    spits(P("%s/onnxruntime.dll", V19), "MZ old bridge rbstems bridge: 0");
    x = install();
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1), "an unlisted bridge with a complete set: repaired, then installed (%d)", x);
    /* a damaged saved set: ours is still ours (listed) but force needs a complete set */
    spits(P("%s/pioneer/7.2.19/SHA256SUMS", ROOT), "nonsense\n");
    x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
    CHECK(x == ADMIN_EXIT_NOT_PIONEERS && holds(P("%s/onnxruntime.dll", V19), BRIDGE1), "ours with a damaged saved set: refused, nothing changed (%d)", x);
}

static void corrupt_saved(fake *f) {
    (void)f;
    spits(P("%s/pioneer/7.2.19/onnxruntime.dll", ROOT), "MZ damaged");
}

static void t_rollback(void) {
    /* the load check fails after the swap: the one put-back routine puts Pioneer's file back */
    fresh();
    F.load_fail = 1;
    int x = install();
    CHECK(x == ADMIN_EXIT_ROLLED_BACK && pioneers(V19, "7.2.19") && holds(P("%s/pioneer/7.2.19/onnxruntime.dll", ROOT), ort_of("7.2.19")),
          "the load check fails: rolled back, 6; the saved set complete (%d)\n%s", x, F.said);
    CHECK(!exists(P("%s/onnxruntime.dll.rbsp-new", V19)) && !exists(P("%s/installed-bridge", ROOT)) && no_stage(), "nothing left behind");
    /* and the put-back fails too: the saved copy damaged during the run */
    fresh();
    F.load_fail = 1;
    F.on_load = corrupt_saved;
    x = install();
    CHECK(x == ADMIN_EXIT_ROLLBACK_FAILED && holds(P("%s/onnxruntime.dll", V19), BRIDGE1),
          "a bridge that failed its load check and couldn't be put back: 11, never \"nothing changed\" (A8: %d)", x);
}

static void t_uninstall(void) {
    /* from bin\, the POSIX branch: everything gone */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    int x = run_verb(ADMIN_UNINSTALL, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && !exists(ROOT), "uninstall: Pioneer's file back, the root gone (%d)\n%s", x, F.said);
    CHECK(!fake_said(&F, "@restart"), "no restart needed");
    /* the reboot branch (J7): the image renamed to *.del, it and its folders scheduled child first */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    F.reboot = 1;
    x = run_verb(ADMIN_UNINSTALL, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "@restart") && fake_said(&F, "next restart"), "the reboot branch is told (%d)", x);
    CHECK(F.n_scheduled == 4 && strstr(F.scheduled[0], ".del") && !strcmp(F.scheduled[3], ROOT) && strstr(F.scheduled[1], sha12_of(WORKER_A)),
          "scheduled: the renamed image, bin\\<sha12>\\, bin\\, the root, child first");
    CHECK(!exists(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A))) && !exists(P("%s/pioneer", ROOT)) && !exists(P("%s/lock", ROOT)),
          "everything else removed now");
    /* a reinstall before the restart: the *.del doesn't count as a worker: a first install */
    snprintf(F.self, sizeof F.self, "%s/admin/RB Stems Plus.exe", APP);
    F.reboot = 0;
    x = install();
    CHECK(x == ADMIN_EXIT_OK && holds(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A)), WORKER_A), "a reinstall before the restart is a first install (%d)", x);
    CHECK(!exists(F.scheduled[0]), "and it removes the leftover *.del");
    /* no saved set: 3 */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    sh(P("rm -rf '%s/pioneer/7.2.19'", ROOT));
    x = run_verb(ADMIN_UNINSTALL, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_NO_ORIGINALS && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && exists(ROOT), "no saved set: 3, nothing removed (%d)", x);
    /* Pioneer's already (rekordbox's update replaced the bridge), no root: nothing to do */
    fresh();
    CHECK(run_verb(ADMIN_UNINSTALL, "7.2.19", 0) == ADMIN_EXIT_OK && !exists(ROOT), "nothing of ours: 0, no root made");
    /* rekordbox open: 5 before anything changes */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    F.mapped = 1;
    CHECK(run_verb(ADMIN_UNINSTALL, "7.2.19", 0) == ADMIN_EXIT_REKORDBOX_OPEN && holds(P("%s/onnxruntime.dll", V19), BRIDGE1), "mapped: 5");
}

static void t_restore(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    int x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && exists(P("%s/pioneer/7.2.19/SHA256SUMS", ROOT)), "restore: Pioneer's file back, the root kept (%d)", x);
    CHECK(run_verb(ADMIN_RESTORE, "7.2.19", 0) == ADMIN_EXIT_OK && fake_said(&F, "already"), "restore when it is Pioneer's: 0");
    /* I71: the live DLL deleted (Defender, R1), then restore */
    unlink(P("%s/onnxruntime.dll", V19));
    x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && fake_said(&F, "is missing"), "restore with no live DLL: moved into place (%d)", x);
    /* a foreign file, the set complete */
    spits(P("%s/onnxruntime.dll", V19), "MZ quarantined and replaced");
    CHECK(run_verb(ADMIN_RESTORE, "7.2.19", 0) == ADMIN_EXIT_OK && pioneers(V19, "7.2.19"), "restore over a foreign file");
    /* no saved set */
    fresh();
    spits(P("%s/onnxruntime.dll", V19), "MZ quarantined and replaced");
    CHECK(run_verb(ADMIN_RESTORE, "7.2.19", 0) == ADMIN_EXIT_NO_ORIGINALS, "restore with no saved set: 3");
}

static void t_versions(void) {
    /* rekordbox updated to 7.2.20 (side by side): installing there puts 7.2.19's file back first,
     * then deletes its saved set (one bridged version at a time) */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    version_folder(V20, "7.2.20", 1);
    int x = run_verb(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V20, "7.2.20", BRIDGE1), "installed into 7.2.20 (%d)\n%s", x, F.said);
    CHECK(pioneers(V19, "7.2.19") && !exists(P("%s/pioneer/7.2.19", ROOT)), "7.2.19's file back, its saved set deleted");
    /* uninstall puts back every version folder that holds our bridge */
    sh(P("cp '%s/onnxruntime.dll' '%s/onnxruntime.dll'", V20, V19));   /* a bridge in 7.2.19 again, with no saved set */
    x = run_verb(ADMIN_UNINSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_NO_ORIGINALS && exists(ROOT), "a bridged version without its saved set stops the uninstall: 3 (%d)", x);
    spits(P("%s/onnxruntime.dll", V19), ort_of("7.2.19"));
    x = run_verb(ADMIN_UNINSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V20, "7.2.20") && pioneers(V19, "7.2.19") && !exists(ROOT), "then uninstall: every version Pioneer's, the root gone (%d)", x);
}

/* the review's recovery cases (A2, A9, A13, A14, A11, A5, A6) */
static void t_other_versions(void) {
    /* A2: another version's live DLL gone (Defender): its file is put back, then its set goes */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    unlink(P("%s/onnxruntime.dll", V19));
    version_folder(V20, "7.2.20", 1);
    int x = run_verb(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && !exists(P("%s/pioneer/7.2.19", ROOT)) && installed(V20, "7.2.20", BRIDGE1),
          "an absent live DLL in another version: Pioneer's file back first, only then its set deleted (%d)\n%s", x, F.said);
    /* A2: a stranger's file there, the set complete: a half-run, put back */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    spits(P("%s/onnxruntime.dll", V19), "MZ somebody else's");
    version_folder(V20, "7.2.20", 1);
    x = run_verb(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && !exists(P("%s/pioneer/7.2.19", ROOT)), "a stranger's file in another version: put back (%d)", x);
    /* A2: another version's live DLL that can't be read: left, and its saved set kept */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    chmod(P("%s/onnxruntime.dll", V19), 0);
    version_folder(V20, "7.2.20", 1);
    x = run_verb(ADMIN_INSTALL, "7.2.20", 0);
    chmod(P("%s/onnxruntime.dll", V19), 0644);
    CHECK(x == ADMIN_EXIT_OK && holds(P("%s/pioneer/7.2.19/onnxruntime.dll", ROOT), ort_of("7.2.19")) && fake_said(&F, "couldn't check rekordbox 7.2.19"),
          "an unreadable live DLL in another version: its saved set kept (%d)", x);
    /* A9: another version bridged with no complete saved set: stop before the current one changes */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    sh(P("rm -rf '%s/pioneer/7.2.19'", ROOT));
    version_folder(V20, "7.2.20", 1);
    x = run_verb(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && pioneers(V20, "7.2.20") && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && fake_said(&F, "reinstall rekordbox 7.2.19"),
          "another bridged version that can't be put back: 4, the current one untouched (%d)", x);
    CHECK(holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.19")), "ort\\ untouched");
    /* A9: another version that can't be checked but has a saved set: stop */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    version_folder(V20, "7.2.20", 1);
    F.bad_acl[F.n_bad_acl++] = V19;
    x = run_verb(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && pioneers(V20, "7.2.20") && exists(P("%s/pioneer/7.2.19", ROOT)), "an unchecked version with a saved set: 4 (%d)", x);
    /* B2: another version's folder behind a link, with a saved set: stop like a folder not held */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    sh(P("mv '%s' '%s/elsewhere' && ln -s '%s/elsewhere' '%s'", V19, S, S, V19));
    version_folder(V20, "7.2.20", 1);
    x = run_verb(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && pioneers(V20, "7.2.20") && exists(P("%s/pioneer/7.2.19", ROOT)) && holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.19")),
          "a linked version folder with a saved set: 4, nothing changed (%d)\n%s", x, F.said);
    sh(P("rm -f '%s' && rm -rf '%s/elsewhere'", V19, S));
    /* B4: a version folder gone: its saved set goes */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    sh(P("rm -rf '%s'", V19));
    version_folder(V20, "7.2.20", 1);
    x = run_verb(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && !exists(P("%s/pioneer/7.2.19", ROOT)) && installed(V20, "7.2.20", BRIDGE1), "a version folder gone: its saved set deleted (%d)", x);
    /* B4's contrast: the folder there, NOTHING (no live DLL, saved set incomplete): set kept */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    unlink(P("%s/onnxruntime.dll", V19));
    unlink(P("%s/pioneer/7.2.19/SHA256SUMS", ROOT));
    version_folder(V20, "7.2.20", 1);
    x = run_verb(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && exists(P("%s/pioneer/7.2.19", ROOT)), "a version folder there, nothing of ours: its saved files kept (%d)\n%s", x, F.said);
}

static void t_put_back_changed(void) {
    /* A13: a put-back that changed the folder and didn't verify, twice: 11, not "nothing changed" */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    F.pin_ok_left = 4;   /* rekordbox.exe twice, the saved original, the new file; then every pin NO */
    int x = run_verb(ADMIN_UNINSTALL, "7.2.19", 0);
    F.pin_ok_left = 0;
    CHECK(x == ADMIN_EXIT_ROLLBACK_FAILED && exists(ROOT), "the put-back changed the folder and doesn't verify: 11, the root kept (%d)\n%s", x, F.said);
}

static void t_temp_files(void) {
    /* A14: a stopped install's .rbsp-new goes in an uninstall or a restore, whatever the state */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    CHECK(run_verb(ADMIN_RESTORE, "7.2.19", 0) == ADMIN_EXIT_OK, "restored");
    spits(P("%s/onnxruntime.dll.rbsp-new", V19), BRIDGE1);
    CHECK(run_verb(ADMIN_RESTORE, "7.2.19", 0) == ADMIN_EXIT_OK && !exists(P("%s/onnxruntime.dll.rbsp-new", V19)), "restore over Pioneer's: the temp file gone");
    spits(P("%s/onnxruntime.dll.rbsp-new", V19), BRIDGE1);
    spits(P("%s/onnxruntime.dll.rbsp-probe", V19), "");
    CHECK(run_verb(ADMIN_UNINSTALL, "7.2.19", 0) == ADMIN_EXIT_OK && !exists(P("%s/onnxruntime.dll.rbsp-new", V19)) && !exists(P("%s/onnxruntime.dll.rbsp-probe", V19)),
          "uninstall over Pioneer's: the temp files gone");
}

static void t_removal_folders(void) {
    /* A11: folders that can't go now are scheduled (@restart); ones that can't be either are said */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    F.rmdir_fail = 1;
    int x = run_verb(ADMIN_UNINSTALL, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "@restart") && F.n_scheduled == 3 && !fake_said(&F, "stay"), "folders left: scheduled, @restart said (%d)", x);
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    F.rmdir_fail = F.schedule_fail = 1;
    x = run_verb(ADMIN_UNINSTALL, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "some empty folders") && !fake_said(&F, "@restart") && !fake_said(&F, "removed "),
          "folders neither removed nor scheduled: said, never \"removed\" (%d)", x);
}

static void t_worker_replace_fails(void) {
    /* A5: install-worker over a damaged worker of the same name whose replacement fails: refused,
     * no double free (ASan) */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    source("1.6.0", BRIDGE1, WORKER_B, 1, "rbstemsplus-bridge-win-x64.dll");
    const char *d = P("%s/bin/%s", ROOT, sha12_of(WORKER_B));
    sh(P("mkdir -p '%s'", d));
    spits(P("%s/RB Stems Plus.exe", d), "MZ damaged");
    chmod(d, 0555);
    int x = run_verb(ADMIN_INSTALL_WORKER, "", 0);
    chmod(d, 0755);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && holds(P("%s/RB Stems Plus.exe", d), "MZ damaged"), "the replacement refused: 4 (%d)", x);

    /* A6's rule: the lock's descriptor, exactly */
    admin_ace ok[2] = {{0, 0, ADMIN_ROOT_FULL, ADMIN_SID_SYSTEM}, {0, 0, ADMIN_ROOT_FULL, ADMIN_SID_ADMINS}};
    admin_ace users[3] = {ok[0], ok[1], {0, 0, ADMIN_ROOT_READ, ADMIN_SID_USERS}};
    admin_ace inherit[2] = {{0, ADMIN_ROOT_INHERIT, ADMIN_ROOT_FULL, ADMIN_SID_SYSTEM}, ok[1]};
    CHECK(admin_lock_acl_exact(ADMIN_SID_ADMINS, 1, 1, ok, 2), "the lock: SYSTEM and Administrators only");
    CHECK(!admin_lock_acl_exact(ADMIN_SID_ADMINS, 1, 1, users, 3) && !admin_lock_acl_exact(ADMIN_SID_ADMINS, 0, 1, ok, 2)
              && !admin_lock_acl_exact(ADMIN_SID_USERS, 1, 1, ok, 2) && !admin_lock_acl_exact(ADMIN_SID_ADMINS, 1, 0, ok, 2)
              && !admin_lock_acl_exact(ADMIN_SID_ADMINS, 1, 1, inherit, 2),
          "a lock Users can read, unprotected, not Administrators', without a DACL or inheritable: made again");
}

static void t_install_worker(void) {
    /* a bin\ worker installs the app's own worker (I75): staged, hash-checked, "run again" */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed with WORKER_A");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    sh(P("mkdir -p '%s/bin/0123456789ab' && echo old > '%s/bin/0123456789ab/RB Stems Plus.exe'", ROOT, ROOT));
    source("1.6.0", BRIDGE1, WORKER_B, 1, "rbstemsplus-bridge-win-x64.dll");
    int x = run_verb(ADMIN_INSTALL_WORKER, "", 0);
    CHECK(x == ADMIN_EXIT_RUN_AGAIN && holds(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_B)), WORKER_B), "install-worker: 13, the new worker in bin\\ (%d)\n%s", x,
          F.said);
    CHECK(exists(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A))) && !exists(P("%s/bin/0123456789ab", ROOT)), "the running worker stays; older ones go");
    CHECK(holds(P("%s/onnxruntime.dll", V19), BRIDGE1), "rekordbox untouched");
    /* the new worker's first run deletes the old one */
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_B));
    x = run_verb(ADMIN_INSTALL, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && !exists(P("%s/bin/%s", ROOT, sha12_of(WORKER_A))), "later runs delete older worker folders (%d)", x);
    /* a source worker that isn't payload.json's */
    spits(P("%s/admin/RB Stems Plus.exe", SRC), WORKER_A);
    CHECK(run_verb(ADMIN_INSTALL_WORKER, "", 0) == ADMIN_EXIT_CHECK_FAILED, "install-worker checks admin_sha256: 4");
}

static void t_remove_root_folder(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    CHECK(run_verb(ADMIN_REMOVE_ROOT_FOLDER, "", 0) == ADMIN_EXIT_CHECK_FAILED && exists(ROOT), "rekordbox installed: the root stays, 4");
    F.n_entries = 0;
    sh(P("rm -rf '%s'", V19));
    int x = run_verb(ADMIN_REMOVE_ROOT_FOLDER, "", 0);
    CHECK(x == ADMIN_EXIT_OK && !exists(ROOT), "rekordbox gone: the root removed (%d)\n%s", x, F.said);
    CHECK(run_verb(ADMIN_REMOVE_ROOT_FOLDER, "", 0) == ADMIN_EXIT_OK, "no root: nothing to remove");
}

static void t_leftovers(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    sh(P("mkdir -p '%s/stage.deadbeef/x' && echo x > '%s/stage.deadbeef/x/y'", ROOT, ROOT));
    spits(P("%s/onnxruntime.dll.rbsp-new", V19), "half");
    spits(P("%s/bin/%s/RB Stems Plus.exe.1234.del", ROOT, sha12_of(WORKER_A)), "old image");
    CHECK(install() == ADMIN_EXIT_OK, "a run after a stopped one");
    CHECK(no_stage() && !exists(P("%s/onnxruntime.dll.rbsp-new", V19)) && !exists(P("%s/bin/%s/RB Stems Plus.exe.1234.del", ROOT, sha12_of(WORKER_A))),
          "its staging folder, temp file and *.del removed");
}

void test_worker(const char *scratch, const char *pedir) {
    if (!realpath(scratch, S)) abort();
    snprintf(RB, sizeof RB, "%s/rekordbox", S);
    snprintf(V19, sizeof V19, "%s/rekordbox 7.2.19", RB);
    snprintf(V20, sizeof V20, "%s/rekordbox 7.2.20", RB);
    snprintf(ROOT, sizeof ROOT, "%s/RB Stems Plus", S);
    snprintf(SRC, sizeof SRC, "%s/src", S);
    snprintf(APP, sizeof APP, "%s/app", S);
    size_t n;
    unsigned char *pe = slurp(P("%s/rekordbox-x64.exe", pedir), &n);
    if (!pe) {
        fprintf(stderr, "no PE fixture in %s\n", pedir);
        abort();
    }
    exe = malloc(n + 17);
    memcpy(exe, pe, n);
    memcpy(exe + n, "ALPHATHETA-SIGNED", 17);   /* the fake pin's mark, past the image */
    exe_n = n + 17;
    free(pe);
    if (test_key_new(&key)) abort();
    keys[0].name = "release";
    memcpy(keys[0].xy, key.xy, 64);

    t_first_install();
    t_refusals_before_change();
    t_payload();
    t_classification();
    t_rollback();
    t_uninstall();
    t_restore();
    t_versions();
    t_install_worker();
    t_remove_root_folder();
    t_leftovers();
    t_other_versions();
    t_put_back_changed();
    t_temp_files();
    t_removal_folders();
    t_worker_replace_fails();
    test_key_free(&key);
    free(exe);
}
