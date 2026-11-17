/* A file as rekordbox's separator reads it, before chunking: which reader rekordbox uses for the
 * file type, the length it gives, the channels it keeps, its resampler. One file can come out
 * more than one way (AAC): each way is a track, and each is prepared. */
#if defined(_WIN32) || defined(RBSP_WIN_DECODE)
/* Windows (and the Mac tests that compare it with AudioToolbox): the helper's own decoders */
#include "pcm.h"
#else
#include <AudioToolbox/AudioToolbox.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rbsp.h"
#include "big.h"
#ifdef _WIN32
#include "plat_helper.h"
#define strlcat plat_strlcat             /* mingw-w64 has none */
#endif

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
typedef struct { pcm_kind format; int rate, ch; double seconds; } probe_info;

/* pcm.h's probe, by content (.mp3: rekordbox's MP3 frame scanner); why: what it refuses */
static int probe(const char *path, const char *ext, probe_info *p, char *why, size_t whyn) {
    plat_file f;
    pcm_info pi;
    if (plat_file_open_path(path, &f)) { snprintf(why, whyn, "the file can't be read"); return 1; }
    int rc = pcm_probe(f, ext, &pi, why, whyn);
    plat_file_close(f);
    if (rc) return 1;
    if (pi.rate < 1 || pi.ch < 1) { snprintf(why, whyn, "no audio stream RB Stems Plus can read"); return 1; }
    p->format = pi.kind;
    p->rate = pi.rate;
    p->ch = pi.ch;
    p->seconds = pi.seconds;
    return 0;
}

static int is_aac(pcm_kind f) { return f == PCM_AAC; }

/* FLAC, WAV, AIFF, ALAC, as the Mac's decode_pcm below gives them (pcm.h's rules) */
static int decode_pcm(const char *path, float **out, size_t *n_out, int *rate, int *ch_out, char *why, size_t whyn) {
    char ext[16];
    plat_file f;
    pcm_info pi;
    pcm_out o;
    lower_ext(path, ext, sizeof ext);
    if (plat_file_open_path(path, &f)) { snprintf(why, whyn, "the file can't be read"); return 1; }
    int rc = pcm_probe(f, ext, &pi, why, whyn) || pcm_decode(f, &pi, &o, why, whyn);
    plat_file_close(f);
    if (rc) return 1;
    *out = o.s;
    *n_out = o.n;
    *rate = pi.rate;
    *ch_out = pi.ch;
    return 0;
}
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
    AudioFileClose(af);
    if (e || d.mSampleRate < 1 || !d.mChannelsPerFrame) return 1;
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
    ts->t[ts->count] = (track){s, NULL, 2, 1, n};
    ts->count++;
    return 0;
}

#ifdef RBSP_WIN_DECODE
/* AAC through Media Foundation is helper.md's T10: refused before this (refused) */
static int decode_aac(const char *path, tracks *ts, int *rate, char *why, size_t whyn) {
    (void)path, (void)ts, (void)rate;
    snprintf(why, whyn, "AAC: RB Stems Plus doesn't prepare it on Windows yet");
    return 1;
}
#else
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

/* the files load_tracks refuses before decoding, and why: 0, or 1 and why */
static int refused(const char *path, char *ext, size_t extn, probe_info *p, char *why, size_t whyn) {
    lower_ext(path, ext, extn);
    if (!strcmp(ext, ".aac")) { snprintf(why, whyn, "raw AAC (ADTS): rekordbox can't load it"); return 1; }
    static const char *const exts[] = {".mp3", ".m4a", ".mp4", ".wav", ".aif", ".aiff", ".flac"};
    int known = 0;
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
        if (!strcmp(ext, exts[i])) known = 1;
    if (!known) { snprintf(why, whyn, "%s: not a file type rekordbox plays", ext[0] ? ext : "no extension"); return 1; }
#ifdef RBSP_WIN_DECODE
    if (probe(path, ext, p, why, whyn)) return 1;
    if (strcmp(ext, ".mp3") && is_aac(p->format)) { snprintf(why, whyn, "AAC: RB Stems Plus doesn't prepare it on Windows yet"); return 1; }
#else
    if (probe(path, p)) { snprintf(why, whyn, "no audio stream macOS can read"); return 1; }
#endif
    if (strcmp(ext, ".mp3") && is_aac(p->format)) {
        if (p->ch > 2) { snprintf(why, whyn, "AAC with %d channels: rekordbox loads it but doesn't separate it", p->ch); return 1; }
    } else if (strcmp(ext, ".mp3") && (p->rate == 11025 || p->rate == 352800 || p->rate == 384000)) {
        snprintf(why, whyn, "%d Hz: rekordbox can't load it", p->rate);
        return 1;
    }
#ifdef _WIN32
    /* rekordbox for Windows resamples with other code than the Mac's (helper.md §2): until the x64
     * resampler is matched and proved (T11), only 44.1 kHz files are prepared */
    if (p->rate != 44100) {
        snprintf(why, whyn, "%d Hz: RB Stems Plus doesn't prepare files rekordbox resamples on Windows yet", p->rate);
        return 1;
    }
#endif
    return 0;
}

