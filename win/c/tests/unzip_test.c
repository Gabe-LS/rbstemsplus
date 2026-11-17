/* unzip.c on plat_mac.c: the CRC, the inflate on zips made by three other tools (run.sh: Python's
 * zipfile at two levels and stored, Info-ZIP's zip, ditto), compared byte for byte with the files
 * they were made from; the name rules; and crafted zips each rule refuses. */
#include "check.h"

#include "../unzip.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* -- a stored zip, written here (the test's own, so every field can be bent) */

static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { put16(p, v & 0xffff), put16(p + 2, v >> 16); }

unsigned char *test_zip_ex(const test_zfile *f, int nf, size_t *out_n, const test_zbend *bend) {
    size_t cap = 1024;
    for (int i = 0; i < nf; i++) cap += 2 * (46 + strlen(f[i].name)) + f[i].n;
    unsigned char *z = calloc(1, cap), *cd = calloc(1, cap);
    if (!z || !cd) abort();
    size_t at = 0, cn = 0;
    for (int i = 0; i < nf; i++) {
        size_t nl = strlen(f[i].name);
        uint32_t crc = unzip_crc32(0, f[i].data, f[i].n);
        uint32_t method = bend && bend->method && i == bend->index ? bend->method : 0;
        uint32_t flags = bend && i == bend->index ? bend->flags : 0;
        if (bend && i == bend->index && bend->bad_crc) crc ^= 1;
        unsigned char *l = z + at;
        put32(l, 0x04034b50u), put16(l + 4, 20), put16(l + 6, flags), put16(l + 8, method), put32(l + 14, crc);
        put32(l + 18, (uint32_t)f[i].n), put32(l + 22, (uint32_t)f[i].n), put16(l + 26, (uint32_t)nl), put16(l + 28, 0);
        memcpy(l + 30, f[i].name, nl);
        memcpy(l + 30 + nl, f[i].data, f[i].n);
        unsigned char *c = cd + cn;
        uint32_t made_by = bend && i == bend->index && bend->symlink ? 3u << 8 | 20 : 20;
        uint32_t ext = bend && i == bend->index && bend->symlink ? 0120777u << 16 : 0;
        put32(c, 0x02014b50u), put16(c + 4, made_by), put16(c + 6, 20), put16(c + 8, flags), put16(c + 10, method), put32(c + 16, crc);
        put32(c + 20, (uint32_t)f[i].n), put32(c + 24, (uint32_t)f[i].n), put16(c + 28, (uint32_t)nl), put32(c + 38, ext), put32(c + 42, (uint32_t)at);
        memcpy(c + 46, f[i].name, nl);
        at += 30 + nl + f[i].n;
        cn += 46 + nl;
    }
    memcpy(z + at, cd, cn);
    unsigned char *e = z + at + cn;
    put32(e, 0x06054b50u), put16(e + 8, (uint32_t)nf), put16(e + 10, (uint32_t)nf), put32(e + 12, (uint32_t)cn), put32(e + 16, (uint32_t)at);
    *out_n = at + cn + 22;
    free(cd);
    return z;
}

unsigned char *test_zip(const test_zfile *f, int nf, size_t *out_n) { return test_zip_ex(f, nf, out_n, NULL); }

static int read_whole(const char *path, unsigned char **buf, size_t *n) {
    *buf = NULL;
    *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long k = ftell(f);
    fseek(f, 0, SEEK_SET);
    *buf = malloc(k > 0 ? (size_t)k : 1);
    if (!*buf) abort();
    *n = k > 0 && fread(*buf, 1, (size_t)k, f) == (size_t)k ? (size_t)k : 0;
    fclose(f);
    return k >= 0 && *n == (size_t)k ? 0 : -1;
}

/* whether the trees a and b hold the same names and bytes; *files counts the files */
static int same_tree(const char *a, const char *b, int *files) {
    DIR *d = opendir(a);
    if (!d) return 0;
    int ok = 1, na = 0, nb = 0;
    for (struct dirent *e; ok && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        na++;
        char pa[2048], pb[2048];
        snprintf(pa, sizeof pa, "%s/%s", a, e->d_name);
        snprintf(pb, sizeof pb, "%s/%s", b, e->d_name);
        struct stat sa, sb;
        if (lstat(pa, &sa) || lstat(pb, &sb) || (sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) {
            ok = 0;
        } else if (S_ISDIR(sa.st_mode)) {
            ok = same_tree(pa, pb, files);
        } else {
            unsigned char *x = NULL, *y = NULL;
            size_t xn, yn;
            ok = !read_whole(pa, &x, &xn) && !read_whole(pb, &y, &yn) && xn == yn && !memcmp(x, y, xn);
            free(x), free(y);
            (*files)++;
        }
    }
    closedir(d);
    if ((d = opendir(b))) {
        for (struct dirent *e; (e = readdir(d));)
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) nb++;
        closedir(d);
    }
    return ok && na == nb;
}

