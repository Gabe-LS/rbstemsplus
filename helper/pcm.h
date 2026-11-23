/* The Windows helper's own decoders and header probes (helper.md §1, §2; tasks T8, T9). On the
 * Mac, decode.c reads FLAC, WAV, AIFF and ALAC with AudioToolbox and keeps doing so: nothing here
 * is in the Mac helper. On Windows, decode.c's probe and decode_pcm call these instead.
 *
 * Plain C on common/plat.h (files through plat_file: binary, 64-bit offsets, no path handed to a
 * library), so the Mac tests (helper/tests/win-decode.sh) build decode.c's Windows path with
 * RBSP_WIN_DECODE on plat_mac.c and compare it with the Mac's AudioToolbox path on the same files.
 *
 * The rules, as decode.c's (rekordbox's): integer sources are read as 32-bit integers
 * (left-justified: an n-bit sample x 2^(32-n)), then x 2^-31; float32 WAV is copied as is.
 * More than two channels: the first two; mono: both sides. */
#ifndef RBSP_PCM_H
#define RBSP_PCM_H
#include <stddef.h>
#include <stdint.h>
#include "plat.h"

/* decode.c's Windows path: on Windows always; on the Mac only in the tests that compare it */
#if defined(_WIN32) && !defined(RBSP_WIN_DECODE)
#define RBSP_WIN_DECODE 1
#endif

typedef enum { PCM_UNKNOWN, PCM_WAV, PCM_AIFF, PCM_FLAC, PCM_ALAC, PCM_AAC, PCM_MP3 } pcm_kind;

typedef struct {
    pcm_kind kind;
    int rate, ch;           /* the stream's sample rate and channels */
    int bits;               /* bits per sample as stored (WAV, AIFF, FLAC, ALAC); 0 otherwise */
    int flt;                /* WAV IEEE float: 32; otherwise 0 */
    uint64_t frames;        /* the length in sample frames (0: unknown) */
    double seconds;         /* the length in seconds (0: unknown) */
    int approx;             /* MP3: seconds estimated from sampled frame headers, not counted */
    int mp3_layer;          /* MPEG audio layer (1, 2 or 3); 0 for other formats */
    /* where the samples are: WAV/AIFF's data chunk (offset, bytes, byte order) */
    int64_t data_off, data_len;
    int big_endian;
    uint32_t mp4_track_id;  /* selected track in a multi-audio MP4 (0 for the sole audio track) */
    int mf_stream;          /* selected Source Reader index (-1 when unprobed) */
    int aac_lc_rate, aac_lc_ch;  /* original config of single-description LC without signalled extensions */
} pcm_info;

/* A decoded stream: planar stereo float32, big_alloc'd; once decoded to the length the header
 * gives (got == n, then cap == n), s[0..n) left and s[n..2n) right; got counts the frames put so
 * far. */
typedef struct {
    float *s;
    size_t n, got;
    size_t cap;             /* frames a side there is room for (the right side at s[cap..)): it
                             * grows with got, to n, so a claimed length costs nothing up front */
    int ch;                 /* the source's channels */
} pcm_out;

/* 0, or 1 when out of memory (n 0 gets room for one frame, as decode.c's) */
int pcm_out_init(pcm_out *o, size_t n, int ch);
void pcm_out_free(pcm_out *o);
/* frames of interleaved samples, ch channels each, appended to o; 1 when that would pass o->n
 * ("longer than its stated length") */
int pcm_put_i32(pcm_out *o, const int32_t *x, size_t frames);   /* left-justified, x 2^-31 */
int pcm_put_f32(pcm_out *o, const float *x, size_t frames);     /* as is */

/* By content (WAV RIFF/RF64, AIFF/AIFF-C, fLaC, an MP4 ftyp/moov, else MP3): the header only, except
 * FLAC without a length (counted from its stream). 0 and *p, or 1 and why (a sentence, no trailing
 * period): what rekordbox refuses, or what isn't an audio stream this helper reads.
 * ext: the lowercase extension (".mp3": the MP3
 * probe whatever the content, as decode.c reads any .mp3 with rekordbox's MP3 reader). */
int pcm_probe(plat_file f, const char *ext, pcm_info *p, char *why, size_t whyn);
/* FLAC, WAV, AIFF, ALAC after pcm_probe: 0 and *o filled to its end (o->got == o->n), or 1 and
 * why; *o is left freed on failure */
int pcm_decode(plat_file f, const pcm_info *p, pcm_out *o, char *why, size_t whyn);

/* pcm.c, per format (pcm_probe's parts) */
int pcm_probe_wav(plat_file f, int64_t size, pcm_info *p, char *why, size_t whyn);
int pcm_probe_aiff(plat_file f, int64_t size, pcm_info *p, char *why, size_t whyn);
int pcm_probe_flac(plat_file f, int64_t size, pcm_info *p, char *why, size_t whyn);

/* m4a.c: the selected MP4 sound track's header: its codec
 * (stsd 'mp4a' with an AAC esds: PCM_AAC; 'alac': PCM_ALAC; anything else is refused), its rate
 * and channels (ALAC: from its magic cookie). For AAC, frames and seconds describe the stts
 * length at the expected PCM rate (44.1 kHz for a 22.05 kHz AAC core). */
int m4a_probe(plat_file f, int64_t size, const char *ext, pcm_info *p, char *why, size_t whyn);
/* m4a.c, for alac.c: the ALAC track's ALACSpecificConfig (24 bytes) into cookie and the frames to
 * drop at its start into *skip (iTunSMPB's priming), both set before fn's first call; then every
 * packet in order: fn(ctx, offset, bytes) returns 0 (on), 2 (done: the walk ends with 0) or 1 (an
 * error: the walk returns 1, why is fn's) */
typedef int (*m4a_packet_fn)(void *ctx, int64_t off, uint32_t n);
int m4a_alac_packets(plat_file f, int64_t size, uint32_t id, unsigned char cookie[24], uint64_t *skip, m4a_packet_fn fn, void *ctx, char *why,
                     size_t whyn);
/* alac.c: an ALAC track decoded to its end (every packet of the sample table, in order) */
int alac_decode(plat_file f, int64_t size, const pcm_info *p, pcm_out *o, char *why, size_t whyn);

/* mp3.c (RBSP_WIN_DECODE): the header-only MP3 probe (T8): rate and channels of the first frame
 * rekordbox's frame scanner accepts. Use a plausible Xing/Info/VBRI count or count the frame
 * headers. Untagged CBR lengths can be estimated from sampled headers and marked approx. */
int mp3_probe(plat_file f, int64_t size, pcm_info *p, char *why, size_t whyn);

/* reads exactly n bytes at off: 0, or 1 (a read error, or the file ends first) */
int pcm_pread_all(plat_file f, void *buf, size_t n, int64_t off);

#endif
