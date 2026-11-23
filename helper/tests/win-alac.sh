#!/bin/sh
# The Windows helper's MP4 probe and ALAC decoder (helper/m4a.c, helper/alac.c: helper.md T9) on
# this Mac, against AudioToolbox (decode.c's probe and decode_pcm, as the Mac helper has them):
#  1. fixtures in a scratch folder (win-alac-fixtures.py: ffmpeg's and afconvert's ALAC and AAC
#     encoders on music-like WAVs, refused and broken copies; never committed);
#  2. helper/tests/win-alac.c built plain, and with ASan+UBSan, each run on every fixture;
#  3. m4a.c and alac.c cross-compiled for Windows (x64, arm64) with the pinned toolchain, -Werror.
# Apple's ALAC C files come from scripts/deps.sh (deps.lock's alac group), used unmodified (-w).
# afconvert's encoders need AudioToolbox's codec registry: inside a sandbox that hides it, the
# af* fixtures are skipped (a note says so).
#
#   helper/tests/win-alac.sh [OUT]      default build/helper-win-alac; exits 1 on a failure
#   WIN_ALAC_FIXTURES=DIR ...           the fixtures in DIR, kept (made when DIR has no manifest)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/build/helper-win-alac}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
alac="$out/alac"
mkdir -p "$alac"
sh "$root/scripts/deps.sh" group alac "$alac"
temporary="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-win-alac.XXXXXX")"
trap 'rm -rf "${temporary:?}"' EXIT
if [ -n "${WIN_ALAC_FIXTURES:-}" ]; then
  scratch="$WIN_ALAC_FIXTURES"
  mkdir -p "$scratch"
else
  scratch="$temporary"
fi
fails=0

if [ ! -s "$scratch/manifest.txt" ]; then
  echo "making the fixtures in $scratch"
  python3 -I "$here/win-alac-fixtures.py" "$scratch" > "$scratch/manifest.tmp"
  mv "$scratch/manifest.tmp" "$scratch/manifest.txt"
fi
echo "$(wc -l < "$scratch/manifest.txt" | tr -d ' ') fixtures"
reference="$temporary/alac20"
mkdir "$reference"
python3 -I "$here/alac20-fixture.py" "$reference/s20.m4a"
ffmpeg -v error -xerror -c:a alac -i "$reference/s20.m4a" -c:a pcm_f32le "$reference/ffmpeg.wav"
printf 'same\t%s\t%s\n' "$reference/s20.m4a" "$reference/ffmpeg.wav" > "$reference/manifest.txt"

# ours with the helper's Windows warnings; Apple's files as they are; the test unit (decode.c
# inside) with build.sh's
WARN="-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Werror"
APPLE="ALACBitUtilities.c dp_dec.c"           # ag_dec.c: alac.c has its dyn_decomp
for v in plain asan; do
  case $v in
    plain) san="-O2"; asan_apple="-O2" ;;
    # Apple's files: ASan (their reads and writes), not UBSan (their predictor shifts negative
    # values left on any input, as every C decoder of this bitstream does)
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
          asan_apple="-O1 -g -fsanitize=address -fno-omit-frame-pointer" ;;
  esac
  obj="$out/obj-$v"
  rm -rf "$obj"
  mkdir -p "$obj"
  for f in $APPLE; do
    # shellcheck disable=SC2086
    clang -std=c11 -w -fwrapv -mmacosx-version-min=12.0 $asan_apple -c "$alac/$f" -o "$obj/${f%.c}.o"
  done
  for f in m4a alac; do
    # shellcheck disable=SC2086
    (cd "$root" && clang $WARN -mmacosx-version-min=12.0 $san -Ihelper -Icommon -I"$alac" -c "helper/$f.c" -o "$obj/$f.o")
  done
  # shellcheck disable=SC2086
  (cd "$root" && clang -std=c11 -Wall -Wextra -Werror -mmacosx-version-min=12.0 $san -Ihelper -Icommon -Ibridge \
    helper/tests/win-alac.c helper/big.c common/plat_mac.c "$obj"/*.o \
    -framework AudioToolbox -framework CoreFoundation -framework Accelerate -o "$out/win-alac-$v")
  echo "== win-alac ($v)"
  if cat "$scratch/manifest.txt" "$reference/manifest.txt" | "$out/win-alac-$v" > "$out/win-alac-$v.txt" 2>&1; then r=ok; else r=FAIL; fails=$((fails + 1)); fi
  grep -v '^ok  ' "$out/win-alac-$v.txt" || true
  echo "$r   win-alac ($v): $(tail -1 "$out/win-alac-$v.txt") (every line: $out/win-alac-$v.txt)"
done

# the Windows build of our two files: no warnings (Apple's with -w, as the helper's build has them)
for arch in x64 arm64; do
  ok=1
  for f in m4a alac; do
    (cd "$root" && sh scripts/win-cc.sh "$arch" -Wall -Wextra -Werror -Wno-unused-command-line-argument -Ihelper -Icommon -I"$alac" \
      -c "helper/$f.c" -o "$out/$f-$arch.o") || ok=0
  done
  for f in $APPLE; do
    (cd "$root" && sh scripts/win-cc.sh "$arch" -w -fwrapv -Wno-unused-command-line-argument -c "$alac/$f" -o "$out/${f%.c}-$arch.o") || ok=0
  done
  if [ "$ok" = 1 ]; then echo "ok   Windows $arch: m4a.c, alac.c (-Werror) and Apple's ALAC files compile"
  else echo "FAIL Windows $arch: a compile failed"; fails=$((fails + 1)); fi
done
[ "$fails" = 0 ] || exit 1
