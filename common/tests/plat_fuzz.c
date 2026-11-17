/* Fuzz targets for plat_mac.c's two pure rules (bridge.md §0a T1b1), with plat_mac.c's own source
 * included (its static functions are the targets):
 *  - pm_attr_name, the reader of fgetattrlist's ATTR_CMN_NAME reply that plat_dir_name_is trusts:
 *    whatever the reply's bytes, the name it gives lies inside the bytes got and inside the length
 *    the reply gives itself, with its only NUL last; the seed is a real reply from this Mac;
 *  - pm_name_ok, the one-component rule of every _at call, against plat_win.c's wr_name16
 *    (win_rules.h): for every UTF-8 string without a '\' (a separator on Windows only) both accept
 *    the same names, so a name the Mac's suites pass is one Windows opens too.
 *
 * Built by common/tests/run.sh with AddressSanitizer and UndefinedBehaviorSanitizer. Apple's clang
 * has no libFuzzer, so main() is a small mutation fuzzer, as bridge/tests/fuzz.c's.
 *
 *   plat-fuzz SCRATCH [ITERATIONS [SEED]]   default 20000 per target; exits 1 on a failure */
#include "../plat_mac.c"
#include "../win_rules.h"

#include <stdio.h>

static void die(const char *what) {
    fprintf(stderr, "FAIL %s\n", what);
    abort();
}

/* the reply's bounds, written out on their own (not pm_attr_name's code) */
static void t_attr(const uint8_t *d, size_t n) {
    const char *s = pm_attr_name(d, n);
    if (!s) return;
    uint32_t len;
    attrreference_t ref;
    if (n < sizeof len + sizeof ref) die("attr: a name from a reply shorter than its header");
    memcpy(&len, d, sizeof len);
    memcpy(&ref, d + sizeof len, sizeof ref);
    size_t lim = len < n ? len : n, at = (size_t)((const uint8_t *)s - d);
    if ((const uint8_t *)s < d || at >= lim) die("attr: a name outside the reply");
    if (at != sizeof len + (size_t)ref.attr_dataoffset) die("attr: a name not where its reference says");
    if (ref.attr_length == 0 || at + ref.attr_length > lim) die("attr: a name running past the reply");
    if (strlen(s) + 1 != ref.attr_length) die("attr: a name whose NUL isn't its last byte");
}

/* pm_name_ok against wr_name16, relative to a folder and to plat_dir_cwd() */
static void t_name(const uint8_t *d, size_t n) {
    char s[600];
    uint16_t w[600];
    if (n >= sizeof s) n = sizeof s - 1;
    memcpy(s, d, n);
    s[n] = 0;                                               /* a C string: up to its first NUL */
    size_t l = strlen(s);
    plat_dir folder = {3}, cwd = plat_dir_cwd();            /* pm_name_ok never uses the descriptor */
    int mac = pm_name_ok(folder, s);
    if (pm_name_ok(cwd, s) != (s[0] == '/')) die("name: relative to plat_dir_cwd(), not exactly the absolute paths");
    if (wr_utf8_to_16(s, l, w, sizeof w / sizeof *w) < 0 || strchr(s, '\\')) {
        if (mac && (!l || strchr(s, '/') || !strcmp(s, ".") || !strcmp(s, ".."))) die("name: a path accepted as a name");
        return;
    }
    int win = wr_name16(s, w, sizeof w / sizeof *w) >= 0;
    if (mac != win) die(mac ? "name: the Mac accepts a name Windows refuses" : "name: the Mac refuses a name Windows accepts");
}

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

/* one mutation of seed into out (at most cap bytes), as bridge/tests/fuzz.c's, with the bytes that
 * matter here: separators, dots, NULs, and the 32-bit fields' extremes */
