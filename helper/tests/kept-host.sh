#!/bin/sh
# Windows bookkeeping through the Mac implementation of plat.h.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/build"
scratch="$(mktemp -d "$root/build/kept-host.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
cd "$root"
sh helper/tests/kept-parser.sh > "$scratch/kept-parser.inc"
clang -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -Wl,-dead_strip \
  -DRBSP_TEST_WIN_PREPARE -DRBSP_TEST_PREPARE_CACHE_AT -Ihelper -Icommon -I"$scratch" \
  helper/tests/kept-host.c common/entries.c common/plat_mac.c common/plat_helper.c \
  -framework CoreFoundation -framework Accelerate -framework Security -o "$scratch/kept-host"
"$scratch/kept-host" "$scratch"
clang -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -Wl,-dead_strip \
  -Icommon -I"$scratch" helper/tests/kept-check.c common/entries.c common/plat_mac.c \
  -framework CoreFoundation -framework Accelerate -framework Security -o "$scratch/kept-check"
"$scratch/kept-check" "$scratch/.published.kept" 2000000000 1
awk '{printf "%s\r\n", $0}' "$scratch/.published.kept" > "$scratch/crlf.kept"
if "$scratch/kept-check" "$scratch/crlf.kept" 2000000000 1 2> /dev/null; then
  echo "FAIL bridge parser accepted CRLF"
  exit 1
fi
echo "kept-check: 2 passed, 0 failed"
