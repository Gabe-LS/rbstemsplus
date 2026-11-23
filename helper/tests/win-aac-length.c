/* The Windows AAC track assembly with deterministic MF output and synthetic MP4 headers. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aac_mf.h"
#include "pcm.h"
#include "rbsp.h"

static size_t mf_frames;
static int16_t *mf_pcm;
static int mf_rate, mf_ch;
static int mf_stream, mf_probes;

int aac_mf_available(char *why, size_t whyn) { (void)why; (void)whyn; return 1; }

int aac_mf_probe(plat_file f, const char *ext, aac_mf_info *info, char *why, size_t whyn) {
    (void)f; (void)ext; (void)why; (void)whyn;
    mf_probes++;
    *info = (aac_mf_info){.stream = mf_stream, .native_sub = 0x1610, .native_rate = mf_rate,
                         .native_ch = mf_ch, .rate = mf_rate, .ch = mf_ch, .duration = 200000000};
    return 0;
}

int pcm_pread_all(plat_file f, void *buf, size_t n, int64_t off) {
    return pread(f.fd, buf, n, off) != (ssize_t)n;
}

static int fake_aac_decode_all(plat_file f, const char *ext, int stream, aac_outputs *o, char *why, size_t whyn) {
    (void)f; (void)ext; (void)why; (void)whyn;
    if (stream >= 0 && stream != mf_stream) return 1;
    *o = (aac_outputs){.pcm = {mf_pcm}, .frames = {mf_frames}, .count = 1, .rate = mf_rate, .ch = mf_ch};
    return 0;
}

void aac_outputs_free(aac_outputs *o) { free(o->pcm[0]); }

#define aac_decode_all fake_aac_decode_all
#include "aac-length.inc"
#include "aac-chunk.inc"

static int check(const char *dir, const char *name, int output_rate, int ch, size_t decoded,
                 uint64_t table, size_t written, int stream, uint32_t track_id, const char *message) {
    char path[4096], why[200] = "";
    snprintf(path, sizeof path, "%s/%s", dir, name);
    const char *ext = strrchr(name, '.');
    plat_file f = {.fd = open(path, O_RDONLY)};
    struct stat st;
    if (fstat(f.fd, &st)) { perror(path); return 1; }
    mf_stream = stream;
    mf_probes = 0;
    mf_rate = output_rate;
    mf_ch = ch;
    pcm_info p;
    if (m4a_probe(f, st.st_size, ext, &p, why, sizeof why)) {
        fprintf(stderr, "FAIL AAC %s: %s\n", name, why);
        close(f.fd);
        return 1;
    }
    int probe_ok = p.kind == PCM_AAC && p.rate == output_rate && p.ch == ch && p.frames == table &&
                   p.seconds == (double)table / output_rate && p.mp4_track_id == track_id &&
                   p.mf_stream == stream && mf_probes == (stream >= 0);
    mf_frames = decoded;
    size_t capacity = decoded > table ? decoded : (size_t)table;
    mf_pcm = calloc(capacity * ch, sizeof *mf_pcm);
    if (!mf_pcm) { close(f.fd); return 1; }
    mf_pcm[0] = 1234;
    mf_pcm[1] = -2345;
    mf_pcm[2112 * ch] = 6789;
    mf_pcm[(decoded - 1) * ch] = 3456;
    tracks ts = {0};
    int rate = 0;
    int rc = decode_aac(f, ext, &p, &ts, &rate, why, sizeof why);
    close(f.fd);
    track t = ts.t[0];
    int decode_ok = !rc && rate == output_rate && ts.count == 1 && t.n == written && t.owner && t.ch == ch &&
                    t.p16 == mf_pcm && t.p16[0] == 1234 && t.p16[1] == -2345 && !strcmp(ts.how, message) &&
                    ts.aac_length == (decoded < table ? AAC_LENGTH_SHORT : AAC_LENGTH_COMPLETE);
    if (!probe_ok || !decode_ok) {
        fprintf(stderr, "FAIL AAC %s: %d MF readers, stream %d, probe %d Hz/%d ch, %llu frames; decode %zu frames, %s%s\n",
                name, mf_probes, p.mf_stream, p.rate, p.ch, (unsigned long long)p.frames, t.n, ts.how, why);
        free(mf_pcm);
        return 1;
    }
    if (decoded < table) {
        size_t bytes = 2 * CHUNK_L * sizeof(float);
        float *short_chunk = malloc(bytes), *full_chunk = malloc(bytes);
        if (!short_chunk || !full_chunk) {
            free(short_chunk);
            free(full_chunk);
            free(mf_pcm);
            return 1;
        }
        track full = {.p16 = mf_pcm, .ch = ch, .n = (size_t)table};
        for (size_t i = decoded; i < table; i++) mf_pcm[i * ch] = 4567;
        chunk_at(&t, 0, short_chunk);
        chunk_at(&full, 0, full_chunk);
        int same = track_chunks(&t) == 1 && n_chunks(full.n) > 1 && !memcmp(short_chunk, full_chunk, bytes);
        free(short_chunk);
        free(full_chunk);
        if (!same) {
            fprintf(stderr, "FAIL AAC %s: kept chunk differs from a full decode or a chunk needs missing PCM\n", name);
            free(mf_pcm);
            return 1;
        }
    }
    free(mf_pcm);
    printf("ok   AAC %s: %llu table frames, %zu written from sample 0\n", name, (unsigned long long)table, written);
    return 0;
}

static int check_ambiguous(const char *dir) {
    char path[4096], why[200] = "";
    snprintf(path, sizeof path, "%s/ambiguous.m4a", dir);
    plat_file f = {.fd = open(path, O_RDONLY)};
    struct stat st;
    if (fstat(f.fd, &st)) { perror(path); return 1; }
    mf_rate = 44100;
    mf_ch = 2;
    mf_stream = 1;
    pcm_info p;
    int rc = m4a_probe(f, st.st_size, ".m4a", &p, why, sizeof why);
    close(f.fd);
    if (!rc || !strstr(why, "multiple MP4 sound tracks")) {
        fprintf(stderr, "FAIL AAC ambiguous.m4a: expected an ambiguous stream, got %s\n", rc ? why : "success");
        return 1;
    }
    puts("ok   AAC ambiguous.m4a: same-format tracks with different lengths refused");
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int failed = check(argv[1], "tail.m4a", 44100, 2, 482304, 482028, 482028, -1, 0,
                       "Media Foundation x1 output, length 482028");
    failed += check(argv[1], "short.m4a", 44100, 2, 482028, 482029, 482028, -1, 0,
                    "incomplete AAC decode: Media Foundation returned 482028 frames, the sample table says 482029");
    failed += check(argv[1], "short-overlap.m4a", 44100, 2, 484076, 484080, 484076, -1, 0,
                    "incomplete AAC decode: Media Foundation returned 484076 frames, the sample table says 484080");
    failed += check(argv[1], "scale.m4a", 44100, 2, 3072, 2469, 2469, 0, 0,
                    "Media Foundation x1 output, length 2469");
    failed += check(argv[1], "edit.m4a", 44100, 2, 4096, 3136, 3136, -1, 0,
                    "Media Foundation x1 output, length 3136");
    failed += check(argv[1], "short-long.m4a", 44100, 2, 483328, 483264, 483264, 1, 17,
                    "Media Foundation x1 output, length 483264");
    failed += check(argv[1], "long-short.mp4", 48000, 1, 4096, 3200, 3200, 1, 42,
                    "Media Foundation x1 output, length 3200");
    failed += check(argv[1], "video-first.mp4", 44100, 2, 483328, 483264, 483264, 2, 17,
                    "Media Foundation x1 output, length 483264");
    failed += check_ambiguous(argv[1]);
    printf("AAC length: %d passed, %d failed\n", 9 - failed, failed);
    return failed != 0;
}
