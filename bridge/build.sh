#!/bin/sh
# Builds the bridge as a universal (arm64 + x86_64) libonnxruntime.1.18.0.dylib for macOS 12+,
# with libFLAC (no Ogg) built from the official source and linked in statically. Exports only
# rekordbox's three ONNX Runtime entry points, and rbstems_marker.
#
#   bridge/build.sh [OUT]                 release build into OUT (default: build/bridge)
#   bridge/build.sh --test REAL_PATH OUT  for bridge/tests only: the bridge loads the real library
#                                         from REAL_PATH instead of the contract path
#
# libFLAC's tarball is downloaded once into build/flac/ (gitignored) and checked against its
# sha256 (the same on downloads.xiph.org, GitHub's xiph/flac release and Homebrew's formula) on
# every build; libFLAC is then built from it, per architecture, every time, in a scratch folder:
# no library built earlier is reused. The result is reproducible: the same source and toolchain
# give the same bytes. Needs Xcode's command line tools, nothing else.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

FLAC_VERSION=1.5.0
FLAC_SHA256=f2c1c76592a82ffff8413ba3c4a1299b6c7ab06c734dee03fd88630485c2b920
FLAC_URL="https://downloads.xiph.org/releases/flac/flac-$FLAC_VERSION.tar.xz"
MIN_MACOS=12.0
ARCHS="arm64 x86_64"
REAL_PATH="/Library/Application Support/rbstemsplus/ort/libonnxruntime.1.18.0.dylib"
LIB=libonnxruntime.1.18.0.dylib

test_path=
if [ "${1:-}" = "--test" ]; then
  [ $# -eq 3 ] || { echo "usage: $0 --test REAL_PATH OUT" >&2; exit 2; }
  test_path="$2"
  case "$test_path" in *'"'*|*'\'*) echo "REAL_PATH can't contain \" or \\" >&2; exit 2 ;; esac
  shift 2
fi
out="${1:-$root/build/bridge}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

# ---------------------------------------------------------------- libFLAC, per architecture

flac_root="$root/build/flac"
tarball="$flac_root/flac-$FLAC_VERSION.tar.xz"
mkdir -p "$flac_root"
if [ ! -f "$tarball" ] || [ "$(shasum -a 256 "$tarball" | cut -d' ' -f1)" != "$FLAC_SHA256" ]; then
  echo "downloading $FLAC_URL"
  curl -fsSL -o "$tarball.part" "$FLAC_URL"
  got="$(shasum -a 256 "$tarball.part" | cut -d' ' -f1)"
  if [ "$got" != "$FLAC_SHA256" ]; then
    rm -f "$tarball.part"
    echo "flac tarball: sha256 $got, expected $FLAC_SHA256" >&2
    exit 1
  fi
  mv -f "$tarball.part" "$tarball"
fi

jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
# built in a scratch folder: libtool breaks on the space in the repository's path
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbstems-flac.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
for arch in $ARCHS; do
  prefix="$scratch/$FLAC_VERSION-$arch"
  echo "building libFLAC $FLAC_VERSION for $arch"
  case "$arch" in arm64) host=aarch64-apple-darwin ;; *) host=x86_64-apple-darwin ;; esac
  src="$scratch/src-$arch"
  mkdir -p "$src"
  tar -xJf "$tarball" -C "$src"
  (
    cd "$src/flac-$FLAC_VERSION"
    ./configure --quiet --host="$host" \
      --enable-static --disable-shared --with-pic --disable-ogg --disable-programs \
      --disable-examples --disable-cpplibs --disable-doxygen-docs --disable-version-from-git \
      CC="clang -arch $arch" CFLAGS="-O2 -mmacosx-version-min=$MIN_MACOS"
    make -s -j"$jobs" -C src/libFLAC
  ) > "$flac_root/build-$arch.log" 2>&1 || { tail -30 "$flac_root/build-$arch.log" >&2; exit 1; }
  mkdir -p "$prefix/lib" "$prefix/include/FLAC"
  cp "$src/flac-$FLAC_VERSION/include/FLAC/"*.h "$prefix/include/FLAC/"
  cp "$src/flac-$FLAC_VERSION/src/libFLAC/.libs/libFLAC.a" "$prefix/lib/libFLAC.a"
done

# ---------------------------------------------------------------- the bridge

defines=
note="release"
if [ -n "$test_path" ]; then
  defines="-DRBSTEMS_TEST_REAL_PATH=\"$test_path\""
  note="TEST: real library from $test_path"
fi
for arch in $ARCHS; do
  prefix="$scratch/$FLAC_VERSION-$arch"
  clang -dynamiclib -O2 -arch "$arch" -mmacosx-version-min=$MIN_MACOS -Wall -Wextra -fvisibility=hidden \
    ${defines:+"$defines"} -I"$here" -I"$prefix/include" \
    "$here/rbstems_bridge.c" "$prefix/lib/libFLAC.a" -framework CoreFoundation -framework Accelerate \
    -Wl,-exported_symbol,_OrtGetApiBase \
    -Wl,-exported_symbol,_OrtSessionOptionsAppendExecutionProvider_CPU \
    -Wl,-exported_symbol,_OrtSessionOptionsAppendExecutionProvider_CoreML \
    -Wl,-exported_symbol,_rbstems_marker \
    -install_name "@rpath/$LIB" -o "$out/$LIB.$arch"
done
lipo -create "$out/$LIB.arm64" "$out/$LIB.x86_64" -output "$out/$LIB"
rm -f "$out/$LIB.arm64" "$out/$LIB.x86_64"
codesign -f -s - "$out/$LIB"

# ---------------------------------------------------------------- checks

fail() { echo "bridge/build.sh: $*" >&2; exit 1; }
lib="$out/$LIB"
archs="$(lipo -archs "$lib")"
[ "$(echo "$archs" | wc -w | tr -d ' ')" = 2 ] || fail "unexpected slices: $archs"
want="_OrtGetApiBase _OrtSessionOptionsAppendExecutionProvider_CPU _OrtSessionOptionsAppendExecutionProvider_CoreML _rbstems_marker "
for arch in $ARCHS; do
  case " $archs " in *" $arch "*) ;; *) fail "no $arch slice (got: $archs)" ;; esac
  exports="$(nm -gUj -arch "$arch" "$lib" | LC_ALL=C sort | tr '\n' ' ')"
  [ "$exports" = "$want" ] || fail "$arch exports: $exports"
  minos="$(otool -l -arch "$arch" "$lib" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')"
  [ "$minos" = "$MIN_MACOS" ] || fail "$arch minos $minos, expected $MIN_MACOS"
  for d in $(otool -L -arch "$arch" "$lib" | tail -n +2 | awk '{print $1}'); do
    case "$d" in /usr/lib/*|/System/*|"@rpath/$LIB") ;; *) fail "$arch links $d" ;; esac
  done
done
grep -aq "rbstems bridge: " "$lib" || fail "the \"rbstems bridge: \" marker string is missing"
if [ -z "$test_path" ]; then
  grep -aqF "$REAL_PATH" "$lib" || fail "the contract path $REAL_PATH is not in the release build"
  if grep -aq "TEST BUILD" "$lib"; then fail "a release build contains the test path override"; fi
  if grep -aq "RBSTEMS_TEST_" "$lib"; then fail "a release build reads test settings from the environment"; fi
fi
codesign --verify --strict "$lib" || fail "signature does not verify"
echo "built $lib ($archs, macOS $MIN_MACOS+, $note)"
