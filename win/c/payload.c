/* payload.json's rules and check (payload.h). Pure C11 over the core's strict JSON reader
 * (core/json.c, core/arena.c); the signature through sig.c. */
#include "payload.h"

#include "internal.h"   /* the core's jval, json_parse, arena */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ plainJSONKeys */

typedef struct {
    const unsigned char *b;
    size_t n, i;
} pk;

static void pk_ws(pk *s) {
    while (s->i < s->n && (s->b[s->i] == 0x20 || s->b[s->i] == 0x09 || s->b[s->i] == 0x0a || s->b[s->i] == 0x0d)) s->i++;
}
static int pk_at(const pk *s, unsigned char c) { return s->i < s->n && s->b[s->i] == c; }

/* a string's bytes between its quotes (escapes skipped, not decoded): 1 with start and length */
static int pk_string(pk *s, size_t *start, size_t *len) {
    if (!pk_at(s, 0x22)) return 0;
    s->i++;
    size_t st = s->i;
    while (s->i < s->n) {
        unsigned char c = s->b[s->i];
        if (c == 0x22) { s->i++; *start = st; *len = s->i - 1 - st; return 1; }
        if (c == 0x5c) s->i += 2;
        else if (c < 0x20) return 0;
        else s->i++;
    }
    return 0;
}

static int pk_value(pk *s, int depth) {
    pk_ws(s);
    if (depth >= 32 || s->i >= s->n) return 0;
    switch (s->b[s->i]) {
    case 0x7b: {   /* { */
        s->i++;
        pk_ws(s);
        if (pk_at(s, 0x7d)) { s->i++; return 1; }
        /* the keys seen so far in this object, as (start, length) into b: compared bytewise */
        size_t cap = 16, nk = 0;
        size_t local[2 * 16], *keys = local;
        int ok = 0;
        for (;;) {
            size_t ks, kl;
            pk_ws(s);
            if (!pk_string(s, &ks, &kl) || kl == 0) break;
            int plain = 1;
            for (size_t k = 0; k < kl && plain; k++) {
                unsigned char c = s->b[ks + k];
                plain = c >= 0x20 && c < 0x7f && c != 0x5c;
            }
            if (!plain) break;
            int dup = 0;
            for (size_t k = 0; k < nk && !dup; k++) dup = keys[2 * k + 1] == kl && !memcmp(s->b + keys[2 * k], s->b + ks, kl);
            if (dup) break;
            if (nk == cap) {
                size_t *grown = malloc(4 * cap * sizeof *grown);
                if (!grown) break;
                memcpy(grown, keys, 2 * nk * sizeof *keys);
                if (keys != local) free(keys);
                keys = grown;
                cap *= 2;
            }
            keys[2 * nk] = ks;
            keys[2 * nk + 1] = kl;
            nk++;
            pk_ws(s);
            if (!pk_at(s, 0x3a)) break;
            s->i++;
            if (!pk_value(s, depth + 1)) break;
            pk_ws(s);
            if (pk_at(s, 0x2c)) { s->i++; continue; }
            if (pk_at(s, 0x7d)) { s->i++; ok = 1; }
            break;
        }
        if (keys != local) free(keys);
        return ok;
    }
    case 0x5b:   /* [ */
        s->i++;
        pk_ws(s);
        if (pk_at(s, 0x5d)) { s->i++; return 1; }
        for (;;) {
            if (!pk_value(s, depth + 1)) return 0;
            pk_ws(s);
            if (pk_at(s, 0x2c)) { s->i++; continue; }
            if (pk_at(s, 0x5d)) { s->i++; return 1; }
            return 0;
        }
    case 0x22: {
        size_t a, b;
        return pk_string(s, &a, &b);
    }
    default: {   /* a number, true, false or null: only its characters are checked here */
        static const char chars[] = "+-.0123456789Eaeflnrstu";
        size_t start = s->i;
        while (s->i < s->n && s->b[s->i] && strchr(chars, s->b[s->i])) s->i++;
        return s->i > start;
    }
    }
}

int payload_plain_keys(const void *data, size_t n) {
    pk s = {data, n, 0};
    if (!data) return 0;
    pk_ws(&s);
    if (!pk_at(&s, 0x7b) || !pk_value(&s, 0)) return 0;
    pk_ws(&s);
    return s.i == s.n;
}

/* ------------------------------------------------------------------ versions */

/* a part of 1 to 9 ASCII digits at s, its value in *v; the position after it, or NULL */
static const char *digits(const char *s, unsigned long *v) {
    size_t k = 0;
    unsigned long x = 0;
    while (s[k] >= '0' && s[k] <= '9') {
        if (k == 9) return NULL;
        x = x * 10 + (unsigned long)(s[k] - '0');
        k++;
    }
    if (k == 0) return NULL;
    *v = x;
    return s + k;
}

