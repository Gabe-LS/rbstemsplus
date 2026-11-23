/* RB Stems Plus's prepare helper: shared declarations. The rules each part follows were measured
 * against rekordbox 7.2.19; the numbers in comments are from there. */
#ifndef RBSP_H
#define RBSP_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
#include "plat_helper.h"
#elif defined(RBSP_WIN_DECODE)
#include "plat.h"
#endif

/* rekordbox's chunking: chunk i starts at i * HOP - PRE, L samples, zero-padded; the track's final
 * sample is never sent */
#define CHUNK_L 485100
#define CHUNK_HOP 483052
#define CHUNK_PRE 1024
#define BINS 2048
#define FRAMES_F 474                    /* the model's spectrogram frames for one chunk */

/* the most decoded versions of one file: AAC's decoder outputs (2 seen) x two lengths of its last
 * packet (requirement 3, the short last packet) */
#define MAX_TRACKS 8

/* one decoded version of a file, as rekordbox's separator reads it before chunking, at 44.1 kHz:
 * planar stereo float32 (s[0..n) left, s[n..2n) right), or the AAC decoder's own 16-bit samples
 * (p16, interleaved, ch channels: a sample is p16 / 32768), shared by the versions that differ
 * only in length (owner 0 for those) */
typedef struct {
    float *s;
    int16_t *p16;
    int ch, owner;
    size_t n;
#if defined(_WIN32) || defined(RBSP_TEST_MF_TRACK)
    size_t nominal_n;                   /* sample-table length when AAC decoded short */
#endif
} track;

typedef struct {
    track t[MAX_TRACKS];
    int count;
    char how[200];                      /* how it was decoded, for the log */
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE)
    int aac;                             /* decoded through Media Foundation */
#endif
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
    plat_finfo source;                   /* metadata from the file decoded, for the manifest */
#endif
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE) || defined(RBSP_TEST_WIN_PREPARE)
    int source_rate;                     /* decoded sample rate before resampling */
#endif
#if defined(_WIN32) || defined(RBSP_TEST_WIN_PREPARE) || defined(RBSP_TEST_MF_TRACK)
    enum { AAC_LENGTH_NONE, AAC_LENGTH_COMPLETE, AAC_LENGTH_SHORT } aac_length;
#endif
} tracks;

void tracks_free(tracks *ts);

/* decode.c: 0 and ts filled, or 1 and why it is skipped (a sentence, no trailing period) */
int load_tracks(const char *path, const char *rekordbox_app, tracks *ts, char *why, size_t whyn);

/* decode.c, without decoding: 0 when load_tracks would decode the file (as far as its header
 * tells), *seconds its length as macOS estimates it (0 when unknown), *aac 1 for AAC (two decoder
 * outputs, each with its own entries); or 1 and why load_tracks refuses it */
int probe_track(const char *path, double *seconds, int *aac, char *why, size_t whyn);

/* resample.c: rekordbox's sample-rate converter to 44.1 kHz; 0 when done, *out malloc'd planar */
int rb_resample(const float *in, size_t n, int rate, float **out, size_t *nout);
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
/* resample_win.c (Windows): rekordbox for Windows' converter. rb_resample returns 2 when the
 * table built here isn't the one proved for the rate. rb_resample_proved: 1 for a rate whose
 * output was proved equal to rekordbox for Windows' own (Q12); other rates aren't prepared.
 * rb_resample_build_proved: 1 when the core passed --resample-proved (the rekordbox.exe in use is
 * in windows.resampler_proved); without it no file is resampled (Q15) */
int rb_resample_proved(int rate);
/* 1 when the table built on this PC for a proved rate is the proved one (built and checked once,
 * then kept for rb_resample); the probe and the converter both ask it, so they agree */
int rb_resample_table_ok(int rate);
/* the converter's output frames for nin input frames at rate (to 44.1 kHz, from a reset state):
 * the outputs n with floor(M n / L) <= nin - 1, i.e. ceil(L nin / M), L = 882, M = rate / 50;
 * 0 for a rate it can't take or a count that would overflow. The estimate uses it too. */
