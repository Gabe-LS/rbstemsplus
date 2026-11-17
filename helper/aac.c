/* AAC (and ALAC) in an MP4 through AudioToolbox's AudioConverter, set up as rekordbox's
 * FileReadMp4 sets it up: the file's basic data format (HE-AAC: the AAC core, at half the rate),
 * 16-bit signed packed output, the magic cookie, PrimeInfo {0, 0}, one packet per input call.
 *
 * Apple's AAC decoder has two possible outputs for a file, one 16-bit step apart in about 0.1% of
 * samples: which one depends on the stack's alignment (mod 64 bytes) where it runs. rekordbox's
 * is one of them, and stays the same while it runs. So the file is decoded at every 16-byte
 * position of two 64-byte periods (requirement 3), and each distinct output is kept, in a fixed
 * order (by SHA-256 of its bytes, as the prototype): the model runs on the first, and its stems
 * are shared with the other, so the order must not depend on the environment. */
#include <AudioToolbox/AudioToolbox.h>
#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rbsp.h"
#include "big.h"
#include "plat.h"

#define SHIFTS 8                         /* 0, 16, ..., 112 bytes */

typedef struct {
    const char *data;
    const AudioStreamPacketDescription *desc;
    UInt64 n, next;
    AudioStreamPacketDescription one;
} feed;

static OSStatus input(AudioConverterRef c, UInt32 *np, AudioBufferList *bl, AudioStreamPacketDescription **d, void *u) {
    (void)c;
    feed *f = u;
    if (f->next >= f->n) { *np = 0; bl->mBuffers[0].mDataByteSize = 0; return 0; }
    const AudioStreamPacketDescription *p = &f->desc[f->next];
    bl->mBuffers[0].mData = (void *)(f->data + p->mStartOffset);
    bl->mBuffers[0].mDataByteSize = p->mDataByteSize;
    f->one = *p;
    f->one.mStartOffset = 0;
    if (d) *d = &f->one;
    *np = 1;
    f->next++;
    return 0;
}

typedef struct {
    AudioStreamBasicDescription in;
    void *cookie;
    UInt32 cookie_size;
    char *data;
    AudioStreamPacketDescription *desc;
    UInt64 packets;
} source;

/* one whole decode; 0 and *pcm (malloc'd, interleaved) or 1 */
__attribute__((noinline)) static int decode_once(const source *s, int16_t **pcm, size_t *frames) {
    UInt32 ch = s->in.mChannelsPerFrame;
    AudioStreamBasicDescription out = {s->in.mSampleRate, kAudioFormatLinearPCM,
                                       kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
                                       2 * ch, 1, 2 * ch, ch, 16, 0};
    AudioConverterRef c;
    if (AudioConverterNew(&s->in, &out, &c)) return 1;
    if (s->cookie_size && AudioConverterSetProperty(c, kAudioConverterDecompressionMagicCookie, s->cookie_size, s->cookie)) {
        AudioConverterDispose(c);
        return 1;
    }
    AudioConverterPrimeInfo pi = {0, 0};
    AudioConverterSetProperty(c, kAudioConverterPrimeInfo, sizeof pi, &pi);
    feed f = {s->data, s->desc, s->packets, 0, {0}};
    size_t cap = 1 << 20, n = 0;
    int16_t *buf = big_alloc(cap * 2 * ch);
    int rc = buf ? 0 : 1;
    while (!rc) {
        if (cap - n < 4096) {
            int16_t *b = big_realloc(buf, cap * 2 * 2 * ch);
            if (!b) { rc = 1; break; }
            buf = b;
            cap *= 2;
        }
        UInt32 want = 4096;              /* frames per call, as the verified prototype asked */
        AudioBufferList bl = {1, {{ch, want * 2 * ch, buf + n * ch}}};
        UInt32 got = want;
        OSStatus st = AudioConverterFillComplexBuffer(c, input, &f, &got, &bl, NULL);
        n += got;
        if (st) rc = 1;                  /* a damaged file isn't a short one */
        else if (!got) break;
    }
    AudioConverterDispose(c);
    if (rc) { big_free(buf); return 1; }
    *pcm = buf;
    *frames = n;
    return 0;
}

/* the decode, `shift` bytes lower on the stack */
__attribute__((noinline)) static int decode_shifted(const source *s, size_t shift, int16_t **pcm, size_t *frames) {
    volatile char *p = alloca(shift + 1);
    p[0] = 0;                            /* the alloca stays: it is used */
    int rc = decode_once(s, pcm, frames);
    __asm__ volatile("" : : "r"(p) : "memory");
    return rc;
}

