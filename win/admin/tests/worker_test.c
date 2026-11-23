/* The worker's runs on the Mac (app.md §7: RootScripts' scenarios become the worker's test list,
 * plus the admin floor and the protocol), and the multi-version batch (.orchestration/mv-spec.md
 * §2.2, D1, D4-D7, D13): real files in a scratch tree through plat_mac.c, compat.c, payload.c and
 * receipt.c, with the fake OS (fake_os.c), the fake inventory and PE code digest (fake_inventory.c,
 * fake_pe_code.c: lane A's and B0's code stands in their place) and the test crypto shim's keys. */
#include "../../c/tests/check.h"

#include "../../c/receipt.h"
#include "../../c/tests/sig_mac.h"
#include "fake_os.h"

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

void test_worker(const char *scratch, const char *pedir);

static char S[1024], RB[1100], V19[1200], V20[1200], V21[1200], ROOT[1100], SRC[1100], APP[1100];
static unsigned char *exe;   /* the PE fixture with rekordbox's version resource */
static size_t exe_n;
static test_key key;
static sig_pubkey keys[1];
static fake F;

static const char BRIDGE1[] = "MZ bridge one rbstems bridge: 1 rbstems_marker";
static const char BRIDGE2[] = "MZ bridge two rbstems bridge: 2 rbstems_marker";
static const char WORKER_A[] = "MZ worker A rbsp-admin worker: proto=2 minimum=1.5.0\n";
static const char WORKER_B[] = "MZ worker B rbsp-admin worker: proto=2 minimum=1.5.0\n";
static const char WORKER_OLD[] = "MZ worker old rbsp-admin worker: proto=1 minimum=1.5.0\n";

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
/* the sha256 of a file, "" when unreadable */
static const char *sha_file(const char *path) {
    static char o[4][65];
    static int at;
    char *r = o[at++ & 3];
    size_t n;
    unsigned char *b = slurp(path, &n);
    if (b) sha_hex(b, n, r);
    else r[0] = 0;
    free(b);
    return r;
}

static void sh(const char *cmd) {
    if (system(cmd)) {
        fprintf(stderr, "failed: %s\n", cmd);
        abort();
    }
}

/* Pioneer's ORT of version v: "signed", unique per version, the same code (D2a) */
static const char *ort_of(const char *v) {
    char stock[96];
    fake_stock_dll(v, stock, sizeof stock);
    return P("%s", stock);
}

static const char *vdir_of(const char *v) { return !strcmp(v, "7.2.19") ? V19 : !strcmp(v, "7.2.20") ? V20 : V21; }

static void add_install(fake_install install) {
    const fake_inventory inventory = {RB, ROOT, exe, exe_n, BRIDGE1};
    fake_inventory_add(&F, &inventory, install);
}

/* -- the payload: the versions it lists and the originals it binds (mv-spec §2.3) */

/* the source folder: a signed payload.json (payload_version pv) naming bridge and worker, listing
 * versions (comma-separated) with each one's original bound (bindings 0: none, a legacy payload);
 * windows 0: no windows block */
static void source_full(const char *pv, const char *bridge, const char *worker, int windows, const char *bridge_name, const char *versions, int bindings) {
    char bsha[65], wsha[65], list[256] = "", orts[1024] = "", binds[1024] = "";
    sha_hex(bridge, strlen(bridge), bsha);
    sha_hex(worker, strlen(worker), wsha);
    char code[65];
    fake_stock_code(code);
    char vs[256];
    snprintf(vs, sizeof vs, "%s", versions);
    /* the ORTs listed: every fixture version's, listed or not ("an allowed ORT") */
    snprintf(orts, sizeof orts, "\"%s\", \"%s\", \"%s\"", sha_of(ort_of("7.2.19")), sha_of(ort_of("7.2.20")), sha_of(ort_of("7.2.21")));
    for (char *t = strtok(vs, ","); t; t = strtok(NULL, ",")) {
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s\"%s\"", list[0] ? ", " : "", t);
        snprintf(binds + strlen(binds), sizeof binds - strlen(binds), "%s{\"rekordbox\": \"%s\", \"sha256\": \"%s\"}", binds[0] ? ", " : "", t,
                 sha_of(ort_of(t)));
    }
    sh(P("rm -rf '%s' && mkdir -p '%s/admin'", SRC, SRC));
    char json[8192], ortkeys[2048] = "";
    if (bindings) snprintf(ortkeys, sizeof ortkeys, ", \"ort\": [%s], \"ort_code_sha256\": \"%s\"", binds, code);
    snprintf(json, sizeof json,
             "{\"payload_version\": \"%s\", "
             "\"app\": {\"version\": \"1.0.0\", \"zip\": \"a.zip\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"bridge\": {\"file\": \"b.dylib\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"compatible\": {\"rekordbox\": [], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": []}, "
             "\"model\": {\"file\": \"m.onnx\", \"sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\", \"size\": 9}",
             pv);
    if (windows)
        snprintf(json + strlen(json), sizeof json - strlen(json),
                 ", \"windows\": {\"app\": {\"version\": \"%s\", \"zip\": \"w.zip\", "
                 "\"sha256\": \"1111111111111111111111111111111111111111111111111111111111111111\", \"admin_sha256\": \"%s\", "
                 "\"stub_sha256\": {\"x64\": \"3333333333333333333333333333333333333333333333333333333333333333\", "
                 "\"arm64\": \"4444444444444444444444444444444444444444444444444444444444444444\"}}, "
                 "\"bridge\": {\"file\": \"%s\", \"sha256\": \"%s\"}, "
                 "\"compatible\": {\"rekordbox\": [%s], \"ort_sha256\": [%s], \"stems_engine\": [\"0002\"]%s}}",
                 pv, wsha, bridge_name, bsha, list, orts, ortkeys);
    snprintf(json + strlen(json), sizeof json - strlen(json), "}");
    spits(P("%s/payload.json", SRC), json);
    unsigned char sig[SIG_DER_MAX];
    size_t sn = test_sign(&key, json, strlen(json), sig);
    spit(P("%s/payload.json.sig", SRC), sig, sn);
    if (admin_plain_name_ok(bridge_name)) spits(P("%s/%s", SRC, bridge_name), bridge);
    spits(P("%s/admin/RB Stems Plus.exe", SRC), worker);
}

