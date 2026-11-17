/* The fuzz driver for win/c's parsers (app.md §5.1, §7: "the DER and manifest parsers are fuzzed
 * on the Mac"), run under ASan+UBSan by run.sh. No libFuzzer (Xcode's clang has none): seeded
 * mutations of good inputs, as the core's fuzz driver (core/tests/fuzz.c). Besides "no crash",
 * each parser's own property:
 *  - sig_der_to_rs accepts only canonical DER: what it accepts re-encodes to the same bytes;
 *  - sig_spki_p256 accepts only the exact P-256 header;
 *  - sig_base64_decode accepts only canonical base64: what it accepts re-encodes to the same text;
 *  - payload_decode accepts only what payload_plain_keys accepts, and decides the same twice;
 *  - compat_pe_read: every read it asks for is bounds-checked by the callback, and it never fails
 *    on a short read; what it reports is printable ASCII (review C16);
 *  - the update records (pending, shown-version, current) write back the same bytes they accepted;
 *  - unzip_check and unzip_inflate on mutated zips (every gen-zips.py output) and deflate streams:
 *    no read or write outside their buffers (ASan); a zip unzip_check accepts also unpacks;
 *  - update_range_start, unzip_name_ok, stub_args and update_quote_arg (read back as Windows reads
 *    a command line) keep their properties on mutated input.
 *
 *   winc-fuzz [ITERATIONS [SEED [PE [ZIPS]]]]    SEED 0: the default; PE: an exe with a version
 *                                                resource; ZIPS: gen-zips.py's folder */
#include "../compat.h"
#include "../payload.h"
#include "../stub.h"
#include "../unzip.h"
#include "../update.h"
#include "sig_mac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t rng;
static uint32_t rnd(void) {   /* xorshift64* */
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1DULL) >> 32);
}

static int failures;

/* CommandLineToArgvW's reading of the arguments (the UCRT's rules), as update_rules_test.c's */
static int argv_parse(const char *s, char out[][128], int max) {
    int argc = 0;
    size_t i = 0;
    for (;;) {
        while (s[i] == ' ' || s[i] == '\t') i++;
        if (!s[i] || argc == max) break;
        size_t k = 0;
        int quoted = 0;
        char *a = out[argc++];
        while (s[i] && (quoted || (s[i] != ' ' && s[i] != '\t'))) {
            size_t bs = 0;
            while (s[i] == '\\') bs++, i++;
            if (s[i] == '"') {
                for (size_t j = 0; j < bs / 2; j++) a[k++] = '\\';
                if (bs % 2) {
                    a[k++] = '"';
                    i++;
                } else if (quoted && s[i + 1] == '"') {
                    a[k++] = '"';
                    i += 2;
                } else {
                    quoted = !quoted;
                    i++;
                }
            } else {
                for (size_t j = 0; j < bs; j++) a[k++] = '\\';
                if (s[i] && (quoted || (s[i] != ' ' && s[i] != '\t'))) a[k++] = s[i++];
            }
        }
        a[k] = 0;
    }
    return argc;
}
#define FUZZ_FAIL(...) do { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* in[0..n) mutated into out (cap bytes): flips, inserts, deletes, truncations, splices of
 * interesting bytes. Returns the new length. */
static size_t mutate(const unsigned char *in, size_t n, unsigned char *out, size_t cap) {
    static const unsigned char interesting[] = {0x00, 0x01, 0x02, 0x03, 0x30, 0x7f, 0x80, 0x81, 0xff, '"', '\\', '{', '}',
                                                '[', ']', ',', ':', ' ', 'u', '0', '9', '.', '-', 'e', 'n', 't', 'f'};
    if (n > cap) n = cap;
    memcpy(out, in, n);
    int steps = 1 + (int)(rnd() % 4);
    for (int s = 0; s < steps; s++) {
        size_t at = n ? rnd() % n : 0;
        switch (rnd() % 7) {
        case 0: if (n) out[at] ^= (unsigned char)(1u << (rnd() % 8)); break;
        case 1: if (n) out[at] = interesting[rnd() % sizeof interesting]; break;
        case 2: if (n < cap) { memmove(out + at + 1, out + at, n - at); out[at] = interesting[rnd() % sizeof interesting]; n++; } break;
        case 3: if (n) { memmove(out + at, out + at + 1, n - at - 1); n--; } break;
        case 4: n = at; break;
        case 5: if (n) out[at] = (unsigned char)rnd(); break;
        default: {   /* repeat a slice */
            size_t len = n ? 1 + rnd() % (n - at) : 0;
            if (len && n + len <= cap) { memmove(out + at + len, out + at, n - at); n += len; }   /* [at, at+len) now twice */
            break;
        }
        }
    }
    return n;
}