static inline size_t rb_resample_out_frames(size_t nin, int rate) {
    if (rate <= 0 || rate % 50 || !nin) return 0;
    size_t L = 882, M = (size_t)rate / 50, q = nin / M, r = nin % M;
    if (q > (SIZE_MAX - L) / L) return 0;
    return q * L + (r * L + M - 1) / M;
}
#endif
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE) || defined(RBSP_TEST_WIN_PREPARE)
extern int rb_resample_build_proved;
/* The Windows rate gate used by the probe, the decoder and --quick. */
int rb_win_rate_refused(int rate, const char *when, char *why, size_t whyn);
#endif
#if defined(_WIN32) && defined(RBSP_RESEARCH)
#define RBSP_TEST_FORMAT_RATES 1
#endif
#ifdef RBSP_TEST_FORMAT_RATES
extern int rb_aac_test_unproved;
extern int rb_mp3_test_unproved;
extern int rb_rates_test_unproved;

static inline int rb_aac_candidate_rate(int rate, int ch) {
    return ch == 1 && (rate == 16000 || rate == 22050 || rate == 24000);
}

int rb_resample_win_any(const float *in, size_t nin, int rate, float **out, size_t *nout);
#endif

/* mp4.c: the sound track's sample table; 0 when found */
typedef struct {
    uint32_t timescale;
    uint64_t stts_total;                /* the sample table's length, in timescale units */
    uint32_t last_delta;                /* the last packet's length, in timescale units */
    int has_stts;
} mp4_audio;
int mp4_audio_track(const char *path, mp4_audio *out);

/* mp3.c: rekordbox's MP3 reader (frame map, then its own libmpg123): 0 and *out (planar stereo,
 * float32) or 1 and why */
#if defined(_WIN32) || defined(RBSP_WIN_DECODE)
int rb_mp3_file(plat_file f, const char *libmpg123, float **out, size_t *n, int *rate, char *why, size_t whyn);
#else
int rb_mp3(const char *path, const char *libmpg123, float **out, size_t *n, char *why, size_t whyn);
#endif

/* aac.c: every distinct output of Apple's AAC decoder for the file (the 64-byte stack sweep),
 * int16 interleaved, sorted by content; *rate and *ch as the decoder reports them */
typedef struct {
    int16_t *pcm[MAX_TRACKS];
    size_t frames[MAX_TRACKS];
    int count;
    int rate, ch;
} aac_outputs;
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
int aac_decode_all(plat_file f, const char *ext, int stream, aac_outputs *o, char *why, size_t whyn);
#else
int aac_decode_all(const char *path, aac_outputs *o, char *why, size_t whyn);
#endif
void aac_outputs_free(aac_outputs *o);

/* chunk i of a track (rekordbox's), planar [2][CHUNK_L] into c */
void chunk_at(const track *t, size_t i, float *c);
size_t n_chunks(size_t n);
#if defined(_WIN32) || defined(RBSP_TEST_MF_TRACK)
size_t track_chunks(const track *t);
#endif

/* The performance timing lines (helper/build.sh --test-timing, helper/tests/perf.sh; never a
 * release): "rbsp-timing ..." on stderr, times in ns of mach_absolute_time's clock. TIMING(...)
 * marks the statements that exist only there. */
#ifdef RBSP_TEST_TIMING
#include <time.h>
#define TIMING(...) __VA_ARGS__
#if defined(_WIN32)
static inline uint64_t t_now(void) { return plat_now_mono_ns(); }   /* QueryPerformanceCounter's */
#else
static inline uint64_t t_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
#endif
extern uint64_t t_resample;             /* decode.c: the resampler's time for the current file */
extern uint64_t t_ort;                  /* model.c: the last model_run's ONNX Runtime Run alone */
#else
#define TIMING(...)
#endif

#endif
