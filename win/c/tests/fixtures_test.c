/* The shared fixtures (fixtures.txt, from today's Swift: fixtures.swift): sig.c and payload.c must
 * decide as Signing.swift and Payload.swift do on the same bytes. The few cases where the Windows
 * copy is stricter by design are listed in `differs` with the reason, and checked to be refused. */
#include "check.h"

#include "../payload.h"
#include "../native_config.h"
#include "../update.h"

/* -- the record's fields */

typedef struct {
    unsigned char *b;
    size_t n;
    int nil;
} field;

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "-" nil, "=" empty, else hex: decoded into a NUL-terminated malloc'd buffer */
static field unhex(const char *s) {
    field f = {0};
    if (!strcmp(s, "-")) {
        f.nil = 1;
        return f;
    }
    size_t n = strcmp(s, "=") ? strlen(s) / 2 : 0;
    f.b = malloc(n + 1);
    if (!f.b) abort();
    for (size_t i = 0; i < n; i++) {
        int h = hexval(s[2 * i]), l = hexval(s[2 * i + 1]);
        if (h < 0 || l < 0) abort();
        f.b[i] = (unsigned char)(h << 4 | l);
    }
    f.b[n] = 0;
    f.n = n;
    return f;
}

static const char *str(const field *f) { return f->nil ? NULL : (const char *)f->b; }
static int same(const char *a, const char *b) { return (!a && !b) || (a && b && !strcmp(a, b)); }

/* -- the keys the records name */

enum { KEYS_MAX = 8 };
static sig_pubkey keys[KEYS_MAX];
static char key_names[KEYS_MAX][16];
static int nkeys;

