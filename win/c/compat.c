/* rekordbox, its versions and AlphaTheta's files on Windows (compat.h). The pure rules, the PE
 * reader, the memoised file facts over common/plat.h, and the facts the core's gating and R1
 * take. What only Windows answers comes through compat_os (compat_win.c). */
#include "plat.h"

#include "compat.h"
#include "inventory.h"
#include "native_facts.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP '\\'
#  define OTHER_SEP '/'
#else
#  define SEP '/'      /* the Mac tests: paths in a scratch folder */
#  define OTHER_SEP '\\'
#endif

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* ASCII case-blind: whether s starts with p; equality */
static int starts_ci(const char *s, const char *p) {
    for (; *p; s++, p++)
        if (lower((unsigned char)*s) != lower((unsigned char)*p)) return 0;
    return 1;
}
static int equal_ci(const char *a, const char *b) { return strlen(a) == strlen(b) && starts_ci(a, b); }

static int copy(char *dst, size_t cap, const char *s, size_t n) {
    if (n >= cap) return 0;
    memcpy(dst, s, n);
    dst[n] = 0;
    return 1;
}

/* ------------------------------------------------------------------ versions */

/* shortVersion: the first three non-empty "."-separated pieces, joined by "." */
static void short_version(const char *v, char *out, size_t cap) {
    size_t o = 0;
    int pieces = 0;
    out[0] = 0;
    for (const char *p = v; *p && pieces < 3;) {
        if (*p == '.') { p++; continue; }
        size_t n = strcspn(p, ".");
        if (o + (pieces ? 1 : 0) + n >= cap) return;
        if (pieces++) out[o++] = '.';
        memcpy(out + o, p, n);
        o += n;
        out[o] = 0;
        p += n;
    }
}

int compat_same_short_version(const char *a, const char *b) {
    char sa[COMPAT_VERSION_MAX], sb[COMPAT_VERSION_MAX];
    if (!a || !b || !*a || !*b) return 0;
    short_version(a, sa, sizeof sa);
    short_version(b, sb, sizeof sb);
    return *sa && !strcmp(sa, sb);
}

/* "X.Y.Z" or "X.Y.Z.W", each part 1-9 digits: the number of parts, or 0 */
static int numeric_parts(const char *s, unsigned long p[4]) {
    int k = 0;
    while (k < 4) {
        size_t n = 0;
        unsigned long v = 0;
        while (s[n] >= '0' && s[n] <= '9' && n < 10) v = v * 10 + (unsigned long)(s[n++] - '0');
        if (n == 0 || n > 9) return 0;
        p[k++] = v;
        s += n;
        if (!*s) return k >= 3 ? k : 0;
        if (*s++ != '.') return 0;
    }
    return 0;
}

static int version_order(const char *a, const char *b) {
    unsigned long pa[4] = {0}, pb[4] = {0};
    int na = numeric_parts(a, pa), nb = numeric_parts(b, pb);
    if (!na || !nb) return na - nb;
    for (int i = 0; i < 4; i++)
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    return 0;
}

int compat_folder_version(const char *folder, char *out, size_t n) {
    if (!folder) return 0;
    const char *name = folder;
    for (const char *p = folder; *p; p++)
        if (*p == SEP || *p == OTHER_SEP) name = p + 1;
    static const char prefix[] = "rekordbox ";
    if (!starts_ci(name, prefix)) return 0;
    const char *v = name + sizeof prefix - 1;
    unsigned long parts[4];
    if (numeric_parts(v, parts) != 3) return 0;
    return copy(out, n, v, strlen(v));
}

/* ------------------------------------------------------------------ uninstall entries */

int compat_entry_is_rekordbox(const compat_uninstall *e) {
    return e && (starts_ci(e->key, "Pioneer rekordbox ") || starts_ci(e->display_name, "rekordbox "));
}

/* a path out of a registry value: quotes dropped (the quoted part only), an icon index or
 * arguments after the ".exe" dropped, separators made SEP; then its folder when folder_of. 1 or 0. */
static int value_path(const char *v, int folder_of, char *out, size_t cap) {
    size_t n;
    while (*v == ' ') v++;
    if (*v == '"') {
        v++;
        n = strcspn(v, "\"");
    } else {
        n = strlen(v);
        for (size_t i = 0; i + 4 <= n; i++)   /* "...\Uninstall rekordbox.exe /S", "...\rekordbox.exe,0" */
            if (starts_ci(v + i, ".exe") && (i + 4 == n || v[i + 4] == ',' || v[i + 4] == ' ')) { n = i + 4; break; }
    }
    while (n && (v[n - 1] == ' ')) n--;
    if (!copy(out, cap, v, n) || !n) return 0;
    for (char *p = out; *p; p++)
        if (*p == OTHER_SEP) *p = SEP;
    if (folder_of) {
        char *last = strrchr(out, SEP);
        if (!last) return 0;
        *last = 0;
    }
    size_t k = strlen(out);
    while (k > 1 && out[k - 1] == SEP) out[--k] = 0;
    return k > 0;
}

/* whether p is a direct child of root named "rekordbox X.Y.Z" */
static int under_root(const char *p, const char *root) {
    char r[COMPAT_PATH_MAX];
    if (!value_path(root, 0, r, sizeof r)) return 0;
    size_t rn = strlen(r);
    if (strlen(p) <= rn + 1 || !starts_ci(p, r) || p[rn] != SEP) return 0;
    const char *name = p + rn + 1;
    char v[COMPAT_VERSION_MAX];
    return !strchr(name, SEP) && compat_folder_version(name, v, sizeof v);
}

int compat_entry_folder(const compat_uninstall *e, const char *root, char *out, size_t n) {
    char p[COMPAT_PATH_MAX];
    if (!e || !root) return 0;
    if (value_path(e->install_location, 0, p, sizeof p) && under_root(p, root)) return copy(out, n, p, strlen(p));
    if (value_path(e->uninstall_string, 1, p, sizeof p) && under_root(p, root)) return copy(out, n, p, strlen(p));
    if (value_path(e->display_icon, 1, p, sizeof p) && under_root(p, root)) return copy(out, n, p, strlen(p));
    return 0;
}

int compat_pin_subject(const char *organization, const char *serial_number) {
    return organization && serial_number && !strcmp(organization, "AlphaTheta Corporation") && !strcmp(serial_number, "0104-01-151827");
}

/* ------------------------------------------------------------------ the PE reader */

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct {
    compat_read_fn read;
    void *ctx;
    unsigned char sec[96 * 40];
    unsigned nsec;
} pe_ctx;

/* the file offset of rva..rva+len inside one section's raw data, or -1 */
static int64_t rva_off(const pe_ctx *pe, uint32_t rva, uint32_t len) {
    for (unsigned i = 0; i < pe->nsec; i++) {
        const unsigned char *s = pe->sec + 40 * i;
        uint32_t va = le32(s + 12), raw = le32(s + 16), ptr = le32(s + 20);
        if (rva >= va && (uint64_t)rva - va + len <= raw) return (int64_t)ptr + (rva - va);
    }
    return -1;
}

