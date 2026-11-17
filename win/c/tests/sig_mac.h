/* The Mac tests' crypto (sig_mac.c): sig_p256_verify with Security.framework, and the tests' own
 * signer (keys made in memory, signatures in DER as openssl writes them). Test builds only. */
#ifndef RBSP_WIN_TEST_SIG_MAC_H
#define RBSP_WIN_TEST_SIG_MAC_H

#include "../sig.h"

typedef struct {
    void *priv;              /* SecKeyRef */
    unsigned char xy[64];    /* its public point */
} test_key;

int test_key_new(test_key *k);   /* 0, or -1 */
void test_key_free(test_key *k);
/* a DER signature over data: its length, or 0 */
size_t test_sign(const test_key *k, const void *data, size_t n, unsigned char der[SIG_DER_MAX]);

#endif