/* "release,backup" into a list of keys; returns the count */
static size_t pick(const char *names, sig_pubkey *out) {
    size_t n = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", names ? names : "");
    for (char *save = NULL, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        for (int i = 0; i < nkeys; i++)
            if (!strcmp(key_names[i], t)) out[n++] = keys[i];
    return n;
}

/* the cases the Windows copy decides differently on purpose */
static const struct {
    const char *kind, *desc, *why;
} differs[] = {
    {"keytext", "the DER with a byte flipped in the OID",
     "CryptoKit takes any SubjectPublicKeyInfo holding a P-256 point; sig.c only the exact P-256 header. Our keys are ours."},
    {"json", "a windows block that is a string", "the Mac ignores \"windows\"; the Windows app reads it, so it must be the block"},
    {"json", "a windows block, its app's version with a suffix", "the Mac ignores \"windows\"; the Windows app refuses a suffixed version (no beta channel)"},
    {"json", "model size 9468852530000000000e-10", "only plain integers are exact; release.sh writes plain integers"},
    {"failure", "failed sign", "the Windows app isn't signed: no \"sign\" step, so no such key (app.md 5.4)"},
};

static const char *differ(const char *kind, const char *desc) {
    for (size_t i = 0; i < sizeof differs / sizeof differs[0]; i++)
        if (!strcmp(differs[i].kind, kind) && !strcmp(differs[i].desc, desc)) return differs[i].why;
    return NULL;
}

static const char *result_name(int r, const payload_manifest *m, char *buf, size_t n) {
    switch (r) {
    case PAYLOAD_OK: snprintf(buf, n, "ok:%s", m->signed_by ? m->signed_by : "?"); return buf;
    case PAYLOAD_NO_SIGNATURE: return "noSignature";
    case PAYLOAD_BAD_SIGNATURE: return "badSignature";
    case PAYLOAD_NOT_VALID: return "notValid";
    case PAYLOAD_OLDER: return "older";
    default: return "other";
    }
}

void test_fixtures(const char *path) {
    FILE *f = fopen(path, "r");
    CHECK(f != NULL, "the fixtures at %s", path);
    if (!f) return;
    static char line[1 << 19];   /* a 64 KB config.ini and its rewrite, in hex */
    int records = 0, seen_differ = 0, config_compared = 0, config_unwritten = 0;
    static payload_manifest m;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\n")] = 0;
        char *fv[10] = {0};
        int nf = 0;
        for (char *save = NULL, *t = strtok_r(line, " ", &save); t && nf < 10; t = strtok_r(NULL, " ", &save)) fv[nf++] = t;
        if (!nf) continue;
        records++;
        field a[9] = {{0}};
        for (int i = 1; i < nf; i++) a[i - 1] = unhex(fv[i]);
        const char *kind = fv[0];

        if (!strcmp(kind, "key") && nf == 3) {
            sig_key_text t = {str(&a[0]), str(&a[1])};
            CHECK(nkeys < KEYS_MAX && sig_key_from_text(&t, &keys[nkeys]) == 0, "key %s", str(&a[0]));
            snprintf(key_names[nkeys], sizeof key_names[0], "%s", str(&a[0]));
            keys[nkeys].name = key_names[nkeys];
            nkeys++;
        } else if (!strcmp(kind, "keytext") && nf == 4) {
            sig_key_text t = {"x", str(&a[1])};
            sig_pubkey k;
            int got = sig_key_from_text(&t, &k) == 0, want = fv[3][0] == '1';
            if (differ(kind, str(&a[0]))) {
                seen_differ++;
                CHECK(!got, "keytext %s: refused on Windows (%s)", str(&a[0]), differ(kind, str(&a[0])));
            } else {
                CHECK(got == want, "keytext %s: %d, Swift %d", str(&a[0]), got, want);
            }
        } else if (!strcmp(kind, "sig") && nf == 6) {
            sig_pubkey ks[KEYS_MAX];
            size_t n = pick(str(&a[3]), ks);
            int who = a[2].nil ? -1 : sig_signer(a[1].b, a[1].n, a[2].b, a[2].n, ks, n);
            const char *got = who >= 0 ? ks[who].name : NULL;
            CHECK(same(got, str(&a[4])), "sig %s: %s, Swift %s", str(&a[0]), got ? got : "nil", str(&a[4]) ? str(&a[4]) : "nil");
        } else if (!strcmp(kind, "json") && nf == 9) {
            int plain = payload_plain_keys(a[1].b, a[1].n);
            int r = payload_decode(a[1].b, a[1].n, &m);
            const char *got = r == PAYLOAD_OK ? m.payload_version : NULL;
            CHECK(plain == (fv[3][0] == '1'), "json %s: plain keys %d, Swift %s", str(&a[0]), plain, fv[3]);
            if (differ(kind, str(&a[0]))) {
                seen_differ++;
                CHECK(r == PAYLOAD_NOT_VALID, "json %s: refused on Windows (%s)", str(&a[0]), differ(kind, str(&a[0])));
            } else {
                CHECK(same(got, str(&a[3])), "json %s: %s, Swift %s", str(&a[0]), got ? got : "nil", str(&a[3]) ? str(&a[3]) : "nil");
                if (got && str(&a[3])) {   /* decoded on both: the same model and previous models */
                    char size[32], prev[PAYLOAD_LIST_MAX * 66 + 1] = "";
                    snprintf(size, sizeof size, "%lld", (long long)m.model.size);
                    for (int i = 0; i < m.n_previous_models; i++) {
                        if (i) strcat(prev, ",");
                        strcat(prev, m.previous_models[i]);
                    }
                    /* the Mac's nil previous_models and an empty list both keep none */
                    const char *want_prev = str(&a[7]) ? str(&a[7]) : "";
                    CHECK(same(m.model.file, str(&a[4])) && same(m.model.sha256, str(&a[5])) && same(size, str(&a[6])) && !strcmp(prev, want_prev),
                          "json %s: model %s %s %s, previous \"%s\"; Swift %s %s %s, \"%s\"", str(&a[0]), m.model.file, m.model.sha256, size, prev,
                          str(&a[4]), str(&a[5]), str(&a[6]), want_prev);
                }
            }
        } else if (!strcmp(kind, "check") && nf == 7) {
            sig_pubkey ks[KEYS_MAX];
            size_t n = pick(str(&a[3]), ks);
            int r = payload_check(a[1].b, a[1].n, a[2].nil ? NULL : a[2].b, a[2].n, ks, n, str(&a[4]), &m);
            char buf[32];
            const char *got = result_name(r, &m, buf, sizeof buf);
            CHECK(!strcmp(got, str(&a[5])), "check %s: %s, Swift %s", str(&a[0]), got, str(&a[5]));
        } else if (!strcmp(kind, "release") && nf == 3) {
            CHECK(payload_is_release_version(str(&a[0])) == (fv[2][0] == '1'), "release %s", str(&a[0]));
        } else if (!strcmp(kind, "older") && nf == 4) {
            int c = payload_version_cmp(str(&a[0]), str(&a[1]));
            CHECK(c != 2 && (c < 0) == (fv[3][0] == '1'), "older %s %s: cmp %d, Swift %s", str(&a[0]), str(&a[1]), c, fv[3]);
        } else if (!strcmp(kind, "floor") && nf == 3) {
            char v[64];
            const char *got = payload_floor_parse((const char *)a[0].b, a[0].n, v) ? v : NULL;
            CHECK(same(got, str(&a[1])), "floor %s: %s, Swift %s", str(&a[0]), got ? got : "nil", str(&a[1]) ? str(&a[1]) : "nil");
        } else if (!strcmp(kind, "accepted") && nf == 4) {
            char v[64];
            const char *got = payload_accepted_floor(str(&a[0]), str(&a[1]), v) == 1 ? v : NULL;
            CHECK(same(got, str(&a[2])), "accepted %s %s: %s, Swift %s", str(&a[0]) ? str(&a[0]) : "nil", str(&a[1]),
                  got ? got : "nil", str(&a[2]) ? str(&a[2]) : "nil");
        } else if (!strcmp(kind, "shownrec") && nf == 4) {
            update_shown r;
            int ok = update_shown_parse(a[0].b, a[0].n, &r);
            if (differ(kind, str(&a[0]))) {
                seen_differ++;
                CHECK(ok, "shownrec %s: read on Windows (%s)", fv[1], differ(kind, str(&a[0])));
            } else {
                CHECK(same(ok ? r.shown : NULL, str(&a[1])) && same(ok ? r.app : NULL, str(&a[2])), "shownrec %s: %s %s, Swift %s %s", fv[1],
                      ok ? r.shown : "nil", ok ? r.app : "nil", str(&a[1]) ? str(&a[1]) : "nil", str(&a[2]) ? str(&a[2]) : "nil");
            }
        } else if (!strcmp(kind, "shown") && nf == 8) {
            update_shown before, after;
            if (!a[1].nil) {
                snprintf(before.shown, sizeof before.shown, "%s", str(&a[1]));
                snprintf(before.app, sizeof before.app, "%s", str(&a[2]));
            }
            int ok = update_next_shown(str(&a[0]), a[1].nil ? NULL : &before, str(&a[3]), str(&a[4]), &after);
            CHECK(same(ok ? after.shown : NULL, str(&a[5])) && same(ok ? after.app : NULL, str(&a[6])),
                  "shown own %s record %s/%s seen %s/%s: %s %s, Swift %s %s", str(&a[0]), str(&a[1]) ? str(&a[1]) : "nil",
                  str(&a[2]) ? str(&a[2]) : "nil", str(&a[3]) ? str(&a[3]) : "nil", str(&a[4]) ? str(&a[4]) : "nil", ok ? after.shown : "nil",
                  ok ? after.app : "nil", str(&a[5]) ? str(&a[5]) : "nil", str(&a[6]) ? str(&a[6]) : "nil");
            if (ok) {   /* and the shown version follows the record */
                CHECK(!strcmp(update_shown_version(str(&a[0]), &after), after.shown), "shown version of %s", after.shown);
            }
        } else if (!strcmp(kind, "failure") && nf == 3) {
            int key = update_fail_parse(a[0].b, a[0].n), known = key > UPDATE_FAIL_GENERIC;
            if (differ(kind, str(&a[0]))) {
                seen_differ++;
                CHECK(key == UPDATE_FAIL_GENERIC, "failure %s: no key on Windows (%s)", str(&a[0]), differ(kind, str(&a[0])));
            } else {
                CHECK(known == (fv[2][0] == '1'), "failure \"%s\": %d, Swift %s", str(&a[0]), key, fv[2]);
            }
        } else if (!strcmp(kind, "configw") && nf == 5) {
            /* fixtures.swift's configOps */
            static const native_config_pair ops[3][2] = {{{"max_gb", "1.5"}, {"max_days", "30"}},
                                                         {{"rekordbox_model", "1"}, {NULL, NULL}},
                                                         {{"cache_dir", "\"/Volumes/X/Stems\""}, {"cache_id", "0E8A1D84-0C4B-4F6B-9E4B-2D5E1C1C9A11"}}};
            static const int nops[4] = {2, 1, 2, 0};
            static const char *const loc[2] = {"cache_dir", "cache_id"};
            int op = atoi(fv[2]), readable = a[0].nil || native_config_readable((const char *)a[0].b, a[0].n);
            CHECK(readable == (fv[3][0] == '1'), "configw %s: readable %d, Swift %s", fv[1], readable, fv[3]);
            if (readable && a[3].nil) config_unwritten++;   /* Foundation's atomic write refused here (the sandbox) */
            else if (readable && op >= 0 && op < 4) {
                config_compared++;
                char *o = NULL;
                size_t on = 0;
                int r = native_config_rewrite(a[0].nil ? NULL : (const char *)a[0].b, a[0].n, op < 3 ? ops[op] : NULL, nops[op],
                                              op == 3 ? loc : NULL, op == 3 ? 2 : 0, &o, &on);
                if (a[3].n > 65536)   /* CFG_MAX: Windows never writes a config.ini it couldn't read again (review S42) */
                    CHECK(r == -1, "configw %s op %d: over the limit, refused (Swift writes %zu bytes)", fv[1], op, a[3].n);
                else
                    CHECK(!r && on == a[3].n && !memcmp(o, a[3].b, on), "configw %s op %d: \"%.*s\", Swift \"%s\"", fv[1], op, (int)on, o ? o : "",
                          str(&a[3]) ? str(&a[3]) : "nil");
                free(o);
            }
        } else if (!strcmp(kind, "modelcase") && nf == 6) {
            /* models_fixtures_test.c's, with a scratch folder */
        } else if (!strcmp(kind, "configsets") && nf == 4) {
            int got = native_config_sets(a[0].nil ? NULL : (const char *)a[0].b, a[0].n, str(&a[1]));
            int readable = a[0].nil || native_config_readable((const char *)a[0].b, a[0].n);
            if (readable) CHECK(got == (fv[3][0] == '1'), "configsets %s %s: %d, Swift %s", fv[1], str(&a[1]), got, fv[3]);
            else CHECK(fv[3][0] == '0', "configsets of an unreadable file: Swift says no");
        } else {
            CHECK(0, "a record this test doesn't know: %s with %d fields", kind, nf);
        }
        for (int i = 0; i < 9; i++) free(a[i].b);
    }
    fclose(f);
    CHECK(records > 1000, "the fixtures hold %d records", records);
    CHECK(seen_differ == (int)(sizeof differs / sizeof differs[0]), "every listed difference is still a case (%d)", seen_differ);
    /* writeConfigValues writes atomically through a folder outside build/: in a sandbox Swift
     * writes nothing. Then the writer isn't compared at all, which fails (review S44) unless
     * RBSP_WINC_NO_SWIFT_WRITE=1 says so on purpose (run.sh shows the note) */
    CHECK(config_compared + config_unwritten > 50, "the config.ini writer's records (%d)", config_compared + config_unwritten);
    const char *no_write = getenv("RBSP_WINC_NO_SWIFT_WRITE");
    if (no_write && !strcmp(no_write, "1")) {
        if (!config_compared) printf("note: the config.ini writer not compared with Swift (RBSP_WINC_NO_SWIFT_WRITE=1)\n");
    } else
        CHECK(config_compared > 0, "the config.ini writer compared with Swift: none (%d not written by Swift: run unsandboxed, or set "
                                   "RBSP_WINC_NO_SWIFT_WRITE=1)", config_unwritten);
    printf("fixtures: %d records from today's Swift\n", records);
}
