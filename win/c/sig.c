/* payload.json's signature: the DER and key parsing and the signer loop (sig.h). Pure C11; the
 * crypto call is sig_p256_verify (sig_bcrypt.c on Windows). */
#include "sig.h"

#include <string.h>

/* one INTEGER of the signature at p (end e): strictly DER, positive, nonzero, at most 32 bytes
 * once its sign byte is dropped; right-aligned into out[32]. The position after it, or NULL. */
static const unsigned char *der_int(const unsigned char *p, const unsigned char *e, unsigned char out[32]) {
    if (e - p < 2 || p[0] != 0x02) return NULL;
    size_t len = p[1];
    p += 2;
    if (len == 0 || len > 33 || (size_t)(e - p) < len) return NULL;   /* 0x80 and up: long form, refused */
    if (p[0] & 0x80) return NULL;                                     /* negative */
    if (p[0] == 0 && (len == 1 || !(p[1] & 0x80))) return NULL;       /* zero, or a needless leading 0 */
    const unsigned char *v = p;
    size_t vl = len;
    if (v[0] == 0) { v++; vl--; }                                     /* the sign byte */
    if (vl > 32) return NULL;
    memset(out, 0, 32);
    memcpy(out + 32 - vl, v, vl);
    return p + len;
}

int sig_der_to_rs(const void *der, size_t n, unsigned char rs[64]) {
    const unsigned char *p = der, *e;
    if (!p || n < 8 || n > SIG_DER_MAX) return -1;
    if (p[0] != 0x30 || p[1] >= 0x80 || (size_t)p[1] != n - 2) return -1;
    e = p + n;
    p = der_int(p + 2, e, rs);
    if (!p) return -1;
    p = der_int(p, e, rs + 32);
    return p && p == e ? 0 : -1;
}

/* SEQUENCE { SEQUENCE { OID 1.2.840.10045.2.1, OID 1.2.840.10045.3.1.7 }, BIT STRING 00 04 X Y } */
static const unsigned char spki_prefix[27] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06,
    0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04,
};

int sig_spki_p256(const void *der, size_t n, unsigned char xy[64]) {
    if (!der || n != SIG_SPKI_LEN || memcmp(der, spki_prefix, sizeof spki_prefix)) return -1;
    memcpy(xy, (const unsigned char *)der + sizeof spki_prefix, 64);
    return 0;
}

static int b64(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

long sig_base64_decode(const char *s, unsigned char *out, size_t cap) {
    if (!s) return -1;
    size_t n = strlen(s), o = 0;
    if (n % 4) return -1;
    for (size_t i = 0; i < n; i += 4) {
        const unsigned char *q = (const unsigned char *)s + i;
        int last = i + 4 == n;
        int pad = (q[3] == '=') + (q[2] == '=' && q[3] == '=');
        if (pad && !last) return -1;
        int a = b64(q[0]), b = b64(q[1]), c = pad == 2 ? 0 : b64(q[2]), d = pad ? 0 : b64(q[3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
        /* the bits a padded group drops must be zero (Foundation refuses them too) */
        if ((pad == 2 && (b & 0x0f)) || (pad == 1 && (c & 0x03))) return -1;
        unsigned long v = (unsigned long)a << 18 | (unsigned long)b << 12 | (unsigned long)c << 6 | (unsigned long)d;
        size_t k = 3 - (size_t)pad;
        if (cap - o < k) return -1;
        out[o++] = (unsigned char)(v >> 16);
        if (k > 1) out[o++] = (unsigned char)(v >> 8);
        if (k > 2) out[o++] = (unsigned char)v;
    }
    return (long)o;
}

int sig_key_from_text(const sig_key_text *t, sig_pubkey *out) {
    unsigned char der[SIG_SPKI_LEN + 3];
    if (!t || !t->name || !out) return -1;
    long n = sig_base64_decode(t->der_base64, der, sizeof der);
    if (n < 0 || sig_spki_p256(der, (size_t)n, out->xy)) return -1;
    out->name = t->name;
    return 0;
}

int sig_signer(const void *data, size_t n, const void *sig, size_t sig_n, const sig_pubkey *keys, size_t nkeys) {
    unsigned char rs[64];
    if (!sig || sig_n == 0 || sig_der_to_rs(sig, sig_n, rs)) return -1;
    for (size_t i = 0; i < nkeys; i++) {
        int v = sig_p256_verify(keys[i].xy, data, n, rs);
        if (v < 0) return -2;
        if (v) return (int)i;
    }
    return -1;
}
