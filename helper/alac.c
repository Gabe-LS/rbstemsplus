/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

/*
 * RB Stems Plus's C port of Apple's ALACDecoder.cpp and of ag_dec.c's dyn_decomp (macosforge/alac,
 * commit c38887c5), modified: the C++ class became C functions; the output stage writes 32-bit
 * left-justified integers (a b-bit sample x 2^(32-b)) instead of 16/20/24/32-bit packed samples;
 * a packet is checked against its end and the decoder's buffers before each step (a partial frame
 * longer than the config's, a shift or escape run past the packet, a sample size the predictor
 * can't take are errors), so a corrupt packet fails instead of reading or writing outside them;
 * dyn_decomp counts leading zeros with one instruction and does its arithmetic unsigned. Apple's
 * ALACBitUtilities.c and dp_dec.c are used as they are.
 *
 * alac_decode (pcm.h): the Windows helper's ALAC track, the packets of the sample table (m4a.c)
 * decoded in order to the length AudioToolbox gives it (m4a.c's alac_length), for decode.c's rule
 * (x 2^-31, the first two channels, mono both sides).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcm.h"
#include "ALACBitUtilities.h"
#include "aglib.h"
#include "dplib.h"

enum { PAD = 32 };                       /* unpc_block writes numactive (< 32) samples whatever num is */

typedef struct {
    uint32_t frameLength, maxRun;
    uint32_t bitDepth, pb, mb, kb, numChannels;
    int32_t *mixU, *mixV, *pred;
    uint16_t *shift;                     /* two per frame (a pair's); Apple's shares pred's memory */
} alac;

