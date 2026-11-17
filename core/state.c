/* The small files' formats and decisions (State.swift, Location.swift's bridge state): pure,
 * the host reads and writes the files. */
#include "internal.h"
#include <stdio.h>

/* ------------------------------------------------------------------ Swift's String pieces */

/* Swift's split(separator: "\n", omittingEmptySubsequences: false) on a String: "\r\n" is one
 * Character, so only a "\n" without a "\r" before it splits. Calls f for each piece. */
static int each_line(const char *s, size_t n, int (*f)(void *arg, const char *p, size_t k), void *arg) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\n' || (i > 0 && s[i - 1] == '\r')) continue;
        int r = f(arg, s + start, i - start);
        if (r) return r;
        start = i + 1;
    }
    return f(arg, s + start, n - start);
}

/* CharacterSet.whitespaces: Unicode's Zs and the tab, as UTF-8 at p; its length, or 0 */
static size_t white_at(const unsigned char *p, size_t n) {
    if (n >= 1 && (p[0] == ' ' || p[0] == '\t')) return 1;
    if (n >= 2 && p[0] == 0xC2 && p[1] == 0xA0) return 2;                          /* U+00A0 */
    if (n >= 3 && p[0] == 0xE1 && p[1] == 0x9A && p[2] == 0x80) return 3;          /* U+1680 */
    if (n >= 3 && p[0] == 0xE2 && p[1] == 0x80 && ((p[2] >= 0x80 && p[2] <= 0x8A) || p[2] == 0xAF)) return 3;   /* U+2000-200A, U+202F */
    if (n >= 3 && p[0] == 0xE2 && p[1] == 0x81 && p[2] == 0x9F) return 3;          /* U+205F */
    if (n >= 3 && p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) return 3;          /* U+3000 */
    return 0;
}

static int white_before(const unsigned char *s, size_t n, size_t *k) {
    for (size_t w = 1; w <= 3 && w <= n; w++)
        if (white_at(s + n - w, w) == w) { *k = w; return 1; }
    return 0;
}

static pstr trimmed(const char *p, size_t n) {
    const unsigned char *s = (const unsigned char *)p;
    size_t k;
    while (n && (k = white_at(s, n))) { s += k; n -= k; }
    while (n && white_before(s, n, &k)) n -= k;
    return (pstr){(const char *)s, n};
}

/* Int32(text) / Int(text): an optional sign, then ASCII digits, in range; 0 on failure */
static int parse_int(const char *p, size_t n, int64_t lo, int64_t hi, int64_t *out) {
    size_t i = 0;
    int neg = 0;
    if (i < n && (p[i] == '+' || p[i] == '-')) neg = p[i++] == '-';
    if (i == n) return 0;
    uint64_t v = 0, lim = neg ? (uint64_t)0 - (uint64_t)lo : (uint64_t)hi;
    for (; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        unsigned d = (unsigned)(p[i] - '0');
        if (v > (lim - d) / 10) return 0;
        v = v * 10 + d;
    }
    *out = neg ? (int64_t)((uint64_t)0 - v) : (int64_t)v;
    return 1;
}

static const char *find(const char *s, size_t n, const char *lit) {
    size_t k = strlen(lit);
    for (size_t i = 0; k <= n && i + k <= n; i++)
        if (!memcmp(s + i, lit, k)) return s + i;
    return NULL;
}

/* ------------------------------------------------------------------ installed, dismissed */

void state_chosen(const char *text, size_t n, int32_t *model, int32_t *cache) {
    *model = *cache = 0;
    if (!text || !utf8_valid(text, n)) return;
    *model = find(text, n, "model=1") != NULL;
    *cache = find(text, n, "cache=1") != NULL;
}

void state_chosen_text(char *b, size_t cap, int32_t model, int32_t cache) {
    snprintf(b, cap, "model=%d\ncache=%d\n", model ? 1 : 0, cache ? 1 : 0);
}

typedef struct { arena *a; keyset *k; int32_t cap; } keys_acc;

static int key_cmp(const pstr *x, const pstr *y) {
    size_t m = x->n < y->n ? x->n : y->n;
    int c = memcmp(x->s, y->s, m);
    return c ? c : (x->n > y->n) - (x->n < y->n);
}

