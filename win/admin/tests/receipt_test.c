/* receipt.c (mv-spec §2.2, D4, D13): the text both ways, strict; the read (not followed, a regular
 * file, capped); the match against a row (folder name and identity, exe, live bridge, saved
 * original, runtime); plat_id_hex. */
#include "../../c/tests/check.h"

#include "../../c/receipt.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

void test_receipt(const char *scratch);

static void spit(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    fputs(s, f);
    fclose(f);
}

static void fill(char out[65], char c) {
    memset(out, c, 64);
    out[64] = 0;
}

static receipt sample(void) {
    receipt r;
    memset(&r, 0, sizeof r);
    snprintf(r.folder_name, sizeof r.folder_name, "rekordbox 7.2.19");
    snprintf(r.version, sizeof r.version, "7.2.19");
    plat_id id = {0x11, {0x22, 0x33}};
    plat_id_hex(id, r.folder_id);
    fill(r.exe_sha256, 'e');
    fill(r.original_sha256, 'a');
    fill(r.bridge_sha256, 'b');
    fill(r.runtime_code_sha256, 'c');
    snprintf(r.payload_version, sizeof r.payload_version, "1.5.0");
    return r;
}

void test_receipt(const char *scratch) {
    char hex[PLAT_ID_HEX_MAX];
    plat_id_hex((plat_id){0x0123456789abcdefULL, {1, 0xffffffffffffffffULL}}, hex);
    CHECK(!strcmp(hex, "0123456789abcdef0000000000000001ffffffffffffffff"), "plat_id_hex: vol, file[0], file[1], 16 lowercase digits each: %s", hex);

    receipt r = sample(), back;
    char text[RECEIPT_MAX + 1];
    int n = receipt_format(&r, text, sizeof text);
    const char *want = "rbsp-receipt 1\nfolder rekordbox 7.2.19\nfolder_id 000000000000001100000000000000220000000000000033\n";
    CHECK(n > 0 && !strncmp(text, want, strlen(want)) && text[n - 1] == '\n', "the text, in order: %s", text);
    CHECK(!receipt_parse(text, (size_t)n, &back) && !memcmp(&back, &r, sizeof r), "read back as written, version from the folder");

    /* strict */
    char bad[RECEIPT_MAX + 64];
    CHECK(receipt_parse(text, (size_t)n - 1, &back) == -1, "no last newline: refused");
    snprintf(bad, sizeof bad, "%sextra 1\n", text);
    CHECK(receipt_parse(bad, strlen(bad), &back) == -1 && !back.version[0], "an extra line: refused, out cleared");
    snprintf(bad, sizeof bad, "%s", text);
    char *s = strstr(bad, "exe_sha256 ") + 11;
    *s = 'E';
    CHECK(receipt_parse(bad, strlen(bad), &back) == -1, "uppercase hex: refused");
    snprintf(bad, sizeof bad, "%s", text);
    s = strstr(bad, "folder_id");
    memcpy(s, "folder_ix", 9);
    CHECK(receipt_parse(bad, strlen(bad), &back) == -1, "a key misnamed: refused");
    snprintf(bad, sizeof bad, "rbsp-receipt 2%s", text + 14);
    CHECK(receipt_parse(bad, strlen(bad), &back) == -1, "another format: refused");
    snprintf(bad, sizeof bad, "%s", text);
    s = strstr(bad, "rekordbox 7.2.19");
    memcpy(s, "rekordbix", 9);
    CHECK(receipt_parse(bad, strlen(bad), &back) == -1, "a folder that isn't rekordbox X.Y.Z: refused");
    receipt w = r;
    snprintf(w.version, sizeof w.version, "7.2.20");
    CHECK(receipt_format(&w, text, sizeof text) == -1 && !text[0], "a version that isn't the folder's: not written");
    w = r;
    w.runtime_code_sha256[0] = 0;
    CHECK(receipt_format(&w, text, sizeof text) == -1, "a field missing: not written");
    CHECK(receipt_format(&r, text, 40) == -1, "too small: -1");

    /* the read: root\receipts\<version> */
    char root[1100], dir[1200], p[1300];
    snprintf(root, sizeof root, "%s/rroot", scratch);
    snprintf(dir, sizeof dir, "%s/receipts", root);
    mkdir(root, 0755);
    CHECK(receipt_read(root, "7.2.19", &back) == ENOENT, "no receipts\\: ENOENT");
    mkdir(dir, 0755);
    CHECK(receipt_read(root, "7.2.19", &back) == ENOENT, "none: ENOENT");
    n = receipt_format(&r, text, sizeof text);
    snprintf(p, sizeof p, "%s/7.2.19", dir);
    spit(p, text);
    CHECK(!receipt_read(root, "7.2.19", &back) && !strcmp(back.bridge_sha256, r.bridge_sha256), "read");
    CHECK(receipt_read(root, "../7.2.19", &back) == EINVAL && receipt_read(root, "7.2", &back) == EINVAL, "a name that isn't X.Y.Z: EINVAL");
    snprintf(p, sizeof p, "%s/7.2.20", dir);
    spit(p, text);
    CHECK(receipt_read(root, "7.2.20", &back) == EINVAL, "a receipt under another version's name: EINVAL");
    unlink(p);
    char real[1300];
    snprintf(real, sizeof real, "%s/7.2.19", dir);
    if (symlink(real, p)) abort();
    CHECK(receipt_read(root, "7.2.20", &back) == EINVAL, "a link: not followed, EINVAL");
    unlink(p);
    char big[RECEIPT_MAX + 8];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    spit(p, big);
    CHECK(receipt_read(root, "7.2.20", &back) == EINVAL, "over the cap: EINVAL");
    unlink(p);
    mkdir(p, 0755);
    CHECK(receipt_read(root, "7.2.20", &back) == EINVAL, "a folder: EINVAL");
    rmdir(p);

    /* the match */
    inv_install row;
    memset(&row, 0, sizeof row);
    snprintf(row.name, sizeof row.name, "Rekordbox 7.2.19");   /* Windows' names are case-blind */
    snprintf(row.version, sizeof row.version, "7.2.19");
    row.dir_id = (plat_id){0x11, {0x22, 0x33}};
    fill(row.exe.sha256, 'e');
    char a[65], b[65], c[65], x[65];
    fill(a, 'a');
    fill(b, 'b');
    fill(c, 'c');
    fill(x, 'f');
    CHECK(receipt_matches(&r, &row, b, a, c), "every field binds this row");
    CHECK(!receipt_matches(&r, &row, x, a, c), "another live DLL");
    CHECK(!receipt_matches(&r, &row, b, x, c), "another saved original");
    CHECK(!receipt_matches(&r, &row, b, a, x) && !receipt_matches(&r, &row, b, a, ""), "another runtime, or none");
    inv_install other = row;
    other.dir_id.file[0] = 0x23;
    CHECK(!receipt_matches(&r, &other, b, a, c), "a reinstalled folder (a new identity): no match");
    other = row;
    fill(other.exe.sha256, 'd');
    CHECK(!receipt_matches(&r, &other, b, a, c), "another rekordbox.exe (a same-version reinstall): no match");
    other = row;
    snprintf(other.name, sizeof other.name, "rekordbox 7.2.20");
    snprintf(other.version, sizeof other.version, "7.2.20");
    CHECK(!receipt_matches(&r, &other, b, a, c), "another folder: no match");
}
