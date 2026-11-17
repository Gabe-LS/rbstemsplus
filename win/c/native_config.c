/* config.ini's writer and configSets (native_config.h). C11, no I/O; strict UTF-8 by the core's
 * utf8_valid (cfg_ini.c's rule). */
#include "native_config.h"

#include "cfg_ini.h"    /* CFG_MAX, CFG_LINE_MAX */
#include "internal.h"   /* the core's utf8_valid, fmt_config_number */

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* a number as typed with "." for a decimal mark (Double(value)): 0 with *x, or -1 */
static int config_double(const char *v, double *x) {
    char *end = NULL;
    if (!v[0] || v[0] == ' ' || v[0] == '\t' || v[0] == '\n' || v[0] == '\r') return -1;
    *x = strtod(v, &end);
    return *end || !isfinite(*x) ? -1 : 0;
}

int native_config_pairs(const char *const *args, int nargs, native_config_pair pairs[8], char nums[8][32]) {
    if (!args || nargs < 2 || nargs % 2 || nargs > 16) return -1;
    int np = 0;
    for (int i = 0; i < nargs; i += 2, np++) {
        const char *k = args[i], *v = args[i + 1];
        double x;
        if (!k || !v) return -1;
        if (!strcmp(k, "rekordbox_model")) pairs[np] = (native_config_pair){k, v};
        else if ((!strcmp(k, "max_gb") || !strcmp(k, "max_days")) && !config_double(v, &x)) {
            fmt_config_number(x, nums[np]);
            pairs[np] = (native_config_pair){k, nums[np]};
        } else
            return -1;
    }
    return np;
}

/* one line by the grammar (configLines' ConfigLine): its bytes as in the file, without "\n" */
typedef struct {
    const char *p;
    size_t n;
    int section, ours, over_long;
    const char *key;   /* a key line's key, trimmed (NULL: none); key_n its length */
    size_t key_n;
} cline;

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* whether s[0..n) is name (lower case), case ignored */
static int key_is(const char *s, size_t n, const char *name) {
    if (strlen(name) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower(s[i]) != name[i]) return 0;
    return 1;
}

static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};

/* the lines of readable bytes into *out (malloc'd), and whether a section line was over the limit:
 * the count, or -1 (no memory) */
static long split(const char *b, size_t n, cline **out, int *overlong_section) {
    size_t cap = 1;
    for (size_t i = 0; i < n; i++) cap += b[i] == '\n';
    *overlong_section = 0;
    *out = malloc(cap * sizeof **out);
    if (!*out) return -1;
    long k = 0;
    int ours = 1;
    /* split(separator: "\n", omittingEmptySubsequences: false): "a\n" is "a" and ""; no bytes, none */
    for (size_t at = 0; n && at <= n; k++) {
        const char *e = memchr(b + at, '\n', n - at);
        size_t len = e ? (size_t)(e - (b + at)) : n - at;
        cline *l = &(*out)[k];
        memset(l, 0, sizeof *l);
        l->p = b + at;
        l->n = len;
        const char *s = l->p, *end = l->p + len;
        if (k == 0 && len >= 3 && !memcmp(s, bom, 3)) s += 3;   /* a BOM, at offset 0 only */
        l->over_long = (size_t)(end - s) > CFG_LINE_MAX;
        while (s < end && blank(*s)) s++;
        at += len + 1;
        if (s == end || *s == '#') continue;   /* a comment */
        if (*s == '[') {                      /* a section */
            l->section = 1;
            const char *close = memchr(s, ']', (size_t)(end - s));
            if (l->over_long) ours = 0, *overlong_section = 1;
            else if (close) {
                const char *a = s + 1, *z = close;
                while (a < z && (*a == ' ' || *a == '\t')) a++;
                while (z > a && (z[-1] == ' ' || z[-1] == '\t')) z--;
                ours = key_is(a, (size_t)(z - a), "cache");
            } else ours = 0;
            continue;
        }
        l->ours = ours;
        const char *eq = memchr(s, '=', (size_t)(end - s));
        if (!eq) continue;
        const char *z = eq;
        while (z > s && blank(z[-1])) z--;
        l->key = s;
        l->key_n = (size_t)(z - s);
    }
    return k;
}

int native_config_readable(const char *b, size_t n) {
    return b && n <= CFG_MAX && !memchr(b, 0, n) && utf8_valid(b, n);
}

int native_config_sets(const char *b, size_t n, const char *key) {
    if (!b) return 0;
    cline *ls;
    int ov;
    long k = split(b, n, &ls, &ov);
    int r = 0;
    for (long i = 0; i < k && !r; i++) r = ls[i].ours && ls[i].key && key_is(ls[i].key, ls[i].key_n, key);
    free(ls);
    return r;
}