static int read_rva(const pe_ctx *pe, uint32_t rva, void *buf, uint32_t len) {
    int64_t off = rva_off(pe, rva, len);
    return off < 0 ? -1 : pe->read(pe->ctx, (uint64_t)off, buf, len);
}

/* the resource directory at rsrc + off: the entry wanted (id >= 0: that ID; -1: the first), as
 * its OffsetToData field; -1 when none */
static int64_t rsrc_entry(const pe_ctx *pe, uint32_t rsrc, uint32_t off, int id) {
    unsigned char d[16], e[8];
    if (read_rva(pe, rsrc + off, d, 16)) return -1;
    unsigned named = le16(d + 12), ids = le16(d + 14);
    if (named + ids > 4096) return -1;
    for (unsigned i = 0; i < named + ids; i++) {
        if (read_rva(pe, rsrc + off + 16 + 8 * i, e, 8)) return -1;
        uint32_t name = le32(e), to = le32(e + 4);
        if (id < 0 || (!(name & 0x80000000u) && name == (uint32_t)id)) return to;
    }
    return -1;
}

/* a version-resource node at p (blob[0..n)): its key compared with want (ASCII), its value and
 * children. 1 when the node fits. */
typedef struct {
    size_t end, value, value_len, children;
    uint16_t type, value_words;
    int key_is;
} vnode;
static size_t align4(size_t x) { return (x + 3) & ~(size_t)3; }
static int node(const unsigned char *b, size_t n, size_t p, const char *want, vnode *v) {
    if (p + 6 > n) return 0;
    size_t len = le16(b + p);
    if (len < 6 || p + len > n) return 0;
    v->end = p + len;
    v->value_words = le16(b + p + 2);
    v->type = le16(b + p + 4);
    size_t k = p + 6;
    v->key_is = 1;
    const char *w = want;
    for (;; k += 2) {
        if (k + 2 > v->end) return 0;
        uint16_t c = le16(b + k);
        if (!c) break;
        if (!w || !*w || c != (unsigned char)*w) v->key_is = 0;
        else w++;
    }
    if (w && *w) v->key_is = 0;
    v->value = align4(k + 2);
    /* a text value's length is in characters; a binary one's in bytes */
    v->value_len = v->type == 1 ? (size_t)v->value_words * 2 : v->value_words;
    if (v->value > v->end) v->value = v->end;
    if (v->value + v->value_len > v->end) v->value_len = v->end - v->value;
    v->children = align4(v->value + v->value_len);
    return 1;
}

