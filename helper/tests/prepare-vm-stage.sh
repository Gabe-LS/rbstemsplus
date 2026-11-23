#!/bin/sh
# What tests/win/suites/prepare.suite reads from the VM's share (the main checkout's build/win, as
# \\Mac\win): build/win/prepare/ replaced whole (helper.md "T17's first checks", the VM half):
#   release/rbsp-prepare.exe    helper/build-win.sh's release build
#   timing/rbsp-prepare.exe     helper/build-win.sh --test-timing (its track lines end in power= ep=)
#   research/rbsp-prepare.exe   RBSP_RESEARCH=1 helper/build-win.sh (--keys)
#   model/stemsplus-model.onnx  our model (0cc50d87..., checked)
#   corpus/                     four 44.1 kHz corpus tracks (WAV, FLAC, MP3, AAC) and a 48 kHz FLAC,
#                               each checked against tests/perf/corpus-v1.sha256 or win/SHA256SUMS
#   rb-keys.txt                 the first 12 hex digits of every cache key rekordbox itself computed
#                               with Pioneer's model on the owner's PC (PC session 1: the bridge log's
#                               hits and misses, and the cache's .flac names): --keys with Pioneer's
#                               model must name only these
#   commit.txt                  the checkout's HEAD when staged
#
#   helper/tests/prepare-vm-stage.sh --release DIR --timing DIR --research DIR --corpus DIR --evidence DIR
#     --corpus: a folder with v1/ and win/ (tests/rekordbox/win/pc-session.sh stage's --corpus form);
#     --evidence: PC session 1's folder (bridge.log, cache-files.tsv)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
usage() { echo "usage: $0 --release DIR --timing DIR --research DIR --corpus DIR --evidence DIR" >&2; exit 2; }
rel="" tim="" res="" corpus="" ev=""
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || usage
  case "$1" in
    --release) rel="$2" ;; --timing) tim="$2" ;; --research) res="$2" ;;
    --corpus) corpus="$2" ;; --evidence) ev="$2" ;; *) usage ;;
  esac
  shift 2
done
[ -n "$rel" ] && [ -n "$tim" ] && [ -n "$res" ] && [ -n "$corpus" ] && [ -n "$ev" ] || usage
die() { echo "prepare-vm-stage: $*" >&2; exit 1; }
share="$(cd "$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win"
[ -d "$share" ] || die "no share folder $share"
for b in "$rel" "$tim" "$res"; do [ -f "$b/rbsp-prepare.exe" ] || die "no $b/rbsp-prepare.exe (helper/build-win.sh)"; done
grep -aqF 'rbsp-timing track' "$tim/rbsp-prepare.exe" || die "$tim isn't a --test-timing build"
grep -aqF -- '--keys' "$res/rbsp-prepare.exe" || die "$res isn't an RBSP_RESEARCH=1 build"
if grep -aqF 'rbsp-timing' "$rel/rbsp-prepare.exe" || grep -aqF -- '--keys' "$rel/rbsp-prepare.exe"; then die "$rel isn't a release build"; fi
model="$share/dist-test/stemsplus-model.onnx"
[ "$(shasum -a 256 "$model" 2> /dev/null | cut -d' ' -f1)" = 0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772 ] ||
  die "$model isn't our model (0cc50d87...)"
[ -f "$ev/bridge.log" ] && [ -f "$ev/cache-files.tsv" ] || die "--evidence: $ev needs bridge.log and cache-files.tsv"
d="$share/prepare"
rm -rf "${d:?}"
mkdir -p "$d/release" "$d/timing" "$d/research" "$d/model" "$d/corpus"
cp "$rel/rbsp-prepare.exe" "$d/release/"
cp "$tim/rbsp-prepare.exe" "$d/timing/"
cp "$res/rbsp-prepare.exe" "$d/research/"
cp "$model" "$d/model/"
# name, its folder in the corpus, its checksum list
for f in v1/20-wav-16-30s.wav win/w16-flac-longpath.flac v1/02-mp3-vbr2.mp3 v1/05-aac-128.m4a win/w14-flac-mono-48k.flac; do
  n="${f#*/}"
  case "$f" in v1/*) sums="$root/tests/perf/corpus-v1.sha256" ;; *) sums="$corpus/win/SHA256SUMS" ;; esac
  want="$(awk -v n="$n" '{ f = $2; sub(/^\*/, "", f); sub(/.*\//, "", f); if (f == n) print $1 }' "$sums")"
  [ -n "$want" ] || die "$n isn't in $sums"
  [ "$(shasum -a 256 "$corpus/$f" | cut -d' ' -f1)" = "$want" ] || die "$corpus/$f doesn't match $sums"
  cp "$corpus/$f" "$d/corpus/"
done
{ sed -nE 's/.* (miss|hit) +([0-9a-f]{12}) \(.*/\2/p' "$ev/bridge.log"
  sed -nE 's/.*\\([0-9a-f]{12})[0-9a-f]{52}\.flac\t.*/\1/p' "$ev/cache-files.tsv"; } | sort -u > "$d/rb-keys.txt"
[ -s "$d/rb-keys.txt" ] || die "no keys in $ev"
git -C "$root" rev-parse HEAD > "$d/commit.txt"
echo "staged into $d ($(wc -l < "$d/rb-keys.txt" | tr -d ' ') rekordbox keys)"
