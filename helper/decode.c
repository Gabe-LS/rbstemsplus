/* A file as rekordbox's separator reads it, before chunking: which reader rekordbox uses for the
 * file type, the length it gives, the channels it keeps, its resampler. One file can come out
 * more than one way (AAC): each way is a track, and each is prepared. */
#if defined(_WIN32) || defined(RBSP_WIN_DECODE)
/* Windows (and the Mac tests that compare it with AudioToolbox): the helper's own decoders */
#include "pcm.h"
#include "plat_helper.h"                   /* plat_track_open */
#else
#include <AudioToolbox/AudioToolbox.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rbsp.h"
#include "big.h"

void tracks_free(tracks *ts) {
    for (int i = 0; i < ts->count; i++)
        if (ts->t[i].owner) { big_free(ts->t[i].s); big_free(ts->t[i].p16); }
    ts->count = 0;
}

static void lower_ext(const char *path, char *ext, size_t n) {
    const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    ext[0] = 0;
    if (!dot || (slash && dot < slash)) return;
    size_t i = 0;
    for (; dot[i] && i + 1 < n; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    ext[i] = 0;
}

#ifdef RBSP_WIN_DECODE
/* the formats as AudioToolbox names them on the Mac, for the code below that is the same on both */
#define kAudioFormatFLAC PCM_FLAC
#define kAudioFormatAppleLossless PCM_ALAC
typedef struct { pcm_kind format; int rate, ch; double seconds; pcm_info pcm; } probe_info;

/* pcm.h's probe, by content (.mp3: rekordbox's MP3 frame scanner); why: what it refuses */
static int probe(plat_file f, const char *ext, probe_info *p, char *why, size_t whyn) {
    pcm_info *pi = &p->pcm;
    if (pcm_probe(f, ext, pi, why, whyn)) return 1;
    if (pi->rate < 1 || pi->ch < 1) { snprintf(why, whyn, "no audio stream RB Stems Plus can read"); return 1; }
    p->format = pi->kind;
    p->rate = pi->rate;
    p->ch = pi->ch;
    p->seconds = pi->seconds;
    return 0;
}

static int is_aac(pcm_kind f) { return f == PCM_AAC; }
#else
typedef struct { UInt32 format; int rate, ch; double seconds; } probe_info;

static int probe(const char *path, probe_info *p) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    AudioFileID af;
    OSStatus e = url ? AudioFileOpenURL(url, kAudioFileReadPermission, 0, &af) : 1;
    if (url) CFRelease(url);
    if (e) return 1;
    AudioStreamBasicDescription d;
    UInt32 sz = sizeof d;
    e = AudioFileGetProperty(af, kAudioFilePropertyDataFormat, &sz, &d);
    Float64 sec = 0;
    UInt32 ssz = sizeof sec;
    if (AudioFileGetProperty(af, kAudioFilePropertyEstimatedDuration, &ssz, &sec) || !(sec > 0)) sec = 0;
    int empty = 0;
    if (!e && sec == 0) {
        AudioFileTypeID type;
        UInt32 tsz = sizeof type;
        if (!AudioFileGetProperty(af, kAudioFilePropertyFileFormat, &tsz, &type) &&
            (type == kAudioFileWAVEType || type == kAudioFileRF64Type ||
             type == kAudioFileAIFFType || type == kAudioFileAIFCType)) {
            UInt64 packets;
            UInt32 psz = sizeof packets;
            empty = !AudioFileGetProperty(af, kAudioFilePropertyAudioDataPacketCount, &psz, &packets) && packets == 0;
        }
    }
    AudioFileClose(af);
    if (e || empty || d.mSampleRate < 1 || !d.mChannelsPerFrame) return 1;
    p->seconds = sec;
    p->format = d.mFormatID;
    p->rate = (int)d.mSampleRate;
    p->ch = (int)d.mChannelsPerFrame;
    return 0;
}

static int is_aac(UInt32 f) {
    return f == kAudioFormatMPEG4AAC || f == kAudioFormatMPEG4AAC_HE || f == kAudioFormatMPEG4AAC_HE_V2
           || f == kAudioFormatMPEG4AAC_LD || f == kAudioFormatMPEG4AAC_ELD;
}

/* FLAC, WAV, AIFF, ALAC: decoded to 32-bit integers (float files: to float32), then scaled as
 * rekordbox's float samples are (x / 2^31: exact for 8, 16 and 24-bit sources); more than two
 * channels: the first two (rekordbox drops the rest, no fold-down: measured with 5.1 WAV and
 * FLAC); mono: both sides */
