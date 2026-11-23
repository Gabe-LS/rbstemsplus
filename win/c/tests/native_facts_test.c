/* native_facts.c (the core DLL's DRIVE, SIZE_WALK and ALPHATHETA slow facts) on plat_mac.c and a
 * fake compat_os: the free bytes of a folder's volume, the cache's entry size, the pin's verdict
 * and its "no answer". */
#include "check.h"

#include "../native_facts.h"
#include "sig_mac.h"

#include <fcntl.h>
#include <sys/stat.h>

static int pin_answer;
static int pins;
static int nf_pin(void *ctx, const char *path, plat_file f) {
    (void)ctx;
    (void)path;
    (void)f;
    pins++;
    return pin_answer;
}

static void spit(const char *path, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    for (size_t i = 0; i < n; i++) fputc('x', f);
    fclose(f);
}

static void put_text(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    fputs(s, f);
    fclose(f);
}

/* native_r1 on a scratch version folder and root, the pin faked: our bridge listed or not */
static void t_r1(const compat_os *os, const char *scratch) {
    char folder[1100], root[1100], ort[1100], live[1200], copy[1200];
    snprintf(folder, sizeof folder, "%s/rekordbox 7.2.19", scratch);
    snprintf(root, sizeof root, "%s/RB Stems Plus", scratch);
    snprintf(ort, sizeof ort, "%s/ort", root);
    mkdir(folder, 0755);
    mkdir(root, 0755);
    mkdir(ort, 0755);
    snprintf(live, sizeof live, "%s/onnxruntime.dll", folder);
    snprintf(copy, sizeof copy, "%s/onnxruntime.dll", ort);
    payload_manifest *saved = calloc(1, sizeof *saved), *inst = calloc(1, sizeof *inst);
    saved->has_windows = inst->has_windows = 1;
    compat_memo_clear();
    CHECK(native_r1(os, folder, root, saved, inst) == NF_R1_LIVE_MISSING, "the live DLL missing: 1");
    /* our bridge, listed in the saved payload; ort\'s copy there, then missing */
    const char *bridge = "MZ ... rbstems bridge: 1.5.0";
    put_text(live, bridge);
    put_text(copy, "MZ Pioneer's ORT");
    unsigned char d[32];
    CHECK(!plat_sha256(bridge, strlen(bridge), d), "hashed");
    for (int i = 0; i < 32; i++) snprintf(saved->win_bridge.sha256 + 2 * i, 3, "%02x", d[i]);
    compat_memo_clear();
    pin_answer = COMPAT_PIN_OK;
    CHECK(native_r1(os, folder, root, saved, inst) == NF_R1_NOTHING, "ours, listed, ort's copy there: nothing");
    /* the documented limit (core.h's CORE_SLOW_R1, review R11): the file facts are memoised by
     * path, size, mtime and identity, so a same-size rewrite with its mtime put back reads as
     * before until the memo is cleared */
    {
        struct stat st;
        CHECK(!stat(live, &st), "the live DLL's times");
        put_text(live, "MZ ... rbstems bridge: 1.5.9");   /* the same size, unlisted bytes */
        struct timespec ts[2] = {st.st_atimespec, st.st_mtimespec};
        CHECK(!utimensat(AT_FDCWD, live, ts, 0), "its mtime put back");
        int kept = native_r1(os, folder, root, saved, inst);
        compat_memo_clear();
        int fresh = native_r1(os, folder, root, saved, inst);
        CHECK(kept == NF_R1_NOTHING && fresh == NF_R1_FOREIGN, "memoised: kept %d, after compat_memo_clear %d", kept, fresh);
        put_text(live, bridge);
        compat_memo_clear();
    }
    remove(copy);
    compat_memo_clear();
    CHECK(native_r1(os, folder, root, saved, inst) == NF_R1_ORT_MISSING, "ours, ort's copy missing: 3");
    put_text(copy, "MZ Pioneer's ORT");
    /* listed only in the installed record's list: still ours */
    memcpy(inst->win_bridge.sha256, saved->win_bridge.sha256, sizeof inst->win_bridge.sha256);
    memset(saved->win_bridge.sha256, 'f', 64);
    compat_memo_clear();
    CHECK(native_r1(os, folder, root, saved, inst) == NF_R1_NOTHING, "listed in the installed record's: nothing");
    /* listed nowhere, with verified lists: neither Pioneer's nor ours */
    memset(inst->win_bridge.sha256, 'e', 64);
    compat_memo_clear();
    CHECK(native_r1(os, folder, root, saved, inst) == NF_R1_FOREIGN, "the bridge's text, unlisted: 2");
    /* no verified list at all: judged as ours (watch.c's rule) */
    compat_memo_clear();
    CHECK(native_r1(os, folder, root, NULL, NULL) == NF_R1_NOTHING, "no list: ours");
    saved->has_windows = 0;
    compat_memo_clear();
    CHECK(native_r1(os, folder, root, saved, NULL) == NF_R1_NOTHING, "a payload without its windows block counts as no list");
    /* a foreign file: the pin says another signer */
    put_text(live, "MZ someone else's DLL");
    pin_answer = COMPAT_PIN_NO;
    compat_memo_clear();
    CHECK(native_r1(os, folder, root, NULL, inst) == NF_R1_FOREIGN, "another signer: 2");
    pin_answer = COMPAT_PIN_ERROR;
    compat_memo_clear();
    CHECK(native_r1(os, folder, root, NULL, inst) == -1, "the pin with no answer: no answer");
    free(saved);
    free(inst);
    compat_memo_clear();
}

