/* sig.c: payload.json's signature, the Windows copy of Signing.swift (app.md §5.1, W1).
 *
 * The same scheme as the Mac: ECDSA P-256 with SHA-256 over payload.json's exact bytes, the
 * signature DER-encoded as openssl writes it, checked against the compiled-in public keys
 * (release and backup; a test build's test key). A key is the base64 of its DER
 * SubjectPublicKeyInfo, the string build-app.sh writes into Keys.swift, so one writer serves both
 * apps.
 *
 * Pure C11 except sig_p256_verify, the one crypto call: on Windows sig_bcrypt.c (BCrypt, loaded
 * from System32 at the first call, in the core DLL as in the worker); in the Mac tests
 * tests/sig_mac.c (Security.framework). Both take the raw signature r||s that sig_der_to_rs makes,
 * so the conversion is ours and the same on both. */
#ifndef RBSP_WIN_SIG_H
#define RBSP_WIN_SIG_H

#include <stddef.h>
#include <stdint.h>

/* A DER signature of P-256 is at most 72 bytes (0x30 len, two INTEGERs of up to 33 bytes). */
enum { SIG_DER_MAX = 72, SIG_SPKI_LEN = 91 };

/* A trusted key: its name for the log ("release", "backup", "test") and its point. */
typedef struct {
    const char *name;
    unsigned char xy[64];   /* X then Y, big-endian, 32 bytes each */
} sig_pubkey;

/* A key as compiled in: the name and the base64 of its DER SubjectPublicKeyInfo. */
typedef struct {
    const char *name;
    const char *der_base64;
} sig_key_text;

/* Strict DER ECDSA-Sig-Value { r INTEGER, s INTEGER } of P-256 into r||s (32 bytes each,
 * big-endian, left-padded). 0, or -1 for anything else: an empty or truncated input, a long-form
 * or wrong length, a negative, zero or non-minimal integer, one over 32 bytes, bytes after the
 * sequence. */
int sig_der_to_rs(const void *der, size_t n, unsigned char rs[64]);

/* The DER SubjectPublicKeyInfo of a P-256 key (id-ecPublicKey, prime256v1, an uncompressed point:
 * the 91 bytes CryptoKit's derRepresentation and `openssl ec -pubout -outform DER` write) into
 * its point. 0, or -1 for anything else. The point itself is checked by the crypto call. */
int sig_spki_p256(const void *der, size_t n, unsigned char xy[64]);

/* Strict base64 (RFC 4648, padding required, no whitespace), as Foundation's
 * Data(base64Encoded:) reads it: the decoded length, or -1 when it isn't base64 or doesn't fit
 * in cap bytes. */
long sig_base64_decode(const char *s, unsigned char *out, size_t cap);

/* TrustedKey(name:base64:): 0 with *out filled (out->name = t->name, not copied), or -1 unless it
 * is a P-256 public key's SPKI in base64. */
int sig_key_from_text(const sig_key_text *t, sig_pubkey *out);

/* signer(of:signature:keys:): the index in keys of the first key whose signature over data this
 * is, or -1 when none (an empty, malformed or truncated signature included), or -2 when the
 * system's crypto couldn't be used (Windows: bcrypt.dll didn't load). The DER is parsed before any
 * key is tried. */
int sig_signer(const void *data, size_t n, const void *sig, size_t sig_n, const sig_pubkey *keys, size_t nkeys);

/* The one crypto call (sig_bcrypt.c on Windows; tests/sig_mac.c in the Mac tests): whether rs is
 * a valid ECDSA P-256 signature with SHA-256 over data[0..n) by the key xy. 1 valid, 0 not (a key
 * that isn't a point on the curve included), -1 the crypto couldn't be used. */
int sig_p256_verify(const unsigned char xy[64], const void *data, size_t n, const unsigned char rs[64]);

#endif