static int decode_pcm(const char *path, float **out, size_t *n_out, int *rate, int *ch_out, char *why, size_t whyn) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    ExtAudioFileRef f;
    OSStatus e = url ? ExtAudioFileOpenURL(url, &f) : 1;
    if (url) CFRelease(url);
    if (e) { snprintf(why, whyn, "macOS can't open it as audio"); return 1; }
    AudioStreamBasicDescription fd;
    UInt32 sz = sizeof fd;
    SInt64 frames = 0;
    int rc = 1;
    float *s = NULL;
    void *buf = NULL;
    if (ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &sz, &fd)) { snprintf(why, whyn, "no audio format"); goto done; }
    sz = sizeof frames;
    if (ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileLengthFrames, &sz, &frames) || frames < 0) { snprintf(why, whyn, "no length"); goto done; }
    UInt32 ch = fd.mChannelsPerFrame;
    int flt = fd.mFormatID == kAudioFormatLinearPCM && (fd.mFormatFlags & kAudioFormatFlagIsFloat);
    AudioStreamBasicDescription cf = {fd.mSampleRate, kAudioFormatLinearPCM,
                                      (flt ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) | kAudioFormatFlagIsPacked,
                                      4 * ch, 1, 4 * ch, ch, 32, 0};
    if (!ch || ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof cf, &cf)) {
        snprintf(why, whyn, "macOS can't decode it");
        goto done;
    }
    size_t n = (size_t)frames, got = 0;
    s = big_alloc(sizeof(float) * 2 * (n ? n : 1));
    enum { BLOCK = 32768 };
    buf = malloc((size_t)BLOCK * 4 * ch);
    if (!s || !buf) { snprintf(why, whyn, "out of memory"); goto done; }
    const float k = 1.0f / 2147483648.0f;
    for (;;) {
        UInt32 want = BLOCK;
        AudioBufferList bl = {1, {{ch, BLOCK * 4 * ch, buf}}};
        if (ExtAudioFileRead(f, &want, &bl)) { snprintf(why, whyn, "decoding stopped with an error"); goto done; }
        if (!want) break;
        if (got + want > n) { snprintf(why, whyn, "longer than its stated length"); goto done; }
        for (UInt32 i = 0; i < want; i++) {
            size_t c1 = ch > 1 ? 1 : 0;
            if (flt) {
                const float *b = (const float *)buf + (size_t)i * ch;
                s[got + i] = b[0];
                s[n + got + i] = b[c1];
            } else {
                const int32_t *b = (const int32_t *)buf + (size_t)i * ch;
                s[got + i] = (float)b[0] * k;
                s[n + got + i] = (float)b[c1] * k;
            }
        }
        got += want;
    }
    if (got != n) {                      /* a decode that stops early is a failure, not a short track */
        snprintf(why, whyn, "decoded %zu samples, the file says %zu", got, n);
        goto done;
    }
    *out = s;
    s = NULL;
    *n_out = n;
    *rate = (int)fd.mSampleRate;
    *ch_out = (int)ch;
    rc = 0;
done:
    big_free(s);
    free(buf);
    ExtAudioFileDispose(f);
    return rc;
}
#endif

static int add_track(tracks *ts, float *s, size_t n) {
    if (ts->count == MAX_TRACKS) { big_free(s); return 1; }
    ts->t[ts->count] = (track){.s = s, .ch = 2, .owner = 1, .n = n};
    ts->count++;
    return 0;
}

