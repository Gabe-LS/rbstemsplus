/* Masking for Create Report (Report.swift redact, maskSecrets, looksRandom): core_mask.
 *
 * redact runs NSRegularExpression (ICU) patterns over the text, in order, each over the whole
 * result of the one before; then names. The core links no regex engine, so each pattern is a
 * small matcher here, written to find what ICU's backtracking finds: the leftmost match, with
 * the greedy runs and the one possible split each pattern has (the comments say why each is
 * unique). The Unicode it needs is in mask_tables.h, generated from this Mac's ICU
 * (core/tools/gen_mask_tables.c, about 13 KB):
 *
 * - \b is ICU's (rematch.cpp isWordBoundary): \w is Alphabetic, Mark, Decimal_Number,
 *   Connector_Punctuation, U+200C and U+200D; a position before a Grapheme_Extend or Cf code point
 *   is never a boundary, and the code point before a position is found by looking back past those.
 * - A case-insensitive literal (each keyword, and a name) is compared after full case folding on
 *   both sides (URX_STRING_I): "paßwd" holds "passwd", "Anna Straße" matches "ANNA STRASSE", and a
 *   literal never ends inside one code point's fold. A class ([A-Za-z] and the like) is ICU's case
 *   closure: an ASCII letter and its other simple case folds, U+017F (ſ, for s) and U+212A (K,
 *   for k). The generator checks both (and \s) against ICU.
 * - The home folder is found as NSString finds it, where macOS's home is ASCII: a code point
 *   canonically equivalent to one ASCII character (U+212A for K, U+037E for ";", U+1FEF for "`")
 *   stands for it. A match can't end where the next code point joins the last one (a
 *   Grapheme_Extend but U+F870-F89F, or an emoji modifier): NSString leaves "/Users/dj" + U+0301
 *   alone, and so does the core (masking it would hide the text from the later passes). A
 *   non-ASCII home is matched code point for code point, with no canonical equivalence (no Mac
 *   shell gives one; no NFD tables here).
 * - A name is masked when Swift's name.count would be 2 or more: here, code points that can start
 *   a character (not a Grapheme_Extend, U+F870-F89F aside, or U+200D after anything but a
 *   control), an upper bound of Swift's count (masking more, never less).
 *
 * Invalid UTF-8 is kept as it is, each such byte a non-word character no pattern matches.
 *
 * Linear time: every failed start says where the next one worth trying is (a run's end), so a
 * long run is scanned once per pattern. */
#include "internal.h"
#include "mask_tables.h"

#define CP_BAD 0xFFFFFFFFu    /* a byte that isn't UTF-8 */
#define CP_EDGE 0xFFFFFFFEu   /* before the start, after the end */

/* ------------------------------------------------------------------ code points over bytes */

static uint32_t cp_at(const unsigned char *s, size_t n, size_t i, size_t *len) {
    if (i >= n) { *len = 0; return CP_EDGE; }
    unsigned c = s[i];
    if (c < 0x80) { *len = 1; return c; }
    size_t k;
    uint32_t cp, min;
    if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; min = 0x10000; }
    else { *len = 1; return CP_BAD; }
    if (n - i <= k) { *len = 1; return CP_BAD; }
    for (size_t j = 1; j <= k; j++) {
        if ((s[i + j] & 0xC0) != 0x80) { *len = 1; return CP_BAD; }
        cp = cp << 6 | (s[i + j] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { *len = 1; return CP_BAD; }
    *len = k + 1;
    return cp;
}

/* the code point that ends at i, and its length (1 for a byte that isn't UTF-8) */
static uint32_t cp_back(const unsigned char *s, size_t n, size_t i, size_t *len) {
    *len = 0;
    if (i == 0) return CP_EDGE;
    *len = 1;
    for (size_t back = 1; back <= 4 && back <= i; back++) {
        if ((s[i - back] & 0xC0) == 0x80 && back < 4) continue;
        size_t l;
        uint32_t cp = cp_at(s, n, i - back, &l);
        if (l != back) return CP_BAD;
        *len = back;
        return cp;
    }
    return CP_BAD;
}

/* ------------------------------------------------------------------ Unicode (mask_tables.h) */

/* in an inversion list: an odd number of its entries at or below c */
static int inv_has(const uint32_t *v, size_t nv, uint32_t c) {
    size_t lo = 0, hi = nv;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v[mid] <= c) lo = mid + 1; else hi = mid;
    }
    return (int)(lo & 1);
}
#define INV_HAS(t, c) inv_has((t), sizeof (t) / sizeof *(t), (c))

