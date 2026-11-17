#!/bin/bash
# mp3.c's Windows path (helper.md T8), on this Mac: rb_mp3 built with RBSP_WIN_DECODE on
# common/plat_mac.c against rb_mp3 as helper/build.sh compiles it, and mp3_probe (the header-only
# MP3 probe) against rekordbox's frame map and the decoded length; Apple clang, rekordbox's own
# libmpg123. For each fixture (made in a scratch folder by win-mp3-fixtures.py, never committed):
#  (a) the Windows path's rb_mp3 output is the Mac's, bit for bit (or the same refusal);
#  (b) mp3_probe: refuses exactly when the frame map is empty (with rb_mp3's sentence), rate and
#      channels as rb_mp3 takes them, seconds within 1% or 0.1 s (the larger) of the decoded
#      length, no read past the size;
#  (c) AudioToolbox's probe (decode.c's, the Mac estimate's): where it and mp3_probe differ, a
#      "note" line (reported, not a failure);
#  (d) the probe and the decode again under ASan+UBSan, with fuzzed variants of each fixture, and
#      once more with the probe's first window at 8 KB (MP3_PROBE_WIN).
# Also the Windows compile of mp3.c (scripts/win-cc.sh, x64 and arm64, -Werror).
#
#   helper/tests/win-mp3.sh [--long] [OUT]   OUT default $TMPDIR/rbsp-win-mp3; --long adds 6-minute
#                                            files (the probe's cost on a long file)
# RBSP_REKORDBOX: rekordbox's app (default /Applications/rekordbox 7/rekordbox.app). Exits 1 on a FAIL.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
long=
if [ "${1:-}" = "--long" ]; then long=--long; shift; fi
out="${1:-${TMPDIR:-/tmp}/rbsp-win-mp3}"
RBAPP="${RBSP_REKORDBOX:-/Applications/rekordbox 7/rekordbox.app}"
lib="$RBAPP/Contents/MacOS/libmpg123.0.dylib"
[ -f "$lib" ] || { echo "no rekordbox libmpg123 at $lib (RBSP_REKORDBOX)"; exit 1; }
mkdir -p "$out/ref" "$out/win" "$out/asan"
out="$(cd "$out" && pwd)"
pass=0 fail=0 notes=0
ok() { echo "ok	$1"; pass=$((pass + 1)); }
bad() { echo "FAIL	$1"; fail=$((fail + 1)); }
note() { echo "note	$1"; notes=$((notes + 1)); }

MAC="-arch arm64 -mmacosx-version-min=12.0 -Wall -Wextra"
H="$root/helper" C="$root/common"
# the reference: mp3.c as helper/build.sh compiles it, decode.c's AudioToolbox probe
clang -O2 $MAC -I"$H" -I"$root/common" -I"$root/bridge" "$H/tests/win-mp3.c" "$H/mp3.c" "$H/aac.c" "$H/mp4.c" "$H/big.c" "$H/resample.c" \
  -framework AudioToolbox -framework CoreFoundation -framework Accelerate -o "$out/mp3-ref" || exit 1
# the Windows path on plat_mac.c, plain and with ASan+UBSan
clang -O2 $MAC -DRBSP_WIN_DECODE -I"$H" -I"$C" "$H/tests/win-mp3.c" "$C/plat_mac.c" "$H/big.c" \
  -framework CoreFoundation -framework Accelerate -o "$out/mp3-win" || exit 1
# (and with 8 KB first windows: more of them end inside a tag or a frame)
for v in asan asan8k; do
  [ $v = asan8k ] && w=-DMP3_PROBE_WIN=8192 || w=
  clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined $MAC -DRBSP_WIN_DECODE $w \
    -I"$H" -I"$C" "$H/tests/win-mp3.c" "$C/plat_mac.c" "$H/big.c" -framework CoreFoundation -framework Accelerate \
    -o "$out/mp3-$v" || exit 1
done
for a in x64 arm64; do
  if "$root/scripts/win-cc.sh" "$a" -Wall -Wextra -Werror -Wno-unused-command-line-argument -DRBSP_WIN_DECODE \
       -I"$H" -I"$C" -c "$H/mp3.c" -o "$out/mp3-$a.o"; then ok "windows compile ($a)"; else bad "windows compile ($a)"; fi
done

