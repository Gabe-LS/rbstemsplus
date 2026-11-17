#!/bin/sh
# Builds the prepare helper, rbsp-prepare, for Apple silicon (macOS 12+), with libFLAC built from
# the bridge's tarball (its version, URL and sha256 are bridge/build.sh's; downloaded once into
# build/flac/, where bridge/build.sh keeps it too) and linked in statically. Signed ad hoc WITHOUT the hardened runtime: it loads Pioneer's ONNX Runtime and
# rekordbox's libmpg123, which library validation would refuse (requirement 13).
#
#   helper/build.sh [OUT]       into OUT (default build/helper)
#   helper/build.sh --test-timing OUT   with the performance timing lines on stderr
#                               (helper/tests/perf.sh; never a release: build-app.sh never asks)
#
# libFLAC is built once per tarball into build/helper/flac-<version>/ (delete it to rebuild).
# Next to the helper, OUT/rbsp-prepare.inputs: the repository files that reach its bytes (from the
# compiles' depfiles, this script and bridge/build.sh), for the Mac carry-forward (release.sh).
#
# Reading rekordbox's playlists: SQLCipher, built from its release tarball (downloaded once into
# build/sqlcipher/, checked against its sha256: the same in Homebrew's formula for that version)
# with Apple's CommonCrypto, into build/helper/sqlcipher-<version>/ (its amalgamation, compiled
# into the helper). rekordbox's library key isn't in this repository: it is the one pyrekordbox
# publishes (its masterdb/database.py, scrambled with utils.py's BLOB_KEY), both files downloaded
# once from a pinned commit into build/pyrekordbox/<commit>/ and checked against their sha256s;
# RBSP_LIBRARY_KEY_FILE, a file holding the key, is used instead when it is set (offline). Either
# way the key is checked against its sha256 and built in masked, so it isn't a plain string in
# the binary.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
timing=
if [ "${1:-}" = "--test-timing" ]; then
  [ $# -eq 2 ] || { echo "usage: $0 --test-timing OUT" >&2; exit 2; }
  timing=-DRBSP_TEST_TIMING
  shift
fi
out="${1:-$root/build/helper}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
FLAC_VERSION="$(sed -n 's/^FLAC_VERSION=//p' "$root/bridge/build.sh")"
FLAC_SHA256="$(sed -n 's/^FLAC_SHA256=//p' "$root/bridge/build.sh")"
tarball="$root/build/flac/flac-$FLAC_VERSION.tar.xz"
SQLCIPHER_VERSION=4.19.0
SQLCIPHER_SHA256=7075f96cbabe45b4ecfc2e6b1745a625f856f695b0827a5506ce9ed85b906aa0
SQLCIPHER_URL="https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v$SQLCIPHER_VERSION.tar.gz"
LIBKEY_SHA256=ff44e8729e6dfe792d4cef556e4cc1ed3f27225604ede12877a5a04a776c0665
# pyrekordbox v0.5.0 (MIT)
PYREKORDBOX_COMMIT=2dcb57f2ea907c51fa15850f5433ec09ff3223ff
PYREKORDBOX_DATABASE_SHA256=1f9dc4ba70f079942eaf4a6fd3fcf8d1816fcd03a5723408f512e3b3683c7a3b
PYREKORDBOX_UTILS_SHA256=7da83d3de766a2f352001e77b0caff92872569239d449648c1dc03fb28022289
# the object folder: it holds the key while the build runs (mktemp -d: this account's alone)
obj="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-obj.XXXXXX")"
trap 'rm -rf "${scratch:-}" "${obj:?}"' EXIT
# fetch URL FILE SHA256: downloads FILE once, kept only when its sha256 is SHA256
fetch() {
  if [ ! -f "$2" ] || [ "$(shasum -a 256 "$2" | cut -d' ' -f1)" != "$3" ]; then
    echo "downloading $1"
    mkdir -p "$(dirname "$2")"
    curl -fsSL -o "$2.part" "$1"
    got="$(shasum -a 256 "$2.part" | cut -d' ' -f1)"
    if [ "$got" != "$3" ]; then
      rm -f "$2.part"
      echo "$1: sha256 $got, expected $3" >&2
      exit 1
    fi
    mv -f "$2.part" "$2"
  fi
}
fetch "https://downloads.xiph.org/releases/flac/flac-$FLAC_VERSION.tar.xz" "$tarball" "$FLAC_SHA256"
keyfile="${RBSP_LIBRARY_KEY_FILE:-}"
if [ -n "$keyfile" ]; then
  [ -r "$keyfile" ] || { echo "RBSP_LIBRARY_KEY_FILE: $keyfile can't be read" >&2; exit 1; }
