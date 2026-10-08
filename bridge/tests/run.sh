#!/bin/bash
# Headless tests of the bridge. Nothing outside a scratch folder is touched: the bridge is a test
# build (bridge/build.sh --test) that loads the "real" library from the scratch folder, its home
# (RBSTEMS_TEST_HOME, in place of the account's) points into it, and Pioneer's library is a
# read-only scratch copy.
#
#   bridge/tests/run.sh          KEEP=1 keeps the scratch folder
#
# Pioneer's ONNX Runtime 1.18 is copied from (first found, must be signed by team 6BRHGXQ6VU):
#   $RBSTEMS_REAL_ORT
#   /Library/Application Support/StemsPlus/ort/libonnxruntime.1.18.0.dylib       (the prototype's)
#   /Library/Application Support/rbstemsplus/ort/libonnxruntime.1.18.0.dylib
#   ~/Library/Pioneer/rekordbox-stems-backups/*/rekordbox.app/Contents/Frameworks/
# Without it, the tests that need it are skipped (and say so). The cache tests also need
# python3 (stdlib only) to write a tiny Demucs-shaped model. Each test runs for arm64, and for
# x86_64 under Rosetta when it's installed.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
LIB=libonnxruntime.1.18.0.dylib
TEAM=6BRHGXQ6VU

scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbstems-tests.XXXXXX")"
if [ "${KEEP:-}" = 1 ]; then echo "scratch: $scratch"; else trap 'rm -rf "$scratch"' EXIT; fi
mkdir -p "$scratch/ort" "$scratch/pioneer" "$scratch/fake" "$scratch/model"
target="$scratch/ort/$LIB"                  # the test bridge's "real" library: a symlink per test

passed=0 failures=0 skipped=()
pass() { passed=$((passed + 1)); }
fail() { failures=$((failures + 1)); echo "  FAIL [$arch] $*"; }
skip() { skipped+=("$*"); echo "  skip: $*"; }

# ---------------------------------------------------------------- setup

