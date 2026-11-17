/* test tool (helper/tests/win-alac.sh): the Windows helper's MP4 probe and ALAC decoder
 * (helper/m4a.c, alac.c) against the Mac's AudioToolbox path (decode.c's probe and decode_pcm,
 * built here as the Mac helper has them) on the same files.
 *
 * stdin: one fixture per line, KIND TAB PATH [TAB REF]:
 *   alac   m4a_probe and alac_decode agree with AudioToolbox: rate, channels, bit depth, seconds,
 *          the length (FileLengthFrames), the planar float output byte for byte
 *   same   PATH (a rewritten REF: 64-bit box sizes, size 0) decodes to REF's AudioToolbox output
 *   aac    m4a_probe says AAC where decode.c's is_aac does, with its rate, channels and seconds
 *   refuse m4a_probe or alac_decode refuses it (never a crash)
 *   fuzz   whatever it does, no crash or sanitizer report, and a decode gives its stated length
 * stdout: one ok/FAIL line per check (fuzz: one line for all), a total; exit 1 on a failure. */
#include "../decode.c"
#include "pcm.h"
#include <stdarg.h>
#include <time.h>

/* decode.c's other readers: never called here (load_tracks isn't) */
int rb_mp3(const char *path, const char *libmpg123, float **out, size_t *n, char *why, size_t whyn) {
    (void)path, (void)libmpg123, (void)out, (void)n, (void)why, (void)whyn;
    abort();
}
int mp4_audio_track(const char *path, mp4_audio *out) { (void)path, (void)out; abort(); }
int aac_decode_all(const char *path, aac_outputs *o, char *why, size_t whyn) { (void)path, (void)o, (void)why, (void)whyn; abort(); }
void aac_outputs_free(aac_outputs *o) { (void)o; abort(); }
int rb_resample(const float *in, size_t n, int rate, float **out, size_t *nout) { (void)in, (void)n, (void)rate, (void)out, (void)nout; abort(); }

/* pcm.c's stream helpers (as there; pcm.c itself needs libFLAC and the other probes) */
int pcm_out_init(pcm_out *o, size_t n, int ch) {
    memset(o, 0, sizeof *o);
    if (ch < 1 || n > SIZE_MAX / (2 * sizeof(float)) - 1) return 1;
    o->s = big_alloc(sizeof(float) * 2 * (n ? n : 1));
    o->n = n;
    o->ch = ch;
    return o->s ? 0 : 1;
}

void pcm_out_free(pcm_out *o) {
    big_free(o->s);
    memset(o, 0, sizeof *o);
}

int pcm_put_i32(pcm_out *o, const int32_t *x, size_t frames) {
    if (frames > o->n - o->got) return 1;
    const float k = 1.0f / 2147483648.0f;
    size_t ch = (size_t)o->ch, c1 = ch > 1 ? 1 : 0;
    float *l = o->s + o->got, *r = o->s + o->n + o->got;
    for (size_t i = 0; i < frames; i++) {
        l[i] = (float)x[i * ch] * k;
        r[i] = (float)x[i * ch + c1] * k;
    }
    o->got += frames;
    return 0;
}

int pcm_pread_all(plat_file f, void *buf, size_t n, int64_t off) {
    size_t done = 0;
    while (done < n) {
        size_t got = 0;
        if (plat_pread(f, (char *)buf + done, n - done, off + (int64_t)done, &got) || !got) return 1;
        done += got;
    }
    return 0;
}

static int fails, checks;