/* a UTF-16 value as printable ASCII, trimmed; "" when it holds anything else */
static void ascii_value(const unsigned char *b, const vnode *v, char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    for (size_t k = v->value; k + 2 <= v->end; k += 2) {
        uint16_t c = le16(b + k);
        if (!c) break;
        if (c < 0x20 || c > 0x7e || o + 1 >= cap) { out[0] = 0; return; }
        out[o++] = (char)c;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    size_t s = 0;
    while (out[s] == ' ') s++;
    memmove(out, out + s, o - s + 1);
}

static void version_blob(const unsigned char *b, size_t n, compat_pe *out) {
    vnode root, c, t, s;
    if (!node(b, n, 0, "VS_VERSION_INFO", &root) || !root.key_is) return;
    if (root.value_len >= 52 && le32(b + root.value) == 0xFEEF04BDu) {
        uint32_t ms = le32(b + root.value + 16), ls = le32(b + root.value + 20);
        out->fixed_product[0] = (uint16_t)(ms >> 16), out->fixed_product[1] = (uint16_t)ms;
        out->fixed_product[2] = (uint16_t)(ls >> 16), out->fixed_product[3] = (uint16_t)ls;
        out->has_fixed = 1;
    }
    for (size_t p = root.children; p < root.end && node(b, n, p, "StringFileInfo", &c); p = align4(c.end)) {
        if (!c.key_is) continue;
        for (size_t q = c.children; q < c.end && node(b, c.end, q, NULL, &t); q = align4(t.end)) {   /* each StringTable */
            for (size_t r = t.children; r < t.end && node(b, t.end, r, "ProductVersion", &s); r = align4(s.end))
                if (s.key_is && !*out->product_version) ascii_value(b, &s, out->product_version, sizeof out->product_version);
            for (size_t r = t.children; r < t.end && node(b, t.end, r, "ProductName", &s); r = align4(s.end))
                if (s.key_is && !*out->product_name) ascii_value(b, &s, out->product_name, sizeof out->product_name);
            if (*out->product_version && *out->product_name) return;
        }
    }
}

int compat_pe_read(compat_read_fn read, void *ctx, compat_pe *out) {
    static const uint32_t max_version = 64 * 1024;
    memset(out, 0, sizeof *out);
    pe_ctx *pe = calloc(1, sizeof *pe);
    if (!pe) return -1;
    pe->read = read;
    pe->ctx = ctx;
    unsigned char h[64], nt[24], opt[240];
    int r = -1;
    if (read(ctx, 0, h, 64) || h[0] != 'M' || h[1] != 'Z') goto done;
    uint32_t lfanew = le32(h + 0x3c);
    if (lfanew > 0x10000 || read(ctx, lfanew, nt, 24) || memcmp(nt, "PE\0\0", 4)) goto done;
    out->machine = le16(nt + 4);
    pe->nsec = le16(nt + 6);
    uint16_t optsize = le16(nt + 20);
    if (pe->nsec > 96 || optsize < 2 || optsize > sizeof opt || read(ctx, lfanew + 24, opt, optsize)) goto done;
    if (pe->nsec && read(ctx, lfanew + 24 + optsize, pe->sec, 40 * (size_t)pe->nsec)) goto done;
    r = 0;   /* a PE: what follows is its version resource, if any */
    uint16_t magic = le16(opt);
    size_t count_at = magic == 0x20b ? 108 : magic == 0x10b ? 92 : 0;
    if (!count_at || optsize < count_at + 4 || le32(opt + count_at) < 3 || optsize < count_at + 4 + 3 * 8) goto done;
    uint32_t rsrc = le32(opt + count_at + 4 + 2 * 8);
    if (!rsrc) goto done;
    int64_t l1 = rsrc_entry(pe, rsrc, 0, 16);   /* RT_VERSION */
    if (l1 < 0 || !(l1 & 0x80000000)) goto done;
    int64_t l2 = rsrc_entry(pe, rsrc, (uint32_t)l1 & 0x7fffffffu, -1);
    if (l2 < 0 || !(l2 & 0x80000000)) goto done;
    int64_t l3 = rsrc_entry(pe, rsrc, (uint32_t)l2 & 0x7fffffffu, -1);
    if (l3 < 0 || (l3 & 0x80000000)) goto done;
    unsigned char de[8];
    if (read_rva(pe, rsrc + (uint32_t)l3, de, 8)) goto done;
    uint32_t drva = le32(de), dsize = le32(de + 4);
    if (!dsize || dsize > max_version) goto done;
    unsigned char *blob = malloc(dsize);
    if (blob && !read_rva(pe, drva, blob, dsize)) version_blob(blob, dsize, out);
    free(blob);
done:
    free(pe);
    return r;
}

/* ------------------------------------------------------------------ small rules */

int compat_resample_proved(const payload_manifest *m, const char *product_version, const char *exe_sha256) {
    if (!m || !m->has_windows || !product_version || !*product_version || !exe_sha256) return 0;
    for (int i = 0; i < m->n_resampler_proved; i++) {
        const char *pv = m->resampler_proved[i].product_version;
        unsigned long parts[4];
        int sameversion = !strcmp(pv, product_version) || (numeric_parts(pv, parts) == 3 && compat_same_short_version(pv, product_version));
        if (sameversion && !strcmp(m->resampler_proved[i].rekordbox_sha256, exe_sha256)) return 1;
    }
    return 0;
}

static const char *machine_name(uint16_t m) {
    switch (m) {
    case COMPAT_MACHINE_AMD64: return "x64";
    case COMPAT_MACHINE_ARM64: return "ARM64";
    case COMPAT_MACHINE_I386: return "x86";
    default: return NULL;
    }
}

const char *compat_machine_words(uint16_t process, uint16_t native) {
    static const struct { uint16_t p, n; const char *words; } emulated[] = {
        {COMPAT_MACHINE_AMD64, COMPAT_MACHINE_ARM64, "x64 under emulation on ARM64"},
        {COMPAT_MACHINE_I386, COMPAT_MACHINE_ARM64, "x86 under emulation on ARM64"},
        {COMPAT_MACHINE_I386, COMPAT_MACHINE_AMD64, "x86 under emulation on x64"},
    };
    if (!machine_name(process) || !machine_name(native)) return "";
    if (process == native) return "native";
    for (size_t i = 0; i < sizeof emulated / sizeof emulated[0]; i++)
        if (emulated[i].p == process && emulated[i].n == native) return emulated[i].words;
    return "";
}

/* ------------------------------------------------------------------ file facts, memoised */

enum { MEMO_SLOTS = 32 };
static struct {
    char path[COMPAT_PATH_MAX];
    int64_t size, mtime;
    plat_id id;          /* a file swapped in by a rename is another file, whatever its size and time */
    uint64_t used;
    compat_file facts;   /* pin COMPAT_PIN_ERROR: not asked yet */
    int pinned, code_checked;
    int main_only;       /* an unreadable file, remembered for the UI thread's reads only */
    int main_pin_error;  /* the UI thread's pin had no answer: it isn't asked again there */
} memo[MEMO_SLOTS];
static uint64_t memo_clock;
static plat_mutex memo_lock = PLAT_MUTEX_INIT;

static _Thread_local struct {
    int on;
    void (*miss)(void *ctx, const char *path);
    void *ctx;
} main_reads;

void compat_main_reads(int on, void (*miss)(void *ctx, const char *path), void *ctx) {
    main_reads.on = on;
    main_reads.miss = on ? miss : NULL;
    main_reads.ctx = on ? ctx : NULL;
}

#ifdef COMPAT_TEST_HOOKS
int64_t compat_test_scanned;
int (*compat_test_stat_fails)(const char *path);
#endif

void compat_memo_clear(void) {
    plat_mutex_lock(&memo_lock);
    memset(memo, 0, sizeof memo);
    plat_mutex_unlock(&memo_lock);
}

/* under memo_lock: the facts remembered for path while its stat holds (size, mtime, id) */
static int memo_find(const char *path, const plat_stat *st, int want_pin, int want_code, int on_main, compat_file *out, int *need_pin) {
    for (int i = 0; i < MEMO_SLOTS; i++)
        if (memo[i].used && memo[i].size == st->size && memo[i].mtime == st->mtime && plat_id_same(memo[i].id, st->id)
            && !strcmp(memo[i].path, path) && (on_main || !memo[i].main_only) && (!want_code || memo[i].code_checked)) {
            *out = memo[i].facts;
            memo[i].used = ++memo_clock;
            *need_pin = want_pin && !memo[i].pinned && !(on_main && (memo[i].main_pin_error || memo[i].main_only));
            return 1;
        }
    return 0;
}

/* Single-flight (review HR5): a miss on a path whose read is already running waits for it, then
 * stats its own handle again and takes the memo only when that still matches; otherwise it reads
 * the file itself. A hit that still needs its pin flies or waits the same way (review SF5): the
 * woken read finds the pin remembered. One flight per path, under memo_lock. A read re-entered on
 * the thread that owns the path's flight (a pin callback asking the same file) doesn't wait: it
 * reads. With every slot busy a read runs without one (no waiter, as before). FRESH reads never
 * wait or fly. */
enum { FLIGHTS = 8 };
static struct {
    int busy;
    plat_thread_id owner;
    char path[COMPAT_PATH_MAX];
} flight[FLIGHTS];
static plat_cond flight_cond;   /* broadcast when a flight ends */
static plat_once_t flight_once = PLAT_ONCE_INIT;
static void flight_init(void) { plat_cond_init(&flight_cond); }

static int flight_of(const char *path) {
    for (int i = 0; i < FLIGHTS; i++)
        if (flight[i].busy && !strcmp(flight[i].path, path)) return i;
    return -1;
}
static int flight_start(const char *path) {
    for (int i = 0; i < FLIGHTS; i++)
        if (!flight[i].busy) {
            flight[i].busy = 1;
            flight[i].owner = plat_thread_self();
            memcpy(flight[i].path, path, strlen(path) + 1);
            return i;
        }
    return -1;
}
static void flight_end_locked(int k) {
    if (k < 0) return;
    flight[k].busy = 0;
    plat_cond_broadcast(&flight_cond);
}
static void flight_end(int k) {
    if (k < 0) return;
    plat_mutex_lock(&memo_lock);
    flight_end_locked(k);
    plat_mutex_unlock(&memo_lock);
}

/* splits path into its folder and name: 1, or 0 when it has no folder */
static int split(const char *path, char *dir, size_t cap, const char **name) {
    const char *last = strrchr(path, SEP);
    if (!last || last == path) return 0;
    *name = last + 1;
    return **name && copy(dir, cap, path, (size_t)(last - path));
}

static int pread_fn(void *ctx, uint64_t off, void *buf, size_t n) {
    size_t got = 0;
    int e = plat_pread(*(plat_file *)ctx, buf, n, (int64_t)off, &got);
    return e || got != n;
}

/* the whole file: sha256, the marker and the bridge's cache line (bridgeCacheModels: the bytes
 * after the first "rbstems-cache: " up to a ';' within 4096 of them), in one pass */
static int scan(plat_file f, compat_file *out) {
    static const char marker[] = "rbstems bridge: ", cline[] = "rbstems-cache: ";
    enum { CHUNK = 1 << 20, KEEP = sizeof marker - 2, CL = sizeof cline - 1 };
    _Static_assert(KEEP >= CL - 1, "the carry holds a cache line's prefix cut by a chunk");
    int cstate = 0;   /* 0 looking for cline, 1 copying the line, 2 done */
    size_t cn = 0;
    unsigned char *buf = malloc(CHUNK + KEEP);
    if (!buf) return -1;
    plat_sha256_ctx c;
    if (plat_sha256_init(&c)) {
        free(buf);
        return -1;
    }
    size_t carry = 0;
    int e = 0;
    for (;;) {
        size_t got = 0;
        if ((e = plat_read(f, buf + carry, CHUNK, &got)) || !got) break;
#ifdef COMPAT_TEST_HOOKS
        compat_test_scanned += (int64_t)got;   /* the tests read from one thread */
#endif
        plat_sha256_update(&c, buf + carry, got);
        size_t have = carry + got;
        size_t from = carry;   /* the bytes not seen before: the line goes on there */
        /* one pass for both texts (review HG4): each starts with 'r', so only an 'r' is tested */
        for (unsigned char *p = !out->marker || cstate == 0 ? memchr(buf, 'r', have) : NULL; p;
             p = memchr(p + 1, 'r', have - (size_t)(p + 1 - buf))) {
            size_t i = (size_t)(p - buf);
            if (!out->marker && i + sizeof marker - 1 <= have) out->marker = !memcmp(p, marker, sizeof marker - 1);
            if (cstate == 0 && i + CL <= have && !memcmp(p, cline, CL)) {
                cstate = 1;   /* the first one */
                from = i + CL;
            }
            if (out->marker && cstate != 0) break;
        }
        for (size_t i = from; cstate == 1 && i < have; i++) {
            if (cn == COMPAT_CACHE_LINE_MAX) cstate = 2;   /* no ';' within the 4096 bytes after it: no line */
            else if (buf[i] == ';') {
                out->cache_line[cn] = 0;
                out->cache_line_len = cn;
                out->has_cache_line = 1;
                cstate = 2;
            } else out->cache_line[cn++] = (char)buf[i];
        }
        carry = have < KEEP ? have : KEEP;
        memmove(buf, buf + have - carry, carry);
    }
    unsigned char d[32];
    int fe = plat_sha256_final(&c, d);
    free(buf);
    if (e || fe) return -1;
    if (!out->has_cache_line) {
        out->cache_line[0] = 0;
        out->cache_line_len = 0;
    }
    for (int i = 0; i < 32; i++) {
        static const char hex[] = "0123456789abcdef";
        out->sha256[2 * i] = hex[d[i] >> 4];
        out->sha256[2 * i + 1] = hex[d[i] & 15];
    }
    out->sha256[64] = 0;
    return 0;
}

static int file_facts(const compat_os *os, const char *path, int flags,
                      int (*digest)(compat_read_fn, void *, uint64_t, payload_sha), compat_file *out) {
    int want_pin = (flags & COMPAT_PIN) != 0, fresh = (flags & COMPAT_FRESH) != 0;
    memset(out, 0, sizeof *out);
    out->pin = COMPAT_PIN_ERROR;
    char dir[COMPAT_PATH_MAX];
    const char *name;
    if (!path || strlen(path) >= COMPAT_PATH_MAX || !split(path, dir, sizeof dir, &name)) return -1;
    plat_dir d;
    plat_file f;
    int64_t size = 0;
    int e = plat_dir_open(dir, &d);
    if (e) return e == ENOENT || e == ENOTDIR ? 0 : -1;   /* no folder: nothing there */
    e = plat_file_open_at(d, name, INT64_MAX, &size, &f);
    plat_dir_close(d);
    if (e == ENOENT) return 0;
    if (e == PLAT_EFTYPE || e == ELOOP || e == EISDIR) {   /* a link, a reparse point, a folder */
        out->present = 1;
        return 0;
    }
    if (e) return -1;
    out->present = out->exists = 1;
    plat_stat st;
    if (plat_file_stat(f, &st)) {
        plat_file_close(f);
        return 0;
    }
    out->size = st.size;
    out->mtime = st.mtime;
    out->id = st.id;

    int hit = 0, need_pin = want_pin, fl = -1, on_main = main_reads.on;
    plat_once(&flight_once, flight_init);
    plat_mutex_lock(&memo_lock);
    for (;;) {
        hit = !fresh && memo_find(path, &st, want_pin, digest != NULL, on_main, out, &need_pin);   /* FRESH: read again, whatever is remembered */
        if ((hit && !need_pin) || fresh) break;
        /* a miss, or a hit that still needs its pin (review SF5): one flight reads or pins it */
        int k = flight_of(path);
        if (k < 0) {
            fl = flight_start(path);   /* -1: every slot busy, read without one */
            break;
        }
        if (plat_thread_equal(flight[k].owner, plat_thread_self())) break;   /* re-entered from its own pin: read here */
#ifdef COMPAT_TEST_HOOKS
        __atomic_add_fetch(&compat_test_flight_waits, 1, __ATOMIC_SEQ_CST);   /* the test polls it unlocked */
#endif
        plat_cond_wait(&flight_cond, &memo_lock);
        /* the file may have changed while this waited: stat it again through this handle, then
         * the memo must match that (size, mtime, id) or this reads it itself; a hit's facts taken
         * before the wait are dropped (taken again, or read) */
        memset(out, 0, sizeof *out);
        out->present = out->exists = 1;
        out->pin = COMPAT_PIN_ERROR;
        plat_mutex_unlock(&memo_lock);
        int se = plat_file_stat(f, &st);
        plat_mutex_lock(&memo_lock);
        if (se) {
            plat_mutex_unlock(&memo_lock);
            plat_file_close(f);
            return 0;
        }
        out->size = st.size;
        out->mtime = st.mtime;
        out->id = st.id;
    }
    plat_mutex_unlock(&memo_lock);
    /* the UI thread reads or pins: the warm missed this file (said once per file and stat: the
     * answer is remembered below, for the UI thread's next reads too) */
    if (on_main && (!hit || need_pin) && main_reads.miss) main_reads.miss(main_reads.ctx, path);

    if (!hit) {
#ifdef COMPAT_TEST_HOOKS
        if (compat_test_before_read) compat_test_before_read(path);
#endif
        out->readable = scan(f, out) == 0;
#ifdef COMPAT_TEST_HOOKS
        if (compat_test_read_fails && compat_test_read_fails(path)) out->readable = 0;
#endif
        if (compat_pe_read(pread_fn, &f, &out->pe)) memset(&out->pe, 0, sizeof out->pe);
        if (!out->readable) memset(out->sha256, 0, sizeof out->sha256);
    }
    if (need_pin) out->pin = os && os->pin ? os->pin(os->ctx, path, f) : COMPAT_PIN_ERROR;
    int computed_code = digest && !hit && out->readable;
    if (computed_code && !out->marker && digest(pread_fn, &f, (uint64_t)st.size, out->code_sha256))
        out->code_sha256[0] = 0;
    if (need_pin || computed_code) {
        /* The pin and code digest describe this handle and the file still at path. */
        plat_stat st2, st3;
        if (plat_file_stat(f, &st2) || plat_stat_path(path, 0, &st3) || !plat_id_same(st2.id, st.id) || !plat_id_same(st3.id, st.id)
            || st2.size != st.size || st3.size != st.size || st2.mtime != st.mtime || st3.mtime != st.mtime) {
            plat_file_close(f);
            memset(out, 0, sizeof *out);
            out->pin = COMPAT_PIN_ERROR;
            flight_end(fl);
            return -1;
        }
    }
    plat_file_close(f);
    /* unreadable: not memoised, read again next time; but on the UI thread, remembered for its
     * own next reads (a worker's read never sees that entry; a worker waiter reads it itself) */
    if (!out->readable && !(on_main && !hit)) {
        flight_end(fl);
        return 0;
    }

    if (!hit || need_pin) {
        plat_mutex_lock(&memo_lock);
        int slot = 0;
        for (int i = 0; i < MEMO_SLOTS; i++) {
            if (memo[i].used && !strcmp(memo[i].path, path)) { slot = i; break; }   /* one slot per path */
            if (memo[i].used < memo[slot].used) slot = i;
        }
        int same = memo[slot].used && !strcmp(memo[slot].path, path) && memo[slot].size == st.size && memo[slot].mtime == st.mtime
                && plat_id_same(memo[slot].id, st.id);
        if (same && memo[slot].code_checked && !computed_code)
            memcpy(out->code_sha256, memo[slot].facts.code_sha256, sizeof out->code_sha256);
        if (same && memo[slot].pinned && (!want_pin || out->pin == COMPAT_PIN_ERROR)) {
            /* another read pinned this file meanwhile (another thread, or a pin that re-entered):
             * a read with no pin answer keeps that answer and takes it (HR3) */
            if (computed_code) {
                memcpy(memo[slot].facts.code_sha256, out->code_sha256, sizeof out->code_sha256);
                memo[slot].code_checked = 1;
            }
            if (want_pin && !fresh) *out = memo[slot].facts;
            memo[slot].used = ++memo_clock;
        } else {
            memcpy(memo[slot].path, path, strlen(path) + 1);
            memo[slot].size = st.size;
            memo[slot].mtime = st.mtime;
            memo[slot].id = st.id;
            int was_main_pin_error = same && memo[slot].main_pin_error;
            memo[slot].facts = *out;
            /* a pin that couldn't be checked isn't remembered as an answer: a worker asks again;
             * the UI thread doesn't (main_pin_error) until the file changes */
            memo[slot].code_checked = computed_code || (same && memo[slot].code_checked);
            memo[slot].pinned = want_pin && out->pin != COMPAT_PIN_ERROR;
            memo[slot].main_only = !out->readable;
            memo[slot].main_pin_error = !memo[slot].pinned && ((on_main && want_pin && out->pin == COMPAT_PIN_ERROR)
                                                               || (hit && was_main_pin_error));
            memo[slot].used = ++memo_clock;
        }
        flight_end_locked(fl);   /* with the memo in place (or kept): a waiter finds it */
        plat_mutex_unlock(&memo_lock);
    }
    if (!want_pin) out->pin = COMPAT_PIN_ERROR;
    return 0;
}

int compat_file_facts(const compat_os *os, const char *path, int flags, compat_file *out) {
    return file_facts(os, path, flags, NULL, out);
}

int compat_ort_file_facts(const compat_os *os, const char *path, int flags,
                         int (*digest)(compat_read_fn, void *, uint64_t, payload_sha), compat_file *out) {
    return file_facts(os, path, flags, digest, out);
}

/* ------------------------------------------------------------------ rekordbox */

static int join(char *out, size_t cap, const char *a, const char *b) {
    size_t an = strlen(a), bn = strlen(b);
    if (an + 1 + bn >= cap) return 0;
    memcpy(out, a, an);
    out[an] = SEP;
    memcpy(out + an + 1, b, bn + 1);
    return 1;
}

static int inv_stat(const char *path, plat_stat *st) {
    int err = plat_stat_path(path, 0, st);
#ifdef COMPAT_TEST_HOOKS
    if (!err && compat_test_stat_fails && compat_test_stat_fails(path)) err = EACCES;
#endif
    return err;
}

static int inv_index(const inv_list *l, const char *folder) {
    for (int i = 0; i < l->n; i++)
        if (equal_ci(l->it[i].folder, folder)) return i;
    return -1;
}

static inv_install *inv_add(inv_list *l, const char *folder, const char *name, const char *version) {
    inv_install *r = &l->it[l->n++];
    strcpy(r->folder, folder);
    strcpy(r->name, name);
    strcpy(r->version, version);
    return r;
}

static int absent(int err) { return err == ENOENT || err == ENOTDIR; }

static int inv_exe_read(const compat_os *os, inv_install *r, int flags) {
    plat_stat dir, after;
    int err = inv_stat(r->folder, &dir);
    r->exe.pin = COMPAT_PIN_ERROR;
    if (absent(err)) return INV_STALE;
    if (err) return INV_NO_ANSWER;
    if (dir.kind != PLAT_DIR) return INV_UNVERIFIED;
    r->dir_id = dir.id;

    char exe[COMPAT_PATH_MAX];
    if (!join(exe, sizeof exe, r->folder, "rekordbox.exe")
        || compat_file_facts(os, exe, COMPAT_PIN | flags, &r->exe)
        || inv_stat(r->folder, &after) || after.kind != PLAT_DIR || !plat_id_same(dir.id, after.id)) {
        memset(&r->exe, 0, sizeof r->exe);
        r->exe.pin = COMPAT_PIN_ERROR;
        return INV_NO_ANSWER;
    }
    if (!r->exe.present) return INV_SETTLING;
    if (!r->exe.exists) return INV_UNVERIFIED;
    if (!r->exe.readable || r->exe.pin == COMPAT_PIN_ERROR) return INV_NO_ANSWER;
    return r->exe.pin == COMPAT_PIN_OK
        && compat_same_short_version(r->version, r->display_version)
        && compat_same_short_version(r->version, r->exe.pe.product_version) ? INV_VERIFIED : INV_UNVERIFIED;
}

typedef struct {
    const char *root;
    inv_list *list;
} inv_disk_walk;

static int inv_disk_one(void *ctx, const char *name, const plat_stat *st) {
    inv_disk_walk *w = ctx;
    char v[COMPAT_VERSION_MAX], folder[COMPAT_PATH_MAX], exe[COMPAT_PATH_MAX];
    if (st->kind != PLAT_DIR || !compat_folder_version(name, v, sizeof v)) return 0;
    if (!join(folder, sizeof folder, w->root, name)) return -1;
    if (inv_index(w->list, folder) >= 0) return 0;
    if (w->list->n == INV_MAX || !join(exe, sizeof exe, folder, "rekordbox.exe")) return -1;

    plat_stat ds, xs;
    int err = inv_stat(folder, &ds);
    if (absent(err) || (!err && ds.kind != PLAT_DIR)) return 0;
    inv_install *r = inv_add(w->list, folder, name, v);
    r->trust = INV_DISK_ONLY;
    r->exe.pin = COMPAT_PIN_ERROR;
    if (!err) {
        r->dir_id = ds.id;
        err = inv_stat(exe, &xs);
        if (!err && xs.kind == PLAT_FILE) {
            r->exe.present = r->exe.exists = 1;
            r->exe.id = xs.id;
            r->exe.size = xs.size;
            r->exe.mtime = xs.mtime;
        }
    }
    /* A possible EXE in an unreadable disk-only folder is evidence for legacy
     * selection on a partial registry walk. It was never pinned. */
    if (err && err != ENOENT) r->exe.present = 1;
    return 0;
}

static int inv_read_disk(const char *root, inv_list *l) {
    plat_stat st;
    int err = inv_stat(root, &st);
    if (err) return err == ENOENT;
    if (st.kind != PLAT_DIR) return 0;
    plat_dir dir;
    if (plat_dir_open(root, &dir)) return 0;
    inv_disk_walk walk = {root, l};
    int complete = plat_dir_each(dir, inv_disk_one, &walk) == 0;
    plat_dir_close(dir);
    return complete;
}

static int inv_order(const void *a, const void *b) {
    const inv_install *one = a, *two = b;
    int order = version_order(two->version, one->version);
    return order ? order : strcmp(one->version, two->version);
}

static int inv_walk(const compat_os *os, const char *root, int flags, int max_registered, inv_list *out) {
    memset(out, 0, sizeof *out);
    if (!os || !os->uninstall_entries || !root) return 0;
#ifdef COMPAT_TEST_HOOKS
    if (compat_test_entries_alloc_fails) return -1;
#endif
    compat_uninstall *entries = calloc(COMPAT_ENTRIES_MAX, sizeof *entries);
    if (!entries) return -1;
    int partial = 0;
    int n = os->uninstall_entries(os->ctx, entries, COMPAT_ENTRIES_MAX, &partial);
    out->registry_complete = !partial && n >= 0 && n < COMPAT_ENTRIES_MAX;
    if (n > COMPAT_ENTRIES_MAX) n = COMPAT_ENTRIES_MAX;
    for (int i = 0; i < n; i++) {
        const compat_uninstall *entry = &entries[i];
        char folder[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX];
        if (!compat_entry_is_rekordbox(entry) || !compat_entry_folder(entry, root, folder, sizeof folder)) continue;
        int at = inv_index(out, folder);
        if (at >= 0) {
            inv_install *r = &out->it[at];
            if (r->trust == INV_VERIFIED && !compat_same_short_version(r->display_version, entry->display_version))
                r->trust = INV_UNVERIFIED;
            r->views |= entry->view;
        } else if (out->n < max_registered && compat_folder_version(folder, version, sizeof version)) {
            inv_install *r = inv_add(out, folder, strrchr(folder, SEP) + 1, version);
            copy(r->display_version, sizeof r->display_version, entry->display_version, strlen(entry->display_version));
            r->views = entry->view;
            r->trust = inv_exe_read(os, r, flags);
        }
    }
    free(entries);

    out->disk_complete = inv_read_disk(root, out);
    out->complete = out->registry_complete && out->disk_complete;
    for (int i = 0; i < out->n; i++) {
        const inv_install *r = &out->it[i];
        if (r->trust == INV_NO_ANSWER || r->trust == INV_SETTLING || (r->trust == INV_DISK_ONLY && r->exe.present && !r->exe.exists))
            out->complete = 0;
    }
    qsort(out->it, (size_t)out->n, sizeof out->it[0], inv_order);
    return 0;
}

int inv_read(const compat_os *os, const char *root, int flags, inv_list *out) {
#ifdef _WIN32
    uint64_t gen = compat_uninstall_generation();
#else
    uint64_t gen = 0;
#endif
    int rc = inv_walk(os, root, flags, INV_MAX, out);
    out->gen = gen;
    return rc;
}

int inv_current(const inv_list *l, compat_rekordbox *out, int max, int *current, int *state) {
    *current = -1;
    *state = COMPAT_RB_NOT_INSTALLED;
    int n = 0, candidate = -1;
    for (int i = 0; i < l->n && n < max; i++) {
        const inv_install *r = &l->it[i];
        if (r->trust == INV_DISK_ONLY) continue;
        compat_rekordbox *c = &out[n];
        memset(c, 0, sizeof *c);
        strcpy(c->folder, r->folder);
        strcpy(c->folder_version, r->version);
        strcpy(c->display_version, r->display_version);
        c->exe = r->exe;
        c->unknown = r->trust == INV_NO_ANSWER && !r->exe.exists;
        c->agree = compat_same_short_version(r->version, r->display_version)
                && compat_same_short_version(r->version, r->exe.pe.product_version);
        c->ok = r->trust == INV_VERIFIED;
        if (candidate < 0 && (c->exe.exists || c->unknown)) {
            candidate = n;
            c->candidate = 1;
            if (c->ok) {
                *current = n;
                *state = COMPAT_RB_OK;
            } else if (c->unknown || !c->exe.readable) {
                *state = COMPAT_RB_NO_ANSWER;
            } else if (!c->agree || c->exe.pin == COMPAT_PIN_NO || r->trust == INV_UNVERIFIED) {
                *state = COMPAT_RB_UNVERIFIED;
            } else {
                *state = COMPAT_RB_NO_ANSWER;
            }
        }
        n++;
    }
    if (!l->registry_complete) {
        int may_be_newer = !l->disk_complete;
        for (int i = 0; !may_be_newer && i < l->n; i++) {
            const inv_install *r = &l->it[i];
            may_be_newer = r->trust == INV_DISK_ONLY && r->exe.present
                && (candidate < 0 || version_order(r->version, out[candidate].folder_version) > 0);
        }
        if (may_be_newer) {
            if (candidate >= 0) out[candidate].candidate = 0;
            *current = -1;
            *state = COMPAT_RB_NO_ANSWER;
        }
    }
    return n;
}

typedef struct { int err; plat_stat st; } inv_stamp;
typedef struct {
    inv_stamp root;
    struct { inv_stamp dir, exe; } rows[INV_MAX];
} inv_stamps;

static inv_stamp inv_stamp_path(const char *folder, const char *name) {
    inv_stamp stamp = {0};
    if (!name) stamp.err = inv_stat(folder, &stamp.st);
    else {
        char path[COMPAT_PATH_MAX];
        stamp.err = join(path, sizeof path, folder, name) ? inv_stat(path, &stamp.st) : ENAMETOOLONG;
    }
    return stamp;
}

static int inv_same_stamp(inv_stamp a, inv_stamp b) {
    return a.err == b.err && (a.err || (a.st.kind == b.st.kind && a.st.size == b.st.size
           && a.st.mtime == b.st.mtime && plat_id_same(a.st.id, b.st.id)));
}

static int inv_stamps_hold(const inv_list *l, const char *root, const inv_stamps *s) {
    if (!inv_same_stamp(s->root, inv_stamp_path(root, NULL))) return 0;
    for (int i = 0; i < l->n; i++) {
        const char *f = l->it[i].folder;
        if (!inv_same_stamp(s->rows[i].dir, inv_stamp_path(f, NULL))
            || (!s->rows[i].dir.err && s->rows[i].dir.st.kind == PLAT_DIR
                && !inv_same_stamp(s->rows[i].exe, inv_stamp_path(f, "rekordbox.exe")))) return 0;
    }
    return 1;
}

static int inv_stamps_save(const inv_list *l, const char *root, inv_stamps *s, inv_stamp before) {
    s->root = inv_stamp_path(root, NULL);
    if (!inv_same_stamp(s->root, before)) return 0;
    for (int i = 0; i < l->n; i++) {
        const inv_install *r = &l->it[i];
        inv_stamp dir = s->rows[i].dir = inv_stamp_path(r->folder, NULL);
        if (r->trust == INV_STALE) {
            if (!absent(dir.err)) return 0;
        } else if (dir.err) return 0;
        if (dir.err || dir.st.kind != PLAT_DIR) {
            if (r->exe.present || r->trust == INV_VERIFIED || r->trust == INV_DISK_ONLY) return 0;
            continue;
        }
        if (!plat_id_same(dir.st.id, r->dir_id)) return 0;
        inv_stamp exe = s->rows[i].exe = inv_stamp_path(r->folder, "rekordbox.exe");
        if (r->exe.exists) {
            if (exe.err || exe.st.kind != PLAT_FILE || exe.st.size != r->exe.size
                || exe.st.mtime != r->exe.mtime || !plat_id_same(exe.st.id, r->exe.id)) return 0;
        } else if (!exe.err && exe.st.kind == PLAT_FILE) return 0;
    }
    return 1;
}

int inv_read_cached(inv_cache *c, const compat_os *os, uint64_t gen, const char *root, inv_list *out) {
    if (!c || !root) {
        int rc = inv_walk(os, root, 0, INV_MAX, out);
        out->gen = gen;
        return rc;
    }
    int rc = 0;
    plat_mutex_lock(&c->lock);
    if (c->valid && gen && gen == c->gen && !strcmp(root, c->root) && inv_stamps_hold(c->list, root, c->stamps)) {
        *out = *c->list;
    } else {
        c->valid = 0;
        c->walks++;
        inv_stamp before = inv_stamp_path(root, NULL);
        rc = inv_walk(os, root, 0, INV_MAX, out);
        out->gen = gen;
        if (!rc && out->complete && gen && strlen(root) < sizeof c->root) {
            if (!c->list) c->list = malloc(sizeof *c->list);
            if (!c->stamps) c->stamps = malloc(sizeof(inv_stamps));
            if (c->list && c->stamps && inv_stamps_save(out, root, c->stamps, before)) {
                *c->list = *out;
                strcpy(c->root, root);
                c->gen = gen;
                c->valid = 1;
            }
        }
    }
    plat_mutex_unlock(&c->lock);
    return rc;
}

void inv_cache_clear(inv_cache *c) {
    plat_mutex_lock(&c->lock);
    free(c->list);
    free(c->stamps);
    c->list = NULL;
    c->stamps = NULL;
    c->valid = c->walks = 0;
    plat_mutex_unlock(&c->lock);
}

static int find_rekordbox(const compat_os *os, const char *root, compat_rekordbox *out, int max,
                          int *current, int *state, int *partial) {
    *current = -1;
    *state = COMPAT_RB_NOT_INSTALLED;
    *partial = 0;
    if (!os || !os->uninstall_entries || !root || max <= 0) return 0;
    inv_list *list = malloc(sizeof *list);
    if (!list || inv_walk(os, root, 0, max < INV_MAX ? max : INV_MAX, list)) {
        free(list);
        *state = COMPAT_RB_NO_ANSWER;
        *partial = 1;
        return 0;
    }
    *partial = !list->registry_complete;
    int n = inv_current(list, out, max, current, state);
    free(list);
    return n;
}

int compat_find_rekordbox(const compat_os *os, const char *root, compat_rekordbox *out, int max, int *current, int *state) {
    int partial;
    return find_rekordbox(os, root, out, max, current, state, &partial);
}

static int rb_stat(const char *folder, const char *name, plat_stat *st) {
    char p[COMPAT_PATH_MAX];
    memset(st, 0, sizeof *st);
    if (!name) return plat_stat_path(folder, 0, st);
    return join(p, sizeof p, folder, name) ? plat_stat_path(p, 0, st) : ENAMETOOLONG;
}

/* every listed folder and its rekordbox.exe, stat'ed without following a link: the cache's stamp.
 * 1 when it shows what the walk saw (review RC1: a change during the walk isn't kept) */
static int rb_stamp(const compat_rekordbox *list, int n, compat_rb_cache *c) {
    int same = 1;
    for (int i = 0; i < n; i++) {
        const compat_file *x = &list[i].exe;
        c->exe[i].dir_err = rb_stat(list[i].folder, NULL, &c->exe[i].dir);
        c->exe[i].err = rb_stat(list[i].folder, "rekordbox.exe", &c->exe[i].st);
        const plat_stat *s = &c->exe[i].st;
        int e = c->exe[i].err;
        if (c->exe[i].dir_err || c->exe[i].dir.kind != PLAT_DIR) same &= !x->present;   /* no folder: no exe looked at */
        else if (x->exists) same &= !e && s->kind == PLAT_FILE && s->size == x->size && s->mtime == x->mtime && plat_id_same(s->id, x->id);
        else if (x->present) same &= !e && s->kind != PLAT_FILE;
        else same &= e != 0;
    }
    return same;
}

static int rb_stamp_holds(const compat_rb_cache *c) {
    for (int i = 0; i < c->n; i++) {
        plat_stat st, dir;
        int dir_err = rb_stat(c->list[i].folder, NULL, &dir), err = rb_stat(c->list[i].folder, "rekordbox.exe", &st);
        if (dir_err != c->exe[i].dir_err || (!dir_err && (dir.kind != c->exe[i].dir.kind || !plat_id_same(dir.id, c->exe[i].dir.id))))
            return 0;   /* review RC2: the folder itself, never followed */
        if (err != c->exe[i].err) return 0;
        if (!err && (st.kind != c->exe[i].st.kind || st.size != c->exe[i].st.size || st.mtime != c->exe[i].st.mtime || !plat_id_same(st.id, c->exe[i].st.id)))
            return 0;
    }
    return 1;
}

/* an answer worth keeping: everything in it was checked for sure (C7: a pin ERROR or an exe that
 * couldn't be looked at is asked again next time) */
static int rb_sure(const compat_rekordbox *list, int n, int state) {
    if (state == COMPAT_RB_NO_ANSWER) return 0;
    for (int i = 0; i < n; i++)
        if (list[i].unknown || (list[i].exe.exists && list[i].exe.pin == COMPAT_PIN_ERROR)) return 0;
    return 1;
}

int compat_find_rekordbox_cached(compat_rb_cache *c, const compat_os *os, uint64_t gen, const char *root, compat_rekordbox *out, int max,
                                 int *current, int *state) {
    if (!c || max <= 0 || max > COMPAT_ENTRIES_MAX || !root) return compat_find_rekordbox(os, root, out, max, current, state);
    plat_mutex_lock(&c->lock);
    int n;
    if (c->valid && gen && gen == c->gen && c->max == max && !strcmp(c->root, root) && rb_stamp_holds(c)) {
        n = c->n;
        memcpy(out, c->list, (size_t)n * sizeof *out);
        *current = c->current;
        *state = c->state;
    } else {
        c->valid = 0;
        c->walks++;
        int partial;
        n = find_rekordbox(os, root, out, max, current, state, &partial);
        if (!c->list) c->list = calloc(COMPAT_ENTRIES_MAX, sizeof *c->list);
        /* review RC4: an answer from a partial enumeration isn't kept: the next call walks again */
        if (gen && !partial && c->list && strlen(root) < sizeof c->root && rb_sure(out, n, *state) && rb_stamp(out, n, c)) {
            memcpy(c->list, out, (size_t)n * sizeof *out);
            c->n = n;
            c->max = max;
            c->current = *current;
            c->state = *state;
            c->gen = gen;   /* read before the walk: a change during it moves the count past this */
            snprintf(c->root, sizeof c->root, "%s", root);
            c->valid = 1;
        }
    }
    plat_mutex_unlock(&c->lock);
    return n;
}

void compat_rb_cache_clear(compat_rb_cache *c) {
    plat_mutex_lock(&c->lock);
    c->valid = 0;
    c->walks = 0;
    plat_mutex_unlock(&c->lock);
}

#ifdef COMPAT_TEST_HOOKS
void (*compat_test_between_reads)(const char *path);
int (*compat_test_read_fails)(const char *path);
void (*compat_test_before_read)(const char *path);
int compat_test_flight_waits;
int compat_test_entries_alloc_fails;
#endif

static int listed_sha(const char *sha, const payload_sha *list, int n) {
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i], sha)) return 1;
    return 0;
}

