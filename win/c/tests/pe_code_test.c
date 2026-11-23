/* Synthetic PE files only. The optional output directory is for the Python release-check test. */
#include "check.h"
#include "../pe_code.h"
#include "../../../common/plat.h"

enum {
    PE = 0x80, OPT = PE + 24, CHECKSUM = OPT + 64,
    HEADERS = 0x200, RAW = 0x200, CERT = 0x620, CERT_SIZE = 32,
    FIXTURE_CAP = CERT + PE_CODE_CERT_MAX + 8
};

typedef struct {
    unsigned char bytes[FIXTURE_CAP];
    size_t size;
    uint64_t fail_at;
} fixture;

int checks, failures;

static void put16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static int read_fixture(void *ctx, uint64_t off, void *buf, size_t n) {
    const fixture *f = ctx;
    if (off > f->size || n > f->size - off || (off <= f->fail_at && n > f->fail_at - off)) return -1;
    memcpy(buf, f->bytes + off, n);
    return 0;
}

static unsigned make_pe(fixture *f, int plus) {
    memset(f, 0, sizeof *f);
    f->size = CERT + CERT_SIZE;
    f->fail_at = UINT64_MAX;
    unsigned char *b = f->bytes;
    memcpy(b, "MZ", 2);
    put32(b + 0x3c, PE);
    memcpy(b + PE, "PE\0\0", 4);
    put16(b + PE + 4, plus ? 0x8664 : 0x14c);
    put16(b + PE + 6, 2);
    unsigned optional_size = plus ? 240 : 224;
    put16(b + PE + 20, optional_size);
    put16(b + PE + 22, 0x2002);
    put16(b + OPT, plus ? 0x20b : 0x10b);
    put32(b + OPT + 32, 0x1000);  /* section alignment */
    put32(b + OPT + 36, RAW);     /* file alignment */
    put32(b + OPT + 56, 0x3000);  /* image size */
    put32(b + OPT + 60, HEADERS);
    put32(b + CHECKSUM, 0x12345678);
    unsigned dirs = OPT + (plus ? 112 : 96);
    put32(b + dirs - 4, 16);
    unsigned cert_dir = dirs + 4 * 8;
    put32(b + cert_dir, CERT);
    put32(b + cert_dir + 4, CERT_SIZE);
    for (unsigned i = 0; i < 2; i++) {
        unsigned char *s = b + OPT + optional_size + 40 * i;
        memcpy(s, i ? ".rdata" : ".text", i ? 6 : 5);
        put32(s + 8, RAW);
        put32(s + 12, 0x1000 * (i + 1));
        put32(s + 16, RAW);
        put32(s + 20, HEADERS + RAW * i);
    }
    for (unsigned i = HEADERS; i < CERT + CERT_SIZE; i++) b[i] = (unsigned char)(i ^ (i >> 8));
    for (unsigned i = CERT; i < CERT + CERT_SIZE; i += 16) {
        put32(b + i, 16);
        put16(b + i + 4, 0x200);
        put16(b + i + 6, 2);
    }
    return cert_dir;
}

/* Compute the digest directly from this fixture's known ranges. */
static void expected(const fixture *f, unsigned cert_dir, payload_sha out) {
    unsigned char joined[CERT - 12], hash[32];
    memcpy(joined, f->bytes, CHECKSUM);
    memcpy(joined + CHECKSUM, f->bytes + CHECKSUM + 4, cert_dir - CHECKSUM - 4);
    memcpy(joined + cert_dir - 4, f->bytes + cert_dir + 8, CERT - cert_dir - 8);
    if (plat_sha256(joined, sizeof joined, hash)) abort();
    for (size_t i = 0; i < sizeof hash; i++) snprintf(out + 2 * i, 3, "%02x", (unsigned)hash[i]);
}

static void export_pe(const char *dir, const char *name, const fixture *f, const char *digest) {
    if (!dir) return;
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s.dll", dir, name);
    if (n < 0 || (size_t)n >= sizeof path) abort();
    FILE *out = fopen(path, "wb");
    if (!out || fwrite(f->bytes, 1, f->size, out) != f->size || fclose(out)) abort();
    printf("%s %s\n", name, digest);
}

static void test_digests(int plus, const char *dir) {
    fixture f;
    payload_sha want, digest;
    unsigned cert_dir = make_pe(&f, plus);
    expected(&f, cert_dir, want);
    CHECK(!pe_code_sha256(read_fixture, &f, f.size, digest) && !strcmp(digest, want),
          "%s hashes exactly the three retained ranges including overlay", plus ? "PE32+" : "PE32");
    export_pe(dir, plus ? "pe32-plus" : "pe32", &f, digest);

    put32(f.bytes + CHECKSUM, 0x87654321);
    f.bytes[CERT + 8] ^= 1;
    f.size -= 16;
    put32(f.bytes + cert_dir + 4, 16);
    CHECK(!pe_code_sha256(read_fixture, &f, f.size, digest) && !strcmp(digest, want),
          "a different checksum, certificate entry and certificate contents preserve the digest");
    if (plus) export_pe(dir, "resigned", &f, digest);

    f.bytes[HEADERS] ^= 1;
    expected(&f, cert_dir, want);
    CHECK(!pe_code_sha256(read_fixture, &f, f.size, digest) && !strcmp(digest, want),
          "a changed code byte gives its independently calculated digest");
    if (plus) export_pe(dir, "code-change", &f, digest);

    memset(f.bytes + cert_dir, 0, 8);
    f.size = CERT;
    CHECK(!pe_code_sha256(read_fixture, &f, f.size, digest) && !strcmp(digest, want),
          "a zero certificate directory skips only the two header ranges");
    if (plus) export_pe(dir, "unsigned", &f, digest);
}