static void source(const char *pv, const char *bridge, const char *worker, int windows, const char *bridge_name) {
    source_full(pv, bridge, worker, windows, bridge_name, "7.2.19,7.2.20,7.2.21", 1);
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
    add_install((fake_install){.version = "7.2.19", .views = FAKE_REG_64});
    source("1.5.0", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    spits(P("%s/admin/RB Stems Plus.exe", APP), WORKER_A);
    snprintf(F.self, sizeof F.self, "%s/admin/RB Stems Plus.exe", APP);
}

/* -- runs: the app's --targets (each version's rekordbox.exe's first 16 hex digits) and
 * --manifest (the source's payload.json), as the app sends them */

static int run_args(admin_args *a) {
    snprintf(a->pipe, sizeof a->pipe, "\\\\.\\pipe\\rbstemsplus-admin-0123456789abcdef0123456789abcdef");
    admin_env e = {ROOT, RB, keys, 1, "1.5.0", 75000, 1000};
    fake_clear_said(&F);
    F.procs_seen = 0;   /* on_procs counts this run's calls */
    return admin_run(&e, &F.os, a);
}

/* versions: comma-separated, ascending ("" for the verbs that sweep: *) */
static int run_targets(int verb, const char *versions, int force) {
    admin_args a;
    memset(&a, 0, sizeof a);
    a.verb = verb;
    a.proto = ADMIN_PROTO;
    a.force = force;
    if ((verb == ADMIN_INSTALL || verb == ADMIN_RESTORE) && versions[0]) {
        char vs[256];
        snprintf(vs, sizeof vs, "%s", versions);
        for (char *t = strtok(vs, ","); t; t = strtok(NULL, ",")) {
            snprintf(a.t[a.nt].version, sizeof a.t[0].version, "%s", t);
            const char *s = sha_file(P("%s/rekordbox %s/rekordbox.exe", RB, t));
            snprintf(a.t[a.nt].exe16, 17, "%.16s", s[0] ? s : "0000000000000000");
            a.nt++;
        }
    } else
        a.all = 1;
    if (verb == ADMIN_INSTALL || verb == ADMIN_INSTALL_WORKER) {
        snprintf(a.source, sizeof a.source, "%s", SRC);
        const char *m = sha_file(P("%s/payload.json", SRC));
        snprintf(a.manifest, sizeof a.manifest, "%s", m[0] ? m : "0000000000000000000000000000000000000000000000000000000000000000");
    }
    return run_args(&a);
}
static int run_verb(int verb, const char *version, int force) { return run_targets(verb, version, force); }
static int install(void) { return run_targets(ADMIN_INSTALL, "7.2.19", 0); }

/* -- the state */

static int read_receipt(const char *v, receipt *rc) { return receipt_read(ROOT, v, rc); }

/* the state after a good install of bridge into version v (folder vdir): the bridge live,
 * Pioneer's file saved with SHA256SUMS, ort\'s copy a bound original, the receipt binding them
 * (the runtime by its code digest: any bound original's, D2a) */
static int installed(const char *vdir, const char *v, const char *bridge) {
    receipt rc;
    plat_stat st;
    char id[PLAT_ID_HEX_MAX];
    if (read_receipt(v, &rc) || plat_stat_path(vdir, 0, &st)) return 0;
    plat_id_hex(st.id, id);
    char code[65];
    fake_stock_code(code);
    return holds(P("%s/onnxruntime.dll", vdir), bridge) && holds(P("%s/pioneer/%s/onnxruntime.dll", ROOT, v), ort_of(v))
           && holds(P("%s/pioneer/%s/SHA256SUMS", ROOT, v), P("%s  onnxruntime.dll\n", sha_of(ort_of(v)))) && !strcmp(rc.bridge_sha256, sha_of(bridge))
           && !strcmp(rc.original_sha256, sha_of(ort_of(v))) && !strcmp(rc.runtime_code_sha256, code) && !strcmp(rc.folder_id, id)
           && !strcmp(rc.exe_sha256, sha_file(P("%s/rekordbox.exe", vdir)))
           && !exists(P("%s/onnxruntime.dll.rbsp-new", vdir)) && !exists(P("%s/onnxruntime.dll.rbsp-probe", vdir));
}

static int has_receipt(const char *v) { return exists(P("%s/receipts/%s", ROOT, v)); }

static int no_stage(void) {
    return system(P("for d in '%s'/stage.* '%s'/undo-stage.*; do [ ! -e \"$d\" ] || exit 1; done", ROOT, ROOT)) == 0;
}

static int pioneers(const char *vdir, const char *v) { return holds(P("%s/onnxruntime.dll", vdir), ort_of(v)); }

/* a saved set of version v holding text (its SHA256SUMS agreeing) */
static void put_set(const char *v, const char *text) {
    sh(P("mkdir -p '%s/pioneer/%s'", ROOT, v));
    spits(P("%s/pioneer/%s/onnxruntime.dll", ROOT, v), text);
    spits(P("%s/pioneer/%s/SHA256SUMS", ROOT, v), P("%s  onnxruntime.dll\n", sha_of(text)));
}

/* -- cases (one folder) */

static void t_first_install(void) {
    fresh();
    int x = install();
    CHECK(x == ADMIN_EXIT_OK, "the first install: %d\n%s", x, F.said);
    CHECK(installed(V19, "7.2.19", BRIDGE1), "the bridge in, Pioneer's file saved, ort\\'s copy, the receipt binding them");
    CHECK(holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.19")), "ort\\: the first target's original");
    CHECK(holds(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A)), WORKER_A), "the first install puts the worker into bin\\<sha12>\\ (I75, I78)");
    CHECK(holds(P("%s/payload-version", ROOT), "1.5.0\n") || holds(P("%s/payload-version", ROOT), "1.5.0"), "the floor written at the first verified install");
    CHECK(no_stage(), "no staging folder left");
    CHECK(F.load_calls == 2, "the load check of the bridge and of ort\\ (%d)", F.load_calls);
    CHECK(fake_said(&F, P("@bridge %s", sha_of(BRIDGE1))) && fake_said(&F, "@floor 1.5.0") && fake_said(&F, "@target 7.2.19 0 installed"), "the records for the app");
    CHECK(exists(P("%s/lock", ROOT)), "the lock file stays (only the handle on it is the lock)");

    /* done: the app's scope is empty (no UAC); a run naming it anyway is 15, nothing changed */
    x = install();
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && installed(V19, "7.2.19", BRIDGE1) && F.load_calls == 2, "a done version named again: 15, nothing changed (%d)\n%s", x,
          F.said);
    CHECK(holds(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A)), WORKER_A), "a run from the app folder's copy never removes bin\\'s worker");

    /* Reinstall (force) with a newer bridge: the older bridge, bound by its receipt, is ours (I6) */
    source("1.5.1", BRIDGE2, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE2), "a reinstall over ours: %d\n%s", x, F.said);
    CHECK(!fake_said(&F, "saving Pioneer's file") && holds(P("%s/pioneer/7.2.19/onnxruntime.dll", ROOT), ort_of("7.2.19")),
          "the saved set kept as it was, not saved again over ours");
    CHECK(F.load_calls == 4, "the reinstall's load checks (%d)", F.load_calls);

    /* a reinstall whose bridge doesn't load: rolled back, its receipt gone */
    F.load_fail = 1;
    x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
    CHECK(x == ADMIN_EXIT_ROLLED_BACK && pioneers(V19, "7.2.19") && !has_receipt("7.2.19") && fake_said(&F, "@target 7.2.19 6 put-back"),
          "a reinstall that doesn't load: rolled back, 6, no receipt (%d)", x);
    F.load_fail = 0;

    /* B1: ort\'s copy not AlphaTheta's: the receipt no longer binds it, the version needs repair
     * (never in an install scope, D13); Put Back, then install: ort\ written again */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    spits(P("%s/ort/onnxruntime.dll", ROOT), "MZ not AlphaTheta's, not listed");
    F.load_calls = 0;
    x = install();
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && F.load_calls == 0 && holds(P("%s/onnxruntime.dll", V19), BRIDGE1), "an unsigned ort\\ copy: not done, not taken: 15 (%d)",
          x);
    x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && F.load_calls == 0, "nor by a reinstall (%d)", x);
    CHECK(run_verb(ADMIN_RESTORE, "7.2.19", 0) == ADMIN_EXIT_OK && pioneers(V19, "7.2.19"), "Put Back repairs it\n%s", F.said);
    x = install();
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1) && holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.19")),
          "then install: no bridge live anywhere, ort\\ written again (%d)\n%s", x, F.said);
    /* a linked ort\ copy: never loaded */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    sh(P("mv '%s/ort/onnxruntime.dll' '%s/ort-real.dll' && ln -s '%s/ort-real.dll' '%s/ort/onnxruntime.dll'", ROOT, S, S, ROOT));
    F.load_calls = 0;
    x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && F.load_calls == 0, "a linked ort\\ copy: never loaded (%d, %d loads)", x, F.load_calls);
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

    /* Pioneer's updater, rekordbox open in any session (the system-wide gate, D6), the linger (PLAN R1) */
    fresh();
    fake_proc(&F, "Upmgr rekordbox.exe", "");
    CHECK(install() == ADMIN_EXIT_INSTALLER_RUNNING && pioneers(V19, "7.2.19"), "Pioneer's updater runs: 8");
    fresh();
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
    fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V20));
    F.windows = 1;
    x = install();
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && F.sleeps == 0 && !fake_said(&F, "@still-closing") && pioneers(V19, "7.2.19"),
          "another version of rekordbox open: 5 at once, for every folder (D6) (%d)", x);
    F.windows = 0;
    F.rekordbox_exits_after = 3;
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
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
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && pioneers(V19, "7.2.19") && !exists(P("%s/onnxruntime.dll.rbsp-new", V19)) && fake_said(&F, "@target 7.2.19 5 refused"),
          "the live DLL mapped in another session: 5, nothing changed (%d)", x);

    /* rekordbox missing, not Pioneer's, another version than checked */
    fresh();
    F.n_entries = 0;
    sh(P("rm -rf '%s'", V19));
    CHECK(install() == ADMIN_EXIT_REKORDBOX_MISSING, "no rekordbox: 9");
    fresh();
    CHECK(run_verb(ADMIN_INSTALL, "7.2.18", 0) == ADMIN_EXIT_SCOPE_CHANGED && fake_said(&F, "aren't the ones checked") && pioneers(V19, "7.2.19"),
          "another version than checked: 15");
    fresh();
    spits(P("%s/rekordbox.exe", V19), "MZ not signed");
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS, "rekordbox.exe not AlphaTheta's: 10");
    fresh();
    F.pin_error = 1;
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "couldn't check every installed version"), "the pin can't answer: incomplete, 4");
    fresh();
    F.bad_acl[F.n_bad_acl++] = V19;
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS && pioneers(V19, "7.2.19"), "rekordbox's folder writable by others: 10");
    fresh();
    F.bad_acl[F.n_bad_acl++] = P("%s/onnxruntime.dll", V19);
    CHECK(install() == ADMIN_EXIT_NOT_PIONEERS, "the live DLL writable by others: 10");
    fresh();
    sh(P("mv '%s' '%s.real' && ln -s '%s.real' '%s'", V19, V19, V19, V19));
    x = install();
    CHECK(x == ADMIN_EXIT_NOT_PIONEERS && pioneers(P("%s.real", V19), "7.2.19"), "a link in place of the version folder: refused (%d)", x);

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
    fresh();
    sh(P("mkdir -p '%s/receipts.real' && ln -s '%s/receipts.real' '%s/receipts'", ROOT, ROOT, ROOT));
    CHECK(install() == ADMIN_EXIT_CHECK_FAILED && pioneers(V19, "7.2.19"), "a link in place of receipts\\: 4");

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

    /* --manifest: the staged payload.json must be the one the app verified */
    fresh();
    admin_args a;
    memset(&a, 0, sizeof a);
    a.verb = ADMIN_INSTALL;
    a.proto = ADMIN_PROTO;
    a.nt = 1;
    snprintf(a.t[0].version, sizeof a.t[0].version, "7.2.19");
    snprintf(a.t[0].exe16, 17, "%.16s", sha_file(P("%s/rekordbox.exe", V19)));
    snprintf(a.source, sizeof a.source, "%s", SRC);
    snprintf(a.manifest, sizeof a.manifest, "%s", sha_of("another payload.json"));
    x = run_args(&a);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && pioneers(V19, "7.2.19") && fake_said(&F, "isn't the one the app checked"), "another manifest: 15, nothing changed (%d)", x);
    /* a payload without ORT bindings (mv-spec §2.3): no install */
    fresh();
    source_full("1.5.0", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll", "7.2.19,7.2.20", 0);
    x = install();
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "no ONNX Runtime bindings") && pioneers(V19, "7.2.19") && !exists(P("%s/receipts", ROOT)),
          "no bindings: 4, nothing changed (%d)", x);

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
    /* an original that isn't the payload's binding: never in the scope (15 when named anyway) */
    fresh();
    spits(P("%s/onnxruntime.dll", V19), "MZ onnxruntime other ALPHATHETA-SIGNED");
    CHECK(install() == ADMIN_EXIT_SCOPE_CHANGED && holds(P("%s/onnxruntime.dll", V19), "MZ onnxruntime other ALPHATHETA-SIGNED") && !has_receipt("7.2.19"),
          "Pioneer's ORT not bound: 15, nothing changed");
    /* foreign, no saved set */
    fresh();
    spits(P("%s/onnxruntime.dll", V19), "MZ somebody else's");
    CHECK(install() == ADMIN_EXIT_SCOPE_CHANGED && !exists(P("%s/bin/%s", ROOT, sha12_of(WORKER_A))), "neither Pioneer's nor ours, no saved set: 15, no worker in bin\\");
    /* a bridge-like file with no saved set nor receipt */
    fresh();
    spits(P("%s/onnxruntime.dll", V19), BRIDGE1);
    CHECK(install() == ADMIN_EXIT_SCOPE_CHANGED && holds(P("%s/onnxruntime.dll", V19), BRIDGE1), "a bridge without a receipt: never adopted (D13): 15");
    /* the live DLL missing, no saved set (R1, Defender) */
    fresh();
    unlink(P("%s/onnxruntime.dll", V19));
    CHECK(install() == ADMIN_EXIT_SCOPE_CHANGED, "no live DLL and no saved set: 15");

    /* a half-run: the saved set complete, the live file a stranger's: needs repair (Put Back), never
     * taken by an install */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    spits(P("%s/onnxruntime.dll", V19), "MZ half written");
    int x = install();
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && holds(P("%s/onnxruntime.dll", V19), "MZ half written"), "a stranger's file over ours: 15 (%d)", x);
    x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "stopped half-way") && installed(V19, "7.2.19", BRIDGE1),
          "a Reinstall takes it: the bound set back first, then the bridge (%d)\n%s", x, F.said);
    spits(P("%s/onnxruntime.dll", V19), "MZ half written");
    x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && !has_receipt("7.2.19") && fake_said(&F, "@target 7.2.19 0 put-back"), "Put Back: the receipt binds the set (%d)\n%s",
          x, F.said);
    CHECK(install() == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1), "then installed");
    /* the live DLL gone (Defender), the bound set complete: put back by a no-replace rename (I71), then in */
    unlink(P("%s/onnxruntime.dll", V19));
    x = install();
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "is missing") && installed(V19, "7.2.19", BRIDGE1), "the live DLL absent: put back, then installed (%d)\n%s", x, F.said);
    /* an older bridge of ours, its receipt naming it: replaced by this payload's */
    {
        static const char OLD[] = "MZ old bridge rbstems bridge: 0";
        receipt rc;
        char text[RECEIPT_MAX + 1];
        CHECK(!read_receipt("7.2.19", &rc), "the receipt");
        memcpy(rc.bridge_sha256, sha_of(OLD), 65);
        int tn = receipt_format(&rc, text, sizeof text);
        spit(P("%s/receipts/7.2.19", ROOT), text, tn > 0 ? (size_t)tn : 0);
        spits(P("%s/onnxruntime.dll", V19), OLD);
        x = install();
        CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1), "an older bridge of ours: replaced (%d)\n%s", x, F.said);
    }
    /* a damaged saved set: the receipt no longer binds it: not taken, nothing changed */
    spits(P("%s/pioneer/7.2.19/SHA256SUMS", ROOT), "nonsense\n");
    x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && holds(P("%s/onnxruntime.dll", V19), BRIDGE1), "ours with a damaged saved set: 15, nothing changed (%d)", x);
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
    CHECK(!exists(P("%s/onnxruntime.dll.rbsp-new", V19)) && !has_receipt("7.2.19") && no_stage(), "nothing left behind, no receipt");
    /* and the put-back fails too: the saved copy damaged during the run */
    fresh();
    F.load_fail = 1;
    F.on_load = corrupt_saved;
    x = install();
    CHECK(x == ADMIN_EXIT_ROLLBACK_FAILED && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && !has_receipt("7.2.19"),
          "a bridge that failed its load check and couldn't be put back: 11, never \"nothing changed\", no receipt (A8: %d)", x);
}

