/* The app zip's reader (unzip.h): the zip structure, the name rules and our own RFC 1951 inflate
 * (canonical Huffman decoding as RFC 1951 §3.2.2 defines it; written here). */
#include "plat.h"

#include "unzip.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ CRC-32 */

static uint32_t crc_table[256];
static plat_once_t crc_once = PLAT_ONCE_INIT;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

uint32_t unzip_crc32(uint32_t crc, const void *p, size_t n) {
    plat_once(&crc_once, crc_init);
    const unsigned char *b = p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ b[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------ inflate (RFC 1951) */

enum { MAXBITS = 15, MAXLCODES = 286, MAXDCODES = 30, FIXLCODES = 288 };

typedef struct {
    const unsigned char *in;
    size_t n, pos;
    uint32_t bitbuf;
    int bitcnt;
    unsigned char *out;
    size_t out_n, outpos;
} inf;

/* the next `need` bits (need <= 13), least significant first: 0, or -1 when the input ends */
static int bits(inf *s, int need, unsigned *v) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos == s->n) return -1;
        val |= (uint32_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    *v = (unsigned)(val & ((1u << need) - 1));
    return 0;
}

static int stored(inf *s) {
    s->bitbuf = 0;   /* the rest of the current byte is padding */
    s->bitcnt = 0;
    if (s->n - s->pos < 4) return -1;
    unsigned len = s->in[s->pos] | (unsigned)s->in[s->pos + 1] << 8;
    unsigned nlen = s->in[s->pos + 2] | (unsigned)s->in[s->pos + 3] << 8;
    s->pos += 4;
    if (len != (~nlen & 0xffffu)) return -1;
    if (s->n - s->pos < len || s->out_n - s->outpos < len) return -1;
    if (len) memcpy(s->out + s->outpos, s->in + s->pos, len);
    s->pos += len;
    s->outpos += len;
    return 0;
}

typedef struct {
    short count[MAXBITS + 1];   /* how many codes of each length */
    short symbol[FIXLCODES];    /* the symbols in canonical order */
} huff;

/* the canonical code of these lengths: 0 complete, > 0 incomplete, < 0 over-subscribed */
static int construct(huff *h, const short *length, int n) {
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;   /* no codes: complete, but decode() finds none */
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

/* one symbol: >= 0, or -1 (the input ended, or no code matches) */
static int decode(inf *s, const huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        unsigned b;
        if (bits(s, 1, &b)) return -1;
        code |= (int)b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                         4097, 6145, 8193, 12289, 16385, 24577};
static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(inf *s, const huff *lencode, const huff *distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outpos == s->out_n) return -1;
            s->out[s->outpos++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            unsigned e;
            if (bits(s, lext[sym], &e)) return -1;
            size_t len = (size_t)lbase[sym] + e;
            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            if (bits(s, dext[dsym], &e)) return -1;
            size_t dist = (size_t)dbase[dsym] + e;
            if (dist > s->outpos || s->out_n - s->outpos < len) return -1;
            unsigned char *o = s->out + s->outpos;
            for (size_t i = 0; i < len; i++) o[i] = *(o + i - dist);   /* the copy may overlap itself */
            s->outpos += len;
        }
    }
}

static huff fix_len, fix_dist;
static plat_once_t fix_once = PLAT_ONCE_INIT;