static void check(int ok, const char *name, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void check(int ok, const char *name, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("%s %s: ", ok ? "ok  " : "FAIL", name);
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
    checks++;
    if (!ok) fails++;
    fflush(stdout);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

/* ALAC's source bit depth as AudioToolbox reads it (kAppleLosslessFormatFlag_16/20/24/32BitSourceData) */
static int mac_alac_bits(const char *path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    AudioFileID af;
    OSStatus e = url ? AudioFileOpenURL(url, kAudioFileReadPermission, 0, &af) : 1;
    if (url) CFRelease(url);
    if (e) return 0;
    AudioStreamBasicDescription d;
    UInt32 sz = sizeof d;
    e = AudioFileGetProperty(af, kAudioFilePropertyDataFormat, &sz, &d);
    AudioFileClose(af);
    static const int bits[5] = {0, 16, 20, 24, 32};
    return e || d.mFormatFlags > 4 ? 0 : bits[d.mFormatFlags];
}

/* the Windows path on one file: 0 and *p (*o too when decode and it is ALAC), or 1 and why */
static int win(const char *path, int decode, pcm_info *p, pcm_out *o, double *secs, char *why, size_t whyn) {
    plat_file f;
    plat_stat st;
    memset(o, 0, sizeof *o);
    if (plat_file_open_path(path, &f)) { snprintf(why, whyn, "can't open"); return 1; }
    int rc = 1;
    if (plat_file_stat(f, &st)) { snprintf(why, whyn, "can't stat"); goto done; }
    if (m4a_probe(f, st.size, p, why, whyn)) goto done;
    if (decode && p->kind == PCM_ALAC) {
        double t0 = now();
        if (pcm_out_init(o, (size_t)p->frames, p->ch)) { snprintf(why, whyn, "out of memory"); goto done; }
        if (alac_decode(f, st.size, p, o, why, whyn)) { pcm_out_free(o); goto done; }
        if (secs) *secs = now() - t0;
    }
    rc = 0;
done:
    plat_file_close(f);
    return rc;
}

static const char *base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int seconds_close(double a, double b) {
    double d = a > b ? a - b : b - a, tol = a * 0.01 > 0.1 ? a * 0.01 : 0.1;
    return d <= tol;
}

static void test_alac(const char *path, const char *ref) {
    const char *name = base(path);
    char why[300], mwhy[300];
    pcm_info p;
    pcm_out o;
    double tw = 0;
    int rc = win(path, 1, &p, &o, &tw, why, sizeof why);
    const char *mp = ref ? ref : path;
    probe_info mi;
    if (probe(mp, &mi)) {                /* the Mac helper refuses it: so must this */
        check(rc != 0, name, "AudioToolbox can't open it; here: %s%s", rc ? "refused too: " : "NOT refused", rc ? why : "");
        if (!rc) pcm_out_free(&o);
        return;
    }
    float *ms = NULL;
    size_t mn = 0;
    int mrate = 0, mch = 0;
    double t0 = now();
    int mrc = decode_pcm(mp, &ms, &mn, &mrate, &mch, mwhy, sizeof mwhy);
    double tm = now() - t0;
    if (mrc) {                           /* the Mac helper refuses it: so must this */
        check(rc != 0, name, "AudioToolbox refuses it (%s); here: %s%s", mwhy, rc ? "refused too: " : "NOT refused", rc ? why : "");
        if (!rc) pcm_out_free(&o);
        return;
    }
    if (rc) { check(0, name, "refused: %s (AudioToolbox: %d Hz, %d ch, %zu samples)", why, mi.rate, mi.ch, mn); big_free(ms); return; }
    if (!ref) {
        int mbits = mac_alac_bits(path);
        check(p.kind == PCM_ALAC && mi.format == kAudioFormatAppleLossless && p.rate == mi.rate && p.ch == mi.ch && p.bits == mbits
                  && seconds_close(mi.seconds, p.seconds) && p.frames == mn,
              name, "probe: ALAC %d Hz %d ch %d-bit %.3f s %llu samples (AudioToolbox: %s %d Hz %d ch %d-bit %.3f s %zu samples)",
              p.rate, p.ch, p.bits, p.seconds, (unsigned long long)p.frames, mi.format == kAudioFormatAppleLossless ? "ALAC" : "not ALAC",
              mi.rate, mi.ch, mbits, mi.seconds, mn);
    }
    int same = o.n == mn && o.ch == mch && !memcmp(o.s, ms, sizeof(float) * 2 * mn);
    size_t first = 0;
    if (!same && o.n == mn)
        while (first < 2 * mn && !memcmp(o.s + first, ms + first, sizeof(float))) first++;
    char diff[80] = "";
    if (!same && o.n == mn) snprintf(diff, sizeof diff, ", first difference at %s sample %zu", first < mn ? "left" : "right", first % (mn ? mn : 1));
    check(same, name, "decode%s: %zu samples, %d ch (AudioToolbox: %zu, %d ch)%s%s", ref ? " (as its reference)" : "", o.n, o.ch, mn, mch,
          same ? ", bit-identical" : "", diff);
    if (mn > 44100 * 60) printf("     %s: %.0f ms here, %.0f ms AudioToolbox\n", name, tw * 1e3, tm * 1e3);
    pcm_out_free(&o);
    big_free(ms);
}

static void test_aac(const char *path) {
    const char *name = base(path);
    char why[300];
    pcm_info p;
    pcm_out o;
    int rc = win(path, 0, &p, &o, NULL, why, sizeof why);
    probe_info mi;
    if (probe(path, &mi)) { check(0, name, "AudioToolbox can't open it"); return; }
    if (rc) { check(0, name, "refused: %s (AudioToolbox: %s, %d Hz, %d ch)", why, is_aac(mi.format) ? "AAC" : "not AAC", mi.rate, mi.ch); return; }
    check(p.kind == PCM_AAC && is_aac(mi.format) && p.rate == mi.rate && p.ch == mi.ch && seconds_close(mi.seconds, p.seconds), name,
          "probe: %s %d Hz %d ch %.3f s (AudioToolbox: %s '%c%c%c%c' %d Hz %d ch %.3f s)", p.kind == PCM_AAC ? "AAC" : "not AAC", p.rate,
          p.ch, p.seconds, is_aac(mi.format) ? "AAC" : "not AAC", (char)(mi.format >> 24), (char)(mi.format >> 16), (char)(mi.format >> 8),
          (char)mi.format, mi.rate, mi.ch, mi.seconds);
}

/* reason: when the manifest gives one, the refusal's sentence must contain it */
static void test_refuse(const char *path, const char *reason) {
    char why[300];
    pcm_info p;
    pcm_out o;
    int rc = win(path, 1, &p, &o, NULL, why, sizeof why);
    probe_info mi;
    char mac[80];
    if (probe(path, &mi)) snprintf(mac, sizeof mac, "AudioToolbox can't open it either");
    else snprintf(mac, sizeof mac, "AudioToolbox opens it: '%c%c%c%c' %d Hz %d ch", (char)(mi.format >> 24), (char)(mi.format >> 16),
                  (char)(mi.format >> 8), (char)mi.format, mi.rate, mi.ch);
    if (!rc) pcm_out_free(&o);
    check(rc != 0 && (!reason || strstr(why, reason)), base(path), "%s%s (%s)%s%s", rc ? "refused: " : "NOT refused", rc ? why : "", mac,
          reason ? ", expected: " : "", reason ? reason : "");
}

/* an accepted difference (helper.md): AudioToolbox decodes it, this helper refuses it */
static void test_mac_accepts(const char *path) {
    char why[300], mwhy[300];
    pcm_info p;
    pcm_out o;
    int rc = win(path, 1, &p, &o, NULL, why, sizeof why);
    if (!rc) pcm_out_free(&o);
    float *ms = NULL;
    size_t mn = 0;
    int mrate = 0, mch = 0;
    int mrc = decode_pcm(path, &ms, &mn, &mrate, &mch, mwhy, sizeof mwhy);
    if (!mrc) big_free(ms);
    check(rc != 0 && mrc == 0, base(path), "the Mac decodes it (%s), Windows refuses it (%s)", mrc ? mwhy : "yes", rc ? why : "NOT refused");
}

/* also counted, not checked: how often AudioToolbox does the same (refuses it too, or gives the
 * same samples) */
static int fuzz_n, fuzz_refused, fuzz_decoded, fuzz_same;
static void test_fuzz(const char *path) {
    char why[300], mwhy[300];
    pcm_info p;
    pcm_out o;
    fuzz_n++;
    int rc = win(path, 1, &p, &o, NULL, why, sizeof why) || p.kind != PCM_ALAC;
    probe_info mi;
    float *ms = NULL;
    size_t mn = 0;
    int mrate, mch;
    int mrc = probe(path, &mi) || decode_pcm(path, &ms, &mn, &mrate, &mch, mwhy, sizeof mwhy);
    if (rc) fuzz_refused++;
    else {
        fuzz_decoded++;
        if (o.got != o.n) check(0, base(path), "decoded %zu of %zu samples without an error", o.got, o.n);
    }
    if ((rc && mrc) || (!rc && !mrc && o.n == mn && !memcmp(o.s, ms, sizeof(float) * 2 * mn))) fuzz_same++;
    if (!rc) pcm_out_free(&o);
    if (!mrc) big_free(ms);
}

int main(void) {
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        char *kind = line, *path = strchr(line, '\t'), *ref = NULL;
        if (!path) continue;
        *path++ = 0;
        if ((ref = strchr(path, '\t'))) *ref++ = 0;
        if (!strcmp(kind, "alac")) test_alac(path, NULL);
        else if (!strcmp(kind, "same")) test_alac(path, ref);
        else if (!strcmp(kind, "aac")) test_aac(path);
        else if (!strcmp(kind, "refuse")) test_refuse(path, ref);
        else if (!strcmp(kind, "mac-accepts")) test_mac_accepts(path);
        else if (!strcmp(kind, "fuzz")) test_fuzz(path);
        else { fprintf(stderr, "unknown kind %s\n", kind); return 2; }
    }
    if (fuzz_n)
        check(1, "fuzz", "%d corrupted files: %d refused, %d decoded to their stated length, no crash (as AudioToolbox: %d)", fuzz_n,
              fuzz_refused, fuzz_decoded, fuzz_same);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
