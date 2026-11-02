#!/bin/bash
# The performance gate: the bridge's own timings with a chosen location on a
# DEDICATED drive (never the owner's music drive), with rekordbox-size chunks (485,100 samples,
# about 2.4 MB per entry), cold (the volume unmounted and mounted again) unless said otherwise.
#
#   bridge/tests/fs/perfgate.sh VOLUME [ENTRIES]     ENTRIES: 10000 (default) or 100000
#
# VOLUME is the drive's mount point, e.g. /Volumes/PERF. To show the drive may be filled and
# emptied, create an empty file named RBSP-PERF-SCRATCH at its root first (touch
# /Volumes/PERF/RBSP-PERF-SCRATCH). The script refuses any other volume, this Mac's own disk and a
# volume named GLS. It writes only in VOLUME/rbsp-perf (deleted at the end) and a scratch folder.
# Needs: a copy of Pioneer's ONNX Runtime (installed Stems Cache's), python3, about 3.4 GB per
# 1000 entries free on the drive. Outside a sandbox (it unmounts and mounts the drive).
# REUSE=1 keeps the entries for the next run (and uses those of the last run with REUSE=1);
# then nothing is cleared.
#
# What it reports (thresholds written down before running):
#   populate       writing the entries, as the bridge does (temp file, rename): MB/s
#   index (cold)   rekordbox's session to its summaries loaded (near hits wait for this)
#   hit (cold)     one exact hit, read and served, right after a cold start
#   miss + save    one miss's entry written to the drive (the writer queue)
#   prune (cold)   the limits' walk of every entry
#   memory         the harness's peak memory with the index loaded, against an empty one
#   clear          deleting every entry (rm -rf, a stand-in for the app's descriptor walk)
set -u
VOL="${1:-}" N="${2:-10000}"
here="$(cd "$(dirname "$0")" && pwd)"
die() { echo "perfgate: $*" >&2; exit 1; }
[ -n "$VOL" ] && [ -d "$VOL" ] || die "usage: $0 VOLUME [ENTRIES]"
VOL="$(cd "$VOL" && pwd -P)"
name="$(basename "$VOL")"
[ "$(stat -f %d "$VOL")" != "$(stat -f %d /)" ] && [ "$(stat -f %d "$VOL")" != "$(stat -f %d "$HOME")" ] || die "$VOL is on this Mac's own disk"
[ "$name" != GLS ] || die "never on GLS (the owner's music drive)"
[ -f "$VOL/RBSP-PERF-SCRATCH" ] || die "no $VOL/RBSP-PERF-SCRATCH: create it to show this drive may be used"
dev="$(diskutil info "$VOL" | awk -F': *' '/Device Identifier/ {print $2; exit}')"
[ -n "$dev" ] || die "diskutil doesn't know $VOL"

S="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/rbsp-perf.XXXXXX")" && pwd -P)"
if [ "${REUSE:-}" = 1 ]; then trap 'rm -rf "$S"' EXIT; else trap 'rm -rf "$S" "$VOL/rbsp-perf"' EXIT; fi
LIB=libonnxruntime.1.18.0.dylib
real="/Library/Application Support/rbstemsplus/ort/$LIB"
[ -f "$real" ] || die "no $real (install Stems Cache first)"
mkdir -p "$S/ort" "$S/pioneer" "$S/model"
cp "$real" "$S/pioneer/$LIB" && chmod 444 "$S/pioneer/$LIB" && ln -s "$S/pioneer/$LIB" "$S/ort/$LIB"
"$here/../../build.sh" --test "$S/ort/$LIB" "$S/bridge" > "$S/build.log" 2>&1 || { tail "$S/build.log"; die "the test bridge didn't build"; }
clang -O1 -DL=485100 -I"$here/../.." "$here/../harness.c" "$S/bridge/$LIB" -Wl,-rpath,"$S/bridge" -o "$S/harness" || die "harness"
python3 -I "$here/../make_model.py" "$S/model/hdemucs.onnx" || die "model"
m="$S/model/hdemucs.onnx"

ID=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F
loc="$VOL/rbsp-perf/RB Stems Plus Cache.noindex"
reused=
[ "${REUSE:-}" = 1 ] && [ -f "$VOL/rbsp-perf/populated-$N" ] && reused=1
[ -n "$reused" ] || rm -rf "$VOL/rbsp-perf"
mkdir -p "$loc" "$S/home/Library/Application Support/rbstemsplus"
[ -n "$reused" ] || printf 'id=%s\nformat=rbstems-v1\n' "$ID" > "$loc/.rbstemsplus-cache"
printf 'cache_dir="%s"\ncache_id=%s\nmax_gb=100000\nmax_days=36500\n' "$loc" "$ID" > "$S/home/Library/Application Support/rbstemsplus/config.ini"
logf="$S/home/Library/Logs/rbstemsplus/bridge.log"
mkdir -p "$(dirname "$logf")"
now() { python3 -c 'import time; print(time.time())'; }
H() { env HOME="$S/home" RBSTEMS_TEST_HOME="$S/home" RBSTEMS_HARNESS_CACHE="$loc" RBSTEMS_HARNESS_READY=600 "$S/harness" run "$m" "$@"; }
Hc() { RBSTEMS_HARNESS_WAIT_FOR="cache: using" H "$@"; }    # waits only for the location's check
# cold: unmounted and mounted again (forced if something, e.g. a Finder window, keeps it busy:
# the drive is a dedicated test drive)
remount() {
  { diskutil unmount "$VOL" > /dev/null 2>&1 || diskutil unmount force "$VOL" > /dev/null; } || die "couldn't unmount $VOL"
  # mounting at once after an unmount can fail while FSKit's exFAT driver still lets go (a stick
  # then dropped off the bus): wait, then try a few times
  local i
  for i in 1 2 3 4 5; do
    sleep 3
    diskutil mount "$dev" > /dev/null 2>&1 && [ -d "$VOL" ] && return 0
  done
  die "couldn't mount $VOL again (diskutil list: is the drive still there?)"
}
timed() { local t0 t1; t0=$(now); "$@" > "$S/out" 2>&1; t1=$(now); python3 -c "print('%.2f' % ($t1 - $t0))"; }
row() { printf '  %-16s %s\n' "$1" "$2"; }

