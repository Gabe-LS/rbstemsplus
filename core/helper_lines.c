/* The prepare helper's lines, read as Prepare.swift reads them (pure.h has the rules and the known
 * differences). helper/prepare.c's header comment is the grammar's other half. */
#include "internal.h"
#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ String(decoding:as: UTF8.self)
 * Each maximal ill-formed subsequence becomes one U+FFFD (Unicode's recommended practice, what
 * Swift does). Returns the line itself when it is valid UTF-8. */

static size_t utf8_seq(const unsigned char *p, size_t n, int *ok) {
    unsigned c = p[0];
    *ok = 1;
    if (c < 0x80) return 1;
    unsigned lo = 0x80, hi = 0xBF;
    size_t k;
    if (c >= 0xC2 && c <= 0xDF) k = 1;
    else if (c == 0xE0) { k = 2; lo = 0xA0; }
    else if (c >= 0xE1 && c <= 0xEC) k = 2;
    else if (c == 0xED) { k = 2; hi = 0x9F; }
    else if (c >= 0xEE && c <= 0xEF) k = 2;
    else if (c == 0xF0) { k = 3; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) k = 3;
    else if (c == 0xF4) { k = 3; hi = 0x8F; }
    else { *ok = 0; return 1; }
    size_t i = 1;
    for (; i <= k; i++) {
        if (i >= n || p[i] < lo || p[i] > hi) { *ok = 0; return i; }   /* the subpart so far */
        lo = 0x80;
        hi = 0xBF;
    }
    return i;
}

const char *utf8_repair(arena *a, const char *line, size_t n, size_t *out_n) {
    if (utf8_valid(line, n)) { *out_n = n; return line; }
    if (n > (SIZE_MAX - 1) / 3) return NULL;
    char *o = arena_alloc(a, n * 3 + 1), *w = o;
    if (!o) return NULL;
    const unsigned char *p = (const unsigned char *)line;
    size_t i = 0;
    while (i < n) {
        int ok;
        size_t k = utf8_seq(p + i, n - i, &ok);
        if (ok) memcpy(w, p + i, k), w += k;
        else memcpy(w, "\xEF\xBF\xBD", 3), w += 3;
        i += k;
    }
    *w = 0;
    *out_n = (size_t)(w - o);
    return o;
}

/* ------------------------------------------------------------------ JSONSerialization's reader */

enum { HV_NONE, HV_NULL, HV_BOOL, HV_INT, HV_UINT, HV_DBL, HV_DEC, HV_STR, HV_ARR, HV_OBJ };
#define HJ_MAX_DEPTH 512    /* a value inside more containers than this fails, as JSONSerialization's */

typedef struct {
    uint8_t t;
    int64_t i;          /* HV_INT, HV_BOOL (0/1), HV_DEC (NSDecimalNumber's intValue: the mantissa mod 2^64) */
    uint64_t u;         /* HV_UINT */
    double d;           /* HV_DBL, HV_DEC */
    pstr s;             /* HV_STR; HV_DEC: its stringValue */
    int32_t all_str;    /* HV_ARR: every element a string (as? [String]) */
    int32_t n;          /* HV_ARR: the strings, when all_str */
    pstr *strs;
} hval;

typedef struct { int32_t n, cap; pstr *keys; hval *vals; } hobj;

typedef struct {
    arena *a;
    const unsigned char *p, *end;
    int nomem;
} hj;

static void hj_ws(hj *r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) r->p++;
}

static int hex4(const unsigned char *p, unsigned *v) {
    unsigned x = 0;
    for (int i = 0; i < 4; i++) {
        unsigned c = p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        x = x << 4 | d;
    }
    *v = x;
    return 1;
}

