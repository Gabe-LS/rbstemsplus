/* sig.c beyond the shared fixtures: the DER and base64 edges, and round trips with keys made here. */
#include "check.h"

#include "sig_mac.h"

static int der(const unsigned char *d, size_t n, unsigned char rs[64]) { return sig_der_to_rs(d, n, rs); }

void test_sig(void) {
    unsigned char rs[64];

    /* -- DER by hand */
    static const unsigned char small[] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x02};
    CHECK(der(small, sizeof small, rs) == 0 && rs[31] == 1 && rs[63] == 2 && rs[0] == 0 && rs[32] == 0, "r = 1, s = 2");
    static const unsigned char signbyte[] = {0x30, 0x07, 0x02, 0x02, 0x00, 0x80, 0x02, 0x01, 0x02};
    CHECK(der(signbyte, sizeof signbyte, rs) == 0 && rs[31] == 0x80, "r = 0x80 with its sign byte");
    static const unsigned char nosign[] = {0x30, 0x06, 0x02, 0x01, 0x80, 0x02, 0x01, 0x02};
    CHECK(der(nosign, sizeof nosign, rs) == -1, "r = 0x80 without its sign byte: negative");
    static const unsigned char pad[] = {0x30, 0x07, 0x02, 0x02, 0x00, 0x01, 0x02, 0x01, 0x02};
    CHECK(der(pad, sizeof pad, rs) == -1, "a needless leading zero");
    static const unsigned char zero[] = {0x30, 0x06, 0x02, 0x01, 0x00, 0x02, 0x01, 0x02};
    CHECK(der(zero, sizeof zero, rs) == -1, "r = 0");
    static const unsigned char empty_int[] = {0x30, 0x05, 0x02, 0x00, 0x02, 0x01, 0x02};
    CHECK(der(empty_int, sizeof empty_int, rs) == -1, "an INTEGER of no bytes");
    CHECK(der(small, sizeof small - 1, rs) == -1 && der(small, 0, rs) == -1 && der(NULL, 8, rs) == -1, "truncated, empty, NULL");
    unsigned char big[2 + 2 * 35];   /* two 33-byte INTEGERs, sign bytes: the largest, 72 bytes */
    big[0] = 0x30;
    big[1] = 2 * 35;
    for (int i = 0; i < 2; i++) {
        unsigned char *p = big + 2 + 35 * i;
        p[0] = 0x02, p[1] = 33, p[2] = 0;
        memset(p + 3, 0xab, 32);
    }
    CHECK(der(big, sizeof big, rs) == 0 && rs[0] == 0xab && rs[63] == 0xab, "the largest form (72 bytes)");
    big[3] = 34;
    CHECK(der(big, sizeof big, rs) == -1, "an INTEGER of 34 bytes");

    /* -- base64, as Foundation reads it */
    unsigned char buf[16];
    CHECK(sig_base64_decode("", buf, sizeof buf) == 0, "empty");
    CHECK(sig_base64_decode("YQ==", buf, sizeof buf) == 1 && buf[0] == 'a', "one byte");
    CHECK(sig_base64_decode("YWI=", buf, sizeof buf) == 2 && !memcmp(buf, "ab", 2), "two bytes");
    CHECK(sig_base64_decode("YWJj", buf, sizeof buf) == 3 && !memcmp(buf, "abc", 3), "three bytes");
    CHECK(sig_base64_decode("YR==", buf, sizeof buf) == -1, "padding over bits that aren't zero");
    CHECK(sig_base64_decode("YWJ=YWJj", buf, sizeof buf) == -1, "padding before the end");
    CHECK(sig_base64_decode("Y===", buf, sizeof buf) == -1 && sig_base64_decode("=AAA", buf, sizeof buf) == -1, "too much padding");
    CHECK(sig_base64_decode("YWJjZA", buf, sizeof buf) == -1, "padding missing");
    CHECK(sig_base64_decode("YWJjZGVm", buf, 5) == -1, "over the buffer");
    CHECK(sig_base64_decode("YW=j", buf, sizeof buf) == -1, "\"=\" then a character");

    /* -- round trips: keys made here, signatures as openssl writes them */
    test_key k[3];
    int made = 1;
    for (int i = 0; i < 3; i++) made &= test_key_new(&k[i]) == 0;
    CHECK(made, "three keys made");
    if (!made) return;
    sig_pubkey keys[2] = {{"release", {0}}, {"backup", {0}}};
    memcpy(keys[0].xy, k[0].xy, 64);
    memcpy(keys[1].xy, k[1].xy, 64);
    int fails = 0, short_seen = 0, long_seen = 0;
    for (int i = 0; i < 300; i++) {
        char msg[64];
        int n = snprintf(msg, sizeof msg, "message %d", i);
        unsigned char d[SIG_DER_MAX];
        size_t dn = test_sign(&k[i % 3], msg, (size_t)n, d);
        short_seen += dn < 70;
        long_seen += dn == 72;
        int who = sig_signer(msg, (size_t)n, d, dn, keys, 2);
        int want = i % 3 == 2 ? -1 : i % 3;
        if (who != want) fails++;
        msg[0] ^= 1;
        if (sig_signer(msg, (size_t)n, d, dn, keys, 2) != -1) fails++;
    }
    CHECK(fails == 0, "300 signatures: each verifies with its own key only (%d wrong)", fails);
    CHECK(short_seen + long_seen > 0, "short and sign-byte forms came up (%d, %d)", short_seen, long_seen);
    /* a key that isn't a point on the curve verifies nothing */
    sig_pubkey bad = {"bad", {0}};
    memset(bad.xy, 0x11, 64);
    unsigned char d[SIG_DER_MAX];
    size_t dn = test_sign(&k[0], "x", 1, d);
    CHECK(sig_signer("x", 1, d, dn, &bad, 1) == -1, "a point off the curve");
    CHECK(sig_signer("x", 1, d, dn, keys, 0) == -1 && sig_signer("x", 1, NULL, 0, keys, 2) == -1, "no keys; no signature");
    for (int i = 0; i < 3; i++) test_key_free(&k[i]);
}
