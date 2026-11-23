/* rekordbox 7.2.19's MP3 stream (rbxfrm::FileReadMp3, from its arm64 code), read from sample 0:
 * its frame map (FrameAnalyzer::analyze, checkContinuousMpegSync, checkXingVbri), then each mapped
 * frame fed alone to rekordbox's own libmpg123, set up as decoderOpen sets it up, one
 * decode_frame per feed, with its lag bookkeeping. Ported from the verified prototype
 * (mp3rule/rbmp3emu.py, rbmp3fast.py): every rule below, including the odd ones, is rekordbox's. */
#if defined(_WIN32) || defined(RBSP_WIN_DECODE)    /* pcm.h defines RBSP_WIN_DECODE on Windows */
/* Windows (and the Mac test that compares it, helper/tests/win-mp3.sh): the same reader on
 * common/plat.h (binary reads, 64-bit sizes, the library loaded by its full path), and mp3_probe */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcm.h"
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include "rbsp.h"
#include "big.h"

static const int M1_BR[3][16] = {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, -1},
                                 {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, -1},
                                 {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1}};
static const int M2_BR[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1};
static const int M1_SR[3] = {44100, 48000, 32000}, M2_SR[3] = {22050, 24000, 16000};
#define MASK 0xFFFE0CC0u                 /* sync + version + layer, sample rate, channel mode */

typedef struct { size_t off, start, size; } frame;
typedef struct { const unsigned char *d; size_t n; } buf;

/* 4 bytes at pos, or NULL past the end */
static const unsigned char *at4(const buf *b, size_t pos) { return pos + 4 <= b->n ? b->d + pos : NULL; }

static int hdr_ok(const unsigned char *b) {   /* byte0 FF, byte1 >= F0, layer != 0, bitrate 1..14, rate != 3 */
    return b[0] == 0xFF && b[1] >= 0xF0 && ((b[1] >> 1) & 3) && b[2] >= 0x10 && (b[2] >> 4) != 15 && ((b[2] >> 2) & 3) != 3;
}

static size_t fsize(const unsigned char *b) {
    int ver = (b[1] >> 3) & 1, layer = (b[1] >> 1) & 3;
    int br = ver ? M1_BR[layer ^ 3][b[2] >> 4] : M2_BR[b[2] >> 4];
    int sr = (ver ? M1_SR : M2_SR)[(b[2] >> 2) & 3];
    long spf = ver ? 1152 : 576;
    return (size_t)(((spf >> 3) * (long)br * 1000) / sr) + ((b[2] >> 1) & 1);
}

