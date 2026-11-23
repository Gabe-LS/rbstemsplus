/* pcm.c: WAV, AIFF and FLAC for the Windows helper, and the dispatch by content (pcm.h). What
 * AudioToolbox does on the Mac (decode.c's probe and decode_pcm), done from the file's bytes:
 *  - WAV (RIFF and RF64): PCM 8 (unsigned), 16, 24, 32 bits and IEEE float 32, also as
 *    WAVE_FORMAT_EXTENSIBLE (a smaller valid width in a wider container reads as the container:
 *    the samples are left-justified there). IEEE float 64 is refused (T2, PC session 1).
 *  - AIFF and AIFF-C: rekordbox's own parser takes only NONE, twos, sowt and in24 (helper.md §0
 *    1, from its code): every other compression is refused, as there.
 *  - FLAC: libFLAC's stream decoder, on the file's handle (no path handed to libFLAC, so a long
 *    or Unicode path needs nothing of it).
 * Every read goes through plat_file: binary, 64-bit offsets. */
#include "pcm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <FLAC/stream_decoder.h>
#include "big.h"

/* ---------------------------------------------------------------- the decoded stream */

/* Room for at most this many frames is made before any is decoded (8 MB of floats); past it the
 * buffer grows, doubling, as frames are put, up to the stated length: a header that claims more
 * than its file holds costs only what its frames really decode to (the GPT review's High 2: a
 * 42-byte FLAC claiming 2^36 frames asked for 512 GiB up front). */
#define PCM_FIRST ((size_t)1 << 20)

int pcm_out_init(pcm_out *o, size_t n, int ch) {
    memset(o, 0, sizeof *o);
    if (ch < 1 || n > SIZE_MAX / (2 * sizeof(float)) - 1) return 1;
    size_t cap = n < PCM_FIRST ? n : PCM_FIRST;
    o->s = big_alloc(sizeof(float) * 2 * (cap ? cap : 1));
    o->n = n;
    o->cap = cap;
    o->ch = ch;
    return o->s ? 0 : 1;
}

void pcm_out_free(pcm_out *o) {
    big_free(o->s);
    memset(o, 0, sizeof *o);
}

/* room for `frames` more: 0, 1 past the stated length, 2 out of memory. The right channel sits
 * at s[cap..): it moves up when the buffer grows. */
static int pcm_room(pcm_out *o, size_t frames) {
    if (frames > o->n - o->got) return 1;
    if (o->got + frames <= o->cap) return 0;
    size_t cap = o->cap * 2 > o->got + frames ? o->cap * 2 : o->got + frames;
    if (cap > o->n) cap = o->n;
    float *s = big_realloc(o->s, sizeof(float) * 2 * cap);
    if (!s) return 2;
    memmove(s + cap, s + o->cap, sizeof(float) * o->got);
    o->s = s;
    o->cap = cap;
    return 0;
}

/* decode.c's rule: the first two channels, mono on both sides, an integer x 2^-31 */
int pcm_put_i32(pcm_out *o, const int32_t *x, size_t frames) {
    if (pcm_room(o, frames)) return 1;
    const float k = 1.0f / 2147483648.0f;
    size_t ch = (size_t)o->ch, c1 = ch > 1 ? 1 : 0;
    float *l = o->s + o->got, *r = o->s + o->cap + o->got;
    for (size_t i = 0; i < frames; i++) {
        l[i] = (float)x[i * ch] * k;
        r[i] = (float)x[i * ch + c1] * k;
    }
    o->got += frames;
    return 0;
}

int pcm_put_f32(pcm_out *o, const float *x, size_t frames) {
    if (pcm_room(o, frames)) return 1;
    size_t ch = (size_t)o->ch, c1 = ch > 1 ? 1 : 0;
    float *l = o->s + o->got, *r = o->s + o->cap + o->got;
    for (size_t i = 0; i < frames; i++) {
        l[i] = x[i * ch];
        r[i] = x[i * ch + c1];
    }
    o->got += frames;
    return 0;
}

/* ---------------------------------------------------------------- reading */