static int counter;
/* a fresh empty folder under scratch */
static void fresh(const char *scratch, char *out, size_t n) {
    snprintf(out, n, "%s/x%d", scratch, ++counter);
    mkdir(out, 0755);
}

void test_unzip(const char *scratch, const char *zips) {
    CHECK(unzip_crc32(0, "123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    CHECK(unzip_crc32(unzip_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u, "CRC-32 continued");

    /* zips from other tools, against the tree they were made from */
    static const char *const made[] = {"python-deflate9.zip", "python-deflate1.zip", "python-stored.zip", "infozip.zip", "ditto.zip"};
    char src[1024];
    snprintf(src, sizeof src, "%s/src", zips);
    for (size_t i = 0; i < sizeof made / sizeof made[0]; i++) {
        char p[1024], d[1024];
        snprintf(p, sizeof p, "%s/%s", zips, made[i]);
        unsigned char *z;
        size_t n;
        CHECK(!read_whole(p, &z, &n), "%s is there", made[i]);
        fresh(scratch, d, sizeof d);
        int err = 0, r = unzip_extract(z, n, d, &err);
        int files = 0;
        CHECK(r == UNZIP_OK, "%s unpacks (%d, %s)", made[i], r, strerror(err));
        CHECK(r || (same_tree(src, d, &files) && files >= 6), "%s: the same files and bytes (%d files)", made[i], files);
        free(z);
    }

    /* inflate on its own: a stream cut short, one that would write past the end, garbage */
    {
        char p[1024];
        snprintf(p, sizeof p, "%s/raw.deflate", zips);
        unsigned char *in, *out;
        size_t n, want = 300000;
        CHECK(!read_whole(p, &in, &n), "raw.deflate is there");
        out = malloc(want);
        size_t used = 0;
        CHECK(unzip_inflate(in, n, out, want, &used) == 0 && used == n, "a raw stream inflates whole");
        char q[1024];
        snprintf(q, sizeof q, "%s/raw.txt", zips);
        unsigned char *plain;
        size_t pn;
        CHECK(!read_whole(q, &plain, &pn) && pn == want && !memcmp(plain, out, want), "to the bytes it was made from");
        CHECK(unzip_inflate(in, n - 1, out, want, NULL) != 0, "cut by one byte: refused");
        CHECK(unzip_inflate(in, n / 2, out, want, NULL) != 0, "cut in half: refused");
        CHECK(unzip_inflate(in, n, out, want - 1, NULL) != 0, "one byte more than asked: refused");
        CHECK(unzip_inflate(in, n, out, want + 1, NULL) != 0, "one byte less than asked: refused");
        static const unsigned char garbage[] = {0xff, 0xff, 0xff, 0xff, 0x00, 0x12, 0x34};
        CHECK(unzip_inflate(garbage, sizeof garbage, out, 16, NULL) != 0, "garbage: refused");
        static const unsigned char reserved[] = {0x07, 0x00};   /* BFINAL, BTYPE 11 */
        CHECK(unzip_inflate(reserved, sizeof reserved, out, 16, NULL) != 0, "block type 3: refused");
        static const unsigned char stored_bad[] = {0x01, 0x03, 0x00, 0xfc, 0xfe, 'a', 'b', 'c'};   /* NLEN not ~LEN */
        CHECK(unzip_inflate(stored_bad, sizeof stored_bad, out, 3, NULL) != 0, "a stored block whose NLEN isn't LEN's complement: refused");
        static const unsigned char stored_ok[] = {0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c'};
        CHECK(unzip_inflate(stored_ok, sizeof stored_ok, out, 3, NULL) == 0 && !memcmp(out, "abc", 3), "a stored block");
        static const unsigned char empty_fixed[] = {0x03, 0x00};
        CHECK(unzip_inflate(empty_fixed, sizeof empty_fixed, out, 0, &used) == 0 && used == 2, "an empty fixed block");
        static const unsigned char far_back[] = {0x63, 0x00, 0x02, 0x00};   /* a match before the start */
        CHECK(unzip_inflate(far_back, sizeof far_back, out, 16, NULL) != 0, "a distance before the output's start: refused");
        free(in), free(out), free(plain);
    }

    /* the name rules */
    static const char *const good[] = {"RB Stems Plus.exe", "x64/rbsp-core.dll", "dir/", "a/b/c/d/e/f/g/h", "COM10", "console.txt", "LPT", "a.b.c",
                                       "NOTICE-windows.txt", "stub/arm64/RB Stems Plus.exe", ".hidden", "a-b_c (1).txt"};
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) CHECK(unzip_name_ok(good[i], strlen(good[i])), "a name allowed: %s", good[i]);
    static const char *const bad[] = {"", "/abs", "../x", "a/../b", "a/./b", "./a", "a//b", "a\\b", "C:x", "a:b", "con", "CON.txt", "a/Nul",
                                      "com1.log", "LPT9", "conin$", "a.", "a ", "dir./x", "a/b /c", "a*b", "a?b", "a\"b", "a<b", "a>b",
                                      "a|b", "a/b/c/d/e/f/g/h/i", "\x01", "caf\xc3\xa9", "/", "a\x7f"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(!unzip_name_ok(bad[i], strlen(bad[i])), "a name refused: %s", bad[i]);
    char longname[300];
    memset(longname, 'a', sizeof longname);
    CHECK(unzip_name_ok(longname, 255) && !unzip_name_ok(longname, 256), "255 bytes at most");
    /* review BM2 (3): a device name in any case, with and without an extension, as a file or a
     * folder, and a control character in a folder's name */
    static const char *const devices[] = {"CON", "con", "Con", "CON.txt", "con.tar.gz", "CON .txt", "NUL", "nul", "Nul.log", "nUl.",
                                          "COM1", "com1", "Com1.txt", "COM9.x", "LPT1", "lpt1", "Lpt1.dat", "LPT9.x", "PRN", "aux.h",
                                          "CONIN$", "conout$.log", "CON/x", "nul/a.txt", "com1/b", "a/LPT1", "a/b/lpt1.txt", "x/Con.ext/y"};
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++)
        CHECK(!unzip_name_ok(devices[i], strlen(devices[i])), "a device name refused: %s", devices[i]);
    static const char *const ctl[] = {"a\x01" "b/c.txt", "\x1f/x", "dir\t/x", "a/b\x7f/c", "a\n/b", "x/\x1b" "d/e", "a\r/b/"};
    for (size_t i = 0; i < sizeof ctl / sizeof ctl[0]; i++)
        CHECK(!unzip_name_ok(ctl[i], strlen(ctl[i])), "a control character in a folder's name refused (case %zu)", i);
    /* review BM2 (2): every name the release zip holds (win/zip-contents.txt, release.md T6's list,
     * build-windows.sh's exact diff) passes the same rule the updater and bootstrap apply */
    {
        const char *list = getenv("RBSP_ZIP_CONTENTS");
        if (!list || !list[0]) list = "win/zip-contents.txt";
        unsigned char *txt;
        size_t tn;
        int lines = 0, refused = 0;
        CHECK(read_whole(list, &txt, &tn) == 0 && tn > 0, "win/zip-contents.txt read (%s)", list);
        for (size_t at = 0; txt && at < tn;) {
            size_t e = at;
            while (e < tn && txt[e] != '\n') e++;
            size_t len = e > at && txt[e - 1] == '\r' ? e - at - 1 : e - at;
            if (len) {
                lines++;
                if (!unzip_name_ok((const char *)txt + at, len)) {
                    refused++;
                    CHECK(0, "a release zip name refused by unzip_name_ok: %.*s", (int)len, (const char *)txt + at);
                }
            }
            at = e + 1;
        }
        CHECK(lines >= 12 && refused == 0, "every name of win/zip-contents.txt passes unzip_name_ok (%d names)", lines);
        free(txt);
    }

    /* crafted zips */
    test_zfile ok2[] = {{"a.txt", "hello", 5}, {"d/", "", 0}, {"d/b.txt", "world", 5}};
    size_t n;
    unsigned char *z = test_zip(ok2, 3, &n);
    char d[1024];
    fresh(scratch, d, sizeof d);
    CHECK(unzip_extract(z, n, d, NULL) == UNZIP_OK, "the test's own zip unpacks");
    char p[1100];
    snprintf(p, sizeof p, "%s/d/b.txt", d);
    unsigned char *got;
    size_t gn;
    CHECK(!read_whole(p, &got, &gn) && gn == 5 && !memcmp(got, "world", 5), "into its folders");
    free(got);
    CHECK(unzip_extract(z, n, d, NULL) == UNZIP_WRITE, "again into the same folder: a name exists, refused");
    CHECK(unzip_check(z, n - 1) == UNZIP_NOT_ZIP, "cut by a byte: not a zip");
    unsigned char *longer = malloc(n + 3);
    memcpy(longer, z, n);
    memcpy(longer + n, "abc", 3);
    CHECK(unzip_check(longer, n + 3) == UNZIP_NOT_ZIP, "bytes after the end record: not a zip");
    free(longer);
    free(z);

    /* two entries sharing bytes: the first's size grown over the second's local header */
    test_zfile two[] = {{"a.txt", "aaaa", 4}, {"b.txt", "bbbb", 4}};
    z = test_zip(two, 2, &n);
    size_t cd = (size_t)z[n - 6] | (size_t)z[n - 5] << 8 | (size_t)z[n - 4] << 16 | (size_t)z[n - 3] << 24;
    size_t grow = 4 + 30 + 5 + 4;   /* a.txt's data, then all of b.txt's local entry */
    z[cd + 20] = z[cd + 24] = (unsigned char)grow;
    CHECK(unzip_check(z, n) == UNZIP_NOT_ZIP, "overlapping entries: not a zip (before any CRC)");
    free(z);

    /* many entries (gen-zips.py's 1,024) */
    {
        char mp[1024], md[1024];
        snprintf(mp, sizeof mp, "%s/many.zip", zips);
        unsigned char *mz;
        size_t mn;
        CHECK(!read_whole(mp, &mz, &mn), "many.zip is there");
        fresh(scratch, md, sizeof md);
        CHECK(unzip_extract(mz, mn, md, NULL) == UNZIP_OK, "1,024 entries unpack");
        snprintf(mp, sizeof mp, "%s/d/f1023.txt", md);
        unsigned char *last;
        size_t ln;
        CHECK(!read_whole(mp, &last, &ln) && ln == 5 && !memcmp(last, "1023\n", 5), "the last one too");
        free(last);
        free(mz);
    }

    struct {
        const char *what;
        test_zfile f[2];
        int nf;
        test_zbend bend;
        int want;
    } cases[] = {
        {"a name going up", {{"../evil", "x", 1}}, 1, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"an absolute name", {{"/evil", "x", 1}}, 1, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"a backslash", {{"a\\evil", "x", 1}}, 1, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"a drive", {{"C:evil", "x", 1}}, 1, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"a device name", {{"x/aux.dll", "x", 1}}, 1, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"a trailing dot", {{"evil.", "x", 1}}, 1, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"two names the same ignoring case", {{"A.txt", "x", 1}, {"a.TXT", "y", 1}}, 2, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"a folder and a file of one name", {{"a/", "", 0}, {"A", "y", 1}}, 2, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"a file, then a path through it", {{"a", "x", 1}, {"A/b", "y", 1}}, 2, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"a path, then a file in its way", {{"x/Y/z", "x", 1}, {"x/y", "y", 1}}, 2, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
        {"a Unix symbolic link", {{"link", "/etc/passwd", 11}}, 1, {0, 0, 0, 1, 0}, UNZIP_BAD_NAME},
        {"an encrypted entry", {{"a", "x", 1}}, 1, {0, 1, 0, 0, 0}, UNZIP_NOT_ZIP},
        {"bzip2", {{"a", "x", 1}}, 1, {0, 0, 12, 0, 0}, UNZIP_NOT_ZIP},
        {"a CRC that doesn't match", {{"a", "x", 1}, {"b", "y", 1}}, 2, {1, 0, 0, 0, 1}, UNZIP_DAMAGED},
        {"a folder with bytes", {{"d/", "x", 1}}, 1, {-1, 0, 0, 0, 0}, UNZIP_BAD_NAME},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        z = test_zip_ex(cases[i].f, cases[i].nf, &n, &cases[i].bend);
        fresh(scratch, d, sizeof d);
        int r = unzip_extract(z, n, d, NULL);
        CHECK(r == cases[i].want, "%s: %d, want %d", cases[i].what, r, cases[i].want);
        DIR *dd = opendir(d);
        int names = 0;
        for (struct dirent *e; dd && (e = readdir(dd));) names += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
        if (dd) closedir(dd);
        CHECK(names == 0, "%s: nothing written", cases[i].what);
        free(z);
    }
}