static int ascii_alpha(uint32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int ascii_digit(uint32_t c) { return c >= '0' && c <= '9'; }
/* [A-Za-z] under CASE_INSENSITIVE: the letters and their two other case folds */
static int ci_alpha(uint32_t c) { return ascii_alpha(c) || c == 0x17F || c == 0x212A; }
static int ci_alnum(uint32_t c) { return ci_alpha(c) || ascii_digit(c); }
/* c in a case-insensitive class holding the ASCII character p (a lower case letter or not a letter) */
static int ci_eq(uint32_t c, char p) {
    if (p >= 'a' && p <= 'z') {
        if (c == (uint32_t)p || c == (uint32_t)(p - 32)) return 1;
        return (p == 's' && c == 0x17F) || (p == 'k' && c == 0x212A);
    }
    return c == (uint32_t)(unsigned char)p;
}

/* ICU's \w */
static int is_word(uint32_t c) {
    if (c == CP_EDGE || c == CP_BAD) return 0;
    if (c < 0x80) return ascii_alpha(c) || ascii_digit(c) || c == '_';
    if (c < 0x10000 && (mt_page[c >> 8] & (MT_ALL_WORD | MT_NO_WORD))) return mt_page[c >> 8] & MT_ALL_WORD;
    return INV_HAS(mt_word, c);
}
static int page_no_skip(uint32_t c) { return c < 0x80 || (c < 0x10000 && (mt_page[c >> 8] & MT_NO_SKIP)) || c >= 0x110000; }
static int is_ext(uint32_t c) { return !page_no_skip(c) && INV_HAS(mt_ext, c); }
static int is_cf(uint32_t c) { return !page_no_skip(c) && INV_HAS(mt_cf, c); }
/* Grapheme_Extend in Apple's ICU, but its own character to Swift and NSString: U+F870-F89F */
static int joins(uint32_t c) { return is_ext(c) && !(c >= 0xF870 && c <= 0xF89F); }
/* what ICU's \b looks through */
static int is_skip(uint32_t c) { return is_ext(c) || is_cf(c); }

/* ICU's \s: Unicode's White_Space (the generator checks it) */
static int is_ws(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A)
        || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* ICU's \b (isWordBoundary): never before a Grapheme_Extend or Cf; the code point before is the
 * first one back that is neither. Looking back is linear overall: a run of those is looked
 * through only from the one position after it that isn't in the run. */
static int boundary(const unsigned char *s, size_t n, size_t i) {
    size_t len;
    uint32_t c = cp_at(s, n, i, &len);
    if (is_skip(c)) return 0;
    uint32_t p;
    while ((p = cp_back(s, n, i, &len)) != CP_EDGE && is_skip(p)) i -= len;
    return is_word(c) != is_word(p);
}

/* full case folding (ICU's u_strFoldCase, U_FOLD_CASE_DEFAULT): 1 to 3 code points in out */
static int fold_cp(uint32_t c, uint32_t out[3]) {
    out[0] = c;
    if (c < 0x80) {
        if (c >= 'A' && c <= 'Z') out[0] = c + 32;
        return 1;
    }
    if (c >= 0x110000 || (c < 0x10000 && (mt_page[c >> 8] & MT_NO_FOLD))) return 1;
    size_t lo = 0, hi = sizeof mt_foldn / sizeof *mt_foldn;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (mt_foldn[mid].cp < c) lo = mid + 1; else hi = mid;
    }
    if (lo < sizeof mt_foldn / sizeof *mt_foldn && mt_foldn[lo].cp == c) {
        out[0] = mt_foldn[lo].to[0];
        out[1] = mt_foldn[lo].to[1];
        out[2] = mt_foldn[lo].to[2];
        return out[2] ? 3 : 2;
    }
    lo = 0, hi = sizeof mt_fold1 / sizeof *mt_fold1;   /* the last run starting at or before c */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (mt_fold1[mid].lo <= c) lo = mid + 1; else hi = mid;
    }
    if (lo) {
        const mt_fold_run *r = &mt_fold1[lo - 1];
        uint32_t d = c - r->lo;
        if (d % r->step == 0 && d / r->step < r->n) out[0] = (uint32_t)((int32_t)c + r->delta);
    }
    return 1;
}

/* the first code point of c's fold */
static uint32_t fold_first(uint32_t c) {
    uint32_t f[3];
    fold_cp(c, f);
    return f[0];
}

/* a cheap first test before \b: the fold of the code point at i starts with one of lit's first
 * letters (firsts, lower case) */
static int first_is(const unsigned char *s, size_t n, size_t i, const char *firsts) {
    size_t len;
    uint32_t f = fold_first(cp_at(s, n, i, &len));
    return f && f < 0x80 && strchr(firsts, (int)f);
}

