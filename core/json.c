/* A small strict JSON reader for the low-rate events and the start config (mac-core §3.3).
 * RFC 8259, with two deliberate limits: "\u0000" is refused (strings are C strings), and a key may
 * appear once per object. Integers that fit int64 are exact (is_int); other numbers are kept as an
 * approximate double, without the locale-dependent strtod. Input up to 64 KiB, nesting up
 * to 16, at most 256 members per container (json_parse_lim: another input size and array limit,
 * objects keep 256, so the duplicate-key check stays bounded). Everything goes into the caller's
 * arena. Pure. */
#include "internal.h"

enum { JSON_MAX = 64 * 1024, JSON_DEPTH = 16, JSON_ITEMS = 256 };

typedef struct {
    const unsigned char *p, *end;
    arena *a;
    int depth;
    size_t max_items;   /* an array's members at most (an object's: JSON_ITEMS) */
    size_t values_left; /* values still allowed in the whole text */
    int max_depth;
} jp;

int utf8_valid(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    while (p < e) {
        unsigned c = *p;
        if (c < 0x80) { p++; continue; }
        size_t k;
        unsigned min, cp;
        if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        if ((size_t)(e - p) <= k) return 0;
        for (size_t i = 1; i <= k; i++) {
            if ((p[i] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        p += k + 1;
    }
    return 1;
}

static void ws(jp *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int lit(jp *j, const char *w) {
    size_t n = strlen(w);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, w, n)) return 0;
    j->p += n;
    return 1;
}

static int hex4(const unsigned char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned c = p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v << 4 | d;
    }
    *out = v;
    return 1;
}

static size_t put_utf8(unsigned char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) { o[0] = (unsigned char)(0xC0 | cp >> 6); o[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (unsigned char)(0xE0 | cp >> 12); o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (unsigned char)(0xF0 | cp >> 18); o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* a string at j->p (the opening quote), unescaped into the arena */
static int str(jp *j, const char **out, size_t *len) {
    const unsigned char *s = ++j->p, *q = s;
    while (q < j->end && *q != '"') {
        if (*q < 0x20) return CORE_E_PARSE;
        if (*q == '\\') { if (++q >= j->end) return CORE_E_PARSE; }
        q++;
    }
    if (q >= j->end) return CORE_E_PARSE;
    /* raw bytes must be UTF-8 (escapes are ASCII, so checking the raw span is enough) */
    if (!utf8_valid((const char *)s, (size_t)(q - s))) return CORE_E_PARSE;
    unsigned char *o = arena_alloc(j->a, (size_t)(q - s) + 1), *w = o;
    if (!o) return CORE_E_NOMEM;
    const unsigned char *p = s;
    while (p < q) {
        if (*p != '\\') { *w++ = *p++; continue; }
        p++;
        switch (*p++) {
        case '"': *w++ = '"'; break;
        case '\\': *w++ = '\\'; break;
        case '/': *w++ = '/'; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (q - p < 4 || !hex4(p, &cp)) return CORE_E_PARSE;
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (q - p < 6 || p[0] != '\\' || p[1] != 'u' || !hex4(p + 2, &lo) || lo < 0xDC00 || lo > 0xDFFF) return CORE_E_PARSE;
                p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) return CORE_E_PARSE;
            if (cp == 0) return CORE_E_PARSE;
            w += put_utf8(w, cp);   /* \uXXXX is 6 bytes, its UTF-8 at most 4: fits in place */
            break;
        }
        default: return CORE_E_PARSE;
        }
    }
    *w = 0;
    *out = (const char *)o;
    *len = (size_t)(w - o);
    j->p = q + 1;
    return CORE_OK;
}

static int num(jp *j, jval *v) {
    const unsigned char *p = j->p;
    int neg = 0;
    if (p < j->end && *p == '-') { neg = 1; p++; }
    if (p >= j->end || *p < '0' || *p > '9') return CORE_E_PARSE;
    if (*p == '0' && p + 1 < j->end && p[1] >= '0' && p[1] <= '9') return CORE_E_PARSE;
    uint64_t m = 0, mant = 0;   /* m: the exact integer; mant: up to 19 significant digits */
    int big = 0, sig = 0, frac = 0;
    long e10 = 0;
    while (p < j->end && *p >= '0' && *p <= '9') {
        unsigned k = *p++ - '0';
        if (m > (UINT64_MAX - k) / 10) big = 1;
        else m = m * 10 + k;
        if (sig < 19) {
            mant = mant * 10 + k;
            sig += mant != 0;
        } else e10++;   /* a dropped integer digit */
    }
    if (p < j->end && *p == '.') {
        frac = 1;
        if (++p >= j->end || *p < '0' || *p > '9') return CORE_E_PARSE;
        while (p < j->end && *p >= '0' && *p <= '9') {
            unsigned k = *p++ - '0';
            if (sig < 19) {
                mant = mant * 10 + k;
                sig += mant != 0;
                e10--;
            }
        }
    }
    if (p < j->end && (*p == 'e' || *p == 'E')) {
        frac = 1;
        int eneg = 0;
        long x = 0;
        if (++p < j->end && (*p == '+' || *p == '-')) eneg = *p++ == '-';
        if (p >= j->end || *p < '0' || *p > '9') return CORE_E_PARSE;
        while (p < j->end && *p >= '0' && *p <= '9') {
            if (x < 100000) x = x * 10 + (*p - '0');
            p++;
        }
        e10 += eneg ? -x : x;
    }
    v->type = J_NUM;
    if (!frac && !big && m <= (uint64_t)INT64_MAX + neg) {
        v->is_int = 1;
        v->i = neg ? (m == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)m) : (int64_t)m;
        v->num = (double)v->i;
    } else {
        /* a fraction, an exponent or past int64 (C14): a bounded, locale-free conversion. A zero
         * mantissa is 0 whatever its exponent; the exponent is clamped to +-400; the scaling uses
         * exact powers of ten, at most 19 steps. Approximate; nothing in the core reads one yet. */
        static const double pow10_exact[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                               1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
        double d = (double)mant;
        if (!mant) e10 = 0;
        if (e10 > 400) e10 = 400;
        if (e10 < -400) e10 = -400;
        int steps = 0;
        while (e10 > 0 && d != 0) { long s = e10 > 22 ? 22 : e10; d *= pow10_exact[s]; e10 -= s; steps++; }
        while (e10 < 0 && d != 0) { long s = -e10 > 22 ? 22 : -e10; d /= pow10_exact[s]; e10 += s; steps++; }
#ifdef CORE_TEST_HOOKS
        v->scale_steps = steps;
#else
        (void)steps;
#endif
        v->num = neg ? -d : d;
    }
    j->p = p;
    return CORE_OK;
}

static int value(jp *j, jval *v);

static int container(jp *j, jval *v, int obj) {
    if (++j->depth > j->max_depth) return CORE_E_PARSE;
    j->p++;
    size_t cap = 4, n = 0;
    jval *items = arena_alloc(j->a, cap * sizeof *items);
    const char **keys = obj ? arena_alloc(j->a, cap * sizeof *keys) : NULL;
    size_t *klens = obj ? arena_alloc(j->a, cap * sizeof *klens) : NULL;
    if (!items || (obj && (!keys || !klens))) return CORE_E_NOMEM;
    ws(j);
    if (j->p < j->end && *j->p == (obj ? '}' : ']')) {
        j->p++;
        goto done;
    }
    for (;;) {
        if (n == (obj ? (size_t)JSON_ITEMS : j->max_items)) return CORE_E_PARSE;
        if (n == cap) {
            size_t nc = cap * 2;
            jval *ni = arena_alloc(j->a, nc * sizeof *ni);
            if (!ni) return CORE_E_NOMEM;
            memcpy(ni, items, n * sizeof *ni);
            items = ni;
            if (obj) {
                const char **nk = arena_alloc(j->a, nc * sizeof *nk);
                size_t *nl = arena_alloc(j->a, nc * sizeof *nl);
                if (!nk || !nl) return CORE_E_NOMEM;
                memcpy(nk, keys, n * sizeof *nk);
                memcpy(nl, klens, n * sizeof *nl);
                keys = nk;
                klens = nl;
            }
            cap = nc;
        }
        ws(j);
        if (obj) {
            if (j->p >= j->end || *j->p != '"') return CORE_E_PARSE;
            int r = str(j, &keys[n], &klens[n]);
            if (r) return r;
            for (size_t k = 0; k < n; k++)
                if (klens[k] == klens[n] && !memcmp(keys[k], keys[n], klens[n])) return CORE_E_PARSE;
            ws(j);
            if (j->p >= j->end || *j->p != ':') return CORE_E_PARSE;
            j->p++;
            ws(j);
        }
        int r = value(j, &items[n]);
        if (r) return r;
        n++;
        ws(j);
        if (j->p >= j->end) return CORE_E_PARSE;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == (obj ? '}' : ']')) { j->p++; break; }
        return CORE_E_PARSE;
    }
done:
    v->type = obj ? J_OBJ : J_ARR;
    v->n = n;
    v->items = items;
    v->keys = keys;
    v->key_lens = klens;
    j->depth--;
    return CORE_OK;
}