echo "== $VOL ($(diskutil info "$VOL" | awk -F': *' '/File System Personality|Protocol|Solid State/ {printf "%s; ", $2}')), $N entries${reused:+ (reused)}"
if [ -z "$reused" ]; then
t_miss=$(timed H a wait)
grep -q "stored 1" "$S/out" || die "the first entry wasn't stored: $(cat "$S/out")"
row "miss + save" "${t_miss} s (one chunk: the model, then its entry written)"

t0=$(now)
python3 -I - "$loc" "$N" "$HOME/Library/Caches/rbstemsplus" << 'EOF' || die "populate"
import glob, hashlib, os, sys
loc, n, saved = sys.argv[1], int(sys.argv[2]), sys.argv[3]
model = os.path.dirname(os.path.dirname(glob.glob(os.path.join(loc, "*", "*", "*.flac"))[0]))
# the size of a real entry when this Mac has one (the harness's test audio compresses less)
real = [f for f in glob.glob(os.path.join(saved, "*", "*", "*.flac")) if os.path.exists(f[:-5] + ".desc")]
flac = real[0] if real else glob.glob(os.path.join(loc, "*", "*", "*.flac"))[0]
print("  template: %s entry, %.2f MB" % ("a real saved" if real else "the harness's", os.path.getsize(flac) / 2**20))
fb, db = open(flac, "rb").read(), open(flac[:-5] + ".desc", "rb").read()
for i in range(n):
    k = hashlib.sha256(b"perf%d" % i).hexdigest()
    d = os.path.join(model, k[:2]); os.makedirs(d, exist_ok=True)
    for ext, b in ((".flac", fb), (".desc", db)):
        t = os.path.join(d, k + ext + ".1.tmp")
        with open(t, "xb") as o: o.write(b)
        os.rename(t, os.path.join(d, k + ext))
EOF
t1=$(now)
mb=$(du -sm "$loc" | cut -f1)
row "populate" "$(python3 -c "print('%.0f s, %.1f MB/s' % ($t1 - $t0, $mb / ($t1 - $t0)))") ($mb MB on disk)"
[ "${REUSE:-}" = 1 ] && touch "$VOL/rbsp-perf/populated-$N"
fi

: > "$logf"; remount; t_ready=$(timed H)
row "index (cold)" "$t_ready s to the session's summaries line ($(grep -o 'summaries [a-z]*[: ]*[^(]*' "$logf" | tail -1))"
# an exact hit doesn't wait for the index: only for the location's check at launch
: > "$logf"; remount; t_start=$(timed Hc)
: > "$logf"; remount; t_hit=$(timed Hc a)
grep -q "a cache" "$S/out" || die "no hit: $(cat "$S/out")"
row "hit (cold)" "$(python3 -c "print('%.2f' % ($t_hit - $t_start))") s over a cold start to the location's check ($t_start s), (with near hits on, the index still loading)"
before=$(grep -c 'limits:' "$logf")
remount; H "sh=sleep 900" > /dev/null 2>&1 &              # alive until its prune has logged
hp=$!
for i in $(seq 900); do [ "$(grep -c 'limits:' "$logf")" -gt "$before" ] && break; sleep 1; done
pkill -P $hp 2> /dev/null; kill $hp 2> /dev/null; wait $hp 2> /dev/null
p="$(python3 -I - "$logf" << 'EOF'
import sys, datetime
lines = open(sys.argv[1]).read().splitlines()
t = lambda l: datetime.datetime.strptime(l[:19], "%Y-%m-%d %H:%M:%S")
s = [l for l in lines if "  summaries " in l][-1]; p = [l for l in lines if "limits:" in l][-1]
print("%d s after the summaries line (log times, to the second; with near hits off the first walk of the drive, cold): %s" % ((t(p) - t(s)).total_seconds(), p[21:100]))
EOF
)"
row "prune (cold)" "$p"
rss_full=$( { /usr/bin/time -l env HOME="$S/home" RBSTEMS_TEST_HOME="$S/home" RBSTEMS_HARNESS_READY=900 "$S/harness" run "$m" > /dev/null; } 2>&1 | awk '/maximum resident/ {print $1}')
mv "$loc" "$loc.away"; mkdir -p "$loc"; cp "$loc.away/.rbstemsplus-cache" "$loc/"
rss_empty=$( { /usr/bin/time -l env HOME="$S/home" RBSTEMS_TEST_HOME="$S/home" RBSTEMS_HARNESS_READY=900 "$S/harness" run "$m" > /dev/null; } 2>&1 | awk '/maximum resident/ {print $1}')
rm -rf "$loc"; mv "$loc.away" "$loc"
row "memory" "$(( (rss_full - rss_empty) / 1048576 )) MB more with the index loaded"
if [ "${REUSE:-}" != 1 ]; then
  remount; t0=$(now); rm -rf "$loc"; t1=$(now)
  row "clear" "$(python3 -c "print('%.1f s' % ($t1 - $t0))") for $N entries"
fi