/* the code point NSString's search takes c for (an ASCII one canonically equivalent), or c */
static uint32_t canon_ascii(uint32_t c) {
    for (size_t k = 0; k < sizeof mt_canon_ascii / sizeof *mt_canon_ascii; k++)
        if (mt_canon_ascii[k][0] == c) return mt_canon_ascii[k][1];
    return c;
}

/* A case-insensitive literal at i (URX_STRING_I): lit is ASCII, lower case (already folded), and
 * each code point of the text is folded in full; the literal can't end inside one's fold. '?' in
 * lit is an optional "_" or "-" ([_-]?), between two literals. Its end, or 0. */
static size_t lit_at(const unsigned char *s, size_t n, size_t i, size_t end, const char *lit) {
    const char *p = lit;
    while (*p) {
        size_t len;
        uint32_t c = cp_at(s, n, i, &len);
        if (*p == '?') {
            if (i < end && (c == '_' || c == '-')) i += len;
            p++;
            continue;
        }
        if (i >= end || c == CP_BAD) return 0;
        uint32_t f[3];
        int k = fold_cp(c, f);
        for (int j = 0; j < k; j++, p++)
            if (!*p || *p == '?' || f[j] != (uint32_t)(unsigned char)*p) return 0;
        i += len;
    }
    return i;
}

/* the end of the run from i of code points in class f */
static size_t run_end(const unsigned char *s, size_t n, size_t i, int (*f)(uint32_t)) {
    size_t len;
    while (i < n && f(cp_at(s, n, i, &len))) i += len;
    return i;
}

static size_t next_cp(const unsigned char *s, size_t n, size_t i) {
    size_t len;
    cp_at(s, n, i, &len);
    return i + (len ? len : 1);
}