static void t_uninstall(void) {
    /* from bin\, the POSIX branch: everything gone */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    int x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && !exists(ROOT) && fake_said(&F, "@target 7.2.19 0 put-back"),
          "uninstall: Pioneer's file back, the root gone (%d)\n%s", x, F.said);
    CHECK(!fake_said(&F, "@restart") && !fake_said(&F, "@root-kept"), "no restart needed, the root not kept");
    /* the reboot branch (J7): the image renamed to *.del, it and its folders scheduled child first */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    F.reboot = 1;
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "@restart") && fake_said(&F, "next restart"), "the reboot branch is told (%d)", x);
    CHECK(F.n_scheduled == 4 && strstr(F.scheduled[0], ".del") && !strcmp(F.scheduled[3], ROOT) && strstr(F.scheduled[1], sha12_of(WORKER_A)),
          "scheduled: the renamed image, bin\\<sha12>\\, bin\\, the root, child first");
    CHECK(!exists(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A))) && !exists(P("%s/pioneer", ROOT)) && !exists(P("%s/lock", ROOT))
              && !exists(P("%s/receipts", ROOT)),
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
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_NO_ORIGINALS && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && exists(ROOT) && fake_said(&F, "@root-kept"),
          "no saved set: 3, the root kept (%d)", x);
    /* Pioneer's already (rekordbox's update replaced the bridge), no root: nothing to do */
    fresh();
    CHECK(run_verb(ADMIN_UNINSTALL, "", 0) == ADMIN_EXIT_OK && !exists(ROOT), "nothing of ours: 0, no root made");
    /* rekordbox open: 5 before anything changes */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    F.mapped = 1;
    CHECK(run_verb(ADMIN_UNINSTALL, "", 0) == ADMIN_EXIT_REKORDBOX_OPEN && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && exists(ROOT), "mapped: 5");
}

static void t_restore(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    int x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && exists(P("%s/pioneer/7.2.19/SHA256SUMS", ROOT)) && !has_receipt("7.2.19"),
          "restore: Pioneer's file back, the root kept, the receipt gone (%d)", x);
    CHECK(run_verb(ADMIN_RESTORE, "7.2.19", 0) == ADMIN_EXIT_OK && fake_said(&F, "@target 7.2.19 0 already"), "restore when it is Pioneer's: 0");
    /* I71: the live DLL deleted (Defender, R1), then restore: the bound set */
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
    /* another installation than the app checked (its rekordbox.exe changed): refused, 15 */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    admin_args a;
    memset(&a, 0, sizeof a);
    a.verb = ADMIN_RESTORE;
    a.proto = ADMIN_PROTO;
    a.nt = 1;
    snprintf(a.t[0].version, sizeof a.t[0].version, "7.2.19");
    snprintf(a.t[0].exe16, 17, "0123456789abcdef");
    x = run_args(&a);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && fake_said(&F, "@target 7.2.19 15 refused"),
          "restore of an installation other than checked: 15, nothing changed (%d)", x);
}

/* -- several versions (mv-spec §2.2, D1, D4, D7) */

static void two(void) {
    fresh();
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
}
static void three(void) {
    two();
    add_install((fake_install){.version = "7.2.21", .views = FAKE_REG_64});
}

static void t_batch_two_stock(void) {
    two();
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1) && installed(V20, "7.2.20", BRIDGE1), "two versions in one run (%d)\n%s", x, F.said);
    CHECK(holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.19")) && fake_said(&F, "@target 7.2.19 0 installed") && fake_said(&F, "@target 7.2.20 0 installed"),
          "one ort\\ copy (7.2.19's bytes, 7.2.20's re-signed twin shares its code: D2a), a record per target");
    CHECK(F.load_calls == 4, "each target load-checked");
    /* nothing left: a run naming them is 15; the order is the version order */
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_SCOPE_CHANGED, "done: 15");
    /* a reinstall takes both again */
    source("1.5.1", BRIDGE2, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE2) && installed(V20, "7.2.20", BRIDGE2), "a newer bridge over both (%d)", x);
    /* the uninstall: both back, the root gone */
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && pioneers(V20, "7.2.20") && !exists(ROOT), "uninstall: both Pioneer's, the root gone (%d)\n%s", x, F.said);
}

static void t_batch_three(void) {
    three();
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20,7.2.21", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1) && installed(V20, "7.2.20", BRIDGE1) && installed(V21, "7.2.21", BRIDGE1),
          "three versions in one run (%d)\n%s", x, F.said);
    /* a version added later: only it in the scope; ort\ and the others' receipts stay */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19");
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
    CHECK(run_targets(ADMIN_INSTALL, "7.2.20", 0) == ADMIN_EXIT_OK, "then 7.2.20");
    CHECK(installed(V19, "7.2.19", BRIDGE1) && installed(V20, "7.2.20", BRIDGE1) && holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.19")),
          "7.2.19's receipt still binds it: ort\\ unchanged when 7.2.20 is added (D2a)");
}

/* ort\ rewritten from another version's original (re-signed, the same code: D2a) keeps every
 * receipt matching: the receipts bind the runtime by its PE code digest, not its file sha256 */
static void t_batch_ort_rewritten(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK && holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.19")), "7.2.19, ort\\ its original");
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
    unlink(P("%s/ort/onnxruntime.dll", ROOT));
    int x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.20")) && fake_said(&F, "copied to ort"),
          "ort\\ gone, 7.2.20 added: ort\\ written from 7.2.20's original, not 7.2.19's bytes (%d)\n%s", x, F.said);
    CHECK(installed(V19, "7.2.19", BRIDGE1) && installed(V20, "7.2.20", BRIDGE1), "both receipts bind the rewritten runtime");
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 1);
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "@target 7.2.19 0 installed") && holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.20")),
          "7.2.19's bridge is still ours: a Reinstall takes both, ort\\ kept (%d)\n%s", x, F.said);
    /* a runtime of other code: no receipt binds it, so neither bridge is ours to reinstall over */
    static const char OTHER[] = "MZ other code #cert 7.2.19 ALPHATHETA-SIGNED";
    spits(P("%s/ort/onnxruntime.dll", ROOT), OTHER);
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 1);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && holds(P("%s/ort/onnxruntime.dll", ROOT), OTHER) && holds(P("%s/onnxruntime.dll", V19), BRIDGE1),
          "other code in ort\\: the receipts don't bind it, nothing changed (%d)\n%s", x, F.said);
}

static void t_batch_scope_changed(void) {
    /* a third folder appears after the app's scope */
    three();
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && pioneers(V19, "7.2.19") && pioneers(V20, "7.2.20") && !exists(P("%s/receipts", ROOT)) && no_stage(),
          "a version more than checked: 15, nothing changed (%d)\n%s", x, F.said);
    /* a target's rekordbox.exe changed (a reinstall by Pioneer) */
    two();
    admin_args a;
    memset(&a, 0, sizeof a);
    a.verb = ADMIN_INSTALL;
    a.proto = ADMIN_PROTO;
    a.nt = 2;
    snprintf(a.t[0].version, sizeof a.t[0].version, "7.2.19");
    snprintf(a.t[0].exe16, 17, "%.16s", sha_file(P("%s/rekordbox.exe", V19)));
    snprintf(a.t[1].version, sizeof a.t[1].version, "7.2.20");
    snprintf(a.t[1].exe16, 17, "ffffffffffffffff");
    snprintf(a.source, sizeof a.source, "%s", SRC);
    snprintf(a.manifest, sizeof a.manifest, "%s", sha_file(P("%s/payload.json", SRC)));
    x = run_args(&a);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && pioneers(V19, "7.2.19") && pioneers(V20, "7.2.20"), "another rekordbox.exe than checked: 15 (%d)", x);
    /* a version gone since */
    two();
    sh(P("rm -rf '%s'", V20));
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19", 0) == ADMIN_EXIT_OK, "the remaining one installs (its entry stale)");
    /* a hostile target: a named folder's rekordbox.exe that isn't AlphaTheta's */
    two();
    spits(P("%s/rekordbox.exe", V20), "MZ not signed");
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_NOT_PIONEERS && pioneers(V19, "7.2.19") && fake_said(&F, "@target 7.2.20 10 refused"), "a hostile target: 10, nothing changed (%d)", x);
}

/* the root's state a refused run must leave exactly as it was: the saved payload pair, the floor,
 * a stopped run's staging folder and renamed worker */
typedef struct {
    char pj[65], sig[65], floor[65];
} root_state;
static void root_state_of(root_state *s) {
    snprintf(s->pj, sizeof s->pj, "%s", sha_file(P("%s/payload.json", ROOT)));
    snprintf(s->sig, sizeof s->sig, "%s", sha_file(P("%s/payload.json.sig", ROOT)));
    snprintf(s->floor, sizeof s->floor, "%s", sha_file(P("%s/payload-version", ROOT)));
}
static int root_state_same(const root_state *s) {
    root_state now;
    root_state_of(&now);
    return !strcmp(s->pj, now.pj) && !strcmp(s->sig, now.sig) && !strcmp(s->floor, now.floor) && exists(P("%s/stage.deadbeef/y", ROOT))
           && exists(P("%s/bin/%s/RB Stems Plus.exe.1234.del", ROOT, sha12_of(WORKER_A)));
}

/* review finding 1: a scope or manifest mismatch (15) writes nothing in the root: not the newer
 * payload's pair, not its floor, no cleanup; the payload the app confirmed before still installs */
static void t_batch_refusal_writes_nothing(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged with payload 1.5.0");
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
    sh(P("mkdir -p '%s/stage.deadbeef' && echo x > '%s/stage.deadbeef/y'", ROOT, ROOT));
    spits(P("%s/bin/%s/RB Stems Plus.exe.1234.del", ROOT, sha12_of(WORKER_A)), "old image");
    root_state before;
    root_state_of(&before);
    source("1.5.1", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    /* a stale manifest */
    admin_args a;
    memset(&a, 0, sizeof a);
    a.verb = ADMIN_INSTALL;
    a.proto = ADMIN_PROTO;
    a.nt = 1;
    snprintf(a.t[0].version, sizeof a.t[0].version, "7.2.20");
    snprintf(a.t[0].exe16, 17, "%.16s", sha_file(P("%s/rekordbox.exe", V20)));
    snprintf(a.source, sizeof a.source, "%s", SRC);
    snprintf(a.manifest, sizeof a.manifest, "%s", sha_of("the payload.json the app checked before"));
    int x = run_args(&a);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && root_state_same(&before) && pioneers(V20, "7.2.20") && !fake_said(&F, "@floor"),
          "a stale manifest: 15, the pair, the floor and the leftovers as they were (%d)\n%s", x, F.said);
    a.verb = ADMIN_INSTALL_WORKER;
    a.all = 1;
    a.nt = 0;
    x = run_args(&a);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && root_state_same(&before), "install-worker with a stale manifest: 15, nothing written (%d)", x);
    /* a stale scope (7.2.19 is done), the newer payload's own manifest */
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && root_state_same(&before) && pioneers(V20, "7.2.20"), "a stale scope: 15, nothing written (%d)\n%s", x, F.said);
    /* the payload the app confirmed before still installs: never 14 */
    source("1.5.0", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V20, "7.2.20", BRIDGE1) && !exists(P("%s/stage.deadbeef", ROOT)), "then the confirmed payload installs (%d)\n%s", x, F.said);
}

/* review finding 2: Pioneer's repair replaces 7.2.20's DLL with an unbound build after the
 * assessment, while the preflight holds the folders: the targets are assessed again under their
 * holds and the scope compared before any write (15, 7.2.19 untouched too) */
static void swap_v20_unbound(fake *f, const char *path) {
    (void)f;
    if (strstr(path, "rekordbox 7.2.19/rekordbox.exe")) spits(P("%s/onnxruntime.dll", V20), "MZ onnxruntime-1.19 #cert 7.2.20 ALPHATHETA-SIGNED");
}
static void t_batch_reassessed_under_hold(void) {
    two();
    F.on_admin_only = swap_v20_unbound;
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    F.on_admin_only = NULL;
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && pioneers(V19, "7.2.19") && !has_receipt("7.2.19") && !exists(P("%s/pioneer", ROOT)),
          "a target changed after its assessment: 15, nothing written anywhere (%d)\n%s", x, F.said);
}

/* review finding 5: a bridged folder the worker can't check (its folder refuses a hold, or its
 * live DLL can't be read) counts as a user of ort\'s runtime: an ort\ copy that no longer passes
 * is never replaced then (4, ort\ and 7.2.20 as they were) */