int probe_track(const char *path, double *seconds, int *aac, char *why, size_t whyn) {
    char ext[16];
    probe_info p;
    if (refused(path, ext, sizeof ext, &p, why, whyn)) return 1;
    *seconds = p.seconds;
    *aac = strcmp(ext, ".mp3") && is_aac(p.format);
    return 0;
}

#ifdef RBSP_TEST_TIMING
uint64_t t_resample;
#endif

int load_tracks(const char *path, const char *rekordbox_app, tracks *ts, char *why, size_t whyn) {
    memset(ts, 0, sizeof *ts);
    char ext[16];
    probe_info p;
    if (refused(path, ext, sizeof ext, &p, why, whyn)) return 1;
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
        if (rb_mp3(path, lib, &s, &n, why, whyn)) return 1;
        add_track(ts, s, n);
        snprintf(ts->how, sizeof ts->how, "rekordbox's MP3 frame map + libmpg123");
        /* the MP3 reader's rate is the first frame's (rb_mp3), as probe's */
    } else if (is_aac(p.format)) {
        if (decode_aac(path, ts, &rate, why, whyn)) { tracks_free(ts); return 1; }
    } else {
        float *s;
        size_t n;
        int ch;
        if (decode_pcm(path, &s, &n, &rate, &ch, why, whyn)) return 1;
        add_track(ts, s, n);
        snprintf(ts->how, sizeof ts->how, "%s %d Hz%s", p.format == kAudioFormatFLAC ? "flac" : p.format == kAudioFormatAppleLossless ? "alac" : "pcm",
                 rate, ch > 2 ? ", first 2 channels" : "");
    }
    for (int i = 0; i < ts->count; i++)
        if (!ts->t[i].n) { tracks_free(ts); snprintf(why, whyn, "no audio"); return 1; }
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
    return 0;
}

/* rekordbox's chunk i of a track: start i*HOP-PRE, zero-padded; the track's final sample is
 * never sent */
void chunk_at(const track *t, size_t i, float *c) {
    long n = (long)t->n, lo = (long)i * CHUNK_HOP - CHUNK_PRE;
    memset(c, 0, sizeof(float) * 2 * CHUNK_L);
    long s0 = lo > 0 ? lo : 0, s1 = lo + CHUNK_L < n ? lo + CHUNK_L : n;
    const float k = 1.0f / 32768.0f;
    size_t tch = (size_t)t->ch;
    for (int ch = 0; ch < 2; ch++) {
        float *o = c + (size_t)ch * CHUNK_L;
        if (s1 > s0 && t->s) memcpy(o + (s0 - lo), t->s + (size_t)ch * t->n + s0, sizeof(float) * (size_t)(s1 - s0));
        else if (s1 > s0) {
            size_t c1 = ch && tch > 1 ? 1 : 0;
            for (long j = s0; j < s1; j++) o[j - lo] = t->p16[(size_t)j * tch + c1] * k;
        }
        if (lo <= n - 1 && n - 1 < lo + CHUNK_L) o[n - 1 - lo] = 0;
    }
}

size_t n_chunks(size_t n) { return (n + CHUNK_PRE + CHUNK_HOP - 1) / CHUNK_HOP; }