/* a signed payload.json and its .sig in dir: windows.app.version app, windows.bridge.sha256 bridge;
 * flip: the signature's last byte changed; nosig: no .sig written */
/* the pair in dir removed */
static void rm_pair(const char *dir) {
    char p[1200];
    snprintf(p, sizeof p, "%s/payload.json", dir);
    remove(p);
    snprintf(p, sizeof p, "%s/payload.json.sig", dir);
    remove(p);
}

static void put_pair(const test_key *k, const char *dir, const char *app, const char *bridge, int flip, int nosig) {
    char j[2048], p[1200];
    snprintf(j, sizeof j,
             "{\"payload_version\": \"1.1.0\", "
             "\"app\": {\"version\": \"1.0.0\", \"zip\": \"a.zip\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"bridge\": {\"file\": \"b.dylib\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"compatible\": {\"rekordbox\": [], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": []}, "
             "\"model\": {\"file\": \"m.onnx\", \"sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\", \"size\": 9}, "
             "\"windows\": {\"app\": {\"version\": \"%s\", \"zip\": \"w.zip\", "
             "\"sha256\": \"1111111111111111111111111111111111111111111111111111111111111111\", "
             "\"admin_sha256\": \"2222222222222222222222222222222222222222222222222222222222222222\", "
             "\"stub_sha256\": {\"x64\": \"3333333333333333333333333333333333333333333333333333333333333333\", "
             "\"arm64\": \"4444444444444444444444444444444444444444444444444444444444444444\"}}, "
             "\"bridge\": {\"file\": \"b.dll\", \"sha256\": \"%s\"}, "
             "\"compatible\": {\"rekordbox\": [\"7.2.19\"], \"ort_sha256\": [], \"stems_engine\": [\"0002\"]}}}",
             app, bridge);
    unsigned char s[SIG_DER_MAX];
    size_t sn = test_sign(k, j, strlen(j), s);
    if (!sn) abort();
    if (flip) s[sn - 1] ^= 1;
    plat_mkdirs(dir);
    snprintf(p, sizeof p, "%s/payload.json", dir);
    put_text(p, j);
    snprintf(p, sizeof p, "%s/payload.json.sig", dir);
    remove(p);
    if (!nosig) {
        FILE *f = fopen(p, "wb");
        if (!f) abort();
        fwrite(s, 1, sn, f);
        fclose(f);
    }
}

