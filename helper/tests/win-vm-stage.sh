#!/bin/sh
# What tests/win/suites/helper.suite reads from the VM's share (the main checkout's build/win, as
# \\Mac\win): build/win/helper/ replaced whole with helper/build-win.sh's test exes and
# rbsp-decode-dump.exe and rbsp-aac-mf-probe.exe, the AAC fixtures (helper/aac/: aac-mf-fixtures.sh's
# .m4a and .mp4 files, made into build/aac-mf-fix with their .wav sources, which stay here), and the UNC fixture (helper.md T13, the UNC ruling): one WAV under
# helper/unc/ in folders with Unicode names, its path over 260 characters as the VM sees it.
#
#   helper/tests/win-vm-stage.sh [BUILD]      BUILD: helper/build-win.sh's folder (default build/helper-win)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
src="${1:-$root/build/helper-win}"
share="$(cd "$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win"
[ -d "$share" ] || { echo "no share folder $share" >&2; exit 1; }
d="$share/helper"
rm -rf "${d:?}"
mkdir -p "$d"
for e in rbsp-win-readable-test.exe rbsp-win-stop-test.exe rbsp-win-library-test.exe rbsp-win-rules-test.exe rbsp-decode-dump.exe rbsp-aac-mf-probe.exe; do
  [ -f "$src/$e" ] || { echo "missing $src/$e (helper/build-win.sh)" >&2; exit 1; }
  cp "$src/$e" "$d/"
done
fix="$root/build/aac-mf-fix"
if [ ! -f "$fix/ff-video.mp4" ] || [ ! -f "$fix/ff-lc-22k.m4a" ] || [ ! -f "$fix/ff-hop-boundary.m4a" ] ||
   [ ! -f "$fix/ff-lc-32k-mono-mp4.mp4" ]; then
  "$here/aac-mf-fixtures.sh" "$fix" > /dev/null   # afconvert's AAC encoder: outside the Mac sandbox
fi
mkdir -p "$d/aac"
cp "$fix"/*.m4a "$fix"/*.mp4 "$d/aac/"
python3 -I - "$d/unc" << 'PY'
import math, os, sys, wave
d1 = "Ünïcödé Tracks 日本語 " + "x" * 120
d2 = "Ålbum ☃ " + "y" * 120
folder = os.path.join(sys.argv[1], d1, d2)
os.makedirs(folder)
w = wave.open(os.path.join(folder, "ä tëst ☃.wav"), "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
fr = bytearray()
for i in range(44100):
    v = int(12000 * math.sin(2 * math.pi * 440 * i / 44100))
    fr += v.to_bytes(2, "little", signed=True) + (-v).to_bytes(2, "little", signed=True)
w.writeframes(bytes(fr)); w.close()
with open(os.path.join(sys.argv[1], "path.txt"), "w", encoding="utf-8") as f:
    f.write("unc\\" + d1 + "\\" + d2 + "\\ä tëst ☃.wav")
PY
echo "staged into $d"
