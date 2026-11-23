#!/bin/sh
# Exercise Prepare's handle-relative cache path on the Mac with symlinks in place of junctions.
set -eu
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
# the scratch folder outside the checkout: a checkout under ~/Documents (a protected folder, readable.c)
# would make --quick count the track as one that may ask for permission, never "ready"
s="$(mktemp -d "${TMPDIR:-/tmp}/prepare-cache.XXXXXX")"
trap 'rm -rf "$s"' EXIT
export TMPDIR="$s"
cd "$root"
clang -O2 -Wall -Wextra -Werror -fblocks -DRBSP_TEST_PREPARE_CACHE_AT \
  -Dplat_free_bytes=prepare_cache_test_free_bytes -Ihelper -Icommon -c helper/prepare.c -o "$s/prepare.o"
clang -O2 -Wall -Wextra -Werror -fblocks -DRBSP_TEST_PREPARE_CACHE_AT -Ihelper -Icommon \
  "$s/prepare.o" helper/tests/prepare-cache.c helper/kept.c helper/manifest.c helper/readable.c \
  common/entries.c common/path_rules.c common/plat_mac.c common/plat_helper.c \
  -framework CoreFoundation -framework Accelerate -framework Security -o "$s/prepare"

model=0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772
entry=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.flac
mkdir "$s/cache" "$s/kept" "$s/outside"
touch "$s/track.wav"
run() { "$s/prepare" --model test --cache "$s/cache" --chosen --kept-dir "$s/kept" "$@" "$s/track.wav"; }
pass=0 fail=0
check() {
  label="$1"; shift
  if "$@"; then pass=$((pass + 1)); else echo "FAIL $label"; fail=$((fail + 1)); fi
}
stopped() { grep -q '"stopped":"the cache folder can.t be written:' "$s/result"; }
unchanged() { grep -q '"how":"unchanged since it was prepared"' "$s/result"; }

ln -s "$s/outside" "$s/cache/$model"
run --no-keep > "$s/result"
check 'model link stops a write' stopped
check 'model link leaves target without a prefix' test ! -e "$s/outside/aa"
rm "$s/cache/$model"

run --keep test > "$s/result"
check 'plain cache accepts a write' grep -q '"written":1' "$s/result"
f="$s/cache/$model/aa/$entry"
touch -t 200001010000 "$f"
old="$(stat -f %m "$f")"
run --keep test > "$s/result"
check 'plain cache uses manifest fast path' unchanged
check 'plain cache refreshes timestamps' test "$(stat -f %m "$f")" -gt "$old"
run --quick --keep test > "$s/result"
check 'estimate finds an entry in the plain cache' grep -q '"result":"ready"' "$s/result"

mv "$s/cache/$model" "$s/outside/model"
ln -s "$s/outside/model" "$s/cache/$model"
f="$s/outside/model/aa/$entry"
touch -t 200001010000 "$f"
run --quick --keep test > "$s/result"
check 'estimate treats an entry through a model link as missing' grep -q '"missing":1' "$s/result"
run --keep test > "$s/result"
check 'manifest cannot reuse an entry through a model link' stopped
check 'model link leaves target timestamp alone' test "$(stat -f %m "$f")" -eq "$old"
rm "$s/cache/$model"
mv "$s/outside/model" "$s/cache/$model"

mv "$s/cache/$model/aa" "$s/outside/prefix"
ln -s "$s/outside/prefix" "$s/cache/$model/aa"
f="$s/outside/prefix/$entry"
touch -t 200001010000 "$f"
run --keep test > "$s/result"
check 'prefix link stops Prepare' stopped
check 'prefix link leaves target timestamp alone' test "$(stat -f %m "$f")" -eq "$old"

mv "$s/cache" "$s/cache-original"
ln -s "$s/outside" "$s/cache"
reject_root() { run --no-keep > "$s/result" 2> "$s/error" && return 1; grep -q 'no cache folder' "$s/error"; }
check 'cache root link is rejected' reject_root

rm "$s/cache"
mkdir -p "$s/cache/$model/bb"
victim="$s/cache/$model/bb/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.flac"
dd if=/dev/zero of="$victim" bs=1024 count=1024 2> /dev/null
run --no-keep --max-gb 0.0045 > "$s/result"
check 'room is made through the open cache root' test ! -e "$victim"
check 'writing resumes after making room' grep -q '"written":1' "$s/result"

echo "prepare-cache: $pass passed, $fail failed"
test "$fail" -eq 0
