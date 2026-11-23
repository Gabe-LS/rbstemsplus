#!/bin/sh
# The Windows helper's resampler (helper/resample_win.c, T11) on the Mac: helper/tests/resample-win.c
# (software FMAs against the hardware's, every standard rate's table, the proved checksums, a
# golden output, the refusals). With the owner's PC dumps (rekordbox for Windows' own resampled
# audio) and the corpus files they came from, it also compares each proved row whole, bit for bit:
# 48 kHz (17, 13, w14 mono) and 96 kHz (08, 11, 18, 15). Rows whose files aren't there are skipped,
# and it says so. The inputs are decoded with ffmpeg (lossless: the same samples as the Windows
# decoders give, mono on both sides). w15 (22.05 kHz MP3) needs rekordbox's libmpg123.dll: its
# row runs on the PC.
#   RBSP_T11_CORPUS  the corpus (v1/ and win/), default build/win/t2/pc1/corpus
#   RBSP_T11_DUMPS   the folder holding dumps-20261119-190535/ and dumps-20261119-210435/,
#                    default build/rekordbox/win
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
corpus="${RBSP_T11_CORPUS:-$root/build/win/t2/pc1/corpus}"
dumps="${RBSP_T11_DUMPS:-$root/build/rekordbox/win}"
mkdir -p "$root/build"
scratch="$(mktemp -d "$root/build/resample-win.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
cd "$root"
# the golden: rb_resample's output on resample-win.c's 48 kHz noise (proved code, 2026-11-19)
clang -O2 -Wall -Wextra -Werror -ffp-contract=off -DRBSP_RESAMPLE_WIN_TEST -DRBSP_TEST_WIN_RATE -DRBSP_TEST_FORMAT_RATES \
  -DRBSP_GOLDEN_48000='"cffd220e4e0b20aa6922ae54481c44618cb0fb3d9a4d5f9ef279b1aec2c9cda3"' \
  -Ihelper -Icommon helper/tests/resample-win.c helper/resample_win.c helper/big.c common/plat_mac.c common/plat_helper.c \
  -framework CoreFoundation -framework Accelerate -framework Security -o "$scratch/resample-win"
set --
skipped=""
while read -r rate file dump; do
  d="$dumps/$dump"
  if [ ! -f "$corpus/$file" ] || [ ! -f "$d" ] || ! command -v ffmpeg > /dev/null; then skipped="$skipped $(basename "$file")"; continue; fi
  n="$scratch/$(basename "$file").f32"
  ffmpeg -loglevel error -i "$corpus/$file" -f f32le -acodec pcm_f32le "$n.raw"
  ch="$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$corpus/$file")"
  # planar stereo, as the decoders hand it to the resampler (mono on both sides)
  python3 -I - "$n.raw" "$n" "$ch" <<'PY'
import array, sys
a = array.array('f'); a.frombytes(open(sys.argv[1], 'rb').read())
ch = int(sys.argv[3])
l = a[0::ch]
r = a[1::ch] if ch > 1 else l
assert sys.byteorder == 'little'
(l + r).tofile(open(sys.argv[2], 'wb'))
PY
  set -- "$@" "$rate" "$n" "$d"
done <<'ROWS'
48000 v1/17-aiff-24-48k.aif dumps-20261119-190535/tracks/17-aiff-24-48k.rb.f32
48000 v1/13-wav-24-48k.wav dumps-20261119-210435/tracks/13-wav-24-48k.rb.f32
48000 win/w14-flac-mono-48k.flac dumps-20261119-210435/tracks/w14-flac-mono-48k.rb.f32
96000 v1/08-alac-24-96k.m4a dumps-20261119-210435/tracks/08-alac-24-96k.rb.f32
96000 v1/11-flac-24-96k.flac dumps-20261119-210435/tracks/11-flac-24-96k.rb.f32
96000 v1/18-aiff-24-96k.aif dumps-20261119-210435/tracks/18-aiff-24-96k.rb.f32
96000 v1/15-wav-f32-96k.wav dumps-20261119-210435/tracks/15-wav-f32-96k.rb.f32
ROWS
rc=0
"$scratch/resample-win" "$@" > "$scratch/out.txt" 2> "$scratch/err.txt" || rc=$?
grep -v '^  table ' "$scratch/out.txt" || true
[ -z "$skipped" ] || echo "skipped the row checks of:$skipped (the owner's PC dumps and the corpus, build/ only; ffmpeg)"
if [ "$rc" = 0 ]; then
  awk '$1 == "table" && $2 ~ /^(16000|22050|24000|32000|88200|192000)$/ {
    print "Candidate resampler table: " $2 " Hz sha256 " $5
  }' "$scratch/out.txt" > "$scratch/want.txt"
  if [ "$(wc -l < "$scratch/want.txt")" -ne 6 ] || ! cmp -s "$scratch/want.txt" "$scratch/err.txt"; then
    echo 'FAIL resample-win: stderr did not report the six candidate table hashes' >&2
    diff -u "$scratch/want.txt" "$scratch/err.txt" || true
    exit 1
  fi
  echo 'resample-win candidate SHA-256: six stderr lines match the reference tables'
fi
exit "$rc"
