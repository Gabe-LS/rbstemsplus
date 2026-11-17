/* test tool for helper/tests/win-mp3.sh: mp3.c's Windows path (RBSP_WIN_DECODE, on common/plat.h)
 * against the Mac helper's own build, on this Mac.
 *
 * Built twice:
 *  - without RBSP_WIN_DECODE (the reference): links mp3.c as helper/build.sh compiles it and
 *    #includes decode.c for its AudioToolbox probe.
 *      dec LIB FILE OUT    rb_mp3: "dec ok N" (OUT: planar float32) or "dec refused WHY"
 *      at FILE             decode.c's probe: "at ok RATE CH SECONDS" or "at refused"
 *  - with RBSP_WIN_DECODE: #includes mp3.c built that way (its statics: the frame map oracle).
 *      dec LIB FILE OUT    as above, the Windows path's rb_mp3
 *      check LIB FILE OUT  dec, then mp3_probe against the frame map and the decode: one ok/FAIL
 *                          line per check, and "probe ok RATE CH SECONDS APPROX READS BYTES US"
 *      fuzz FILE N SEED TMP   N variants of FILE (cuts, flipped bytes, garbage, ID3 sizes): the
 *                          probe refuses exactly when the frame map is empty, the same rate and
 *                          channels, never a read past the size; one ok/FAIL line
 * Lines are tab-separated; the exit status is 1 when a check fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(RBSP_WIN_DECODE)
#include "../mp3.c"

/* pcm.c's, counted, and checked against the size the probe was given */
static int64_t g_size = -1;
static long g_reads, g_past;
static int64_t g_bytes;
int pcm_pread_all(plat_file f, void *b, size_t n, int64_t off) {
    g_reads++;
    g_bytes += (int64_t)n;
    if (g_size >= 0 && (off < 0 || off + (int64_t)n > g_size)) g_past++;
    size_t got = 0, g;
    while (got < n) {
        if (plat_pread(f, (char *)b + got, n - got, off + (int64_t)got, &g) || !g) return 1;
        got += g;
    }
    return 0;
}

static int fails;
static void check(int ok, const char *name, const char *what) {
    printf("%s\t%s\t%s\n", ok ? "ok" : "FAIL", name, what);
    if (!ok) fails++;
}

/* the oracle: rekordbox's frame map of the whole file (analyze), as rb_mp3 reads it */
typedef struct { int read, rate, ch; size_t nf, spf; } fmap;
static fmap frame_map(const unsigned char *d, size_t n) {
    fmap r = {1, 0, 0, 0, 0};
    buf b = {d, n};
    size_t nf, H;
    frame *F = analyze(&b, &nf, &H);
    if (F && nf) {
        const unsigned char *h = d + F[0].off;
        int ver = (h[1] >> 3) & 1;
        r.rate = (ver ? M1_SR : M2_SR)[(h[2] >> 2) & 3];
        r.ch = (h[3] >> 6) == 3 ? 1 : 2;
        r.spf = ver ? 1152 : 576;
        r.nf = nf;
    }
    free(F);
    return r;
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(l > 0 ? (size_t)l : 1);
    *n = l > 0 ? (size_t)l : 0;
    if (d && *n && fread(d, 1, *n, f) != *n) { free(d); d = NULL; }
    fclose(f);
    return d;
}

