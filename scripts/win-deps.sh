#!/bin/sh
# The Windows C builds' pinned source dependencies (release.md §9.2), downloaded once into build/
# (gitignored), checked against their SHA-256 on every call, never patched:
#
#   scripts/win-deps.sh pocketfft    -> build/pocketfft  (pocketfft.c, pocketfft.h, LICENSE.md)
#   scripts/win-deps.sh blocksrt     -> build/blocksrt   (BlocksRuntime's Block.h, Block_private.h,
#                                       data.c, runtime.c, and the empty config.h its build wants)
#
# Prints the folder. pocketfft: BSD-3-Clause, commit 81d171a (the C version, no releases), FFT on
# Windows only (bridge.md §1, T2). BlocksRuntime: MIT (Apple), llvmorg-23.1.5, kept by the Phase 0
# blocks spike (bridge.md §0). Both need their notice in NOTICE-windows (bridge.md §7).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

get() {   # DIR BASEURL, then "FILE SHA256" lines on stdin
  d="$1"; base="$2"
  mkdir -p "$d"
  while read -r f sum; do
    if [ ! -f "$d/$f" ] || [ "$(shasum -a 256 "$d/$f" | cut -d' ' -f1)" != "$sum" ]; then
      echo "downloading $base/$f" >&2
      curl -fsSL -o "$d/$f.part" "$base/$f"
      got="$(shasum -a 256 "$d/$f.part" | cut -d' ' -f1)"
      if [ "$got" != "$sum" ]; then
        rm -f "$d/$f.part"
        echo "$f: sha256 $got, expected $sum" >&2
        exit 1
      fi
      mv -f "$d/$f.part" "$d/$f"
    fi
  done
}

case "${1:-}" in
  pocketfft)
    d="$root/build/pocketfft"
    get "$d" https://raw.githubusercontent.com/mreineck/pocketfft/81d171a6d5562e3aaa2c73489b70f564c633ff81 <<EOF
pocketfft.c 4f9f4095080fdba96d7112bcfe6ed66cb5c094077e92f7544602756eccd0f97c
pocketfft.h 8c12a7b2467a86c3f8818a5ab85cec2a8e90dfb2ff273cb8cf3ad937ea286ba3
LICENSE.md c254b2b5fd30ae3c89d36ba061730563b9760fca04f1b746a1b2c3157da2c789
EOF
    ;;
  blocksrt)
    d="$root/build/blocksrt"
    get "$d" https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.5/compiler-rt/lib/BlocksRuntime <<EOF
Block.h b9865a1ced1d282626221af7bdb28c23378d788afeb43dfb80cd8014bb712034
Block_private.h e29e0f94e4b544d65c0711cb20907411bb3674b1714efcfd2a42b07ae8817992
data.c cce2ba6f5366fa59470cda56617aae97edca07753370335b28f7a702bb50fc29
runtime.c 30acfda75c053f720664cac4819455dc0060dfbc20f1cf5fb6966399c9a9e578
EOF
    [ -f "$d/config.h" ] || : > "$d/config.h"             # what its CMake leaves for Windows: nothing
    ;;
  *) echo "usage: $0 pocketfft|blocksrt" >&2; exit 2 ;;
esac
echo "$d"
