/* The test crypto shim (app.md §7): sig_p256_verify for the Mac-native tests, with the Mac's own
 * Security.framework (an OS component). It takes the same raw r||s that sig_bcrypt.c hands to
 * BCryptVerifySignature (kSecKeyAlgorithmECDSASignatureMessageRFC4754SHA256 is ECDSA with SHA-256
 * over the message, the signature as r||s), so the DER conversion under test is the one Windows
 * runs. Test builds only. */
#include <Security/Security.h>

#include "sig_mac.h"

int sig_p256_verify(const unsigned char xy[64], const void *data, size_t n, const unsigned char rs[64]) {
    unsigned char point[65];
    point[0] = 0x04;
    memcpy(point + 1, xy, 64);
    CFDataRef pd = CFDataCreate(NULL, point, sizeof point);
    const void *k[] = {kSecAttrKeyType, kSecAttrKeyClass};
    const void *v[] = {kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClassPublic};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SecKeyRef key = pd && attrs ? SecKeyCreateWithData(pd, attrs, NULL) : NULL;
    if (pd) CFRelease(pd);
    if (attrs) CFRelease(attrs);
    if (!key) return 0;   /* not a point on the curve: this key verifies nothing */
    CFDataRef msg = CFDataCreate(NULL, data ? data : (const void *)"", (CFIndex)n);
    CFDataRef sig = CFDataCreate(NULL, rs, 64);
    int ok = msg && sig && SecKeyVerifySignature(key, kSecKeyAlgorithmECDSASignatureMessageRFC4754SHA256, msg, sig, NULL);
    if (msg) CFRelease(msg);
    if (sig) CFRelease(sig);
    CFRelease(key);
    return ok ? 1 : 0;
}

/* -- the tests' own signer: a P-256 key in memory, and DER signatures as openssl writes them */

int test_key_new(test_key *k) {
    int bits = 256;
    CFNumberRef nb = CFNumberCreate(NULL, kCFNumberIntType, &bits);
    const void *ak[] = {kSecAttrKeyType, kSecAttrKeySizeInBits};
    const void *av[] = {kSecAttrKeyTypeECSECPrimeRandom, nb};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SecKeyRef priv = SecKeyCreateRandomKey(attrs, NULL);
    CFRelease(attrs);
    CFRelease(nb);
    if (!priv) return -1;
    SecKeyRef pub = SecKeyCopyPublicKey(priv);
    CFDataRef ext = pub ? SecKeyCopyExternalRepresentation(pub, NULL) : NULL;
    int ok = ext && CFDataGetLength(ext) == 65;
    if (ok) memcpy(k->xy, CFDataGetBytePtr(ext) + 1, 64);
    if (ext) CFRelease(ext);
    if (pub) CFRelease(pub);
    if (!ok) {
        CFRelease(priv);
        return -1;
    }
    k->priv = (void *)priv;
    return 0;
}

void test_key_free(test_key *k) {
    if (k->priv) CFRelease((SecKeyRef)k->priv);
    k->priv = NULL;
}

size_t test_sign(const test_key *k, const void *data, size_t n, unsigned char der[SIG_DER_MAX]) {
    CFDataRef msg = CFDataCreate(NULL, data ? data : (const void *)"", (CFIndex)n);
    CFDataRef sig = SecKeyCreateSignature((SecKeyRef)k->priv, kSecKeyAlgorithmECDSASignatureMessageX962SHA256, msg, NULL);
    CFRelease(msg);
    if (!sig) return 0;
    size_t len = (size_t)CFDataGetLength(sig);
    if (len > SIG_DER_MAX) len = 0;
    else memcpy(der, CFDataGetBytePtr(sig), len);
    CFRelease(sig);
    return len;
}