real=
cands=("${RBSTEMS_REAL_ORT:-}" "/Library/Application Support/StemsPlus/ort/$LIB" "/Library/Application Support/rbstemsplus/ort/$LIB")
for c in "$HOME"/Library/Pioneer/rekordbox-stems-backups/*/rekordbox.app/Contents/Frameworks/$LIB; do cands+=("$c"); done
for c in "${cands[@]}"; do
  [ -n "$c" ] && [ -f "$c" ] || continue
  grep -aq "rbstems bridge: " "$c" && continue                     # an installed bridge
  codesign -dv "$c" 2>&1 | grep -q "TeamIdentifier=$TEAM" || continue
  cp "$c" "$scratch/pioneer/$LIB" && chmod 444 "$scratch/pioneer/$LIB" && real="$scratch/pioneer/$LIB"
  echo "real library: a copy of $c"
  break
done
[ -n "$real" ] || echo "real library: none found (tests that need it are skipped)"

echo "building the test bridge"
"$here/../build.sh" --test "$target" "$scratch/bridge" > "$scratch/build.log" 2>&1 \
  || { cat "$scratch/build.log"; echo "the test bridge didn't build"; exit 1; }
bridge="$scratch/bridge/$LIB"

clang -dynamiclib -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 -Wall -Wextra -fvisibility=hidden \
  "$here/fake_ort.c" -install_name "@rpath/$LIB" -o "$scratch/fake/$LIB" || exit 1
fake="$scratch/fake/$LIB"

archs=(arm64)
if arch -x86_64 /usr/bin/true 2>/dev/null; then archs+=(x86_64); else skip "x86_64: Rosetta is not installed"; fi
for a in "${archs[@]}"; do
  clang -arch "$a" -mmacosx-version-min=12.0 -O1 -Wall -Wextra -I"$here/.." "$here/harness.c" "$bridge" \
    -Wl,-rpath,"$scratch/bridge" -o "$scratch/harness-$a" || exit 1
done

have_model=
if [ -n "$real" ] && command -v python3 > /dev/null; then
  python3 -I "$here/make_model.py" "$scratch/model/hdemucs.onnx" && cp "$scratch/model/hdemucs.onnx" "$scratch/model/other.onnx" \
    && have_model=1
  # shaped like rekordbox's own model (make_model.py --pioneer): good (its four stems' extra parts
  # cancel), bad (they don't: the stems don't add up to the chunk), loud (beyond the cache's range),
  # nan; each named hdemucs.onnx in its own folder, as the bridge wants
  for v in "good 0.64 -0.64 0.32 -0.32" "bad 0.64 0.64 0.64 0.64" "loud 200 -200 100 -100" "nan nan 0 0 0"; do
    set -- $v
    mkdir -p "$scratch/pmodel/$1" && python3 -I "$here/make_model.py" --pioneer "$2" "$3" "$4" "$5" "$scratch/pmodel/$1/hdemucs.onnx" || have_model=
  done
fi

# ---------------------------------------------------------------- helpers

point() { ln -sfn "$1" "$target"; }                      # what the test bridge finds as "real"
# a new home; the cache's disk is 1000 GB with 1000 GB free unless a test says otherwise (free="",
# disk="": measured; disk=0: its size can't be read); ours: a checksum the test bridge counts as one
# of our models; clone_fail: its options copy fails
fresh() { h="$scratch/home-$arch-$1"; th="$h"; free=1000; disk=1000; free_every=; statfs=; ours=; pioneer=; clone_fail=; rm -rf "$h"; mkdir -p "$h"; }
harness() {                                              # runs the harness with HOME=$h, the bridge's home $th
  local e=(env -u RBSTEMS_TEST_FREE_GB -u RBSTEMS_TEST_DISK_GB -u RBSTEMS_TEST_FREE_SECONDS -u RBSTEMS_TEST_STATFS
           -u RBSTEMS_TEST_OUR_MODEL -u RBSTEMS_TEST_PIONEER_MODEL -u RBSTEMS_TEST_CLONE_FAIL HOME="$h" RBSTEMS_TEST_HOME="$th")
  [ -n "$free" ] && e+=(RBSTEMS_TEST_FREE_GB="$free")
  [ -n "$disk" ] && e+=(RBSTEMS_TEST_DISK_GB="$disk")
  [ -n "$free_every" ] && e+=(RBSTEMS_TEST_FREE_SECONDS="$free_every")
  [ -n "$statfs" ] && e+=(RBSTEMS_TEST_STATFS=1)
  [ -n "$ours" ] && e+=(RBSTEMS_TEST_OUR_MODEL="$ours")
  [ -n "$clone_fail" ] && e+=(RBSTEMS_TEST_CLONE_FAIL=1)
  [ -n "$pioneer" ] && e+=(RBSTEMS_TEST_PIONEER_MODEL="$pioneer")
  if [ "$arch" = arm64 ]; then "${e[@]}" "$scratch/harness-arm64" "$@"
  else "${e[@]}" arch -x86_64 "$scratch/harness-x86_64" "$@"; fi
}
logf() { echo "$h/Library/Logs/rbstemsplus/bridge.log"; }
has() { grep -qF -- "$1" "$(logf)" 2>/dev/null; }        # the log has this text
count() { grep -cF -- "$1" "$(logf)" 2>/dev/null || true; }
expect() { if eval "$1"; then pass; else fail "$2"; fi; }
harness_ok() {                                           # the harness's own checks all pass
  local out
  out="$(harness "$@" 2>&1)"
  if [ $? -eq 0 ] && ! grep -q FAIL <<< "$out"; then pass; else fail "harness $1: $(grep FAIL <<< "$out" | head -3)"; fi
}
config() {                                               # writes config.ini (printf %b escapes)
  mkdir -p "$h/Library/Application Support/rbstemsplus"
  printf '%b' "$1" > "$h/Library/Application Support/rbstemsplus/config.ini"
}

# ---------------------------------------------------------------- the tests

loading() {
  if [ -n "$real" ]; then
    point "$real"; fresh load
    harness_ok load "$real"
    expect 'has "bridge loaded (real library 1.18."' "loads the 1.18 library and says so"
    expect 'has ", TEST BUILD)"' "the log names the test build"
    expect 'has "cache $h/Library/Caches/rbstemsplus, enabled=1, max_gb=20.0, max_days=60"' "cache path and defaults"
    expect '[ -d "$h/Library/Caches/rbstemsplus" ]' "the cache folder is created"
    expect '[ ! -e "$h/Library/Application Support/rbstemsplus" ]' "config.ini is not created by the bridge"
    expect '! has ERROR' "no ERROR in the log"
  else
    skip "loading Pioneer's library (no copy found)"
  fi

  point "$fake"; fresh passthrough
  harness_ok passthrough "$fake"
  expect 'has "bridge loaded (real library 1.17.0 from " && has "/fake/$LIB, NOT 1.18.x: passing everything through"' "pass-through is logged"

  point "$scratch/nonexistent/$LIB"; fresh missing
  harness_ok missing
  expect 'has "ERROR no real ONNX Runtime library"' "a missing library is logged"

  point "$bridge"; fresh itself
  harness_ok missing
  expect 'has "resolved to the bridge itself"' "the bridge refuses itself as the real library"

  # a library others can change is never loaded (the file, then its folder)
  mkdir -p "$scratch/writable/ort"
  cp "$fake" "$scratch/writable/ort/$LIB" && chmod 666 "$scratch/writable/ort/$LIB"
  point "$scratch/writable/ort/$LIB"; fresh writable
  harness_ok missing
  expect 'has "is not owned by root, or others can change it: not loaded" && has "ERROR no real ONNX Runtime library"' "a group- or world-writable library is refused"
  chmod 644 "$scratch/writable/ort/$LIB" && chmod 777 "$scratch/writable/ort"
  fresh writable-dir
  harness_ok missing
  expect 'has "its folder " && has "/writable/ort is not owned by root, or others can change it"' "a library in a folder others can change is refused"
  chmod 755 "$scratch/writable/ort"
}

# config.ini: content, expected "enabled=E, max_gb=G, max_days=D", number of "config: ignored" lines
config_case() {
  local want="enabled=$3, max_gb=$4, max_days=$5" ignored=$6 n
  fresh config
  config "$2"
  harness passthrough "$fake" > /dev/null 2>&1
  expect 'has "$want"' "config $1: want $want; log: $(grep -o 'enabled=.*' "$(logf)" | head -1)"
  n="$(count "config: ignored")"
  expect '[ "$n" = "$ignored" ]' "config $1: $n lines ignored, want $ignored"
}

configs() {
  point "$fake"
  fresh config; harness passthrough "$fake" > /dev/null 2>&1
  expect 'has "enabled=1, max_gb=20.0, max_days=60"' "config: none gives the defaults"
  config_case plain 'enabled=0\nmax_gb=5\nmax_days=7\n' 0 5.0 7 0
  config_case spaces '  enabled = off   # off for now\n max_gb =\t0.5\nmax_days= 30 # a month\n' 0 0.5 30 0
  config_case words 'enabled=No\n' 0 20.0 60 0
  config_case words-on 'enabled=0\nenabled=TRUE\n' 1 20.0 60 0
  config_case crlf 'enabled=no\r\nmax_days=30\r\n' 0 20.0 30 0
  config_case bom '\xEF\xBB\xBFenabled=0\n' 0 20.0 60 0
  config_case no-newline 'max_days=9' 1 20.0 9 0
  config_case comments '# enabled=0\n#max_gb=1\n\n   \n' 1 20.0 60 0
  config_case unknown 'config_version = 1\nfoo=1\nenabled\nmax_gbx=3\nmax_gb_extra=3\n' 1 20.0 60 0
  config_case case 'ENABLED=Off\nMax_GB=3\n' 0 3.0 60 0
  config_case quotes 'enabled = "no"\nmax_gb = \x277\x27\nmax_days="9" # q\n' 0 7.0 9 0
  config_case quoted-spaces 'max_gb=" 7 "\n' 1 20.0 60 1
  config_case sections 'max_days=9\n[watcher]\nenabled = no\nmax_gb = 1\n[ Cache ]\nmax_gb = 2\n[log]\nmax_days = 3\n' 1 2.0 9 0
  config_case section-cache '[cache]\nenabled = no\nmax_gb = 5\nmax_days = 6\n' 0 5.0 6 0
  config_case strategy-auto '[cache]\nmax_gb = auto\n' 1 20.0 60 1
  config_case last-wins 'max_gb=3\nmax_gb=4\n' 1 4.0 60 0
  config_case low-bounds 'max_gb=0.1\nmax_days=1\n' 1 0.1 1 0
  config_case high-bounds 'max_gb=100000\nmax_days=36500\n' 1 100000.0 36500 0
  config_case out-of-range 'max_gb=0.05\nmax_gb=100001\nmax_days=0\nmax_days=36501\nmax_gb=-1\n' 1 20.0 60 5
  config_case not-numbers 'max_gb=nan\nmax_days=inf\nmax_gb=20abc\nmax_gb=\nmax_days=ten\nenabled=maybe\nenabled=\n' 1 20.0 60 7
  # a comment longer than a read: its tail must not count as a line of its own
  config_case long-comment "#$(printf 'x%.0s' {1..254})max_gb=1\nmax_days=5\n" 1 20.0 5 0
  config_case long-line "max_gb=2 $(printf ' %.0s' {1..300})\nmax_days=5\n" 1 20.0 5 1
  fresh config; harness passthrough "$fake" > /dev/null 2>&1
  expect 'has "max_days=60, rekordbox_model=0, rebuild 1"' "config: rekordbox_model is off by default"
  local v given got
  for v in "1 1" "yes 1" "On 1" "0 0" "no 0"; do
    read -r given got <<< "$v"
    fresh config; config "[cache]\nrekordbox_model = $given\n"; harness passthrough "$fake" > /dev/null 2>&1
    expect 'has "rekordbox_model=$got, rebuild 1"' "config: rekordbox_model=$given gives $got"
  done
  fresh config; config 'rekordbox_model=maybe\n'; harness passthrough "$fake" > /dev/null 2>&1
  expect 'has "rekordbox_model=0, rebuild 1" && has "config: ignored rekordbox_model=maybe (allowed 1 or 0)"' "config: a bad rekordbox_model is ignored and logged"
}

logs() {
  point "$fake"
  local dir
  fresh rotate; dir="$h/Library/Logs/rbstemsplus"; mkdir -p "$dir"
  head -c 5242881 /dev/zero | tr '\0' a > "$dir/bridge.log"
  echo OLD > "$dir/bridge.log.1"
  harness passthrough "$fake" > /dev/null 2>&1
  expect '[ "$(stat -f %z "$dir/bridge.log.1")" = 5242881 ]' "log over 5 MB becomes bridge.log.1 (replacing the older one)"
  expect '[ "$(stat -f %z "$dir/bridge.log")" -lt 10000 ] && has "bridge loaded"' "a new bridge.log is started"
  expect '[ "$(ls "$dir" | wc -l | tr -d " ")" = 2 ]' "only bridge.log and bridge.log.1 are kept"

  fresh at-limit; dir="$h/Library/Logs/rbstemsplus"; mkdir -p "$dir"
  head -c 5242880 /dev/zero | tr '\0' a > "$dir/bridge.log"
  harness passthrough "$fake" > /dev/null 2>&1
  expect '[ ! -e "$dir/bridge.log.1" ] && [ "$(stat -f %z "$dir/bridge.log")" -gt 5242880 ]' "a log of exactly 5 MB is appended to"

  fresh log-link; dir="$h/Library/Logs/rbstemsplus"; mkdir -p "$dir"
  echo VICTIM > "$scratch/victim-$arch"
  ln -s "$scratch/victim-$arch" "$dir/bridge.log"
  harness_ok passthrough "$fake"
  expect '[ "$(cat "$scratch/victim-$arch")" = VICTIM ] && [ -L "$dir/bridge.log" ]' "a symlink at bridge.log is not written through"
}

# the home is the account's (RBSTEMS_TEST_HOME stands in for the user database), never $HOME;
# without a usable one nothing is written anywhere
long_home() {                                            # the cache's files can't fit under it; its log can
  local n="long"
  fresh "$n"
  while [ $((${#h} + 27)) -lt 864 ]; do n="$n/$(printf 'd%.0s' {1..60})"; fresh "$n"; done
}

homes() {
  point "$fake"
  fresh home-env; th="$h/account"; mkdir -p "$th"
  harness_ok passthrough "$fake"
  expect '[ -f "$th/Library/Logs/rbstemsplus/bridge.log" ] && [ -d "$th/Library/Caches/rbstemsplus" ] && [ ! -e "$h/Library" ]' \
    "the cache and log are under the account's home, not \$HOME"

  fresh no-home; th=""
  harness_ok passthrough "$fake"
  expect '[ -z "$(ls -A "$h")" ]' "no home: nothing is written (no log, no cache, no /tmp fallback)"
  fresh relative-home; th="relative-home-$arch"
  harness_ok passthrough "$fake"
  expect '[ -z "$(ls -A "$h")" ] && [ ! -e "$th" ]' "a relative home counts as none"

  long_home
  harness_ok passthrough "$fake"
  expect 'has "is too long for the cache" && has "cache NONE, " && [ ! -e "$h/Library/Caches" ]' "a home too long for the cache's files: no cache, and the log says so"

  if [ -z "$have_model" ]; then skip "homes with a model (need Pioneer's library and python3)"; return; fi
  point "$real"
  local out old
  long_home; old="$(date -v-40d +%Y%m%d%H%M)"
  mkdir -p "$h/Library"; touch -t "$old" "$h/x.flac" "$h/x.tmp" "$h/Library/x.flac" "$h/Library/x.desc"
  out="$(outcome "$scratch/model/hdemucs.onnx" a sleep a | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 a model " ]' "a home too long for the cache: the model runs, nothing is cached: got \"$out\""
  expect '[ -f "$h/x.flac" ] && [ -f "$h/x.tmp" ] && [ -f "$h/Library/x.flac" ] && [ -f "$h/Library/x.desc" ] && [ ! -e "$h/Library/Caches" ]' \
    "a home too long for the cache: nothing in it is removed or created"
  fresh no-home-model; th=""
  out="$(outcome "$scratch/model/hdemucs.onnx" a sleep a | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 a model " ] && [ -z "$(ls -A "$h")" ]' "no home: the model runs, nothing is written: got \"$out\""
}

layout_ok() {                                            # every entry is <model>/<key[0:2]>/<key>.flac
  local f re='/[0-9a-f]{64}/([0-9a-f]{2})/([0-9a-f]{64})\.flac$'
  while IFS= read -r f; do
    [[ "$f" =~ $re ]] && [ "${BASH_REMATCH[2]:0:2}" = "${BASH_REMATCH[1]}" ] || return 1
  done < <(find "$1" -name "*.flac")
}

# the outcome lines of "harness run": "a model", "a cache", "stored N", ...
outcome() { harness run "$@" 2>&1 | sed -E 's/ \(x zero 1, finite 1, error [^)]*\)//'; }

cache() {
  if [ -z "$have_model" ]; then skip "cache tests (need Pioneer's library and python3)"; return; fi
  point "$real"; fresh cache
  local m="$scratch/model" c="$h/Library/Caches/rbstemsplus" out want
  out="$(outcome "$m/hdemucs.onnx" a wait a b c d wait | tr '\n' ' ')"
  want="a model stored 1 a cache b cache c model d model stored 3 "
  expect '[ "$out" = "$want" ]' "cache, first run: got \"$out\", want \"$want\""
  expect '[ "$(count "miss ")" = 3 ] && [ "$(count "hit  ")" = 1 ] && [ "$(grep -c "near [0-9a-f]* for" "$(logf)")" = 1 ]' \
    "cache, first run: 3 misses, 1 hit, 1 near in the log"
  expect 'has "rejected (residual) for"' "a chunk with extra content (c) is refused by the residual check"
  expect '[ "$(find "$c" -name "*.flac" | wc -l | tr -d " ")" = 3 ] && [ "$(find "$c" -name "*.desc" | wc -l | tr -d " ")" = 3 ]' \
    "cache: 3 entries (.flac and .desc)"
  expect 'layout_ok "$c"' "cache layout <model sha256>/<key[0:2]>/<key>.flac"
  expect 'ls "$c"/model-*.sha256 > /dev/null 2>&1' "the model checksum is remembered"
  expect '! has ERROR' "cache: no ERROR in the log"

  out="$(outcome "$m/hdemucs.onnx" a b | tr '\n' ' ')"
  expect '[ "$out" = "a cache b cache " ]' "cache, new process: got \"$out\", want a and b from the cache"

  out="$(outcome "$m/other.onnx" a | tr '\n' ' ')"
  expect '[ "$out" = "a model " ] && [ "$(count "miss ")" = 3 ]' "a model not named hdemucs.onnx is not cached: got \"$out\""

  config 'enabled=0\n'
  out="$(outcome "$m/hdemucs.onnx" a | tr '\n' ' ')"
  expect '[ "$out" = "a model " ] && [ "$(find "$c" -name "*.flac" | wc -l | tr -d " ")" = 3 ]' "enabled=0: the model runs, nothing is cached: got \"$out\""

  config 'max_days=30\n'                                 # an entry unused for 40 days goes
  touch -t "$(date -v-40d +%Y%m%d%H%M)" "$(find "$c" -name "*.flac" | head -1)"
  outcome "$m/hdemucs.onnx" wait > /dev/null
  expect 'has "removed 1 unused for 30 days" && [ "$(find "$c" -name "*.flac" | wc -l | tr -d " ")" = 2 ]' "max_days: an old entry is removed"
}

# The cleanup removes only what the bridge writes: <model>/<key[0:2]>/<key>.flac|.desc, their
# .<pid>.tmp, model-<inode>.sha256.<pid>.tmp. Everything else, however close, stays; so does
# anything behind a symlink.
foreign() {
  if [ -z "$have_model" ]; then skip "foreign files in the cache (need Pioneer's library and python3)"; return; fi
  point "$real"; fresh foreign
  local c="$h/Library/Caches/rbstemsplus" M K K2 F o="$scratch/outside-$arch" old f missing=() gone=()
  M="$(printf 'e%.0s' {1..64})" F="$(printf 'f%.0s' {1..64})" K="ab$(printf '1%.0s' {1..62})" K2="ab$(printf '2%.0s' {1..62})"
  old="$(date -v-40d +%Y%m%d%H%M)"
  local ours=("$c/$M/ab/$K.flac" "$c/$M/ab/$K.desc" "$c/$M/ab/$K.flac.123.tmp" "$c/$M/ab/$K.desc.45.tmp"
              "$c/model-77.sha256.88.tmp" "$c/$M/cd/cd${K:2}.desc")
  # (no names differing only by case: the volume may ignore case)
  local theirs=("$c/x.flac" "$c/x.desc" "$c/x.tmp" "$c/$K.flac" "$c/$K.flac.1.tmp" "$c/model-77.sha256.tmp"
                "$c/model-x.sha256.1.tmp" "$c/model-77.sha256.88.tmp.x" "$c/$M/$K.flac" "$c/$M/model-7.sha256.1.tmp"
                "$c/$M/ab/$K.flac.tmp" "$c/$M/ab/$K.flac.1x.tmp" "$c/$M/ab/$K.flac.12.tmp.x" "$c/$M/ab/$K.flack"
                "$c/$M/ab/$K.tmp" "$c/$M/ab/${K:0:63}.flac" "$c/$M/9f/9F${K:2}.flac" "$c/$M/ab/cd${K:2}.flac"
                "$c/$M/ab/cd${K:2}.flac.1.tmp" "$c/$M/ab/sub/$K.flac" "$c/$M/0A/0a${K:2}.flac" "$c/$M/abc/$K.flac"
                "$c/${M:1}/ab/$K.flac" "$c/${M}e/ab/$K.flac" "$c/notamodel/ab/$K.flac"
                "$h/Library/Caches/x.flac" "$h/Library/x.flac" "$h/x.flac" "$h/x.tmp"
                "$o/$K.flac" "$o/ab/$K.flac" "$o/ab/$K.desc" "$o/ef${K:2}.flac")
  rm -rf "$o"
  for f in "${ours[@]}" "${theirs[@]}"; do mkdir -p "$(dirname "$f")"; echo x > "$f"; touch -t "$old" "$f"; done
  ln -s "$o" "$c/$M/ef"                                  # a symlinked prefix folder
  ln -s "$o" "$c/$F"                                     # a symlinked model folder
  ln -s "$o/$K.flac" "$c/$M/ab/$K2.flac"                 # a symlink named like an entry
  ln -s "$o/ab/$K.desc" "$c/$M/ab/$K2.desc"
  touch -h -t "$old" "$c/$M/ef" "$c/$F" "$c/$M/ab/$K2.flac" "$c/$M/ab/$K2.desc"
  config 'max_days=30\n'
  outcome "$scratch/model/hdemucs.onnx" wait > /dev/null
  for f in "${theirs[@]}"; do [ -f "$f" ] || missing+=("${f#$scratch/}"); done
  for f in "$c/$M/ef" "$c/$F" "$c/$M/ab/$K2.flac" "$c/$M/ab/$K2.desc"; do [ -L "$f" ] || missing+=("${f#$scratch/}"); done
  for f in "${ours[@]}"; do [ ! -e "$f" ] || gone+=("${f#$scratch/}"); done
  expect '[ ${#missing[@]} = 0 ]' "the cleanup removed files that aren't its own: ${missing[*]:-}"
  expect '[ ${#gone[@]} = 0 ] && has "removed 1 unused for 30 days" && has ", 3 stale temp files"' \
    "the cleanup still removes its own old entries, temp files and orphan summaries: left ${gone[*]:-}"
}

# Under its floor (10% of the cache's disk, at most 50 GB; 50 GB when the disk's size can't be
# read) nothing is written to the cache (no FLAC, .desc or temp file), cached stems still load, the
# space is measured at most once a minute, and the pause and the resume are each logged once.
# Unless a test says otherwise the disk is 1000 GB, so the floor is 50 GB.
written() { find "$h/Library/Caches/rbstemsplus" -type f \( -name "*.flac" -o -name "*.desc" -o -name "*.tmp" \) | wc -l | tr -d " "; }
space() {
  if [ -z "$have_model" ]; then skip "free space (needs Pioneer's library and python3)"; return; fi
  point "$real"; fresh space
  local m="$scratch/model/hdemucs.onnx" out avail size fl got d under label
  free=49.9
  out="$(outcome "$m" a d sleep | tr '\n' ' ')"
  expect '[ "$out" = "a model d model stored 0 " ] && [ "$(written)" = 0 ]' "under 50 GB free: nothing is written: got \"$out\", $(written) files"
  expect '[ "$(count "cache: saving paused, 49.9 GB free on its disk (test), under its floor of 50.0 GB (disk 1000.0 GB)")" = 1 ]' "the pause is logged once"
  free=50
  out="$(outcome "$m" a wait | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 1 " ] && [ "$(written)" = 2 ] && has "cache: 50.0 GB free on its disk (test), floor 50.0 GB (disk 1000.0 GB)"' "at 50 GB free the cache writes: got \"$out\""
  free=10
  out="$(outcome "$m" a b sleep | tr '\n' ' ')"
  expect '[ "$out" = "a cache b cache stored 1 " ] && [ "$(written)" = 2 ]' "under 50 GB free, saved stems still load (exact and near): got \"$out\""
  out="$(outcome "$m" d sleep free=100 d sleep | tr '\n' ' ')"
  expect '[ "$out" = "d model stored 1 d model stored 1 " ] && [ "$(written)" = 2 ] && [ "$(count "saving resumed")" = 0 ]' \
    "the free space is measured at most once a minute: got \"$out\""
  free_every=0
  out="$(outcome "$m" d sleep free=100 d wait | tr '\n' ' ')"
  expect '[ "$out" = "d model stored 1 d model stored 2 " ] && [ "$(written)" = 4 ]' "saving resumes when space returns: got \"$out\""
  expect '[ "$(count "cache: saving resumed, 100.0 GB free on its disk (test), floor 50.0 GB (disk 1000.0 GB)")" = 1 ] && [ "$(count "saving paused")" = 3 ]' \
    "the resume is logged once (and each process's pause once)"
  expect '! has ERROR' "free space: no ERROR in the log"

  # the floor by the disk's size: "size floor just-under" (size 0: it can't be read)
  for d in "256 25.6 25.5" "1000 50.0 49.9" "2000 50.0 49.9" "0 50.0 49.9"; do
    read -r size fl under <<< "$d"
    if [ "$size" = 0 ]; then label="disk size unknown"; else label="disk $size.0 GB"; fi
    fresh "space-under-$size"; disk=$size; free=$under
    out="$(outcome "$m" a sleep | tr '\n' ' ')"
    expect '[ "$out" = "a model stored 0 " ] && [ "$(written)" = 0 ] && has "cache: saving paused, $under GB free on its disk (test), under its floor of $fl GB ($label)"' \
      "$label: under its $fl GB floor nothing is written: got \"$out\""
    fresh "space-at-$size"; disk=$size; free=$fl
    out="$(outcome "$m" a wait | tr '\n' ' ')"
    expect '[ "$out" = "a model stored 1 " ] && [ "$(written)" = 2 ] && has "cache: $fl GB free on its disk (test), floor $fl GB ($label)"' \
      "$label: exactly at its $fl GB floor the cache writes: got \"$out\""
  done

  # the real measures: Finder's count, and statfs's free space and disk size (compared with df)
  fresh space-real; free=; disk=
  avail=$(( $(df -k "$h" | awk 'NR==2 {print $4}') * 1024 / 1000000000 ))
  size=$(( $(df -k "$h" | awk 'NR==2 {print $2}') * 1024 / 1000000000 ))
  fl=$(( size / 10 < 50 ? size / 10 : 50 ))
  out="$(outcome "$m" a sleep | tr '\n' ' ')"
  if [ "$avail" -ge $((fl + 2)) ]; then
    expect '[ "$out" = "a model stored 1 " ] && has "GB free on its disk (Finder'"'"'s count)"' "the real free space (Finder's count) allows writes: got \"$out\""
  elif [ "$avail" -lt $((fl - 2)) ]; then
    expect 'has "saving paused"' "this disk has under its $fl GB floor free ($avail GB): saving is paused"
  fi
  got="$(grep -o "(disk [0-9.]* GB)" "$(logf)" | head -1 | tr -d '()' | cut -d' ' -f2)"
  expect '[ -n "$got" ] && awk -v a="$got" -v b="$size" "BEGIN { exit !(a - b < 2 && b - a < 2) }"' "statfs's disk size matches df ($got vs $size GB)"
  fresh space-statfs; free=; disk=; statfs=1
  outcome "$m" a sleep > /dev/null
  got="$(grep -o "[0-9.]* GB free on its disk (statfs)" "$(logf)" | head -1 | cut -d' ' -f1)"
  expect '[ -n "$got" ] && awk -v a="$got" -v b="$avail" "BEGIN { exit !(a - b < 2 && b - a < 2) }"' "statfs's count matches df ($got vs $avail GB)"
}

# Our model (by checksum) is opened with the memory pattern off, anything else with rekordbox's
# options; a failure falls back to rekordbox's options. The test model's answers are exact either
# way (the harness checks every sample against tile(mix) * 2.475).
memory() {
  if [ -z "$have_model" ]; then skip "memory pattern (needs Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/model/hdemucs.onnx" sha out plain off want
  sha="$(shasum -a 256 "$m" | cut -d' ' -f1)"
  fresh mem-theirs
  plain="$(harness run "$m" a d c 2>&1)"
  expect 'has "Demucs session opened: model ${sha:0:12}, rekordbox'"'"'s session options" && ! has "memory pattern"' \
    "a model that isn't ours gets rekordbox's options"
  fresh mem-ours; ours="$sha"
  off="$(harness run "$m" a d c 2>&1)"
  expect 'has "Demucs session opened: model ${sha:0:12}, memory pattern off" && ! has ERROR' "our model is opened with the memory pattern off"
  expect '[ "$off" = "$plain" ] && [ "$(grep -c " model (x zero 1, finite 1, error 0.00e+00)" <<< "$off")" = 3 ]' \
    "the memory pattern off gives exactly the same answers: got \"$off\", want \"$plain\""
  fresh mem-ours-cache; ours="$sha"
  out="$(outcome "$m" a wait a b c d wait | tr '\n' ' ')"
  want="a model stored 1 a cache b cache c model d model stored 3 "
  expect '[ "$out" = "$want" ]' "the cache works the same on our model's sessions: got \"$out\""
  config 'enabled=0\n'
  out="$(outcome "$m" a | tr '\n' ' ')"
  expect '[ "$out" = "a model " ] && has "memory pattern off, not cached"' "enabled=0: the memory pattern is still off, nothing is cached"
  fresh mem-other-name; ours="$sha"
  out="$(outcome "$scratch/model/other.onnx" a | tr '\n' ' ')"
  expect '[ "$out" = "a model " ] && ! has "Demucs session" && [ -z "$(ls "$h/Library/Caches/rbstemsplus")" ]' \
    "a model not named hdemucs.onnx is neither hashed nor changed"
  fresh mem-fail; ours="$sha"; clone_fail=1
  out="$(outcome "$m" a d | tr '\n' ' ')"
  expect '[ "$out" = "a model d model " ] && has "memory pattern off failed (test failure), opening it with rekordbox'"'"'s session options" && has "${sha:0:12}, rekordbox'"'"'s session options"' \
    "if the options can't be copied, our model opens with rekordbox's options: got \"$out\""
}

# rekordbox's own model (a listed checksum, in tests RBSTEMS_TEST_PIONEER_MODEL): its stems are
# rebuilt from x and cached only where rekordbox_model=1, in their own folder; its answers reach
# the caller untouched; stems that can't be stored as they are (not adding up, beyond range, NaN,
# other shapes) aren't cached; a model taken off the list no longer finds them.
rebuilt() {
  if [ -z "$have_model" ]; then skip "rebuilt models (need Pioneer's library and python3)"; return; fi
  point "$real"
  local m="$scratch/pmodel" W="w=0.64,-0.64,0.32,-0.32" sha id out want c
  sha="$(shasum -a 256 "$m/good/hdemucs.onnx" | cut -d' ' -f1)"
  id="$(printf 'rbstems-rebuild-1:%s' "$sha" | shasum -a 256 | cut -d' ' -f1)"

  fresh rb-on; pioneer="$sha"; config 'rekordbox_model=1\n'; c="$h/Library/Caches/rbstemsplus"
  out="$(harness prun "$m/good/hdemucs.onnx" "$W" a wait a b d wait 2>&1 | sed -E 's/ \(x zero [01], worst [^)]*\)//' | tr '\n' ' ')"
  want="a model stored 1 a cache b cache d model stored 2 "
  expect '[ "$out" = "$want" ]' "rebuilt: a miss gives the model's own answer, then the rebuilt stems are served: got \"$out\", want \"$want\""
  expect 'has "rekordbox'"'"'s own: stems rebuilt" && [ "$(count ", rebuilt)")" = 2 ] && [ "$(count "hit  ")" = 1 ] && [ "$(grep -c "near [0-9a-f]* for" "$(logf)")" = 1 ]' \
    "rebuilt: the log says so (2 rebuilt misses, 1 hit, 1 near)"
  expect '[ -d "$c/$id" ] && [ ! -e "$c/$sha" ] && layout_ok "$c"' "rebuilt: the entries are in the rebuild's own folder, not the model's"
  expect '! has ERROR && ! has "not cached"' "rebuilt: no ERROR, nothing refused"
  out="$(harness prun "$m/good/hdemucs.onnx" "$W" a b 2>&1 | sed -E 's/ \(x zero [01], worst [^)]*\)//' | tr '\n' ' ')"
  expect '[ "$out" = "a cache b cache " ]' "rebuilt: a new process finds them: got \"$out\""

  pioneer=""                                              # taken off the list: its entries aren't found
  out="$(harness prun "$m/good/hdemucs.onnx" "$W" a b sleep 2>&1 | sed -E 's/ \(x zero [01], worst [^)]*\)//' | tr '\n' ' ')"
  expect '[ "$out" = "a model b model stored 2 " ]' "a model off the list: no hit, no near hit, nothing stored: got \"$out\""

  fresh rb-off; pioneer="$sha"                            # listed, but this account didn't turn it on
  out="$(harness prun "$m/good/hdemucs.onnx" "$W" a sleep a 2>&1 | sed -E 's/ \(x zero [01], worst [^)]*\)//' | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 a model " ] && has "not cached here (rekordbox_model=0)"' "rekordbox_model=0: rekordbox's own model passes through: got \"$out\""

  fresh rb-unlisted; config 'rekordbox_model=1\n'         # turned on, but not a listed model
  out="$(harness prun "$m/good/hdemucs.onnx" "$W" a sleep a 2>&1 | sed -E 's/ \(x zero [01], worst [^)]*\)//' | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 a model " ] && ! has "rekordbox'"'"'s own"' "an unlisted model with a spectrogram passes through, as in 1.0: got \"$out\""

  local v name ws why
  for v in "bad w=0.64,0.64,0.64,0.64 don't-add-up" "loud w=200,-200,100,-100 beyond-the-cache's-range" "nan w=nan,0,0,0 aren't-finite"; do
    read -r name ws why <<< "$v"; why="${why//-/ }"
    fresh "rb-$name"; pioneer="$(shasum -a 256 "$m/$name/hdemucs.onnx" | cut -d' ' -f1)"; config 'rekordbox_model=1\n'
    out="$(harness prun "$m/$name/hdemucs.onnx" "$ws" a sleep 2>&1 | sed -E 's/ \(x zero [01], worst [^)]*\)//' | tr '\n' ' ')"
    expect '[ "$out" = "a model stored 0 " ] && has "$why"' "rebuilt $name: the model's answer, nothing cached, logged ($why): got \"$out\""
  done

  # a near-silent chunk (a track's end, under -60 dBFS) isn't judged: stored even if it doesn't add up
  fresh rb-quiet; pioneer="$(shasum -a 256 "$m/bad/hdemucs.onnx" | cut -d' ' -f1)"; config 'rekordbox_model=1\n'
  out="$(harness prun "$m/bad/hdemucs.onnx" "w=0.64,0.64,0.64,0.64" gain=0.0001 a wait 2>&1 | sed -E 's/ \(x zero [01], worst [^)]*\)//' | tr '\n' ' ')"
  expect '[[ "$out" == *"stored 1 " ]] && ! has "not cached"' "rebuilt: a chunk under -60 dBFS is stored without the residual check: got \"$out\""

  fresh rb-frames; pioneer="$sha"; config 'rekordbox_model=1\n'   # F that doesn't match L
  out="$(harness prun "$m/good/hdemucs.onnx" "$W" frames=86 a sleep 2>&1 | sed -E 's/ \(x zero [01], worst [^)]*\)//' | tr '\n' ' ')"
  expect '[ "$out" = "a model stored 0 " ] && has "unexpected shapes for a rebuilt model"' "rebuilt: other shapes aren't cached: got \"$out\""

  fresh rb-ours; config 'rekordbox_model=1\n'             # our kind of model is cached as before
  out="$(outcome "$scratch/model/hdemucs.onnx" a wait a | tr '\n' ' ')"
  sha="$(shasum -a 256 "$scratch/model/hdemucs.onnx" | cut -d' ' -f1)"
  expect '[ "$out" = "a model stored 1 a cache " ] && [ -d "$h/Library/Caches/rbstemsplus/$sha" ]' "rekordbox_model=1: a model with x zero is cached as before, in its own folder: got \"$out\""

  expect 'grep -aq "rbstems-cache: rebuild=1 pioneer=435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a;" "$bridge"' \
    "the bridge says what it can cache, for the app"
}

for arch in "${archs[@]}"; do
  echo "== $arch"
  loading
  configs
  logs
  homes
  cache
  foreign
  space
  memory
  rebuilt
done

echo
echo "$passed passed, $failures failed${skipped[*]:+, skipped: ${skipped[*]}}"
[ "$failures" = 0 ]