void compat_r1_facts(const compat_os *os, const char *folder, const char *root, const payload_sha *bridges, int nbridges,
                     const payload_manifest *m, compat_r1 *out) {
    memset(out, 0, sizeof *out);
    out->live = COMPAT_LIVE_UNREADABLE;
    out->ort = COMPAT_ORT_NONE;
    char live[COMPAT_PATH_MAX], ort[COMPAT_PATH_MAX], ortdir[COMPAT_PATH_MAX];
    if (!folder || !root || !join(live, sizeof live, folder, "onnxruntime.dll") || !join(ortdir, sizeof ortdir, root, "ort")
        || !join(ort, sizeof ort, ortdir, "onnxruntime.dll"))
        return;
    compat_file f;
    if (compat_file_facts(os, live, 0, &f)) return;   /* couldn't be looked at: no answer, no offer */
    if (!f.present) out->live = COMPAT_LIVE_MISSING;
    else if (!f.exists) out->live = COMPAT_LIVE_FOREIGN;   /* a link, a reparse point or a folder in its place */
    else if (f.readable && listed_sha(f.sha256, bridges, nbridges)) out->live = COMPAT_LIVE_OURS;
    else if (f.readable && f.marker) out->live = COMPAT_LIVE_OURS_UNLISTED;
    else if (f.readable) {
        /* pinned: read again through a new handle, so classified again from what that read */
#ifdef COMPAT_TEST_HOOKS
        if (compat_test_between_reads) compat_test_between_reads(live);
#endif
        if (compat_file_facts(os, live, COMPAT_PIN, &f)) return;   /* it changed while pinned: no answer, no offer */
        if (f.readable && listed_sha(f.sha256, bridges, nbridges)) out->live = COMPAT_LIVE_OURS;
        else if (f.readable && f.marker) out->live = COMPAT_LIVE_OURS_UNLISTED;
        else out->live = f.pin == COMPAT_PIN_OK ? COMPAT_LIVE_PIONEER : f.pin == COMPAT_PIN_NO ? COMPAT_LIVE_FOREIGN : COMPAT_LIVE_UNREADABLE;
    }

    /* Pioneer's ORT: the live DLL while it is Pioneer's, else ort\'s copy (never the live bridge) */
    compat_file o;
    memset(&o, 0, sizeof o);
    if (out->live == COMPAT_LIVE_PIONEER) o = f;
    else if (out->live == COMPAT_LIVE_OURS || out->live == COMPAT_LIVE_OURS_UNLISTED) {
        /* ort\ that can't be looked at (refused, or changed while pinned) is no answer: no offer
         * on its account */
        if (compat_file_facts(os, ort, COMPAT_PIN, &o)) {
            out->ort = COMPAT_ORT_NONE;
            out->offer_restore = out->live == COMPAT_LIVE_OURS_UNLISTED;
            return;
        }
        out->ort_looked = 1;
        out->ort_copy = o.exists;
    }
    out->offer_restore = out->live == COMPAT_LIVE_MISSING || out->live == COMPAT_LIVE_FOREIGN || out->live == COMPAT_LIVE_OURS_UNLISTED
                      || (out->live == COMPAT_LIVE_OURS && !out->ort_copy);
    if (o.exists && o.readable) {
        memcpy(out->ort_sha256, o.sha256, sizeof out->ort_sha256);
        if (o.pin == COMPAT_PIN_ERROR) out->ort = COMPAT_ORT_NONE;   /* no answer: not "not the original" */
        else if (o.pin != COMPAT_PIN_OK) out->ort = COMPAT_ORT_NOT_ORIGINAL;
        else if (!m || !m->has_windows || !listed_sha(o.sha256, m->win_ort_sha256, m->n_win_ort_sha256)) out->ort = COMPAT_ORT_UNLISTED;
        else out->ort = COMPAT_ORT_OK;
    }
}