static int add_key(keys_acc *acc, const char *p, size_t n) {
    keyset *k = acc->k;
    pstr s = {p, n};
    int32_t lo = 0, hi = k->n;
    while (lo < hi) {   /* sorted, no repeats */
        int32_t mid = lo + (hi - lo) / 2;
        int c = key_cmp(&k->keys[mid], &s);
        if (!c) return 0;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    if (k->n == acc->cap) {
        int32_t nc = acc->cap ? acc->cap * 2 : 16;
        if (nc > (1 << 24)) return -1;
        pstr *nk = arena_alloc(acc->a, (size_t)nc * sizeof *nk);
        if (!nk) return -1;
        if (k->n) memcpy(nk, k->keys, (size_t)k->n * sizeof *nk);
        k->keys = nk;
        acc->cap = nc;
    }
    char *c = arena_strdup(acc->a, p, n);
    if (!c) return -1;
    memmove(k->keys + lo + 1, k->keys + lo, (size_t)(k->n - lo) * sizeof *k->keys);
    k->keys[lo] = (pstr){c, n};
    k->n++;
    return 0;
}

static int key_line(void *arg, const char *p, size_t n) {
    if (!n) return 0;   /* split(separator:) drops empty pieces here */
    return add_key(arg, p, n);
}

int keyset_parse(arena *a, const char *text, size_t n, keyset *out) {
    out->keys = NULL;
    out->n = 0;
    if (!text || !utf8_valid(text, n)) return 0;
    keys_acc acc = {a, out, 0};
    return each_line(text, n, key_line, &acc) ? -1 : 0;
}

int32_t keyset_has(const keyset *k, const char *key, size_t n) {
    pstr s = {key, n};
    int32_t lo = 0, hi = k->n;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        int c = key_cmp(&k->keys[mid], &s);
        if (!c) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

void keyset_union_text(tbuf *b, const keyset *have, const pstr *add, int32_t nadd) {
    /* a merge of the sorted set and the (sorted here) additions */
    int32_t i = 0, j = 0, first = 1;
    pstr sorted[16];
    int32_t ns = 0;
    for (int32_t x = 0; x < nadd && ns < 16; x++) {
        int32_t pos = ns;
        while (pos > 0 && key_cmp(&sorted[pos - 1], &add[x]) > 0) { sorted[pos] = sorted[pos - 1]; pos--; }
        sorted[pos] = add[x];
        ns++;
    }
    while (i < have->n || j < ns) {
        const pstr *next;
        if (j >= ns || (i < have->n && key_cmp(&have->keys[i], &sorted[j]) <= 0)) {
            next = &have->keys[i++];
            while (j < ns && !key_cmp(next, &sorted[j])) j++;
        } else {
            next = &sorted[j++];
            while (j < ns && !key_cmp(next, &sorted[j])) j++;
        }
        if (!first) tb_puts(b, "\n");
        first = 0;
        tb_put(b, next->s, next->n);
    }
    tb_puts(b, "\n");
}

int32_t state_missing_keys(arena *a, int32_t chosen_model, int32_t chosen_cache, int32_t cache_present, int32_t plus_present,
                           const char *rekordbox_version, const char *engine, const keyset *dismissed, pstr out[2]) {
    int32_t n = 0;
    for (int part = 0; part < 2; part++) {
        int want = part ? chosen_model && !plus_present : chosen_cache && !cache_present;
        if (!want) continue;
        const char *pre = part ? "model:" : "cache:", *v = part ? engine : rekordbox_version;
        size_t pn = strlen(pre), vn = v ? strlen(v) : 0;
        char *k = arena_alloc(a, pn + vn + 1);
        if (!k) return -1;
        memcpy(k, pre, pn);
        if (vn) memcpy(k + pn, v, vn);
        k[pn + vn] = 0;
        if (dismissed && keyset_has(dismissed, k, pn + vn)) continue;
        out[n++] = (pstr){k, pn + vn};
    }
    return n;
}

/* ------------------------------------------------------------------ the free-space floor */

int64_t state_free_floor(int64_t disk) {
    const int64_t max = 50000000000LL;
    if (disk <= 0) return max;
    return disk / 10 < max ? disk / 10 : max;
}

int32_t state_paused_floor(int64_t free_bytes, int64_t disk, int64_t *floor) {
    *floor = state_free_floor(disk);
    return free_bytes >= 0 && free_bytes < *floor;
}

/* ------------------------------------------------------------------ the busy marker */

typedef struct { pstr lines[4]; int32_t n; int32_t more; } first_lines;

static int take_line(void *arg, const char *p, size_t n) {
    first_lines *l = arg;
    if (l->n < 4) l->lines[l->n++] = trimmed(p, n);
    else l->more = 1;
    return 0;
}

static int all_ascii_digits(pstr s) {
    if (!s.n) return 0;
    for (size_t i = 0; i < s.n; i++)
        if (s.s[i] < '0' || s.s[i] > '9') return 0;
    return 1;
}

int busy_marker_parse(arena *a, const char *text, size_t n, busy_marker *out) {
    memset(out, 0, sizeof *out);
    if (!text || !utf8_valid(text, n)) return 0;
    first_lines l = {{{0}}, 0, 0};
    each_line(text, n, take_line, &l);
    int64_t pid;
    if (!parse_int(l.lines[0].s, l.lines[0].n, INT32_MIN, INT32_MAX, &pid) || pid <= 0) return 0;
    out->pid = (int32_t)pid;
    int64_t start;
    if (l.n >= 2 && all_ascii_digits(l.lines[1]) && parse_int(l.lines[1].s, l.lines[1].n, INT64_MIN, INT64_MAX, &start)) {
        out->has_start = 1;
        out->start = start;
    }
    int32_t first = out->has_start ? 2 : 1;
    if (l.n - first >= 2 && l.lines[first].n) {
        char *id = arena_strdup(a, l.lines[first].s, l.lines[first].n);
        char *label = arena_strdup(a, l.lines[first + 1].s, l.lines[first + 1].n);
        if (!id || !label) return -1;
        out->has_action = 1;
        out->id = (pstr){id, l.lines[first].n};
        out->label = (pstr){label, l.lines[first + 1].n};
    }
    return 1;
}

int32_t busy_marker_live(const busy_marker *m, int64_t started, int32_t has_written, int64_t written) {
    if (started < 0) return 0;
    if (m->has_start) return started == m->start;
    if (!has_written) return 0;
    return started <= written;
}

void busy_marker_text(tbuf *b, int32_t pid, int64_t start, const char *id, const char *label) {
    tb_i64(b, pid);
    tb_puts(b, "\n");
    tb_i64(b, start);
    tb_puts(b, "\n");
    tb_puts(b, id);
    tb_puts(b, "\n");
    tb_puts(b, label);
    tb_puts(b, "\n");
}

/* ------------------------------------------------------------------ the bridge's state */

typedef struct { pstr *k, *v; int32_t n, cap; } kv_acc;

static int kv_line(void *arg, const char *p, size_t n) {
    kv_acc *acc = arg;
    const char *eq = memchr(p, '=', n);
    if (!eq) return 0;
    pstr k = {p, (size_t)(eq - p)}, v = {eq + 1, n - (size_t)(eq - p) - 1};
    for (int32_t i = 0; i < acc->n; i++)
        if (acc->k[i].n == k.n && !memcmp(acc->k[i].s, k.s, k.n)) { acc->v[i] = v; return 0; }   /* the later line wins */
    if (acc->n < acc->cap) { acc->k[acc->n] = k; acc->v[acc->n++] = v; }
    return 0;
}

static const pstr *kv_get(const kv_acc *acc, const char *key) {
    size_t n = strlen(key);
    for (int32_t i = 0; i < acc->n; i++)
        if (acc->k[i].n == n && !memcmp(acc->k[i].s, key, n)) return &acc->v[i];
    return NULL;
}

static int is_lit(const pstr *s, const char *lit) { return s->n == strlen(lit) && !memcmp(s->s, lit, s->n); }

static pstr dup_or_empty(arena *a, const pstr *s, int *nomem) {
    if (!s) return (pstr){"", 0};
    char *c = arena_strdup(a, s->s, s->n);
    if (!c) { *nomem = 1; return (pstr){"", 0}; }
    return (pstr){c, s->n};
}

int bridge_state_parse(arena *a, const char *text, size_t n, bridge_state *out) {
    memset(out, 0, sizeof *out);
    if (!text || n > 8192) return 0;
    size_t rn;
    text = utf8_repair(a, text, n, &rn);   /* String(decoding:as:) */
    if (!text) return -1;
    int32_t lines = 1;
    for (size_t i = 0; i < rn; i++) lines += text[i] == '\n';
    kv_acc acc = {arena_alloc(a, (size_t)lines * sizeof(pstr)), arena_alloc(a, (size_t)lines * sizeof(pstr)), 0, lines};
    if (!acc.k || !acc.v) return -1;
    each_line(text, rn, kv_line, &acc);
    const pstr *pid = kv_get(&acc, "pid"), *start = kv_get(&acc, "start"), *state = kv_get(&acc, "state");
    int64_t p, s;
    if (!pid || !parse_int(pid->s, pid->n, INT32_MIN, INT32_MAX, &p) || p <= 0) return 0;
    if (!start || !parse_int(start->s, start->n, INT64_MIN, INT64_MAX, &s)) return 0;
    if (!state || !(is_lit(state, "pending") || is_lit(state, "available") || is_lit(state, "unavailable"))) return 0;
    int nomem = 0;
    out->pid = (int32_t)p;
    out->start = s;
    out->state = dup_or_empty(a, state, &nomem);
    out->location = dup_or_empty(a, kv_get(&acc, "location"), &nomem);
    out->reason = dup_or_empty(a, kv_get(&acc, "reason"), &nomem);
    return nomem ? -1 : 1;
}

int32_t bridge_state_live(const bridge_state *s, const char *dir, int64_t started) {
    return s && dir && s->location.n == strlen(dir) && !memcmp(s->location.s, dir, s->location.n) && started == s->start;
}

/* Location.swift thisMacNeedsCleanup (M6) */
int32_t this_mac_needs_cleanup(int32_t chosen_available, int32_t bridge_location, int32_t model_folders) {
    return bridge_location && model_folders && chosen_available;
}

/* Location.swift mayRetryThisMacDeletion (M6) */
int32_t may_retry_this_mac_deletion(int32_t flag, int32_t chosen_available, int32_t bridge_location, int32_t rekordbox_running) {
    return flag && bridge_location && !rekordbox_running && chosen_available;
}
