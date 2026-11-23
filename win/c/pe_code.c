/* See pe_code.h. Keep the exclusions and validation in sync with scripts/win-ort-code.py. */
#include "pe_code.h"
#include "../../common/plat.h"

#include <string.h>

enum {
    DOS_PE_OFFSET = 0x3c, DOS_SIZE = 64, NT_SIZE = 24, SECTION_SIZE = 40, MAX_SECTIONS = 96,
    OPT_CHECKSUM = 64, OPT_HEADERS_SIZE = 60, PE32_DIRECTORIES = 96, PE32_PLUS_DIRECTORIES = 112,
    DIRECTORY_SIZE = 8, CERT_DIRECTORY = 4, CERT_HEADER_SIZE = 8
};

typedef struct {
    compat_read_fn read;
    void *ctx;
    uint64_t size;
} image;

typedef struct { uint64_t start, end; } range;

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16; }

static int read_at(const image *f, uint64_t at, void *buf, size_t n) {
    return at > f->size || n > f->size - at || f->read(f->ctx, at, buf, n);
}

static int sections_fit(const image *f, uint64_t table, unsigned count, uint64_t headers, uint64_t limit) {
    range raw[MAX_SECTIONS];
    unsigned used = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned char entry[SECTION_SIZE];
        if (read_at(f, table + i * SECTION_SIZE, entry, sizeof entry)) return 0;
        uint32_t n = le32(entry + 16);
        if (!n) continue;
        uint64_t start = le32(entry + 20), end = start + n;
        if (start < headers || end > limit) return 0;
        for (unsigned j = 0; j < used; j++)
            if (start < raw[j].end && raw[j].start < end) return 0;
        raw[used++] = (range){start, end};
    }
    return 1;
}

/* Validate each WIN_CERTIFICATE's framing. The contents of each record are opaque. */
static int certificates_fit(const image *f, uint64_t at) {
    while (at < f->size) {
        unsigned char header[CERT_HEADER_SIZE];
        if (read_at(f, at, header, sizeof header)) return 0;
        uint64_t length = le32(header), aligned = (length + 7) & ~(uint64_t)7;
        if (length < sizeof header || aligned > f->size - at) return 0;
        at += aligned;
    }
    return 1;
}

static int retained_ranges(const image *f, range keep[3]) {
    unsigned char dos[DOS_SIZE], nt[NT_SIZE], opt[PE32_PLUS_DIRECTORIES + (CERT_DIRECTORY + 1) * DIRECTORY_SIZE];
    if (read_at(f, 0, dos, sizeof dos) || memcmp(dos, "MZ", 2)) return 0;
    uint64_t pe = le32(dos + DOS_PE_OFFSET);
    if (pe < sizeof dos || read_at(f, pe, nt, sizeof nt) || memcmp(nt, "PE\0\0", 4)) return 0;
    unsigned count = le16(nt + 6), opt_size = le16(nt + 20);
    uint64_t optional = pe + sizeof nt;
    if (!count || count > MAX_SECTIONS || opt_size < 2 || read_at(f, optional, opt, 2)) return 0;

    unsigned directories;
    switch (le16(opt)) {
    case 0x10b: directories = PE32_DIRECTORIES; break;
    case 0x20b: directories = PE32_PLUS_DIRECTORIES; break;
    default: return 0;
    }
    unsigned cert_entry = directories + CERT_DIRECTORY * DIRECTORY_SIZE;
    unsigned required = cert_entry + DIRECTORY_SIZE;
    if (opt_size < required || read_at(f, optional, opt, required)) return 0;
    uint32_t dir_count = le32(opt + directories - 4);
    if (dir_count <= CERT_DIRECTORY || dir_count > (opt_size - directories) / DIRECTORY_SIZE) return 0;

    uint64_t sections = optional + opt_size;
    uint64_t headers = le32(opt + OPT_HEADERS_SIZE);
    if (sections + count * SECTION_SIZE > headers || headers > f->size) return 0;
    uint64_t cert_at = le32(opt + cert_entry), cert_size = le32(opt + cert_entry + 4);
    uint64_t limit = f->size;
    if (cert_at || cert_size) {
        if (cert_at < headers || (cert_at & 7) || !cert_size || cert_size > PE_CODE_CERT_MAX || cert_at + cert_size != f->size) return 0;
        limit = cert_at;
    }
    if (!sections_fit(f, sections, count, headers, limit) || (cert_at && !certificates_fit(f, cert_at))) return 0;

    uint64_t checksum = optional + OPT_CHECKSUM, entry = optional + cert_entry;
    keep[0] = (range){0, checksum};
    keep[1] = (range){checksum + 4, entry};
    keep[2] = (range){entry + DIRECTORY_SIZE, limit};
    return 1;
}

static int hash_range(const image *f, range part, plat_sha256_ctx *hash) {
    unsigned char buf[16 * 1024];
    for (uint64_t at = part.start; at < part.end;) {
        size_t n = part.end - at < sizeof buf ? (size_t)(part.end - at) : sizeof buf;
        if (read_at(f, at, buf, n)) return -1;
        plat_sha256_update(hash, buf, n);
        at += n;
    }
    return 0;
}

int pe_code_sha256(compat_read_fn read, void *ctx, uint64_t size, payload_sha out) {
    if (!out) return -1;
    out[0] = 0;
    image f = {read, ctx, size};
    range keep[3];
    plat_sha256_ctx hash;
    if (!read || !retained_ranges(&f, keep) || plat_sha256_init(&hash)) return -1;

    int error = hash_range(&f, keep[0], &hash) || hash_range(&f, keep[1], &hash) || hash_range(&f, keep[2], &hash);
    unsigned char digest[32];
    if (plat_sha256_final(&hash, digest) || error) return -1;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof digest; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
    return 0;
}