static int listed(const char *s, size_t n, const char *const *names, int nn) {
    for (int i = 0; i < nn; i++)
        if (key_is(s, n, names[i])) return 1;
    return 0;
}

/* one line of the new text: bytes, or key=value */
typedef struct {
    const char *p;
    size_t n;
    const native_config_pair *kv;
} oline;

static const char header[] = "# RB Stems Plus settings (RB Stems Plus \xe2\x80\xba Settings\xe2\x80\xa6), read by Stems Cache when rekordbox starts.";
static const char unreadable_section[] = "[unreadable-section]";

int native_config_rewrite(const char *b, size_t n, const native_config_pair *values, int nvalues, const char *const *remove, int nremove,
                          char **out, size_t *out_n) {
    static const char *const location[2] = {"cache_dir", "cache_id"};
    int touches = 0;
    for (int i = 0; i < nvalues; i++) touches |= listed(values[i].key, strlen(values[i].key), location, 2);
    for (int i = 0; i < nremove; i++) touches |= listed(remove[i], strlen(remove[i]), location, 2);
    cline *ls = NULL;
    int ov = 0;
    long k = 0;
    if (b && (k = split(b, n, &ls, &ov)) < 0) return -1;
    oline *o = malloc(((size_t)k + (size_t)nvalues + 2) * sizeof *o);
    char *done = calloc((size_t)nvalues + 1, 1);
    if (!o || !done) {
        free(ls), free(o), free(done);
        return -1;
    }
    long m = 0, first_section = -1;
    int has_bom = 0;
    for (long i = 0; i < k; i++) {
        cline *l = &ls[i];
        const char *p = l->p;
        size_t pn = l->n;
        if (i == 0 && pn >= 3 && !memcmp(p, bom, 3)) p += 3, pn -= 3, has_bom = 1;
        if (l->section && first_section < 0) first_section = m;
        if (touches && ov) {
            if (l->section && l->over_long) {
                o[m++] = (oline){unreadable_section, sizeof unreadable_section - 1, NULL};
                continue;
            }
            if (!l->ours && l->key && listed(l->key, l->key_n, location, 2)) continue;
        }
        if (l->ours && l->key && listed(l->key, l->key_n, remove, nremove)) continue;
        int v = -1;
        for (int j = 0; l->ours && l->key && j < nvalues && v < 0; j++)
            if (key_is(l->key, l->key_n, values[j].key)) v = j;
        if (v >= 0) {
            o[m++] = (oline){NULL, 0, &values[v]};
            for (int j = 0; j < nvalues; j++)   /* done.insert(k): every pair of that key */
                if (!strcmp(values[j].key, values[v].key)) done[j] = 1;
        } else o[m++] = (oline){p, pn, NULL};
    }
    if (!b) {
        o[m++] = (oline){header, sizeof header - 1, NULL};
        o[m++] = (oline){"", 0, NULL};
    }
    /* missing keys go before the first [section] (top-level keys), else before a final newline */
    long at = first_section >= 0 ? first_section : m;
    if (at == m && m > 0 && o[m - 1].kv == NULL && o[m - 1].n == 0) at = m - 1;
    int add = 0;
    for (int j = 0; j < nvalues; j++) add += !done[j];
    memmove(o + at + add, o + at, (size_t)(m - at) * sizeof *o);
    for (int j = 0, a = 0; j < nvalues; j++)
        if (!done[j]) o[at + a++] = (oline){NULL, 0, &values[j]};
    m += add;
    size_t total = 3 + 1;
    for (long i = 0; i < m; i++) total += (o[i].kv ? strlen(o[i].kv->key) + 1 + strlen(o[i].kv->value) : o[i].n) + 1;
    char *t = malloc(total);
    if (!t) {
        free(ls), free(o), free(done);
        return -1;
    }
    size_t w = 0;
    if (has_bom) memcpy(t, bom, 3), w = 3;
    for (long i = 0; i < m; i++) {
        if (i) t[w++] = '\n';
        if (o[i].kv) {
            size_t kn = strlen(o[i].kv->key), vn = strlen(o[i].kv->value);
            memcpy(t + w, o[i].kv->key, kn), w += kn;
            t[w++] = '=';
            memcpy(t + w, o[i].kv->value, vn), w += vn;
        } else memcpy(t + w, o[i].p, o[i].n), w += o[i].n;
    }
    /* `if !out.hasSuffix("\n")` compares Characters: a final "\r\n" is one, not "\n", so Swift adds
     * a newline after it too (kept, so both write the same bytes) */
    if (!w || t[w - 1] != '\n' || (w >= 2 && t[w - 2] == '\r')) t[w++] = '\n';
    free(ls), free(o), free(done);
    if (w > CFG_MAX) {   /* readable no more: never written (review S42; the Mac's writer differs here) */
        free(t);
        return -1;
    }
    *out = t;
    *out_n = w;
    return 0;
}