int pcm_pread_all(plat_file f, void *buf, size_t n, int64_t off) {
    size_t done = 0;
    while (done < n) {
        size_t got = 0;
        if (plat_pread(f, (char *)buf + done, n - done, off + (int64_t)done, &got) || !got) return 1;
        done += got;
    }
    return 0;
}

static uint16_t le16(const unsigned char *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t le32(const unsigned char *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint64_t le64(const unsigned char *b) { return (uint64_t)le32(b) | (uint64_t)le32(b + 4) << 32; }
static uint16_t be16(const unsigned char *b) { return (uint16_t)(b[0] << 8 | b[1]); }
static uint32_t be32(const unsigned char *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

static void set_length(pcm_info *p) { p->seconds = p->rate > 0 ? (double)p->frames / p->rate : 0; }

/* ---------------------------------------------------------------- WAV */

#define WAV_PCM 1
#define WAV_FLOAT 3
#define WAV_EXTENSIBLE 0xFFFE
/* KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT: the format tag, then this */
static const unsigned char WAV_GUID_TAIL[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

int pcm_probe_wav(plat_file f, int64_t size, pcm_info *p, char *why, size_t whyn) {
    unsigned char h[12];
    memset(p, 0, sizeof *p);
    p->kind = PCM_WAV;
    if (size < 12 || pcm_pread_all(f, h, 12, 0) || memcmp(h + 8, "WAVE", 4)) { snprintf(why, whyn, "not a WAV file"); return 1; }
    int rf64 = !memcmp(h, "RF64", 4);
    uint64_t ds64_data = 0, len = 0;
    int have_fmt = 0, have_data = 0, ext = 0, tag = 0, ch = 0, align = 0, bits = 0;
    uint32_t rate = 0;
    /* every chunk, in any order: a later fmt or data chunk replaces an earlier one (AudioToolbox
     * takes the last of each); a data chunk that runs to the file's end ends the scan */
    for (int64_t off = 12; off <= size - 8;) {
        unsigned char c[8];
        if (pcm_pread_all(f, c, 8, off)) goto read_error;
        uint64_t clen = le32(c + 4);
        int64_t body = off + 8;
        if (!memcmp(c, "ds64", 4) && rf64 && clen >= 16) {
            unsigned char d[16];
            if (size - body < 16 || pcm_pread_all(f, d, 16, body)) goto read_error;
            ds64_data = le64(d + 8);
        } else if (!memcmp(c, "fmt ", 4) && clen >= 16) {
            unsigned char d[40] = {0};
            ext = 0;
            size_t k = clen < sizeof d ? (size_t)clen : sizeof d;
            if (k > (uint64_t)(size - body) || pcm_pread_all(f, d, k, body)) goto read_error;
            tag = le16(d);
            ch = le16(d + 2);
            rate = le32(d + 4);
            align = le16(d + 12);
            bits = le16(d + 14);
            if (tag == WAV_EXTENSIBLE) {
                ext = 1;
                if (k < 40 || le16(d + 16) < 22 || memcmp(d + 26, WAV_GUID_TAIL, sizeof WAV_GUID_TAIL)) tag = -1;
                else tag = le16(d + 24);
            }
            have_fmt = 1;
        } else if (!memcmp(c, "data", 4)) {
            if (rf64 && clen == 0xFFFFFFFFu) clen = ds64_data;
            p->data_off = body;
            /* a data chunk that says more than the file holds: what is there (AudioToolbox's rule) */
            len = clen < (uint64_t)(size - body) ? clen : (uint64_t)(size - body);
            p->data_len = (int64_t)len;
            have_data = 1;
            if (len < clen) break;                          /* it runs to the end: nothing after it */
        }
        uint64_t padded = clen + (clen & 1);               /* chunks are padded to an even size */
        if (padded > (uint64_t)(size - body)) break;
        off = body + (int64_t)padded;
    }
    if (!have_fmt || !have_data) { snprintf(why, whyn, "a WAV file without its format or data"); return 1; }
    if (tag != WAV_PCM && tag != WAV_FLOAT) {
        if (tag < 0) snprintf(why, whyn, "an extensible WAV whose encoding rekordbox doesn't read");
        else snprintf(why, whyn, "a WAV encoding rekordbox doesn't read (format %d)", tag);
        return 1;
    }
    /* A sample's bytes. Extensible: the container (block align / channels), a smaller valid width
     * left-justified in it. Plain format 1 and 3: the declared width in whole bytes, packed, the
     * block align ignored (AudioToolbox's rule: mono 24-bit with block align 4, 2 or 0 reads 3
     * bytes a frame). */
    int cbytes = ext ? (ch > 0 ? align / ch : 0) : (bits + 7) / 8;
    if (tag == WAV_FLOAT) {
        int float_bits = ext && cbytes == 8 ? 64 : bits;
        /* T2 PC session 1: rekordbox for Windows refuses float64 WAV. For other plain format 3
         * widths, AudioToolbox refuses 16 and 24 bits and reads non-float widths as integer PCM. */
        if (float_bits == 64 || (!ext && (bits == 16 || bits == 24))) {
            snprintf(why, whyn, "a %d-bit float WAV: rekordbox can't load it", float_bits);
            return 1;
        }
        if (!ext && bits != 32) tag = WAV_PCM;
    }
    if (ch < 1 || rate < 1 || cbytes < 1 || cbytes > 4 || bits < 1 || bits > cbytes * 8 || (ext && cbytes * ch != align)
        || (tag == WAV_FLOAT && cbytes != 4)) {
        snprintf(why, whyn, "a WAV header that doesn't add up (%d channels, %d-bit, block %d)", ch, bits, align);
        return 1;
    }
    /* channels: more than 1024 only at a declared 32 bits, and at most 16383 (AudioToolbox's limits,
     * measured) */
    if ((ch > 1024 && bits != 32) || ch > 16383) { snprintf(why, whyn, "a WAV with %d channels: rekordbox can't load it", ch); return 1; }
    p->rate = (int)rate;
    p->ch = ch;
    p->bits = cbytes * 8;
    p->flt = tag == WAV_FLOAT ? 32 : 0;
    p->frames = len / ((uint64_t)cbytes * (uint64_t)ch);
    set_length(p);
    return 0;
read_error:
    snprintf(why, whyn, "the WAV file can't be read");
    return 1;
}

/* ---------------------------------------------------------------- AIFF */

/* the 80-bit IEEE extended sample rate */
static double ext80(const unsigned char *b) {
    int e = ((b[0] & 0x7F) << 8 | b[1]) - 16383 - 63;
    uint64_t m = 0;
    for (int i = 0; i < 8; i++) m = m << 8 | b[2 + i];
    double v = (double)m;
    for (; e > 0; e--) v *= 2;
    for (; e < 0; e++) v /= 2;
    return b[0] & 0x80 ? -v : v;
}

int pcm_probe_aiff(plat_file f, int64_t size, pcm_info *p, char *why, size_t whyn) {
    unsigned char h[12];
    memset(p, 0, sizeof *p);
    p->kind = PCM_AIFF;
    if (size < 12 || pcm_pread_all(f, h, 12, 0) || memcmp(h, "FORM", 4) || (memcmp(h + 8, "AIFF", 4) && memcmp(h + 8, "AIFC", 4))) {
        snprintf(why, whyn, "not an AIFF file");
        return 1;
    }
    int aifc = !memcmp(h + 8, "AIFC", 4), have_comm = 0, have_ssnd = 0, ch = 0, bits = 0;
    double rate = 0;
    char comp[5] = "NONE";
    /* COMM and SSND, in either order */
    for (int64_t off = 12; off + 8 <= size && !(have_comm && have_ssnd);) {
        unsigned char c[8];
        if (pcm_pread_all(f, c, 8, off)) break;
        uint64_t len = be32(c + 4);
        int64_t body = off + 8;
        if (!memcmp(c, "COMM", 4) && len >= 18 && !have_comm) {
            unsigned char d[22];
            size_t k = aifc && len >= 22 ? 22 : 18;
            if (pcm_pread_all(f, d, k, body)) break;
            ch = (int16_t)be16(d);
            bits = (int16_t)be16(d + 6);                   /* d + 2, the frame count: not used */
            rate = ext80(d + 8);
            if (aifc) {
                if (k < 22) { snprintf(why, whyn, "an AIFF-C file without its compression type"); return 1; }
                memcpy(comp, d + 18, 4);
            }
            have_comm = 1;
        } else if (!memcmp(c, "SSND", 4) && len >= 8 && !have_ssnd) {
            unsigned char d[4];
            if (pcm_pread_all(f, d, 4, body)) break;
            p->data_off = body + 8 + be32(d);
            p->data_len = (int64_t)len - 8 - be32(d);
            have_ssnd = 1;
        }
        off = body + (int64_t)len + (int64_t)(len & 1);
    }
    if (!have_comm || !have_ssnd) { snprintf(why, whyn, "an AIFF file without its format or sound data"); return 1; }
    int le = !memcmp(comp, "sowt", 4);
    if (memcmp(comp, "NONE", 4) && memcmp(comp, "twos", 4) && !le && memcmp(comp, "in24", 4)) {
        for (int i = 0; i < 4; i++) if (comp[i] < 0x20 || comp[i] > 0x7E) comp[i] = '?';
        snprintf(why, whyn, "AIFF-C compressed as '%s': rekordbox can't load it", comp);
        return 1;
    }
    if (!memcmp(comp, "in24", 4)) bits = 24;
    if (ch < 1 || bits < 1 || bits > 32 || !(rate >= 1 && rate < 2147483647.0) || p->data_len < 0) {
        snprintf(why, whyn, "an AIFF header that doesn't add up (%d channels, %d-bit)", ch, bits);
        return 1;
    }
    if (ch > 1024) { snprintf(why, whyn, "an AIFF with %d channels: rekordbox can't load it", ch); return 1; }   /* measured */
    /* the length is SSND's whole frames (after its offset), whatever COMM's count says:
     * AudioToolbox's rule; when those frames run past the file's end it is refused, as there (an
     * overshoot of less than a frame decodes) */
    uint64_t fb = (uint64_t)ch * (uint64_t)((bits + 7) / 8);
    p->frames = (uint64_t)p->data_len / fb;
    if (p->data_off + (int64_t)(p->frames * fb) > size) { snprintf(why, whyn, "the file ends before its sound data does"); return 1; }
    p->rate = (int)rate;
    p->ch = ch;
    p->bits = (bits + 7) / 8 * 8;                         /* left-justified in whole bytes */
    p->big_endian = !le;
    set_length(p);
    return 0;
}

/* WAV and AIFF: the data chunk, block by block, through the container width */
static int decode_raw(plat_file f, const pcm_info *p, pcm_out *o, char *why, size_t whyn) {
    size_t cb = (size_t)p->bits / 8, ch = (size_t)p->ch, frame = cb * ch;
    size_t n = (size_t)p->frames;
    /* frames a block: 32768, fewer when a frame is wide (many channels), so a block's buffers stay
     * within about 4 MB each */
    enum { BLOCK = 32768 };
    size_t blk = ((size_t)4 << 20) / (ch * 4);
    if (blk > BLOCK) blk = BLOCK;
    if (blk > n) blk = n;
    if (blk < 1) blk = 1;
    unsigned char *buf = malloc(blk * frame);
    void *conv = malloc(blk * ch * 4);
    int rc = 1;
    if (!buf || !conv) { snprintf(why, whyn, "out of memory"); goto done; }
    int64_t off = p->data_off;
    for (size_t at = 0; at < n;) {
        size_t want = n - at < blk ? n - at : blk;
        if (pcm_pread_all(f, buf, want * frame, off)) {
            snprintf(why, whyn, "decoded %zu samples, the file says %zu", at, (size_t)p->frames);
            goto done;
        }
        off += (int64_t)(want * frame);
        size_t m = want * ch;
        int bad;
        if (p->flt == 32) {
            float *x = conv;
            for (size_t i = 0; i < m; i++) { uint32_t u = le32(buf + 4 * i); memcpy(&x[i], &u, 4); }
            bad = pcm_put_f32(o, x, want);
        } else {
            int32_t *x = conv;
            const unsigned char *b = buf;
            for (size_t i = 0; i < m; i++, b += cb) {
                uint32_t u = 0;
                if (p->big_endian) for (size_t j = 0; j < cb; j++) u |= (uint32_t)b[j] << (24 - 8 * j);
                else for (size_t j = 0; j < cb; j++) u |= (uint32_t)b[j] << (32 - 8 * cb + 8 * j);
                if (p->kind == PCM_WAV && cb == 1) u ^= 0x80000000u;      /* 8-bit WAV is unsigned */
                x[i] = (int32_t)u;
            }
            bad = pcm_put_i32(o, x, want);
        }
        if (bad) { snprintf(why, whyn, "longer than its stated length"); goto done; }
        at += want;
    }
    if (o->got != (size_t)p->frames) {
        snprintf(why, whyn, "decoded %zu samples, the file says %zu", o->got, (size_t)p->frames);
        goto done;
    }
    rc = 0;
done:
    free(buf);
    free(conv);
    return rc;
}

/* ---------------------------------------------------------------- FLAC */

/* Where the stream starts after the ID3v2 tags in front of it, as many as there are (the Mac
 * decodes FLAC behind 2, 3 and 5): each tag 10 + its size, a footer not counted (AudioToolbox
 * refuses a FLAC whose tag has one, and so does this, finding no fLaC there). 0 without a tag. */
static int64_t id3_skip(plat_file f, int64_t size) {
    int64_t at = 0;
    unsigned char h[10];
    while (at <= size - 10 && !pcm_pread_all(f, h, 10, at) && !memcmp(h, "ID3", 3))
        at += 10 + ((int64_t)(h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F));
    return at;
}

static int read_flac(plat_file f, const pcm_info *p, pcm_out *o, uint64_t *frames, char *why, size_t whyn);

int pcm_probe_flac(plat_file f, int64_t size, pcm_info *p, char *why, size_t whyn) {
    memset(p, 0, sizeof *p);
    p->kind = PCM_FLAC;
    int64_t at = id3_skip(f, size);
    unsigned char h[42];
    /* "fLaC", then STREAMINFO: it must be the first metadata block, 34 bytes */
    if (at + 42 > size || pcm_pread_all(f, h, 42, at) || memcmp(h, "fLaC", 4) || (h[4] & 0x7F) != 0) {
        snprintf(why, whyn, "not a FLAC file");
        return 1;
    }
    const unsigned char *s = h + 8;
    uint32_t rate = (uint32_t)s[10] << 12 | (uint32_t)s[11] << 4 | s[12] >> 4;
    int ch = ((s[12] >> 1) & 7) + 1, bits = ((s[12] & 1) << 4 | s[13] >> 4) + 1;
    uint64_t total = (uint64_t)(s[13] & 0x0F) << 32 | be32(s + 14);
    if (!rate) { snprintf(why, whyn, "a FLAC header that doesn't add up (no sample rate)"); return 1; }
    /* the metadata blocks to their last one: a file that ends there holds no audio frame (the Mac's
     * AudioToolbox doesn't open it either) */
    int64_t mb = at + 4;
    for (int last = 0, k = 0; !last; k++) {
        unsigned char bh[4];
        if (k > 4096 || mb + 4 > size || pcm_pread_all(f, bh, 4, mb)) { snprintf(why, whyn, "a FLAC file without audio frames"); return 1; }
        last = bh[0] >> 7;
        mb += 4 + ((int64_t)bh[1] << 16 | bh[2] << 8 | bh[3]);
    }
    if (mb >= size) { snprintf(why, whyn, "a FLAC file without audio frames"); return 1; }
    p->rate = (int)rate;
    p->ch = ch;
    p->bits = bits;
    p->frames = total;
    p->data_off = at;                                     /* the fLaC, past any ID3v2 tags */
    if (!total && read_flac(f, p, NULL, &p->frames, why, whyn)) return 1;
    set_length(p);
    return 0;
}

/* libFLAC reads the stream from its fLaC on: pos and size count from base, the file's offset of
 * it, so libFLAC never meets an ID3 tag (it skips one, not two) */
typedef struct {
    plat_file f;
    int64_t base, pos, size;
    pcm_out *o;
    const pcm_info *p;
    uint64_t frames;
    int err, over;
} flac_io;

static FLAC__StreamDecoderReadStatus fl_read(const FLAC__StreamDecoder *d, FLAC__byte b[], size_t *n, void *c) {
    (void)d;
    flac_io *io = c;
    size_t got = 0;
    if (*n && plat_pread(io->f, b, *n, io->base + io->pos, &got)) { *n = 0; return FLAC__STREAM_DECODER_READ_STATUS_ABORT; }
    io->pos += (int64_t)got;
    *n = got;
    return got ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}
static FLAC__StreamDecoderSeekStatus fl_seek(const FLAC__StreamDecoder *d, FLAC__uint64 to, void *c) {
    (void)d;
    flac_io *io = c;
    if (to > (FLAC__uint64)io->size) return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    io->pos = (int64_t)to;
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}
static FLAC__StreamDecoderTellStatus fl_tell(const FLAC__StreamDecoder *d, FLAC__uint64 *at, void *c) {
    (void)d;
    *at = (FLAC__uint64)((flac_io *)c)->pos;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}
static FLAC__StreamDecoderLengthStatus fl_length(const FLAC__StreamDecoder *d, FLAC__uint64 *len, void *c) {
    (void)d;
    *len = (FLAC__uint64)((flac_io *)c)->size;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}
static FLAC__bool fl_eof(const FLAC__StreamDecoder *d, void *c) {
    (void)d;
    flac_io *io = c;
    return io->pos >= io->size;
}
static FLAC__StreamDecoderWriteStatus fl_write(const FLAC__StreamDecoder *d, const FLAC__Frame *fr, const FLAC__int32 *const buf[], void *c) {
    (void)d;
    flac_io *io = c;
    pcm_out *o = io->o;
    size_t want = fr->header.blocksize;
    if (fr->header.bits_per_sample != (unsigned)io->p->bits || fr->header.channels != (unsigned)io->p->ch ||
        fr->header.sample_rate != (unsigned)io->p->rate) {
        io->err = 1;                    /* a frame unlike STREAMINFO */
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    if (!o) {                           /* length of a stream written without a sample count */
        if (want > SIZE_MAX / (2 * sizeof(float)) - 1 - io->frames) {
            io->over = 1;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        io->frames += want;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    int room = pcm_room(o, want);
    if (room) { io->over = room == 1; io->err = room != 1; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
    /* as pcm_put_i32, from libFLAC's planar right-justified samples */
    const float k = 1.0f / 2147483648.0f;
    int sh = 32 - io->p->bits;
    const FLAC__int32 *a = buf[0], *b = buf[o->ch > 1 ? 1 : 0];
    float *l = o->s + o->got, *r = o->s + o->cap + o->got;
    for (size_t i = 0; i < want; i++) {
        l[i] = (float)(int32_t)((uint32_t)a[i] << sh) * k;
        r[i] = (float)(int32_t)((uint32_t)b[i] << sh) * k;
    }
    o->got += want;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void fl_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *c) {
    (void)d;
    (void)s;
    ((flac_io *)c)->err = 1;
}

static int read_flac(plat_file f, const pcm_info *p, pcm_out *o, uint64_t *frames, char *why, size_t whyn) {
    plat_stat st;
    if (plat_file_stat(f, &st)) { snprintf(why, whyn, "the file can't be read"); return 1; }
    flac_io io = {f, p->data_off, 0, st.size - p->data_off, o, p, 0, 0, 0};
    FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
    if (!d) { snprintf(why, whyn, "out of memory"); return 1; }
    int rc = 1;
    if (FLAC__stream_decoder_init_stream(d, fl_read, fl_seek, fl_tell, fl_length, fl_eof, fl_write, NULL, fl_error, &io)
        != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        snprintf(why, whyn, "libFLAC can't start");
    } else if (!FLAC__stream_decoder_process_until_end_of_stream(d) || io.err) {
        if (io.over) snprintf(why, whyn, "%s", o ? "longer than its stated length" : "a FLAC file that's too long");
        else snprintf(why, whyn, "decoding stopped with an error");
    } else if (o && o->got != (size_t)p->frames) {
        snprintf(why, whyn, "decoded %zu samples, the file says %zu", o->got, (size_t)p->frames);
    } else if (!o && !io.frames) {
        snprintf(why, whyn, "a FLAC file without audio frames");
    } else {
        if (frames) *frames = io.frames;
        rc = 0;
    }
    FLAC__stream_decoder_delete(d);
    return rc;
}

/* ---------------------------------------------------------------- by content */

int pcm_probe(plat_file f, const char *ext, pcm_info *p, char *why, size_t whyn) {
    plat_stat st;
    memset(p, 0, sizeof *p);
    if (plat_file_stat(f, &st) || st.kind != PLAT_FILE) { snprintf(why, whyn, "the file can't be read"); return 1; }
    if (!strcmp(ext, ".mp3")) return mp3_probe(f, st.size, p, why, whyn);
    unsigned char h[12] = {0};
    if (st.size >= 12 && pcm_pread_all(f, h, 12, 0)) { snprintf(why, whyn, "the file can't be read"); return 1; }
    int wav = (!memcmp(h, "RIFF", 4) || !memcmp(h, "RF64", 4)) && !memcmp(h + 8, "WAVE", 4);
    int aiff = !memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4));
    if (wav || aiff) {
        int rc = wav ? pcm_probe_wav(f, st.size, p, why, whyn) : pcm_probe_aiff(f, st.size, p, why, whyn);
        if (rc) return rc;
        if (!p->frames) { snprintf(why, whyn, "no audio"); return 1; }
        return 0;
    }
    if (!memcmp(h, "fLaC", 4)) return pcm_probe_flac(f, st.size, p, why, whyn);
    /* fLaC after ID3v2 tags, whatever the extension (AudioToolbox reads it as .flac, .wav, .m4a) */
    int64_t at = id3_skip(f, st.size);
    unsigned char g[4];
    if (at > 0 && at <= st.size - 4 && !pcm_pread_all(f, g, 4, at) && !memcmp(g, "fLaC", 4)) return pcm_probe_flac(f, st.size, p, why, whyn);
    static const char *const boxes[] = {"ftyp", "moov", "mdat", "free", "skip", "wide"};
    for (size_t i = 0; i < sizeof boxes / sizeof *boxes; i++)
        if (!memcmp(h + 4, boxes[i], 4)) return m4a_probe(f, st.size, ext, p, why, whyn);
    snprintf(why, whyn, "no audio stream RB Stems Plus can read");
    return 1;
}

int pcm_decode(plat_file f, const pcm_info *p, pcm_out *o, char *why, size_t whyn) {
    if (pcm_out_init(o, (size_t)p->frames, p->ch)) { snprintf(why, whyn, "out of memory"); return 1; }
    int rc;
    switch (p->kind) {
    case PCM_WAV:
    case PCM_AIFF: rc = decode_raw(f, p, o, why, whyn); break;
    case PCM_FLAC: rc = read_flac(f, p, o, NULL, why, whyn); break;
    case PCM_ALAC: {
        plat_stat st;
        rc = plat_file_stat(f, &st) ? (snprintf(why, whyn, "the file can't be read"), 1) : alac_decode(f, st.size, p, o, why, whyn);
        break;
    }
    default: snprintf(why, whyn, "not decoded here"); rc = 1; break;
    }
    /* decoded to its end, the buffer is exactly n frames a side (the planar layout s[0..n), s[n..2n)) */
    if (!rc && (o->got != o->n || o->cap != o->n)) { snprintf(why, whyn, "decoded %zu samples, the file says %zu", o->got, o->n); rc = 1; }
    if (rc) pcm_out_free(o);
    return rc;
}