static int open_source(const char *path, source *s, char *why, size_t whyn) {
    memset(s, 0, sizeof *s);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    AudioFileID af;
    OSStatus e = url ? AudioFileOpenURL(url, kAudioFileReadPermission, 0, &af) : 1;
    if (url) CFRelease(url);
    if (e) { snprintf(why, whyn, "macOS can't open it as audio"); return 1; }
    UInt32 sz = sizeof s->in;
    UInt64 bytes = 0;
    int rc = 1;
    if (AudioFileGetProperty(af, kAudioFilePropertyDataFormat, &sz, &s->in)) { snprintf(why, whyn, "no audio format"); goto done; }
    sz = sizeof s->packets;
    if (AudioFileGetProperty(af, kAudioFilePropertyAudioDataPacketCount, &sz, &s->packets)) { snprintf(why, whyn, "no packet count"); goto done; }
    sz = sizeof bytes;
    if (AudioFileGetProperty(af, kAudioFilePropertyAudioDataByteCount, &sz, &bytes)) { snprintf(why, whyn, "no byte count"); goto done; }
    if (!s->packets || !bytes || bytes > (1ULL << 32) || s->packets > (1ULL << 28)) { snprintf(why, whyn, "an unusual packet table"); goto done; }
    s->data = big_alloc(bytes);
    s->desc = malloc(sizeof(AudioStreamPacketDescription) * s->packets);
    UInt32 nb = (UInt32)bytes, np = (UInt32)s->packets;
    if (!s->data || !s->desc || AudioFileReadPacketData(af, false, &nb, s->desc, 0, &np, s->data) || np != s->packets) {
        snprintf(why, whyn, "its audio packets can't be read");
        goto done;
    }
    UInt32 cs = 0;
    if (!AudioFileGetPropertyInfo(af, kAudioFilePropertyMagicCookieData, &cs, NULL) && cs) {
        s->cookie = malloc(cs);
        if (!s->cookie || AudioFileGetProperty(af, kAudioFilePropertyMagicCookieData, &cs, s->cookie)) {
            snprintf(why, whyn, "its decoder settings (magic cookie) can't be read");
            goto done;
        }
        s->cookie_size = cs;
    }
    rc = 0;
done:
    AudioFileClose(af);
    return rc;
}

static void close_source(source *s) {
    big_free(s->data);
    free(s->desc);
    free(s->cookie);
}

void aac_outputs_free(aac_outputs *o) {
    for (int i = 0; i < o->count; i++) big_free(o->pcm[i]);
    o->count = 0;
}

int aac_decode_all(const char *path, aac_outputs *o, char *why, size_t whyn) {
    memset(o, 0, sizeof *o);
    source s;
    if (open_source(path, &s, why, whyn)) { close_source(&s); return 1; }
    o->rate = (int)s.in.mSampleRate;
    o->ch = (int)s.in.mChannelsPerFrame;
    unsigned char dg[MAX_TRACKS][32];
    int rc = 0;
    /* one after another: each output of an hour of audio is about 300 MB, and only distinct ones
     * are kept */
    for (int k = 0; k < SHIFTS && !rc; k++) {
        int16_t *pcm;
        size_t frames;
        if (decode_shifted(&s, (size_t)k * 16, &pcm, &frames)) { snprintf(why, whyn, "Apple's AAC decoder stopped with an error"); rc = 1; break; }
        unsigned char d[32];
        /* the 32-bit byte count CC_SHA256 took (1.3.0's: an output past 4 GB is hashed in its
         * first count mod 2^32 bytes): the outputs' order, and so which runs first, unchanged */
        plat_sha256(pcm, (uint32_t)(frames * 2 * (size_t)o->ch), d);
        int seen = 0;
        for (int j = 0; j < o->count; j++)
            if (!memcmp(dg[j], d, sizeof d)) seen = 1;
        if (seen || o->count == MAX_TRACKS) { big_free(pcm); continue; }
        /* insert in digest order */
        int at = o->count;
        while (at > 0 && memcmp(dg[at - 1], d, sizeof d) > 0) {
            memcpy(dg[at], dg[at - 1], sizeof d);
            o->pcm[at] = o->pcm[at - 1];
            o->frames[at] = o->frames[at - 1];
            at--;
        }
        memcpy(dg[at], d, sizeof d);
        o->pcm[at] = pcm;
        o->frames[at] = frames;
        o->count++;
    }
    close_source(&s);
    if (rc) aac_outputs_free(o);
    return rc;
}
