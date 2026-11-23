/* Focused tests of the Windows PCM readers, with failures injected at plat_pread. */
#include "pcm.h"
#include <assert.h>
#include <string.h>

static int64_t fail_at = -1;
static int headers_only;

static int test_pread(plat_file f, void *buf, size_t n, int64_t off, size_t *got) {
    if (off == fail_at || (headers_only && n > 42)) return EIO;
    return plat_pread(f, buf, n, off, got);
}

#define plat_pread test_pread
#include "../pcm.c"
#undef plat_pread

static void check(int ok, const char *name) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) exit(1);
}

static void wav(const char *path) {
    plat_file f;
    plat_stat st;
    pcm_info p;
    char why[160];
    assert(!plat_file_open_at(plat_dir_cwd(), path, INT64_MAX, NULL, &f));
    assert(!plat_file_stat(f, &st));
    check(!pcm_probe_wav(f, st.size, &p, why, sizeof why) && p.rate == 48000, "later WAV format wins");
    fail_at = 46;
    check(pcm_probe_wav(f, st.size, &p, why, sizeof why), "failed in-bounds WAV chunk header");
    fail_at = 54;
    check(pcm_probe_wav(f, st.size, &p, why, sizeof why), "failed in-bounds WAV format body");
    fail_at = -1;
    check(!pcm_probe_wav(f, 45, &p, why, sizeof why) && p.frames == 0, "WAV data clipped at end of file");
    plat_file_close(f);
    puts("pcm-host WAV: 4 passed, 0 failed");
}

static int wav_float(int argc, char **paths) {
    int failed = 0;
    const float samples[] = {1.5f, -0.25f, 1.5f, -0.25f};  /* mono, copied to both sides */
    for (int i = 0; i < argc; i++) {
        const char *slash = strrchr(paths[i], '/');
        const char *name = slash ? slash + 1 : paths[i];
        plat_file f;
        pcm_info p;
        char why[160] = "";
        assert(!plat_file_open_at(plat_dir_cwd(), paths[i], INT64_MAX, NULL, &f));
        int rc = pcm_probe(f, ".wav", &p, why, sizeof why);
        int ok;
        if (strstr(name, "f64")) {
            ok = rc == 1 && p.kind == PCM_WAV && !strcmp(why, "a 64-bit float WAV: rekordbox can't load it");
        } else {
            pcm_out o;
            ok = !rc && p.kind == PCM_WAV && p.flt == 32 && p.frames == 2 &&
                 !pcm_decode(f, &p, &o, why, sizeof why);
            if (ok) {
                ok = o.n == 2 && !memcmp(o.s, samples, sizeof samples);
                pcm_out_free(&o);
            }
        }
        printf("%s %s\n", ok ? "ok  " : "FAIL", name);
        failed += !ok;
        plat_file_close(f);
    }
    printf("pcm-host float WAV: %d passed, %d failed\n", argc - failed, failed);
    return failed != 0;
}

static void flac(const char *path, const char *reference) {
    plat_file f, known;
    pcm_info p, k;
    pcm_out o, expected;
    char why[160];
    assert(!plat_file_open_at(plat_dir_cwd(), reference, INT64_MAX, NULL, &known));
    headers_only = 1;
    check(!pcm_probe(known, ".flac", &k, why, sizeof why) && k.frames == 44100, "known FLAC length needs only headers");
    headers_only = 0;
    assert(!pcm_decode(known, &k, &expected, why, sizeof why));
    plat_file_close(known);
    assert(!plat_file_open_at(plat_dir_cwd(), path, INT64_MAX, NULL, &f));
    check(!pcm_probe(f, ".flac", &p, why, sizeof why) && p.frames == 44100 && p.seconds == 1.0,
          "FLAC with unknown length probes as one second");
    check(!pcm_decode(f, &p, &o, why, sizeof why) && o.n == expected.n &&
          !memcmp(o.s, expected.s, 2 * o.n * sizeof *o.s), "unknown-length FLAC decodes the same samples");
    pcm_out_free(&o);
    pcm_out_free(&expected);
    plat_file_close(f);
    puts("pcm-host FLAC: 3 passed, 0 failed");
}

int alac_decode(plat_file f, int64_t size, const pcm_info *p, pcm_out *o, char *why, size_t whyn) {
    (void)f, (void)size, (void)p, (void)o, (void)why, (void)whyn;
    abort();
}

int main(int argc, char **argv) {
    assert(argc == 12);
    wav(argv[1]);
    flac(argv[2], argv[3]);
    return wav_float(argc - 4, argv + 4);
}