/* native_r1_read (review R14): the lists nw_admin_r1 picks, from a scratch support folder with the
 * test key: the saved payload (support\payload) and the installed-release record (or its prev\) */
static void t_r1_read(const compat_os *os, const char *scratch) {
    char base[1100], support[1200], folder[1200], root[1200], ort[1300], live[1300], copy[1400], pay[1300], inst[1300], prev[1400], other[65];
    snprintf(base, sizeof base, "%s/r1-read", scratch);
    snprintf(support, sizeof support, "%s/support", base);
    snprintf(folder, sizeof folder, "%s/rekordbox 7.2.19", base);
    snprintf(root, sizeof root, "%s/RB Stems Plus", base);
    snprintf(ort, sizeof ort, "%s/ort", root);
    snprintf(live, sizeof live, "%s/onnxruntime.dll", folder);
    snprintf(copy, sizeof copy, "%s/onnxruntime.dll", ort);
    snprintf(pay, sizeof pay, "%s/payload", support);
    snprintf(inst, sizeof inst, "%s/installed-release", support);
    snprintf(prev, sizeof prev, "%s/prev", inst);
    plat_mkdirs(support);
    plat_mkdirs(folder);
    plat_mkdirs(ort);
    const char *bridge = "MZ ... rbstems bridge: 1.5.0";
    put_text(live, bridge);
    put_text(copy, "MZ Pioneer's ORT");
    unsigned char d[32];
    char listed[65];
    CHECK(!plat_sha256(bridge, strlen(bridge), d), "hashed");
    for (int i = 0; i < 32; i++) snprintf(listed + 2 * i, 3, "%02x", d[i]);
    memset(other, 'e', 64);
    other[64] = 0;
    test_key k;
    CHECK(test_key_new(&k) == 0, "the test key");
    sig_pubkey keys[1] = {{"test", {0}}};
    memcpy(keys[0].xy, k.xy, 64);
    pin_answer = COMPAT_PIN_OK;

    CHECK(native_r1_read(os, NULL, "1.0.0", keys, 1, folder, root) == -1 && native_r1_read(os, support, NULL, keys, 1, folder, root) == -1,
          "no support folder or own version: no answer");
    /* a signed saved payload that lists the live bridge, a record that lists another: 0 */
    put_pair(&k, pay, "1.0.0", listed, 0, 0);
    put_pair(&k, inst, "1.0.0", other, 0, 0);
    compat_memo_clear();
    CHECK(native_r1_read(os, support, "1.0.0", keys, 1, folder, root) == NF_R1_NOTHING, "the saved payload lists it: 0");
    /* the same payload with its signature flipped, or none: only the record's list, another sha: 2 */
    put_pair(&k, pay, "1.0.0", listed, 1, 0);
    compat_memo_clear();
    CHECK(native_r1_read(os, support, "1.0.0", keys, 1, folder, root) == NF_R1_FOREIGN, "its signature flipped: 2");
    put_pair(&k, pay, "1.0.0", listed, 0, 1);
    compat_memo_clear();
    CHECK(native_r1_read(os, support, "1.0.0", keys, 1, folder, root) == NF_R1_FOREIGN, "no .sig: 2");
    /* a record that names another app version and no payload: no list, judged as ours, by ort\ */
    rm_pair(pay);
    put_pair(&k, inst, "9.9.9", other, 0, 0);
    compat_memo_clear();
    CHECK(native_r1_read(os, support, "1.0.0", keys, 1, folder, root) == NF_R1_NOTHING, "another version's record, no payload: no list, 0");
    remove(copy);
    compat_memo_clear();
    CHECK(native_r1_read(os, support, "1.0.0", keys, 1, folder, root) == NF_R1_ORT_MISSING, "and ort\\'s copy missing: 3");
    put_text(copy, "MZ Pioneer's ORT");
    /* a broken installed-release\ with a good prev\ that lists the bridge (a saved payload lists
     * another, so without prev\'s list it would be 2): 0 */
    put_pair(&k, pay, "1.0.0", other, 0, 0);
    put_pair(&k, inst, "1.0.0", other, 1, 0);
    put_pair(&k, prev, "1.0.0", listed, 0, 0);
    compat_memo_clear();
    CHECK(native_r1_read(os, support, "1.0.0", keys, 1, folder, root) == NF_R1_NOTHING, "a broken record, its prev\\ lists it: 0");
    rm_pair(prev);
    compat_memo_clear();
    CHECK(native_r1_read(os, support, "1.0.0", keys, 1, folder, root) == NF_R1_FOREIGN, "and without prev\\: 2");
    test_key_free(&k);
    compat_memo_clear();
}