static void test_bad_pe(void) {
    fixture f;
    payload_sha digest;
    enum { DIR = OPT + 112 + 4 * 8, SECTION = OPT + 240 };
    static const struct { unsigned off; uint32_t value; unsigned width; const char *why; } bad[] = {
        {0, 0, 2, "missing DOS magic"},
        {0x3c, UINT32_MAX, 4, "out-of-range PE offset"},
        {PE, 0, 4, "missing PE magic"},
        {PE + 6, 0, 2, "no sections"},
        {PE + 6, 97, 2, "too many sections"},
        {PE + 20, 150, 2, "optional header cuts off the certificate directory"},
        {OPT, 0x107, 2, "non-image optional header"},
        {OPT + 108, 4, 4, "certificate directory not declared"},
        {OPT + 108, 17, 4, "directory count extends beyond optional header"},
        {OPT + 60, SECTION + 40, 4, "section table extends beyond SizeOfHeaders"},
        {OPT + 60, CERT + CERT_SIZE + 1, 4, "SizeOfHeaders extends past the file"},
        {SECTION + 20, HEADERS - 8, 4, "raw data overlaps headers"},
        {SECTION + 40 + 20, HEADERS, 4, "raw sections overlap"},
        {SECTION + 40 + 16, CERT - (HEADERS + RAW) + 8, 4, "certificate table overlaps raw data"},
        {SECTION + 40 + 16, UINT32_MAX, 4, "raw size extends past the file"},
        {DIR, 0, 4, "zero certificate offset with nonzero size"},
        {DIR + 4, 0, 4, "nonzero certificate offset with zero size"},
        {CERT, 0, 4, "certificate record smaller than its header"},
        {CERT, CERT_SIZE + 1, 4, "certificate record extends past its table"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        make_pe(&f, 1);
        if (bad[i].width == 2) put16(f.bytes + bad[i].off, (uint16_t)bad[i].value);
        else put32(f.bytes + bad[i].off, bad[i].value);
        strcpy(digest, "stale");
        CHECK(pe_code_sha256(read_fixture, &f, f.size, digest) == -1 && !digest[0], "%s is refused", bad[i].why);
    }
    make_pe(&f, 1);
    memmove(f.bytes + 8, f.bytes + PE, SECTION + 2 * 40 - PE);
    put32(f.bytes + 0x3c, 8);
    CHECK(pe_code_sha256(read_fixture, &f, f.size, digest) == -1, "PE header inside DOS header is refused");

    make_pe(&f, 1);
    memmove(f.bytes + CERT - 1, f.bytes + CERT, CERT_SIZE);
    put32(f.bytes + DIR, CERT - 1);
    f.size--;
    CHECK(pe_code_sha256(read_fixture, &f, f.size, digest) == -1, "unaligned certificate offset is refused");

    make_pe(&f, 1);
    put32(f.bytes + f.size, 8);
    f.size += 8;
    CHECK(pe_code_sha256(read_fixture, &f, f.size, digest) == -1, "certificate table ending before EOF is refused");
    f.size = CERT + 16;
    CHECK(pe_code_sha256(read_fixture, &f, f.size, digest) == -1, "certificate table ending after EOF is refused");

    make_pe(&f, 1);
    f.size = CERT + PE_CODE_CERT_MAX;
    put32(f.bytes + DIR + 4, PE_CODE_CERT_MAX);
    put32(f.bytes + CERT, PE_CODE_CERT_MAX);
    CHECK(pe_code_sha256(read_fixture, &f, f.size, digest) == 0, "certificate table at the cap is accepted");
    f.size += 8;
    put32(f.bytes + DIR + 4, PE_CODE_CERT_MAX + 8);
    put32(f.bytes + CERT, PE_CODE_CERT_MAX + 8);
    CHECK(pe_code_sha256(read_fixture, &f, f.size, digest) == -1, "certificate table above the cap is refused");

    make_pe(&f, 1);
    f.fail_at = HEADERS + 9;
    CHECK(pe_code_sha256(read_fixture, &f, f.size, digest) == -1 && !digest[0], "a failed hash read leaves no digest");
}

int main(int argc, char **argv) {
    if (argc > 2) return 2;
    test_digests(0, argv[1]);
    test_digests(1, argv[1]);
    test_bad_pe();
    printf("PE code: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