static int run_probe(const char *path, pcm_info *p, char *why, size_t whyn, double *us) {
    plat_file f;
    plat_stat st;
    if (plat_file_open_path(path, &f)) { snprintf(why, whyn, "open failed"); return 2; }
    if (plat_file_stat(f, &st)) { plat_file_close(f); snprintf(why, whyn, "stat failed"); return 2; }
    g_size = st.size;
    g_reads = 0;
    g_bytes = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = mp3_probe(f, st.size, p, why, whyn);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_size = -1;
    plat_file_close(f);
    if (us) *us = (double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;
    return rc;
}

static double tol(double s) { return s * 0.01 > 0.1 ? s * 0.01 : 0.1; }

static const char NOMAP[] = "MP3 the frame scanner rejects: rekordbox finds no frame map";
#else
#include "../decode.c"
#endif

static int dec(const char *lib, const char *path, const char *out, size_t *n_out, char *why, size_t whyn) {
    float *s;
    size_t n;
    if (rb_mp3(path, lib, &s, &n, why, whyn)) { printf("dec\trefused\t%s\n", why); return 1; }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(s, sizeof(float), 2 * n, f) != 2 * n) { perror(out); exit(2); }
    fclose(f);
    big_free(s);
    printf("dec\tok\t%zu\n", n);
    *n_out = n;
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    char why[300] = "";
    size_t n = 0;
    if (argc == 5 && !strcmp(argv[1], "dec")) return dec(argv[2], argv[3], argv[4], &n, why, sizeof why) ? 3 : 0;
#if !defined(RBSP_WIN_DECODE)
    if (argc == 3 && !strcmp(argv[1], "at")) {
        probe_info p;
        if (probe(argv[2], &p)) printf("at\trefused\n");
        else printf("at\tok\t%d\t%d\t%.6f\n", p.rate, p.ch, p.seconds);
        return 0;
    }
#else
    if (argc == 5 && !strcmp(argv[1], "check")) {
        const char *path = argv[3];
        int drc = dec(argv[2], path, argv[4], &n, why, sizeof why);
        size_t len;
        unsigned char *d = slurp(path, &len);
        fmap m = d && len ? frame_map(d, len) : (fmap){0, 0, 0, 0, 0};
        free(d);
        pcm_info p;
        char pwhy[300] = "";
        double us = 0;
        int prc = run_probe(path, &p, pwhy, sizeof pwhy, &us);
        if (prc == 2) { check(0, "probe", pwhy); return 1; }
        check(!g_past, "probe-bounds", "no read past the size");
        int mapped = m.nf > 0;
        if (prc) printf("probe\trefused\t%s\n", pwhy);
        else printf("probe\tok\t%d\t%d\t%.6f\t%d\t%ld\t%lld\t%.0f\n", p.rate, p.ch, p.seconds, p.approx, g_reads, (long long)g_bytes, us);
        char w[400];
        snprintf(w, sizeof w, "probe %s, frame map %zu frames", prc ? "refuses" : "accepts", m.nf);
        /* the probe refuses exactly what rekordbox's frame map refuses: a refusal of a file rb_mp3
         * maps would stop load_tracks before rb_mp3 (refused() runs the probe on the load path) */
        check((prc == 0) == mapped, "probe-refusal", w);
        if (prc && !mapped) check(!strcmp(pwhy, NOMAP) || !len, "probe-why", pwhy);
        if (!drc && !mapped) check(0, "dec-map", "rb_mp3 decoded without a frame map?");
        if (!drc) check(n == m.nf * m.spf, "dec-length", "rb_mp3's n is the map's nf x spf");
        if (drc && mapped) printf("note\trb_mp3 refuses after its frame map\t%s\n", why);
        if (drc && !mapped) check(!strcmp(why, NOMAP) || !len, "dec-why", why);
        if (!prc && mapped) {
            snprintf(w, sizeof w, "%d Hz %d ch, rb_mp3 %d Hz %d ch", p.rate, p.ch, m.rate, m.ch);
            check(p.rate == m.rate && p.ch == m.ch, "probe-format", w);
            double s = (double)(m.nf * m.spf) / m.rate;
            snprintf(w, sizeof w, "%.3f s, decoded %.3f s (%+.3f, tolerance %.3f)%s", p.seconds, s, p.seconds - s, tol(s),
                     p.approx ? " approx" : "");
            check(p.seconds - s <= tol(s) && s - p.seconds <= tol(s), "probe-seconds", w);
            check(p.kind == PCM_MP3, "probe-kind", "PCM_MP3");
        }
        return fails ? 1 : 0;
    }
    if (argc == 6 && !strcmp(argv[1], "fuzz")) {
        size_t len;
        unsigned char *d0 = slurp(argv[2], &len);
        int N = atoi(argv[3]);
        srand((unsigned)atoi(argv[4]));
        const char *tmp = argv[5];
        if (!d0) { check(0, "fuzz", "can't read the fixture"); return 1; }
        if (!len) { check(1, "fuzz", "an empty fixture: nothing to vary"); return 0; }
        enum { PRE = 200000 };
        unsigned char *d = malloc(len + PRE);
        long refusal = 0, format = 0, past = 0, secs = 0, accepted = 0, refused = 0;
        for (int i = 0; i < N; i++) {
            size_t l = len;
            memcpy(d, d0, len);
            int kind = i % 7;
            if (kind == 0) l = (size_t)rand() % (len + 1);                   /* a cut */
            else if (kind == 1) { for (int k = 0; k < 1 + rand() % 8; k++) d[(size_t)rand() % len] = (unsigned char)rand(); }
            else if (kind == 2) {                                            /* garbage over the start */
                size_t g = (size_t)rand() % (len < 20000 ? len : 20000);
                for (size_t k = 0; k < g; k++) d[k] = (unsigned char)rand();
            } else if (kind == 3 && len > 10) {                              /* an ID3v2 header with any size */
                memcpy(d, "ID3", 3);
                d[3] = (unsigned char)(rand() % 2 ? 3 : rand() % 6);
                d[4] = (unsigned char)(rand() % 2);
                uint32_t z = rand() % 2 ? (uint32_t)rand() % (uint32_t)len : (uint32_t)rand();
                for (int k = 0; k < 4; k++) d[6 + k] = (unsigned char)((z >> (21 - 7 * k)) & 0x7F);
            } else if (kind == 4) {                                          /* a cut, then garbage at the end */
                l = (size_t)rand() % (len + 1);
                size_t g = (size_t)rand() % 4096;
                if (l + g > len) g = len - l;
                for (size_t k = 0; k < g; k++) d[l + k] = (unsigned char)rand();
                l += g;
            } else if (kind == 5) {                                          /* a few headers broken near the start */
                for (int k = 0; k < 6; k++) { size_t q = (size_t)rand() % (len < 8192 ? len : 8192); d[q] ^= (unsigned char)(1 << (rand() % 8)); }
            } else if (kind == 6) {                                          /* shifted: zeros, garbage or an ID3v2 tag first */
                size_t g = (size_t)rand() % PRE, how = (size_t)rand() % 3;
                memmove(d + g, d, len);
                for (size_t k = 0; k < g; k++) d[k] = how == 1 ? (unsigned char)rand() : 0;
                if (how == 2 && g >= 10) {
                    memcpy(d, "ID3\3\0\0", 6);
                    for (int k = 0; k < 4; k++) d[6 + k] = (unsigned char)(((g - 10) >> (21 - 7 * k)) & 0x7F);
                }
                l = len + g;
            }
            FILE *f = fopen(tmp, "wb");
            if (!f || (l && fwrite(d, 1, l, f) != l)) { perror(tmp); return 2; }
            fclose(f);
            fmap m = l ? frame_map(d, l) : (fmap){1, 0, 0, 0, 0};
            pcm_info p;
            char pw[300];
            int prc = run_probe(tmp, &p, pw, sizeof pw, NULL);
            if (prc == 2) { check(0, "fuzz", pw); return 1; }
            if (g_past) past++;
            g_past = 0;
            if ((prc == 0) != (m.nf > 0)) {
                refusal++;
                fprintf(stderr, "fuzz %d (kind %d, %zu bytes): probe %s (%s), map %zu frames\n", i, kind, l, prc ? "refuses" : "accepts", prc ? pw : "", m.nf);
            }
            if (!prc && m.nf) {
                accepted++;
                if (p.rate != m.rate || p.ch != m.ch) format++;
                double s = (double)(m.nf * m.spf) / m.rate;
                if (p.seconds - s > tol(s) || s - p.seconds > tol(s)) {
                    secs++;
                    fprintf(stderr, "fuzz %d (kind %d, %zu bytes): seconds %.3f, decoded %.3f%s\n", i, kind, l, p.seconds, s, p.approx ? " approx" : "");
                }
            } else if (prc) {
                refused++;
            }
        }
        char w[300];
        snprintf(w, sizeof w, "%d variants: %ld accepted, %ld refused; refusal mismatches %ld, format %ld, reads past the size %ld; seconds out of tolerance %ld",
                 N, accepted, refused, refusal, format, past, secs);
        check(!refusal && !format && !past, "fuzz", w);
        free(d);
        free(d0);
        return fails ? 1 : 0;
    }
#endif
    fprintf(stderr, "usage: see helper/tests/win-mp3.c\n");
    return 2;
}
