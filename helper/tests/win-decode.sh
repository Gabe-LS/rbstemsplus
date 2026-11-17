#!/bin/bash
# The Windows helper's decoders against the Mac's, on this Mac (T8, T9): decode.c's two paths on
# the same files. One build as helper/build.sh compiles the helper (AudioToolbox, the reference);
# one with RBSP_WIN_DECODE, the Windows path (pcm.c's WAV/AIFF/FLAC, m4a.c and alac.c's ALAC,
# mp3.c's plat-based loading and reading, pcm.h's probes), on plat_mac.c. For each fixture
# (helper/tests/win-decode-fixtures.py, made fresh in a scratch folder):
#  - same: both decode it to the same tracks, bit for bit (decode_dump's .f32 files, after the
#    resampler where the rate isn't 44.1 kHz), or both refuse it; their probes (probe_track, the
#    estimate's) agree on refusing, on AAC, and on seconds within 0.1 s or 1%;
#  - win-refuses: the Windows path refuses it by design (rekordbox's own parsers' limits, or a
#    later task), the line says why;
#  - mac-refuses: the Mac helper's AudioToolbox can't read it but rekordbox's own reader can, and so
#    does the Windows path (8-bit FLAC);
#  - both-refuse: both refuse it (rekordbox can't load it);
#  - run-refuses: both runs refuse it, and the Windows probe already does (the Mac's probe, which
#    only opens the file, accepts it).
# expect.txt's fourth column is what the real Windows build must give (T16/T17 run it there;
# helper/tests/win-decode-vm.sh compares it). Then
# the Windows path again with ASan+UBSan: the same output on every fixture, and a seeded mutation
# pass (flipped header bytes, cuts, chunk sizes rewritten: 40 variants a fixture) with no
# sanitizer report.
# MP3 and ALAC have their own, larger sets (helper/tests/win-mp3.sh, win-alac.sh).
#
#   helper/tests/win-decode.sh [OUT]     default build/helper-win-decode; exits 1 on a failure
#
# Run outside a sandbox: inside one, AudioToolbox stops decoding some FLAC files and afconvert has
# no ALAC encoder. Needs rekordbox's libmpg123 (/Applications/rekordbox 7), ffmpeg, flac and lame.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/build/helper-win-decode}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
app="${RBSP_REKORDBOX_APP:-/Applications/rekordbox 7/rekordbox.app}"
[ -f "$app/Contents/MacOS/libmpg123.0.dylib" ] || { echo "no rekordbox at $app (RBSP_REKORDBOX_APP)"; exit 1; }
FLAC_VERSION="$(sed -n 's/^FLAC_VERSION=//p' "$root/bridge/build.sh")"
flac="$root/build/helper/flac-$FLAC_VERSION"
[ -f "$flac/lib/libFLAC.a" ] || "$root/helper/build.sh" "$out/helper" > "$out/helper-build.log"   # builds that libFLAC
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-win-decode.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
ln -s "$flac" "$scratch/flac"
alac="$scratch/alac"
"$root/scripts/deps.sh" group alac "$alac" > /dev/null

