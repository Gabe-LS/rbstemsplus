/* payload.c beyond the shared fixtures: the windows block (release.md §1.2), suffixed versions
 * refused (§2), the size cap and the order of the checks, the version order, the floor. */
#include "check.h"

#include "../payload.h"
#include "sig_mac.h"

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (!d) abort();
    memcpy(d, s, n);
    return d;
}

/* s with its one occurrence of from replaced by to (a test fails if there isn't exactly one) */
static char *edit(const char *s, const char *from, const char *to) {
    const char *p = strstr(s, from);
    CHECK(p && !strstr(p + 1, from), "the payload has %s once", from);
    if (!p) return dup_str(s);
    size_t a = (size_t)(p - s), fl = strlen(from), tl = strlen(to), rest = strlen(p + fl);
    char *o = malloc(a + tl + rest + 1);
    if (!o) abort();
    memcpy(o, s, a);
    memcpy(o + a, to, tl);
    memcpy(o + a + tl, p + fl, rest + 1);
    return o;
}

#define H(c) c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c
static const char *const mac_part =
    "\"app\": {\"version\": \"%s\", \"commit\": \"c\", \"zip\": \"rbstemsplus-app.zip\", \"sha256\": \"" H("a") "\"},\n"
    " \"bridge\": {\"file\": \"libonnxruntime.1.18.0.dylib\", \"commit\": \"c\", \"sha256\": \"" H("a") "\"},\n"
    " \"compatible\": {\"rekordbox\": [\"7.2.17\", \"7.2.18\", \"7.2.19\"], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": [\"0002\"]},\n";
static const char *const windows_part =
    ",\n \"windows\": {\n"
    "  \"app\": {\"version\": \"%s\", \"zip\": \"rbstemsplus-windows.zip\", \"sha256\": \"" H("1") "\", \"admin_sha256\": \"" H("2") "\",\n"
    "           \"stub_sha256\": {\"x64\": \"" H("3") "\", \"arm64\": \"" H("4") "\"}},\n"
    "  \"bridge\": {\"file\": \"rbstemsplus-bridge-win-x64.dll\", \"sha256\": \"" H("5") "\"},\n"
    "  \"compatible\": {\"rekordbox\": [\"7.2.19\"], \"ort_sha256\": [\"" H("6") "\"], \"stems_engine\": [\"0002\"]},\n"
    "  \"resampler_proved\": [{\"product_version\": \"7.2.19\", \"rekordbox_sha256\": \"" H("7") "\"}]\n"
    " }";

/* a payload.json as release.sh will write it: the Mac part when mac, the windows block when win_app */
static char *build(const char *pv, int mac, const char *win_app) {
    char macbuf[1024] = "", winbuf[1024] = "";
    if (mac) snprintf(macbuf, sizeof macbuf, mac_part, pv);
    if (win_app) snprintf(winbuf, sizeof winbuf, windows_part, win_app);
    char *o = malloc(4096);
    if (!o) abort();
    snprintf(o, 4096,
             "{\n \"payload_version\": \"%s\",\n \"commit\": \"c\",\n %s"
             " \"model\": {\"file\": \"stemsplus-model.onnx\", \"sha256\": \"" H("c") "\", \"size\": 308572524},\n"
             " \"previous_models\": [\"" H("d") "\"]%s\n}\n",
             pv, macbuf, winbuf);
    return o;
}

static payload_manifest m;

/* a JSON list of n copies of item */
static char *list_of(const char *item, int n) {
    size_t len = strlen(item);
    char *o = malloc((len + 2) * (size_t)n + 3);
    if (!o) abort();
    char *w = o;
    *w++ = '[';
    for (int i = 0; i < n; i++) {
        if (i) *w++ = ',';
        memcpy(w, item, len);
        w += len;
    }
    *w++ = ']';
    *w = 0;
    return o;
}

static int decode(const char *s) { return payload_decode(s, strlen(s), &m); }

static int bound_ort(const char *version, const char *sha) {
    const char *actual = payload_win_ort_for(&m, version);
    return actual && !strcmp(actual, sha);
}

#define ORT_BIND_19 "{\"rekordbox\": \"7.2.19\", \"sha256\": \"" H("6") "\"}"
#define ORT_BIND_20 "{\"rekordbox\": \"7.2.20\", \"sha256\": \"" H("8") "\"}"

static void test_ort_bindings(const char *old) {
    char *bound = edit(old, "[\"7.2.19\"], \"ort_sha256\": [\"" H("6") "\"]",
                       "[\"7.2.19\", \"7.2.20\"], \"ort_sha256\": [\"" H("6") "\", \"" H("8") "\"], "
                       "\"ort\": [" ORT_BIND_20 ", " ORT_BIND_19 "], \"ort_code_sha256\": \"" H("9") "\"");
    CHECK(decode(bound) == PAYLOAD_OK && m.n_win_ort_bind == 2 && !strcmp(m.win_ort_code_sha256, H("9")),
          "the version bindings and shared code digest are decoded");
    CHECK(bound_ort("7.2.19", H("6")) && bound_ort("7.2.20", H("8")), "each version has its own original hash");
    CHECK(bound_ort("7.2.19.0342", H("6")) && bound_ort("7.2.20.0001", H("8")), "dotted build suffixes inherit the binding");
    CHECK(bound_ort("7.2.19", H("6")) && !payload_win_ort_for(&m, "7.2.190") && !payload_win_ort_for(&m, "7.2.19x")
              && !payload_win_ort_for(&m, "7.2.21") && !payload_win_ort_for(&m, NULL), "only the listed version or its dotted prefix binds");

    char *short_list = edit(old, "[\"7.2.19\"], \"ort_sha256", "[\"7.2\"], \"ort_sha256");
    CHECK(decode(short_list) == PAYLOAD_NOT_VALID, "a Windows list cannot authorize a whole minor series");
    free(short_list);

    char *build = edit(bound, "7.2.20\"], \"ort_sha256", "7.2.19.0342\"], \"ort_sha256");
    char *more_specific = edit(build, "[" ORT_BIND_20 ", " ORT_BIND_19 "]",
                               "[" ORT_BIND_19 ", {\"rekordbox\": \"7.2.19.0342\", \"sha256\": \"" H("8") "\"}]");
    CHECK(decode(more_specific) == PAYLOAD_NOT_VALID, "a Windows list names releases rather than individual builds");
    free(more_specific);
    free(build);

    static const struct { const char *from, *to, *why; } invalid[] = {
        {ORT_BIND_20 ", ", "", "missing version binding"},
        {ORT_BIND_20, ORT_BIND_19, "duplicate version binding"},
        {ORT_BIND_20, "{\"rekordbox\": \"7.2.21\", \"sha256\": \"" H("8") "\"}", "unknown version"},
        {ORT_BIND_20, "{\"rekordbox\": \"7.2.20.0001\", \"sha256\": \"" H("8") "\"}", "version only matches by prefix"},
        {ORT_BIND_20, "{\"rekordbox\": \"7.2.20\", \"sha256\": \"" H("a") "\"}", "unlisted original hash"},
        {"\"ort_code_sha256\"", "\"other\"", "missing shared code digest"},
        {"\"ort_code_sha256\": \"" H("9") "\"", "\"ort_code_sha256\": \"" H("A") "\"", "malformed shared code digest"},
        {"[" ORT_BIND_20 ", " ORT_BIND_19 "]", "null", "null binding list"},
        {ORT_BIND_20, "1", "non-object binding"},
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        char *bad = edit(bound, invalid[i].from, invalid[i].to);
        CHECK(decode(bad) == PAYLOAD_NOT_VALID, "%s is refused", invalid[i].why);
        free(bad);
    }
    CHECK(decode(bound) == PAYLOAD_OK && bound_ort("7.2.19", H("6")) && decode(old) == PAYLOAD_OK && m.n_win_ort_bind == 0
              && !m.win_ort_code_sha256[0] && !payload_win_ort_for(&m, "7.2.19"), "an older payload clears previously decoded bindings");
    free(bound);
}

void test_payload(void) {
    /* -- versions */
    CHECK(payload_is_release_version("1.5.0") && payload_is_release_version("123456789.1.1"), "release versions");
    CHECK(!payload_is_release_version("1.5.0-beta.1") && !payload_is_release_version("1.5.0-rc.1") && !payload_is_release_version("1.5")
              && !payload_is_release_version("1234567890.1.1") && !payload_is_release_version(NULL),
          "not release versions: a suffix is refused (no beta channel)");
    static const struct { const char *a, *b; int c; } cmp[] = {
        {"1.4.9", "1.5.0", -1}, {"1.5.1", "1.5.0", 1}, {"1.0.10", "1.0.9", 1}, {"08.09.10", "8.9.10", 0}, {"1.5.0", "1.5.0", 0},
        {"1.5.0", "1.5", 2}, {"x", "1.0.0", 2}, {"1.0.0", NULL, 2}, {"1.5.0-beta.2", "1.5.0", 2}, {"1.5.0", "1.5.0-beta.2", 2},
    };
    for (size_t i = 0; i < sizeof cmp / sizeof cmp[0]; i++)
        CHECK(payload_version_cmp(cmp[i].a, cmp[i].b) == cmp[i].c, "%s vs %s", cmp[i].a, cmp[i].b ? cmp[i].b : "NULL");

    /* -- a final with the windows block */
    char *fin = build("1.5.0", 1, "1.5.0");
    CHECK(decode(fin) == PAYLOAD_OK && m.has_windows, "a final with the windows block");
    CHECK(!strcmp(m.win_app.version, "1.5.0") && !strcmp(m.win_app.zip, "rbstemsplus-windows.zip") && m.win_app.admin_sha256[0] == '2'
              && m.win_app.stub_x64[0] == '3' && m.win_app.stub_arm64[63] == '4' && !strcmp(m.win_bridge.file, "rbstemsplus-bridge-win-x64.dll")
              && m.win_bridge.sha256[0] == '5',
          "its app and bridge are read");
    CHECK(m.n_win_rekordbox == 1 && !strcmp(m.win_rekordbox[0], "7.2.19") && m.n_win_ort_sha256 == 1 && m.win_ort_sha256[0][0] == '6'
              && m.n_win_stems_engine == 1 && !strcmp(m.win_stems_engine[0], "0002") && m.n_resampler_proved == 1
              && !strcmp(m.resampler_proved[0].product_version, "7.2.19") && m.resampler_proved[0].rekordbox_sha256[0] == '7',
          "its lists are read");
    CHECK(!strcmp(m.model.file, "stemsplus-model.onnx") && m.model.size == 308572524 && m.n_previous_models == 1 && m.previous_models[0][0] == 'd',
          "the shared model");
    test_ort_bindings(fin);
    char *carried = build("1.5.1", 1, "1.5.0");
    CHECK(decode(carried) == PAYLOAD_OK && !strcmp(m.win_app.version, "1.5.0"), "a carried Windows app (older than the release)");
    char *newer = build("1.5.0", 1, "1.5.1");
    CHECK(decode(newer) == PAYLOAD_NOT_VALID, "windows.app.version above payload_version");
    char *mac_only = build("1.4.0", 1, NULL);
    CHECK(decode(mac_only) == PAYLOAD_OK && !m.has_windows, "a Mac-only final: valid, no news for Windows");

    /* every windows key is required, except resampler_proved */
    static const char *const keys[] = {"\"version\": \"1.5.0\", \"zip\"", "\"zip\": \"rbstemsplus-windows.zip\", ",
                                       "\"admin_sha256\"", "\"x64\"", "\"arm64\"", "\"stub_sha256\"", "\"bridge\": {\"file\": \"rbstemsplus",
                                       "\"ort_sha256\"", "\"stems_engine\": [\"0002\"]},\n  \"resampler", "\"product_version\"",
                                       "\"rekordbox_sha256\""};
    static const char *const renamed[] = {"\"versio\": \"1.5.0\", \"zip\"", "\"zi\": \"rbstemsplus-windows.zip\", ", "\"admin_sha25\"",
                                          "\"x6\"", "\"arm6\"", "\"stub_sha25\"", "\"bridg\": {\"file\": \"rbstemsplus", "\"ort_sha25\"",
                                          "\"stems_engin\": [\"0002\"]},\n  \"resampler", "\"product_versio\"", "\"rekordbox_sha25\""};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char *e = edit(fin, keys[i], renamed[i]);
        CHECK(decode(e) == PAYLOAD_NOT_VALID, "without %s", keys[i]);
        free(e);
    }
    char *e = edit(fin, ",\n  \"resampler_proved\": [{\"product_version\": \"7.2.19\", \"rekordbox_sha256\": \"" H("7") "\"}]", "");
    CHECK(decode(e) == PAYLOAD_OK && m.n_resampler_proved == 0, "resampler_proved absent: none proved");
    free(e);
    e = edit(fin, "\"resampler_proved\": [{", "\"resampler_proved\": null, \"x\": [{");
    CHECK(decode(e) == PAYLOAD_OK && m.n_resampler_proved == 0, "resampler_proved null: none proved");
    free(e);
    /* each checksum and name checked */
    static const struct { const char *from, *to; } bad[] = {
        {"\"sha256\": \"" H("1") "\"", "\"sha256\": \"" H("A") "\""},
        {"\"admin_sha256\": \"" H("2") "\"", "\"admin_sha256\": \"12\""},
        {"\"x64\": \"" H("3") "\"", "\"x64\": 3"},
        {"\"arm64\": \"" H("4") "\"", "\"arm64\": \"" H("g") "\""},
        {"\"sha256\": \"" H("5") "\"", "\"sha256\": \"\""},
        {"[\"" H("6") "\"]", "[\"" H("6") "\", \"x\"]"},
        {"\"rekordbox_sha256\": \"" H("7") "\"", "\"rekordbox_sha256\": null"},
        {"\"rbstemsplus-windows.zip\"", "\"..\""},
        {"\"rbstemsplus-bridge-win-x64.dll\"", "\"a\\\\b.dll\""},
        {"\"rekordbox\": [\"7.2.19\"], \"ort", "\"rekordbox\": [\"7.2 19\"], \"ort"},
        {"\"stems_engine\": [\"0002\"]},\n  \"resampler", "\"stems_engine\": \"0002\"},\n  \"resampler"},
        {"\"product_version\": \"7.2.19\"", "\"product_version\": \"\""},
        {"\"resampler_proved\": [{", "\"resampler_proved\": [1, {"},
        {"\"stub_sha256\": {\"x64\"", "\"stub_sha256\": [], \"y\": {\"x64\""},
        {"\"version\": \"1.5.0\", \"zip\": \"rbstemsplus-windows", "\"version\": \"1.5\", \"zip\": \"rbstemsplus-windows"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        e = edit(fin, bad[i].from, bad[i].to);
        CHECK(decode(e) == PAYLOAD_NOT_VALID, "%s -> %s", bad[i].from, bad[i].to);
        free(e);
    }
    e = edit(fin, "\"compatible\": {\"rekordbox\": [\"7.2.19\"]", "\"future\": {\"a\": [1, 2]}, \"compatible\": {\"rekordbox\": [\"7.2.19\"]");
    CHECK(decode(e) == PAYLOAD_OK, "an unknown key in the windows block is ignored");
    free(e);
    /* a list over PAYLOAD_LIST_MAX */
    char list[64 * 12 + 16] = "[";
    for (int i = 0; i <= PAYLOAD_LIST_MAX; i++) strcat(list, i ? ", \"7.2.1\"" : "\"7.2.1\"");
    strcat(list, "]");
    e = edit(fin, "\"rekordbox\": [\"7.2.19\"], \"ort", "\"rekordbox\": LIST, \"ort");
    char *e2 = edit(e, "LIST", list);
    CHECK(decode(e2) == PAYLOAD_NOT_VALID, "%d rekordbox versions: over the list's room", PAYLOAD_LIST_MAX + 1);
    free(e2);
    free(e);

    /* every kept list holds PAYLOAD_LIST_MAX entries, and refuses one more (C12) */
    static const struct { const char *from, *item, *what; } lists[] = {
        {"[\"" H("d") "\"]", "\"" H("d") "\"", "previous_models"},
        {"[\"" H("6") "\"]", "\"" H("6") "\"", "ort_sha256"},
        {"\"stems_engine\": [\"0002\"]},\n  \"resampler", "\"0002\"", "stems_engine"},
        {"[{\"product_version\": \"7.2.19\", \"rekordbox_sha256\": \"" H("7") "\"}]",
         "{\"product_version\": \"7.2.19\", \"rekordbox_sha256\": \"" H("7") "\"}", "resampler_proved"},
    };
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++)
        for (int n = PAYLOAD_LIST_MAX; n <= PAYLOAD_LIST_MAX + 1; n++) {
            char *l = list_of(lists[i].item, n), *to = l;
            if (!strcmp(lists[i].what, "stems_engine")) {
                size_t cap = strlen(l) + 64;
                to = malloc(cap);
                if (!to) abort();
                snprintf(to, cap, "\"stems_engine\": %s},\n  \"resampler", l);
            }
            e = edit(fin, lists[i].from, to);
            int r = decode(e);
            int count = !strcmp(lists[i].what, "previous_models") ? m.n_previous_models
                      : !strcmp(lists[i].what, "ort_sha256")      ? m.n_win_ort_sha256
                      : !strcmp(lists[i].what, "stems_engine")    ? m.n_win_stems_engine
                                                                  : m.n_resampler_proved;
            if (n == PAYLOAD_LIST_MAX) CHECK(r == PAYLOAD_OK && count == n, "%s with %d entries: kept (%d)", lists[i].what, n, count);
            else CHECK(r == PAYLOAD_NOT_VALID, "%s with %d entries: over the list's room", lists[i].what, n);
            if (to != l) free(to);
            free(l);
            free(e);
        }

    /* -- a suffixed version anywhere, or no Mac part: refused (there is no beta channel, release.md §2) */
    char *suffixed = build("1.5.0-beta.2", 1, "1.5.0");
    CHECK(decode(suffixed) == PAYLOAD_NOT_VALID, "a suffixed payload_version: refused");
    char *suffixed_bare = build("1.5.0-beta.2", 0, "1.4.0");
    CHECK(decode(suffixed_bare) == PAYLOAD_NOT_VALID, "a suffixed payload_version without the Mac's keys: refused");
    char *final_bare = build("1.5.0", 0, "1.5.0");
    CHECK(decode(final_bare) == PAYLOAD_NOT_VALID, "a final without the Mac's keys: refused");
    char *suffixed_app = build("1.5.0", 1, "1.5.0-beta.2");
    CHECK(decode(suffixed_app) == PAYLOAD_NOT_VALID, "a suffixed windows.app.version: refused");
    e = edit(fin, "\"app\": {\"version\": \"1.5.0\", \"commit\"", "\"ap\": {\"version\": \"1.5.0\", \"commit\"");
    CHECK(decode(e) == PAYLOAD_NOT_VALID, "the Mac's part in half: refused");
    free(e);

    /* -- stricter than the Mac, by the core's reader (payload.h) */
    e = edit(fin, "\"commit\": \"c\",\n ", "\"note\": \"a\\u0000b\", \"commit\": \"c\",\n ");
    CHECK(decode(e) == PAYLOAD_NOT_VALID, "\\u0000 in a value");
    free(e);
    char deep[64] = "";
    for (int i = 0; i < 17; i++) strcat(deep, "[");
    for (int i = 0; i < 17; i++) strcat(deep, "]");
    char deepkey[96];
    snprintf(deepkey, sizeof deepkey, "\"deep\": %s, \"commit\": \"c\",\n ", deep);
    e = edit(fin, "\"commit\": \"c\",\n ", deepkey);
    CHECK(decode(e) == PAYLOAD_NOT_VALID, "nesting past the core reader's 16");
    free(e);

    /* -- payload_check: the size cap first, then the signature, then JSON, then the floor */
    test_key k;
    CHECK(test_key_new(&k) == 0, "a key");
    sig_pubkey key = {"release", {0}};
    memcpy(key.xy, k.xy, 64);
    unsigned char sig[SIG_DER_MAX];
    size_t sn = test_sign(&k, fin, strlen(fin), sig);
    CHECK(payload_check(fin, strlen(fin), sig, sn, &key, 1, "1.4.0", &m) == PAYLOAD_OK && !strcmp(m.signed_by, "release"), "signed");
    CHECK(payload_check(fin, strlen(fin), sig, sn, &key, 1, "1.5.0", &m) == PAYLOAD_OK, "at the floor");
    CHECK(payload_check(fin, strlen(fin), sig, sn, &key, 1, "1.5.1", &m) == PAYLOAD_OLDER && !m.signed_by, "under the floor");
    static const char *const bad_floor[] = {"v1.5.0", "1.5", "1.5.0 x", "garbage_", "1.0.0-beta.1"};
    for (size_t i = 0; i < sizeof bad_floor / sizeof bad_floor[0]; i++)
        CHECK(payload_check(fin, strlen(fin), sig, sn, &key, 1, bad_floor[i], &m) == PAYLOAD_OLDER, "a floor that isn't a version (%s): refused",
              bad_floor[i]);
    size_t bn = test_sign(&k, suffixed, strlen(suffixed), sig);
    CHECK(payload_check(suffixed, strlen(suffixed), sig, bn, &key, 1, NULL, &m) == PAYLOAD_NOT_VALID && !m.signed_by,
          "a signed payload with a suffixed version: refused");
    const char *notjson = "not JSON at all";
    CHECK(payload_check(notjson, strlen(notjson), NULL, 0, &key, 1, NULL, &m) == PAYLOAD_NO_SIGNATURE, "no signature: JSON never read");
    CHECK(payload_check(notjson, strlen(notjson), sig, bn, &key, 1, NULL, &m) == PAYLOAD_BAD_SIGNATURE, "another's signature: JSON never read");
    size_t big = PAYLOAD_MAX + 1;
    char *huge = malloc(big);
    if (!huge) abort();
    memset(huge, ' ', big);
    huge[0] = '{';
    huge[big - 1] = '}';
    CHECK(payload_check(huge, big, NULL, 0, &key, 1, NULL, &m) == PAYLOAD_TOO_BIG, "over the cap: refused before the signature");
    CHECK(payload_decode(huge, big, &m) == PAYLOAD_TOO_BIG, "over the cap: never parsed");
    CHECK(payload_plain_keys(huge, big), "(it is plain JSON otherwise)");
    free(huge);
    test_key_free(&k);

    /* -- the floor record and the refusals' texts */
    char v[64], t[160];
    CHECK(payload_floor_parse("1.5.0\n", 6, v) && !strcmp(v, "1.5.0"), "a floor record");
    CHECK(payload_accepted_floor("1.5.0", "1.4.0", v) == 1 && !strcmp(v, "1.5.0"), "the record above the minimum");
    CHECK(payload_accepted_floor("1.5.0-beta.2", "1.4.0", v) == 1 && !strcmp(v, "1.4.0"), "a suffixed record isn't a version: ignored");
    CHECK(payload_accepted_floor("garbage_", "1.4.0", v) == 1 && !strcmp(v, "1.4.0"), "a record that isn't a version is ignored");
    CHECK(payload_accepted_floor(NULL, "", v) == 0 && payload_accepted_floor("garbage_", NULL, v) == 0, "no floor at all");
    /* a minimum that isn't a version fails closed (C4) */
    static const char *const bad_min[] = {"v1.5.0", "1.5", "1.5.0 x", "garbage_"};
    for (size_t i = 0; i < sizeof bad_min / sizeof bad_min[0]; i++)
        CHECK(payload_accepted_floor("1.6.0", bad_min[i], v) == -1, "minimum %s: refused", bad_min[i]);
    CHECK(!strcmp(payload_refusal_text(PAYLOAD_OLDER, "1.1.0", "1.2.0", t, sizeof t),
                  "it is payload 1.1.0, older than 1.2.0, the oldest this app accepts now"),
          "the older text, as the Mac's");
    CHECK(!strcmp(payload_refusal_text(PAYLOAD_BAD_SIGNATURE, NULL, NULL, t, sizeof t), "its signature doesn't verify with RB Stems Plus's keys"),
          "the bad signature text, as the Mac's");

    free(fin), free(carried), free(newer), free(mac_only), free(suffixed), free(suffixed_bare), free(final_bare), free(suffixed_app);
}