static int value(jp *j, jval *v) {
    memset(v, 0, sizeof *v);
    if (j->p >= j->end || !j->values_left) return CORE_E_PARSE;
    j->values_left--;
    switch (*j->p) {
    case '{': return container(j, v, 1);
    case '[': return container(j, v, 0);
    case '"': v->type = J_STR; return str(j, &v->s, &v->len);
    case 't': if (!lit(j, "true")) return CORE_E_PARSE; v->type = J_BOOL; v->b = 1; return CORE_OK;
    case 'f': if (!lit(j, "false")) return CORE_E_PARSE; v->type = J_BOOL; return CORE_OK;
    case 'n': if (!lit(j, "null")) return CORE_E_PARSE; v->type = J_NULL; return CORE_OK;
    default: return num(j, v);
    }
}

int json_parse(arena *a, const char *p, size_t n, jval *out) {
    static const json_lim ev = {JSON_MAX, JSON_ITEMS, SIZE_MAX, JSON_DEPTH};
    return json_parse_lim(a, p, n, &ev, out);
}

int json_parse_lim(arena *a, const char *p, size_t n, const json_lim *lim, jval *out) {
    if (!p || n > lim->bytes) return CORE_E_PARSE;
    jp j = {(const unsigned char *)p, (const unsigned char *)p + n, a, 0, lim->arr_items, lim->values, lim->depth};
    ws(&j);
    int r = value(&j, out);
    if (r) return r;
    ws(&j);
    return j.p == j.end ? CORE_OK : CORE_E_PARSE;
}

const jval *json_get(const jval *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    size_t n = strlen(key);
    for (size_t i = 0; i < obj->n; i++)
        if (obj->key_lens[i] == n && !memcmp(obj->keys[i], key, n)) return &obj->items[i];
    return NULL;
}