/* BLOCKED's facts: the list (saved, else the record; none: nothing listed), the engine, Pioneer's
 * ORT by sha256 */
static void t_blocked(const compat_os *os, const char *scratch) {
    char folder[1100], root[1100], ort[1200], live[1200], model[1100], ver[1200];
    snprintf(folder, sizeof folder, "%s/blocked/rekordbox 7.2.19", scratch);
    snprintf(root, sizeof root, "%s/blocked/RB Stems Plus", scratch);
    snprintf(ver, sizeof ver, "%s/blocked", scratch);
    mkdir(ver, 0755);
    mkdir(folder, 0755);
    mkdir(root, 0755);
    snprintf(ort, sizeof ort, "%s/ort", root);
    mkdir(ort, 0755);
    snprintf(live, sizeof live, "%s/onnxruntime.dll", folder);
    snprintf(model, sizeof model, "%s/hdemucs.onnx", folder);
    payload_manifest *saved = calloc(1, sizeof *saved), *inst = calloc(1, sizeof *inst);
    saved->has_windows = inst->has_windows = 1;
    snprintf(saved->win_rekordbox[0], PAYLOAD_NAME_MAX, "7.2.19");
    saved->n_win_rekordbox = 1;
    snprintf(saved->win_stems_engine[0], PAYLOAD_NAME_MAX, "0002");
    saved->n_win_stems_engine = 1;

    /* listed */
    CHECK(payload_win_rekordbox_supported(saved, "7.2.19.0342") && payload_win_rekordbox_supported(saved, "7.2.19")
              && !payload_win_rekordbox_supported(saved, "7.2.190") && !payload_win_rekordbox_supported(saved, "7.2.1")
              && !payload_win_rekordbox_supported(saved, ""),
          "listed: the entry, or it and a '.'");

    /* plus: the engine from demucs3_ver.txt beside the model */
    char engine[64];
    int sup;
    native_blocked_plus(model, saved, engine, sizeof engine, &sup);
    CHECK(!engine[0] && !sup, "no engine file: none");
    snprintf(ver, sizeof ver, "%s/demucs3_ver.txt", folder);
    put_text(ver, " 0002\r\n");
    native_blocked_plus(model, saved, engine, sizeof engine, &sup);
    CHECK(!strcmp(engine, "0002") && sup, "the engine, trimmed, listed");
    native_blocked_plus(model, NULL, engine, sizeof engine, &sup);
    CHECK(!strcmp(engine, "0002") && !sup, "no verified list: not listed");
    /* the engine matches exactly (engineSupported's contains), never by a dotted prefix */
    put_text(ver, "0002.1\n");
    native_blocked_plus(model, saved, engine, sizeof engine, &sup);
    CHECK(!strcmp(engine, "0002.1") && !sup, "\"0002.1\" against [\"0002\"]: unsupported");
    CHECK(native_listed_exact("0002", saved->win_stems_engine, 1) && !native_listed_exact("0002.1", saved->win_stems_engine, 1)
              && !native_listed_exact("000", saved->win_stems_engine, 1) && !native_listed_exact("", saved->win_stems_engine, 1),
          "exact: the entry alone");
    put_text(ver, " 0002\r\n");

    /* cache */
    native_blocked b;
    compat_memo_clear();
    CHECK(!native_blocked_cache(os, folder, "7.2.19.0342", root, NULL, NULL, NULL, &b) && !b.rekordbox_supported && !b.has_lib && !b.has_root_ort,
          "no verified list: rekordbox unlisted, nothing more");
    CHECK(!native_blocked_cache(os, folder, "7.2.20", root, saved, inst, NULL, &b) && !b.rekordbox_supported, "another rekordbox: unlisted");
    /* listed by the installed record when nothing is saved */
    memcpy(inst->win_rekordbox, saved->win_rekordbox, sizeof inst->win_rekordbox);
    inst->n_win_rekordbox = 1;
    compat_memo_clear();
    CHECK(!native_blocked_cache(os, folder, "7.2.19.0342", root, NULL, inst, NULL, &b) && b.rekordbox_supported && b.has_lib && !b.lib_exists,
          "the record's list; the live DLL missing: no ORT");
    /* Pioneer's ORT, listed by its sha256 */
    const char *pioneer = "MZ Pioneer's onnxruntime";
    put_text(live, pioneer);
    unsigned char d[32];
    plat_sha256(pioneer, strlen(pioneer), d);
    for (int i = 0; i < 32; i++) snprintf(saved->win_ort_sha256[0] + 2 * i, 3, "%02x", d[i]);
    saved->n_win_ort_sha256 = 1;
    pin_answer = COMPAT_PIN_OK;
    compat_memo_clear();
    CHECK(!native_blocked_cache(os, folder, "7.2.19.0342", root, saved, inst, NULL, &b) && b.rekordbox_supported && b.lib_exists && b.lib_signed && b.ort_listed
              && !b.root_ort_exists,
          "Pioneer's, listed by its sha256");
    memset(saved->win_ort_sha256[0], 'e', 64);
    compat_memo_clear();
    CHECK(!native_blocked_cache(os, folder, "7.2.19.0342", root, saved, inst, NULL, &b) && b.lib_signed && !b.ort_listed, "Pioneer's, unlisted");
    /* another signer's DLL: not the original */
    put_text(live, "MZ someone else's");
    pin_answer = COMPAT_PIN_NO;
    compat_memo_clear();
    CHECK(!native_blocked_cache(os, folder, "7.2.19.0342", root, saved, inst, NULL, &b) && b.lib_exists && !b.lib_signed, "another signer: not signed");
    /* the pin with no answer: no answer */
    pin_answer = COMPAT_PIN_ERROR;
    compat_memo_clear();
    CHECK(native_blocked_cache(os, folder, "7.2.19.0342", root, saved, inst, NULL, &b) == -1, "the pin with no answer: -1");
    CHECK(b.rekordbox_supported && b.has_root_ort && !b.has_lib, "-1: rekordbox supported and the root's ORT read before it (HF7)");
    /* our bridge in: Pioneer's copy in ort\ is the one checked */
    const char *bridge = "MZ rbstems bridge: 1.5.0";
    put_text(live, bridge);
    plat_sha256(bridge, strlen(bridge), d);
    for (int i = 0; i < 32; i++) snprintf(saved->win_bridge.sha256 + 2 * i, 3, "%02x", d[i]);
    char copy[1300];
    snprintf(copy, sizeof copy, "%s/onnxruntime.dll", ort);
    pin_answer = COMPAT_PIN_OK;
    compat_memo_clear();
    CHECK(!native_blocked_cache(os, folder, "7.2.19.0342", root, saved, inst, NULL, &b) && b.has_lib && !b.lib_exists, "ours, ort's copy missing: no ORT");
    put_text(copy, pioneer);
    plat_sha256(pioneer, strlen(pioneer), d);
    for (int i = 0; i < 32; i++) snprintf(saved->win_ort_sha256[0] + 2 * i, 3, "%02x", d[i]);
    compat_memo_clear();
    CHECK(!native_blocked_cache(os, folder, "7.2.19.0342", root, saved, inst, NULL, &b) && b.root_ort_exists && b.lib_exists && b.lib_signed && b.ort_listed,
          "ours, ort's copy Pioneer's and listed");
    free(saved);
    free(inst);
    compat_memo_clear();
}

