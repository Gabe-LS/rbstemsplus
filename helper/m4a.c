/* The MP4 sound track's header for the Windows helper (pcm.h) on a plat_file with 64-bit offsets.
 * moov is read whole (capped) and parsed in
 * memory, every box inside its parent: a size past the parent's end, a count past its box, a
 * 64-bit size or size 0 (to the parent's end) never reads outside what was read. The ALAC
 * sample table (stsz/stz2, stsc, stco/co64) is walked here for alac.c, and an ALAC track's
 * length is AudioToolbox's (alac_length). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcm.h"
#if defined(_WIN32) || defined(RBSP_TEST_MF_TRACK)
#include "aac_mf.h"
#endif

enum { MOOV_MAX = 64 << 20 };            /* a long video's moov is a few MB */

static uint32_t be16(const unsigned char *b) { return (uint32_t)b[0] << 8 | b[1]; }
static uint32_t be32(const unsigned char *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static uint64_t be64(const unsigned char *b) { return (uint64_t)be32(b) << 32 | be32(b + 4); }

typedef struct { const unsigned char *p; size_t n; } span;

/* the next box of s: its type and body; 0 when there is one */
static int next_box(span *s, char type[4], span *body) {
    if (s->n < 8) return 1;
    uint64_t size = be32(s->p), hdr = 8;
    if (size == 1) {
        if (s->n < 16) return 1;
        size = be64(s->p + 8);
        hdr = 16;
    } else if (size == 0) {
        size = s->n;
    }
    if (size < hdr || size > s->n) return 1;
    memcpy(type, s->p + 4, 4);
    body->p = s->p + hdr;
    body->n = (size_t)(size - hdr);
    s->p += size;
    s->n -= (size_t)size;
    return 0;
}

static int find(span s, const char *type, span *body) {
    char t[4];
    while (!next_box(&s, t, body))
        if (!memcmp(t, type, 4)) return 0;
    return 1;
}

static int find_path(span s, const char *const *path, int n, span *body) {
    for (int i = 0; i < n; i++) {
        if (find(s, path[i], body)) return 1;
        s = *body;
    }
    return 0;
}

/* the top-level moov, read whole: 0 and *buf (free it), or 1 and why */
static int read_moov(plat_file f, int64_t size, unsigned char **buf, size_t *len, char *why, size_t whyn) {
    int64_t off = 0;
    unsigned char h[16];
    *buf = NULL;
    while (size - off >= 8) {
        if (pcm_pread_all(f, h, 8, off)) break;
        uint64_t bs = be32(h), hdr = 8;
        if (bs == 1) {
            if (size - off < 16 || pcm_pread_all(f, h + 8, 8, off + 8)) break;
            bs = be64(h + 8);
            hdr = 16;
        } else if (bs == 0) {
            bs = (uint64_t)(size - off);
        }
        /* a moov past the file's end is read to the end, as AudioToolbox does (a download cut
         * short in the metadata after the sample table still plays) */
        if (bs > (uint64_t)(size - off) && !memcmp(h + 4, "moov", 4)) bs = (uint64_t)(size - off);
        if (bs < hdr || bs > (uint64_t)(size - off)) break;
        if (!memcmp(h + 4, "moov", 4)) {
            if (bs - hdr > MOOV_MAX) { snprintf(why, whyn, "the MP4's header (moov) is too large"); return 1; }
            *len = (size_t)(bs - hdr);
            if (!(*buf = malloc(*len ? *len : 1))) { snprintf(why, whyn, "out of memory"); return 1; }
            if (pcm_pread_all(f, *buf, *len, off + (int64_t)hdr)) {
                free(*buf);
                *buf = NULL;
                snprintf(why, whyn, "the MP4's header can't be read");
                return 1;
            }
            return 0;
        }
        off += (int64_t)bs;
    }
    snprintf(why, whyn, "no MP4 header (moov)");
    return 1;
}

typedef struct {
    span entry;                          /* stsd's first sample entry: its body */
    char fmt[4];
    span stsz, stz2, stsc, stco, co64;   /* bodies; n 0 when missing */
    uint32_t timescale;
    uint64_t stts_total;                 /* the sample table's length (in timescale units) */
    uint32_t stts_n;
    uint32_t descriptions;
    int fragmented;                      /* moov has mvex */
} track;

/* Walk only audio tracks, leaving the cursor after the returned trak. */
static int next_sound_track(span *s, span *tb) {
    static const char *const hdlr[] = {"mdia", "hdlr"};
    char ty[4];
    span b;
    while (!next_box(s, ty, tb)) {
        if (memcmp(ty, "trak", 4)) continue;
        if (!find_path(*tb, hdlr, 2, &b) && b.n >= 12 && !memcmp(b.p + 8, "soun", 4)) return 0;
    }
    return 1;
}

static uint32_t track_id(span tb) {
    span b;
    if (find(tb, "tkhd", &b) || b.n < 16) return 0;
    if (b.p[0] == 0) return be32(b.p + 12);
    if (b.p[0] == 1 && b.n >= 24) return be32(b.p + 20);
    return 0;
}

static int sample_description(span tb, span *st, track *t, char *why, size_t whyn) {
    static const char *const stbl[] = {"mdia", "minf", "stbl"};
    span b;
    if (find_path(tb, stbl, 3, st)) { snprintf(why, whyn, "the sound track has no sample table"); return 1; }
    if (find(*st, "stsd", &b) || b.n < 8 || !be32(b.p + 4)) { snprintf(why, whyn, "the sound track has no sample description"); return 1; }
    t->descriptions = be32(b.p + 4);
    span e = {b.p + 8, b.n - 8};
    if (next_box(&e, t->fmt, &t->entry)) { snprintf(why, whyn, "the sound track's sample description is cut short"); return 1; }
    return 0;
}

static int sound_track(span moov, span tb, track *t, char *why, size_t whyn) {
    memset(t, 0, sizeof *t);
    span b, st;
    if (sample_description(tb, &st, t, why, whyn)) return 1;
    t->fragmented = !find(moov, "mvex", &b);
    static const char *const mdhd[] = {"mdia", "mdhd"};
    if (!find_path(tb, mdhd, 2, &b)) {
        if (b.n >= 32 && b.p[0] == 1) t->timescale = be32(b.p + 20);
        else if (b.n >= 20 && b.p[0] == 0) t->timescale = be32(b.p + 12);
    }
    /* the time table: AAC's length (mp4.c); a cut-short one counts as none (ALAC doesn't use it) */
    if (!find(st, "stts", &b) && b.n >= 8 && (uint64_t)be32(b.p + 4) * 8 <= b.n - 8) {
        uint32_t n = be32(b.p + 4);
        uint64_t total = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t d = (uint64_t)be32(b.p + 8 + 8 * (size_t)i) * be32(b.p + 12 + 8 * (size_t)i);
            total = total + d < total ? UINT64_MAX : total + d;
        }
        t->stts_total = total;
        t->stts_n = n;
    }
    if (find(st, "stsz", &t->stsz)) t->stsz.n = 0;
    if (find(st, "stz2", &t->stz2)) t->stz2.n = 0;
    if (find(st, "stsc", &t->stsc)) t->stsc.n = 0;
    if (find(st, "stco", &t->stco)) t->stco.n = 0;
    if (find(st, "co64", &t->co64)) t->co64.n = 0;
    return 0;
}

