/* The worker tests' pe_code_sha256 (win/c/pe_code.h): the fixtures' "DLLs" are short texts, not
 * PE files, so their "code" is the bytes before " #cert " and their "certificate table" what
 * follows it: a re-signed twin (fake_os.c's ort of another version) has the same code digest and
 * another file sha256 (D2a). No " #cert ": -1, never bound. The worker reads the digest only
 * through this function, so the real one (lane B0) changes nothing here. */
#include "../../c/pe_code.h"

#include <stdlib.h>
#include <string.h>

int pe_code_sha256(compat_read_fn read, void *ctx, uint64_t size, payload_sha out) {
    out[0] = 0;
    if (size < 2 || size > 1024 * 1024) return -1;
    unsigned char *b = malloc((size_t)size);
    if (!b || read(ctx, 0, b, (size_t)size) || memcmp(b, "MZ", 2)) {
        free(b);
        return -1;
    }
    const unsigned char *cert = NULL;
    for (size_t i = 0; i + 7 <= (size_t)size && !cert; i++)
        if (!memcmp(b + i, " #cert ", 7)) cert = b + i;
    unsigned char d[32];
    if (!cert || plat_sha256(b, (size_t)(cert - b), d)) {
        free(b);
        return -1;
    }
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[2 * i] = h[d[i] >> 4], out[2 * i + 1] = h[d[i] & 15];
    out[64] = 0;
    free(b);
    return 0;
}