/* x.y.z: 1 with the parts, or 0 */
static int parse_version(const char *s, unsigned long p[3]) {
    if (!s) return 0;
    for (int i = 0; i < 3; i++) {
        s = digits(s, &p[i]);
        if (!s) return 0;
        if (i < 2 && *s++ != '.') return 0;
    }
    return !*s;
}

int payload_is_release_version(const char *s) {
    unsigned long p[3];
    return parse_version(s, p);
}

int payload_version_cmp(const char *a, const char *b) {
    unsigned long pa[3], pb[3];
    if (!parse_version(a, pa) || !parse_version(b, pb)) return 2;
    for (int i = 0; i < 3; i++)
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    return 0;
}

int payload_is_sha256(const char *s) {
    if (!s) return 0;
    for (int i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return s[64] == 0;
}

/* Manifest.valid's plain: non-empty, not "." or "..", ASCII letters, digits, ".", "_", "-" */
static int plain_name(const char *s) {
    if (!s || !*s || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ the manifest */

/* a string value of obj's key (NULL when absent, null or not a string), its length checked */
static const char *str_of(const jval *v) { return v && v->type == J_STR ? v->s : NULL; }

static int copy(char *dst, size_t cap, const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n >= cap) return 0;
    memcpy(dst, s, n + 1);
    return 1;
}

/* a required string key: present and a string (JSONDecoder: a missing key, null or another type
 * fails the decode) */
static const char *req_str(const jval *obj, const char *key) { return str_of(json_get(obj, key)); }

/* an optional key: absent or null counts as absent (decodeIfPresent) */
static const jval *opt(const jval *obj, const char *key) {
    const jval *v = json_get(obj, key);
    return v && v->type != J_NULL ? v : NULL;
}

/* an array of strings, every one passing ok (NULL: any string); kept into out (up to cap) when
 * out is given. 1 or 0. */
static int str_list(const jval *v, int (*ok)(const char *), char *out, size_t width, int cap, int *count) {
    if (!v || v->type != J_ARR) return 0;
    if (out && v->n > (size_t)cap) return 0;
    for (size_t i = 0; i < v->n; i++) {
        const char *s = str_of(&v->items[i]);
        if (!s || (ok && !ok(s))) return 0;
        if (out && !copy(out + i * width, width, s)) return 0;
    }
    if (count) *count = (int)v->n;
    return 1;
}

/* an integer for an Int, as JSONDecoder reads one: a whole number in range, also written with a
 * fraction or an exponent ("9.0", "9e0", "1E3" are 9, 9 and 1000; "9.5" fails). Past 2^53 only
 * the plain integer form is taken (the core keeps other forms as an approximate double). */
static int int_of(const jval *v, int64_t *out) {
    if (!v || v->type != J_NUM) return 0;
    if (v->is_int) { *out = v->i; return 1; }
    double d = v->num;
    if (!(d > -9007199254740992.0 && d < 9007199254740992.0) || d != (double)(int64_t)d) return 0;
    *out = (int64_t)d;
    return 1;
}

/* Manifest.File: file and sha256 strings, size an optional Int */
static int file_block(const jval *f, const char **file, const char **sha, int64_t *size, int *has_size) {
    if (!f || f->type != J_OBJ) return 0;
    *file = req_str(f, "file");
    *sha = req_str(f, "sha256");
    if (!*file || !*sha) return 0;
    const jval *s = opt(f, "size");
    *has_size = s != NULL;
    *size = 0;
    return !s || int_of(s, size);
}

/* The Mac's part: Manifest's app, bridge and compatible, decoded and valid as the Mac decodes
 * them (the model and previous_models are checked by the caller, shared). */
static int mac_part(const jval *root, const char *payload_version) {
    const jval *app = json_get(root, "app"), *bridge = json_get(root, "bridge"), *compat = json_get(root, "compatible");
    if (!app || app->type != J_OBJ || !compat || compat->type != J_OBJ) return 0;
    const char *av = req_str(app, "version"), *az = req_str(app, "zip"), *as = req_str(app, "sha256");
    const char *bf, *bs;
    int64_t bsize;
    int has;
    if (!av || !az || !as || !file_block(bridge, &bf, &bs, &bsize, &has)) return 0;
    if (!str_list(json_get(compat, "rekordbox"), NULL, NULL, 0, 0, NULL) || !req_str(compat, "ort_version_prefix")
        || !str_list(json_get(compat, "stems_engine"), NULL, NULL, 0, 0, NULL))
        return 0;
    return plain_name(av) && plain_name(az) && plain_name(bf) && payload_is_release_version(payload_version)
        && payload_is_release_version(av) && payload_is_sha256(as) && payload_is_sha256(bs);
}

static int windows_block(const jval *w, payload_manifest *m) {
    if (w->type != J_OBJ) return 0;
    const jval *app = json_get(w, "app"), *bridge = json_get(w, "bridge"), *compat = json_get(w, "compatible");
    if (!app || app->type != J_OBJ || !bridge || bridge->type != J_OBJ || !compat || compat->type != J_OBJ) return 0;

    const jval *stub = json_get(app, "stub_sha256");
    if (!stub || stub->type != J_OBJ) return 0;
    const char *v = req_str(app, "version");
    if (!copy(m->win_app.version, sizeof m->win_app.version, v) || !copy(m->win_app.zip, sizeof m->win_app.zip, req_str(app, "zip"))
        || !copy(m->win_app.sha256, sizeof m->win_app.sha256, req_str(app, "sha256"))
        || !copy(m->win_app.admin_sha256, sizeof m->win_app.admin_sha256, req_str(app, "admin_sha256"))
        || !copy(m->win_app.stub_x64, sizeof m->win_app.stub_x64, req_str(stub, "x64"))
        || !copy(m->win_app.stub_arm64, sizeof m->win_app.stub_arm64, req_str(stub, "arm64")))
        return 0;
    /* the app's own version: x.y.z, at most the release's */
    int c = payload_version_cmp(v, m->payload_version);
    if (c == 2 || c > 0) return 0;
    if (!plain_name(m->win_app.zip) || !payload_is_sha256(m->win_app.sha256) || !payload_is_sha256(m->win_app.admin_sha256)
        || !payload_is_sha256(m->win_app.stub_x64) || !payload_is_sha256(m->win_app.stub_arm64))
        return 0;

    if (!copy(m->win_bridge.file, sizeof m->win_bridge.file, req_str(bridge, "file"))
        || !copy(m->win_bridge.sha256, sizeof m->win_bridge.sha256, req_str(bridge, "sha256"))
        || !plain_name(m->win_bridge.file) || !payload_is_sha256(m->win_bridge.sha256))
        return 0;

    if (!str_list(json_get(compat, "rekordbox"), plain_name, (char *)m->win_rekordbox, PAYLOAD_NAME_MAX, PAYLOAD_LIST_MAX, &m->n_win_rekordbox)
        || !str_list(json_get(compat, "ort_sha256"), payload_is_sha256, (char *)m->win_ort_sha256, sizeof(payload_sha), PAYLOAD_LIST_MAX,
                     &m->n_win_ort_sha256)
        || !str_list(json_get(compat, "stems_engine"), plain_name, (char *)m->win_stems_engine, PAYLOAD_NAME_MAX, PAYLOAD_LIST_MAX,
                     &m->n_win_stems_engine))
        return 0;

    const jval *rp = opt(w, "resampler_proved");
    m->n_resampler_proved = 0;
    if (rp) {
        if (rp->type != J_ARR || rp->n > PAYLOAD_LIST_MAX) return 0;
        for (size_t i = 0; i < rp->n; i++) {
            const jval *e = &rp->items[i];
            if (e->type != J_OBJ) return 0;
            if (!copy(m->resampler_proved[i].product_version, PAYLOAD_NAME_MAX, req_str(e, "product_version"))
                || !copy(m->resampler_proved[i].rekordbox_sha256, sizeof(payload_sha), req_str(e, "rekordbox_sha256"))
                || !plain_name(m->resampler_proved[i].product_version) || !payload_is_sha256(m->resampler_proved[i].rekordbox_sha256))
                return 0;
        }
        m->n_resampler_proved = (int)rp->n;
    }
    m->has_windows = 1;
    return 1;
}

static int decode(const jval *root, payload_manifest *m) {
    if (root->type != J_OBJ) return 0;
    const char *pv = req_str(root, "payload_version");
    if (!copy(m->payload_version, sizeof m->payload_version, pv) || !payload_is_release_version(pv)) return 0;

    /* the Mac's part, as the Mac reads it */
    if (!mac_part(root, pv)) return 0;

    const char *mf, *ms;
    int64_t size;
    int has;
    if (!file_block(json_get(root, "model"), &mf, &ms, &size, &has) || !plain_name(mf) || !payload_is_sha256(ms) || size <= 0)
        return 0;
    if (!copy(m->model.file, sizeof m->model.file, mf) || !copy(m->model.sha256, sizeof m->model.sha256, ms)) return 0;
    m->model.size = size;

    const jval *prev = opt(root, "previous_models");
    m->n_previous_models = 0;
    if (prev && !str_list(prev, payload_is_sha256, (char *)m->previous_models, sizeof(payload_sha), PAYLOAD_LIST_MAX, &m->n_previous_models))
        return 0;

    const jval *w = opt(root, "windows");
    m->has_windows = 0;
    return !w || windows_block(w, m);
}

int payload_decode(const void *data, size_t n, payload_manifest *out) {
    if (!data || !out) return PAYLOAD_NOT_VALID;
    if (n > PAYLOAD_MAX) return PAYLOAD_TOO_BIG;
    memset(out, 0, sizeof *out);
    if (!payload_plain_keys(data, n)) return PAYLOAD_NOT_VALID;
    arena a = {0};
    jval root;
    int r = PAYLOAD_NOT_VALID;
    if (json_parse(&a, data, n, &root) == CORE_OK && decode(&root, out)) r = PAYLOAD_OK;
    arena_free(&a);
    if (r) memset(out, 0, sizeof *out);
    return r;
}

int payload_check(const void *data, size_t n, const void *sig, size_t sig_n, const sig_pubkey *keys, size_t nkeys,
                  const char *floor, payload_manifest *out) {
    if (!out) return PAYLOAD_NOT_VALID;
    memset(out, 0, sizeof *out);
    if (n > PAYLOAD_MAX) return PAYLOAD_TOO_BIG;
    if (!sig || sig_n == 0) return PAYLOAD_NO_SIGNATURE;
    int who = sig_signer(data, n, sig, sig_n, keys, nkeys);
    if (who == -2) return PAYLOAD_NO_CRYPTO;
    if (who < 0) return PAYLOAD_BAD_SIGNATURE;
    int r = payload_decode(data, n, out);
    if (r) return r;
    if (floor && *floor) {   /* a floor that isn't a version refuses everything (fails closed) */
        int c = payload_version_cmp(out->payload_version, floor);
        if (c < 0 || c == 2) return PAYLOAD_OLDER;
    }
    out->signed_by = keys[who].name;
    return PAYLOAD_OK;
}

/* ------------------------------------------------------------------ the floor */

int payload_floor_parse(const char *text, size_t n, char out[64]) {
    if (!text) return 0;
    size_t a = 0, b = n;
    /* trimmingCharacters(in: .whitespacesAndNewlines), for the ASCII a record holds */
    while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\n' || text[a] == '\r')) a++;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\n' || text[b - 1] == '\r')) b--;
    if (b == a || b - a >= 64) return 0;
    for (size_t i = a; i < b; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    memcpy(out, text + a, b - a);
    out[b - a] = 0;
    return 1;
}

int payload_accepted_floor(const char *saved, const char *minimum, char out[64]) {
    int has_s = saved && *saved && payload_version_cmp(saved, saved) == 0;
    int has_m = minimum && *minimum && payload_version_cmp(minimum, minimum) == 0;
    if (minimum && *minimum && !has_m) return -1;   /* a compiled-in minimum that isn't a version: a build bug */
    const char *f = has_s && has_m ? (payload_version_cmp(saved, minimum) < 0 ? minimum : saved) : has_s ? saved : has_m ? minimum : NULL;
    if (!f || !copy(out, 64, f)) return 0;
    return 1;
}

const char *payload_refusal_text(int why, const char *version, const char *floor, char *out, size_t n) {
    const char *t;
    switch (why) {
    case PAYLOAD_OK: t = "it was accepted"; break;
    case PAYLOAD_NO_SIGNATURE: t = "it has no signature"; break;
    case PAYLOAD_BAD_SIGNATURE: t = "its signature doesn't verify with RB Stems Plus's keys"; break;
    case PAYLOAD_NOT_VALID: t = "it isn't a valid payload description"; break;
    case PAYLOAD_NOT_SAVED: t = "it couldn't be saved"; break;
    case PAYLOAD_TOO_BIG: t = "it is larger than any payload description"; break;
    case PAYLOAD_NO_CRYPTO: t = "Windows' cryptography couldn't be loaded"; break;
    case PAYLOAD_NO_NEWS: t = "it has nothing for Windows"; break;
    case PAYLOAD_NONE: t = "none was saved"; break;
    case PAYLOAD_OLDER:
        snprintf(out, n, "it is payload %s, older than %s, the oldest this app accepts now", version ? version : "?", floor ? floor : "?");
        return out;
    default: t = "?"; break;
    }
    snprintf(out, n, "%s", t);
    return out;
}