static void t_inventory_r1_missing(const compat_os *os, const char *scratch) {
    inv_list *list = calloc(1, sizeof *list);
    if (!list) abort();
    static const int trusts[] = {INV_VERIFIED, INV_UNVERIFIED, INV_SETTLING, INV_NO_ANSWER};
    list->n = sizeof trusts / sizeof trusts[0];
    for (int i = 0; i < list->n; i++) {
        list->it[i].trust = trusts[i];
        snprintf(list->it[i].folder, sizeof list->it[i].folder, "%s/missing-runtime-%d", scratch, i);
    }
    inv_assess_row rows[sizeof trusts / sizeof trusts[0]];
    native_inv_caps caps[sizeof trusts / sizeof trusts[0]];
    native_inventory_assess(os, list, NULL, NULL, NULL, rows, caps);
    CHECK(rows[0].r1 == NF_R1_LIVE_MISSING && rows[1].r1 == NF_R1_LIVE_MISSING,
          "registered rows with a present EXE still report a missing runtime");
    CHECK(rows[2].r1 == NF_R1_NOTHING && rows[3].r1 == NF_R1_NOTHING,
          "settling and no-answer rows without a bridge do not offer R1");
    free(list);
}

void test_native_facts(const char *scratch) {
    native_fact nf;
    compat_os os;
    memset(&os, 0, sizeof os);
    os.pin = nf_pin;
    char p[1200], top[1100], pre[1200];

    /* DRIVE: the folder's volume answers; a folder that isn't there can't be measured */
    CHECK(!native_fact_read(&os, NF_SLOW_DRIVE, scratch, &nf) && nf.has_free && nf.free_bytes > 0 && !nf.has_size && !nf.has_signature,
          "free bytes: %lld", (long long)nf.free_bytes);
    snprintf(p, sizeof p, "%s/not-connected/Stems", scratch);
    CHECK(native_fact_read(&os, NF_SLOW_DRIVE, p, &nf) != 0 && !nf.has_free, "a location not there: couldn't be measured");

    /* SIZE_WALK: entriesSize */
    snprintf(top, sizeof top, "%s/cache", scratch);
    CHECK(!native_fact_read(&os, NF_SLOW_SIZE_WALK, top, &nf) && nf.has_size && nf.size_bytes == 0, "not there: 0");
    mkdir(top, 0755);
    snprintf(pre, sizeof pre, "%s/1111111111111111111111111111111111111111111111111111111111111111", top);
    mkdir(pre, 0755);
    snprintf(pre + strlen(pre), sizeof pre - strlen(pre), "/0a");
    mkdir(pre, 0755);
    snprintf(p, sizeof p, "%s/0a00000000000000000000000000000000000000000000000000000000000000.flac", pre);
    spit(p, 100000);
    CHECK(!native_fact_read(&os, NF_SLOW_SIZE_WALK, top, &nf) && nf.has_size && nf.size_bytes >= 100000, "an entry counts: %lld",
          (long long)nf.size_bytes);
    snprintf(p, sizeof p, "%s/a-file", scratch);
    spit(p, 10);
    CHECK(native_fact_read(&os, NF_SLOW_SIZE_WALK, p, &nf) != 0, "a file, not a folder: couldn't be measured");

    /* ALPHATHETA: the pin's verdict; nothing there is "not signed"; no answer can't be measured */
    snprintf(p, sizeof p, "%s/onnxruntime.dll", scratch);
    spit(p, 64);
    compat_memo_clear();
    pin_answer = COMPAT_PIN_OK;
    CHECK(!native_fact_read(&os, NF_SLOW_ALPHATHETA, p, &nf) && nf.has_signature && nf.signature_ok && pins == 1, "pinned: ok");
    compat_memo_clear();
    pin_answer = COMPAT_PIN_NO;
    CHECK(!native_fact_read(&os, NF_SLOW_ALPHATHETA, p, &nf) && nf.has_signature && !nf.signature_ok, "another signer: not ok");
    compat_memo_clear();
    pin_answer = COMPAT_PIN_ERROR;
    CHECK(native_fact_read(&os, NF_SLOW_ALPHATHETA, p, &nf) != 0 && !nf.has_signature, "no answer: couldn't be measured");
    snprintf(p, sizeof p, "%s/missing.dll", scratch);
    CHECK(!native_fact_read(&os, NF_SLOW_ALPHATHETA, p, &nf) && nf.has_signature && !nf.signature_ok, "nothing there: not signed");
    CHECK(!native_fact_read(&os, NF_SLOW_ALPHATHETA, scratch, &nf) && nf.has_signature && !nf.signature_ok, "a folder: not signed");

    /* R1's state from compat_r1_facts' answer */
    compat_r1 r;
    memset(&r, 0, sizeof r);
    r.live = COMPAT_LIVE_PIONEER;
    CHECK(native_r1_state(&r, 1) == NF_R1_NOTHING, "Pioneer's: nothing");
    r.live = COMPAT_LIVE_MISSING;
    CHECK(native_r1_state(&r, 1) == NF_R1_LIVE_MISSING, "missing: 1");
    r.live = COMPAT_LIVE_FOREIGN;
    CHECK(native_r1_state(&r, 1) == NF_R1_FOREIGN, "foreign: 2");
    r.live = COMPAT_LIVE_OURS_UNLISTED;
    CHECK(native_r1_state(&r, 1) == NF_R1_FOREIGN, "ours unlisted: 2");
    r.live = COMPAT_LIVE_OURS;
    r.ort_looked = 1;
    r.ort_copy = 1;
    CHECK(native_r1_state(&r, 1) == NF_R1_NOTHING, "ours, the ort copy there: nothing");
    r.ort_copy = 0;
    CHECK(native_r1_state(&r, 1) == NF_R1_ORT_MISSING, "ours, the ort copy missing: 3");
    r.ort_looked = 0;
    CHECK(native_r1_state(&r, 1) == -1, "ours, the ort copy not looked at: no answer");
    r.live = COMPAT_LIVE_UNREADABLE;
    CHECK(native_r1_state(&r, 1) == -1, "unreadable: no answer");
    /* no verified list: an unlisted bridge is judged as ours (watch.c's R1 rule) */
    r.live = COMPAT_LIVE_OURS_UNLISTED;
    r.ort_looked = 1;
    r.ort_copy = 1;
    CHECK(native_r1_state(&r, 0) == NF_R1_NOTHING, "no list, unlisted, the ort copy there: nothing");
    r.ort_copy = 0;
    CHECK(native_r1_state(&r, 0) == NF_R1_ORT_MISSING, "no list, unlisted, the ort copy missing: 3");
    r.ort_looked = 0;
    CHECK(native_r1_state(&r, 0) == -1, "no list, unlisted, the ort copy not looked at: no answer");

    CHECK(native_fact_read(&os, 0, scratch, &nf) != 0 && native_fact_read(&os, 5, scratch, &nf) != 0, "other kinds: not here");
    CHECK(native_fact_read(&os, NF_SLOW_DRIVE, "", &nf) != 0 && native_fact_read(&os, NF_SLOW_DRIVE, NULL, &nf) != 0, "no key");
    t_r1(&os, scratch);
    t_blocked(&os, scratch);
    t_r1_read(&os, scratch);
    t_inventory_r1_missing(&os, scratch);
    compat_memo_clear();
}
