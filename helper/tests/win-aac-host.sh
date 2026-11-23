#!/bin/sh
# Source checks for the Windows AAC path that run on the host without the Windows SDK.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-win-aac.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT

# Exercise the production MF startup latch and reader-thread COM cleanup.
{
  cat <<'C'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int32_t HRESULT;
typedef void *HMODULE;
typedef int plat_once_t;
#define PLAT_ONCE_INIT 0
#define FAILED(h) ((h) < 0)
#define SUCCEEDED(h) ((h) >= 0)
#define RPC_E_CHANGED_MODE ((HRESULT)0x80010106)
enum { COINIT_MULTITHREADED, MF_VERSION, MFSTARTUP_NOSOCKET };
static HRESULT co_result, mf_result;
static int startups, cleanups, loads, exits, available = 1;
static HRESULT CoInitializeEx(void *p, int mode) { (void)p; (void)mode; return co_result; }
static void CoUninitialize(void) { cleanups++; }
static HRESULT startup(int v, int flags) { (void)v; (void)flags; startups++; return mf_result; }
static void shutdown(void) {}
static struct {
    HRESULT (*startup)(int, int);
    void (*shutdown)(void);
    int create_attributes, create_media_type, bs_on_stream, reader_from_bs, ok;
} mf = {startup, shutdown, 1, 1, 1, 1, 0};
static HMODULE mf_dll(const wchar_t *name) { (void)name; loads++; return available ? &available : NULL; }
static void plat_once(plat_once_t *o, void (*fn)(void)) { if (!*o) { *o = 1; fn(); } }
static int save_cleanup(void (*fn)(void)) { (void)fn; exits++; return 0; }
#define atexit save_cleanup
#define MF_PROC(dst, mod, name) ((void)(dst), (void)(mod), (void)(name))
C
  sed -n '/^static void mf_stop(/,/^\/\* One sample/{ /^\/\* One sample/!p; }' "$here/../aac_mf.c"
  cat <<'C'
static int decode_once(char *why, size_t whyn) {
C
  sed -n '/^    if (!aac_mf_available(why, whyn)) return 1;/,/^    IMFByteStream \*bs = NULL;/{ /^    IMFByteStream /!p; }' "$here/../aac_mf.c"
  echo '    rc = 0;'
  sed -n '/^    if (SUCCEEDED(co)) CoUninitialize();/,/^    return rc;$/p' "$here/../aac_mf.c"
  cat <<'C'
}

static void check(HRESULT co, int want, int releases, const char *message) {
    char why[128] = "";
    co_result = co;
    cleanups = 0;
    int rc = decode_once(why, sizeof why);
    if (rc != want || cleanups != releases || !strstr(why, message)) {
        fprintf(stderr, "COM result 0x%08x: returned %d, MF startups %d, COM releases %d, message: %s\n",
                (unsigned)co, rc, startups, cleanups, why);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    HRESULT oom = (HRESULT)0x8007000e;
    available = strcmp(argv[1], "missing") != 0;
    mf_result = !strcmp(argv[1], "startup-failed") ? oom : 0;
    int ok = available && !mf_result;
    if (ok) {
        check(oom, oom, 0, "0x8007000e");
        check(RPC_E_CHANGED_MODE, 0, 0, "");
        check(0, 0, 1, "");
        check(1, 0, 1, "");
    } else {
        check(0, 1, 0, "Media Feature Pack");
    }
    for (int i = 0; i < 5000; i++) {
        char why[120];
        if (aac_mf_available(why, sizeof why) != ok || (!ok && !strstr(why, "Media Feature Pack"))) return 1;
    }
    if (loads != 1 + available || startups != available || exits != ok) {
        fprintf(stderr, "FAIL AAC MF %s: %d DLL loads, %d startups, %d cleanup callbacks\n", argv[1], loads, startups, exits);
        return 1;
    }
    printf("AAC MF %s: 5,000 availability calls, %d startup, %d cleanup callback\n", argv[1], startups, exits);
}
C
} > "$scratch/com.c"
clang -std=c11 -Wall -Wextra -Werror "$scratch/com.c" -o "$scratch/com"
for state in ready missing startup-failed; do "$scratch/com" "$state"; done

# Exercise the production decoder version lookup with controlled Windows version calls.
sed -n '/^#define MF_PROC/,/^static void mf_stop/{ /^static void mf_stop/!p; }' "$here/../aac_mf.c" > "$scratch/aac-version.inc"
clang -std=c11 -Wall -Wextra -Werror -I"$scratch" -I"$here/.." -I"$here/../../common" \
  "$here/win-aac-version.c" -o "$scratch/version"
"$scratch/version" known
"$scratch/version" unavailable

# Exercise the production IStream and open_stream against a sparse host file.
sed -n '/^\/\* ---.* the byte stream \*\//,/^\/\* ---.* one decode \*\//p' "$here/../aac_mf.c" > "$scratch/aac-stream.inc"
clang -std=c11 -Wall -Wextra -Werror -I"$scratch" -I"$here/../../common" \
  "$here/win-aac-stream.c" -o "$scratch/stream"
"$scratch/stream" "$scratch/large.mp4"

# Exercise the Source Reader's format-change handling in research and release.
{
  cat <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aac_mf.h"
#include "rbsp.h"
typedef int32_t HRESULT;
typedef unsigned long DWORD;
typedef long long LONGLONG;
typedef unsigned char BYTE;
typedef int IMFSample, IMFMediaBuffer;
typedef struct { int vt; struct { uint64_t QuadPart; } uhVal; } PROPVARIANT;
#define FAILED(h) ((h) < 0)
#define SUCCEEDED(h) ((h) >= 0)
enum { MF_SOURCE_READERF_ERROR = 1, MF_SOURCE_READERF_ENDOFSTREAM = 2,
       MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED = 0x20, MF_SOURCE_READERF_STREAMTICK = 0x100,
       MF_SOURCE_READER_MEDIASOURCE = -1, VT_UI8 = 21 };
#ifdef RBSP_TEST_FORMAT_RATES
int rb_aac_test_unproved, rb_rates_test_unproved;
#endif
static const int rb_MF_PD_DURATION = 0;
static int initial_rate, next_rate, next_ch, next_bits, no_change, has_sample, reads, releases, changed;
static size_t prior_frames;
static int16_t data[2048] = {123, -456};
static IMFSample sample_bytes;
static void PropVariantInit(PROPVARIANT *pv) { pv->vt = 0; }
static void PropVariantClear(PROPVARIANT *pv) { (void)pv; }
static HRESULT IMFSourceReader_GetPresentationAttribute(void *r, DWORD st, const int *id, PROPVARIANT *pv) {
    (void)r; (void)st; (void)id; (void)pv;
    return -1;
}
static HRESULT current_pcm(void *r, DWORD st, aac_mf_info *now) {
    (void)r; (void)st;
    if (changed && !next_rate) return (HRESULT)0x80004005;
    now->rate = changed ? next_rate : initial_rate;
    now->ch = changed ? next_ch : 2;
    now->bits = changed ? next_bits : 16;
    return 0;
}
static HRESULT next_sample(void *r, DWORD st, void *cb, DWORD *flags, LONGLONG *ts, IMFSample **s) {
    (void)r; (void)st; (void)cb;
    int step = reads++ - !!prior_frames;
    if (step < 0) {
        sample_bytes = (int)(prior_frames * 4);
        *s = &sample_bytes;
    } else if (step == 0) {
        changed = !no_change;
        if (changed) *flags = MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED;
        sample_bytes = 2 * (no_change ? 2 : next_ch);
        if (has_sample) *s = &sample_bytes;
    } else if (step == 1 && !has_sample) {
        *s = &sample_bytes;
    } else {
        *flags = MF_SOURCE_READERF_ENDOFSTREAM;
    }
    *ts = 0;
    return 0;
}
static HRESULT IMFSample_ConvertToContiguousBuffer(IMFSample *s, IMFMediaBuffer **b) { *b = s; return 0; }
static HRESULT IMFMediaBuffer_Lock(IMFMediaBuffer *b, BYTE **p, DWORD *max, DWORD *len) {
    (void)max;
    *p = (BYTE *)data;
    *len = (DWORD)*b;
    return 0;
}
static void IMFMediaBuffer_Unlock(IMFMediaBuffer *b) { (void)b; }
static void IMFMediaBuffer_Release(IMFMediaBuffer *b) { (void)b; }
static void IMFSample_Release(IMFSample *s) { (void)s; releases++; }
#define big_alloc malloc
#define big_realloc realloc
#define big_free free

static int read_pcm(aac_mf_info *in, int16_t **pcm, size_t *frames, char *why, size_t whyn) {
    const int last = 0;
    void *r = NULL, *cb = NULL;
    HRESULT h;
    int rc = 1;
    int16_t *buf = NULL;
    size_t cap = 0, n = 0;
C
  sed -n '/^    if (FAILED(current_pcm(r,/,/^    if (t) IMFMediaType_Release(t);/{ /^    if (t) IMFMediaType_Release(t);/!p; }' "$here/../aac_mf.c"
  cat <<'C'
    return rc;
}

int main(int argc, char **argv) {
    if (argc != 8 && argc != 12) return 2;
#ifdef RBSP_TEST_FORMAT_RATES
    rb_aac_test_unproved = atoi(argv[1]);
    rb_rates_test_unproved = argc == 12 && atoi(argv[8]);
#endif
    initial_rate = atoi(argv[2]);
    no_change = !strcmp(argv[3], "none");
    next_rate = atoi(argv[3]);
    prior_frames = (size_t)atoi(argv[4]);
    has_sample = atoi(argv[5]);
    next_ch = atoi(argv[6]);
    next_bits = argc == 12 ? atoi(argv[11]) : 16;
    aac_mf_info in = {
        .native_rate = argc == 12 ? atoi(argv[9]) : (initial_rate == 48000 ? 48000 : 22050),
        .native_ch = argc == 12 ? atoi(argv[10]) : 2,
        .native_sub = 0x1610,
    };
    const aac_mf_info *got = &in;   /* The export scan checks for host names. */
    int probe = !strcmp(argv[7], "probe"), refuse = !strcmp(argv[7], "refuse");
    int16_t *pcm = NULL;
    size_t frames = 0;
    char why[160] = "";
    int rc = read_pcm(&in, probe ? NULL : &pcm, probe ? NULL : &frames, why, sizeof why);
    int ok = rc == refuse && got->typechanges == !no_change;
    if (refuse) {
        ok = ok && !frames && !pcm && releases == !!prior_frames + has_sample && strstr(why, "changes format partway");
    } else {
        int want_rate = no_change ? initial_rate : next_rate;
        int want_ch = no_change ? 2 : next_ch;
        ok = ok && got->rate == want_rate && got->ch == want_ch && releases == 1 && !why[0];
        if (probe) ok = ok && !frames && reads == (has_sample ? 1 : 2);
        else ok = ok && frames == 1 && pcm && pcm[0] == 123 && (want_ch == 1 || pcm[1] == -456);
    }
    if (!ok)
        fprintf(stderr, "FAIL AAC format change (%s, %d -> %s Hz/%d ch at frame %zu): rc=%d rate=%d ch=%d frames=%zu reads=%d releases=%d typechanges=%zu: %s\n",
                argv[7], initial_rate, argv[3], next_ch, prior_frames, rc, got->rate, got->ch, frames, reads, releases, got->typechanges, why);
    free(pcm);
    return !ok;
}
C
} > "$scratch/type-change.c"
for mode in research release; do
  define=
  if [ "$mode" = research ]; then define=-DRBSP_TEST_FORMAT_RATES; fi
  clang -std=c11 -Wall -Wextra -Werror $define -I"$here/.." -I"$here/../../common" \
    "$scratch/type-change.c" -o "$scratch/type-change-$mode"
done
type_change() {
  mode=$1; shift
  "$scratch/type-change-$mode" "$@" 2> "$scratch/type-change.log" || { cat "$scratch/type-change.log" >&2; return 1; }
}
type_change research 1 44100 22050 0 1 2 accept
grep -qF 'PCM before=44100 Hz/2 ch/16 bit after=22050 Hz/2 ch/16 bit' "$scratch/type-change.log" ||
  { echo 'FAIL AAC format change: missing research PCM types' >&2; exit 1; }
grep -qF 'frames=0 sample=1' "$scratch/type-change.log"
type_change research 1 44100 22050 1024 1 2 refuse
grep -qF 'PCM before=44100 Hz/2 ch/16 bit after=22050 Hz/2 ch/16 bit' "$scratch/type-change.log"
grep -qF 'frames=1024 sample=1' "$scratch/type-change.log"
type_change research 1 22050 0 0 0 2 refuse
grep -qF 'after=unavailable' "$scratch/type-change.log"
grep -qF 'type_hr=0x80004005' "$scratch/type-change.log"
type_change research 1 22050 22050 0 0 2 accept
grep -qF 'PCM before=22050 Hz/2 ch/16 bit after=22050 Hz/2 ch/16 bit' "$scratch/type-change.log"
type_change research 1 48000 44100 0 1 1 accept
test ! -s "$scratch/type-change.log"
type_change research 0 22050 44100 0 1 2 refuse
test ! -s "$scratch/type-change.log"
type_change release 0 44100 22050 0 1 2 accept
type_change release 0 48000 44100 0 1 1 refuse
type_change release 0 22050 44100 0 1 2 refuse
type_change release 0 44100 44100 0 1 1 refuse
type_change release 0 44100 22050 1024 1 2 refuse
type_change release 0 44100 22050 0 1 2 probe
test ! -s "$scratch/type-change.log"
type_change release 0 44100 none 0 1 2 probe
type_change release 0 44100 none 0 1 2 accept
echo 'AAC format change: observed startup accepted, unproved release and later changes refused, stable probe and PCM preserved'

for row in 16000:2 24000:2 32000:1; do
  rate=${row%:*} ch=${row#*:}
  type_change release 0 44100 "$rate" 0 1 "$ch" accept 0 "$rate" "$ch" 16
  type_change release 0 48000 "$rate" 0 0 "$ch" probe 0 "$rate" "$ch" 16
  type_change research 0 44100 "$rate" 0 1 "$ch" accept 0 "$rate" "$ch" 16
  type_change research 0 44100 "$rate" 0 1 "$ch" accept 1 "$rate" "$ch" 16
done
for rate in 16000 22050 24000; do
  type_change release 0 44100 "$rate" 0 1 1 refuse 1 "$rate" 1 16
  type_change research 0 44100 "$rate" 0 1 1 refuse 0 "$rate" 1 16
  type_change research 0 44100 "$rate" 0 1 1 accept 1 "$rate" 1 16
  type_change research 0 44100 "$rate" 0 0 1 probe 1 "$rate" 1 16
done
for mode in release research; do
  type_change "$mode" 0 44100 16000 1024 1 2 refuse 1 16000 2 16
  type_change "$mode" 0 44100 24000 0 1 2 refuse 1 16000 2 16
  type_change "$mode" 0 44100 16000 0 1 1 refuse 1 16000 2 16
  type_change "$mode" 0 44100 16000 0 1 2 refuse 1 16000 2 24
  type_change "$mode" 0 44100 48000 0 1 1 refuse 1 48000 1 16
  type_change "$mode" 0 44100 32000 0 1 2 refuse 1 32000 2 16
  type_change "$mode" 0 48000 22050 0 1 2 refuse 1 22050 2 16
done
type_change research 0 44100 16000 1024 1 1 refuse 1 16000 1 16
type_change research 0 44100 24000 0 1 1 refuse 1 16000 1 16
type_change research 0 44100 16000 0 1 2 refuse 1 16000 1 16
type_change research 0 44100 16000 0 1 1 refuse 1 16000 1 24
echo 'AAC frame zero: proved rows settle to their 16-bit header format, remaining candidates require the switch, later and mismatching changes refused'

# The production MP4 probe and Windows track assembly, with known PCM in place of MF.
sed -n '/^static int decode_aac(plat_file /,/^}$/p' "$here/../decode.c" > "$scratch/aac-length.inc"
sed -n '/^void chunk_at(/,$p' "$here/../decode.c" > "$scratch/aac-chunk.inc"
clang -std=c11 -Wall -Wextra -Werror -DRBSP_TEST_MF_TRACK -I"$scratch" -I"$here/.." -I"$here/../../common" \
  "$here/win-aac-length.c" "$here/../m4a.c" -o "$scratch/length"
python3 -I - "$scratch" <<'PY'
from pathlib import Path
from struct import pack
import sys


def box(kind, body):
    return pack('>I4s', 8 + len(body), kind) + body


def audio(track_id, scale, entries, rate=44100, ch=2, edit=False):
    asc = pack('>H', (2 << 11) | ({44100: 4, 48000: 3, 22050: 7}[rate] << 7) | (ch << 3))
    config = b'\x40\x15' + bytes(11) + b'\x05\x02' + asc
    esds = box(b'esds', bytes(4) + bytes((4, len(config))) + config)
    entry = bytes(16) + pack('>HHHHI', ch, 16, 0, 0, rate << 16) + esds
    stsd = box(b'stsd', pack('>II', 0, 1) + box(b'mp4a', entry))
    stts = box(b'stts', pack('>II', 0, len(entries)) + b''.join(pack('>II', *e) for e in entries))
    stsz = box(b'stsz', pack('>III', 0, 8, sum(count for count, _ in entries)))
    # mdhd and elst durations deliberately disagree with stts.
    mdhd = box(b'mdhd', pack('>IIIIII', 0, 0, 0, scale, 960, 0))
    hdlr = box(b'hdlr', bytes(8) + b'soun')
    mdia = box(b'mdia', mdhd + hdlr + box(b'minf', box(b'stbl', stsd + stts + stsz)))
    edts = box(b'edts', box(b'elst', pack('>IIiiHH', 0, 1, 1024, 2112, 1, 0))) if edit else b''
    tkhd = box(b'tkhd', pack('>IIIII', 3, 0, 0, track_id, 0) + bytes(64))
    return box(b'trak', tkhd + edts + mdia)


def fixture(name, *tracks):
    (Path(sys.argv[1]) / name).write_bytes(box(b'moov', b''.join(tracks)))


fixture('tail.m4a', audio(1, 44100, [(470, 1024), (1, 748)]))
fixture('short.m4a', audio(1, 44100, [(470, 1024), (1, 749)]))
fixture('short-overlap.m4a', audio(1, 44100, [(472, 1024), (1, 752)]))
fixture('scale.m4a', audio(1, 1000, [(2, 18), (1, 20)], rate=22050))
fixture('edit.m4a', audio(1, 44100, [(3, 1024), (1, 64)], edit=True))
long = audio(17, 44100, [(471, 1024), (1, 960)])
short = audio(42, 48000, [(3, 1024), (1, 128)], rate=48000, ch=1)
video = box(b'trak', box(b'tkhd', pack('>IIIII', 3, 0, 0, 99, 0) + bytes(64)) +
            box(b'mdia', box(b'hdlr', bytes(8) + b'vide')))
fixture('short-long.m4a', short, long)
fixture('long-short.mp4', long, short)
fixture('video-first.mp4', video, short, long)
fixture('ambiguous.m4a', audio(42, 44100, [(3, 1024), (1, 128)]), long)
PY
"$scratch/length" "$scratch"

# Use the build's GUID list check with the host preprocessor. Neither needs Windows headers.
guid_check="$(sed -n '/^guids=/,/^cat > "\$scratch\/guids\.c"/{ /^cat > "\$scratch\/guids\.c"/!p; }' "$here/../build-win.sh")"
cc="$scratch/cpp"
cat > "$cc" <<'SH'
#!/bin/sh
shift   # the build passes its architecture before the compiler arguments
exec clang "$@"
SH
chmod +x "$cc"
fail() { echo "$*" >&2; exit 1; }
check_guids() (
  here="$1"
  eval "$guid_check"
  printf '%s\n' "$guids"
)

mkdir -p "$scratch/tab" "$scratch/missing"
awk '!changed && sub(/^    X\(/, "\tX(") { changed = 1 } 1' "$here/../mf_guids.h" > "$scratch/tab/mf_guids.h"
awk '!changed && sub(/^    X\(/, "    X (") { changed = 1 } 1' "$here/../mf_guids.h" > "$scratch/missing/mf_guids.h"
plain="$(check_guids "$here/..")"
tab="$(check_guids "$scratch/tab")"
[ "$plain" = "$tab" ] || fail "AAC GUIDs: the tab-indented row was lost"
if check_guids "$scratch/missing" > "$scratch/missing.log" 2>&1; then
  fail "AAC GUIDs: the build accepted a row missing from the shell list"
fi
grep -q 'doesn.t match the preprocessed row count' "$scratch/missing.log"
echo "AAC GUIDs: tab indentation and missing-row checks passed"