static void t_batch_unchecked_runtime_user(void) {
    static const char OTHER[] = "MZ other code #cert 7.2.19 ALPHATHETA-SIGNED";
    for (int k = 0; k < 2; k++) {
        fresh();
        CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged");
        add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
        spits(P("%s/ort/onnxruntime.dll", ROOT), OTHER);
        if (k == 0) F.bad_acl[F.n_bad_acl++] = V19;
        else chmod(P("%s/onnxruntime.dll", V19), 0);
        int x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
        if (k == 1) chmod(P("%s/onnxruntime.dll", V19), 0644);
        CHECK(x == ADMIN_EXIT_CHECK_FAILED && holds(P("%s/ort/onnxruntime.dll", ROOT), OTHER) && pioneers(V20, "7.2.20") && holds(P("%s/onnxruntime.dll", V19), BRIDGE1),
              "%s: a possible runtime user, ort\\ never replaced (%d)\n%s", k ? "an unreadable live DLL" : "an unchecked folder", x, F.said);
    }
}

/* review finding 3: the saved set replaced by another consistent, AlphaTheta-signed one after it
 * was authorized: put-back and rollback restore only the authorized original's bytes, never the
 * replacement (the folder keeps the bridge, the outcome says so) */
static const char REBUILT[] = "MZ onnxruntime-1.18 #cert 7.2.20 rebuilt ALPHATHETA-SIGNED";
static void replace_v20_set_at_its_load(fake *f) {
    if (f->load_calls == 3) put_set("7.2.20", REBUILT);   /* 7.2.20's bridge: its load check fails */
}
static void replace_v19_set(fake *f) {
    (void)f;
    put_set("7.2.19", "MZ onnxruntime-1.18 #cert 7.2.19 rebuilt ALPHATHETA-SIGNED");
}
static void t_put_back_authorized_only(void) {
    two();
    F.load_fail_sub = "rekordbox 7.2.20";
    F.on_load = replace_v20_set_at_its_load;
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    F.load_fail_sub = NULL;
    F.on_load = NULL;
    CHECK(x == ADMIN_EXIT_PARTIAL && installed(V19, "7.2.19", BRIDGE1) && !holds(P("%s/onnxruntime.dll", V20), REBUILT) && !fake_said(&F, "@target 7.2.20 6")
              && fake_said(&F, "@target 7.2.20 11"),
          "the rollback never puts back a replaced set: 7.2.20 reported as not rolled back (%d)\n%s", x, F.said);
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged");
    F.on_mapped = replace_v19_set;
    x = run_targets(ADMIN_RESTORE, "7.2.19", 0);
    F.on_mapped = NULL;
    CHECK(x != ADMIN_EXIT_OK && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && has_receipt("7.2.19"),
          "Put Back never restores a set replaced after its authorization (%d)\n%s", x, F.said);
}

/* re-review finding 3: after a put-back that didn't happen, another AlphaTheta-signed file in
 * the folder (not the authorized original) is never "rolled back" or "put back" */
static void v19_gets_another_signed_file(fake *f) {
    (void)f;
    put_set("7.2.19", "MZ onnxruntime-1.18 #cert 7.2.19 rebuilt ALPHATHETA-SIGNED");   /* the put-back refuses it */
    spits(P("%s/onnxruntime.dll", V19), ort_of("7.2.20"));
}
static void t_success_needs_authorized_file(void) {
    fresh();
    F.load_fail = 1;
    F.on_load = v19_gets_another_signed_file;
    int x = install();
    F.load_fail = 0;
    F.on_load = NULL;
    CHECK(x == ADMIN_EXIT_ROLLBACK_FAILED && !fake_said(&F, "@target 7.2.19 6"), "the rollback: another signed file isn't Pioneer's original back (%d)\n%s", x, F.said);
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged");
    F.on_mapped = v19_gets_another_signed_file;
    x = run_targets(ADMIN_RESTORE, "7.2.19", 0);
    F.on_mapped = NULL;
    CHECK(x != ADMIN_EXIT_OK && !fake_said(&F, "@target 7.2.19 0"), "Put Back: another signed file isn't the original put back (%d)\n%s", x, F.said);
}

/* re-review finding 4: each Restore target's rekordbox.exe is the confirmed one at its own
 * replacement too (a later target's replaced after the checks, with the same ProductVersion:
 * 15 there), and a rekordbox.exe gone at the ACL check is refused: only the live DLL may be
 * missing there */
static int maps;
static void v20_exe_replaced(fake *f) {
    (void)f;
    if (++maps == 1) {
        size_t n;
        unsigned char *b = slurp(P("%s/rekordbox.exe", V20), &n);
        b = realloc(b, n + 8);
        memcpy(b + n, "rebuilt", 7);
        spit(P("%s/rekordbox.exe", V20), b, n + 7);
        free(b);
    }
}
static const char *exe_to_drop;
static void exe_goes(fake *f, const char *path) {
    (void)f;
    if (exe_to_drop && !strcmp(path, exe_to_drop)) unlink(path), exe_to_drop = NULL;
}
static void t_exe_rechecked_at_replacement(void) {
    two();
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_OK, "both installed");
    maps = 0;
    F.on_mapped = v20_exe_replaced;
    int x = run_targets(ADMIN_RESTORE, "7.2.19,7.2.20", 0);
    F.on_mapped = NULL;
    CHECK(x == ADMIN_EXIT_PARTIAL && pioneers(V19, "7.2.19") && holds(P("%s/onnxruntime.dll", V20), BRIDGE1) && fake_said(&F, "@target 7.2.20 15"),
          "a later target's rekordbox.exe replaced after the checks: 15 there, its bridge kept (%d)\n%s", x, F.said);
    /* the install's checks: 7.2.20's rekordbox.exe gone at its ACL check */
    two();
    exe_to_drop = P("%s/rekordbox.exe", V20);
    F.on_admin_only = exe_goes;
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    F.on_admin_only = NULL;
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && pioneers(V19, "7.2.19") && !has_receipt("7.2.19"), "install: a rekordbox.exe gone at its ACL check: 15, nothing changed (%d)\n%s", x,
          F.said);
    /* Uninstall's replacement: the same */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged");
    exe_to_drop = P("%s/rekordbox.exe", V19);
    F.on_admin_only = exe_goes;
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    F.on_admin_only = NULL;
    CHECK(x != ADMIN_EXIT_OK && holds(P("%s/onnxruntime.dll", V19), BRIDGE1), "uninstall: a rekordbox.exe gone at its ACL check: left as it is (%d)\n%s", x, F.said);
}

/* review finding 4: Restore checks every requested identity, read again under the held folders,
 * before it changes anything (15, nothing changed anywhere); Restore and Uninstall check the files'
 * ACLs before each replacement (10) */
static void t_restore_checks_all_first(void) {
    two();
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_OK, "both installed");
    admin_args a;
    memset(&a, 0, sizeof a);
    a.verb = ADMIN_RESTORE;
    a.proto = ADMIN_PROTO;
    a.nt = 2;
    snprintf(a.t[0].version, sizeof a.t[0].version, "7.2.19");
    snprintf(a.t[0].exe16, 17, "%.16s", sha_file(P("%s/rekordbox.exe", V19)));
    snprintf(a.t[1].version, sizeof a.t[1].version, "7.2.20");
    snprintf(a.t[1].exe16, 17, "%.16s", sha_file(P("%s/rekordbox.exe", V20)));
    size_t en;
    unsigned char *b = slurp(P("%s/rekordbox.exe", V20), &en);
    b = realloc(b, en + 8);
    memcpy(b + en, " rebuilt", 8);
    spit(P("%s/rekordbox.exe", V20), b, en + 8);
    free(b);
    int x = run_args(&a);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && has_receipt("7.2.19") && holds(P("%s/onnxruntime.dll", V20), BRIDGE1),
          "one target's rekordbox.exe changed: 15, neither folder changed (%d)\n%s", x, F.said);
    /* a live DLL other accounts can change: never replaced, by Restore or Uninstall */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged");
    F.bad_acl[F.n_bad_acl++] = P("%s/onnxruntime.dll", V19);
    x = run_targets(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_NOT_PIONEERS && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && has_receipt("7.2.19"), "Restore: the file ACL refused, 10 (%d)\n%s", x, F.said);
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_NOT_PIONEERS && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && exists(ROOT), "Uninstall: the same, the root kept (%d)\n%s", x, F.said);
    F.n_bad_acl = 0;
    F.bad_acl[F.n_bad_acl++] = P("%s/rekordbox.exe", V19);
    x = run_targets(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_NOT_PIONEERS && holds(P("%s/onnxruntime.dll", V19), BRIDGE1), "Restore: rekordbox.exe's ACL refused, 10 (%d)", x);
}

/* review finding 8: rekordbox started mid-run (another session, another version): the
 * system-wide close gate is asked again before the shared writes and before each target; the
 * targets done stay done, the rest is reported */
static void t_batch_gate_each_target(void) {
    /* the gate's calls: the start (1), before the shared writes (2), before each target (3, 4) */
    two();
    fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V19));
    F.windows = 1;
    F.procs_from_call = 4;
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_PARTIAL && installed(V19, "7.2.19", BRIDGE1) && pioneers(V20, "7.2.20") && fake_said(&F, "@target 7.2.20 5"),
          "rekordbox opened after the first target: 16, 7.2.20 untouched (%d)\n%s", x, F.said);
    two();
    fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V19));
    F.windows = 1;
    F.procs_from_call = 2;
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && pioneers(V19, "7.2.19") && pioneers(V20, "7.2.20") && !exists(P("%s/pioneer", ROOT)) && !exists(P("%s/payload-version", ROOT)),
          "rekordbox opened before the shared writes: 5, nothing written (%d)\n%s", x, F.said);
}

/* the scratch tree as it is (every name, every file's sha256), but the app's source and folder */
static char *tree_of(void) {
    char cmd[2600];
    snprintf(cmd, sizeof cmd,
             "cd '%s' && { find . \\( -path ./src -o -path ./app \\) -prune -o -print | LC_ALL=C sort; "
             "find . \\( -path ./src -o -path ./app \\) -prune -o -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256; }",
             S);
    FILE *p = popen(cmd, "r");
    if (!p) abort();
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    for (size_t k; (k = fread(b + n, 1, cap - n - 1, p)) > 0;)
        if ((n += k) + 1 == cap) b = realloc(b, cap *= 2);
    b[n] = 0;
    pclose(p);
    return b;
}
static int tree_same(const char *before) {
    char *now = tree_of();
    int same = !strcmp(before, now);
    if (!same) fprintf(stderr, "--- before\n%s--- after\n%s", before, now);
    free(now);
    return same;
}

/* the owner's PC (build 88470aeb): a refused Install left the root behind (payload, floor, lock,
 * empty ort\ and pioneer\, the worker in bin\), which the app then reads as Stems Cache traces.
 * Any refusal before the first target's change leaves the machine exactly as it was: no root
 * made, nothing in an existing one changed */