else
  pyrb="$root/build/pyrekordbox/$PYREKORDBOX_COMMIT"
  pyrb_url="https://raw.githubusercontent.com/dylanljones/pyrekordbox/$PYREKORDBOX_COMMIT/pyrekordbox"
  fetch "$pyrb_url/masterdb/database.py" "$pyrb/database.py" "$PYREKORDBOX_DATABASE_SHA256"
  fetch "$pyrb_url/utils.py" "$pyrb/utils.py" "$PYREKORDBOX_UTILS_SHA256"
  # pyrekordbox's deobfuscate(BLOB): base85, XOR with BLOB_KEY, zlib. Both read as literals,
  # nothing of pyrekordbox's is run.
  keyfile="$obj/libkey.raw"
  python3 -I - "$pyrb/database.py" "$pyrb/utils.py" > "$keyfile" <<'PY' || { echo "couldn't read the key from pyrekordbox's files" >&2; exit 1; }
import ast, base64, re, sys, zlib
def literal(path, name):
    m = re.search(r'^' + name + r'\s*=\s*(b"[^"]*")', open(path, encoding="utf-8").read(), re.M)
    if not m:
        sys.exit(path + ": no " + name)
    return ast.literal_eval(m.group(1))
blob, k = literal(sys.argv[1], "BLOB"), literal(sys.argv[2], "BLOB_KEY")
data = base64.b85decode(blob)
sys.stdout.buffer.write(zlib.decompress(bytes(b ^ k[i % len(k)] for i, b in enumerate(data))))
PY
fi
if [ "$(tr -d '\r\n' < "$keyfile" | shasum -a 256 | cut -d' ' -f1)" != "$LIBKEY_SHA256" ]; then
  echo "$keyfile doesn't hold rekordbox's library key (sha256 differs)" >&2
  exit 1
fi
flac="$root/build/helper/flac-$FLAC_VERSION"
if [ ! -f "$flac/lib/libFLAC.a" ]; then
  scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-flac.XXXXXX")"
  tar -xJf "$tarball" -C "$scratch"
  (
    cd "$scratch/flac-$FLAC_VERSION"
    ./configure --quiet --host=aarch64-apple-darwin \
      --enable-static --disable-shared --with-pic --disable-ogg --disable-programs \
      --disable-examples --disable-cpplibs --disable-doxygen-docs --disable-version-from-git \
      CC="clang -arch arm64" CFLAGS="-O2 -mmacosx-version-min=12.0"
    make -s -j"$(sysctl -n hw.ncpu)" -C src/libFLAC
  ) > "$scratch/build.log" 2>&1 || { tail -30 "$scratch/build.log" >&2; exit 1; }
  mkdir -p "$flac/lib" "$flac/include/FLAC"
  cp "$scratch/flac-$FLAC_VERSION/include/FLAC/"*.h "$flac/include/FLAC/"
  cp "$scratch/flac-$FLAC_VERSION/src/libFLAC/.libs/libFLAC.a" "$flac/lib/libFLAC.a"
fi
sqlc_tar="$root/build/sqlcipher/sqlcipher-$SQLCIPHER_VERSION.tar.gz"
fetch "$SQLCIPHER_URL" "$sqlc_tar" "$SQLCIPHER_SHA256"
# SQLCipher's options: its codec, CommonCrypto (no OpenSSL), temp files in memory (as its README
# asks), no extension loading
SQLC_DEFS="-DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_CC -DSQLITE_TEMP_STORE=2 -DSQLITE_THREADSAFE=1
  -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0"
sqlc="$root/build/helper/sqlcipher-$SQLCIPHER_VERSION"
if [ ! -f "$sqlc/sqlite3.c" ]; then
  scratch2="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-sqlc.XXXXXX")"
  tar -xzf "$sqlc_tar" -C "$scratch2"
  (
    cd "$scratch2/sqlcipher-$SQLCIPHER_VERSION"
    ./configure --with-tempstore=yes --disable-tcl --disable-shared CC="clang -arch arm64" \
      CFLAGS="-O2 -mmacosx-version-min=12.0 $(echo $SQLC_DEFS)"
    make sqlite3.c
  ) > "$scratch2/build.log" 2>&1 || { tail -30 "$scratch2/build.log" >&2; rm -rf "${scratch2:?}"; exit 1; }
  mkdir -p "$sqlc"
  cp "$scratch2/sqlcipher-$SQLCIPHER_VERSION/sqlite3.c" "$scratch2/sqlcipher-$SQLCIPHER_VERSION/sqlite3.h" "$sqlc/"
  rm -rf "${scratch2:?}"