#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
/* T10 (helper.md): keep Media Foundation's PCM from frame zero, up to the sample table's length. */
static int decode_aac(plat_file f, const char *ext, const pcm_info *p, tracks *ts, int *rate, char *why, size_t whyn) {
    aac_outputs o;
    if (aac_decode_all(f, ext, p->mf_stream, &o, why, whyn)) return 1;
    uint64_t frames = p->frames;
    *rate = o.rate;
    if (o.rate != p->rate) {
        snprintf(why, whyn, "Windows' AAC decoder gives %d Hz samples, the probe says %d Hz", o.rate, p->rate);
        aac_outputs_free(&o);
        return 1;
    }
    if (o.ch > 2) {                      /* the Mac's rule (refused there, from the header) */
        snprintf(why, whyn, "AAC with %d channels: rekordbox loads it but doesn't separate it", o.ch);
        aac_outputs_free(&o);
        return 1;
    }
    size_t decoded = o.frames[0];
    size_t n = frames < decoded ? (size_t)frames : decoded;
    ts->t[ts->count++] = (track){.p16 = o.pcm[0], .ch = o.ch, .owner = 1, .n = n,
                                .nominal_n = decoded < frames ? (size_t)frames : 0};
    ts->aac_length = decoded < frames ? AAC_LENGTH_SHORT : AAC_LENGTH_COMPLETE;
    if (decoded < frames)
        snprintf(ts->how, sizeof ts->how, "incomplete AAC decode: Media Foundation returned %zu frames, the sample table says %llu",
                 decoded, (unsigned long long)frames);
    else
        snprintf(ts->how, sizeof ts->how, "Media Foundation x1 output, length %zu", n);
    return 0;
}
#elif !defined(RBSP_WIN_DECODE)
/* AAC in an MP4: every decoder output (requirement 3) x the lengths rekordbox may give it */
static int decode_aac(const char *path, tracks *ts, int *rate, char *why, size_t whyn) {
    mp4_audio a;
    if (mp4_audio_track(path, &a) || !a.has_stts) { snprintf(why, whyn, "no sample table for the sound track (fragmented MP4?)"); return 1; }
    aac_outputs o;
    if (aac_decode_all(path, &o, why, whyn)) return 1;
    /* the rate and channels the decoder itself reports: the file's basic format, as rekordbox
     * uses it (HE-AAC: the AAC core at half the rate, mono for HE-AAC v2) */
    *rate = o.rate;
    uint64_t ts_ = a.timescale ? a.timescale : (uint64_t)o.rate;
    /* the sample table counts in the track's timescale (ffmpeg's HE-AAC: 44,100 for a 22,050 core) */
#define UNITS(x) ((ts_ != (uint64_t)o.rate) ? (uint64_t)(x) * (uint64_t)o.rate / ts_ : (uint64_t)(x))
    size_t n = (size_t)UNITS(a.stts_total);
    for (int v = 0; v < o.count; v++)
        if (o.frames[v] < n) {
            snprintf(why, whyn, "decoded %zu samples, the sample table says %zu", o.frames[v], n);
            aac_outputs_free(&o);
            return 1;
        }
    /* a short last packet: rekordbox keeps it (ffmpeg's own encoder, 1-1008 samples) or ends one
     * sample into it (ffmpeg's aac_at, always 64): what tells them apart isn't known, so the last
     * chunk is prepared both ways */
    uint64_t last = UNITS(a.last_delta);
    size_t lens[2] = {n, n - (size_t)last + 1};
    int nlens = (last > 1 && last < 1024) ? 2 : 1;
#undef UNITS
    if (o.count * nlens > MAX_TRACKS) {
        snprintf(why, whyn, "Apple's AAC decoder gave %d different outputs", o.count);
        aac_outputs_free(&o);
        return 1;
    }
    /* each output once, in 16 bits; its lengths share it */
    for (int v = 0; v < o.count; v++) {
        for (int l = 0; l < nlens; l++) ts->t[ts->count++] = (track){NULL, o.pcm[v], o.ch, l == 0, lens[l]};
        o.pcm[v] = NULL;                 /* the first of its tracks owns it now */
    }
    char alt[40] = "", scale[40] = "";
    if (nlens == 2) snprintf(alt, sizeof alt, " or %zu", lens[1]);
    if (ts_ != (uint64_t)o.rate) snprintf(scale, sizeof scale, " (timescale %llu)", (unsigned long long)ts_);
    snprintf(ts->how, sizeof ts->how, "AudioConverter x%d outputs, length %zu%s%s", o.count, n, alt, scale);
    o.count = 0;                         /* every output was handed over */
    return 0;
}
#endif

/* a 16-bit track as planar stereo float32 (big_alloc'd) */
static float *to_float(const track *t) {
    float *s = big_alloc(sizeof(float) * 2 * (t->n ? t->n : 1));
    if (!s) return NULL;
    const float k = 1.0f / 32768.0f;
    size_t ch = (size_t)t->ch, c1 = ch > 1 ? 1 : 0;
    for (size_t i = 0; i < t->n; i++) {
        s[i] = t->p16[i * ch] * k;
        s[t->n + i] = t->p16[i * ch + c1] * k;
    }
    return s;
}

#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
#ifdef RBSP_TEST_FORMAT_RATES
int rb_mp3_test_unproved;
#endif

enum win_resample_mode { RESAMPLE_REFUSE, RESAMPLE_CHECKED, RESAMPLE_CANDIDATE };

/* 22.05 kHz WAV (u8, s16, s24, s32, f32), AIFF (s8 to s32), FLAC (s8 to s32, .flac and .fla) and
 * ALAC (s16, s24), mono and stereo: 38/38 rows MATCH on 7.2.20, owner's PC, 2026-11-23
 * (.evidence/2026-11-23/lossless-22050). Set to 0 to refuse them again. */
#define RB_WIN_LOSSLESS_22050_PROVED 1

/* A cell's channel counts and sample encodings, as bit sets */
#define CH(n) (1 << (n))
enum {
    CH12 = CH(1) | CH(2),
    ENC_8 = 1 << 0, ENC_16 = 1 << 1, ENC_24 = 1 << 2, ENC_32 = 1 << 3,   /* integer, as stored */
    ENC_F32 = 1 << 4,                    /* WAV IEEE float 32 */
    ENC_MP3_L3 = 1 << 5,                 /* MPEG audio Layer III */
    ENC_AAC_LC = 1 << 6,                 /* one AAC-LC description, no signalled extension, its
                                          * header's rate and channels (pcm.h aac_lc_rate/ch) */
    ENC_16_24 = ENC_16 | ENC_24,
};