/* the sample entry's children (an ISO entry's, or a QuickTime v1/v2 entry's, or inside its 'wave') */
static int entry_child(const track *t, const char *type, span *body) {
    const span *e = &t->entry;
    if (e->n < 28) return 1;
    size_t at = 28, v = be16(e->p + 8);
    if (v == 1) at = 44;
    else if (v == 2) at = 64;
    if (at > e->n) return 1;
    span c = {e->p + at, e->n - at}, w;
    if (!find(c, type, body)) return 0;
    return find(c, "wave", &w) || find(w, type, body);
}

/* the entry's rate and channels (16.16 rate; a v2 entry's float64 and 32-bit count) */
static void entry_format(const track *t, int *rate, int *ch) {
    const span *e = &t->entry;
    *rate = *ch = 0;
    if (e->n < 28) return;
    if (be16(e->p + 8) == 2 && e->n >= 44) {
        uint64_t u = be64(e->p + 32);
        double r;
        memcpy(&r, &u, sizeof r);
        *rate = r >= 1 && r < 16e6 ? (int)r : 0;
        uint32_t c = be32(e->p + 40);
        *ch = c <= 64 ? (int)c : 0;
        return;
    }
    *ch = (int)be16(e->p + 16);
    *rate = (int)(be32(e->p + 24) >> 16);
}