static uint32_t masked(const unsigned char *b) { return ((uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) & MASK; }

/* 0 = three frames in a row with the same masked header, 1 = no, 2 = the end or "TAG" */
static int check_continuous(const buf *d, size_t pos, uint32_t mh) {
    size_t p = pos;
    for (int k = 0; k < 3; k++) {
        const unsigned char *b = at4(d, p);
        if (!b || !memcmp(b, "TAG", 3)) return 2;
        if (k < 2) {
            if (!hdr_ok(b) || masked(b) != mh) return 1;
            p += fsize(b);
        } else if (b[0] != 0xFF || b[1] < 0xF0 || !((b[1] >> 1) & 3) || ((b[2] >> 2) & 3) == 3 || !(b[2] >= 0x10 && b[2] <= 0xEF)
                   || masked(b) != mh) {
            return 1;
        }
    }
    return 0;
}

typedef struct { int xing, info; } xstate;

static int check_xing_vbri(const buf *d, size_t pos, int side, xstate *st) {
    size_t a = pos + 4 + (size_t)side;
    if (a + 12 > d->n) return -1;
    const unsigned char *b = d->d + a;
    if (!memcmp(b, "Xing", 4) && (b[7] & 1)) { st->xing = 1; return 0; }
    if (!memcmp(b, "Info", 4)) st->info = 1;
    if (st->xing) return 0;
    if (a + 18 > d->n) return -1;
    if (side != 32) {
        if (pos + 54 > d->n) return -1;
        b = d->d + pos + 36;
    }
    if (!memcmp(b, "VBRI", 4)) st->xing = 1;
    return 0;
}

/* FrameAnalyzer::analyze: the frame map, and H (the samples of the Xing/Info/VBRI frame, never fed) */
#if defined(RBSP_WIN_DECODE)
/* mp3_probe's form: it also returns once `stop` frames are mapped (0: never; from the fourth
 * frame on nothing restarts, so the first is settled), *at the position the scan ended at */
static frame *analyze_to(const buf *d, size_t *count, size_t *H, size_t stop, size_t *at) {
#else
static frame *analyze(const buf *d, size_t *count, size_t *H) {
#endif
    size_t cap = 1024, nf = 0, samples = 0, pos = 0;
    frame *F = malloc(sizeof(frame) * cap);
    if (!F) return NULL;
    int first = 1, mism = 0, ref[4] = {0};
    xstate st = {0, 0};
    *H = 0;
    for (;;) {
#if defined(RBSP_WIN_DECODE)
        if (stop && nf >= stop) break;
#endif
        const unsigned char *b = at4(d, pos);
        if (!b) break;
        if (!memcmp(b, "ID3", 3)) {
            if (pos + 5 > d->n) break;
            int vv = d->d[pos + 3] * 10 + d->d[pos + 4];
            if (vv == 20 || vv == 21 || vv == 30 || vv == 31 || vv == 40) {
                if (pos + 10 > d->n) break;      /* "ID3V2 tag size error" */
                const unsigned char *s = d->d + pos + 6;
                size_t n = (size_t)(s[0] & 0x7F) << 21 | (size_t)(s[1] & 0x7F) << 14 | (size_t)(s[2] & 0x7F) << 7 | (s[3] & 0x7F);
                pos = pos + n + 10;
            } else {
                pos += 3;
            }
            continue;
        }
        if (!hdr_ok(b)) { pos++; continue; }
        int ver = (b[1] >> 3) & 1, layer = (b[1] >> 1) & 3, sri = (b[2] >> 2) & 3, chm = b[3] >> 6;
        size_t spf = ver ? 1152 : 576;
        if (first) {
            int r = check_continuous(d, pos, masked(b));
            if (r == 1) { pos++; continue; }
            if (r == 2) break;
            size_t size = fsize(b);
            int side = ver ? (chm == 3 ? 17 : 32) : (chm == 3 ? 9 : 17);
            if (check_xing_vbri(d, pos, side, &st) < 0) break;
            if (st.xing || st.info) *H = spf;
            if (nf == cap) { frame *g = realloc(F, sizeof(frame) * (cap *= 2)); if (!g) { free(F); return NULL; } F = g; }
            F[nf++] = (frame){pos, samples, size};
            samples += spf;
            pos += size;
            first = 0;
            ref[0] = ver; ref[1] = layer; ref[2] = sri; ref[3] = chm;
            continue;
        }
        size_t size = fsize(b);
        if (ver == ref[0] && layer == ref[1] && sri == ref[2] && chm == ref[3]) {
            if (nf == cap) { frame *g = realloc(F, sizeof(frame) * (cap *= 2)); if (!g) { free(F); return NULL; } F = g; }
            F[nf++] = (frame){pos, samples, size};
            samples += spf;
            pos += size;
            mism = 0;
            continue;
        }
        if (nf < 4) {                    /* restart one byte after the last mapped frame */
            pos = nf ? F[nf - 1].off + 1 : pos;
            nf = 0; samples = 0; mism = 0; first = 1;
            st.xing = st.info = 0;
            continue;
        }
        if (mism > 3) break;             /* the fifth mismatch in a row */
        mism++;
        pos++;
    }
    *count = nf;
#if defined(RBSP_WIN_DECODE)
    *at = pos;
#endif
    return F;
}
#if defined(RBSP_WIN_DECODE)
static frame *analyze(const buf *d, size_t *count, size_t *H) { size_t at; return analyze_to(d, count, H, 0, &at); }
#endif

/* the libmpg123 calls rekordbox makes */
typedef struct mpg123_handle_struct mpg123_handle;
static struct {
    int (*init)(void);
    mpg123_handle *(*new_)(const char *, int *);
    void (*delete_)(mpg123_handle *);
    int (*param)(mpg123_handle *, int, long, double);
    int (*format_none)(mpg123_handle *);
    int (*format)(mpg123_handle *, long, int, int);
    int (*open_feed)(mpg123_handle *);
    int (*feed)(mpg123_handle *, const unsigned char *, size_t);
#if defined(RBSP_WIN_DECODE)
    int (*decode_frame)(mpg123_handle *, int64_t *, unsigned char **, size_t *);
#else
    int (*decode_frame)(mpg123_handle *, off_t *, unsigned char **, size_t *);
#endif
    int ready;
} m;

#if defined(RBSP_WIN_DECODE)
/* Only one call takes libmpg123's off_t: mpg123_decode_frame's *num (never read). Its width in
 * Pioneer's libmpg123.dll isn't known (helper.md Q2): a Windows build exports mpg123_decode_frame
 * with a 32-bit long; a large-file build exports mpg123_decode_frame_64 (the Mac's dylib has both);
 * 1.32+ also mpg123_decode_frame64 with an int64_t. A zeroed int64_t is wide enough for any of
 * them (little-endian: a 32-bit write fills its low half). The VM's export list settles which (Q2). */
static int load_mpg123(const char *lib) {
    if (m.ready) return 0;
    const char *err = NULL;
    plat_lib *h = plat_lib_open(lib, &err);   /* the full path: LoadLibraryExW, DLL_LOAD_DIR | SYSTEM32 */
    if (!h) return 1;
    m.init = (int (*)(void))plat_lib_sym(h, "mpg123_init");
    m.new_ = (mpg123_handle * (*)(const char *, int *)) plat_lib_sym(h, "mpg123_new");
    m.delete_ = (void (*)(mpg123_handle *))plat_lib_sym(h, "mpg123_delete");
    m.param = (int (*)(mpg123_handle *, int, long, double))plat_lib_sym(h, "mpg123_param");
    m.format_none = (int (*)(mpg123_handle *))plat_lib_sym(h, "mpg123_format_none");
    m.format = (int (*)(mpg123_handle *, long, int, int))plat_lib_sym(h, "mpg123_format");
    m.open_feed = (int (*)(mpg123_handle *))plat_lib_sym(h, "mpg123_open_feed");
    m.feed = (int (*)(mpg123_handle *, const unsigned char *, size_t))plat_lib_sym(h, "mpg123_feed");
    static const char *const df[] = {"mpg123_decode_frame", "mpg123_decode_frame_64", "mpg123_decode_frame64"};
    for (size_t i = 0; i < sizeof df / sizeof *df && !m.decode_frame; i++)
        m.decode_frame = (int (*)(mpg123_handle *, int64_t *, unsigned char **, size_t *))plat_lib_sym(h, df[i]);
#else
static int load_mpg123(const char *lib) {
    if (m.ready) return 0;
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) return 1;
    m.init = (int (*)(void))dlsym(h, "mpg123_init");
    m.new_ = (mpg123_handle * (*)(const char *, int *)) dlsym(h, "mpg123_new");
    m.delete_ = (void (*)(mpg123_handle *))dlsym(h, "mpg123_delete");
    m.param = (int (*)(mpg123_handle *, int, long, double))dlsym(h, "mpg123_param");
    m.format_none = (int (*)(mpg123_handle *))dlsym(h, "mpg123_format_none");
    m.format = (int (*)(mpg123_handle *, long, int, int))dlsym(h, "mpg123_format");
    m.open_feed = (int (*)(mpg123_handle *))dlsym(h, "mpg123_open_feed");
    m.feed = (int (*)(mpg123_handle *, const unsigned char *, size_t))dlsym(h, "mpg123_feed");
    m.decode_frame = (int (*)(mpg123_handle *, off_t *, unsigned char **, size_t *))dlsym(h, "mpg123_decode_frame");
#endif
    if (!m.init || !m.new_ || !m.delete_ || !m.param || !m.format_none || !m.format || !m.open_feed || !m.feed || !m.decode_frame)
        return 1;
    m.init();
    m.ready = 1;
    return 0;
}

#define NEED_MORE (-10)

static int feed_decode(mpg123_handle *h, const buf *d, const frame *f, unsigned char **pcm, size_t *nb) {
    size_t len = f->size < 0x2000 ? f->size : 0x2000;
    if (f->off >= d->n) len = 0;
    else if (len > d->n - f->off) len = d->n - f->off;
    m.feed(h, d->d + f->off, len);
#if defined(RBSP_WIN_DECODE)
    int64_t num = 0;
#else
    off_t num = 0;
#endif
    *pcm = NULL;
    *nb = 0;
    return m.decode_frame(h, &num, pcm, nb);
}

#if defined(RBSP_WIN_DECODE)
static int read_file(plat_file f, buf *b) {   /* the whole file, at most 2 GB, as below */
    plat_stat st;
    if (plat_file_stat(f, &st) || st.size <= 0 || st.size > (2LL << 30)) return 1;
    unsigned char *d = big_alloc((size_t)st.size);
    /* The header probe uses positional reads, leaving the handle at the start. */
    if (!d || plat_read_all(f, d, (size_t)st.size)) { big_free(d); return 1; }
    b->d = d;
    b->n = (size_t)st.size;
    return 0;
}
#else
static int read_file(const char *path, buf *b) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0 || st.st_size > (2LL << 30)) { if (fd >= 0) close(fd); return 1; }
    unsigned char *d = big_alloc((size_t)st.st_size);
    size_t got = 0;
    while (d && got < (size_t)st.st_size) {
        ssize_t r = read(fd, d + got, (size_t)st.st_size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!d || got != (size_t)st.st_size) { big_free(d); return 1; }
    b->d = d;
    b->n = got;
    return 0;
}
#endif

#ifdef RBSP_WIN_DECODE
int rb_mp3_file(plat_file file, const char *libmpg123, float **out, size_t *n_out, int *rate_out, char *why, size_t whyn) {
#else
int rb_mp3(const char *path, const char *libmpg123, float **out, size_t *n_out, char *why, size_t whyn) {
#endif
    if (load_mpg123(libmpg123)) { snprintf(why, whyn, "rekordbox's MP3 decoder (libmpg123) can't be loaded"); return 1; }
    buf d;
#ifdef RBSP_WIN_DECODE
    if (read_file(file, &d)) { snprintf(why, whyn, "the file can't be read"); return 1; }
#else
    if (read_file(path, &d)) { snprintf(why, whyn, "the file can't be read"); return 1; }
#endif
    size_t nf, H;
    frame *F = analyze(&d, &nf, &H);
    if (!F || !nf) {
        free(F); big_free((void *)d.d);
        snprintf(why, whyn, "MP3 the frame scanner rejects: rekordbox finds no frame map");
        return 1;
    }
    const unsigned char *b = d.d + F[0].off;
    int ver = (b[1] >> 3) & 1;
    int rate = (ver ? M1_SR : M2_SR)[(b[2] >> 2) & 3];
    int ch = (b[3] >> 6) == 3 ? 1 : 2;
    size_t spf = ver ? 1152 : 576;
    int err = 0;
    mpg123_handle *h = m.new_(NULL, &err);
    if (!h) { free(F); big_free((void *)d.d); snprintf(why, whyn, "libmpg123 can't start"); return 1; }
    m.param(h, 0, 0, 0.0);               /* VERBOSE 0 */
    m.param(h, 1, 0x300, 0.0);           /* FLAGS: SEEKBUFFER | FUZZY, no GAPLESS */
    m.param(h, 15, -1, 0.0);             /* INDEX_SIZE -1 */
    m.format_none(h);
    m.format(h, rate, ch, 0xd0);         /* signed 16-bit */
    unsigned char *pcm;
    size_t nb;
    m.open_feed(h);                      /* decoderOpen: probe until the first non-NEED_MORE */
    for (size_t i = 0; i < nf; i++)
        if (F[i].start >= H && feed_decode(h, &d, &F[i], &pcm, &nb) != NEED_MORE) break;
    m.open_feed(h);                      /* decode(0, 0, reopen = true) */
    size_t n = nf * spf, fc = 0;
    int16_t *s16 = big_calloc(n * (size_t)ch, sizeof(int16_t));
    int rc = s16 ? 0 : 1;
    if (rc) snprintf(why, whyn, "out of memory");
    for (size_t i = 0; i < nf && !rc; i++) {
        if (F[i].start < H) continue;    /* the Xing/Info/VBRI frame: never fed, spf zero samples */
        int r = feed_decode(h, &d, &F[i], &pcm, &nb);
        if (r == NEED_MORE && i + 1 < nf) { fc += F[i + 1].start - F[i].start; continue; }
        if (nb != spf * (size_t)ch * 2) {
            /* rekordbox would fill zeros to the end of its current read and seek again: that
             * depends on its read sizes, not modelled */
            snprintf(why, whyn, "MP3 the frame scanner rejects: frame %zu decodes to %zu bytes", i, nb);
            rc = 1;
            break;
        }
        size_t p = F[i].start - fc;
        if (p + spf > n) { snprintf(why, whyn, "MP3 frame bookkeeping out of range"); rc = 1; break; }
        memcpy(s16 + p * (size_t)ch, pcm, nb);
    }
    m.delete_(h);
    free(F);
    big_free((void *)d.d);
    if (rc) { big_free(s16); return 1; }
    float *f = big_alloc(sizeof(float) * 2 * n);
    if (!f) { big_free(s16); snprintf(why, whyn, "out of memory"); return 1; }
    const float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < n; i++) {
        f[i] = s16[i * (size_t)ch] * k;
        f[n + i] = s16[i * (size_t)ch + (size_t)(ch - 1)] * k;
    }
    big_free(s16);
    *out = f;
    *n_out = n;
#ifdef RBSP_WIN_DECODE
    *rate_out = rate;
#endif
    return 0;
}

#if defined(RBSP_WIN_DECODE)
/* mp3_probe (helper.md T8): the header only. The first frame is the one analyze maps first on the
 * whole file, found by running analyze itself on windows of the file; the length is rb_mp3's,
 * nf * spf (the Xing/Info/VBRI frame counts as one, its spf samples zeros), with nf counted or
 * estimated as below. */
#ifndef MP3_PROBE_WIN
#define MP3_PROBE_WIN (64 << 10)        /* win-mp3.sh also runs the probe with 8 KB: more window ends */
#endif
enum {
    P_WIN = MP3_PROBE_WIN,              /* the first window (at least 2 x P_MARGIN) */
    P_WIN_MAX = 16 << 20,               /* the largest (one frame mapped, then no header that far) */
    P_MARGIN = 4096,                    /* the most analyze reads past its position: 2 frames of 2,017 + 4 */
    P_SAMPLES = 8,                      /* reads of frame headers spread over the file, the last at its end */
    P_CHUNK = 8192,                     /* each of them: at least three frames */
    P_WALK = 256 << 10,                 /* the walk's reads */
};

typedef struct {
    int64_t f0;                         /* the first frame's offset */
    unsigned char h[4];                 /* its header */
    int br[4], nbr;                     /* the bitrate indices of the first frames (up to 4) */
    int whole;                          /* nf is the whole file's count (the scan reached its end) */
    int info;                           /* an Info tag declares a constant bitrate */
    uint32_t tag_count, tag_bytes, lame_bytes;
    size_t nf;
    int64_t next;                       /* else: the scan's position after the fourth frame */
} first_map;

static uint32_t be32(const unsigned char *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static uint32_t le32(const unsigned char *b) { return (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0]; }

/* Read the metadata from the first window, which the scanner already has. */
static void read_frame_tag(first_map *r, const unsigned char *h, size_t n) {
    int ver = (h[1] >> 3) & 1, mono = h[3] >> 6 == 3;
    size_t at = 4 + (ver ? (mono ? 17 : 32) : (mono ? 9 : 17));
    if (n < at + 12) return;
    const unsigned char *x = h + at;
    r->info = !memcmp(x, "Info", 4);
    if (r->info || !memcmp(x, "Xing", 4)) {
        unsigned flags = x[7];
        if (!(flags & 1)) return;
        r->tag_count = be32(x + 8);
        at += 12;
        if (flags & 2) {
            if (at + 4 > n) return;
            r->tag_bytes = be32(h + at);
            at += 4;
        }
        if (flags & 4) at += 100;
        if (flags & 8) at += 4;
        /* LAME's encoder extension records the music length independently of Xing. */
        if (at + 36 <= n && !memcmp(h + at, "LAME", 4))
            r->lame_bytes = be32(h + at + 28);
    } else if (n >= 54 && !memcmp(h + 36, "VBRI", 4)) {
        r->tag_bytes = be32(h + 46);
        r->tag_count = be32(h + 50);
    }
}

/* analyze on windows of the file. Each window starts where the whole-file scan is in its first
 * state (nothing mapped: the file's start, past an ID3v2 tag it skipped, the first of the frames
 * it mapped), so the window's scan is the file's until it needs a byte past the window; every
 * such need ends the scan (at4, the ID3 header, check_continuous's end, check_xing_vbri), within
 * P_MARGIN of the window's end, and from 4 frames on nothing restarts. The scan ending further
 * from the window's end is the file's own end of the scan (a "TAG" before any frame).
 * 0 found, 1 no frame map, 2 a read error, 3 out of memory, 4 past P_WIN_MAX. */
static int find_first(plat_file f, int64_t size, first_map *r) {
    size_t W = P_WIN, cap = 0;
    int64_t base = 0;
    unsigned char *w = NULL;
    int rc;
    for (;;) {
        if (base >= size) { rc = 1; break; }   /* an ID3 size past the end */
        size_t wn = size - base < (int64_t)W ? (size_t)(size - base) : W;
        int eof = base + (int64_t)wn == size;
        if (cap < wn) {
            unsigned char *g = realloc(w, wn);
            if (!g) { rc = 3; break; }
            w = g;
            cap = wn;
        }
        if (pcm_pread_all(f, w, wn, base)) { rc = 2; break; }
        buf d = {w, wn};
        size_t nf, H, at;
        frame *F = analyze_to(&d, &nf, &H, eof ? 0 : 4, &at);   /* to the end when the window reaches it */
        if (!F) { rc = 3; break; }
        if (eof || nf >= 4 || at + P_MARGIN <= wn) {
            if (!nf) { free(F); rc = 1; break; }
            r->f0 = base + (int64_t)F[0].off;
            memcpy(r->h, w + F[0].off, 4);
            r->nbr = nf < 4 ? (int)nf : 4;
            for (int k = 0; k < r->nbr; k++) r->br[k] = w[F[k].off + 2] >> 4;
            r->whole = eof || nf < 4;
            if (H && !r->whole) {
                size_t left = wn - F[0].off;
                read_frame_tag(r, w + F[0].off, F[0].size < left ? F[0].size : left);
            }
            r->nf = nf;
            r->next = base + (int64_t)at;
            free(F);
            rc = 0;
            break;
        }
        /* the window's end stopped it: again from the scan's last first state */
        size_t from = nf ? F[0].off : at;
        free(F);
        if (from < W / 2) {
            if (W >= P_WIN_MAX) { rc = 4; break; }
            W *= 2;
        }
        base += (int64_t)from;
    }
    free(w);
    return rc;
}

/* the end of the stream: before an ID3v1 tag (and its "TAG+" extension), APEv2 and Lyrics3v2 tags
 * at the end */
static int64_t audio_end(plat_file f, int64_t size) {
    unsigned char t[32];
    int64_t end = size;
    if (end >= 128 && !pcm_pread_all(f, t, 3, end - 128) && !memcmp(t, "TAG", 3)) {
        end -= 128;
        if (end >= 227 && !pcm_pread_all(f, t, 4, end - 227) && !memcmp(t, "TAG+", 4)) end -= 227;
    }
    for (int k = 0; k < 4; k++) {
        if (end >= 32 && !pcm_pread_all(f, t, 32, end - 32) && !memcmp(t, "APETAGEX", 8)) {
            int64_t n = (int64_t)le32(t + 12) + ((t[23] & 0x80) ? 32 : 0);   /* footer and items, + header */
            if (le32(t + 12) >= 32 && n <= end) { end -= n; continue; }
        }
        if (end >= 15 && !pcm_pread_all(f, t, 15, end - 15) && !memcmp(t + 6, "LYRICS200", 9)) {
            int64_t n = 0;
            int k6 = 0;
            for (; k6 < 6 && t[k6] >= '0' && t[k6] <= '9'; k6++) n = n * 10 + (t[k6] - '0');
            if (k6 == 6 && n + 15 <= end) { end -= n + 15; continue; }
        }
        break;
    }
    return end;
}

/* The count excludes the metadata frame, which the decoder maps too. Check the tag's byte
 * counts and whether those bytes can hold the claimed number of frames at the allowed bitrates. */
static size_t tag_frames(const first_map *r, int64_t end) {
    if (!r->tag_count) return 0;
    int64_t have = end - r->f0, count = (int64_t)r->tag_count + 1;
    if ((r->tag_bytes && r->tag_bytes != have) || (r->lame_bytes && r->lame_bytes != have)) return 0;
    unsigned char lo[4], hi[4];
    memcpy(lo, r->h, 4);
    memcpy(hi, r->h, 4);
    lo[2] = (unsigned char)((lo[2] & 0x0C) | 0x10);
    hi[2] = (unsigned char)((hi[2] & 0x0C) | 0xE2);
    if (r->info && r->br[1] == r->br[2] && r->br[1] == r->br[3]) {
        /* The Info frame can have its own bitrate. The rest may differ by one padding byte. */
        lo[2] = (unsigned char)((lo[2] & 0x0C) | r->br[1] << 4);
        hi[2] = lo[2] | 2;
        have -= (int64_t)fsize(r->h);
        if (have < (count - 1) * (int64_t)fsize(lo) || have > (count - 1) * (int64_t)fsize(hi)) return 0;
    } else if ((double)have < (double)count * (double)fsize(lo) * 0.98
               || (double)have > (double)count * (double)fsize(hi) * 1.02) {
        return 0;
    }
    return (size_t)count;
}

/* the first three frames in a row in c, as the scanner accepts a first frame (hdr_ok, the masked
 * header mh, check_continuous): their position, else (size_t)-1 */
static size_t chain_at(const unsigned char *c, size_t n, uint32_t mh) {
    buf d = {c, n};
    for (size_t p = 0; p + 4 <= n; p++)
        if (hdr_ok(c + p) && masked(c + p) == mh && check_continuous(&d, p, mh) == 0) return p;
    return (size_t)-1;
}

/* CBR: every sampled frame (the first four, three in a row at 7 offsets, and every frame of the
 * last chunk, which must run to the stream's end) at the first frame's bitrate. nf from the last
 * frame's offset and the mean frame size (spf / 8 x bitrate / rate, padding included); 0 when the
 * headers say otherwise or can't be read. */
static size_t cbr_frames(plat_file f, const first_map *r, int64_t end) {
    int64_t span = end - (int64_t)P_CHUNK - r->f0;
    if (span < (int64_t)P_SAMPLES * P_CHUNK) return 0;
    for (int k = 0; k < r->nbr; k++)
        if (r->br[k] != r->br[0]) return 0;
    unsigned char *c = malloc(P_CHUNK);
    if (!c) return 0;
    uint32_t mh = masked(r->h);
    size_t nf = 0;
    int ok = 1;
    for (int k = 1; k <= P_SAMPLES && ok; k++) {
        int64_t off = r->f0 + span * k / P_SAMPLES;          /* the last: end - P_CHUNK */
        if (pcm_pread_all(f, c, P_CHUNK, off)) { ok = 0; break; }
        size_t p = chain_at(c, P_CHUNK, mh);
        if (p == (size_t)-1) { ok = 0; break; }
        if (k < P_SAMPLES) {
            for (int j = 0; j < 3; j++, p += fsize(c + p))
                if (c[p + 2] >> 4 != r->br[0]) ok = 0;
            continue;
        }
        size_t last = p;
        for (; p + 4 <= P_CHUNK; p += fsize(c + p)) {        /* to the end: no junk after the frames */
            if (!hdr_ok(c + p) || masked(c + p) != mh || c[p + 2] >> 4 != r->br[0]) { ok = 0; break; }
            last = p;
        }
        if (!ok) break;
        int ver = (r->h[1] >> 3) & 1, layer = (r->h[1] >> 1) & 3;
        int br = ver ? M1_BR[layer ^ 3][r->br[0]] : M2_BR[r->br[0]];
        int sr = (ver ? M1_SR : M2_SR)[(r->h[2] >> 2) & 3];
        double mean = (double)((ver ? 1152 : 576) >> 3) * br * 1000.0 / sr;
        nf = (size_t)((double)(off + (int64_t)last - r->f0) / mean + 0.5) + 1;
    }
    free(c);
    return ok ? nf : 0;
}

/* analyze's loop after its fourth frame (nothing restarts from there), on the rest of the file
 * read in pieces: the whole-file count. 0, or 1 on a read error or no memory. */
static int walk(plat_file f, int64_t size, const first_map *r, size_t *count) {
    unsigned char *w = malloc(P_WALK);
    if (!w) return 1;
    const unsigned char *h = r->h;
    int ver = (h[1] >> 3) & 1, layer = (h[1] >> 1) & 3, sri = (h[2] >> 2) & 3, chm = h[3] >> 6;
    int64_t pos = r->next, wb = 0, we = 0;                    /* w holds [wb, we) */
    size_t nf = r->nf;
    int mism = 0, rc = 0;
    for (;;) {
        if (pos + 4 > size) break;
        if (pos < wb || (pos + 10 > we && we < size)) {
            wb = pos;
            we = size - pos < P_WALK ? size : pos + P_WALK;
            if (pcm_pread_all(f, w, (size_t)(we - wb), wb)) { rc = 1; break; }
        }
        const unsigned char *b = w + (pos - wb);
        if (!memcmp(b, "ID3", 3)) {
            if (pos + 5 > size) break;
            int vv = b[3] * 10 + b[4];
            if (vv == 20 || vv == 21 || vv == 30 || vv == 31 || vv == 40) {
                if (pos + 10 > size) break;
                const unsigned char *s = b + 6;
                pos += (int64_t)((size_t)(s[0] & 0x7F) << 21 | (size_t)(s[1] & 0x7F) << 14 | (size_t)(s[2] & 0x7F) << 7 | (s[3] & 0x7F)) + 10;
            } else {
                pos += 3;
            }
            continue;
        }
        if (!hdr_ok(b)) { pos++; continue; }
        if (((b[1] >> 3) & 1) == ver && ((b[1] >> 1) & 3) == layer && ((b[2] >> 2) & 3) == sri && b[3] >> 6 == chm) {
            nf++;
            pos += (int64_t)fsize(b);
            mism = 0;
            continue;
        }
        if (mism > 3) break;
        mism++;
        pos++;
    }
    free(w);
    *count = nf;
    return rc;
}

/* Use a plausible tag count or sample untagged CBR headers. Walk the remaining frames when
 * the tag is inconsistent or there is no usable estimate. */
int mp3_probe(plat_file f, int64_t size, pcm_info *p, char *why, size_t whyn) {
    memset(p, 0, sizeof *p);
    if (size <= 0 || size > (2LL << 30)) { snprintf(why, whyn, "the file can't be read"); return 1; }   /* rb_mp3's read_file */
    first_map r = {0};
    switch (find_first(f, size, &r)) {
    case 0: break;
    case 1: snprintf(why, whyn, "MP3 the frame scanner rejects: rekordbox finds no frame map"); return 1;
    case 2: snprintf(why, whyn, "the file can't be read"); return 1;
    case 3: snprintf(why, whyn, "out of memory"); return 1;
    default: {
        /* no fourth frame within P_WIN_MAX of the first ones (a long stretch of no frames): the
         * whole file through rekordbox's scan, as rb_mp3 runs it, so the probe never refuses a
         * file the run would prepare; it reads the whole file, for such a file only */
        buf d = {big_alloc((size_t)size), (size_t)size};
        if (!d.d) { snprintf(why, whyn, "out of memory"); return 1; }
        if (pcm_pread_all(f, (void *)d.d, d.n, 0)) { big_free((void *)d.d); snprintf(why, whyn, "the file can't be read"); return 1; }
        size_t nf0, H;
        frame *F = analyze(&d, &nf0, &H);
        if (!F || !nf0) {
            free(F);
            big_free((void *)d.d);
            if (!F) { snprintf(why, whyn, "out of memory"); return 1; }
            snprintf(why, whyn, "MP3 the frame scanner rejects: rekordbox finds no frame map");
            return 1;
        }
        r.f0 = (int64_t)F[0].off;
        memcpy(r.h, d.d + F[0].off, 4);
        r.whole = 1;
        r.nf = nf0;
        free(F);
        big_free((void *)d.d);
        break;
    }
    }
    const unsigned char *b = r.h;
    int ver = (b[1] >> 3) & 1;
    size_t nf = r.nf;
    if (!r.whole) {
        int64_t end = audio_end(f, size);
        if (end < r.next) end = size;
        if ((nf = tag_frames(&r, end))) {
            /* The tag's frame count includes the same samples as the frame map. */
        } else if (!r.tag_count && (nf = cbr_frames(f, &r, end))) {
            p->approx = 1;
        } else if (walk(f, size, &r, &nf)) {
            snprintf(why, whyn, "the file can't be read");
            return 1;
        }
    }
    p->kind = PCM_MP3;
    p->mp3_layer = 4 - ((b[1] >> 1) & 3);
    p->rate = (ver ? M1_SR : M2_SR)[(b[2] >> 2) & 3];   /* as rb_mp3 takes them */
    p->ch = (b[3] >> 6) == 3 ? 1 : 2;
    p->frames = (uint64_t)nf * (ver ? 1152 : 576);
    p->seconds = (double)p->frames / p->rate;
    return 0;
}
#endif