static void fix_init(void) {
    short lengths[FIXLCODES];
    int sym = 0;
    for (; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
    construct(&fix_len, lengths, FIXLCODES);
    for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
    construct(&fix_dist, lengths, MAXDCODES);
}

static int fixed(inf *s) {
    plat_once(&fix_once, fix_init);
    return codes(s, &fix_len, &fix_dist);
}

static int dynamic(inf *s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[MAXLCODES + MAXDCODES];
    huff lencode, distcode;
    unsigned v;
    if (bits(s, 5, &v)) return -1;
    int nlen = (int)v + 257;
    if (bits(s, 5, &v)) return -1;
    int ndist = (int)v + 1;
    if (bits(s, 4, &v)) return -1;
    int ncode = (int)v + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -1;
    int index;
    for (index = 0; index < ncode; index++) {
        if (bits(s, 3, &v)) return -1;
        lengths[order[index]] = (short)v;
    }
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (construct(&lencode, lengths, 19) != 0) return -1;   /* the code length code must be complete */
    for (index = 0; index < nlen + ndist;) {
        int sym = decode(s, &lencode);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[index++] = (short)sym;
        } else {
            short len = 0;
            int repeat;
            if (sym == 16) {
                if (index == 0) return -1;   /* nothing to repeat */
                len = lengths[index - 1];
                if (bits(s, 2, &v)) return -1;
                repeat = 3 + (int)v;
            } else if (sym == 17) {
                if (bits(s, 3, &v)) return -1;
                repeat = 3 + (int)v;
            } else {
                if (bits(s, 7, &v)) return -1;
                repeat = 11 + (int)v;
            }
            if (index + repeat > nlen + ndist) return -1;
            while (repeat--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) return -1;   /* no end-of-block code */
    /* an incomplete code is allowed only as a single code of one bit */
    int err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -1;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -1;
    return codes(s, &lencode, &distcode);
}

int unzip_inflate(const unsigned char *in, size_t n, unsigned char *out, size_t out_n, size_t *used) {
    inf s = {in, n, 0, 0, 0, out, out_n, 0};
    unsigned last, type;
    do {
        if (bits(&s, 1, &last) || bits(&s, 2, &type)) return -1;
        int r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (r) return -1;
    } while (!last);
    if (s.outpos != out_n) return -1;
    if (used) *used = s.pos;
    return 0;
}

/* ------------------------------------------------------------------ names */

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/* a Windows device name: CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$, CONOUT$, with any extension */
static int device_name(const char *c, size_t n) {
    size_t base = 0;
    while (base < n && c[base] != '.') base++;
    while (base && c[base - 1] == ' ') base--;
    static const char *const fixed_names[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    for (size_t i = 0; i < sizeof fixed_names / sizeof fixed_names[0]; i++) {
        size_t k = strlen(fixed_names[i]), j = 0;
        if (k != base) continue;
        while (j < k && upper((unsigned char)c[j]) == fixed_names[i][j]) j++;
        if (j == k) return 1;
    }
    if (base == 4 && c[3] >= '0' && c[3] <= '9') {
        int a = upper((unsigned char)c[0]), b = upper((unsigned char)c[1]), d = upper((unsigned char)c[2]);
        if ((a == 'C' && b == 'O' && d == 'M') || (a == 'L' && b == 'P' && d == 'T')) return 1;
    }
    return 0;
}

int unzip_name_ok(const char *name, size_t n) {
    if (n == 0 || n > UNZIP_NAME_MAX || name[0] == '/') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7e || strchr("\\:*?\"<>|", c)) return 0;
    }
    size_t end = name[n - 1] == '/' ? n - 1 : n;   /* a folder's entry ends in '/' */
    if (end == 0) return 0;
    int depth = 0;
    for (size_t at = 0; at < end;) {
        size_t k = at;
        while (k < end && name[k] != '/') k++;
        size_t len = k - at;
        const char *c = name + at;
        if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.')) return 0;
        if (c[len - 1] == ' ' || c[len - 1] == '.') return 0;
        if (device_name(c, len)) return 0;
        if (++depth > UNZIP_DEPTH_MAX) return 0;
        at = k + 1;
    }
    return 1;
}

/* the same name ignoring ASCII case and a folder's trailing '/' */
static int same_name(const char *a, size_t an, const char *b, size_t bn) {
    if (an && a[an - 1] == '/') an--;
    if (bn && b[bn - 1] == '/') bn--;
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (upper((unsigned char)a[i]) != upper((unsigned char)b[i])) return 0;
    return 1;
}

/* whether file a (no trailing '/') is a folder of b's path, ignoring ASCII case: "a" of "A/b" */
static int file_is_folder_of(const char *a, size_t an, const char *b, size_t bn) {
    if (!an || a[an - 1] == '/' || bn <= an || b[an] != '/') return 0;
    for (size_t i = 0; i < an; i++)
        if (upper((unsigned char)a[i]) != upper((unsigned char)b[i])) return 0;
    return 1;
}

/* ------------------------------------------------------------------ the zip */

static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct {
    const char *name;
    size_t name_n;
    uint32_t method, crc, csize, usize;
    size_t data;   /* where its bytes start */
    size_t local;  /* where its local header starts */
} entry;

/* the entries by where their local header starts */
static int by_local(const void *a, const void *b) {
    size_t x = ((const entry *)a)->local, y = ((const entry *)b)->local;
    return x < y ? -1 : x > y;
}

/* whether two entries' bytes, local header to the end of the data, overlap anywhere (one zip byte
 * read as two files: a zip bomb, or two names for one file) */
static int overlapping(const entry *list, size_t n) {
    if (n < 2) return 0;
    entry *s = malloc(n * sizeof *s);
    if (!s) return 1;
    memcpy(s, list, n * sizeof *s);
    qsort(s, n, sizeof *s, by_local);
    int bad = 0;
    for (size_t i = 1; i < n && !bad; i++) bad = s[i].local < s[i - 1].data + s[i - 1].csize;
    free(s);
    return bad;
}

/* the central directory into *out (malloc'd), *count entries */
static int read_directory(const unsigned char *z, size_t n, entry **out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (n < 22) return UNZIP_NOT_ZIP;
    size_t eocd = SIZE_MAX;
    size_t low = n - 22 > 65535 ? n - 22 - 65535 : 0;
    for (size_t at = n - 22 + 1; at-- > low;) {
        if (rd32(z + at) == 0x06054b50u && at + 22 + rd16(z + at + 20) == n) {
            eocd = at;
            break;
        }
    }
    if (eocd == SIZE_MAX) return UNZIP_NOT_ZIP;
    const unsigned char *e = z + eocd;
    uint32_t entries = rd16(e + 10), cd_size = rd32(e + 12), cd_off = rd32(e + 16);
    if (rd16(e + 4) || rd16(e + 6) || rd16(e + 8) != entries) return UNZIP_NOT_ZIP;   /* one disk */
    if (entries == 0xffff || cd_size == 0xffffffffu || cd_off == 0xffffffffu) return UNZIP_NOT_ZIP;   /* ZIP64 */
    if (entries > UNZIP_ENTRIES_MAX || (size_t)cd_off > eocd || (size_t)cd_size > eocd - cd_off) return UNZIP_NOT_ZIP;
    entry *list = calloc(entries ? entries : 1, sizeof *list);
    if (!list) return UNZIP_NOT_ZIP;
    size_t at = cd_off, end = (size_t)cd_off + cd_size;
    uint64_t total = 0;
    int r = UNZIP_OK;
    for (uint32_t i = 0; i < entries && r == UNZIP_OK; i++) {
        if (end - at < 46 || rd32(z + at) != 0x02014b50u) { r = UNZIP_NOT_ZIP; break; }
        const unsigned char *c = z + at;
        uint32_t made_by = rd16(c + 4), flags = rd16(c + 8), method = rd16(c + 10);
        uint32_t crc = rd32(c + 16), csize = rd32(c + 20), usize = rd32(c + 24);
        size_t nl = rd16(c + 28), xl = rd16(c + 30), cl = rd16(c + 32);
        uint32_t disk = rd16(c + 34), ext = rd32(c + 38), local = rd32(c + 42);
        if (end - at - 46 < nl + xl + cl) { r = UNZIP_NOT_ZIP; break; }
        const char *name = (const char *)c + 46;
        /* encrypted (0, 6), masked headers (13); ZIP64 sizes; another disk; methods but stored and deflate */
        if ((flags & (1u | 1u << 6 | 1u << 13)) || csize == 0xffffffffu || usize == 0xffffffffu || local == 0xffffffffu || disk ||
            (method != 0 && method != 8) || usize > UNZIP_FILE_MAX || (method == 0 && csize != usize)) {
            r = UNZIP_NOT_ZIP;
            break;
        }
        if (!unzip_name_ok(name, nl) || ((made_by >> 8) == 3 && ((ext >> 16) & 0170000u) == 0120000u) ||
            (name[nl - 1] == '/' && usize != 0)) {
            r = UNZIP_BAD_NAME;
            break;
        }
        for (uint32_t j = 0; j < i; j++)
            if (same_name(list[j].name, list[j].name_n, name, nl) || file_is_folder_of(list[j].name, list[j].name_n, name, nl) ||
                file_is_folder_of(name, nl, list[j].name, list[j].name_n))
                r = UNZIP_BAD_NAME;
        if (r) break;
        total += usize;
        if (total > UNZIP_TOTAL_MAX) { r = UNZIP_NOT_ZIP; break; }
        /* the local header: the same name and method; its data before the central directory */
        if (local > cd_off || cd_off - local < 30 || rd32(z + local) != 0x04034b50u) { r = UNZIP_NOT_ZIP; break; }
        const unsigned char *l = z + local;
        size_t lnl = rd16(l + 26), lxl = rd16(l + 28);
        size_t data = (size_t)local + 30 + lnl + lxl;
        if (rd16(l + 8) != method || lnl != nl || data > cd_off || cd_off - data < csize || memcmp(l + 30, name, nl)) {
            r = UNZIP_NOT_ZIP;
            break;
        }
        list[i] = (entry){name, nl, method, crc, csize, usize, data, local};
        at += 46 + nl + xl + cl;
    }
    if (r == UNZIP_OK && at != end) r = UNZIP_NOT_ZIP;
    if (r == UNZIP_OK && overlapping(list, entries)) r = UNZIP_NOT_ZIP;
    if (r) {
        free(list);
        return r;
    }
    *out = list;
    *count = entries;
    return UNZIP_OK;
}

/* opens (making it when missing) folder `name` in d */
static int open_or_make(plat_dir d, const char *name, plat_dir *out) {
    int e = plat_dir_open_at(d, name, out);
    if (e == ENOENT) {
        e = plat_mkdir_at(d, name);
        if (e && e != EEXIST) return e;
        e = plat_dir_open_at(d, name, out);
    }
    return e;
}

/* one entry's bytes, already checked, into its file under root (its folders made) */
static int write_entry(plat_dir root, const entry *en, const unsigned char *bytes, int *err) {
    char name[UNZIP_NAME_MAX + 1];
    memcpy(name, en->name, en->name_n);
    name[en->name_n] = 0;
    int is_dir = name[en->name_n - 1] == '/';
    if (is_dir) name[en->name_n - 1] = 0;
    plat_dir d = root, next;
    int owned = 0, e = 0;
    char *comp = name;
    for (char *slash; (slash = strchr(comp, '/')) != NULL; comp = slash + 1) {
        *slash = 0;
        e = open_or_make(d, comp, &next);
        if (owned) plat_dir_close(d);
        if (e) {
            *err = e;
            return UNZIP_WRITE;
        }
        d = next;
        owned = 1;
    }
    if (is_dir) {
        e = open_or_make(d, comp, &next);
        if (!e) plat_dir_close(next);
    } else {
        plat_file f;
        e = plat_file_create_at(d, comp, 1, &f);
        if (!e) {
            void *addr = NULL;
            if (en->usize && !(e = plat_map_rw(f, (int64_t)en->usize, &addr))) {
                memcpy(addr, bytes, en->usize);
                e = plat_map_flush(addr, (int64_t)en->usize);   /* the view written (FlushViewOfFile) */
                plat_unmap(addr, (int64_t)en->usize);
            }
            /* to the disk before `current` can name this folder (fs_write.c's pattern: the view
             * flushed, then the file) */
            if (!e) e = plat_fsync(f);
            plat_file_close(f);
        }
    }
    if (owned) plat_dir_close(d);
    if (e) {
        *err = e;
        return UNZIP_WRITE;
    }
    return UNZIP_OK;
}

/* one entry's bytes: *bytes (into *buf, malloc'd, for a deflated one), inflated and CRC-checked */
static int entry_bytes(const unsigned char *z, const entry *en, unsigned char **buf, const unsigned char **bytes) {
    *buf = NULL;
    *bytes = z + en->data;
    if (en->method == 8) {
        size_t used = 0;
        if (!(*buf = malloc(en->usize ? en->usize : 1))) return UNZIP_WRITE;
        if (unzip_inflate(z + en->data, en->csize, *buf, en->usize, &used) || used != en->csize) return UNZIP_DAMAGED;
        *bytes = *buf;
    }
    return unzip_crc32(0, *bytes, en->usize) == en->crc ? UNZIP_OK : UNZIP_DAMAGED;
}

int unzip_check(const void *data, size_t n) {
    entry *list;
    size_t count;
    int r = read_directory(data, n, &list, &count);
    for (size_t i = 0; i < count && r == UNZIP_OK; i++) {
        unsigned char *buf;
        const unsigned char *bytes;
        r = entry_bytes(data, &list[i], &buf, &bytes);
        free(buf);
    }
    free(list);
    return r;
}

int unzip_extract_at(const void *data, size_t n, plat_dir root, int *err) {
    int dummy = 0;
    if (!err) err = &dummy;
    *err = 0;
    /* every entry checked before anything is written */
    int r = unzip_check(data, n);
    if (r) {
        if (r == UNZIP_WRITE) *err = ENOMEM;
        return r;
    }
    const unsigned char *z = data;
    entry *list;
    size_t count;
    if ((r = read_directory(z, n, &list, &count))) return r;
    for (size_t i = 0; i < count && r == UNZIP_OK; i++) {
        unsigned char *buf;
        const unsigned char *bytes;
        r = entry_bytes(z, &list[i], &buf, &bytes);
        if (r == UNZIP_WRITE) *err = ENOMEM;
        if (r == UNZIP_OK) r = write_entry(root, &list[i], bytes, err);
        free(buf);
    }
    free(list);
    return r;
}

int unzip_extract(const void *data, size_t n, const char *dest, int *err) {
    plat_dir root;
    int e = plat_dir_open(dest, &root);
    if (e) {   /* the zip is checked first all the same: its verdict before the folder's */
        int c = unzip_check(data, n);
        if (err) *err = c ? (c == UNZIP_WRITE ? ENOMEM : 0) : e;
        return c ? c : UNZIP_WRITE;
    }
    int r = unzip_extract_at(data, n, root, err);
    plat_dir_close(root);
    return r;
}