/* any of the keywords lying wholly in [a, e) */
static int keyword_in(const unsigned char *s, size_t n, size_t a, size_t e, const char *const *kw, int nkw) {
    for (size_t q = a; q < e; q = next_cp(s, n, q)) {
        size_t len;
        uint32_t f[3];
        fold_cp(cp_at(s, n, q, &len), f);
        for (int k = 0; k < nkw; k++)
            if (f[0] == (uint32_t)(unsigned char)kw[k][0] && lit_at(s, n, q, e, kw[k])) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ the classes of the patterns */

static int c_pem(uint32_t c) { return ci_alpha(c) || c == ' '; }                                        /* [A-Z ] */
static int c_local(uint32_t c) { return ci_alnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-'; }
static int c_domain(uint32_t c) { return ci_alnum(c) || c == '-'; }
static int c_word_dash(uint32_t c) { return ci_alnum(c) || c == '_' || c == '-'; }                     /* [A-Za-z0-9_-] */
static int c_query_key(uint32_t c) { return !(c == '=' || c == '&' || is_ws(c) || c == '#' || c == '"' || c == '\'' || c == '<' || c == '>' || c == CP_EDGE); }
static int c_query_value(uint32_t c) { return !(c == '&' || is_ws(c) || c == '#' || c == '"' || c == '\'' || c == '<' || c == '>' || c == CP_EDGE); }
static int c_bearer(uint32_t c) { return ci_alnum(c) || c == '.' || c == '_' || c == '~' || c == '+' || c == '/' || c == '=' || c == '-'; }
static int c_value(uint32_t c) { return !(is_ws(c) || c == '"' || c == '\'' || c == ',' || c == ';' || c == '&' || c == CP_EDGE); }
static int c_ws(uint32_t c) { return is_ws(c); }
static int c_aws(uint32_t c) { return ci_alnum(c); }
static int c_gh_pat(uint32_t c) { return ci_alnum(c) || c == '_'; }
static int c_slack(uint32_t c) { return ci_alnum(c) || c == '-'; }
/* longRun's, not case-insensitive */
static int c_run(uint32_t c) { return ascii_alpha(c) || ascii_digit(c) || c == '_' || c == '+' || c == '-'; }
static int c_run_eq(uint32_t c) { return c_run(c) || c == '='; }

/* A matcher: the match's end (> i) or 0; *skip the next start worth trying (> i) on a failure;
 * *g1 the end of the pattern's group 1 where its template uses it. */
typedef size_t (*matcher)(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1);

/* "-----BEGIN [A-Z ]*PRIVATE KEY-----": [A-Z ]* is greedy, and "PRIVATE KEY" is in its class
 * while "-" isn't, so the one split puts "PRIVATE KEY" at the end of the class's run */
static size_t pem_marker(const unsigned char *s, size_t n, size_t i, const char *head) {
    size_t p = lit_at(s, n, i, n, head);
    if (!p) return 0;
    size_t e = run_end(s, n, p, c_pem);
    /* the last 11 code points of [p, e) must be "private key" (the run holds only whole code points) */
    size_t q = e;
    for (int k = 0; k < 11; k++) {
        if (q <= p) return 0;
        do q--; while (q > p && (s[q] & 0xC0) == 0x80);
    }
    if (lit_at(s, n, q, e, "private key") != e) return 0;
    return lit_at(s, n, e, n, "-----");
}

static size_t m_pem(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    (void)g1;
    *skip = next_cp(s, n, i);
    size_t h = pem_marker(s, n, i, "-----begin ");
    if (!h) return 0;
    for (size_t q = h; q < n; q = next_cp(s, n, q)) {   /* [\s\S]*? then the end marker, or the end */
        size_t e = pem_marker(s, n, q, "-----end ");
        if (e) return e;
    }
    return n;
}

/* an email: the local part's run must end at "@"; then the labels, and the last one (after the
 * first) starting with two letters ends the match (the (\.label)* greedy loop gives back whole
 * labels only: a shorter label is followed by a label character, never "."). */
static size_t m_email(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    (void)g1;
    size_t a = run_end(s, n, i, c_local);
    *skip = a > i ? a : next_cp(s, n, i);
    if (a == i || a >= n || s[a] != '@') return 0;
    size_t d = run_end(s, n, a + 1, c_domain);
    if (d == a + 1) return 0;
    size_t best = 0;
    while (d < n && s[d] == '.') {
        size_t e = run_end(s, n, d + 1, c_domain);
        if (e == d + 1) break;
        size_t l = run_end(s, n, d + 1, ci_alpha);
        size_t len, k = d + 1, letters = 0;
        while (k < l && letters < 2) { cp_at(s, n, k, &len); k += len; letters++; }
        if (letters >= 2) best = l;
        d = e;
    }
    return best;
}

static const char *const query_kw[] = {"token", "key", "auth", "session", "sig", "secret", "passw", "pwd", "credential", "code"};

/* ([?&;][^=&\s#"'<>]*(token|...)[^=&\s#"'<>]*=)[^&\s#"'<>]+ : the name's run can't hold "=", so it
 * ends at the "=" or the match fails; the keyword anywhere in it */
static size_t m_query(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    *skip = next_cp(s, n, i);
    if (s[i] != '?' && s[i] != '&' && s[i] != ';') return 0;
    size_t r = run_end(s, n, i + 1, c_query_key);
    if (r > *skip) *skip = r;
    if (r >= n || s[r] != '=') return 0;
    if (!keyword_in(s, n, i + 1, r, query_kw, (int)(sizeof query_kw / sizeof *query_kw))) return 0;
    size_t v = run_end(s, n, r + 1, c_query_value);
    if (v == r + 1) return 0;
    *g1 = r + 1;
    return v;
}

/* \b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]{8,} */
static size_t m_bearer(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    *skip = next_cp(s, n, i);
    if (!first_is(s, n, i, "b") || !boundary(s, n, i)) return 0;
    static const char *const kw[] = {"bearer", "basic"};
    for (int k = 0; k < 2; k++) {
        size_t p = lit_at(s, n, i, n, kw[k]);
        if (!p) continue;
        size_t w = run_end(s, n, p, c_ws);
        if (w == p) continue;
        size_t e = run_end(s, n, w, c_bearer), cps = 0, len;
        for (size_t q = w; q < e && cps < 8; q += len) { cp_at(s, n, q, &len); cps++; }
        if (cps < 8) continue;
        *g1 = p;
        return e;
    }
    return 0;
}

static const char *const secret_kw[] = {"password", "passwd", "pwd", "secret", "token", "api?key", "access?key", "private?key",
                                        "session?id", "sessionid", "cookie", "authorization"};

/* m_secret after the name's second run (from e): "?\s*[:=]\s*"?(?!<token>|Bearer\b|Basic\b)[^\s"',;&]+,
 * each part deterministic (a shorter \s* or no quote leaves a character the value can't start
 * with); the match's end, or 0, and *g1 the value's start */
static size_t secret_tail(const unsigned char *s, size_t n, size_t e, size_t *g1) {
    size_t p = e;
    if (p < n && s[p] == '"') p++;
    p = run_end(s, n, p, c_ws);
    if (p >= n || (s[p] != ':' && s[p] != '=')) return 0;
    p = run_end(s, n, p + 1, c_ws);
    if (p < n && s[p] == '"') p++;
    /* (?!<token>|Bearer\b|Basic\b) */
    if (lit_at(s, n, p, n, "<token>")) return 0;
    size_t b = lit_at(s, n, p, n, "bearer");
    if (b && boundary(s, n, b)) return 0;
    b = lit_at(s, n, p, n, "basic");
    if (b && boundary(s, n, b)) return 0;
    size_t v = run_end(s, n, p, c_value);
    if (v == p) return 0;
    *g1 = p;
    return v;
}

/* \b([A-Za-z0-9_-]*(password|...)[A-Za-z0-9_-]*"?\s*[:=]\s*"?)(?!<token>|Bearer\b|Basic\b)[^\s"',;&]+
 * From a start i (a \b), the first run reaches r1 (the run's end); ICU tries the keyword at
 * every j from r1 back to i, each keyword in order. A keyword can hold a code point outside the
 * class ("paßwd" for "passwd"), so it may end past r1. The second run is greedy and what follows
 * it isn't in its class, so it ends where its run ends (e2: r1 itself for a keyword inside the
 * first run, found without rescanning it, so a run packed with keywords stays linear), and the
 * rest depends on e2 alone:
 * the first (j, keyword) in that order whose tail matches is ICU's match. A later start in
 * (i, r1] tries a subset of the same (j, keyword) pairs, so after a failure the next start worth
 * trying is r1. */
static size_t m_secret(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    enum { NKW = sizeof secret_kw / sizeof *secret_kw };
    size_t len;
    uint32_t c = cp_at(s, n, i, &len);
    *skip = i + len;
    if (!c_word_dash(c)) {   /* then only a keyword can start here */
        uint32_t f = fold_first(c);
        int any = 0;
        for (int k = 0; k < NKW && !any; k++) any = f == (uint32_t)(unsigned char)secret_kw[k][0];
        if (!any) return 0;
    }
    if (!boundary(s, n, i)) return 0;
    size_t r1 = run_end(s, n, i, c_word_dash);
    if (r1 > *skip) *skip = r1;
    size_t tail_r1 = 0, g1_r1 = 0;
    int tried_r1 = 0;
    for (size_t j = r1;;) {
        uint32_t f = fold_first(cp_at(s, n, j, &len));
        for (int k = 0; k < NKW; k++) {
            if (f != (uint32_t)(unsigned char)secret_kw[k][0]) continue;
            size_t e = lit_at(s, n, j, n, secret_kw[k]);
            if (!e) continue;
            size_t e2 = e <= r1 ? r1 : run_end(s, n, e, c_word_dash), g = 0, v;
            if (e2 == r1) {
                if (!tried_r1) { tail_r1 = secret_tail(s, n, r1, &g1_r1); tried_r1 = 1; }
                v = tail_r1;
                g = g1_r1;
            } else v = secret_tail(s, n, e2, &g);
            if (v) { *g1 = g; return v; }
        }
        if (j == i) break;
        do j--; while (j > i && (s[j] & 0xC0) == 0x80);   /* the run is UTF-8: back one code point */
    }
    return 0;
}

static size_t count_cps(const unsigned char *s, size_t n, size_t a, size_t e, size_t enough) {
    size_t k = 0, len;
    for (size_t q = a; q < e && k < enough; q += len) { cp_at(s, n, q, &len); k++; }
    return k;
}

/* \beyJ[A-Za-z0-9_-]{4,}\.[A-Za-z0-9_-]{4,}\.[A-Za-z0-9_-]* (each run greedy, then a "." it can't hold) */
static size_t m_jwt(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    (void)g1;
    *skip = next_cp(s, n, i);
    if (!first_is(s, n, i, "e") || !boundary(s, n, i)) return 0;
    size_t p = lit_at(s, n, i, n, "eyj");
    if (!p) return 0;
    for (int part = 0; part < 2; part++) {
        size_t e = run_end(s, n, p, c_word_dash);
        if (count_cps(s, n, p, e, 4) < 4 || e >= n || s[e] != '.') return 0;
        p = e + 1;
    }
    return run_end(s, n, p, c_word_dash);
}

/* \b(AKIA|ASIA)[A-Z0-9]{16}\b */
static size_t m_aws(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    (void)g1;
    *skip = next_cp(s, n, i);
    if (!first_is(s, n, i, "a") || !boundary(s, n, i)) return 0;
    size_t p = lit_at(s, n, i, n, "akia");
    if (!p) p = lit_at(s, n, i, n, "asia");
    if (!p) return 0;
    size_t len;
    for (int k = 0; k < 16; k++) {
        if (!c_aws(cp_at(s, n, p, &len))) return 0;
        p += len;
    }
    return boundary(s, n, p) ? p : 0;
}

/* \b(gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})\b: a shorter run ends between two
 * word characters, so only the whole run can meet the \b */
static size_t m_github(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    (void)g1;
    *skip = next_cp(s, n, i);
    if (!first_is(s, n, i, "g") || !boundary(s, n, i)) return 0;
    size_t p = lit_at(s, n, i, n, "gh");
    if (p) {
        size_t len;
        uint32_t c = cp_at(s, n, p, &len);
        if (ci_eq(c, 'p') || ci_eq(c, 'o') || ci_eq(c, 'u') || ci_eq(c, 's') || ci_eq(c, 'r')) {
            size_t q = p + len;
            if (q < n && s[q] == '_') {
                size_t e = run_end(s, n, q + 1, c_aws);
                if (count_cps(s, n, q + 1, e, 30) >= 30 && boundary(s, n, e)) return e;
            }
        }
    }
    p = lit_at(s, n, i, n, "github_pat_");
    if (p) {
        size_t e = run_end(s, n, p, c_gh_pat);
        if (count_cps(s, n, p, e, 30) >= 30 && boundary(s, n, e)) return e;
    }
    return 0;
}

/* \bxox[abposr]-[A-Za-z0-9-]{10,} */
static size_t m_slack(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    (void)g1;
    *skip = next_cp(s, n, i);
    if (!first_is(s, n, i, "x") || !boundary(s, n, i)) return 0;
    size_t p = lit_at(s, n, i, n, "xox");
    if (!p) return 0;
    size_t len;
    uint32_t c = cp_at(s, n, p, &len);
    if (!(ci_eq(c, 'a') || ci_eq(c, 'b') || ci_eq(c, 'p') || ci_eq(c, 'o') || ci_eq(c, 's') || ci_eq(c, 'r'))) return 0;
    p += len;
    if (p >= n || s[p] != '-') return 0;
    size_t e = run_end(s, n, p + 1, c_slack);
    return count_cps(s, n, p + 1, e, 10) >= 10 ? e : 0;
}

/* \bsk-[A-Za-z0-9_-]{20,} */
static size_t m_sk(const unsigned char *s, size_t n, size_t i, size_t *skip, size_t *g1) {
    (void)g1;
    *skip = next_cp(s, n, i);
    if (!first_is(s, n, i, "s") || !boundary(s, n, i)) return 0;
    size_t p = lit_at(s, n, i, n, "sk-");
    if (!p) return 0;
    size_t e = run_end(s, n, p, c_word_dash);
    return count_cps(s, n, p, e, 20) >= 20 ? e : 0;
}

/* ------------------------------------------------------------------ passes */

typedef struct { arena *a; char *p; size_t len, cap; int nomem; } sbuf;

static void sb_put(sbuf *b, const void *s, size_t n) {
    if (b->nomem || !n) return;
    if (n > b->cap - b->len) {
        size_t need = b->len + n, cap = b->cap ? b->cap : 256;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) { b->nomem = 1; return; }
            cap *= 2;
        }
        char *np = arena_alloc(b->a, cap);
        if (!np) { b->nomem = 1; return; }
        if (b->len) memcpy(np, b->p, b->len);
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

enum { REPL_WHOLE, REPL_G1 };

static int pass(arena *a, const char *in, size_t n, matcher m, const char *repl, int mode, sbuf *out) {
    const unsigned char *s = (const unsigned char *)in;
    *out = (sbuf){a, NULL, 0, 0, 0};
    size_t last = 0, i = 0;
    while (i < n) {
        size_t skip = 0, g1 = i;
        size_t e = m(s, n, i, &skip, &g1);
        if (e > i) {
            sb_put(out, s + last, i - last);
            if (mode == REPL_G1) sb_put(out, s + i, g1 - i);
            sb_put(out, repl, strlen(repl));
            last = i = e;
            continue;
        }
        i = skip > i ? skip : i + 1;
    }
    sb_put(out, s + last, n - last);
    return out->nomem ? -1 : 0;
}

/* looksRandom: hex with a digit and a letter (unless kept), or upper, lower and digits with no
 * word in it (8 lower case letters in a row) */
static int looks_random(const unsigned char *r, size_t n, const core_mask_ctx *mc) {
    int hex = 1, digit = 0, letter = 0, upper = 0, lower = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned c = r[i];
        int h = ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        hex &= h;
        digit |= ascii_digit(c);
        letter |= ascii_alpha(c);
        upper |= c >= 'A' && c <= 'Z';
        lower |= c >= 'a' && c <= 'z';
    }
    if (hex) {
        if (!digit || !letter) return 0;
        for (int32_t k = 0; k < mc->n_allowed; k++) {
            const char *kept = mc->allowed[k];
            if (!kept || strlen(kept) != n) continue;
            size_t j = 0;
            while (j < n && (unsigned char)kept[j] == ((r[j] >= 'A' && r[j] <= 'Z') ? r[j] + 32 : r[j])) j++;
            if (j == n) return 0;
        }
        return 1;
    }
    if (!digit || !upper || !lower) return 0;
    int run = 0;
    for (size_t i = 0; i < n; i++) {
        run = r[i] >= 'a' && r[i] <= 'z' ? run + 1 : 0;
        if (run >= 8) return 0;
    }
    return 1;
}

/* (?<![A-Za-z0-9_+=-])[A-Za-z0-9_+-]{32,}={0,2}(?![A-Za-z0-9_+=-]), kept unless it looks random */
static int long_runs(arena *a, const char *in, size_t n, const core_mask_ctx *mc, sbuf *out) {
    const unsigned char *s = (const unsigned char *)in;
    *out = (sbuf){a, NULL, 0, 0, 0};
    size_t last = 0, i = 0;
    while (i < n) {
        if (!c_run(s[i]) || (i > 0 && c_run_eq(s[i - 1]))) { i++; continue; }
        size_t r = i;
        while (r < n && c_run(s[r])) r++;
        size_t k = 0;
        while (r + k < n && s[r + k] == '=' && k < 3) k++;
        int ok = r - i >= 32 && k <= 2 && !(r + k < n && c_run_eq(s[r + k]));
        if (ok && looks_random(s + i, r + k - i, mc)) {
            sb_put(out, s + last, i - last);
            sb_put(out, "<token>", 7);
            last = r + k;
        }
        i = r;
    }
    sb_put(out, s + last, n - last);
    return out->nomem ? -1 : 0;
}

/* ------------------------------------------------------------------ names: \bname\b, case-insensitive */

/* Swift's name.count >= 2, from above: a code point starts a character unless it joins (a
 * Grapheme_Extend but U+F870-F89F) or is U+200D (GB9), after anything but a control, CR or LF
 * (GB4; Cc, Zl, Zp, Cf and the unassigned default ignorables all count as one here, which only
 * counts more). Swift joins more (spacing marks, Hangul, emoji sequences, CR LF): this count is
 * never less than Swift's, so a name Swift masks is masked. -1 for a name that isn't UTF-8 (never
 * matched). */
static int name_counts(const unsigned char *s, size_t n) {
    size_t chars = 0, len;
    uint32_t prev = CP_EDGE;
    for (size_t q = 0; q < n; q += len) {
        uint32_t c = cp_at(s, n, q, &len);
        if (c == CP_BAD) return -1;
        int control = prev < 0x20 || (prev >= 0x7F && prev <= 0x9F) || prev == 0x2028 || prev == 0x2029
            || (is_cf(prev) && prev != 0x200C && prev != 0x200D) || prev == 0x2065 || (prev >= 0xFFF0 && prev <= 0xFFF8)
            || prev >= 0xE0000;
        if (prev == CP_EDGE || control || !(joins(c) || c == 0x200D)) chars++;
        prev = c;
    }
    return chars >= 2;
}

/* a name, folded in full (ICU folds a pattern's literal so) */
typedef struct { const uint32_t *f; size_t nf; } name_m;

static int name_fold(arena *a, const char *name, name_m *nm) {
    const unsigned char *s = (const unsigned char *)name;
    size_t n = strlen(name), len, k = 0;
    uint32_t *f = arena_alloc(a, (n * 3 + 1) * sizeof *f);
    if (!f) return -1;
    for (size_t q = 0; q < n; q += len) k += (size_t)fold_cp(cp_at(s, n, q, &len), f + k);
    *nm = (name_m){f, k};
    return 0;
}

/* the name at i (each code point of the text folded in full, the name not ending inside one's
 * fold): its end, or 0 */
static size_t name_at(const unsigned char *s, size_t n, size_t i, const name_m *nm) {
    size_t p = i, q = 0, len;
    while (q < nm->nf) {
        uint32_t c = cp_at(s, n, p, &len), f[3];
        if (c == CP_EDGE || c == CP_BAD) return 0;
        int k = fold_cp(c, f);
        for (int j = 0; j < k; j++, q++)
            if (q >= nm->nf || f[j] != nm->f[q]) return 0;
        p += len;
    }
    return p;
}

static int names_pass(arena *a, const char *in, size_t n, const name_m *nm, sbuf *out) {
    const unsigned char *s = (const unsigned char *)in;
    *out = (sbuf){a, NULL, 0, 0, 0};
    size_t last = 0, i = 0;
    while (i < n) {
        size_t e;
        if ((e = name_at(s, n, i, nm)) && boundary(s, n, i) && boundary(s, n, e)) {
            sb_put(out, s + last, i - last);
            sb_put(out, "<user>", 6);
            last = i = e;
            continue;
        }
        i = next_cp(s, n, i);
    }
    sb_put(out, s + last, n - last);
    return out->nomem ? -1 : 0;
}

/* ------------------------------------------------------------------ redact */

/* NSString's search for the home folder (no options): a code point canonically equivalent to one
 * ASCII character stands for it, on either side (the header says what isn't covered), and the
 * match doesn't end before a code point that joins its last one. A home that isn't UTF-8 is
 * found byte for byte. */
static size_t home_at(const unsigned char *s, size_t n, size_t i, const unsigned char *h, size_t hn) {
    size_t p = i, q = 0, l1, l2;
    while (q < hn) {
        uint32_t a = cp_at(s, n, p, &l1), b = cp_at(h, hn, q, &l2);
        if (a == CP_EDGE || a == CP_BAD || canon_ascii(a) != canon_ascii(b)) return 0;
        p += l1;
        q += l2;
    }
    size_t l;
    uint32_t nx = cp_at(s, n, p, &l);   /* NSString won't end a match inside a composed character sequence */
    if (joins(nx) || (nx >= 0x1F3FB && nx <= 0x1F3FF)) return 0;
    return p;
}

static int home_pass(arena *a, const char *in, size_t n, const char *home, sbuf *out) {
    const unsigned char *s = (const unsigned char *)in, *h = (const unsigned char *)home;
    size_t hn = strlen(home), last = 0, i = 0, len;
    *out = (sbuf){a, NULL, 0, 0, 0};
    int utf8 = 1;
    for (size_t q = 0; q < hn; q += len) utf8 &= cp_at(h, hn, q, &len) != CP_BAD;
    while (hn && i < n) {
        size_t e = utf8 ? home_at(s, n, i, h, hn) : (hn <= n - i && !memcmp(s + i, h, hn) ? i + hn : 0);
        if (e) {
            sb_put(out, s + last, i - last);
            sb_put(out, "~", 1);
            last = i = e;
        } else i = utf8 ? next_cp(s, n, i) : i + 1;
    }
    sb_put(out, s + last, n - last);
    return out->nomem ? -1 : 0;
}

int32_t core_mask(const core_mask_ctx *mc, const char *in, size_t in_len, char *buf, size_t cap, size_t *len) {
    if (len) *len = 0;
    if (cap && buf) buf[0] = 0;
    if (!mc || mc->size < sizeof(core_mask_ctx) || mc->reserved || mc->n_names < 0 || mc->n_allowed < 0) return CORE_E_ARG;
    if ((cap && !buf) || (!in && in_len) || (mc->n_names && !mc->names) || (mc->n_allowed && !mc->allowed)) return CORE_E_ARG;
    if (in_len == CORE_NTS) in_len = in ? strlen(in) : 0;
    if (!in) in = "";
    static const struct { matcher m; const char *repl; int mode; } masks[] = {
        {m_pem, "<private key>", REPL_WHOLE}, {m_email, "<email>", REPL_WHOLE}, {m_query, "<token>", REPL_G1},
        {m_bearer, " <token>", REPL_G1}, {m_secret, "<token>", REPL_G1}, {m_jwt, "<token>", REPL_WHOLE},
        {m_aws, "<token>", REPL_WHOLE}, {m_github, "<token>", REPL_WHOLE}, {m_slack, "<token>", REPL_WHOLE},
        {m_sk, "<token>", REPL_WHOLE},
    };
    arena a[3] = {{0}, {0}, {0}};   /* the current text in one, the next pass into the other; a folded name */
    int cur = 0, r = CORE_OK;
    const char *s = in;
    size_t n = in_len;
    sbuf out;
#define STEP(call) do { \
        if ((call) < 0) { r = CORE_E_NOMEM; goto done; } \
        arena_reset(&a[cur]); cur ^= 1; s = out.p ? out.p : ""; n = out.len; \
    } while (0)
    if (mc->home && *mc->home) STEP(home_pass(&a[cur ^ 1], s, n, mc->home, &out));
    for (size_t k = 0; k < sizeof masks / sizeof *masks; k++) STEP(pass(&a[cur ^ 1], s, n, masks[k].m, masks[k].repl, masks[k].mode, &out));
    STEP(long_runs(&a[cur ^ 1], s, n, mc, &out));
    for (int32_t k = 0; k < mc->n_names; k++) {
        const char *name = mc->names[k];
        if (!name || name_counts((const unsigned char *)name, strlen(name)) <= 0) continue;
        name_m nm;
        if (name_fold(&a[2], name, &nm) < 0) { r = CORE_E_NOMEM; goto done; }
        STEP(names_pass(&a[cur ^ 1], s, n, &nm, &out));
        arena_reset(&a[2]);
    }
#undef STEP
    if (len) *len = n;
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        if (k) memcpy(buf, s, k);
        buf[k] = 0;
    }
    r = n < cap ? CORE_OK : CORE_E_SPACE;
done:
    if (r == CORE_E_NOMEM) {
        if (cap) buf[0] = 0;
        if (len) *len = 0;
    }
    arena_free(&a[0]);
    arena_free(&a[1]);
    arena_free(&a[2]);
    return r;
}
