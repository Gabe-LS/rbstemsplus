/* receipt.c: see receipt.h. Pure C11 over common/plat.h's file layer. */
#include "receipt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"
#endif

static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_hex(int c) { return is_digit(c) || (c >= 'a' && c <= 'f'); }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* X.Y.Z, each part 1 to 9 digits */
static int xyz_ok(const char *v) {
    for (int part = 0; part < 3; part++) {
        int k = 0;
        while (is_digit((unsigned char)*v)) v++, k++;
        if (k < 1 || k > 9) return 0;
        if (part < 2 && *v++ != '.') return 0;
    }
    return *v == 0;
}

static int hex_ok(const char *s, size_t n) {
    if (strlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (!is_hex((unsigned char)s[i])) return 0;
    return 1;
}

/* "rekordbox X.Y.Z" (the word case-blind, as Windows names it): 1 with X.Y.Z in v (cap bytes) */
static int folder_version(const char *name, char *v, size_t cap) {
    static const char word[] = "rekordbox ";
    for (size_t i = 0; i < sizeof word - 1; i++)
        if (lower((unsigned char)name[i]) != word[i]) return 0;
    const char *x = name + sizeof word - 1;
    if (!xyz_ok(x) || strlen(x) >= cap) return 0;
    memcpy(v, x, strlen(x) + 1);
    return 1;
}

static int fields_ok(const receipt *r) {
    char v[COMPAT_VERSION_MAX];
    return folder_version(r->folder_name, v, sizeof v) && !strcmp(v, r->version) && hex_ok(r->folder_id, PLAT_ID_HEX_MAX - 1)
           && payload_is_sha256(r->exe_sha256) && payload_is_sha256(r->original_sha256) && payload_is_sha256(r->bridge_sha256)
           && payload_is_sha256(r->runtime_code_sha256) && xyz_ok(r->payload_version);
}

int receipt_format(const receipt *r, char *out, size_t cap) {
    if (!cap) return -1;
    out[0] = 0;
    if (!r || !fields_ok(r)) return -1;
    int k = snprintf(out, cap,
                     RECEIPT_MAGIC "\n"
                                   "folder %s\n"
                                   "folder_id %s\n"
                                   "exe_sha256 %s\n"
                                   "original_sha256 %s\n"
                                   "bridge_sha256 %s\n"
                                   "runtime_code_sha256 %s\n"
                                   "payload_version %s\n",
                     r->folder_name, r->folder_id, r->exe_sha256, r->original_sha256, r->bridge_sha256, r->runtime_code_sha256, r->payload_version);
    if (k < 0 || (size_t)k >= cap || k > RECEIPT_MAX) {
        out[0] = 0;
        return -1;
    }
    return k;
}

/* the line at *at in b[0..n) that starts with key and one space: its value (up to "\n", which it
 * must end in) copied into out (cap bytes); *at past the line. 0, or -1 */
static int take(const char *b, size_t n, size_t *at, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key), i = *at;
    if (n - i < kl + 1 || memcmp(b + i, key, kl) || b[i + kl] != ' ') return -1;
    i += kl + 1;
    size_t s = i;
    while (i < n && b[i] != '\n') {
        unsigned char c = (unsigned char)b[i];
        if (c < 0x20 || c > 0x7e) return -1;   /* printable ASCII only: no CR, no NUL */
        i++;
    }
    if (i == n || i == s || i - s >= cap) return -1;
    memcpy(out, b + s, i - s);
    out[i - s] = 0;
    *at = i + 1;
    return 0;
}

int receipt_parse(const char *b, size_t n, receipt *out) {
    memset(out, 0, sizeof *out);
    size_t ml = strlen(RECEIPT_MAGIC), at = ml + 1;
    if (!b || n > RECEIPT_MAX || n < at || memcmp(b, RECEIPT_MAGIC, ml) || b[ml] != '\n') return -1;
    receipt r;
    memset(&r, 0, sizeof r);
    if (take(b, n, &at, "folder", r.folder_name, sizeof r.folder_name) || take(b, n, &at, "folder_id", r.folder_id, sizeof r.folder_id)
        || take(b, n, &at, "exe_sha256", r.exe_sha256, sizeof r.exe_sha256)
        || take(b, n, &at, "original_sha256", r.original_sha256, sizeof r.original_sha256)
        || take(b, n, &at, "bridge_sha256", r.bridge_sha256, sizeof r.bridge_sha256)
        || take(b, n, &at, "runtime_code_sha256", r.runtime_code_sha256, sizeof r.runtime_code_sha256)
        || take(b, n, &at, "payload_version", r.payload_version, sizeof r.payload_version) || at != n)
        return -1;
    if (!folder_version(r.folder_name, r.version, sizeof r.version) || !fields_ok(&r)) return -1;
    *out = r;
    return 0;
}

int receipt_read(const char *root, const char *version, receipt *out) {
    memset(out, 0, sizeof *out);
    if (!root || !version || !xyz_ok(version)) return EINVAL;
    char p[COMPAT_PATH_MAX + 16];
    if (snprintf(p, sizeof p, "%s" SEP RECEIPT_DIR, root) >= (int)sizeof p) return EINVAL;
    plat_dir d;
    int e = plat_dir_open(p, &d);
    if (e) return e == ENOENT ? ENOENT : EINVAL;
    plat_file f;
    int64_t size = 0;
    e = plat_file_open_at(d, version, RECEIPT_MAX, &size, &f);
    plat_dir_close(d);
    if (e) return e == ENOENT ? ENOENT : EINVAL;
    char b[RECEIPT_MAX];
    e = size > 0 ? plat_read_all(f, b, (size_t)size) : EINVAL;
    plat_file_close(f);
    if (e || receipt_parse(b, (size_t)size, out) || strcmp(out->version, version)) {
        memset(out, 0, sizeof *out);
        return EINVAL;
    }
    return 0;
}

static int same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
    return *a == *b;
}

int receipt_matches(const receipt *r, const inv_install *row, const char *live_sha, const char *saved_sha, const char *ort_code) {
    if (!r || !row || !live_sha || !saved_sha || !ort_code || !fields_ok(r)) return 0;
    char id[PLAT_ID_HEX_MAX];
    plat_id_hex(row->dir_id, id);
    return same_name(r->folder_name, row->name) && !strcmp(r->version, row->version) && !strcmp(r->folder_id, id)
           && !strcmp(r->exe_sha256, row->exe.sha256) && !strcmp(r->bridge_sha256, live_sha) && !strcmp(r->original_sha256, saved_sha)
           && !strcmp(r->runtime_code_sha256, ort_code);
}