static void t_refused_install_changes_nothing(void) {
    for (int k = 0; k < 8; k++) {
        fresh();
        add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
        const char *manifest = NULL;
        switch (k) {
        case 0: spits(P("%s/onnxruntime.dll", V20), "MZ onnxruntime-1.19 #cert 7.2.20 ALPHATHETA-SIGNED"); break;   /* an ORT this release doesn't bind */
        case 1: manifest = sha_of("another payload.json"); break;
        case 2: fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V19)), F.windows = 1; break;
        case 3: fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V19)), F.windows = 1, F.procs_from_call = 2; break;
        case 4: F.bad_acl[F.n_bad_acl++] = P("%s/onnxruntime.dll", V20); break;
        case 5: spits(P("%s/admin/RB Stems Plus.exe", SRC), WORKER_B); break;   /* not the worker payload.json names */
        case 6: source_full("1.5.0", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll", "7.2.19", 1); break;   /* 7.2.20 unlisted */
        case 7: source_full("1.5.0", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll", "7.2.19,7.2.20", 0); break;   /* no bindings */
        }
        char *before = tree_of();
        admin_args a;
        memset(&a, 0, sizeof a);
        a.verb = ADMIN_INSTALL;
        a.proto = ADMIN_PROTO;
        a.nt = 2;
        snprintf(a.t[0].version, sizeof a.t[0].version, "7.2.19");
        snprintf(a.t[0].exe16, 17, "%.16s", sha_file(P("%s/rekordbox.exe", V19)));
        snprintf(a.t[1].version, sizeof a.t[1].version, "7.2.20");
        snprintf(a.t[1].exe16, 17, "%.16s", sha_file(P("%s/rekordbox.exe", V20)));
        snprintf(a.source, sizeof a.source, "%s", SRC);
        snprintf(a.manifest, sizeof a.manifest, "%s", manifest ? manifest : sha_file(P("%s/payload.json", SRC)));
        int x = run_args(&a);
        CHECK(x != ADMIN_EXIT_OK && !exists(ROOT) && tree_same(before), "case %d: a refused first install (%d) leaves no root, nothing changed\n%s", k, x, F.said);
        free(before);
    }
    /* an existing root: nothing in it changes */
    for (int k = 0; k < 3; k++) {
        fresh();
        CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged");
        add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
        if (k == 0) source("1.5.1", BRIDGE1, WORKER_B, 1, "rbstemsplus-bridge-win-x64.dll");
        if (k == 1) spits(P("%s/ort/onnxruntime.dll", ROOT), "MZ other code #cert 7.2.19 ALPHATHETA-SIGNED"), F.bad_acl[F.n_bad_acl++] = V19;
        if (k == 2) fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V19)), F.windows = 1, F.procs_from_call = 2;
        char *before = tree_of();
        int x = k == 0 ? run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) : run_targets(ADMIN_INSTALL, "7.2.20", 0);
        CHECK(x != ADMIN_EXIT_OK && tree_same(before), "case %d: a refused install over an existing root (%d) changes nothing in it\n%s", k, x, F.said);
        free(before);
    }
}

/* re-review blocker A: two runs both find no root; the other one makes it and finishes a whole
 * install before this one opens it. This run didn't make that root, so its refusal removes
 * nothing of it: the other run's installation stays whole */
static char other_root[1200];
static void other_run_finishes(fake *f) {
    (void)f;
    sh(P("mv '%s' '%s'", other_root, ROOT));
}
static void t_refusal_keeps_another_runs_root(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "the other run's install");
    char *before = tree_of();
    snprintf(other_root, sizeof other_root, "%s.other", ROOT);
    sh(P("mv '%s' '%s'", ROOT, other_root));
    F.on_root_create = other_run_finishes;
    int x = install();   /* 7.2.19 is done by then: 15 */
    F.on_root_create = NULL;
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && installed(V19, "7.2.19", BRIDGE1) && tree_same(before),
          "a refused run that found the root made meanwhile leaves the other run's installation whole (%d)\n%s", x, F.said);
    free(before);
}

/* re-review blocker B: over an existing root, a refusal after the shared writes and before the
 * first target changes leaves the root as it was (the floor, the payload pair), and a stopped
 * run's temp files in a target folder stay through the checks */
static void t_late_refusal_keeps_root(void) {
    for (int k = 0; k < 4; k++) {
        fresh();
        CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged at 1.5.0");
        add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
        source("1.5.1", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");   /* a newer floor and payload pair */
        if (k == 1) sh(P("chflags uchg '%s/payload.json'", ROOT));   /* the pair's save fails after the floor's */
        if (k >= 2) spits(P("%s/onnxruntime.dll.rbsp-new", V20), "a stopped run's"), spits(P("%s/onnxruntime.dll.rbsp-probe", V20), "");
        /* rekordbox starts: at the first target's gate (0, 3), before the shared writes (2) */
        if (k != 1) fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V19)), F.windows = 1, F.procs_from_call = k == 2 ? 2 : 3;
        char *before = tree_of();
        int x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
        if (k == 1) sh(P("chflags nouchg '%s/payload.json'", ROOT));
        CHECK(x == (k == 1 ? ADMIN_EXIT_CHECK_FAILED : ADMIN_EXIT_REKORDBOX_OPEN) && tree_same(before) && holds(P("%s/payload-version", ROOT), "1.5.0\n"),
              "case %d: a refusal before the first target changes leaves the existing root as it was (%d)\n%s", k, x, F.said);
        free(before);
    }
}

/* re-review finding 8: rekordbox starting while the worker is staged and ort\'s copy decided is
 * refused by the close gate just before the shared writes, before the floor or the payload is
 * kept */
static int ort_pins;
static void rekordbox_starts_at_ort_decision(fake *f, const char *path) {
    if (strstr(path, "/ort/onnxruntime.dll") && ++ort_pins == 2) fake_proc(f, "rekordbox.exe", P("%s/rekordbox.exe", V19)), f->windows = 1;
}
static void t_gate_before_shared_writes(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged at 1.5.0");
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
    source("1.5.1", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    ort_pins = 0;
    F.on_pin = rekordbox_starts_at_ort_decision;
    int x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    F.on_pin = NULL;
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && ort_pins >= 2 && !fake_said(&F, "@floor") && !fake_said(&F, "@target 7.2.20 5"),
          "rekordbox started during the dry run: 5 at the gate before the shared writes (%d)\n%s", x, F.said);
}

/* re-review 2, blocker 1: a first target's repair (its live DLL missing, the bound set put back
 * first) that fails before the live DLL changes is still a refusal before the first change: the
 * new floor and payload pair are undone, the root as it was */
static char repair_block[1200];
static int repair_maps;
static void saved_set_unusable_at_repair(fake *f) {
    if (++repair_maps == 2) f->bad_acl[f->n_bad_acl++] = repair_block;   /* after the checks, at the repair */
}
static void t_failed_repair_undone(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged at 1.5.0");
    unlink(P("%s/onnxruntime.dll", V19));
    source("1.5.1", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");   /* a newer floor and payload pair */
    snprintf(repair_block, sizeof repair_block, "%s/pioneer/7.2.19", ROOT);
    char *before = tree_of();
    repair_maps = 0;
    F.on_mapped = saved_set_unusable_at_repair;
    int x = install();
    F.on_mapped = NULL;
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && fake_said(&F, "is missing") && tree_same(before) && holds(P("%s/payload-version", ROOT), "1.5.0\n"),
          "a failed first-target repair: the shared writes undone, the root as it was (%d)\n%s", x, F.said);
    free(before);
}

/* re-review 2, blocker 2: a Reinstall whose first swap fails after its receipt went, the receipt's
 * write-back failing too: the journal brings the receipt back (the old bridge stays live with it) */
static char rc_dir[1200], rc_file[1200];
static int rc_maps, rc_locked;
static void reinstall_swap_blocked(fake *f) {
    (void)f;
    ++rc_maps;
    if (rc_maps == 2) sh(P("chflags uchg '%s'", V19));                          /* just before the swap: it fails */
    if (rc_maps == 3) sh(P("chflags nouchg '%s' '%s'", V19, rc_dir));            /* after the write-back failed */
}
static void receipt_write_back_fails(fake *f) {
    (void)f;
    if (!rc_locked && rc_maps == 2 && !exists(rc_file)) sh(P("chflags uchg '%s'", rc_dir)), rc_locked = 1;
}
static void t_refused_reinstall_keeps_receipt(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged");
    snprintf(rc_dir, sizeof rc_dir, "%s/receipts", ROOT);
    snprintf(rc_file, sizeof rc_file, "%s/receipts/7.2.19", ROOT);
    char was[65];
    snprintf(was, sizeof was, "%s", sha_file(rc_file));
    rc_maps = rc_locked = 0;
    F.on_mapped = reinstall_swap_blocked;
    F.on_random = receipt_write_back_fails;
    int x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
    F.on_mapped = NULL;
    F.on_random = NULL;
    sh(P("chflags nouchg '%s' '%s'", V19, rc_dir));
    unlink(P("%s/onnxruntime.dll.rbsp-new", V19));   /* the accepted residual of a refused first target */
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && rc_locked && !strcmp(sha_file(rc_file), was) && installed(V19, "7.2.19", BRIDGE1),
          "a refused Reinstall whose receipt couldn't be written back: the journal restores it (%d)\n%s", x, F.said);
}

/* Simulate the receipt folder disappearing before undo reopens it. An old receipt must go back
 * to its original path, while an absent newly made entry has already been removed. */
static void receipt_folder_disappears(fake *f) {
    reinstall_swap_blocked(f);
    if (rc_maps == 3) sh(P("mv '%s/receipts' '%s/receipts.moved'", ROOT, ROOT));
}

static void t_undo_missing_receipt_folder(void) {
    for (int replaced = 0; replaced < 2; replaced++) {
        fresh();
        if (replaced) CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged");
        else sh(P("mkdir -p '%s/receipts'", ROOT));
        snprintf(rc_dir, sizeof rc_dir, "%s/receipts", ROOT);
        snprintf(rc_file, sizeof rc_file, "%s/receipts/7.2.19", ROOT);
        char was[65];
        snprintf(was, sizeof was, "%s", sha_file(rc_file));
        rc_maps = 0;
        F.on_mapped = receipt_folder_disappears;
        int x = run_verb(ADMIN_INSTALL, "7.2.19", replaced);
        F.on_mapped = NULL;
        CHECK(rc_maps == 3 && !exists(rc_dir), "the receipt folder disappeared before undo (%d)\n%s", x, F.said);
        if (replaced) {
            const char *line = P("rollback incomplete: the saved copies stay in %s\\", ROOT);
            const char *name = strstr(F.said, line);
            int backed_up = 0;
            if (name) {
                name += strlen(line);
                int len = (int)strcspn(name, "\n");
                for (int i = 0; i < 32 && !backed_up; i++)
                    backed_up = !strcmp(sha_file(P("%s/%.*s/undo.%d", ROOT, len, name, i)), was);
            }
            CHECK(x == ADMIN_EXIT_UNDO_INCOMPLETE && was[0] && backed_up && holds(P("%s/onnxruntime.dll", V19), BRIDGE1),
                  "a missing restore destination leaves the saved receipt and returns 17 (%d)\n%s", x, F.said);
        } else
            CHECK(x == ADMIN_EXIT_CHECK_FAILED && no_stage() && pioneers(V19, "7.2.19"),
                  "a missing removal destination keeps the original refusal (%d)\n%s", x, F.said);
    }
}

/* re-review 2, before release: an undo that can't restore an entry keeps the staging folder with
 * the saved copies and ends with 17 and a line naming that folder; the entries it could restore
 * are back */
static char floor_file[1200];
static void floor_pinned_at_gate(fake *f, int call) {
    (void)f;
    if (call == 3) sh(P("chflags uchg '%s'", floor_file));   /* the first target's gate: rekordbox opened */
}
static void leave_failed_undo(char *kept, size_t cap) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged at 1.5.0");
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
    source("1.5.1", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    snprintf(floor_file, sizeof floor_file, "%s/payload-version", ROOT);
    char json_was[65];
    snprintf(json_was, sizeof json_was, "%s", sha_file(P("%s/payload.json", ROOT)));
    fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V19));
    F.windows = 1;
    F.procs_from_call = 3;
    F.on_procs = floor_pinned_at_gate;
    int x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    F.on_procs = NULL;
    F.n_procs = F.windows = F.procs_from_call = 0;
    sh(P("chflags nouchg '%s'", floor_file));
    const char *line = P("rollback incomplete: the saved copies stay in %s\\", ROOT);
    const char *name = strstr(F.said, line);
    kept[0] = 0;
    if (name) {
        name += strlen(line);
        snprintf(kept, cap, "%s/%.*s/undo.0", ROOT, (int)strcspn(name, "\n"), name);
    }
    CHECK(x == ADMIN_EXIT_UNDO_INCOMPLETE && holds(kept, "1.5.0\n") && !strcmp(sha_file(P("%s/payload.json", ROOT)), json_was),
          "an entry the undo couldn't restore: 17, never \"nothing changed\"; its saved copy stays in the staging folder it names, the others are back (%d)\n%s",
          x, F.said);
}

static void t_failed_undo_keeps_copies(void) {
    char kept[1200];
    leave_failed_undo(kept, sizeof kept);
}

/* A later run can clean its ordinary leftovers, but the saved copy survives until a verified
 * change or full uninstall. Each attempt starts with a real exit-17 journal. */