MAC="-O2 -arch arm64 -mmacosx-version-min=12.0"
FW="-framework AudioToolbox -framework CoreFoundation -framework Accelerate"
(
  cd "$root"
  # the reference: helper/build.sh's flags and files
  for t in decode_dump probe_dump; do
    # shellcheck disable=SC2086
    clang $MAC -Wall -Wextra -Werror -ffp-contract=off -c helper/resample.c -o "$scratch/resample.o"
    # shellcheck disable=SC2086
    clang $MAC -Wall -Wextra -Ihelper -Icommon helper/tests/$t.c helper/decode.c helper/aac.c helper/mp3.c helper/mp4.c helper/big.c \
      "$scratch/resample.o" $FW -o "$out/$t-mac"
  done
  for f in "$alac"/*.c; do clang $MAC -w -fwrapv -c "$f" -o "$scratch/alac_$(basename "$f" .c).o"; done
  # the Windows path, on plat_mac.c
  W="-DRBSP_WIN_DECODE -Wall -Wextra -Werror -Ihelper -Icommon -I$alac -I$scratch/flac/include"
  for t in decode_dump probe_dump; do
    # shellcheck disable=SC2086
    clang $MAC $W helper/tests/$t.c helper/decode.c helper/pcm.c helper/m4a.c helper/alac.c helper/mp3.c helper/big.c \
      "$scratch/resample.o" "$scratch"/alac_*.o common/plat_mac.c common/plat_helper.c "$scratch/flac/lib/libFLAC.a" $FW -o "$out/$t-win"
    # shellcheck disable=SC2086
    clang $MAC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer helper/tests/$t.c \
      helper/decode.c helper/pcm.c helper/m4a.c helper/alac.c helper/mp3.c helper/big.c helper/resample.c "$scratch"/alac_*.o \
      common/plat_mac.c common/plat_helper.c "$scratch/flac/lib/libFLAC.a" $FW -o "$out/$t-win-asan"
  done
) > "$out/cc.log" 2>&1 || { cat "$out/cc.log"; echo "FAIL the builds"; exit 1; }

fix="$scratch/fix"
mkdir -p "$fix" "$scratch/mac" "$scratch/win"
python3 -I "$here/win-decode-fixtures.py" "$fix" | sed 's/^/  /'
files=()
while IFS=$'\t' read -r n _ _ _; do files+=("$fix/$n"); done < "$fix/expect.txt"
"$out/decode_dump-mac" "$app" "$scratch/mac" "${files[@]}" > "$scratch/mac.txt" 2> "$scratch/mac.err"
"$out/decode_dump-win" "$app" "$scratch/win" "${files[@]}" > "$scratch/win.txt" 2> "$scratch/win.err"
"$out/probe_dump-mac" "${files[@]}" > "$scratch/pmac.txt"
"$out/probe_dump-win" "${files[@]}" > "$scratch/pwin.txt"

passed=0 failed=0
ok() { echo "ok   $*"; passed=$((passed + 1)); }
bad() { echo "FAIL $*"; failed=$((failed + 1)); }
line() { grep -m1 "^$1	" "$2" | cut -f2- || true; }
while IFS=$'\t' read -r n want why _; do
  m="$(line "$n" "$scratch/mac.txt")" w="$(line "$n" "$scratch/win.txt")"
  pm="$(line "$n" "$scratch/pmac.txt")" pw="$(line "$n" "$scratch/pwin.txt")"
  case "$want" in
    same)
      if [ "${m%%:*}" = skipped ] && [ "${w%%:*}" = skipped ]; then ok "$n: both refuse it ($w)"
      elif [ "$m" != "$w" ]; then bad "$n: mac '$m', windows '$w'"; continue
      else
        k="$(echo "$m" | cut -f1 | cut -d' ' -f1)" same=1
        for ((i = 0; i < k; i++)); do cmp -s "$scratch/mac/$n.$i.f32" "$scratch/win/$n.$i.f32" || same=0; done
        if [ "$same" = 1 ]; then ok "$n: the same $k track(s), bit for bit ($(echo "$m" | cut -f2))"; else bad "$n: the samples differ"; fi
      fi
      # the probes: the same verdict, AAC the same, seconds close
      if [ "${pm%%	*}" != "${pw%%	*}" ]; then bad "$n probe: mac '$pm', windows '$pw'"
      elif [ "${pm%%	*}" = ok ]; then
        if python3 -I -c 'import sys; a, b = float(sys.argv[1]), float(sys.argv[2]); sys.exit(0 if abs(a - b) <= max(0.1, 0.01 * a) and sys.argv[3] == sys.argv[4] else 1)' \
          "$(echo "$pm" | cut -f2)" "$(echo "$pw" | cut -f2)" "$(echo "$pm" | cut -f3)" "$(echo "$pw" | cut -f3)"; then
          ok "$n probe: $(echo "$pm" | cut -f2) s (mac) vs $(echo "$pw" | cut -f2) s"
        else bad "$n probe: mac '$pm', windows '$pw'"; fi
      else ok "$n probe: both refuse it"; fi ;;
    win-refuses)
      if [ "${w%%:*}" = skipped ] && [ "${pw%%	*}" = refused ]; then ok "$n: the Windows path refuses it: ${w#skipped: } ($why)"
      else bad "$n: the Windows path should refuse it ($why): '$w' / '$pw'"; fi ;;
    mac-refuses)
      if [ "${m%%:*}" = skipped ] && [ "${w%%:*}" != skipped ] && [ -n "$w" ]; then ok "$n: the Mac's AudioToolbox refuses it, the Windows path decodes it ($why)"
      else bad "$n: expected the Mac to refuse it and Windows to decode it ($why): mac '$m', windows '$w'"; fi ;;
    run-refuses)
      # both runs refuse it; the Mac's AudioToolbox probe still accepts it (its estimate counts a
      # file its run then skips), the Windows probe refuses it already
      if [ "${m%%:*}" = skipped ] && [ "${w%%:*}" = skipped ] && [ "${pw%%	*}" = refused ]; then ok "$n: both runs refuse it (${w#skipped: }; the Mac's probe: ${pm%%	*})"
      else bad "$n: both runs should refuse it, the Windows probe too: mac '$m' / '$pm', windows '$w' / '$pw'"; fi ;;
    both-refuse)
      if [ "${m%%:*}" = skipped ] && [ "${w%%:*}" = skipped ] && [ "${pm%%	*}" = refused ] && [ "${pw%%	*}" = refused ]; then ok "$n: both refuse it (${w#skipped: })"
      else bad "$n: both should refuse it: mac '$m' / '$pm', windows '$w' / '$pw'"; fi ;;
  esac
done < "$fix/expect.txt"
# the Windows path under ASan+UBSan: the same lines and samples as built plainly
mkdir -p "$scratch/asan"
if "$out/decode_dump-win-asan" "$app" "$scratch/asan" "${files[@]}" > "$scratch/asan.txt" 2> "$scratch/asan.err" \
  && cmp -s "$scratch/asan.txt" "$scratch/win.txt" && ! grep -qE 'Sanitizer|runtime error' "$scratch/asan.err"; then
  same=1
  for g in "$scratch"/win/*.f32; do cmp -s "$g" "$scratch/asan/$(basename "$g")" || same=0; done
  if [ "$same" = 1 ]; then ok "ASan+UBSan: the Windows path gives the same output on every fixture"; else bad "ASan+UBSan: samples differ"; fi
else bad "ASan+UBSan on the fixtures: $(grep -m3 -E 'Sanitizer|runtime error' "$scratch/asan.err")"; fi
# a seeded mutation pass over the WAV, AIFF, FLAC and MP4 fixtures
mut="$scratch/mut"
mkdir -p "$mut" "$scratch/mutout"
python3 -I - "$fix" "$mut" << 'PY'
import os, random, struct, sys
src, dst = sys.argv[1], sys.argv[2]
rng = random.Random(20261112)
for name in sorted(os.listdir(src)):
    if not name.endswith((".wav", ".aif", ".aiff", ".flac", ".m4a")):
        continue
    b = open(os.path.join(src, name), "rb").read()
    stem, ext = os.path.splitext(name)
    for i in range(40):
        m = bytearray(b)
        k = i % 4
        if k == 0:                      # flip header bytes
            for _ in range(rng.randint(1, 8)):
                at = rng.randrange(min(len(m), 4096))
                m[at] ^= 1 << rng.randrange(8)
        elif k == 1:                    # a cut
            m = m[:rng.randrange(1, len(m))]
        elif k == 2:                    # a 32-bit field near the start rewritten (chunk and box sizes)
            at = rng.randrange(0, max(1, min(len(m) - 4, 512)))
            m[at:at + 4] = struct.pack("<I" if rng.random() < 0.5 else ">I", rng.choice([0, 1, 0x7FFFFFFF, 0xFFFFFFFF, rng.randrange(1 << 32)]))
        else:                           # random bytes over a stretch of the audio
            at = rng.randrange(len(m))
            n = rng.randrange(1, 64)
            m[at:at + n] = bytes(rng.randrange(256) for _ in range(min(n, len(m) - at)))
        open(os.path.join(dst, "%s-m%02d%s" % (stem, i, ext)), "wb").write(bytes(m))
PY
n_mut="$(ls "$mut" | wc -l | tr -d ' ')"
if "$out/probe_dump-win-asan" "$mut"/* > /dev/null 2> "$scratch/mutp.err" \
  && "$out/decode_dump-win-asan" "$app" "$scratch/mutout" "$mut"/* > /dev/null 2> "$scratch/mutd.err" \
  && ! grep -qE 'Sanitizer|runtime error' "$scratch/mutp.err" "$scratch/mutd.err"; then
  ok "mutation pass: $n_mut mutated files probed and decoded under ASan+UBSan, no report"
else
  bad "mutation pass: $(grep -h -m3 -E 'Sanitizer|runtime error|SUMMARY' "$scratch/mutp.err" "$scratch/mutd.err")"
fi
echo "win-decode: $passed passed, $failed failed"
[ "$failed" = 0 ]