/* The cells proved against rekordbox for Windows' own model input (3/3 keys on the owner's PC):
 * a file at a rate other than 44.1 kHz is prepared only when its format, extension, rate,
 * channels and encoding are one cell's (decode.c's probe, before any decoding). Every rate here
 * also needs its pinned resampler table (resample_win.c) and a proved rekordbox build (Q15).
 * Sources: .evidence/2026-11-22/format-measurements (formats-final.md, rates-proof.md), the T11/T2
 * rows (pc-720-t2.md) and OI-90 (2026-11-23). EXTS: the extensions, each between '|'. */
static const struct {
    pcm_kind format;
    const char *exts;
    int rate, ch, enc, on;
} win_cells[] = {
    /* MP3, CBR and VBR: 22.05 and 48 kHz (03-mp3-48k, w15-mp3-22k, fmt-mp3-*), 16/24/32 kHz (rates-proof) */
    {PCM_MP3, "|.mp3|", 16000, CH12, ENC_MP3_L3, 1},
    {PCM_MP3, "|.mp3|", 22050, CH12, ENC_MP3_L3, 1},
    {PCM_MP3, "|.mp3|", 24000, CH12, ENC_MP3_L3, 1},
    {PCM_MP3, "|.mp3|", 32000, CH12, ENC_MP3_L3, 1},
    {PCM_MP3, "|.mp3|", 48000, CH12, ENC_MP3_L3, 1},
    /* AAC-LC in .m4a and .mp4 (06-aac-48k, fmt-aac-lc-*; 22.05 kHz stereo .mp4: OI-90). Mono at
     * 16, 22.05 and 24 kHz mismatches (OI-151-N4) */
    {PCM_AAC, "|.m4a|.mp4|", 16000, CH(2), ENC_AAC_LC, 1},
    {PCM_AAC, "|.m4a|.mp4|", 22050, CH(2), ENC_AAC_LC, 1},
    {PCM_AAC, "|.m4a|.mp4|", 24000, CH(2), ENC_AAC_LC, 1},
    {PCM_AAC, "|.m4a|.mp4|", 32000, CH12, ENC_AAC_LC, 1},
    {PCM_AAC, "|.m4a|.mp4|", 48000, CH12, ENC_AAC_LC, 1},
    /* WAV (13-wav-24-48k, 15-wav-f32-96k, w09-wav-5.1 at 48 kHz, fmt-wav-pcm-*) */
    {PCM_WAV, "|.wav|", 48000, CH12, ENC_16_24, 1},
    {PCM_WAV, "|.wav|", 48000, CH(6), ENC_16, 1},
    {PCM_WAV, "|.wav|", 88200, CH12, ENC_16_24, 1},
    {PCM_WAV, "|.wav|", 96000, CH12, ENC_16_24, 1},
    {PCM_WAV, "|.wav|", 96000, CH(2), ENC_F32, 1},
    {PCM_WAV, "|.wav|", 192000, CH12, ENC_16_24, 1},
    /* AIFF, big-endian samples (17-aiff-24-48k, 18-aiff-24-96k, fmt-aiff-*) */
    {PCM_AIFF, "|.aif|.aiff|", 48000, CH12, ENC_16_24, 1},
    {PCM_AIFF, "|.aif|.aiff|", 88200, CH12, ENC_16_24, 1},
    {PCM_AIFF, "|.aif|.aiff|", 96000, CH12, ENC_16_24, 1},
    {PCM_AIFF, "|.aif|.aiff|", 192000, CH12, ENC_16_24, 1},
    /* FLAC (10-flac-24-48k, w14-flac-mono-48k, w08-flac-5.1 at 48 kHz, 11-flac-24-96k, fmt-flac-*) */
    {PCM_FLAC, "|.flac|.fla|", 48000, CH12, ENC_16_24, 1},
    {PCM_FLAC, "|.flac|", 48000, CH(6), ENC_16, 1},
    {PCM_FLAC, "|.flac|.fla|", 88200, CH12, ENC_16_24, 1},
    {PCM_FLAC, "|.flac|.fla|", 96000, CH12, ENC_16_24, 1},
    {PCM_FLAC, "|.flac|.fla|", 192000, CH12, ENC_16_24, 1},
    /* ALAC in .m4a only (08-alac-24-96k, fmt-alac-*) */
    {PCM_ALAC, "|.m4a|", 48000, CH12, ENC_16_24, 1},
    {PCM_ALAC, "|.m4a|", 88200, CH12, ENC_16_24, 1},
    {PCM_ALAC, "|.m4a|", 96000, CH12, ENC_16_24, 1},
    {PCM_ALAC, "|.m4a|", 192000, CH12, ENC_16_24, 1},
    /* 22.05 kHz lossless (fmt-{wav,aiff,flac,alac}-22050-*, 2026-11-23) */
    {PCM_WAV, "|.wav|", 22050, CH12, ENC_8 | ENC_16 | ENC_24 | ENC_32 | ENC_F32, RB_WIN_LOSSLESS_22050_PROVED},
    {PCM_AIFF, "|.aif|.aiff|", 22050, CH12, ENC_8 | ENC_16 | ENC_24 | ENC_32, RB_WIN_LOSSLESS_22050_PROVED},
    {PCM_FLAC, "|.flac|.fla|", 22050, CH12, ENC_8 | ENC_16 | ENC_24 | ENC_32, RB_WIN_LOSSLESS_22050_PROVED},
    {PCM_ALAC, "|.m4a|", 22050, CH12, ENC_16_24, RB_WIN_LOSSLESS_22050_PROVED},
};