static void t_retained_undo_cleanup(void) {
    char kept[1200];
    leave_failed_undo(kept, sizeof kept);
    F.mapped = 1;
    int x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && holds(kept, "1.5.0\n"), "a refused Put Back keeps the undo copy (%d)\n%s", x, F.said);
    F.mapped = 0;

    leave_failed_undo(kept, sizeof kept);
    x = run_verb(ADMIN_RESTORE, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "@target 7.2.20 0 already") && holds(kept, "1.5.0\n"),
          "Put Back over a stock DLL keeps the undo copy (%d)\n%s", x, F.said);

    leave_failed_undo(kept, sizeof kept);
    source("1.5.1", BRIDGE1, WORKER_B, 1, "rbstemsplus-bridge-win-x64.dll");
    x = run_verb(ADMIN_INSTALL_WORKER, "", 0);
    CHECK(x == ADMIN_EXIT_RUN_AGAIN && holds(kept, "1.5.0\n"), "a worker upgrade keeps the undo copy (%d)\n%s", x, F.said);
    x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && !exists(kept), "verified Put Back removes the old undo journal (%d)\n%s", x, F.said);

    leave_failed_undo(kept, sizeof kept);
    F.load_fail = 1;
    x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_ROLLED_BACK && pioneers(V20, "7.2.20") && holds(kept, "1.5.0\n"),
          "an Install whose swap fails verification keeps the previous undo copy (%d)\n%s", x, F.said);
    F.load_fail = 0;
    x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V20, "7.2.20", BRIDGE1) && !exists(kept), "verified Install removes the old undo journal (%d)\n%s", x, F.said);

    leave_failed_undo(kept, sizeof kept);
    F.mapped = 1;
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && holds(kept, "1.5.0\n"), "a refused Uninstall keeps the undo copy (%d)\n%s", x, F.said);
    F.mapped = 0;
    spits(P("%s/onnxruntime.dll", V19), ort_of("7.2.19"));
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && !exists(ROOT), "a full Uninstall with stock DLLs removes the old undo journal (%d)\n%s", x, F.said);
}

/* re-review 2, before release (finding 4): Restore holds the verified rekordbox.exe against writes
 * and replacement through the put-back and its retry: an update replacing it while the saved
 * file is copied in is refused, and the hold is released afterwards */
static int exe_update_tried, exe_update_refused;
static void exe_updated_during_put_back(fake *f, const char *path) {
    if (exe_update_tried || !strstr(path, "/rekordbox 7.2.19/onnxruntime.dll.rbsp-new")) return;
    exe_update_tried = 1;
    const char *exe_path = P("%s/rekordbox.exe", V19);
    if (fake_file_held(f, exe_path)) {
        exe_update_refused = 1;
        return;
    }
    size_t n;
    unsigned char *b = slurp(exe_path, &n);
    b = realloc(b, n + 8);
    memcpy(b + n, "updated", 7);
    spit(exe_path, b, n + 7);
    free(b);
}
static void t_restore_holds_exe(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged");
    char was[65];
    snprintf(was, sizeof was, "%s", sha_file(P("%s/rekordbox.exe", V19)));
    exe_update_tried = exe_update_refused = 0;
    F.on_pin = exe_updated_during_put_back;
    int x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    F.on_pin = NULL;
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && exe_update_tried && exe_update_refused && !strcmp(sha_file(P("%s/rekordbox.exe", V19)), was)
              && !fake_file_held(&F, P("%s/rekordbox.exe", V19)),
          "Restore: the checked rekordbox.exe held through the put-back, released after (%d)\n%s", x, F.said);
}

/* how many files match the shell pattern pat (quoted parts in it as the shell takes them) */
static int count_of(const char *pat) {
    FILE *p = popen(P("ls -d %s 2>/dev/null | wc -l", pat), "r");
    int n = 0;
    if (!p || fscanf(p, "%d", &n) != 1) n = -1;
    if (p) pclose(p);
    return n;
}

/* re-review 2, before release (finding 8): the payload pair's saved copies are made before the
 * last close gate, so no disk I/O is left between that gate and the floor's write */
static int gate_copies;
static void count_copies_before_floor(fake *f, int call) {
    (void)call;
    if (!fake_said(f, "@floor")) gate_copies = count_of(P("'%s'/undo-stage.*/undo.* '%s'/stage.*/undo.*", ROOT, ROOT));
}
static void t_payload_copies_before_gate(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "7.2.19 bridged at 1.5.0");
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
    source("1.5.1", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    gate_copies = -1;
    F.on_procs = count_copies_before_floor;
    int x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    F.on_procs = NULL;
    CHECK(x == ADMIN_EXIT_OK && gate_copies == 3 && installed(V20, "7.2.20", BRIDGE1),
          "the last close gate before the floor comes after the payload pair's three saved copies (%d, %d)\n%s", x, gate_copies, F.said);
}

static void t_batch_incomplete_inventory(void) {
    two();
    F.entries_partial = 1;
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && pioneers(V19, "7.2.19") && !exists(P("%s/receipts", ROOT)), "a partial registry: 4, nothing changed (%d)", x);
    two();
    sh(P("mkdir -p '%s/rekordbox 7.2.22'", RB));
    fake_entry(&F, P("%s/rekordbox 7.2.22", RB), "7.2.22.0400");   /* an installer mid-copy: no rekordbox.exe yet */
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && pioneers(V20, "7.2.20"), "a version settling: 4, nothing changed (%d)", x);
}

static void t_batch_fail_second(void) {
    two();
    F.load_fail_sub = "rekordbox 7.2.20";
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    F.load_fail_sub = NULL;
    CHECK(x == ADMIN_EXIT_PARTIAL, "the second target fails: 16 (%d)\n%s", x, F.said);
    CHECK(installed(V19, "7.2.19", BRIDGE1) && fake_said(&F, "@target 7.2.19 0 installed"), "7.2.19 keeps its bridge and receipt");
    CHECK(pioneers(V20, "7.2.20") && !has_receipt("7.2.20") && fake_said(&F, "@target 7.2.20 6 put-back"), "7.2.20 rolled back, no receipt");
    /* the retry: only 7.2.20 is left */
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_SCOPE_CHANGED, "the old scope: 15");
    x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1) && installed(V20, "7.2.20", BRIDGE1), "the retry installs 7.2.20 only (%d)", x);

    /* the same, then uninstall: 7.2.19 uninstallable, both stock, the root gone */
    two();
    F.load_fail_sub = "rekordbox 7.2.20";
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_PARTIAL, "partial");
    F.load_fail_sub = NULL;
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && pioneers(V20, "7.2.20") && !exists(ROOT), "uninstall after a partial install (%d)\n%s", x, F.said);

    /* three folders, the second fails: the third not started */
    three();
    F.load_fail_sub = "rekordbox 7.2.20";
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20,7.2.21", 0);
    F.load_fail_sub = NULL;
    CHECK(x == ADMIN_EXIT_PARTIAL && installed(V19, "7.2.19", BRIDGE1) && pioneers(V20, "7.2.20") && pioneers(V21, "7.2.21")
              && fake_said(&F, "@target 7.2.21 - not-started") && !exists(P("%s/pioneer/7.2.21", ROOT)),
          "the third not started, untouched (%d)", x);
    /* the first fails: its own code, the rest not started */
    two();
    F.load_fail_sub = "rekordbox 7.2.19";
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    F.load_fail_sub = NULL;
    CHECK(x == ADMIN_EXIT_ROLLED_BACK && pioneers(V19, "7.2.19") && pioneers(V20, "7.2.20") && fake_said(&F, "@target 7.2.20 - not-started"), "the first fails: 6 (%d)", x);
}

static void t_batch_preflight_refuses_all(void) {
    two();
    F.bad_acl[F.n_bad_acl++] = P("%s/onnxruntime.dll", V20);
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_NOT_PIONEERS && pioneers(V19, "7.2.19") && pioneers(V20, "7.2.20") && !exists(P("%s/pioneer", ROOT)) && !exists(P("%s/receipts", ROOT)),
          "one target's file writable by others: 10, nothing changed anywhere (%d)", x);
    two();
    F.bad_acl[F.n_bad_acl++] = V20;
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_NOT_PIONEERS && pioneers(V19, "7.2.19") && !exists(P("%s/receipts", ROOT)), "one target's folder writable by others: 10 (%d)", x);
}

static void t_batch_unbound_original(void) {
    two();
    spits(P("%s/onnxruntime.dll", V20), "MZ onnxruntime-1.19 #cert 7.2.20 ALPHATHETA-SIGNED");   /* other code */
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && pioneers(V19, "7.2.19"), "an original with other code named: 15 (%d)", x);
    x = run_targets(ADMIN_INSTALL, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1) && !has_receipt("7.2.20"), "the bound one alone: installed (%d)", x);
    /* 7.2.20's own re-signed original under 7.2.19's binding: not its own version's hash */
    two();
    spits(P("%s/onnxruntime.dll", V20), ort_of("7.2.19"));
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_SCOPE_CHANGED, "another version's original: not bound, 15");
}

static void t_batch_unlisted_version(void) {
    /* 7.2.20 isn't listed, its ORT is (ort_sha256): never in the scope */
    two();
    source_full("1.5.0", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll", "7.2.19", 1);
    int x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && pioneers(V20, "7.2.20"), "an unlisted version with an allowed ORT: 15 (%d)", x);
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_SCOPE_CHANGED && pioneers(V19, "7.2.19"), "with a listed one: 15, nothing changed");
    x = run_targets(ADMIN_INSTALL, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1) && pioneers(V20, "7.2.20"), "the listed one alone (%d)", x);
    /* a bridged version that became unlisted: Put Back still works */
    two();
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_OK, "both installed");
    source_full("1.5.0", BRIDGE1, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll", "7.2.19", 1);
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19", 1) == ADMIN_EXIT_OK, "a reinstall of the listed one (the floor's payload: 7.2.19 only)");
    x = run_targets(ADMIN_RESTORE, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V20, "7.2.20") && installed(V19, "7.2.19", BRIDGE1), "an unlisted version still permits recovery (%d)\n%s", x, F.said);
}

static void t_batch_keeps_old_bridge(void) {
    /* D1: no "one bridged version at a time": installing into 7.2.20 leaves 7.2.19's bridge */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64});
    int x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V20, "7.2.20", BRIDGE1) && installed(V19, "7.2.19", BRIDGE1) && exists(P("%s/pioneer/7.2.19", ROOT)),
          "7.2.20 installed; 7.2.19's bridge, receipt and saved set stay (%d)\n%s", x, F.said);
}

static void t_restore_targets(void) {
    /* a bridged 7.2.19 whose ort\ copy is missing, a healthy 7.2.20: Put Back {7.2.19} fixes 7.2.19
     * only; 7.2.20 and the root stay */
    two();
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_OK, "both installed");
    sh(P("rm -f '%s/ort/onnxruntime.dll'", ROOT));
    int x = run_targets(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && holds(P("%s/onnxruntime.dll", V20), BRIDGE1) && has_receipt("7.2.20") && exists(ROOT),
          "Put Back one version: the other's bridge and the root stay (%d)\n%s", x, F.said);
    /* both, one of them failing: the other one done, 16 */
    two();
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_OK, "both installed");
    sh(P("rm -rf '%s/pioneer/7.2.19'", ROOT));
    x = run_targets(ADMIN_RESTORE, "7.2.19,7.2.20", 0);
    CHECK(x == ADMIN_EXIT_PARTIAL && pioneers(V20, "7.2.20") && holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && fake_said(&F, "@target 7.2.19 3 kept"),
          "one fails, the other goes on: 16 (%d)\n%s", x, F.said);
}

/* a same-version reinstall by Pioneer: a new folder (identity) and a new rekordbox.exe */
static void reinstall_pioneer(const char *v, int live) {
    const char *d = vdir_of(v);
    sh(P("rm -rf '%s'", d));
    int n = F.n_entries;
    add_install((fake_install){.version = v, .views = FAKE_REG_64});
    F.n_entries = n;   /* its uninstall entry was there already */
    size_t en;
    unsigned char *b = slurp(P("%s/rekordbox.exe", d), &en);
    b = realloc(b, en + 8);
    memcpy(b + en, " rebuilt", 8);   /* another build of the same version: other bytes */
    spit(P("%s/rekordbox.exe", d), b, en + 8);
    free(b);
    if (!live) unlink(P("%s/onnxruntime.dll", d));
}