static size_t put_utf8(char *w, unsigned cp) {
    if (cp < 0x80) { w[0] = (char)cp; return 1; }
    if (cp < 0x800) { w[0] = (char)(0xC0 | cp >> 6); w[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { w[0] = (char)(0xE0 | cp >> 12); w[1] = (char)(0x80 | (cp >> 6 & 0x3F)); w[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    w[0] = (char)(0xF0 | cp >> 18); w[1] = (char)(0x80 | (cp >> 12 & 0x3F)); w[2] = (char)(0x80 | (cp >> 6 & 0x3F)); w[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* a string at r->p ('"'); into out when out isn't NULL. 0 on a syntax error. */
static int hj_string(hj *r, pstr *out) {
    const unsigned char *s = ++r->p, *q = s;
    while (q < r->end && *q != '"') {
        if (*q < 0x20) return 0;                         /* a raw control character */
        if (*q == '\\') { if (++q >= r->end) return 0; }
        q++;
    }
    if (q >= r->end) return 0;
    r->p = q + 1;
    char *o = NULL, *w = NULL;
    if (out) {
        o = arena_alloc(r->a, (size_t)(q - s) + 1);   /* escapes never grow: \uXXXX is 6 bytes for at most 3, a pair 12 for 4 */
        if (!o) { r->nomem = 1; return 0; }
        w = o;
    }
    for (const unsigned char *p = s; p < q;) {
        if (*p != '\\') { if (w) *w++ = (char)*p; p++; continue; }
        p++;
        unsigned c = *p++, cp;
        switch (c) {
        case '"': case '\\': case '/': cp = c; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (q - p < 4 || !hex4(p, &cp)) return 0;
            p += 4;
            if (cp >= 0xDC00 && cp <= 0xDFFF) return 0;              /* a low surrogate alone */
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (q - p < 6 || p[0] != '\\' || p[1] != 'u' || !hex4(p + 2, &lo) || lo < 0xDC00 || lo > 0xDFFF) return 0;
                p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            break;
        default: return 0;
        }
        if (w) w += put_utf8(w, cp);
    }
    if (out) {
        *w = 0;
        out->s = o;
        out->n = (size_t)(w - o);
    }
    return 1;
}

/* a double from digits and a decimal exponent, without the locale's decimal point */
static double digits_to_double(const char *digits, size_t nd, long exp10) {
    char buf[64];
    if (nd == 0) return 0;
    if (nd > 40) {   /* only the first digits matter for a double; the rest move the exponent */
        exp10 += (long)(nd - 40);
        nd = 40;
    }
    if (exp10 > 100000) exp10 = 100000;
    if (exp10 < -100000) exp10 = -100000;
    memcpy(buf, digits, nd);
    int k = snprintf(buf + nd, sizeof buf - nd, "e%ld", exp10);
    if (k < 0) return 0;
    return strtod(buf, NULL);
}

/* a number at r->p, classified as JSONSerialization does */
static int hj_number(hj *r, hval *v) {
    const unsigned char *p = r->p, *e = r->end, *start = p;
    int neg = 0;
    if (p < e && *p == '-') { neg = 1; p++; }
    if (p >= e || *p < '0' || *p > '9') return 0;
    const unsigned char *ip = p;
    if (*p == '0') p++;
    else while (p < e && *p >= '0' && *p <= '9') p++;
    if (p < e && *p >= '0' && *p <= '9') return 0;      /* 01 */
    const unsigned char *ie = p, *fp = NULL, *fe = NULL;
    int has_frac = 0, has_exp = 0;
    long ex = 0;
    if (p < e && *p == '.') {
        p++;
        fp = p;
        while (p < e && *p >= '0' && *p <= '9') p++;
        if (p == fp) return 0;
        fe = p;
        has_frac = 1;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        int eneg = 0;
        if (p < e && (*p == '+' || *p == '-')) eneg = *p++ == '-';
        const unsigned char *xp = p;
        while (p < e && *p >= '0' && *p <= '9') {
            if (ex < 1000000) ex = ex * 10 + (*p - '0');
            p++;
        }
        if (p == xp) return 0;
        if (eneg) ex = -ex;
        has_exp = 1;
    }
    r->p = p;
    /* the mantissa's digits, leading zeros dropped */
    char dig[64];
    size_t nd = 0, total = 0;
    int over64 = 0;
    uint64_t m = 0, mod = 0;   /* the mantissa, and the mantissa mod 2^64 */
    long frac_digits = has_frac ? (long)(fe - fp) : 0;
    for (int part = 0; part < 2; part++) {
        const unsigned char *a = part ? fp : ip, *b = part ? fe : ie;
        if (!a) continue;
        for (const unsigned char *q = a; q < b; q++) {
            unsigned d = *q - '0';
            mod = mod * 10 + d;
            if (!nd && !d) continue;
            total++;
            if (nd < sizeof dig) dig[nd++] = (char)*q;
            if (!over64) {
                if (m > (UINT64_MAX - d) / 10) over64 = 1;
                else m = m * 10 + d;
            }
        }
    }
    long exp10 = ex - frac_digits + (long)(total - nd);
    double dv = digits_to_double(dig, nd, exp10);
    if (neg) dv = -dv;
    if (!has_frac && !has_exp && !over64) {
        if (!neg && m <= (uint64_t)INT64_MAX) { v->t = HV_INT; v->i = (int64_t)m; }
        else if (!neg) { v->t = HV_UINT; v->u = m; }
        else if (m <= (uint64_t)INT64_MAX + 1) { v->t = HV_INT; v->i = m == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)m; }
        else over64 = 1;   /* below INT64_MIN: an NSDecimalNumber */
    }
    if (v->t == HV_INT || v->t == HV_UINT) { v->d = dv; return 1; }
    if (isinf(dv)) return 0;                             /* 1e400 */
    if (!over64) { v->t = HV_DBL; v->d = dv; return 1; }
    /* NSDecimalNumber: its literal as stringValue (exact for integers), the mantissa mod 2^64 */
    v->t = HV_DEC;
    v->d = dv;
    uint64_t im = neg ? (uint64_t)0 - mod : mod;
    memcpy(&v->i, &im, sizeof im);
    size_t len = (size_t)(p - start);
    char *s = arena_strdup(r->a, (const char *)start, len);
    if (!s) { r->nomem = 1; return 0; }
    v->s.s = s;
    v->s.n = len;
    return 1;
}

static int hj_lit(hj *r, const char *w, size_t n) {
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, w, n)) return 0;
    r->p += n;
    return 1;
}

/* Skips one value of any kind (validated), `depth` containers around it. Iterative: a stack of the
 * open containers' kinds (0 array, 1 object), at most HJ_MAX_DEPTH + 1 deep. */
static int hj_skip(hj *r, int depth) {
    unsigned char stack[HJ_MAX_DEPTH + 2];
    int sp = 0;
    for (;;) {
        /* a value */
        if (depth + sp > HJ_MAX_DEPTH) return 0;
        hj_ws(r);
        if (r->p >= r->end) return 0;
        unsigned c = *r->p;
        if (c == '[' || c == '{') {
            r->p++;
            stack[sp++] = c == '{';
            hj_ws(r);
            if (r->p < r->end && *r->p == (c == '{' ? '}' : ']')) { r->p++; sp--; goto after; }
            if (c == '{') {
                if (r->p >= r->end || *r->p != '"' || !hj_string(r, NULL)) return 0;
                hj_ws(r);
                if (r->p >= r->end || *r->p++ != ':') return 0;
            }
            continue;
        }
        if (c == '"') { if (!hj_string(r, NULL)) return 0; }
        else if (c == 't') { if (!hj_lit(r, "true", 4)) return 0; }
        else if (c == 'f') { if (!hj_lit(r, "false", 5)) return 0; }
        else if (c == 'n') { if (!hj_lit(r, "null", 4)) return 0; }
        else { hval v = {0}; if (!hj_number(r, &v)) return 0; }
    after:
        /* after a value: close containers, or the next element */
        for (;;) {
            if (!sp) return 1;
            hj_ws(r);
            if (r->p >= r->end) return 0;
            int obj = stack[sp - 1];
            unsigned d = *r->p;
            if (d == (obj ? '}' : ']')) { r->p++; sp--; continue; }
            if (d != ',') return 0;
            r->p++;
            hj_ws(r);
            if (r->p < r->end && *r->p == (obj ? '}' : ']')) { r->p++; sp--; continue; }   /* a trailing comma */
            if (obj) {
                if (r->p >= r->end || *r->p != '"' || !hj_string(r, NULL)) return 0;
                hj_ws(r);
                if (r->p >= r->end || *r->p++ != ':') return 0;
            }
            break;
        }
    }
}

/* a value directly inside the line's object (1 container around it) */
static int hj_value(hj *r, hval *v) {
    hj_ws(r);
    if (r->p >= r->end) return 0;
    unsigned c = *r->p;
    memset(v, 0, sizeof *v);
    if (c == '"') { v->t = HV_STR; return hj_string(r, &v->s); }
    if (c == 't') { v->t = HV_BOOL; v->i = 1; return hj_lit(r, "true", 4); }
    if (c == 'f') { v->t = HV_BOOL; v->i = 0; return hj_lit(r, "false", 5); }
    if (c == 'n') { v->t = HV_NULL; return hj_lit(r, "null", 4); }
    if (c == '{') { v->t = HV_OBJ; return hj_skip(r, 1); }
    if (c != '[') return hj_number(r, v);
    /* an array: its strings kept while every element is one */
    v->t = HV_ARR;
    v->all_str = 1;
    r->p++;
    int32_t cap = 0;
    hj_ws(r);
    if (r->p < r->end && *r->p == ']') { r->p++; return 1; }
    for (;;) {
        hj_ws(r);
        if (r->p >= r->end) return 0;
        if (*r->p == '"' && v->all_str) {
            pstr s;
            if (!hj_string(r, &s)) return 0;
            if (v->n == cap) {
                int32_t nc = cap ? cap * 2 : 8;
                if (nc > (1 << 24)) { r->nomem = 1; return 0; }
                pstr *ns = arena_alloc(r->a, (size_t)nc * sizeof *ns);
                if (!ns) { r->nomem = 1; return 0; }
                if (v->n) memcpy(ns, v->strs, (size_t)v->n * sizeof *ns);
                v->strs = ns;
                cap = nc;
            }
            v->strs[v->n++] = s;
        } else {
            v->all_str = 0;
            if (!hj_skip(r, 2)) return 0;
        }
        hj_ws(r);
        if (r->p >= r->end) return 0;
        if (*r->p == ']') { r->p++; break; }
        if (*r->p++ != ',') return 0;
        hj_ws(r);
        if (r->p < r->end && *r->p == ']') { r->p++; break; }
    }
    if (!v->all_str) { v->n = 0; v->strs = NULL; }
    return 1;
}

/* Prepare.swift jsonLine: the line as a top-level object, or 0 (nil). -1 when out of memory. */
static int hj_line(arena *a, const char *line, size_t n, hobj *o) {
    memset(o, 0, sizeof *o);
    if (!line || !n) return 0;
    size_t rn;
    const char *ln = utf8_repair(a, line, n, &rn);
    if (!ln) return -1;
    if (ln[0] != '{') return 0;
    hj r = {a, (const unsigned char *)ln, (const unsigned char *)ln + rn, 0};
    r.p++;
    hj_ws(&r);
    if (r.p < r.end && *r.p == '}') { r.p++; goto end; }
    for (;;) {
        hj_ws(&r);
        pstr k;
        hval v;
        if (r.p >= r.end || *r.p != '"' || !hj_string(&r, &k)) goto bad;
        hj_ws(&r);
        if (r.p >= r.end || *r.p++ != ':') goto bad;
        if (!hj_value(&r, &v)) goto bad;
        int dup = 0;   /* the first of two equal keys wins */
        for (int32_t i = 0; i < o->n && !dup; i++) dup = o->keys[i].n == k.n && !memcmp(o->keys[i].s, k.s, k.n);
        if (!dup) {
            if (o->n == o->cap) {
                int32_t nc = o->cap ? o->cap * 2 : 16;
                if (nc > (1 << 24)) return -1;
                pstr *nk = arena_alloc(a, (size_t)nc * sizeof *nk);
                hval *nv = arena_alloc(a, (size_t)nc * sizeof *nv);
                if (!nk || !nv) return -1;
                if (o->n) { memcpy(nk, o->keys, (size_t)o->n * sizeof *nk); memcpy(nv, o->vals, (size_t)o->n * sizeof *nv); }
                o->keys = nk;
                o->vals = nv;
                o->cap = nc;
            }
            o->keys[o->n] = k;
            o->vals[o->n++] = v;
        }
        hj_ws(&r);
        if (r.p >= r.end) goto bad;
        if (*r.p == '}') { r.p++; break; }
        if (*r.p++ != ',') goto bad;
        hj_ws(&r);
        if (r.p < r.end && *r.p == '}') { r.p++; break; }   /* a trailing comma */
    }
end:
    hj_ws(&r);
    if (r.p != r.end) goto bad;
    return 1;
bad:
    return r.nomem ? -1 : 0;
}

static const hval *get(const hobj *o, const char *key) {
    size_t n = strlen(key);
    for (int32_t i = 0; i < o->n; i++)
        if (o->keys[i].n == n && !memcmp(o->keys[i].s, key, n)) return &o->vals[i];
    return NULL;
}

static int is_num(const hval *v) { return v && (v->t == HV_BOOL || v->t == HV_INT || v->t == HV_UINT || v->t == HV_DBL || v->t == HV_DEC); }

/* NSNumber's stringValue */
static pstr num_string(arena *a, const hval *v) {
    char b[64];
    pstr out = {"", 0};
    switch (v->t) {
    case HV_BOOL: return v->i ? (pstr){"1", 1} : (pstr){"0", 1};
    case HV_INT: snprintf(b, sizeof b, "%lld", (long long)v->i); break;
    case HV_UINT: snprintf(b, sizeof b, "%llu", (unsigned long long)v->u); break;
    case HV_DBL: snprintf(b, sizeof b, "%.16g", v->d); break;
    case HV_DEC: return v->s;
    default: return out;
    }
    size_t n = strlen(b);
    char *s = arena_strdup(a, b, n);
    if (s) { out.s = s; out.n = n; }
    return out;
}

/* Prepare.swift str(): a string, or a number's stringValue, or "" */
static pstr h_str(arena *a, const hobj *o, const char *k) {
    const hval *v = get(o, k);
    if (v && v->t == HV_STR) return v->s;
    if (is_num(v)) return num_string(a, v);
    return (pstr){"", 0};
}

/* NSNumber's doubleValue (num()) */
static double h_num(const hobj *o, const char *k) {
    const hval *v = get(o, k);
    if (!is_num(v)) return 0;
    if (v->t == HV_BOOL || v->t == HV_INT) return (double)v->i;
    if (v->t == HV_UINT) return (double)v->u;
    return v->d;
}

/* NSNumber's intValue (int()): a double truncated, saturating (as on arm64) */
static int64_t h_int(const hobj *o, const char *k) {
    const hval *v = get(o, k);
    if (!is_num(v)) return 0;
    if (v->t == HV_UINT) { int64_t x; memcpy(&x, &v->u, sizeof x); return x; }
    if (v->t != HV_DBL) return v->i;
    if (v->d >= 9223372036854775807.0) return INT64_MAX;
    if (v->d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)v->d;
}

/* NSNumber's boolValue (bool()) */
static int32_t h_bool(const hobj *o, const char *k) {
    const hval *v = get(o, k);
    if (!is_num(v)) return 0;
    if (v->t == HV_UINT) return v->u != 0;
    if (v->t == HV_DBL || v->t == HV_DEC) return v->d != 0;
    return v->i != 0;
}

static int has(const hobj *o, const char *k) { return get(o, k) != NULL; }

/* as? String */
static int as_string(const hobj *o, const char *k, pstr *out) {
    const hval *v = get(o, k);
    if (!v || v->t != HV_STR) return 0;
    *out = v->s;
    return 1;
}

/* Prepare.swift trackLine */
static void track_line(arena *a, const hobj *o, prep_track *t) {
    t->file = h_str(a, o, "file");
    t->result = h_str(a, o, "result");
    t->why = h_str(a, o, "why");
    t->access = h_str(a, o, "access");
    t->folder = h_str(a, o, "folder");
    t->entries = h_int(o, "entries");
    t->written = h_int(o, "written");
    t->already = h_int(o, "already");
    t->missing = h_int(o, "missing");
    t->refused = h_int(o, "refused");
    t->seconds = h_num(o, "seconds");
    t->gb = h_num(o, "gb");
}

int prep_run_line_parse(arena *a, const char *line, size_t n, prep_run_line *out) {
    hobj o;
    memset(out, 0, sizeof *out);
    int r = hj_line(a, line, n, &o);
    if (r <= 0) return r;
    if (has(&o, "chunk")) {
        out->kind = RUN_PROGRESS;
        out->at = h_int(&o, "at");
        out->files = h_int(&o, "files");
        out->chunk = h_int(&o, "chunk");
        out->chunks = h_int(&o, "chunks");
    } else if (has(&o, "file")) {
        out->kind = RUN_TRACK;
        track_line(a, &o, &out->track);
    } else if (has(&o, "room")) {
        out->kind = RUN_ROOM;
        out->removed = h_int(&o, "room");
        out->cache_gb = h_num(&o, "cache_gb");
    } else if (has(&o, "done")) {
        out->kind = RUN_DONE;
        out->done = h_int(&o, "done");
        out->has_stopped = as_string(&o, "stopped", &out->stopped);
        out->kept_gb = h_num(&o, "kept_gb");
        pstr ke = h_str(a, &o, "kept_error"), me = h_str(a, &o, "manifest_error");
        if (ke.n) { out->has_error = 1; out->error = ke; }
        else if (me.n) { out->has_error = 1; out->error = me; }
    } else return 0;
    return 1;
}

int prep_playlist_parse(arena *a, const char *line, size_t n, prep_playlist *out) {
    hobj o;
    memset(out, 0, sizeof *out);
    int r = hj_line(a, line, n, &o);
    if (r <= 0) return r;
    out->id = h_str(a, &o, "playlist");
    if (!out->id.n) return 0;
    out->parent = h_str(a, &o, "parent");
    out->name = h_str(a, &o, "name");
    out->kind = h_str(a, &o, "kind");
    out->tracks = h_int(&o, "tracks");
    return 1;
}

int prep_track_list_parse(arena *a, const char *line, size_t n, pstr *file) {
    hobj o;
    *file = (pstr){"", 0};
    int r = hj_line(a, line, n, &o);
    if (r <= 0) return r;
    *file = h_str(a, &o, "file");
    return file->n != 0;
}

void prep_estimate_init(prep_estimate *e) {
    memset(e, 0, sizeof *e);
    e->fits = e->disk_ok = 1;
}

int prep_estimate_line(arena *a, prep_estimate *e, const char *line, size_t n) {
    if (e->done) return 0;
    hobj o;
    int r = hj_line(a, line, n, &o);
    if (r <= 0) return r;
    pstr refused;
    if (as_string(&o, "refused", &refused)) {
        e->has_refused = 1;
        e->refused = refused;
        e->done = 1;
        return 0;
    }
    if (has(&o, "file")) {
        if (e->ntracks == e->captracks) {
            int32_t nc = e->captracks ? e->captracks * 2 : 64;
            if (nc > (1 << 24)) return -1;
            prep_track *nt = arena_alloc(a, (size_t)nc * sizeof *nt);
            if (!nt) return -1;
            if (e->ntracks) memcpy(nt, e->tracks, (size_t)e->ntracks * sizeof *nt);
            e->tracks = nt;
            e->captracks = nc;
        }
        track_line(a, &o, &e->tracks[e->ntracks++]);
        return 0;
    }
    if (!has(&o, "estimate")) return 0;
    e->ended = 1;
    e->ready = h_int(&o, "ready");
    e->prepare = h_int(&o, "prepare");
    e->skipped = h_int(&o, "skipped");
    e->unchecked = h_int(&o, "unchecked");
    e->asking = h_int(&o, "asking");
    e->fit_files = h_int(&o, "fit_files");
    e->time_s = h_num(&o, "time_s");
    e->need_gb = h_num(&o, "need_gb");
    e->kept_gb = h_num(&o, "kept_gb");
    e->share_gb = h_num(&o, "share_gb");
    e->max_gb = h_num(&o, "max_gb");
    e->max_gb_needed = h_num(&o, "max_gb_needed");
    e->fits = h_bool(&o, "fits");
    e->disk_ok = h_bool(&o, "disk_ok");
    const hval *f = get(&o, "folders");
    if (f && f->t == HV_ARR && f->all_str) { e->folders = f->strs; e->nfolders = f->n; }
    else { e->folders = NULL; e->nfolders = 0; }
    return 0;
}

int prep_estimate_ok(const prep_estimate *e) { return e->ended || e->has_refused; }

/* NSString.lastPathComponent: trailing slashes dropped ("/" stays "/"), then what follows the last one */
pstr prep_last_path_component(pstr path) {
    const char *s = path.s;
    size_t n = path.n;
    while (n > 1 && s[n - 1] == '/') n--;
    if (n == 1 && s[0] == '/') return (pstr){s, 1};
    size_t i = n;
    while (i > 0 && s[i - 1] != '/') i--;
    return (pstr){s + i, n - i};
}