void compat_procs_classify(const compat_proc *list, int n, const char *folder, compat_procs *out) {
    memset(out, 0, sizeof *out);
    char here[COMPAT_PATH_MAX] = "";
    if (folder) join(here, sizeof here, folder, "rekordbox.exe");
    for (int i = 0; i < n; i++) {
        const compat_proc *p = &list[i];
        if (equal_ci(p->name, "rekordbox.exe")) {
            out->running = 1;
            /* a path that can't be read (another account's) counts as this folder's */
            if (!*p->path || (*here && equal_ci(p->path, here))) out->running_here = 1;
            if (!out->machine) out->machine = p->machine;
        } else if (equal_ci(p->name, "Upmgr rekordbox.exe") || equal_ci(p->name, "Uninstall rekordbox.exe")) {
            /* open: VM question (app.md:1100): the installer and NSIS's temp uninstaller aren't
             * matched yet; the final rule matches on path or command line plus the signature,
             * not only ~nsu*.tmp (review C9) */
            out->updating = 1;
        }
    }
}

int compat_processes(const compat_os *os, const char *folder, compat_procs *out) {
    memset(out, 0, sizeof *out);
    if (!os || !os->processes) return -1;
    compat_proc *list = malloc(COMPAT_PROCS_MAX * sizeof *list);
    if (!list) return -1;
    int n = os->processes(os->ctx, list, COMPAT_PROCS_MAX);
    if (n >= 0) compat_procs_classify(list, n, folder, out);
    free(list);
    return n < 0 ? -1 : 0;
}
