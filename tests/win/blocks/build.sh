#!/bin/sh
# The Phase 0 blocks spike (the private Windows plan's bridge §0 "Blocks"), rebuilt on demand:
#   tests/win/blocks/build.sh [OUT]       default build/blocks-spike
#  - BlocksRuntime at llvmorg-23.1.5 (the four files release.md §9.2 pins, checked against their
#    SHA-256s), downloaded once into build/blocksrt/;
#  - spike.dll and spike.exe for x64 and arm64 with the pinned llvm-mingw (scripts/win-cc.sh),
#    the runtime linked statically; then what the gates read: the link's warnings (exactly the four
#    LNK4217 lines for the block ABI symbols), the DLL's imports (KERNEL32 and api-ms-win-crt-*
#    only), no pthread_/_Unwind_/__cxa_ symbol;
#  - bench, the Mac's block vs function-pointer GCD timing (small, not exclusive).
# The VM step (lane W): copy OUT/x64/spike.exe and spike.dll (and arm64's) to C:\rbtest\ and run
# spike.exe: it prints "blocks spike: ok (0)".
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
cc="$root/scripts/win-cc.sh"
out="${1:-$root/build/blocks-spike}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
rt="$root/build/blocksrt"
mkdir -p "$rt"
base=https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.5/compiler-rt/lib/BlocksRuntime
while read -r f sum; do
  if [ ! -f "$rt/$f" ] || [ "$(shasum -a 256 "$rt/$f" | cut -d' ' -f1)" != "$sum" ]; then
    curl -fsSL -o "$rt/$f.part" "$base/$f"
    [ "$(shasum -a 256 "$rt/$f.part" | cut -d' ' -f1)" = "$sum" ] || { rm -f "$rt/$f.part"; echo "$f: wrong sha256" >&2; exit 1; }
    mv -f "$rt/$f.part" "$rt/$f"
  fi
done <<EOF
Block.h b9865a1ced1d282626221af7bdb28c23378d788afeb43dfb80cd8014bb712034
Block_private.h e29e0f94e4b544d65c0711cb20907411bb3674b1714efcfd2a42b07ae8817992
data.c cce2ba6f5366fa59470cda56617aae97edca07753370335b28f7a702bb50fc29
runtime.c 30acfda75c053f720664cac4819455dc0060dfbc20f1cf5fb6966399c9a9e578
EOF
: > "$out/config.h"                         # what its CMake leaves for Windows: nothing
W="-Wall -Wextra -Werror"
fail=0
for a in x64 arm64; do
  o="$out/$a"; mkdir -p "$o"
  # the runtime's own Windows branch passes an int * to InterlockedCompareExchange (32-bit on
  # LLP64, harmless): compiled as it is, its warnings off, never patched
  for s in runtime data; do
    "$cc" $a -w -Wno-incompatible-pointer-types -c -I"$out" -I"$rt" "$rt/$s.c" -o "$o/$s.o"
  done
  "$cc" $a $W -Wno-unused-command-line-argument -fblocks -c -I"$rt" "$here/spike.c" -o "$o/spike.o"
  "$cc" $a -shared "$o/spike.o" "$o/runtime.o" "$o/data.o" -o "$o/spike.dll" 2> "$o/link.log"
  "$cc" $a $W "$here/main.c" -o "$o/spike.exe"
  warn="$(grep -c 'warning' "$o/link.log" || true)"
  l4217="$(grep -cE 'locally defined symbol imported: (_NSConcreteGlobalBlock|_NSConcreteStackBlock|_Block_object_assign|_Block_object_dispose) ' "$o/link.log" || true)"
  dlls="$("$cc" --tool $a llvm-objdump -p "$o/spike.dll" | sed -n 's/.*DLL Name: //p' | sort -u | tr '\n' ' ')"
  bad="$(echo "$dlls" | tr ' ' '\n' | grep -v -e '^$' -e '^KERNEL32.dll$' -e '^api-ms-win-crt-' || true)"
  syms="$("$cc" --tool $a llvm-nm "$o/spike.dll" 2> /dev/null | grep -cE 'pthread_|_Unwind_|__cxa_' || true)"
  echo "$a: link warnings $warn (LNK4217 on the four block symbols: $l4217); imports: $dlls; pthread/unwind/cxa symbols: $syms"
  if [ "$warn" != 4 ] || [ "$l4217" != 4 ] || [ -n "$bad" ] || [ "$syms" != 0 ]; then echo "FAIL $a"; fail=1; fi
done
if [ "$(uname)" = Darwin ]; then
  clang -O2 -Wall -Wextra "$here/bench.c" -o "$out/bench" && "$out/bench"
fi
[ "$fail" = 0 ] && echo "blocks spike: builds and links (x64, arm64)" || exit 1
