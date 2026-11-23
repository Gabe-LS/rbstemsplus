#!/bin/sh
# Focused host checks of the Windows prepare command, using the real kept and manifest files.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/build"
scratch="$(mktemp -d "$root/build/prepare-host.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
cd "$root"
# Use the same starting path limit as mingw-w64 for the Windows-path host build.
cat > "$scratch/mingw-limits.h" <<'EOF'
#include <limits.h>
#undef PATH_MAX
#define PATH_MAX 260
EOF
compile() {
  clang -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations \
    -DRBSP_TEST_WIN_PREPARE -DRBSP_TEST_PREPARE_CACHE_AT -DRBSP_RESEARCH -DPATHS_WIN_FORM -Ihelper -Icommon "$@"
}
compile -include "$scratch/mingw-limits.h" -c helper/kept.c -o "$scratch/kept.o"
compile_prepare() {
  target=$1; shift
  compile "$@" helper/tests/prepare-host.c helper/manifest.c \
    "$scratch/kept.o" helper/readable.c common/entries.c common/plat_mac.c common/plat_helper.c common/path_rules.c \
    -framework CoreFoundation -framework Accelerate -framework Security -o "$scratch/$target"
}
compile_prepare prepare-release -URBSP_RESEARCH
compile_prepare prepare-host -DRBSP_TEST_FORMAT_RATES

reject_rate_flag() {
  binary=$1 expected=$2; shift 2
  rc=0
  "$scratch/$binary" --test-unproved-rates "$@" > "$scratch/rate.out" 2> "$scratch/rate.err" || rc=$?
  if [ "$rc" != 2 ] || ! grep -qF -- "$expected" "$scratch/rate.err"; then
    echo "FAIL $binary candidate rate flag ($*, exit $rc)" >&2
    cat "$scratch/rate.err" >&2
    return 1
  fi
}
reject_rate_flag prepare-release 'unknown option --test-unproved-rates'
for flag in --resample-proved --keys; do
  reject_rate_flag prepare-host '--test-unproved-rates requires --keys and --resample-proved' "$flag"
done
reject_rate_flag prepare-host '--keys --model MODEL.onnx' --keys --resample-proved
echo 'prepare-host candidate rate option: research prerequisites and release refusal passed'

mkdir "$scratch/kept"
until=$(( $(date +%s) + 3600 ))
printf 'rbstemsplus-kept 1\nuntil %s\n%064d %064d\n' "$until" 1 2 > "$scratch/before.kept"
cp "$scratch/before.kept" "$scratch/kept/set.kept"
touch "$scratch/track.wav"
run() {
  "$scratch/prepare-host" --model fixture --cache "$scratch/cache" --kept-dir "$scratch/kept" \
    --rekordbox 'C:\rekordbox' --ort 'C:\runtime.dll' \
    --keep set --until "$until" "$@"
}
rc=0
RBSP_TEST_LIST_ROW="$(printf '%s\nX' "$scratch/track.wav")"
RBSP_TEST_LIST_ROW="${RBSP_TEST_LIST_ROW%X}"
export RBSP_TEST_LIST_ROW
run --list fault-list > "$scratch/out" 2> "$scratch/err" || rc=$?
if [ "$rc" != 1 ] || ! grep -q "can't read fault-list" "$scratch/err" ||
   ! cmp -s "$scratch/before.kept" "$scratch/kept/set.kept" ||
   [ -s "$scratch/out" ] || [ -e "$scratch/kept/set.manifest" ]; then
  echo "FAIL list read error published the accepted prefix (exit $rc)"
  exit 1
fi
printf '%s' "$scratch/track.wav" > "$scratch/clean-list"
RBSP_TEST_BINARY_LIST=1 run --list "$scratch/clean-list" > "$scratch/out"
grep -q '"done":0' "$scratch/out"
grep -q '"result":"skipped"' "$scratch/out"
echo "prepare-host list: 2 passed, 0 failed"

passed=0 failed=0
check() {
  name="$1"; shift
  if "$@"; then
    echo "ok   $name"
    passed=$((passed + 1))
  else
    echo "FAIL $name"
    failed=$((failed + 1))
  fi
}
record="$scratch/kept/set.manifest"
RBSP_TEST_STEMS=none run "$scratch/track.wav" > "$scratch/out"
grep -q '"written":0.*"refused":2' "$scratch/out"
check "a fully refused track is incomplete" grep -Eq '^file [0-9]+ [0-9]+ 0 0 ' "$record"