static void t_restore_identity(void) {
    /* the same version reinstalled, the live DLL absent, an old backup that isn't the bound
     * original: never restored on X.Y.Z alone (3) */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    reinstall_pioneer("7.2.19", 0);
    put_set("7.2.19", "MZ onnxruntime-1.17 #cert 7.2.19 old build ALPHATHETA-SIGNED");
    int x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_NO_ORIGINALS && !exists(P("%s/onnxruntime.dll", V19)) && fake_said(&F, "reinstall rekordbox 7.2.19 using Pioneer's installer"),
          "a reinstalled folder and a backup the binding doesn't vouch for: 3 (%d)\n%s", x, F.said);
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_NO_ORIGINALS && !exists(P("%s/onnxruntime.dll", V19)) && exists(ROOT), "uninstall the same: 3, the root kept (%d)", x);
    /* with the bound original: adopted (the receipt no longer binds the folder; the signed binding does) */
    put_set("7.2.19", ort_of("7.2.19"));
    x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19"), "the bound original: put back (%d)\n%s", x, F.said);
    /* a reinstalled folder with Pioneer's file: installed again, the receipt binding the new folder */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    reinstall_pioneer("7.2.19", 1);
    x = install();
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1), "a reinstalled folder: installed again, the new identity in its receipt (%d)\n%s", x, F.said);
}

static void t_uninstall_unverified_newest(void) {
    /* gap 2: a newer folder that isn't verified no longer blocks recovery */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    add_install((fake_install){.version = "7.2.21", .views = FAKE_REG_64, .trust = FAKE_RB_UNSIGNED});
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    int x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && !exists(ROOT), "an unsigned newer version (complete): 7.2.19 restored, the root removed (%d)\n%s", x, F.said);
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    add_install((fake_install){.version = "7.2.21", .views = FAKE_REG_64, .trust = FAKE_RB_UNVERIFIABLE});
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && exists(ROOT) && fake_said(&F, "@root-kept inventory-incomplete"),
          "an unverifiable newer version (incomplete): 7.2.19 restored, the root kept (D3) (%d)\n%s", x, F.said);
    /* incomplete: restore-only work on the folders our receipts bind; a receipt-less set (the
     * binding would vouch for it) waits for a whole inventory */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed into 7.2.19");
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_64, .live = FAKE_RB_INTERRUPTED});
    F.entries_partial = 1;
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_PARTIAL && pioneers(V19, "7.2.19") && !exists(P("%s/onnxruntime.dll", V20)) && exists(ROOT) && fake_said(&F, "@target 7.2.20 3 kept"),
          "incomplete: the receipt-bound folder restored, the receipt-less one kept (%d)\n%s", x, F.said);
    F.entries_partial = 0;
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V20, "7.2.20") && !exists(ROOT), "whole again: the signed binding vouches for it, the root goes (%d)\n%s", x, F.said);
}

static void t_uninstall_partial(void) {
    two();
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_OK, "both installed");
    sh(P("rm -rf '%s/pioneer/7.2.20'", ROOT));
    int x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_PARTIAL && pioneers(V19, "7.2.19") && holds(P("%s/onnxruntime.dll", V20), BRIDGE1), "one version can't be put back: the other is, 16 (%d)\n%s",
          x, F.said);
    CHECK(exists(P("%s/ort/onnxruntime.dll", ROOT)) && exists(ROOT) && fake_said(&F, "@root-kept bridge-remains") && fake_said(&F, "@target 7.2.20 3 kept")
              && !has_receipt("7.2.19"),
          "ort\\ and the root kept for the bridge that stays");
}

static void characterize_today_selects_newest_registered(void);

static void t_install_worker_protocol(void) {
    /* D5: never an older protocol's worker into bin\ */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    source("1.6.0", BRIDGE1, WORKER_OLD, 1, "rbstemsplus-bridge-win-x64.dll");
    int x = run_verb(ADMIN_INSTALL_WORKER, "", 0);
    CHECK(x == ADMIN_EXIT_PROTOCOL && !exists(P("%s/bin/%s", ROOT, sha12_of(WORKER_OLD))), "a protocol-1 worker: 12, not placed (%d)\n%s", x, F.said);
}

/* -- today's behaviour kept */

static int selected(const char *version, int expected_count, int expected_state) {
    compat_rekordbox found[FAKE_MAX];
    int current, state;
    compat_memo_clear();
    int n = compat_find_rekordbox(&F.compat, RB, found, FAKE_MAX, &current, &state);
    return n == expected_count && state == expected_state &&
           (version ? current >= 0 && !strcmp(found[current].folder_version, version) : current == -1);
}

static void characterize_today_selects_newest_registered(void) {
    fresh();
    CHECK(selected("7.2.19", 1, COMPAT_RB_OK), "today: the only verified registrant is current");
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_32 | FAKE_REG_64});
    fake_proc(&F, "rekordbox.exe", P("%s/rekordbox.exe", V19));
    CHECK(selected("7.2.20", 2, COMPAT_RB_OK), "today: newest wins even with older EXE running; both views collapse");
    add_install((fake_install){.version = "7.2.21"});
    CHECK(selected("7.2.20", 2, COMPAT_RB_OK), "today: complete registry ignores a newer disk-only folder");
    F.entries_partial = 1;
    CHECK(selected(NULL, 2, COMPAT_RB_NO_ANSWER), "today: newer disk-only folder vetoes an incomplete registry result");
    F.entries_partial = 0;

    fresh();
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_32, .registry_only = 1});
    CHECK(selected("7.2.19", 2, COMPAT_RB_OK), "today: stale registry-only entry is skipped");
    add_install((fake_install){.version = "7.2.21", .views = FAKE_REG_64, .trust = FAKE_RB_UNSIGNED});
    CHECK(selected(NULL, 3, COMPAT_RB_UNVERIFIED), "today: unsigned newest EXE blocks fallback");
    add_install((fake_install){.version = "7.2.21", .trust = FAKE_RB_UNVERIFIABLE});
    CHECK(selected(NULL, 3, COMPAT_RB_NO_ANSWER), "today: unverifiable newest EXE also blocks fallback");
}

static void characterize_today_uninstall_sweeps_disk(void) {
    fresh();
    F.n_entries = 0;
    add_install((fake_install){.version = "7.2.19", .live = FAKE_RB_BRIDGED});
    add_install((fake_install){.version = "7.2.20", .views = FAKE_REG_32, .live = FAKE_RB_INTERRUPTED});
    add_install((fake_install){.version = "7.2.21", .views = FAKE_REG_64});
    /* 7.2.20's set has no receipt: only the verified payload the root keeps vouches for it */
    int x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_PARTIAL && pioneers(V19, "7.2.19") && !exists(P("%s/onnxruntime.dll", V20)) && exists(ROOT) && fake_said(&F, "@target 7.2.20 3 kept"),
          "no saved payload: the receipt-less set isn't put back, the receipt-bound one is (%d)\n%s", x, F.said);
    sh(P("cp '%s/payload.json' '%s/payload.json.sig' '%s/'", SRC, SRC, ROOT));
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && pioneers(V20, "7.2.20") && pioneers(V21, "7.2.21") && !exists(ROOT),
          "uninstall restores the disk-only bridge and an interrupted older version behind a stock newest (%d)\n%s", x, F.said);
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
    x = run_verb(ADMIN_INSTALL, "7.2.19", 1);
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
    /* only behind a whole sweep */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    sh(P("rm -rf '%s'", V19));
    F.n_entries = 0;
    F.entries_partial = 1;
    CHECK(run_verb(ADMIN_REMOVE_ROOT_FOLDER, "", 0) == ADMIN_EXIT_CHECK_FAILED && exists(ROOT), "a partial registry: the root stays");
}

static void t_leftovers(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    sh(P("mkdir -p '%s/stage.deadbeef/x' && echo x > '%s/stage.deadbeef/x/y'", ROOT, ROOT));
    spits(P("%s/onnxruntime.dll.rbsp-new", V19), "half");
    spits(P("%s/bin/%s/RB Stems Plus.exe.1234.del", ROOT, sha12_of(WORKER_A)), "old image");
    CHECK(run_verb(ADMIN_INSTALL, "7.2.19", 1) == ADMIN_EXIT_OK, "a run after a stopped one");
    CHECK(no_stage() && !exists(P("%s/onnxruntime.dll.rbsp-new", V19)) && !exists(P("%s/bin/%s/RB Stems Plus.exe.1234.del", ROOT, sha12_of(WORKER_A))),
          "its staging folder, temp file and *.del removed");
    /* a refused run leaves a version's temp file alone (G9: no write before authorization) */
    spits(P("%s/onnxruntime.dll.rbsp-new", V19), "half");
    CHECK(run_verb(ADMIN_INSTALL, "7.2.18", 0) == ADMIN_EXIT_SCOPE_CHANGED && exists(P("%s/onnxruntime.dll.rbsp-new", V19)), "refused: nothing written there");
}

static void t_put_back_changed(void) {
    /* A13: a put-back that changed the folder and didn't verify, twice: 11, not "nothing changed" */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    F.pin_no_sub = "rekordbox 7.2.19/onnxruntime.dll";   /* the live file put back never verifies */
    int x = run_verb(ADMIN_UNINSTALL, "", 0);
    F.pin_no_sub = NULL;
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
    CHECK(run_verb(ADMIN_UNINSTALL, "", 0) == ADMIN_EXIT_OK && !exists(P("%s/onnxruntime.dll.rbsp-new", V19)) && !exists(P("%s/onnxruntime.dll.rbsp-probe", V19)),
          "uninstall over Pioneer's: the temp files gone");
}

static void t_removal_folders(void) {
    /* A11: folders that can't go now are scheduled (@restart); ones that can't be either are said */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    F.rmdir_fail = 1;
    int x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && fake_said(&F, "@restart") && F.n_scheduled == 3 && !fake_said(&F, "stay"), "folders left: scheduled, @restart said (%d)", x);
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    F.rmdir_fail = F.schedule_fail = 1;
    x = run_verb(ADMIN_UNINSTALL, "", 0);
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

/* review OR3: an older bridge of ours live with its receipt, a newer one in the payload: Install
 * (no force) puts the payload's in */
static void t_older_bridge(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1), "BRIDGE1 installed");
    source("1.5.1", BRIDGE2, WORKER_A, 1, "rbstemsplus-bridge-win-x64.dll");
    int x = install();
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE2), "Install over an older bridge: the payload's in (%d)\n%s", x, F.said);
    CHECK(!fake_said(&F, "saving Pioneer's file") && holds(P("%s/pioneer/7.2.19/onnxruntime.dll", ROOT), ort_of("7.2.19")), "the saved set kept as it was");
}

#ifdef ADMIN_TEST_HOLD
/* the test build's mid-work holds (worker.h's admin_test_mid_hold; the VM's WC3 and WC10): at the
 * points where the work has begun changing rekordbox, the files there being what a run cut off
 * at it (a sign-out) leaves; never in a run that changes nothing */
