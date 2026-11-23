/* aac_mf.c's one decode, for its tests (helper/tests/aac_mf_probe.c): the Windows helper itself
 * calls rbsp.h's aac_decode_all. Windows only. */
#ifndef RBSP_AAC_MF_H
#define RBSP_AAC_MF_H
#include <stddef.h>
#include <stdint.h>
#include "plat.h"

typedef struct {
    int streams, stream;                 /* the streams in the file, the (last) audio one read */
    uint32_t native_sub;                 /* the stream's own subtype's Data1 (0x1610: AAC) */
    int native_rate, native_ch;          /* as the source reports the stream (0: not given) */
    int rate, ch, bits;                  /* the PCM type the decoder settled on */
    int typed;                           /* 1: the byte stream needed a content type */
    long long duration;                  /* MF_PD_DURATION, 100 ns units (-1: not given) */
    long long first_ts, last_ts;         /* the first and last samples' times, 100 ns units */
    size_t samples, gaps;                /* MF samples read; times off the running count by over 1 ms */
    size_t typechanges;                  /* CURRENTMEDIATYPECHANGED flags seen */
} aac_mf_info;

/* Start Media Foundation once per process. Returns 1 when available, or 0 and why. */
int aac_mf_available(char *why, size_t whyn);

#define AAC_MF_VERSION_MAX 24            /* four 16-bit decimal fields, dots and terminator */
/* Process-cached file version of the system AAC decoder, or "" if unavailable. */
const char *aac_mf_decoder_version(void);

/* Discover the last audio stream, its native format and (for AAC) the type of its first PCM frame. */
int aac_mf_probe(plat_file f, const char *ext, aac_mf_info *info, char *why, size_t whyn);

/* one whole decode of the file's last audio stream to 16-bit PCM, interleaved, big_alloc'd:
 * async 1 as rekordbox reads it (MF_SOURCE_READER_ASYNC_CALLBACK, the decoder on MF's threads),
 * 0 with blocking reads on this thread, `shift` bytes lower on the stack. Returns 0 on success,
 * otherwise nonzero and why (the HRESULT if COM initialization failed). */
int aac_mf_decode(const char *path, int async, size_t shift, int16_t **pcm, size_t *frames, aac_mf_info *info, char *why,
                  size_t whyn);

#endif