static size_t mutate(const uint8_t *seed, size_t n, uint8_t *out, size_t cap) {
    if (n > cap) n = cap;
    memcpy(out, seed, n);
    static const uint8_t special[] = {0, 0xFF, 0x7F, 0x80, '/', '\\', '.', 0xC0, 0xED, 0xEF, 0xF4, 0xF5, 1, 4, 8, 12};
    for (int k = 1 + (int)(rnd() % 8); k > 0; k--) {
        size_t at = n ? rnd() % n : 0;
        switch (rnd() % 8) {
        case 0: if (n) out[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: if (n) out[at] = special[rnd() % sizeof special]; break;
        case 2: if (n) out[at] = (uint8_t)rnd(); break;
        case 3: if (n < cap) { memmove(out + at + 1, out + at, n - at); out[at] = (uint8_t)rnd(); n++; } break;
        case 4: if (n) { memmove(out + at, out + at + 1, n - at - 1); n--; } break;
        case 5: n = at; break;
        case 6: if (n >= 12) {                              /* one of the header's three 32-bit fields */
            uint32_t v = rnd() % 4 == 0 ? 0xFFFFFFFFu : rnd() % 3 == 0 ? 0x7FFFFFFFu : rnd() % 2 ? 0x80000000u : rnd() % 64;
            memcpy(out + 4 * (rnd() % 3), &v, 4);
        } break;
        default: {
            size_t len = n ? 1 + rnd() % (n < 64 ? n : 64) : 0, from = n > len ? rnd() % (n - len) : 0;
            if (len && n + len <= cap) { memmove(out + at + len, out + at, n - at); memmove(out + at, out + from + (from >= at ? len : 0), len); n += len; }
        }
        }
    }
    return n;
}

static void run(const char *name, void (*f)(const uint8_t *, size_t), const uint8_t *seed, size_t n, long iters) {
    size_t cap = n * 2 + 1024;
    uint8_t *buf = malloc(cap);
    if (!buf) die("out of memory");
    f(seed, n);
    for (long i = 0; i < iters; i++) {
        size_t m = mutate(seed, n, buf, cap);
        uint8_t *exact = malloc(m ? m : 1);                 /* a buffer of exactly m bytes: ASan sees any read past it */
        if (!exact) die("out of memory");
        memcpy(exact, buf, m);
        f(exact, m);
        free(exact);
    }
    free(buf);
    printf("ok   %s: %ld mutations of a %zu-byte seed\n", name, iters, n);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s SCRATCH [ITERATIONS [SEED]]\n", argv[0]); return 2; }
    long iters = argc > 2 ? atol(argv[2]) : 20000;
    rng = argc > 3 ? strtoull(argv[3], NULL, 0) : (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL;
    if (!rng) rng = 1;
    printf("fuzz: seed %llu (plat-fuzz SCRATCH %ld %llu repeats this run)\n", (unsigned long long)rng, iters, (unsigned long long)rng);

    /* a real reply: the scratch folder's own name, as plat_dir_name_is reads it */
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/J\xc3\xbcrgen \xe6\x9d\xb1\xe4\xba\xac", argv[1]);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) die("can't make the seed folder");
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    struct attrlist al = {.bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_NAME};
    union { uint32_t align; unsigned char b[4 + sizeof(attrreference_t) + 1024]; } a;
    if (fd < 0 || fgetattrlist(fd, &al, a.b, sizeof a.b, 0) != 0) die("can't read the seed reply");
    close(fd);
    uint32_t got;
    memcpy(&got, a.b, sizeof got);
    if (got > sizeof a.b) die("the seed reply's length");
    const char *nm = pm_attr_name(a.b, got);
    if (!nm || strcmp(nm, "J\xc3\xbcrgen \xe6\x9d\xb1\xe4\xba\xac")) die("the seed reply doesn't give its folder's name");
    run("pm_attr_name (fgetattrlist's name reply)", t_attr, a.b, got, iters);

    static const char *const names[] = {"ab", "J\xc3\xbcrgen \xe6\x9d\xb1\xe4\xba\xac", "..", "a/b", "/Volumes/x/y", "x.flac.123.tmp", "\xf0\x9f\x8e\xb5"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char what[96];
        snprintf(what, sizeof what, "pm_name_ok vs wr_name16 (seed %zu)", i);
        run(what, t_name, (const uint8_t *)names[i], strlen(names[i]), iters / 4);
    }
    printf("fuzz: no failures\n");
    return 0;
}