# Simulate an empty complete record written by an earlier helper.
sed -E 's/^(file [0-9]+ [0-9]+) [01] 0 /\1 1 0 /' "$record" > "$scratch/old.manifest"
mv "$scratch/old.manifest" "$record"
run --estimate "$scratch/track.wav" > "$scratch/out"
check "a legacy empty record estimates two chunks to prepare" grep -q '"result":"prepare".*"entries":2' "$scratch/out"

RBSP_TEST_STEMS=partial run "$scratch/track.wav" > "$scratch/out"
grep -q '"written":1.*"refused":1' "$scratch/out"
check "a partly refused track is incomplete" grep -Eq '^file [0-9]+ [0-9]+ 0 1 ' "$record"

RBSP_TEST_STEMS=all run "$scratch/track.wav" > "$scratch/out"
check "the next run keeps the first chunk and retries the other" grep -q '"written":1,"already":1.*"refused":0' "$scratch/out"
check "a fully prepared track is complete" grep -Eq '^file [0-9]+ [0-9]+ 1 2 ' "$record"

# Its entries complete, but the probe now refuses it (a 48 kHz file without --resample-proved):
# the run refuses it with the estimate's line instead of reporting it prepared.
line='48000 Hz: this rekordbox version'"'"'s resampler isn'"'"'t proved yet, so RB Stems Plus doesn'"'"'t prepare files it resamples'
RBSP_TEST_PROBE_REFUSE="$line" run --estimate "$scratch/track.wav" > "$scratch/est"
RBSP_TEST_PROBE_REFUSE="$line" RBSP_TEST_STEMS=all run "$scratch/track.wav" > "$scratch/out"
check "an unflagged run refuses a complete 48 kHz track as the estimate does" sh -c '
  grep -qF "\"result\":\"skipped\",\"why\":\"$1\"" "$2" && grep -qF "\"result\":\"skipped\",\"why\":\"$1\"" "$3" &&
  ! grep -q "\"result\":\"prepared\"" "$3"' sh "$line" "$scratch/est" "$scratch/out"
unset RBSP_TEST_PROBE_REFUSE   # sh keeps an assignment made before a function call
RBSP_TEST_STEMS=all run "$scratch/track.wav" > "$scratch/out"
check "with the probe accepting it again, the complete track is reused" grep -q '"written":0,"already":2' "$scratch/out"
echo "prepare-host refusal: $passed passed, $failed failed"
test "$failed" = 0
python3 -I helper/tests/kept-cli-host.py "$scratch/prepare-host" "$scratch"

# Converting this integral AAC length to seconds and back lands just below the next chunk.
touch "$scratch/aac-estimate.m4a"
RBSP_TEST_AAC_ESTIMATE=1 run --estimate "$scratch/aac-estimate.m4a" > "$scratch/out"
if ! grep -Eq '"runs":25[,}]' "$scratch/out"; then
  echo "FAIL prepare-host AAC estimate: expected 25 runs"
  cat "$scratch/out"
  exit 1
fi
echo "prepare-host AAC estimate: 25 runs at the chunk boundary"
unset RBSP_TEST_AAC_ESTIMATE

# A one-frame shortage crossing the chunk boundary still keeps the first chunk for retry.
printf 'a' > "$scratch/aac-short.m4a"
export RBSP_TEST_AAC_LENGTH=short RBSP_TEST_STEMS=all
run --estimate "$scratch/aac-short.m4a" > "$scratch/est"
check "AAC sample-table estimate is provisional" grep -q '"result":"prepare".*"entries":2,.*"approximate":true' "$scratch/est"
run "$scratch/aac-short.m4a" > "$scratch/out"
check "short AAC reports incomplete after keeping one chunk" grep -q '"result":"skipped","written":1.*"why":"incomplete AAC decode"' "$scratch/out"
check "short AAC manifest stays incomplete" grep -Eq '^file [0-9]+ [0-9]+ 0 1 .*aac-short\.m4a$' "$record"
run --estimate "$scratch/aac-short.m4a" > "$scratch/est"
check "a short AAC manifest cannot become ready" grep -q '"result":"prepare".*"entries":2,.*"approximate":true' "$scratch/est"