/* the ALACSpecificConfig of an 'alac' child (after its version and flags): 0, or 1 and why */
static int alac_cookie(const track *t, unsigned char cookie[24], char *why, size_t whyn) {
    span a;
    if (entry_child(t, "alac", &a) || a.n < 28) { snprintf(why, whyn, "ALAC without its magic cookie"); return 1; }
    memcpy(cookie, a.p + 4, 24);
    uint32_t frames = be32(cookie);
    int bits = cookie[5], ch = cookie[9];
    /* compatibleVersion isn't checked: AudioToolbox decodes a cookie whatever it says */
    if (bits != 16 && bits != 20 && bits != 24 && bits != 32) { snprintf(why, whyn, "ALAC with %d bits per sample", bits); return 1; }
    if (ch < 1 || ch > 8) { snprintf(why, whyn, "ALAC with %d channels", ch); return 1; }
    if (frames < 1 || frames > 65536) { snprintf(why, whyn, "ALAC with %u-sample packets", frames); return 1; }
    return 0;
}

/* an expandable descriptor length (1-4 bytes of 7 bits) */
static int desc_len(span *s, size_t *len) {
    *len = 0;
    for (int i = 0; i < 4; i++) {
        if (!s->n) return 1;
        unsigned char b = *s->p++;
        s->n--;
        *len = *len << 7 | (b & 0x7f);
        if (!(b & 0x80)) return *len > s->n;
    }
    return 1;
}

/* the next descriptor of s: its tag and body */
static int next_desc(span *s, int *tag, span *body) {
    if (!s->n) return 1;
    *tag = *s->p++;
    s->n--;
    size_t len;
    if (desc_len(s, &len)) return 1;
    body->p = s->p;
    body->n = len;
    s->p += len;
    s->n -= len;
    return 0;
}

