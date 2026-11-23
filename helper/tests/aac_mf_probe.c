/* test tool (Windows, T10, Q3): each FILE decoded by aac_mf.c 11 times in this process (async as
 * rekordbox reads, three times; blocking reads at 8 stack shifts, 0-112 bytes), the async output
 * written to OUT/<name>.s16 (16-bit interleaved, little-endian), and one line per file:
 *   name, frames, rate, ch, bits, the stream's own subtype (Data1), rate and ch, MF_PD_DURATION,
 *   the first and last sample times (100 ns), MF samples, gaps, streams, the stream read, typed,
 *   the distinct outputs of the 11, the async output's SHA-256, and its media type changes over
 *   the 11 decodes (tab-separated) or "name<TAB>skipped: why".
 * Run it in separate processes to compare runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rbsp.h"
#include "../aac_mf.h"
#include "big.h"
#include "plat.h"
#include "plat_helper.h"

#define RUNS 11

static const char *base_name(const char *p) {
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\'), *s = a > b ? a : b;
    return s ? s + 1 : p;
}

int main(int argc, char **argv) {
    if (plat_exe_start()) { fprintf(stderr, "plat_exe_start failed\n"); return 1; }
    if (argc < 3) { fprintf(stderr, "usage: %s OUT FILE...\n", argv[0]); return 2; }
    for (int i = 2; i < argc; i++) {
        const char *name = base_name(argv[i]);
        unsigned char dg[RUNS][32];
        int distinct = 0, failed = 0;
        size_t tc = 0;
        aac_mf_info first = {0};
        size_t first_n = 0;
        char why[300];
        for (int k = 0; k < RUNS && !failed; k++) {
            int async = k < 3;
            aac_mf_info in;
            int16_t *pcm;
            size_t n;
            if (aac_mf_decode(argv[i], async, async ? 0 : (size_t)(k - 3) * 16, &pcm, &n, &in, why, sizeof why)) {
                printf("%s\tskipped: %s\n", name, why);
                failed = 1;
                break;
            }
            tc += in.typechanges;
            unsigned char d[32];
            plat_sha256(pcm, n * 2 * (size_t)in.ch, d);
            int seen = 0;
            for (int j = 0; j < distinct; j++)
                if (!memcmp(dg[j], d, 32)) seen = 1;
            if (!seen) memcpy(dg[distinct++], d, 32);
            if (k == 0) {
                first = in;
                first_n = n;
                char out[2048];
                snprintf(out, sizeof out, "%s\\%s.s16", argv[1], name);
                FILE *f = plat_fopen_w(out, 0);
                if (!f || fwrite(pcm, 2 * (size_t)in.ch, n, f) != n) { fprintf(stderr, "%s: can't write\n", out); return 1; }
                fclose(f);
            }
            big_free(pcm);
        }
        if (failed) continue;
        printf("%s\t%zu\t%d\t%d\t%d\t0x%x\t%d\t%d\t%lld\t%lld\t%lld\t%zu\t%zu\t%d\t%d\t%d\t%d\t", name, first_n, first.rate,
               first.ch, first.bits, (unsigned)first.native_sub, first.native_rate, first.native_ch, first.duration,
               first.first_ts, first.last_ts, first.samples, first.gaps, first.streams, first.stream, first.typed, distinct);
        for (int j = 0; j < 32; j++) printf("%02x", dg[0][j]);
        printf("\t%zu\n", tc);
        fflush(stdout);
    }
    return 0;
}
