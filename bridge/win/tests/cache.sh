#!/bin/sh
# Windows cache policy on the Mac, using the real bridge and host directory handles.
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd -P)
scratch=$(mktemp -d "$root/.cache-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
flac=${FLAC_PREFIX:-/opt/homebrew/opt/flac}
clang -O2 -fblocks -ffunction-sections -fdata-sections -Wl,-dead_strip \
    "-DCACHE_BRIDGE_SOURCE=\"${CACHE_BRIDGE_SOURCE:-../../rbstems_bridge.c}\"" \
    -I"$root/common" -I"$flac/include" "$root/bridge/win/tests/cache.c" -L"$flac/lib" -lFLAC \
    -framework Accelerate -framework CoreFoundation -o "$scratch/test"
"$scratch/test" "$scratch" "$@"