/* the probe's sample encoding as a cell names it; 0 for one no cell takes */
static int cell_encoding(const probe_info *p) {
    switch (p->format) {
    case PCM_MP3: return p->pcm.mp3_layer == 3 ? ENC_MP3_L3 : 0;
    case PCM_AAC: return p->rate == p->pcm.aac_lc_rate && p->ch == p->pcm.aac_lc_ch ? ENC_AAC_LC : 0;
    case PCM_AIFF:
        if (!p->pcm.big_endian) return 0;   /* AIFF-C 'sowt': no resampled row */
        break;
    case PCM_WAV:
        if (p->pcm.flt) return p->pcm.flt == 32 && p->pcm.bits == 32 ? ENC_F32 : 0;
        break;
    case PCM_FLAC:
    case PCM_ALAC: break;
    default: return 0;
    }
    switch (p->pcm.bits) {
    case 8: return ENC_8;
    case 16: return ENC_16;
    case 24: return ENC_24;
    case 32: return ENC_32;
    default: return 0;
    }
}

static int proved_cell(const probe_info *p, const char *ext) {
    char key[24];
    int enc = cell_encoding(p);
    if (!enc || p->ch < 1 || p->ch > 30 || strlen(ext) + 3 > sizeof key) return 0;
    snprintf(key, sizeof key, "|%s|", ext);
    for (size_t i = 0; i < sizeof win_cells / sizeof *win_cells; i++)
        if (win_cells[i].on && win_cells[i].format == p->format && win_cells[i].rate == p->rate &&
            (win_cells[i].ch & CH(p->ch)) && (win_cells[i].enc & enc) && strstr(win_cells[i].exts, key))
            return 1;
    return 0;
}

#ifdef RBSP_TEST_FORMAT_RATES
static int mp4_extension(const char *ext) {
    return !strcmp(ext, ".m4a") || !strcmp(ext, ".mp4");
}

/* research builds only (--test-unproved-rates): the unproved rows still measured */
static int research_candidate(const probe_info *p, const char *ext) {
    if (!rb_rates_test_unproved) return 0;
    if (is_aac(p->format))
        return mp4_extension(ext) && cell_encoding(p) == ENC_AAC_LC && rb_aac_candidate_rate(p->rate, p->ch);
    /* The kit proved ALAC in .m4a. Keep the .mp4 alias available for research. */
    return p->format == PCM_ALAC && p->ch <= 2 && !strcmp(ext, ".mp4") && (p->rate == 88200 || p->rate == 192000);
}
#endif

static enum win_resample_mode track_resample_mode(const probe_info *p, const char *ext) {
#ifdef RBSP_TEST_FORMAT_RATES
    if (research_candidate(p, ext)) return RESAMPLE_CANDIDATE;
#endif
    return proved_cell(p, ext) ? RESAMPLE_CHECKED : RESAMPLE_REFUSE;
}

/* a rate rekordbox for Windows resamples from: 0 when it is proved (Q12) on a proved rekordbox
 * build (Q15, --resample-proved), or 1 and why it isn't prepared */
static int win_rate_refused(int rate, int candidate, const char *when, char *why, size_t whyn) {
    if (!candidate && !rb_resample_proved(rate))
        snprintf(why, whyn, "%d Hz%s: RB Stems Plus doesn't prepare files rekordbox resamples from this rate on Windows yet", rate, when);
    else if (!rb_resample_build_proved)
        snprintf(why, whyn, "%d Hz%s: this rekordbox version's resampler isn't proved yet, so RB Stems Plus doesn't prepare files it resamples", rate, when);
    else if (!candidate && !rb_resample_table_ok(rate))  /* checked once; the converter uses the same table */
        snprintf(why, whyn, "%d Hz%s: the resampler's filter built on this PC isn't the proved one", rate, when);
    else return 0;
    return 1;
}

