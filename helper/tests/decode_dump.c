/* test tool: decode each FILE as the helper does and write its tracks to OUT/<name>.<i>.f32
 * (planar stereo float32); one line per file on stdout */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rbsp.h"
#ifdef _WIN32
#include "plat_helper.h"                 /* built by helper/build-win.sh as rbsp-decode-dump.exe */
#define SEP "\\"
/* the name after the last / or \ (libgen's would link mingw-w64's own code page conversion) */
static char *basename(char *p) {
    char *a = strrchr(p, '/'), *b = strrchr(p, '\\'), *s = a > b ? a : b;
    return s ? s + 1 : p;
}
#else
#include <libgen.h>
#define SEP "/"
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    if (plat_exe_start()) { fprintf(stderr, "plat_exe_start failed\n"); return 1; }
#endif
    if (argc < 4) { fprintf(stderr, "usage: %s REKORDBOX_APP OUT FILE...\n", argv[0]); return 2; }
    for (int i = 3; i < argc; i++) {
        tracks ts;
        char why[300];
        char *b = strdup(argv[i]), *name = basename(b);
        if (load_tracks(argv[i], argv[1], &ts, why, sizeof why)) { printf("%s\tskipped: %s\n", name, why); free(b); continue; }
        printf("%s\t%d tracks\t%s", name, ts.count, ts.how);
        for (int t = 0; t < ts.count; t++) {
            char p[2048];
            snprintf(p, sizeof p, "%s" SEP "%s.%d.f32", argv[2], name, t);
            const track *tr = &ts.t[t];
            float *s = tr->s;
            if (!s) {                    /* 16-bit: as chunk_at reads it */
                s = malloc(sizeof(float) * 2 * tr->n);
                size_t ch = (size_t)tr->ch, c1 = ch > 1 ? 1 : 0;
                for (size_t j = 0; j < tr->n; j++) {
                    s[j] = tr->p16[j * ch] * (1.0f / 32768.0f);
                    s[tr->n + j] = tr->p16[j * ch + c1] * (1.0f / 32768.0f);
                }
            }
            FILE *f = fopen(p, "wb");
            if (!f || fwrite(s, sizeof(float), 2 * tr->n, f) != 2 * tr->n) { perror(p); return 1; }
            fclose(f);
            if (s != tr->s) free(s);
            printf("\t%zu", ts.t[t].n);
        }
        printf("\n");
        fflush(stdout);
        tracks_free(&ts);
        free(b);
    }
    return 0;
}
