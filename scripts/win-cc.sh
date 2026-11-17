#!/bin/sh
# The pinned Windows C toolchain (release.md §3.2, task T1): llvm-mingw, UCRT, for x86_64 and
# aarch64, with the release flags. Every Windows build goes through this wrapper.
#
#   scripts/win-cc.sh x64|arm64 [clang args...]   compile/link with the release flags
#   scripts/win-cc.sh --tool x64|arm64 NAME [args] run a pinned tool (llvm-objdump, windres, ...)
#   scripts/win-cc.sh --fetch                      download and check the toolchain only
#
# The macOS universal tarball is downloaded once into build/llvm-mingw/ (gitignored), checked
# against its pinned SHA-256 and used by absolute path. Homebrew's mingw-w64 is never used.
# No MSVC, clang-cl, xwin or Windows SDK (the Windows plan §8.3): headers and import libraries are
# mingw-w64's, shipped inside the tarball.
#
# Flags (release.md §3.2): -O2 -static, C11, no timestamp, no build id, the repo path mapped to
# ".", ASLR/NX/high-entropy VA, no -march beyond the baseline. SOURCE_DATE_EPOCH is the commit
# time unless the caller set it. Pass extra flags after the arch (e.g. -shared, -mwindows).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

LLVM_MINGW_VERSION=20261104 # LLVM 23.1.5
LLVM_MINGW_SHA256=66cbd5178c4a71b0d473e68ede172f8eb7d15d023e67f39201d0a69e21f6bf3c
LLVM_MINGW_TAR="llvm-mingw-$LLVM_MINGW_VERSION-ucrt-macos-universal.tar.xz"
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$LLVM_MINGW_TAR"

tc="$root/build/llvm-mingw"
stamp="$tc/.rbsp-pinned"

fetch() {
  if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$LLVM_MINGW_SHA256" ]; then return 0; fi
  dl="$root/build/dl"
  mkdir -p "$dl"
  tarball="$dl/$LLVM_MINGW_TAR"
  if [ ! -f "$tarball" ] || [ "$(shasum -a 256 "$tarball" | cut -d' ' -f1)" != "$LLVM_MINGW_SHA256" ]; then
    echo "downloading $LLVM_MINGW_URL" >&2
    curl -fsSL -o "$tarball.part" "$LLVM_MINGW_URL"
    got="$(shasum -a 256 "$tarball.part" | cut -d' ' -f1)"
    if [ "$got" != "$LLVM_MINGW_SHA256" ]; then
      rm -f "$tarball.part"
      echo "llvm-mingw tarball: sha256 $got, expected $LLVM_MINGW_SHA256" >&2
      exit 1
    fi
    mv -f "$tarball.part" "$tarball"
  fi
  rm -rf "$tc"
  mkdir -p "$tc"
  tar -xJf "$tarball" -C "$tc" --strip-components=1
  printf '%s\n' "$LLVM_MINGW_SHA256" > "$stamp"
}

triple() {
  case "$1" in
    x64) echo x86_64-w64-mingw32 ;;
    arm64) echo aarch64-w64-mingw32 ;;
    *) echo "unknown arch '$1' (x64 or arm64)" >&2; exit 2 ;;
  esac
}

[ $# -ge 1 ] || { sed -n '5,8p' "$0" >&2; exit 2; }
if [ "$1" = "--fetch" ]; then fetch; exit 0; fi
fetch
if [ "$1" = "--tool" ]; then
  [ $# -ge 3 ] || { echo "usage: $0 --tool x64|arm64 NAME [args]" >&2; exit 2; }
  t="$(triple "$2")"; name="$3"; shift 3
  if [ -x "$tc/bin/$t-$name" ]; then exec "$tc/bin/$t-$name" "$@"; fi
  exec "$tc/bin/$name" "$@"
fi

t="$(triple "$1")"; shift
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
  SOURCE_DATE_EPOCH="$(git -C "$root" log -1 --format=%ct 2>/dev/null || echo 0)"
fi
export SOURCE_DATE_EPOCH
# mingw-w64 defaults _WIN32_WINNT to 0x601 (Windows 7), which hides Windows 10+ declarations
# (IsWow64Process2, SetDefaultDllDirectories' newer flags, ...). The floor is Windows 11 22H2,
# whose headers version is still 0x0A00 (the build is checked at run time, release.md §3.2).
exec "$tc/bin/$t-clang" \
  -std=c11 -O2 -static \
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
  "-ffile-prefix-map=$root=." \
  -Wl,--no-insert-timestamp -Wl,--build-id=none \
  -Wl,--dynamicbase,--nxcompat,--high-entropy-va \
  "$@"