static uint32_t be32(const unsigned char *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

static void alac_free(alac *d) {
    free(d->mixU);
    free(d->mixV);
    free(d->pred);
    free(d->shift);
    memset(d, 0, sizeof *d);
}

/* ALACDecoder::Init from the ALACSpecificConfig (24 bytes, big-endian): 0, or 1 */
static int alac_init(alac *d, const unsigned char *c) {
    memset(d, 0, sizeof *d);
    d->frameLength = be32(c);
    d->bitDepth = c[5];
    d->pb = c[6];
    d->mb = c[7];
    d->kb = c[8];
    d->numChannels = c[9];
    d->maxRun = (uint32_t)c[10] << 8 | c[11];
    /* the bit depths the output stage has; kb 0 or >= 32 is a shift by 32. Not Apple's
     * compatibleVersion check: AudioToolbox decodes a cookie whatever its version says. */
    if (d->frameLength < 1 || d->frameLength > 65536 || d->numChannels < 1 || d->numChannels > 8
        || (d->bitDepth != 16 && d->bitDepth != 20 && d->bitDepth != 24 && d->bitDepth != 32) || d->kb < 1 || d->kb > 31)
        return 1;
    size_t n = d->frameLength + PAD;
    d->mixU = calloc(n, sizeof(int32_t));
    d->mixV = calloc(n, sizeof(int32_t));
    d->pred = calloc(n, sizeof(int32_t));
    d->shift = calloc(2 * n, sizeof(uint16_t));
    if (!d->mixU || !d->mixV || !d->pred || !d->shift) { alac_free(d); return 1; }
    return 0;
}

/* bits left in the packet: 0 at or past its end (at the end, bitIndex can be nonzero: what Apple's
 * reader left after reading past it, which must not count as bits left) */
static uint64_t bits_left(const BitBuffer *b) {
    uint64_t have = b->cur >= b->end ? 0 : (uint64_t)(b->end - b->cur) * 8;
    return have > b->bitIndex ? have - b->bitIndex : 0;
}

/* whether a read went past the packet's end (Apple's reader doesn't stop there: the bytes past
 * it are the zero padding of on_packet's buffer, which must not decode as samples) */
static int past_end(const BitBuffer *b) { return b->cur > b->end || (b->cur == b->end && b->bitIndex); }

/* a b-bit sample, left-justified in 32 bits (Apple's 16/20-bit outputs ignore the shift buffer) */
static int32_t lj(uint32_t v, uint32_t bitDepth) { return (int32_t)(v << (32 - bitDepth)); }

/* the predictor and the shift buffer's low bytes (24 and 32-bit: copyPredictorTo24Shift/32Shift) */
static uint32_t with_shift(int32_t v, const uint16_t *sh, size_t k, uint32_t bytesShifted, uint32_t bitDepth) {
    if (!bytesShifted || bitDepth < 24) return (uint32_t)v;
    return (uint32_t)v << (bytesShifted * 8) | sh[k];
}

/* ag_dec.c's adaptive Golomb decoder (dyn_decomp and its helpers, same commit), its leading-zero
 * count as one instruction, as Apple's comment there advises (its bit loop made a track's decode
 * twice as slow as AudioToolbox's); otherwise as there */
static inline uint32_t lead(uint32_t m) { return m ? (uint32_t)__builtin_clz(m) : 32; }
static inline uint32_t read32bit(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
#define get_next_fromlong(inlong, suff) ((inlong) >> (32 - (suff)))

static inline uint32_t getstreambits(const uint8_t *in, uint32_t bitoffset, uint32_t numbits) {
    uint32_t byteoffset = bitoffset / 8, load1 = read32bit(in + byteoffset), result;
    if (numbits + (bitoffset & 7) > 32) {
        result = load1 << (bitoffset & 7);
        uint32_t load2 = in[byteoffset + 4];
        load2 >>= 8 - (numbits + (bitoffset & 7) - 32);
        result >>= 32 - numbits;
        result |= load2;
    } else {
        result = load1 >> (32 - numbits - (bitoffset & 7));
    }
    if (numbits != 32) result &= ~(0xffffffffu << numbits);
    return result;
}

static inline uint32_t dyn_get(const uint8_t *in, uint32_t *bitPos, uint32_t m, uint32_t k) {
    uint32_t tempbits = *bitPos, result, streamlong = read32bit(in + (tempbits >> 3)) << (tempbits & 7), pre = lead(~streamlong);
    if (pre >= MAX_PREFIX_16) {
        pre = MAX_PREFIX_16;
        tempbits += pre;
        streamlong <<= pre;
        result = get_next_fromlong(streamlong, MAX_DATATYPE_BITS_16);
        tempbits += MAX_DATATYPE_BITS_16;
    } else {
        tempbits += pre + 1;
        streamlong <<= pre + 1;
        uint32_t v = get_next_fromlong(streamlong, k);
        tempbits += k;
        result = pre * m + v - 1;
        if (v < 2) {
            result -= v - 1;
            tempbits -= 1;
        }
    }
    *bitPos = tempbits;
    return result;
}

static inline uint32_t dyn_get_32bit(const uint8_t *in, uint32_t *bitPos, uint32_t m, uint32_t k, uint32_t maxbits) {
    uint32_t tempbits = *bitPos, streamlong = read32bit(in + (tempbits >> 3)) << (tempbits & 7), result = lead(~streamlong);
    if (result >= MAX_PREFIX_32) {
        result = getstreambits(in, tempbits + MAX_PREFIX_32, maxbits);
        tempbits += MAX_PREFIX_32 + maxbits;
    } else {
        tempbits += result + 1;
        if (k != 1) {
            streamlong <<= result + 1;
            uint32_t v = get_next_fromlong(streamlong, k);
            tempbits += k - 1;
            result = result * m;
            if (v >= 2) {
                result += v - 1;
                tempbits += 1;
            }
        }
    }
    *bitPos = tempbits;
    return result;
}

/* dyn_decomp with set_ag_params' values (mb0 = mb, pb, kb, wb = 2^kb - 1): 0, or 1 */
static int ag_decomp(BitBuffer *bits, int32_t *pc, uint32_t numSamples, uint32_t maxSize, uint32_t mb0, uint32_t pb, uint32_t kb) {
    const uint8_t *in = bits->cur;
    uint32_t startPos = bits->bitIndex, maxPos = bits->byteSize * 8, bitPos = startPos, wb = (1u << kb) - 1;
    uint32_t mb = mb0, zmode = 0, c = 0;
    int status = 0;
    int32_t *outPtr = pc;
    while (c < numSamples) {
        if (bitPos >= maxPos) { status = 1; break; }
        uint32_t m = mb >> QBSHIFT, k = 31 - lead(m + 3);    /* lg3a */
        if (k > kb) k = kb;
        m = (1u << k) - 1;
        uint32_t n = dyn_get_32bit(in, &bitPos, m, k, maxSize);
        /* the least significant bit is the sign */
        uint32_t ndecode = n + zmode;
        int32_t multiplier = -(int32_t)(ndecode & 1);
        multiplier |= 1;
        *outPtr++ = (int32_t)(((ndecode + 1) >> 1) * (uint32_t)multiplier);
        c++;
        mb = pb * (n + zmode) + mb - ((pb * mb) >> QBSHIFT);
        if (n > 0xffff) mb = 0xffff;                         /* N_MAX_MEAN_CLAMP, N_MEAN_CLAMP_VAL */
        zmode = 0;
        if (((mb << MMULSHIFT) < QB) && (c < numSamples)) {
            zmode = 1;
            k = lead(mb) - BITOFF + ((mb + MOFF) >> MDENSHIFT);
            uint32_t mz = ((1u << k) - 1) & wb;
            n = dyn_get(in, &bitPos, mz, k);
            if (c + n > numSamples) { status = 1; break; }
            for (uint32_t j = 0; j < n; j++) {
                *outPtr++ = 0;
                ++c;
            }
            if (n >= 65535) zmode = 0;
            mb = 0;
        }
    }
    BitBufferAdvance(bits, bitPos - startPos);
    return status || past_end(bits);
}

/* one channel's compressed samples: the Golomb decoder, then unpc_block (twice in mode != 0) */
static int decomp(alac *d, BitBuffer *bits, int32_t *out, uint32_t numSamples, uint32_t chanBits, uint32_t pbFactor,
                  uint32_t mode, int16_t *coefs, uint32_t num, uint32_t denShift) {
    if (ag_decomp(bits, d->pred, numSamples, chanBits, d->mb, (d->pb * pbFactor) / 4, d->kb)) return 1;
    if (mode != 0) unpc_block(d->pred, d->pred, (int32_t)numSamples, NULL, 31, chanBits, 0);
    unpc_block(d->pred, out, (int32_t)numSamples, coefs, (int32_t)num, chanBits, denShift);
    return 0;
}

/* one channel's predictor: mode, denominator shift, pb factor, coefficients (the caller checks
 * the reads against the packet's end) */
typedef struct { uint32_t mode, denShift, pbFactor, num; int16_t coefs[32]; } params;
static void read_params(BitBuffer *bits, params *q) {
    uint32_t h = BitBufferRead(bits, 8);
    q->mode = h >> 4;
    q->denShift = h & 0xfu;
    h = BitBufferRead(bits, 8);
    q->pbFactor = h >> 5;
    q->num = h & 0x1fu;
    for (uint32_t i = 0; i < q->num; i++) q->coefs[i] = (int16_t)BitBufferRead(bits, 16);
}

/* ALACDecoder::Decode: one packet to numChannels interleaved left-justified int32s; *outNumSamples
 * as Apple's (the last element's count). 0, or 1 (a corrupt packet). bits must have room to read
 * past its end: see on_packet. */
static int alac_packet(alac *d, BitBuffer *bits, int32_t *out, uint32_t *outNumSamples) {
    uint32_t numChannels = d->numChannels, numSamples = d->frameLength, channelIndex = 0, firstCount = 0;
    int counted = 0;
    *outNumSamples = numSamples;
    for (;;) {
        if (bits->cur >= bits->end) return 1;
        uint32_t tag = BitBufferReadSmall(bits, 3);
        switch (tag) {
        case ID_SCE:
        case ID_LFE:
        case ID_CPE: {
            int pair = tag == ID_CPE;
            if (pair && channelIndex + 2 > numChannels) goto no_more_channels;
            BitBufferReadSmall(bits, 4);             /* element instance tag */
            if (BitBufferRead(bits, 12) != 0) return 1;
            uint32_t h = BitBufferRead(bits, 4);
            uint32_t partialFrame = h >> 3, bytesShifted = (h >> 1) & 0x3u, escapeFlag = h & 0x1u;
            if (bytesShifted == 3) return 1;
            uint32_t chanBits = d->bitDepth - bytesShifted * 8 + (pair ? 1 : 0);
            if (partialFrame) {
                numSamples = BitBufferRead(bits, 16) << 16;
                numSamples |= BitBufferRead(bits, 16);
                if (numSamples > d->frameLength) return 1;
            }
            if (past_end(bits)) return 1;                /* the header itself runs past the packet */
            /* every element of a packet has the first one's count: otherwise a channel would keep
             * samples no element wrote (Apple's encoder never writes such a packet) */
            if (counted && numSamples != firstCount) return 1;
            firstCount = numSamples;
            counted = 1;
            int32_t mixRes = 0;
            uint32_t mixBits = 0;
            if (escapeFlag == 0) {
                mixBits = BitBufferRead(bits, 8);
                mixRes = (int8_t)BitBufferRead(bits, 8);
                params u, v;
                read_params(bits, &u);
                if (pair) read_params(bits, &v);
                if (past_end(bits)) return 1;
                BitBuffer shiftBits = *bits;
                if (bytesShifted != 0) {
                    uint64_t skip = (uint64_t)bytesShifted * 8 * (pair ? 2 : 1) * numSamples;
                    if (skip > bits_left(bits)) return 1;
                    BitBufferAdvance(bits, (uint32_t)skip);
                }
                if (chanBits < 1 || chanBits > 32 || (pair && mixRes != 0 && mixBits > 31)) return 1;
                if (decomp(d, bits, d->mixU, numSamples, chanBits, u.pbFactor, u.mode, u.coefs, u.num, u.denShift)) return 1;
                if (pair && decomp(d, bits, d->mixV, numSamples, chanBits, v.pbFactor, v.mode, v.coefs, v.num, v.denShift)) return 1;
                if (bytesShifted != 0) {
                    uint32_t shift = bytesShifted * 8, k = pair ? 2 * numSamples : numSamples;
                    for (uint32_t i = 0; i < k; i++) d->shift[i] = (uint16_t)BitBufferRead(&shiftBits, (uint8_t)shift);
                }
            } else {
                /* uncompressed: a pair's samples are interleaved, bitDepth each (a single channel's
                 * chanBits, as Apple's) */
                if (pair) chanBits = d->bitDepth;
                if (chanBits < 1 || (uint64_t)chanBits * (pair ? 2 : 1) * numSamples > bits_left(bits)) return 1;
                uint32_t shift = 32 - chanBits;
                for (uint32_t i = 0; i < numSamples; i++)
                    for (int c = 0; c < (pair ? 2 : 1); c++) {
                        int32_t val;
                        if (chanBits <= 16) {
                            val = (int32_t)(BitBufferRead(bits, (uint8_t)chanBits) << shift) >> shift;
                        } else {
                            val = (int32_t)(BitBufferRead(bits, 16) << 16) >> shift;
                            val = (int32_t)((uint32_t)val | BitBufferRead(bits, (uint8_t)(chanBits - 16)));
                        }
                        (c ? d->mixV : d->mixU)[i] = val;
                    }
                bytesShifted = 0;
            }
            /* the output stage: unmix (pairs), the shifted-off bytes, left-justified */
            int32_t *o = out + channelIndex;
            if (!pair) {
                for (uint32_t i = 0; i < numSamples; i++, o += numChannels)
                    *o = lj(with_shift(d->mixU[i], d->shift, i, bytesShifted, d->bitDepth), d->bitDepth);
            } else {
                for (uint32_t i = 0; i < numSamples; i++, o += numChannels) {
                    int32_t l = d->mixU[i], r = d->mixV[i];
                    if (mixRes != 0) {   /* L = u + v - [rV/m], R = L - v (matrixed stereo) */
                        int32_t t = (int32_t)(((int64_t)mixRes * r) >> mixBits);
                        l = (int32_t)((uint32_t)l + (uint32_t)r - (uint32_t)t);
                        r = (int32_t)((uint32_t)l - (uint32_t)r);
                    }
                    o[0] = lj(with_shift(l, d->shift, 2 * (size_t)i, bytesShifted, d->bitDepth), d->bitDepth);
                    o[1] = lj(with_shift(r, d->shift, 2 * (size_t)i + 1, bytesShifted, d->bitDepth), d->bitDepth);
                }
            }
            if (past_end(bits)) return 1;                /* the element ends past the packet */
            channelIndex += pair ? 2 : 1;
            *outNumSamples = numSamples;
            break;
        }
        case ID_CCE:
        case ID_PCE:
            return 1;                    /* unsupported, as Apple's */
        case ID_DSE: {                   /* data stream element: skipped */
            BitBufferReadSmall(bits, 4);
            uint32_t align = BitBufferReadOne(bits), count = BitBufferReadSmall(bits, 8);
            if (count == 255) count += BitBufferReadSmall(bits, 8);
            if (align) BitBufferByteAlign(bits, 0);
            if ((uint64_t)count * 8 > bits_left(bits)) return 1;
            BitBufferAdvance(bits, count * 8);
            if (past_end(bits)) return 1;
            break;
        }
        case ID_FIL: {                   /* fill element: skipped */
            uint32_t count = BitBufferReadSmall(bits, 4);
            if (count == 15) count += (uint32_t)BitBufferReadSmall(bits, 8) - 1;
            if ((uint64_t)count * 8 > bits_left(bits)) return 1;
            BitBufferAdvance(bits, count * 8);
            if (past_end(bits)) return 1;
            break;
        }
        case ID_END:
            BitBufferByteAlign(bits, 0);
            if (past_end(bits)) return 1;
            goto no_more_channels;
        }
        if (channelIndex >= numChannels) return 0;
    }
no_more_channels:
    /* fewer channels in the packet than the config's: the rest are zeros (Apple's Zero16/24/32;
     * after an END element too, where Apple's leaves the buffer as it was) */
    for (; channelIndex < numChannels; channelIndex++)
        for (uint32_t i = 0; i < numSamples; i++) out[(size_t)i * numChannels + channelIndex] = 0;
    return 0;
}

enum { WINDOW = 1 << 20 };               /* packets are read a window at a time */

typedef struct {
    plat_file f;
    int64_t size;
    pcm_out *o;
    unsigned char cookie[24];
    alac d;
    int ready;
    uint32_t cap, packet;                /* the largest packet taken; packets so far */
    uint64_t skip;                       /* frames still to drop (iTunSMPB's priming) */
    unsigned char *win, *pkt;            /* the window; one packet, padded */
    int64_t woff;
    size_t wlen, wcap;
    int32_t *out;
    char *why;
    size_t whyn;
} ctx;

static int on_packet(void *vc, int64_t off, uint32_t n) {
    ctx *c = vc;
    if (!c->ready) {
        if (alac_init(&c->d, c->cookie)) { snprintf(c->why, c->whyn, "ALAC magic cookie this decoder can't take"); return 1; }
        c->ready = 1;
        if ((int)c->d.numChannels != c->o->ch) { snprintf(c->why, c->whyn, "ALAC with %u channels, %d expected", c->d.numChannels, c->o->ch); return 1; }
        /* an uncompressed frame's bytes, with room for every element's header and fill */
        c->cap = c->d.frameLength * c->d.numChannels * 4 + 4096;
        c->wcap = c->cap > WINDOW ? c->cap : WINDOW;
        /* the bit reader reads up to a packet's length past where ag_decomp starts (dyn_decomp's
         * bound is the packet's size from there, not its end), and a few bytes past any read */
        c->pkt = malloc(2 * (size_t)c->cap + 1024);
        c->win = malloc(c->wcap);
        c->out = malloc(sizeof(int32_t) * c->d.frameLength * c->d.numChannels);
        if (!c->pkt || !c->win || !c->out) { snprintf(c->why, c->whyn, "out of memory"); return 1; }
    }
    if (n > c->cap) { snprintf(c->why, c->whyn, "ALAC packet %u: %u bytes, more than a packet holds", c->packet, n); return 1; }
    if (off < c->woff || off + n > c->woff + (int64_t)c->wlen) {
        size_t len = c->wcap;
        if ((int64_t)len > c->size - off) len = (size_t)(c->size - off);
        if (pcm_pread_all(c->f, c->win, len, off)) { snprintf(c->why, c->whyn, "the file can't be read"); return 1; }
        c->woff = off;
        c->wlen = len;
    }
    memcpy(c->pkt, c->win + (off - c->woff), n);
    memset(c->pkt + n, 0, (size_t)n + 1024);
    BitBuffer bits;
    BitBufferInit(&bits, c->pkt, n);
    uint32_t frames;
    if (alac_packet(&c->d, &bits, c->out, &frames)) { snprintf(c->why, c->whyn, "corrupt ALAC packet %u", c->packet); return 1; }
    c->packet++;
    /* as ExtAudioFile: the priming frames dropped, the track ends at its length (m4a.c) */
    uint32_t from = 0;
    if (c->skip) {
        from = c->skip < frames ? (uint32_t)c->skip : frames;
        c->skip -= from;
    }
    size_t take = frames - from;
    if (take > c->o->n - c->o->got) take = c->o->n - c->o->got;
    if (take && pcm_put_i32(c->o, c->out + (size_t)from * c->d.numChannels, take)) { snprintf(c->why, c->whyn, "longer than its stated length"); return 1; }
    return c->o->got == c->o->n ? 2 : 0;
}

int alac_decode(plat_file f, int64_t size, const pcm_info *p, pcm_out *o, char *why, size_t whyn) {
    ctx c;
    memset(&c, 0, sizeof c);
    c.f = f;
    c.size = size;
    c.o = o;
    c.why = why;
    c.whyn = whyn;
    int rc = 1;
    if (p->kind != PCM_ALAC) snprintf(why, whyn, "not an ALAC track");
    else if (!m4a_alac_packets(f, size, p->mp4_track_id, c.cookie, &c.skip, on_packet, &c, why, whyn)) {
        if (o->got != o->n) snprintf(why, whyn, "decoded %zu samples, the file says %zu", o->got, o->n);
        else rc = 0;
    }
    alac_free(&c.d);
    free(c.pkt);
    free(c.win);
    free(c.out);
    return rc;
}