/* the minimal DER INTEGER of a 32-byte value */
static size_t der_int(const unsigned char v[32], unsigned char *o) {
    size_t i = 0;
    while (i < 31 && v[i] == 0) i++;
    size_t len = 32 - i, pad = v[i] & 0x80 ? 1 : 0;
    o[0] = 0x02;
    o[1] = (unsigned char)(len + pad);
    if (pad) o[2] = 0;
    memcpy(o + 2 + pad, v + i, len);
    return 2 + pad + len;
}

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64enc(const unsigned char *d, size_t n, char *o) {
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned long v = (unsigned long)d[i] << 16 | (i + 1 < n ? (unsigned long)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o[j++] = b64chars[v >> 18 & 63];
        o[j++] = b64chars[v >> 12 & 63];
        o[j++] = i + 1 < n ? b64chars[v >> 6 & 63] : '=';
        o[j++] = i + 2 < n ? b64chars[v & 63] : '=';
    }
    o[j] = 0;
}

typedef struct {
    const unsigned char *b;
    size_t n;
} pe_mem;
static int pe_read(void *ctx, uint64_t off, void *buf, size_t n) {
    pe_mem *m = ctx;
    if (off > m->n || n > m->n - off) return -1;   /* refused, as plat_pread's short read */
    memcpy(buf, m->b + off, n);
    return 0;
}