fi
cflags="-O2 -arch arm64 -mmacosx-version-min=12.0 -Wall -Wextra${timing:+ $timing}"
# RBSP_RESEARCH=1: a research build (--keys: each chunk's key, no model run, nothing written), never
# released: scripts/build.sh refuses to run with it set, build-app.sh clears it
if [ "${RBSP_RESEARCH:-}" = 1 ]; then cflags="$cflags -DRBSP_RESEARCH"; echo "RESEARCH BUILD (--keys)"; fi
cc() { clang $cflags -I"$obj" -I"$sqlc" -I"$here" -I"$root/common" -I"$root/bridge" -I"$flac/include" "$@"; }
# the key, masked: byte i stored as key[i] + 0x5a + 13i (library.c undoes it)
tr -d '\r\n' < "$keyfile" | od -An -tu1 -v | awk '
  BEGIN { n = 0 }    # numeric from the start: an unset n indexes b[""], not b[0]
  { for (f = 1; f <= NF; f++) { b[n] = ($f + 90 + 13 * n) % 256; n++ } }
  END { printf "static const unsigned char LIBKEY[] = {"; for (i = 0; i < n; i++) printf "%s%d", i ? "," : "", b[i];
        printf "};\n#define LIBKEY_LEN %d\n", n }' > "$obj/libkey.h"
# the masked bytes must give the key back (its sha256), or the build stops
cat > "$obj/keycheck.c" <<'C'
#include <stdio.h>
#include <CommonCrypto/CommonDigest.h>
#include "libkey.h"
int main(void) {
    unsigned char k[LIBKEY_LEN], h[32];
    for (int i = 0; i < LIBKEY_LEN; i++) k[i] = (unsigned char)(LIBKEY[i] - 0x5a - 13 * i);
    CC_SHA256(k, LIBKEY_LEN, h);
    for (int i = 0; i < 32; i++) printf("%02x", h[i]);
    printf("\n");
    return 0;
}
C
clang -w -I"$obj" "$obj/keycheck.c" -o "$obj/keycheck"
if [ "$("$obj/keycheck")" != "$LIBKEY_SHA256" ]; then
  echo "the masked key doesn't decode to the key: build.sh's masking and library.c's differ" >&2
  exit 1
fi
rm -f "$obj/keycheck" "$obj/keycheck.c" "$obj/libkey.raw"
# SQLCipher: its own warnings aren't ours
clang -O2 -arch arm64 -mmacosx-version-min=12.0 -w $(echo $SQLC_DEFS) -MD -MF "$obj/sqlite3.d" -c "$sqlc/sqlite3.c" -o "$obj/sqlite3.o"
# the resampler: rekordbox's fused multiply-adds only where it has them
cc -ffp-contract=off -MD -MF "$obj/resample.d" -c "$here/resample.c" -o "$obj/resample.o"
for f in prepare decode aac mp3 mp4 model entry library kept manifest big readable; do cc -MD -MF "$obj/$f.d" -c "$here/$f.c" -o "$obj/$f.o"; done
# common/ (bridge.md §0a, helper.md T5): the file layer, sha256, the cache's layout and size rule,
# and the path rules, each compiled on its own (external linkage)
for f in plat_mac plat_helper entries path_rules; do cc -MD -MF "$obj/c_$f.d" -c "$root/common/$f.c" -o "$obj/c_$f.o"; done
clang -arch arm64 -mmacosx-version-min=12.0 "$obj"/*.o "$flac/lib/libFLAC.a" \
  -framework AudioToolbox -framework CoreFoundation -framework Accelerate -framework Security -o "$out/rbsp-prepare"
# the helper's input list (the Mac carry-forward, scripts/mac-inputs.py): every repository file
# its compiles read (the depfiles: so every bridge/ header through -I bridge/), this script, and
# bridge/build.sh, which pins libFLAC (FLAC_VERSION, FLAC_SHA256)
# (the parser's own status checked, and an empty list refused: the list from before stays then)
deps="$(python3 -I "$root/scripts/mac-inputs.py" deps "$root" "$obj"/*.d)" \
  || { echo "helper/build.sh: the depfiles of rbsp-prepare couldn't be read" >&2; exit 1; }
[ -n "$deps" ] || { echo "helper/build.sh: no input list for rbsp-prepare" >&2; exit 1; }
printf '%s\nhelper/build.sh\nbridge/build.sh\n' "$deps" | LC_ALL=C sort -u > "$out/rbsp-prepare.inputs.tmp"
mv "$out/rbsp-prepare.inputs.tmp" "$out/rbsp-prepare.inputs"
rm -rf "$obj"
codesign -f -s - -i io.github.rbstemsplus.prepare "$out/rbsp-prepare"
echo "built $out/rbsp-prepare${timing:+ (TEST: timing lines)}"