static struct { int bridge, pioneer, saved, ort, receipt, temp, root, bin, v20_receipt, v20_stock, v20_bridge, v20_temp; } at_hold;
static void snap(fake *f) {
    (void)f;
    at_hold.bridge = holds(P("%s/onnxruntime.dll", V19), BRIDGE1);
    at_hold.pioneer = pioneers(V19, "7.2.19");
    at_hold.saved = holds(P("%s/pioneer/7.2.19/SHA256SUMS", ROOT), P("%s  onnxruntime.dll\n", sha_of(ort_of("7.2.19"))));
    at_hold.ort = holds(P("%s/ort/onnxruntime.dll", ROOT), ort_of("7.2.19"));
    at_hold.receipt = has_receipt("7.2.19");
    at_hold.temp = exists(P("%s/onnxruntime.dll.rbsp-new", V19));
    at_hold.root = exists(ROOT);
    at_hold.bin = exists(P("%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A)));
    at_hold.v20_receipt = has_receipt("7.2.20");
    at_hold.v20_stock = exists(V20) && pioneers(V20, "7.2.20");
    at_hold.v20_bridge = holds(P("%s/onnxruntime.dll", V20), BRIDGE1);
    at_hold.v20_temp = exists(P("%s/onnxruntime.dll.rbsp-new", V20));
}

static void t_mid_hold(void) {
    fresh();
    F.on_mid_hold = snap;
    int x = install();
    CHECK(x == ADMIN_EXIT_OK && F.mid_holds == 1 && strstr(F.mid_where, "bridge swapped"), "install: one mid-work hold, after the swap (%d, %d holds: %s)", x,
          F.mid_holds, F.mid_where);
    CHECK(at_hold.bridge && at_hold.saved && at_hold.ort && !at_hold.temp && !at_hold.receipt,
          "at it, rekordbox has the bridge whole, Pioneer's file saved, no temp file, no receipt yet");
    CHECK(installed(V19, "7.2.19", BRIDGE1) && fake_said(&F, "Stems Cache installed"), "and the run goes on after it\n%s", F.said);
    x = install();
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && F.mid_holds == 1, "done, named again: no hold (%d holds)", F.mid_holds);

    snprintf(F.self, sizeof F.self, "%s/bin/%s/RB Stems Plus.exe", ROOT, sha12_of(WORKER_A));
    memset(&at_hold, 0, sizeof at_hold);
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && F.mid_holds == 2 && strstr(F.mid_where, "put back"), "uninstall: one mid-work hold, after the put-back (%d, %d holds: %s)", x,
          F.mid_holds, F.mid_where);
    CHECK(at_hold.pioneer && !at_hold.temp && at_hold.root && at_hold.bin, "at it, rekordbox has Pioneer's file whole, our folder and its worker still there");
    CHECK(pioneers(V19, "7.2.19") && !exists(ROOT), "and the run goes on after it\n%s", F.said);
    x = run_verb(ADMIN_UNINSTALL, "", 0);
    CHECK(x == ADMIN_EXIT_OK && F.mid_holds == 2, "an uninstall with nothing to put back: no hold (%d, %d holds)", x, F.mid_holds);

    fresh();
    F.on_mid_hold = snap;
    CHECK(install() == ADMIN_EXIT_OK && F.mid_holds == 1, "installed");
    x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && F.mid_holds == 1, "restore: no hold (%d, %d holds)", x, F.mid_holds);
    x = run_verb(ADMIN_INSTALL_WORKER, "", 0);
    CHECK(F.mid_holds == 1, "install-worker: no hold (%d, %d holds)", x, F.mid_holds);
}

/* the files as a run cut off at the hold leaves them (a sign-out): kept, then put in place of
 * what the rest of the run did */
static const char *cut_at;   /* the hold to cut at (a part of its where) */
static int cut_done;
static void cut(fake *f) {
    if (cut_done || !strstr(f->mid_where, cut_at)) return;
    cut_done = 1;
    snap(f);
    /* the root, and each version folder's files in place (copying a folder would give it another
     * identity, which its receipt rightly refuses) */
    sh(P("rm -rf '%s/cut' && mkdir -p '%s/cut/v19' '%s/cut/v20' && cp -Rp '%s' '%s/cut/root'", S, S, S, ROOT, S));
    sh(P("cp -p '%s'/* '%s/cut/v19/' && { [ ! -d '%s' ] || cp -p '%s'/* '%s/cut/v20/'; }", V19, S, V20, V20, S));
}
static void cut_apply(void) {
    sh(P("rm -rf '%s' && mv '%s/cut/root' '%s' && rm -f '%s'/* && cp -p '%s/cut/v19'/* '%s/'", ROOT, S, ROOT, V19, S, V19));
    sh(P("[ ! -d '%s' ] || { rm -f '%s'/* && cp -p '%s/cut/v20'/* '%s/'; }; rm -rf '%s/cut'", V20, V20, S, V20, S));
}

/* review OR1: the receipt goes before the swap, so a run cut off after it leaves none; such a
 * bridge is never "done" and never adopted (D13): Put Back repairs it, then an install */
static void t_receipt_cut(void) {
    fresh();
    CHECK(install() == ADMIN_EXIT_OK && has_receipt("7.2.19"), "installed, a receipt");
    CHECK(run_verb(ADMIN_RESTORE, "7.2.19", 0) == ADMIN_EXIT_OK && pioneers(V19, "7.2.19"), "restored");
    cut_at = "bridge swapped";
    cut_done = 0;
    F.mid_holds = 0;
    F.on_mid_hold = cut;
    int x = install();
    F.on_mid_hold = NULL;
    CHECK(x == ADMIN_EXIT_OK && F.mid_holds == 1 && at_hold.bridge && !at_hold.receipt, "installed again, held after the swap: no receipt there (%d)\n%s", x, F.said);
    cut_apply();
    CHECK(holds(P("%s/onnxruntime.dll", V19), BRIDGE1) && !has_receipt("7.2.19"), "cut off there: the bridge, no receipt");
    F.load_calls = 0;
    x = install();
    CHECK(x == ADMIN_EXIT_SCOPE_CHANGED && F.load_calls == 0, "the next install: not done, not taken (needs repair): 15 (%d)", x);
    x = run_verb(ADMIN_RESTORE, "7.2.19", 0);
    CHECK(x == ADMIN_EXIT_OK && pioneers(V19, "7.2.19"), "Put Back: the bound set (%d)\n%s", x, F.said);
    CHECK(install() == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1), "then installed");
    /* a receipt that can't be copied or removed: refused before its deletion, nothing changed */
    fresh();
    CHECK(install() == ADMIN_EXIT_OK, "installed");
    CHECK(run_verb(ADMIN_RESTORE, "7.2.19", 0) == ADMIN_EXIT_OK, "restored");
    sh(P("mkdir '%s/receipts/7.2.19' && touch '%s/receipts/7.2.19/x'", ROOT, ROOT));
    F.mid_holds = 0;
    x = install();
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && pioneers(V19, "7.2.19") && F.mid_holds == 0 && !exists(P("%s/onnxruntime.dll.rbsp-new", V19))
              && fake_said(&F, "couldn't keep a copy of rekordbox 7.2.19's receipt") && !fake_said(&F, "couldn't remove rekordbox 7.2.19's receipt"),
          "the receipt neither copied nor removable: 4 before its deletion, nothing changed (%d)\n%s", x, F.said);
}

/* an interrupted batch: cut after target 1's receipt, before target 2's swap; the retry's scope
 * is the second one only */
static void t_batch_cut(void) {
    two();
    cut_at = "batch: a target done";
    cut_done = 0;
    F.mid_holds = 0;
    memset(&at_hold, 0, sizeof at_hold);
    F.on_mid_hold = cut;
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    F.on_mid_hold = NULL;
    CHECK(x == ADMIN_EXIT_OK && cut_done && at_hold.receipt && !at_hold.v20_receipt && at_hold.v20_stock, "held between the targets (%d, %d holds)\n%s", x, F.mid_holds,
          F.said);
    cut_apply();
    CHECK(installed(V19, "7.2.19", BRIDGE1) && pioneers(V20, "7.2.20"), "cut off there: 7.2.19 installed, 7.2.20 Pioneer's");
    CHECK(run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0) == ADMIN_EXIT_SCOPE_CHANGED, "the old scope: 15");
    x = run_targets(ADMIN_INSTALL, "7.2.20", 0);
    CHECK(x == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1) && installed(V20, "7.2.20", BRIDGE1), "the retry: 7.2.20 only (%d)", x);
}

static void t_test_target_faults(void) {
    two();
    F.fail_target = "7.2.20";
    int x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    F.fail_target = NULL;
    CHECK(x == ADMIN_EXIT_PARTIAL && installed(V19, "7.2.19", BRIDGE1) && pioneers(V20, "7.2.20") && !has_receipt("7.2.20")
              && fake_said(&F, "\n@target 7.2.19 0 installed\n") && fake_said(&F, "\n@target 7.2.20 6 put-back\n"),
          "targeted test failure rolls back only 7.2.20 (%d)\n%s", x, F.said);
    CHECK(run_verb(ADMIN_UNINSTALL, "", 0) == ADMIN_EXIT_OK && pioneers(V19, "7.2.19") && pioneers(V20, "7.2.20"),
          "uninstall following the targeted fault");

    two();
    cut_at = "bridge swapped into rekordbox 7.2.20";
    cut_done = 0;
    F.mid_target = "7.2.20";
    F.on_mid_hold = cut;
    x = run_targets(ADMIN_INSTALL, "7.2.19,7.2.20", 0);
    F.on_mid_hold = NULL;
    F.mid_target = NULL;
    CHECK(x == ADMIN_EXIT_OK && F.mid_holds == 1 && cut_done && at_hold.receipt && at_hold.v20_bridge
              && !at_hold.v20_receipt && !at_hold.v20_temp, "targeted hold is after target 2's swap and before its receipt (%d)\n%s", x, F.said);
    if (!cut_done) return;
    cut_apply();
    CHECK(run_verb(ADMIN_RESTORE, "7.2.20", 0) == ADMIN_EXIT_OK && installed(V19, "7.2.19", BRIDGE1) && pioneers(V20, "7.2.20"),
          "Put Back repairs only the interrupted target");
}
#endif

void test_worker(const char *scratch, const char *pedir) {
    if (!realpath(scratch, S)) abort();
    snprintf(RB, sizeof RB, "%s/rekordbox", S);
    snprintf(V19, sizeof V19, "%s/rekordbox 7.2.19", RB);
    snprintf(V20, sizeof V20, "%s/rekordbox 7.2.20", RB);
    snprintf(V21, sizeof V21, "%s/rekordbox 7.2.21", RB);
    snprintf(ROOT, sizeof ROOT, "%s/RB Stems Plus", S);
    snprintf(SRC, sizeof SRC, "%s/src", S);
    snprintf(APP, sizeof APP, "%s/app", S);
    exe = slurp(P("%s/rekordbox-x64.exe", pedir), &exe_n);
    if (!exe) {
        fprintf(stderr, "no PE fixture in %s\n", pedir);
        abort();
    }
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
    t_batch_two_stock();
    t_batch_three();
    t_batch_ort_rewritten();
    t_batch_scope_changed();
    t_batch_refusal_writes_nothing();
    t_batch_reassessed_under_hold();
    t_batch_unchecked_runtime_user();
    t_put_back_authorized_only();
    t_success_needs_authorized_file();
    t_exe_rechecked_at_replacement();
    t_restore_checks_all_first();
    t_batch_gate_each_target();
    t_refused_install_changes_nothing();
    t_refusal_keeps_another_runs_root();
    t_late_refusal_keeps_root();
    t_gate_before_shared_writes();
    t_failed_repair_undone();
    t_refused_reinstall_keeps_receipt();
    t_undo_missing_receipt_folder();
    t_failed_undo_keeps_copies();
    t_retained_undo_cleanup();
    t_restore_holds_exe();
    t_payload_copies_before_gate();
    t_batch_incomplete_inventory();
    t_batch_fail_second();
    t_batch_preflight_refuses_all();
    t_batch_unbound_original();
    t_batch_unlisted_version();
    t_batch_keeps_old_bridge();
    t_restore_targets();
    t_restore_identity();
    t_uninstall_unverified_newest();
    t_uninstall_partial();
    t_install_worker_protocol();
    characterize_today_selects_newest_registered();
    characterize_today_uninstall_sweeps_disk();
    t_install_worker();
    t_remove_root_folder();
    t_leftovers();
    t_put_back_changed();
    t_temp_files();
    t_removal_folders();
    t_worker_replace_fails();
    t_older_bridge();
#ifdef ADMIN_TEST_HOLD
    t_mid_hold();
    t_test_target_faults();
    t_receipt_cut();
    t_batch_cut();
#endif
    test_key_free(&key);
    free(exe);
}