/* MSB-first bits of an AudioSpecificConfig */
typedef struct { const unsigned char *p; size_t n, bit; } bits;
static int getbits(bits *b, int k, uint32_t *v) {
    *v = 0;
    if (b->bit + (size_t)k > b->n * 8) return 1;
    for (int i = 0; i < k; i++, b->bit++) *v = *v << 1 | ((b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1);
    return 0;
}
static int asc_aot(bits *b, uint32_t *aot) {
    if (getbits(b, 5, aot)) return 1;
    if (*aot != 31) return 0;
    uint32_t x;
    if (getbits(b, 6, &x)) return 1;
    *aot = 32 + x;
    return 0;
}
static int asc_rate(bits *b, uint32_t *rate) {
    static const uint32_t r[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    uint32_t i;
    if (getbits(b, 4, &i)) return 1;
    if (i == 15) return getbits(b, 24, rate);
    *rate = i < 13 ? r[i] : 0;
    return 0;
}

/* Ordinary LC's GASpecificConfig uses 1024 frames and has no core dependency or extension.
 * Allow byte padding and the optional sync extension that explicitly signals no SBR. */
static int lc_without_extensions(bits bs) {
    uint32_t value;
    if (getbits(&bs, 3, &value) || value) return 0;
    size_t left = bs.n * 8 - bs.bit;
    if (left >= 8) {
        if (getbits(&bs, 11, &value) || value != 0x2b7 || getbits(&bs, 5, &value) || value != 5 ||
            getbits(&bs, 1, &value) || value) return 0;
        left = bs.n * 8 - bs.bit;
    }
    return left < 8 && !getbits(&bs, (int)left, &value) && !value;
}

/* an 'mp4a' entry's esds: PCM_AAC and the AudioSpecificConfig's rate and channels when it has
 * one (else the entry's), or 1 and why */
static int aac_esds(const track *t, pcm_info *p, int core, char *why, size_t whyn) {
    span e, d, b = {0};
    int tag;
    if (entry_child(t, "esds", &e) || e.n < 4) { snprintf(why, whyn, "MPEG-4 audio without its decoder description (esds)"); return 1; }
    e.p += 4;                            /* version and flags */
    e.n -= 4;
    if (next_desc(&e, &tag, &d)) { snprintf(why, whyn, "the MP4's decoder description (esds) is cut short"); return 1; }
    if (tag == 3) {                      /* ES_Descriptor: ES_ID, flags, their optional fields */
        if (d.n < 3) { snprintf(why, whyn, "the MP4's decoder description (esds) is cut short"); return 1; }
        unsigned fl = d.p[2];
        size_t skip = 3 + (fl & 0x80 ? 2 : 0);
        if (fl & 0x40) skip += 1 + (skip < d.n ? d.p[skip] : 0);
        if (fl & 0x20) skip += 2;
        if (skip > d.n) { snprintf(why, whyn, "the MP4's decoder description (esds) is cut short"); return 1; }
        d.p += skip;
        d.n -= skip;
        if (next_desc(&d, &tag, &b)) tag = 0;
        d = b;
    }
    if (tag != 4 || d.n < 13) { snprintf(why, whyn, "the MP4's decoder description (esds) has no decoder config"); return 1; }
    int oti = d.p[0];
    if (oti == 0x69 || oti == 0x6b) { snprintf(why, whyn, "MPEG audio (MP3) in an MP4: not read by this helper"); return 1; }
    if (oti != 0x40 && oti != 0x66 && oti != 0x67 && oti != 0x68) { snprintf(why, whyn, "MP4 audio of object type 0x%02x: not AAC or ALAC", oti); return 1; }
    entry_format(t, &p->rate, &p->ch);
    span dsi = {d.p + 13, d.n - 13};
    if (next_desc(&dsi, &tag, &b) || tag != 5) b.n = 0;
    if (b.n) {                           /* AudioSpecificConfig */
        bits bs = {b.p, b.n, 0};
        uint32_t aot, rate, chc, ext;
        if (!asc_aot(&bs, &aot) && !asc_rate(&bs, &rate) && !getbits(&bs, 4, &chc)) {
            if (oti == 0x40 && aot >= 32 && aot <= 34) { snprintf(why, whyn, "MPEG audio (MP3) in an MP4: not read by this helper"); return 1; }
            int aac = aot == 1 || aot == 2 || aot == 3 || aot == 4 || aot == 5 || aot == 29 || aot == 23 || aot == 39;
            if (oti == 0x40 && !aac) { snprintf(why, whyn, "MPEG-4 audio object type %u: not AAC", aot); return 1; }
            if ((oti == 0x40 || oti == 0x67) && aot == 2 && (chc == 1 || chc == 2) &&
                t->descriptions == 1 && lc_without_extensions(bs)) {
                p->aac_lc_rate = (int)rate;
                p->aac_lc_ch = (int)chc;
            }
            if (!core && (aot == 5 || aot == 29) && !asc_rate(&bs, &ext) && ext) rate = ext;  /* explicit SBR */
            if (rate) p->rate = (int)rate;
            if (chc >= 1 && chc <= 7) p->ch = chc == 7 ? 8 : (int)chc;
            if (!core && aot == 29 && p->ch == 1) p->ch = 2;   /* parametric stereo */
        }
    }
    p->kind = PCM_AAC;
    return 0;
}

#if defined(_WIN32) || defined(RBSP_TEST_MF_TRACK)
/* The MPEG-4 source can assign its own IDs. Match the last Source Reader audio stream
 * to a unique sample description, using AAC's core format for the native MF type. */
static int match_mf_track(span s, const aac_mf_info *mf, span *selected, char *why, size_t whyn) {
    enum { MF_AAC = 0x1610, MF_ALAC = 0x6c61 };
    span tb;
    int found = 0;
    while (!next_sound_track(&s, &tb)) {
        track t;
        span st;
        if (sample_description(tb, &st, &t, why, whyn)) return 1;
        pcm_info p = {0};
        if (mf->native_sub == MF_AAC && !memcmp(t.fmt, "mp4a", 4)) {
            char detail[128];
            if (aac_esds(&t, &p, 1, detail, sizeof detail)) continue;
        } else if (mf->native_sub == MF_ALAC && !memcmp(t.fmt, "alac", 4)) {
            unsigned char c[24];
            if (alac_cookie(&t, c, why, whyn)) return 1;
            p.rate = (int)be32(c + 20);
            p.ch = c[9];
        } else {
            continue;
        }
        if (p.rate <= 0 || p.ch <= 0 || p.rate != mf->native_rate || p.ch != mf->native_ch) continue;
        if (found) {
            snprintf(why, whyn, "multiple MP4 sound tracks match Media Foundation's selected stream");
            return 1;
        }
        *selected = tb;
        found = 1;
    }
    if (!found) snprintf(why, whyn, "no MP4 sound track matches Media Foundation's selected stream");
    return !found;
}
#endif

/* every packet of the sample table in order: fn(ctx, offset, bytes) (0 on, 1 an error, 2 done);
 * *packets: how many the table has. 0 (all of them, or fn said done), 1 and why, or 2 (fn's
 * error: why is fn's) */
static int each_packet(const track *t, int64_t size, m4a_packet_fn fn, void *ctx, uint32_t *packets, char *why, size_t whyn) {
    /* packet sizes: stsz (a fixed size or one per packet), else stz2 (4, 8 or 16 bits each) */
    uint32_t count, fixed = 0, field = 32;
    const unsigned char *sizes;
    if (t->stsz.n >= 12) {
        fixed = be32(t->stsz.p + 4);
        count = be32(t->stsz.p + 8);
        sizes = t->stsz.p + 12;
        if (!fixed && (uint64_t)count * 4 > t->stsz.n - 12) { snprintf(why, whyn, "the sample size table is cut short"); return 1; }
    } else if (t->stz2.n >= 12) {
        field = t->stz2.p[7];
        count = be32(t->stz2.p + 8);
        sizes = t->stz2.p + 12;
        if ((field != 4 && field != 8 && field != 16) || ((uint64_t)count * field + 7) / 8 > t->stz2.n - 12) {
            snprintf(why, whyn, "the sample size table is cut short");
            return 1;
        }
    } else {
        snprintf(why, whyn, "no sample size table");
        return 1;
    }
    if (packets) *packets = count;
    /* chunk offsets */
    int wide = !t->stco.n && t->co64.n;
    const span *co = wide ? &t->co64 : &t->stco;
    if (co->n < 8) { snprintf(why, whyn, "no chunk offset table"); return 1; }
    /* a count past the box: the offsets the box holds (AudioToolbox reads such a file) */
    uint32_t chunks = be32(co->p + 4);
    if ((uint64_t)chunks * (wide ? 8 : 4) > co->n - 8) chunks = (uint32_t)((co->n - 8) / (wide ? 8 : 4));
    /* sample-to-chunk runs: first chunks from 1, increasing */
    if (t->stsc.n < 8) { snprintf(why, whyn, "no sample-to-chunk table"); return 1; }
    uint32_t runs = be32(t->stsc.p + 4);
    if ((uint64_t)runs * 12 > t->stsc.n - 8 || !runs || be32(t->stsc.p + 8) != 1) {
        snprintf(why, whyn, "the sample-to-chunk table is cut short or out of order");
        return 1;
    }
    uint32_t s = 0, r = 0;
    for (uint32_t c = 1; c <= chunks && s < count; c++) {
        while (r + 1 < runs && be32(t->stsc.p + 8 + 12 * (size_t)(r + 1)) <= c) {
            if (be32(t->stsc.p + 8 + 12 * (size_t)(r + 1)) <= be32(t->stsc.p + 8 + 12 * (size_t)r)) {
                snprintf(why, whyn, "the sample-to-chunk table is out of order");
                return 1;
            }
            r++;
        }
        uint32_t per = be32(t->stsc.p + 12 + 12 * (size_t)r);
        uint64_t off = wide ? be64(co->p + 8 + 8 * (size_t)(c - 1)) : be32(co->p + 8 + 4 * (size_t)(c - 1));
        for (uint32_t k = 0; k < per && s < count; k++, s++) {
            uint32_t n = fixed;
            if (!fixed) {
                if (field == 32) n = be32(sizes + 4 * (size_t)s);
                else if (field == 16) n = be16(sizes + 2 * (size_t)s);
                else if (field == 8) n = sizes[s];
                else n = (sizes[s / 2] >> (s & 1 ? 0 : 4)) & 15;
            }
            if (off > (uint64_t)size || n > (uint64_t)size - off) { snprintf(why, whyn, "packet %u is past the end of the file", s); return 1; }
            int e = fn(ctx, (int64_t)off, n);
            if (e == 2) return 0;
            if (e) return 2;
            off += n;
        }
    }
    if (s != count) { snprintf(why, whyn, "the chunk table holds %u of the %u packets", s, count); return 1; }
    return 0;
}

/* iTunes' gapless info (moov/udta/meta/ilst/----, name iTunSMPB): " 00000000 PPPPPPPP RRRRRRRR
 * VVVVVVVVVVVVVVVV ..." in hex: priming, remainder and valid frames. 1 when there is one with
 * valid frames (0 there: AudioToolbox ignores it, its priming too) */
static int itunsmpb(span moov, uint64_t *priming, uint64_t *valid) {
    static const char *const path[] = {"udta", "meta"};
    span meta, ilst, item, b;
    if (find_path(moov, path, 2, &meta)) return 0;
    if (!(meta.n >= 8 && !memcmp(meta.p + 4, "hdlr", 4)) && meta.n >= 4) {   /* ISO's meta: version and flags first */
        meta.p += 4;
        meta.n -= 4;
    }
    if (find(meta, "ilst", &ilst)) return 0;
    char ty[4];
    while (!next_box(&ilst, ty, &item)) {
        if (memcmp(ty, "----", 4) || find(item, "name", &b) || b.n != 12 || memcmp(b.p + 4, "iTunSMPB", 8) || find(item, "data", &b) || b.n < 8)
            continue;
        char txt[160];
        size_t n = b.n - 8 < sizeof txt - 1 ? b.n - 8 : sizeof txt - 1;
        memcpy(txt, b.p + 8, n);
        txt[n] = 0;
        unsigned long long v[4];
        char *s = txt, *e;
        for (int i = 0; i < 4; i++, s = e) {
            v[i] = strtoull(s, &e, 16);
            if (e == s) return 0;
        }
        if (!v[3]) return 0;
        *priming = v[1];
        *valid = v[3];
        return 1;
    }
    return 0;
}

static int last_packet(void *ctx, int64_t off, uint32_t n) {
    int64_t *last = ctx;
    last[0] = off;
    last[1] = n;
    return 0;
}

/* an ALAC track's length as AudioToolbox gives it (ExtAudioFile's FileLengthFrames; measured: the
 * sample table's durations, mdhd and the edit list don't count): iTunSMPB's valid frames, after
 * its priming frames; without it, every packet but the last a full frame, the last as its own
 * header says (a partial frame's count; a count of 0, or a full frame: frameLength) */
static int alac_length(plat_file f, int64_t size, span moov, const track *t, const unsigned char cookie[24], uint64_t *frames,
                       uint64_t *skip, char *why, size_t whyn) {
    uint64_t priming, valid;
    *skip = 0;
    int64_t last[2] = {-1, 0};
    uint32_t packets = 0;
    if (each_packet(t, size, last_packet, last, &packets, why, whyn)) return 1;
    uint64_t frameLength = be32(cookie), n = frameLength;
    if (itunsmpb(moov, &priming, &valid)) {
        /* the packets can't hold more than packets x frameLength frames: a claim beyond that is
         * refused here (AudioToolbox's run refuses it too, decoding fewer), never sized for */
        if (priming > (uint64_t)packets * frameLength || valid > (uint64_t)packets * frameLength - priming) {
            snprintf(why, whyn, "ALAC whose iTunSMPB says more frames than its packets hold");
            return 1;
        }
        *frames = valid;
        *skip = priming;
        return 0;
    }
    *frames = 0;
    if (!packets) return 0;
    unsigned char h[8] = {0};
    if (pcm_pread_all(f, h, last[1] < 8 ? (size_t)last[1] : 8, last[0])) { snprintf(why, whyn, "the file can't be read"); return 1; }
    /* the first element: tag (3 bits), instance (4), unused (12), partial frame (1), shift (2),
     * escape (1), then a partial frame's 32-bit count */
    uint64_t b = (uint64_t)be32(h) << 32 | be32(h + 4);
    unsigned tag = (unsigned)(b >> 61);
    if ((tag == 0 || tag == 1 || tag == 3) && (b >> 44 & 1) && (b >> 9 & 0xffffffffu)) n = b >> 9 & 0xffffffffu;
    *frames = (uint64_t)(packets - 1) * frameLength + n;
    return 0;
}

int m4a_probe(plat_file f, int64_t size, const char *ext, pcm_info *p, char *why, size_t whyn) {
    memset(p, 0, sizeof *p);
    p->mf_stream = -1;
    unsigned char *buf;
    size_t len;
    if (read_moov(f, size, &buf, &len, why, whyn)) return 1;
    span moov = {buf, len};
    track t;
    int rc = 1;
    span scan = moov, selected;
    if (next_sound_track(&scan, &selected)) { snprintf(why, whyn, "no sound track in the MP4"); goto done; }
#if defined(_WIN32) || defined(RBSP_TEST_MF_TRACK)
    aac_mf_info mf;
    span second;
    if (!next_sound_track(&scan, &second)) {
        if (aac_mf_probe(f, ext, &mf, why, whyn) || match_mf_track(moov, &mf, &selected, why, whyn)) goto done;
        p->mp4_track_id = track_id(selected);
        if (!p->mp4_track_id) { snprintf(why, whyn, "the selected MP4 sound track has no track ID"); goto done; }
        p->mf_stream = mf.stream;
    }
#else
    (void)ext;
#endif
    if (sound_track(moov, selected, &t, why, whyn)) goto done;
    /* fragmented (moof): not read here; AudioToolbox reads fragmented ALAC, decode.c refuses AAC */
    if (t.fragmented || (!t.stts_n && memcmp(t.fmt, "alac", 4))) { snprintf(why, whyn, "no sample table for the sound track (fragmented MP4?)"); goto done; }
    if (!memcmp(t.fmt, "alac", 4)) {
        unsigned char c[24];
        uint64_t skip;
        if (alac_cookie(&t, c, why, whyn) || alac_length(f, size, moov, &t, c, &p->frames, &skip, why, whyn)) goto done;
        p->kind = PCM_ALAC;
        p->bits = c[5];
        p->ch = c[9];
        p->rate = (int)be32(c + 20);
        if (!p->rate) entry_format(&t, &p->rate, &(int){0});
        if (!p->rate) p->rate = (int)t.timescale;
        if (p->rate > 0) p->seconds = (double)p->frames / p->rate;     /* AudioToolbox's estimate: the length */
    } else if (!memcmp(t.fmt, "mp4a", 4)) {
        if (aac_esds(&t, p, 0, why, whyn)) goto done;
#if defined(_WIN32) || defined(RBSP_TEST_MF_TRACK)
        if (p->mf_stream < 0) {
            if (!aac_mf_available(why, whyn)) goto done;
            /* A 22.05 kHz core can give 44.1 kHz PCM with implicit SBR. */
            if (p->rate == 22050 || p->rate < 1 || p->ch < 1) {
                if (aac_mf_probe(f, ext, &mf, why, whyn)) goto done;
                p->mf_stream = mf.stream;
            }
        }
        if (p->mf_stream >= 0) {
            if (mf.rate < 1 || mf.ch < 1) {
                snprintf(why, whyn, "the sound track Media Foundation reads isn't AAC (subtype 0x%08lx)",
                         (unsigned long)mf.native_sub);
                goto done;
            }
            p->rate = mf.rate;
            p->ch = mf.ch;
        }
#endif
        if (p->rate > 0) {
            uint64_t rate = (uint64_t)p->rate;
#if !defined(_WIN32) && !defined(RBSP_TEST_MF_TRACK)
            if (rate == 22050) rate = 44100;  /* header-only host probe, with no MF */
#endif
            uint64_t scale = t.timescale ? t.timescale : rate;
            /* Floor as the Mac's UNITS() does. Split the product to avoid overflow. */
            uint64_t whole = t.stts_total / scale;
            uint64_t rest = t.stts_total % scale * rate / scale;
            if (whole > (INT64_MAX - rest) / rate) { snprintf(why, whyn, "the AAC sample table is too long"); goto done; }
            p->frames = whole * rate + rest;
            p->seconds = (double)p->frames / rate;
        }
    } else {
        char s[5];
        for (int i = 0; i < 4; i++) s[i] = t.fmt[i] >= 32 && t.fmt[i] < 127 ? t.fmt[i] : '?';
        s[4] = 0;
        snprintf(why, whyn, "'%s' audio in an MP4: not AAC or ALAC", s);
        goto done;
    }
    if (p->rate < 1 || p->ch < 1) { snprintf(why, whyn, "the sound track has no sample rate or channels"); goto done; }
    rc = 0;
done:
    free(buf);
    return rc;
}

int m4a_alac_packets(plat_file f, int64_t size, uint32_t id, unsigned char cookie[24], uint64_t *skip, m4a_packet_fn fn, void *ctx,
                     char *why, size_t whyn) {
    unsigned char *buf;
    size_t len;
    if (read_moov(f, size, &buf, &len, why, whyn)) return 1;
    span moov = {buf, len};
    track t;
    uint64_t frames;
    int rc = 1;
    span scan = moov, selected = {0}, tb;
    while (!next_sound_track(&scan, &tb)) {
        if (!id) { selected = tb; break; }
        if (track_id(tb) != id) continue;
        if (selected.p) { snprintf(why, whyn, "the MP4 has duplicate sound track IDs"); goto done; }
        selected = tb;
    }
    if (!selected.p) { snprintf(why, whyn, "no selected sound track in the MP4"); goto done; }
    if (sound_track(moov, selected, &t, why, whyn)) goto done;
    if (memcmp(t.fmt, "alac", 4)) { snprintf(why, whyn, "the sound track isn't ALAC"); goto done; }
    if (alac_cookie(&t, cookie, why, whyn) || alac_length(f, size, moov, &t, cookie, &frames, skip, why, whyn)) goto done;
    rc = each_packet(&t, size, fn, ctx, NULL, why, whyn) ? 1 : 0;
done:
    free(buf);
    return rc;
}