static const char payload_seed[] =
    "{\n \"payload_version\": \"1.5.0\",\n \"commit\": \"c\",\n"
    " \"app\": {\"version\": \"1.5.0\", \"zip\": \"rbstemsplus-app.zip\", \"sha256\": "
    "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\n"
    " \"bridge\": {\"file\": \"libonnxruntime.1.18.0.dylib\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\n"
    " \"model\": {\"file\": \"stemsplus-model.onnx\", \"sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\", \"size\": 9},\n"
    " \"previous_models\": [],\n"
    " \"compatible\": {\"rekordbox\": [\"7.2.19\"], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": [\"0002\"]},\n"
    " \"windows\": {\"app\": {\"version\": \"1.5.0\", \"zip\": \"w.zip\", \"sha256\": \"1111111111111111111111111111111111111111111111111111111111111111\","
    " \"admin_sha256\": \"2222222222222222222222222222222222222222222222222222222222222222\", \"stub_sha256\": {\"x64\": "
    "\"3333333333333333333333333333333333333333333333333333333333333333\", \"arm64\": \"4444444444444444444444444444444444444444444444444444444444444444\"}},"
    " \"bridge\": {\"file\": \"b.dll\", \"sha256\": \"5555555555555555555555555555555555555555555555555555555555555555\"},"
    " \"compatible\": {\"rekordbox\": [\"7.2.19\"], \"ort_sha256\": [\"6666666666666666666666666666666666666666666666666666666666666666\"],"
    " \"stems_engine\": [\"0002\"]}, \"resampler_proved\": [{\"product_version\": \"7.2.19\", \"rekordbox_sha256\": "
    "\"7777777777777777777777777777777777777777777777777777777777777777\"}]}\n}\n";

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    rng = argc > 2 ? strtoull(argv[2], NULL, 10) : 0;
    if (!rng) rng = 0x9e3779b97f4a7c15ULL;
    printf("fuzz: %ld iterations, seed %llu\n", iters, (unsigned long long)rng);
    test_key k;
    if (test_key_new(&k)) { fprintf(stderr, "no key\n"); return 1; }
    unsigned char good[16][SIG_DER_MAX];
    size_t goodn[16];
    for (int i = 0; i < 16; i++) goodn[i] = test_sign(&k, &i, sizeof i, good[i]);
    unsigned char spki[SIG_SPKI_LEN] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06,
                                        0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04};
    memcpy(spki + 27, k.xy, 64);
    static payload_manifest m1, m2;
    static unsigned char buf[PAYLOAD_MAX + 64];
    long accepted_der = 0, accepted_payload = 0;
    for (long it = 0; it < iters; it++) {
        unsigned char rs[64], xy[64], re[SIG_DER_MAX + 8];
        /* DER */
        int g = (int)(rnd() % 16);
        size_t n = mutate(good[g], goodn[g], buf, SIG_DER_MAX + 8);
        if (sig_der_to_rs(buf, n, rs) == 0) {
            accepted_der++;
            size_t a = der_int(rs, re + 2), b = der_int(rs + 32, re + 2 + a);
            re[0] = 0x30;
            re[1] = (unsigned char)(a + b);
            if (a + b + 2 != n || memcmp(re, buf, n)) FUZZ_FAIL("DER accepted but not canonical (%zu bytes)", n);
        }
        /* SPKI */
        n = mutate(spki, sizeof spki, buf, sizeof spki + 8);
        if (sig_spki_p256(buf, n, xy) == 0 && (n != sizeof spki || memcmp(buf, spki, 27))) FUZZ_FAIL("SPKI accepted with another header");
        /* base64 */
        char text[160], back[160];
        b64enc(spki, sizeof spki, text);
        n = mutate((const unsigned char *)text, strlen(text), buf, sizeof text - 1);
        buf[n] = 0;
        unsigned char dec[128];
        long dn = sig_base64_decode((const char *)buf, dec, sizeof dec);
        if (dn >= 0) {
            b64enc(dec, (size_t)dn, back);
            if (strcmp(back, (const char *)buf)) FUZZ_FAIL("base64 accepted but not canonical: %s", (const char *)buf);
        }
        /* the manifest */
        n = mutate((const unsigned char *)payload_seed, sizeof payload_seed - 1, buf, sizeof buf);
        int plain = payload_plain_keys(buf, n);
        int r1 = payload_decode(buf, n, &m1), r2 = payload_decode(buf, n, &m2);
        if (r1 == PAYLOAD_OK) accepted_payload++;
        if (r1 == PAYLOAD_OK && !plain) FUZZ_FAIL("a manifest decoded that plainJSONKeys refuses");
        if (r1 != r2 || memcmp(&m1, &m2, sizeof m1)) FUZZ_FAIL("the same bytes decoded twice differently");
    }
    test_key_free(&k);
    printf("fuzz: %ld DER and %ld manifests accepted among the mutations\n", accepted_der, accepted_payload);

    /* the PE reader: mutations of a real exe, mostly in its headers and resources */
    if (argc > 3) {
        FILE *f = fopen(argv[3], "rb");
        static unsigned char exe[1 << 20], mut[(1 << 20) + 64];
        size_t n = f ? fread(exe, 1, sizeof exe, f) : 0;
        if (f) fclose(f);
        if (n < 1024) { fprintf(stderr, "no PE at %s\n", argv[3]); return 1; }
        long versions = 0;
        for (long it = 0; it < iters / 4; it++) {
            size_t len;
            if (rnd() & 1) {   /* the headers: a mutated first 1 KB, the rest as it was */
                len = mutate(exe, 1024, mut, 1024 + 32);
                if (len + (n - 1024) > sizeof mut) continue;
                memcpy(mut + len, exe + 1024, n - 1024);
                len += n - 1024;
            } else {
                len = mutate(exe, n, mut, sizeof mut);
            }
            pe_mem pm = {mut, len};
            compat_pe pe;
            compat_pe_read(pe_read, &pm, &pe);
            for (const char *c = pe.product_version; *c; c++)
                if (*c < 0x20 || *c > 0x7e) { FUZZ_FAIL("a ProductVersion that isn't printable ASCII"); break; }
            for (const char *c = pe.product_name; *c; c++)
                if (*c < 0x20 || *c > 0x7e) { FUZZ_FAIL("a ProductName that isn't printable ASCII"); break; }
            versions += *pe.product_version != 0;
        }
        printf("fuzz: %ld PE mutations, %ld with a ProductVersion\n", iters / 4, versions);
    }

    /* the updater's parsers: what a record parser accepts it writes back the same */
    {
        static const char *const pend_seeds[] = {"1.5.2\n1.5.3\n4242\n1794177745\n", "1.4.0\n1.5.0\n1\n0\n"};
        static const char *const shown_seeds[] = {"1.4.1\n1.4.0\n", "1.5.2\n1.5.1\n"};
        long rec_ok = 0;
        for (long it = 0; it < iters; it++) {
            size_t n;
            const char *s = pend_seeds[rnd() % 2];
            n = mutate((const unsigned char *)s, strlen(s), buf, 200);
            update_pending p;
            char back[UPDATE_RECORD_MAX];
            if (update_pending_parse(buf, n, &p)) {
                rec_ok++;
                if (update_pending_format(&p, back, sizeof back) != n || memcmp(back, buf, n)) FUZZ_FAIL("a pending record that doesn't write back the same");
            }
            s = shown_seeds[rnd() % 2];
            n = mutate((const unsigned char *)s, strlen(s), buf, 100);
            update_shown sh;
            if (update_shown_parse(buf, n, &sh) && (update_shown_format(&sh, back, sizeof back) != n || memcmp(back, buf, n)))
                FUZZ_FAIL("a shown-version record that doesn't write back the same");
            char v[PAYLOAD_VERSION_MAX];
            n = mutate((const unsigned char *)"1.5.3\n", 6, buf, 64);
            if (update_current_parse(buf, n, v) && !update_is_version(v)) FUZZ_FAIL("current read as something not a version");
            update_fail_parse(buf, n);
        }
        printf("fuzz: %ld update records accepted\n", rec_ok);
    }

    /* the zip reader and inflate: mutations of a real zip (Python's, deflated), mostly in its
     * headers; and of a deflate stream alone. Nothing is written (unzip_check), ASan watches every
     * read and write. */
    if (argc > 4) {
        char path[1024];
        /* every zip gen-zips.py makes, one per iteration */
        static const char *const seeds[] = {"python-deflate9.zip", "python-deflate1.zip", "python-stored.zip", "infozip.zip", "ditto.zip", "many.zip"};
        enum { NSEEDS = sizeof seeds / sizeof seeds[0] };
        static unsigned char zips[NSEEDS][1 << 19], mut[(1 << 20) + 64], out[300000], raw[1 << 16];
        size_t zn[NSEEDS];
        for (int i = 0; i < NSEEDS; i++) {
            snprintf(path, sizeof path, "%s/%s", argv[4], seeds[i]);
            FILE *zf = fopen(path, "rb");
            zn[i] = zf ? fread(zips[i], 1, sizeof zips[i], zf) : 0;
            if (zf) fclose(zf);
            if (zn[i] < 1024 || zn[i] == sizeof zips[i] || unzip_check(zips[i], zn[i]) != UNZIP_OK) { fprintf(stderr, "no good zip at %s\n", path); return 1; }
        }
        snprintf(path, sizeof path, "%s/raw.deflate", argv[4]);
        FILE *f = fopen(path, "rb");
        size_t rn = f ? fread(raw, 1, sizeof raw, f) : 0;
        if (f) fclose(f);
        if (rn < 1024 || unzip_inflate(raw, rn, out, sizeof out, NULL)) { fprintf(stderr, "no good deflate stream at %s\n", path); return 1; }
        /* a scratch folder for the extract target: a zip unzip_check accepts unpacks */
        char scratch[512];
        snprintf(scratch, sizeof scratch, "%s/winc-fuzz.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        if (!mkdtemp(scratch)) { fprintf(stderr, "no scratch folder\n"); return 1; }
        long ok = 0, inflated = 0, extracted = 0;
        for (long it = 0; it < iters / 8; it++) {
            int si = (int)(rnd() % NSEEDS);
            const unsigned char *zip = zips[si];
            size_t n = zn[si];
            size_t len;
            int where = (int)(rnd() % 3);
            if (where == 0) {   /* the end: the central directory and the end record */
                size_t keep = n > 2048 ? n - 2048 : 0;
                memcpy(mut, zip, keep);
                len = keep + mutate(zip + keep, n - keep, mut + keep, sizeof mut - keep);
            } else if (where == 1) {   /* the start: the first local header and its data */
                len = mutate(zip, 1024, mut, 1024 + 32);
                memcpy(mut + len, zip + 1024, n - 1024);
                len += n - 1024;
            } else {
                len = mutate(zip, n, mut, sizeof mut);
            }
            int checked = unzip_check(mut, len);
            ok += checked == UNZIP_OK;
            if (checked == UNZIP_OK && extracted < 64) {   /* the extract target, bounded */
                char d[600], cmd[700];
                snprintf(d, sizeof d, "%s/x%ld", scratch, extracted++);
                mkdir(d, 0755);
                int err = 0, x = unzip_extract(mut, len, d, &err);
                if (x != UNZIP_OK) FUZZ_FAIL("a zip unzip_check accepts didn't unpack (%d, errno %d)", x, err);
                snprintf(cmd, sizeof cmd, "rm -rf '%s'", d);
                if (system(cmd)) FUZZ_FAIL("couldn't clean %s", d);
            }
            /* a deflate stream alone (dynamic blocks): mutated, into an output of the right size or
             * a random smaller one */
            size_t sl = mutate(raw, rn, mut, rn + 32), used;
            size_t want = rnd() & 1 ? sizeof out : rnd() % sizeof out;
            inflated += unzip_inflate(mut, sl, out, want, &used) == 0;
        }
        rmdir(scratch);
        printf("fuzz: %ld zip mutations of %d zips, %ld still whole (%ld unpacked); %ld deflate mutations inflated\n", iters / 8, NSEEDS, ok, extracted,
               inflated);
    }

    /* the small rules: what they accept holds their properties */
    {
        long ranges = 0, names = 0, quoted = 0;
        for (long it = 0; it < iters; it++) {
            /* update_range_start: an accepted 206 starts where asked and ends past its start, at
             * the total when one is given */
            static const char *const crs[] = {"bytes 100-999/1000", "bytes 100-999/*", "bytes 5-5/6"};
            const char *cr = crs[rnd() % 3];
            size_t n = mutate((const unsigned char *)cr, strlen(cr), buf, 64);
            buf[n] = 0;
            int64_t from = cr[6] == '5' ? 5 : 100, total, end;
            int64_t at = update_range_start(from, 206, (const char *)buf, -1, &total, &end);
            if (at >= 0) {
                ranges++;
                if (at != from || end <= at || (total >= 0 && end != total)) FUZZ_FAIL("a range accepted wrongly: %s", (const char *)buf);
            }
            /* unzip_name_ok: an accepted name is relative, printable, with no "..", '\\' or ':' */
            static const char *const zn_seeds[] = {"x64/rbsp-core.dll", "RB Stems Plus.exe", "a/b/c/", "NOTICE-windows.txt"};
            const char *nm = zn_seeds[rnd() % 4];
            n = mutate((const unsigned char *)nm, strlen(nm), buf, 300);
            if (unzip_name_ok((const char *)buf, n)) {
                names++;
                int bad = buf[0] == '/';
                for (size_t i = 0; i < n; i++) bad |= buf[i] < 0x20 || buf[i] > 0x7e || buf[i] == '\\' || buf[i] == ':';
                for (size_t i = 0; i + 1 < n; i++) bad |= buf[i] == '.' && buf[i + 1] == '.' && (i == 0 || buf[i - 1] == '/') && (i + 2 == n || buf[i + 2] == '/');
                if (bad) FUZZ_FAIL("a zip name accepted that shouldn't be");
            }
            /* stub_args: inside the line, never on a space or a tab */
            wchar_t wl[80];
            static const char cmdline[] = "\"C:\\a b\\RB Stems Plus.exe\" --watcher x";
            n = mutate((const unsigned char *)cmdline, sizeof cmdline - 1, buf, 79);
            for (size_t i = 0; i < n; i++) wl[i] = buf[i] ? (wchar_t)buf[i] : L'x';
            wl[n] = 0;
            const wchar_t *a = stub_args(wl);
            if (a < wl || a > wl + n || *a == L' ' || *a == L'\t') FUZZ_FAIL("stub_args outside the line or on a space");
            /* quoting: what update_quote_arg writes reads back as the argument */
            char arg[40], line[256] = "", back[4][128];
            size_t an = rnd() % sizeof arg;
            static const char alphabet[] = "ab \t\"\\\\x";
            for (size_t i = 0; i < an; i++) arg[i] = alphabet[rnd() % (sizeof alphabet - 1)];
            arg[an] = 0;
            if (!update_quote_arg(arg, line, sizeof line)) {
                quoted++;
                if (argv_parse(line, back, 4) != 1 || strcmp(back[0], arg)) FUZZ_FAIL("quoted \"%s\" read back as another: %s", arg, line);
            }
        }
        printf("fuzz: %ld ranges, %ld zip names accepted, %ld arguments quoted and read back\n", ranges, names, quoted);
    }
    if (failures) { printf("fuzz: %d failures\n", failures); return 1; }
    printf("fuzz: no failures\n");
    return 0;
}