# Old manifests have no evidence that the decoder reached the sample-table length.
sed -E 's/^(file [0-9]+ [0-9]+) 0 1 hz=44100 /\1 1 1 /' "$record" > "$scratch/old.manifest"
mv "$scratch/old.manifest" "$record"
run --estimate "$scratch/aac-short.m4a" > "$scratch/est"
check "legacy complete AAC is still provisional" grep -q '"result":"prepare".*"entries":2,.*"approximate":true' "$scratch/est"
run "$scratch/aac-short.m4a" > "$scratch/out"
check "legacy complete AAC has to be decoded again" grep -q '"result":"skipped","written":0,"already":1.*"why":"incomplete AAC decode"' "$scratch/out"
RBSP_TEST_AAC_LENGTH=complete run "$scratch/aac-short.m4a" > "$scratch/out"
check "full AAC decode verifies its manifest" grep -Eq '^file [0-9]+ [0-9]+ 2 2 .*aac-short\.m4a$' "$record"
run --estimate "$scratch/aac-short.m4a" > "$scratch/est"
check "verified complete AAC is ready" grep -q '"result":"ready","entries":2' "$scratch/est"
test "$failed" = 0
echo "prepare-host AAC length: provisional estimate, incomplete run, legacy retry and verified reuse passed"

unset RBSP_TEST_AAC_LENGTH
export RBSP_TEST_TRACK_48K=1
printf 'r' > "$scratch/resampled track.flac"
track="$scratch/resampled track.flac"
run --resample-proved "$track" > "$scratch/out"
check "preparing a resampled track records 48 kHz" grep -Eq '^file [0-9]+ [0-9]+ 1 2 hz=48000 .*resampled track\.flac$' "$record"
run --resample-proved "$track" > "$scratch/out"
check "manifest reuse retains 48 kHz" sh -c '
  grep -q "\"how\":\"unchanged since it was prepared\"" "$1" &&
  grep -Eq "^file [0-9]+ [0-9]+ 1 2 hz=48000 .*resampled track\.flac$" "$2"
' sh "$scratch/out" "$record"
run --quick "$track" > "$scratch/quick"
run --estimate "$track" > "$scratch/est"
run "$track" > "$scratch/out"
for result in quick est out; do head -1 "$scratch/$result" > "$scratch/$result.row"; done
check "unproved quick agrees with full estimate and run" sh -c '
  cmp -s "$1" "$2" && cmp -s "$1" "$3" && grep -qF "\"result\":\"skipped\",\"why\":\"$4\"" "$1"
' sh "$scratch/quick.row" "$scratch/est.row" "$scratch/out.row" "$line"
(RBSP_TEST_PROBE_REFUSE="unexpected read in quick mode" run --quick --resample-proved "$track") > "$scratch/quick"
check "proved quick reuses the rate carried after refusal without probing" grep -q '"result":"ready","entries":2' "$scratch/quick"

sed 's/ hz=48000 / /' "$record" > "$scratch/old.manifest"
mv "$scratch/old.manifest" "$record"
run --quick --resample-proved "$track" > "$scratch/quick"
check "an old complete record without a rate is unchecked" grep -q '"result":"unchecked"' "$scratch/quick"
run --resample-proved "$track" > "$scratch/out"
check "rerunning a legacy record fills in the rate" grep -Eq '^file [0-9]+ [0-9]+ 1 2 hz=48000 .*resampled track\.flac$' "$record"
test "$failed" = 0
echo "prepare-host quick: rate round trip, Q15 refusal, no track probe, and legacy upgrade passed"
unset RBSP_TEST_TRACK_48K

printf x > "$scratch/fallback-first.wav"
printf y > "$scratch/fallback-second.wav"
RBSP_TEST_STEMS=all RBSP_TEST_BATCH_MODEL_ERROR=1 run "$scratch/fallback-first.wav" "$scratch/fallback-second.wav" > "$scratch/out"
if ! grep -q '"result":"stopped"' "$scratch/out" ||
   ! grep -q '"done":1,"stopped":"ONNX Runtime: CPU session creation failed"' "$scratch/out" ||
   grep -q 'fallback-second.wav' "$scratch/out"; then
  echo "FAIL prepare-host CPU session failure did not stop the batch"
  cat "$scratch/out"
  exit 1
fi
echo "prepare-host: failed CPU session stops the batch after one track"

rc=0
RBSP_TEST_STEMS=all RBSP_TEST_CHUNK_OOM=1 run "$scratch/fallback-second.wav" > "$scratch/out" 2> "$scratch/err" || rc=$?
if [ "$rc" != 1 ] || ! grep -qx 'out of memory' "$scratch/err" || [ -s "$scratch/out" ]; then
  echo "FAIL prepare-host chunk allocation failure (exit $rc)"
  exit 1
fi
echo "prepare-host: chunk allocation failure exits with an out-of-memory diagnostic"