rm -f "$out"/fx/*.mp3                    # this run's fixtures only (the music WAVs are kept)
python3 -I "$here/win-mp3-fixtures.py" "$out/fx" $long || { echo "fixtures failed"; exit 1; }

tolerance() { awk -v s="$1" 'BEGIN { t = s * 0.01; if (t < 0.1) t = 0.1; print t }'; }
for f in "$out"/fx/*.mp3; do
  name="$(basename "$f")"
  echo "== $name"
  rdec="$("$out/mp3-ref" dec "$lib" "$f" "$out/ref/$name.f32")"
  wout="$("$out/mp3-win" check "$lib" "$f" "$out/win/$name.f32")"
  wst=$?
  printf '%s\n' "$wout" | grep -E '^(ok|FAIL|note)	' | sed 's/^/  /'
  pass=$((pass + $(printf '%s\n' "$wout" | grep -c '^ok	')))
  fail=$((fail + $(printf '%s\n' "$wout" | grep -c '^FAIL	')))
  notes=$((notes + $(printf '%s\n' "$wout" | grep -c '^note	')))
  [ $wst -le 1 ] || bad "$name: the Windows path's check exited $wst"
  wdec="$(printf '%s\n' "$wout" | grep '^dec	')"
  # (a)
  if [ "$rdec" != "$wdec" ]; then
    bad "$name: rb_mp3 Mac [$rdec] vs Windows path [$wdec]"
  elif [ "${rdec#dec	ok}" != "$rdec" ]; then
    if cmp -s "$out/ref/$name.f32" "$out/win/$name.f32"; then ok "$name: rb_mp3 bit-identical (${rdec##*	} frames)"; else bad "$name: rb_mp3 output differs"; fi
  else
    ok "$name: rb_mp3 refuses on both (${rdec##*	})"
  fi
  rm -f "$out/ref/$name.f32" "$out/win/$name.f32"
  # (c) the Mac estimate's probe against mp3_probe
  at="$("$out/mp3-ref" at "$f")"
  pr="$(printf '%s\n' "$wout" | grep '^probe	')"
  IFS='	' read -r _ ast arate ach asec <<< "$at"
  IFS='	' read -r _ pst prate pch psec papprox preads pbytes pus <<< "$pr"
  if [ "$pst" = ok ]; then echo "  probe: $prate Hz $pch ch $psec s approx=$papprox, $preads reads, $pbytes bytes, $pus us"; fi
  if [ "$ast" != "$pst" ]; then
    note "$name: AudioToolbox's probe $ast, mp3_probe $pst${rdec:+ (rb_mp3: ${rdec#dec	})}"
  elif [ "$ast" = ok ]; then
    [ "$arate" = "$prate" ] && [ "$ach" = "$pch" ] || note "$name: AudioToolbox $arate Hz $ach ch, mp3_probe $prate Hz $pch ch"
    t="$(tolerance "$psec")"
    awk -v a="$asec" -v p="$psec" -v t="$t" 'BEGIN { d = a - p; if (d < 0) d = -d; exit !(d > t) }' \
      && note "$name: AudioToolbox's estimate $asec s, mp3_probe $psec s"
  fi
  # (d)
  for v in asan asan8k; do
    if "$out/mp3-$v" check "$lib" "$f" "$out/asan/$name.f32" > "$out/asan/$name.$v.log" 2>&1; then
      ok "$name: $v check"
    elif grep -q 'Sanitizer' "$out/asan/$name.$v.log"; then
      bad "$name: $v: $(grep -m1 'Sanitizer' "$out/asan/$name.$v.log")"
    else
      bad "$name: $v check: $(grep -m2 '^FAIL' "$out/asan/$name.$v.log" | tr '\n\t' '  ')"
    fi
    rm -f "$out/asan/$name.f32"
    fz="$("$out/mp3-$v" fuzz "$f" 60 "$(cksum <<< "$name" | cut -d' ' -f1)" "$out/asan/fuzz.mp3" 2> "$out/asan/$name.$v.fuzz.log")"
    fst=$?
    if [ $fst -eq 0 ]; then ok "$name: $v fuzz: ${fz#*fuzz	}"; else bad "$name: $v fuzz (exit $fst): ${fz#*fuzz	} $(grep -m3 -E 'Sanitizer|fuzz [0-9]' "$out/asan/$name.$v.fuzz.log" | tr '\n' ' ')"; fi
  done
done
echo "== total: $pass ok, $fail FAIL, $notes notes"
[ $fail -eq 0 ]