int rb_win_rate_refused(int rate, const char *when, char *why, size_t whyn) {
    return win_rate_refused(rate, 0, when, why, whyn);
}
#endif

/* the files load_tracks refuses before decoding, and why: 0, or 1 and why */
#ifdef RBSP_WIN_DECODE
static int track_extension(const char *path, char *ext, size_t extn, char *why, size_t whyn) {
#else
static int refused(const char *path, char *ext, size_t extn, probe_info *p, char *why, size_t whyn) {
#endif
    lower_ext(path, ext, extn);
    if (!strcmp(ext, ".aac")) { snprintf(why, whyn, "raw AAC (ADTS): rekordbox can't load it"); return 1; }
    static const char *const exts[] = {".mp3", ".m4a", ".mp4", ".wav", ".aif", ".aiff", ".flac"
#ifdef RBSP_WIN_DECODE
        , ".fla"
#endif
    };
    int known = 0;
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
        if (!strcmp(ext, exts[i])) known = 1;
    if (!known) { snprintf(why, whyn, "%s: not a file type rekordbox plays", ext[0] ? ext : "no extension"); return 1; }
#ifdef RBSP_WIN_DECODE
    return 0;
}

static int refused(plat_file f, const char *ext, probe_info *p, char *why, size_t whyn) {
    if (probe(f, ext, p, why, whyn)) return 1;
    /* rekordbox 7.2.20 differs on 20-bit ALAC (owner's PC, 2026-11-21, win-alac s20)
     * and 32-bit ALAC (2026-11-22, all four controls matched 0/3): skip with the generic reason
     * (owner D-1122-11: no rekordbox-specific text) */
    if (p->format == PCM_ALAC && (p->pcm.bits == 20 || p->pcm.bits == 32)) {
        snprintf(why, whyn, "%d-bit ALAC: RB Stems Plus doesn't prepare this rekordbox format on Windows yet", p->pcm.bits);
        return 1;
    }
#if !defined(_WIN32) && !defined(RBSP_TEST_MF_TRACK)
    /* The Mac decoder comparison has no Media Foundation. rate-host supplies a stub. */
    if (strcmp(ext, ".mp3") && is_aac(p->format)) { snprintf(why, whyn, "AAC: Media Foundation's decoder is Windows' only"); return 1; }
#endif
#else
    if (probe(path, p)) { snprintf(why, whyn, "no audio stream macOS can read"); return 1; }
#endif
    if (strcmp(ext, ".mp3") && is_aac(p->format)) {
        if (p->ch > 2) { snprintf(why, whyn, "AAC with %d channels: rekordbox loads it but doesn't separate it", p->ch); return 1; }
    } else if (strcmp(ext, ".mp3") && (p->rate == 11025 || p->rate == 352800 || p->rate == 384000)) {
        snprintf(why, whyn, "%d Hz: rekordbox can't load it", p->rate);
        return 1;
    }
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
    /* rekordbox for Windows resamples with other code than the Mac's (helper.md §2, T11):
     * resample_win.c reproduces it, and only the cells proved against rekordbox's own output are
     * prepared (Q12, win_cells). Everything is decided here, so the estimate and the run agree */
    if (p->rate != 44100) {
        enum win_resample_mode mode = track_resample_mode(p, ext);
        if (mode == RESAMPLE_REFUSE) {
            snprintf(why, whyn, "%d Hz: RB Stems Plus doesn't prepare files rekordbox resamples from this rate on Windows yet", p->rate);
            return 1;
        }
        if (win_rate_refused(p->rate, mode == RESAMPLE_CANDIDATE, "", why, whyn)) return 1;
    }
#endif
    return 0;
}

int probe_track(const char *path, double *seconds, int *aac, char *why, size_t whyn) {
    char ext[16];
    probe_info p;
#ifdef RBSP_WIN_DECODE
    if (track_extension(path, ext, sizeof ext, why, whyn)) return 1;
    plat_file f;
    if (plat_track_open(path, &f)) { snprintf(why, whyn, "the file can't be read"); return 1; }
    int rc = refused(f, ext, &p, why, whyn);
    plat_file_close(f);
    if (rc) return 1;
#else
    if (refused(path, ext, sizeof ext, &p, why, whyn)) return 1;
#endif
    *seconds = p.seconds;
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
    /* a counted length at a resampled rate: the converter's exact output frames, so the
     * estimate's chunk count is the run's (the estimate rounds seconds x 44100 back to frames) */
    if (p.rate != 44100 && p.pcm.frames && !p.pcm.approx) {     /* 64-bit builds: size_t holds it */
        size_t out = rb_resample_out_frames((size_t)p.pcm.frames, p.rate);
        if (out) *seconds = (double)out / 44100;
    }
#endif
    *aac = strcmp(ext, ".mp3") && is_aac(p.format);
    return 0;
}

#ifdef RBSP_TEST_TIMING
uint64_t t_resample;
#endif

#ifdef RBSP_WIN_DECODE
static int load_open_track(plat_file f, const char *ext, const char *rekordbox_app, tracks *ts, char *why, size_t whyn) {
    probe_info p;
    if (refused(f, ext, &p, why, whyn)) return 1;
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
    if (plat_finfo_file(f, &ts->source)) { snprintf(why, whyn, "the file's metadata can't be read"); return 1; }
#endif
#else
int load_tracks(const char *path, const char *rekordbox_app, tracks *ts, char *why, size_t whyn) {
    memset(ts, 0, sizeof *ts);
    char ext[16];
    probe_info p;
    if (refused(path, ext, sizeof ext, &p, why, whyn)) return 1;
#endif
    int rate = p.rate;
    if (!strcmp(ext, ".mp3")) {          /* rekordbox's MP3 reader, whatever the layer */
        char lib[1024];
#ifdef _WIN32
        snprintf(lib, sizeof lib, "%s\\libmpg123.dll", rekordbox_app);   /* --rekordbox DIR: rekordbox.exe's folder */
#else
        snprintf(lib, sizeof lib, "%s/Contents/MacOS/libmpg123.0.dylib", rekordbox_app);
#endif
        float *s;
        size_t n;
#ifdef RBSP_WIN_DECODE
        if (rb_mp3_file(f, lib, &s, &n, &rate, why, whyn)) return 1;
#else
        if (rb_mp3(path, lib, &s, &n, why, whyn)) return 1;
        /* the MP3 reader's rate is the first frame's (rb_mp3), as probe's */
#endif
        add_track(ts, s, n);
        snprintf(ts->how, sizeof ts->how, "rekordbox's MP3 frame map + libmpg123");
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE) || !defined(RBSP_WIN_DECODE)
    } else if (is_aac(p.format)) {
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
        if (decode_aac(f, ext, &p.pcm, ts, &rate, why, whyn)) { tracks_free(ts); return 1; }
#ifdef _WIN32
        ts->aac = 1;
#endif
#else
        if (decode_aac(path, ts, &rate, why, whyn)) { tracks_free(ts); return 1; }
#endif
#endif
    } else {
#ifdef RBSP_WIN_DECODE
        pcm_out o;
        if (pcm_decode(f, &p.pcm, &o, why, whyn)) return 1;
        float *s = o.s;
        size_t n = o.n;
        int ch = o.ch;
#else
        float *s;
        size_t n;
        int ch;
        if (decode_pcm(path, &s, &n, &rate, &ch, why, whyn)) return 1;
#endif
        add_track(ts, s, n);
        snprintf(ts->how, sizeof ts->how, "%s %d Hz%s", p.format == kAudioFormatFLAC ? "flac" : p.format == kAudioFormatAppleLossless ? "alac" : "pcm",
                 rate, ch > 2 ? ", first 2 channels" : "");
    }
    for (int i = 0; i < ts->count; i++)
        if (!ts->t[i].n) { tracks_free(ts); snprintf(why, whyn, "no audio"); return 1; }
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
    enum win_resample_mode mode = track_resample_mode(&p, ext);
    int candidate = mode == RESAMPLE_CANDIDATE;
    /* the cell was the probe's: a decode at another rate isn't that cell */
    if (rate != p.rate) {
        tracks_free(ts);
        snprintf(why, whyn, "%d Hz after decoding: the file's header says %d Hz", rate, p.rate);
        return 1;
    }
    if (rate != 44100 && win_rate_refused(rate, candidate, " after decoding", why, whyn)) {
        tracks_free(ts);
        return 1;
    }
    ts->source_rate = rate;
    if (rate != 44100) {
        for (int i = 0; i < ts->count; i++) {
            track *t = &ts->t[i];
            if (t->s) continue;
            if (!t->p16 || mode == RESAMPLE_REFUSE || t->ch != p.ch) {
                tracks_free(ts);
                snprintf(why, whyn, "%d Hz after decoding: RB Stems Plus doesn't prepare files rekordbox resamples from this rate on Windows yet", rate);
                return 1;
            }
            /* Resampling an incomplete decode would filter the missing tail as silence. */
            if (t->nominal_n) {
                snprintf(why, whyn, "%s", ts->how);
                tracks_free(ts);
                return 1;
            }
            t->s = to_float(t);
            if (!t->s) { tracks_free(ts); snprintf(why, whyn, "out of memory"); return 1; }
        }
        /* resample_win.c, rekordbox for Windows' converter */
        tracks out = *ts;
        int rc = 0;
        for (int i = 0; i < ts->count && !rc; i++) {
            float *o = NULL;
            size_t n = 0;
            TIMING(uint64_t r0 = t_now();)
#ifdef RBSP_TEST_FORMAT_RATES
            if (candidate) rc = rb_resample_win_any(ts->t[i].s, ts->t[i].n, rate, &o, &n);
            else
#endif
                rc = rb_resample(ts->t[i].s, ts->t[i].n, rate, &o, &n);
            TIMING(t_resample += t_now() - r0;)
            out.t[i] = (track){.s = o, .ch = 2, .owner = 1, .n = n};
            if (rc) out.count = i + (o != NULL);
        }
        tracks_free(ts);
        *ts = out;
        if (rc) {
            tracks_free(ts);
            if (rc == 2) snprintf(why, whyn, "%d Hz: the resampler's filter built on this PC isn't the proved one", rate);
            else snprintf(why, whyn, "%d Hz: rekordbox's resampler doesn't take it", rate);
            return 1;
        }
        plat_strlcat(ts->how, ", rekordbox for Windows' resampler", sizeof ts->how);
    }
#else
    if (rate != 44100) {
        tracks out = *ts;
        int bad = 0;
        for (int i = 0; i < ts->count; i++) {
            float *in = ts->t[i].s, *o = NULL;
            size_t n = 0;
            if (!in) in = to_float(&ts->t[i]);
            TIMING(uint64_t r0 = t_now();)
            if (!in || rb_resample(in, ts->t[i].n, rate, &o, &n)) bad = 1;
            TIMING(t_resample += t_now() - r0;)
            if (in != ts->t[i].s) big_free(in);
            out.t[i] = (track){o, NULL, 2, 1, n};
            if (bad) { out.count = i + (o != NULL); break; }
        }
        tracks_free(ts);
        *ts = out;
        if (bad) {
            tracks_free(ts);
            snprintf(why, whyn, "%d Hz: rekordbox's resampler doesn't take it", rate);
            return 1;
        }
        strlcat(ts->how, ", rekordbox's resampler", sizeof ts->how);
    }
#endif
    return 0;
}

#ifdef RBSP_WIN_DECODE
int load_tracks(const char *path, const char *rekordbox_app, tracks *ts, char *why, size_t whyn) {
    memset(ts, 0, sizeof *ts);
    char ext[16];
    if (track_extension(path, ext, sizeof ext, why, whyn)) return 1;
    plat_file f;
    if (plat_track_open(path, &f)) { snprintf(why, whyn, "the file can't be read"); return 1; }
    int rc = load_open_track(f, ext, rekordbox_app, ts, why, whyn);
    plat_file_close(f);
    return rc;
}
#endif

/* rekordbox's chunk i of a track: start i*HOP-PRE, zero-padded; the track's final sample is
 * never sent */
void chunk_at(const track *t, size_t i, float *c) {
    int64_t n = (int64_t)t->n, lo = (int64_t)i * CHUNK_HOP - CHUNK_PRE;
    int64_t last = n - 1;
#if defined(_WIN32) || defined(RBSP_TEST_MF_TRACK)
    if (t->nominal_n) last = (int64_t)t->nominal_n - 1;
#endif
    memset(c, 0, sizeof(float) * 2 * CHUNK_L);
    int64_t s0 = lo > 0 ? lo : 0, s1 = lo + CHUNK_L < n ? lo + CHUNK_L : n;
    const float k = 1.0f / 32768.0f;
    size_t tch = (size_t)t->ch;
    for (int ch = 0; ch < 2; ch++) {
        float *o = c + (size_t)ch * CHUNK_L;
        if (s1 > s0 && t->s) memcpy(o + (s0 - lo), t->s + (size_t)ch * t->n + s0, sizeof(float) * (size_t)(s1 - s0));
        else if (s1 > s0) {
            size_t c1 = ch && tch > 1 ? 1 : 0;
            for (int64_t j = s0; j < s1; j++) o[j - lo] = t->p16[(size_t)j * tch + c1] * k;
        }
        if (lo <= last && last < lo + CHUNK_L) o[last - lo] = 0;
    }
}

size_t n_chunks(size_t n) { return (n + CHUNK_PRE + CHUNK_HOP - 1) / CHUNK_HOP; }

#if defined(_WIN32) || defined(RBSP_TEST_MF_TRACK)
/* If audio is missing before the nominal last sample, keep only windows ending in decoded
 * audio. The nominal last sample itself is zero in every chunk, even after a full decode. */
size_t track_chunks(const track *t) {
    if (!t->nominal_n || t->n >= t->nominal_n - 1) return n_chunks(t->n);
    size_t end = t->n + CHUNK_PRE;
    return end < CHUNK_L ? 0 : 1 + (end - CHUNK_L) / CHUNK_HOP;
}
#endif
