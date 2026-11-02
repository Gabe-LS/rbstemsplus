#!/bin/bash
# The unplug test, on a SACRIFICIAL USB stick formatted exFAT (never the
# owner's music drive): the bridge writing and pruning on it, the stick pulled out, then put
# back: what macOS's check says, and what the bridge does with what's left.
#
#   bridge/tests/fs/unplug.sh VOLUME [ROUNDS]       ROUNDS: default 5
#
# Interactive: it says when to pull the stick out and when to put it back. To show the stick may
# be damaged, create an empty file named RBSP-UNPLUG-SCRATCH at its root first. Refuses any other
# volume, this Mac's own disk and a volume named GLS. The results go to unplug-<date>.log in the
# current folder, for the test log. Outside a sandbox. SIMULATE=IMAGE (a disk image
# attached at VOLUME) detaches and attaches it instead, to check the script itself: a forced
# detach isn't an unplug (the kernel still finishes what it can).
set -u
VOL="${1:-}" ROUNDS="${2:-5}"
here="$(cd "$(dirname "$0")" && pwd)"
die() { echo "unplug: $*" >&2; exit 1; }
[ -n "$VOL" ] && [ -d "$VOL" ] || die "usage: $0 VOLUME [ROUNDS]"
VOL="$(cd "$VOL" && pwd -P)"
[ "$(basename "$VOL")" != GLS ] || die "never on GLS (the owner's music drive)"
[ "$(stat -f %d "$VOL")" != "$(stat -f %d /)" ] && [ "$(stat -f %d "$VOL")" != "$(stat -f %d "$HOME")" ] || die "$VOL is on this Mac's own disk"
[ -f "$VOL/RBSP-UNPLUG-SCRATCH" ] || die "no $VOL/RBSP-UNPLUG-SCRATCH: create it to show this stick may be damaged"
fs="$(diskutil info "$VOL" | awk -F': *' '/File System Personality/ {print $2; exit}')"
case "$fs" in *ExFAT*|*exFAT*) ;; *) echo "note: $VOL is $fs, not exFAT" ;; esac
out="$PWD/unplug-$(date +%Y%m%d-%H%M).log"
note() { echo "$*" | tee -a "$out"; }
speak() { echo ">>> $*"; /usr/bin/say "$*" 2> /dev/null & }   # aloud: the person is at the Mac, not the log

S="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/rbsp-unplug.XXXXXX")" && pwd -P)"
trap 'rm -rf "$S"' EXIT
LIB=libonnxruntime.1.18.0.dylib
real="/Library/Application Support/rbstemsplus/ort/$LIB"
[ -f "$real" ] || die "no $real (install Stems Cache first)"
mkdir -p "$S/ort" "$S/pioneer" "$S/model" "$S/home/Library/Application Support/rbstemsplus"
cp "$real" "$S/pioneer/$LIB" && chmod 444 "$S/pioneer/$LIB" && ln -s "$S/pioneer/$LIB" "$S/ort/$LIB"
"$here/../../build.sh" --test "$S/ort/$LIB" "$S/bridge" > "$S/build.log" 2>&1 || die "the test bridge didn't build"
clang -O1 -DL=485100 -I"$here/../.." "$here/../harness.c" "$S/bridge/$LIB" -Wl,-rpath,"$S/bridge" -o "$S/harness" || die "harness"
python3 -I "$here/../make_model.py" "$S/model/hdemucs.onnx" || die "model"

ID=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F
loc="$VOL/rbsp-unplug/RB Stems Plus Cache.noindex"
mkdir -p "$loc"
printf 'id=%s\nformat=rbstems-v1\n' "$ID" > "$loc/.rbstemsplus-cache"
# a small limit: prune runs after nearly every write, so it is running when the stick goes too
printf 'cache_dir="%s"\ncache_id=%s\nmax_gb=0.1\n' "$loc" "$ID" > "$S/home/Library/Application Support/rbstemsplus/config.ini"
logf="$S/home/Library/Logs/rbstemsplus/bridge.log"
mkdir -p "$(dirname "$logf")"
H() { env HOME="$S/home" RBSTEMS_TEST_HOME="$S/home" RBSTEMS_HARNESS_CACHE="$loc" RBSTEMS_HARNESS_READY=120 "$S/harness" run "$S/model/hdemucs.onnx" "$@"; }
# unrelated audio each (a miss and a write each: never a near hit), for as long as a person may
# take to pull the stick out
chunks() { local i; for i in $(seq 3000); do printf 'n%d ' "$(( $1 * 5000 + i ))"; done; }

note "== unplug test on $VOL ($fs), $ROUNDS rounds, $(date)"
for r in $(seq "$ROUNDS"); do
  note "-- round $r"
  : > "$logf"
  H $(chunks "$r") > "$S/run.out" 2>&1 &
  hp=$!
  sleep 8
  if [ -n "${SIMULATE:-}" ]; then hdiutil detach -quiet -force "$VOL"; else speak "Round $r. Pull the stick out now."; fi
  while [ -d "$loc" ]; do sleep 0.2; kill -0 $hp 2> /dev/null || { note "   the harness finished before the stick was pulled out"; break; }; done
  note "   pulled out at $(date +%T); the harness keeps running"
  wait $hp; rc=$?
  note "   harness exit $rc ($(grep -c ' model ' "$S/run.out") chunks separated, crash: $([ $rc -gt 1 ] && echo YES || echo no))"
  note "   before the pull: $(grep -c '^.\{21\}miss ' "$logf") misses, $(grep -c 'limits:' "$logf") prunes, $(grep -o 'removed [0-9]* unused for [^,]*, [0-9]* over' "$logf" | awk '{s+=$(NF-1)} END {print s+0}') entries evicted, $(grep -c 'chunks are waiting' "$logf") times the writer was full"
  note "   bridge: $(grep -o 'not available ([^)]*)' "$logf" | head -1); losses logged: $(grep -c 'not available' "$logf")"
  if [ -n "${SIMULATE:-}" ]; then hdiutil attach -quiet -nobrowse "$SIMULATE"; else sleep 5; speak "Put the stick back in."; fi
  while [ ! -d "$VOL" ]; do sleep 0.5; done
  sleep 3
  note "   mounted again: $(mount | grep -F " on $VOL " | sed 's/.*(\(.*\))/\1/')"
  note "   macOS's check: $(diskutil verifyVolume "$VOL" 2>&1 | tail -1)"
  diskutil mount "$VOL" > /dev/null 2>&1
  [ -d "$loc" ] || { note "   our folder is gone after the check"; mkdir -p "$loc"; printf 'id=%s\nformat=rbstems-v1\n' "$ID" > "$loc/.rbstemsplus-cache"; }
  note "   left: $(find "$loc" -name '*.tmp' | wc -l | tr -d ' ') temp files, $(find "$loc" -name '._*' | wc -l | tr -d ' ') companions, $(find "$loc" -name '*.flac' ! -name '._*' | wc -l | tr -d ' ') FLACs, $(find "$loc" -name '*.desc' ! -name '._*' | wc -l | tr -d ' ') summaries"
  : > "$logf"
  H a wait a > "$S/after.out" 2>&1
  note "   a new run: $(tr '\n' ' ' < "$S/after.out")| $(grep -o 'cache: using[^(]*\|not available ([^)]*)' "$logf" | head -1) | refused/unreadable: $(grep -c 'refused\|unreadable' "$logf")"
done
note "== done: $out"
[ -n "${SIMULATE:-}" ] || speak "The unplug test is done."
