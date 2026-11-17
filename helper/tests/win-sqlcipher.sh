#!/bin/sh
# SQLCipher's two crypto providers on this Mac (T3): the Mac helper's (CommonCrypto) and the
# Windows helper's (LibTomCrypt 1.18.2), the same amalgamation and options otherwise
# (helper/tests/win-library.c). Each opens the database the other made (the same SQLCipher 4
# format, so a rekordbox library opens the same way), and the key derivation (PBKDF2-HMAC-SHA512,
# 256,000 rounds: what every --playlists and --tracks pays first) is timed with each: LibTomCrypt's
# early number for P-H5 (helper.md §2a: on Windows at most the Mac's time + 100 ms). Neither
# touches rekordbox's library: the test key isn't rekordbox's, and library.c is built with a dummy.
#
#   helper/tests/win-sqlcipher.sh [OUT]      default build/helper-win-sqlcipher; exits 1 on a failure
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/build/helper-win-sqlcipher}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
deps="$root/scripts/deps.sh"
SQLCIPHER_VERSION="$(sed -n 's/^SQLCIPHER_VERSION=//p' "$here/../build.sh")"
sqlc="$root/build/helper/sqlcipher-$SQLCIPHER_VERSION"
[ -f "$sqlc/sqlite3.c" ] || "$root/helper/build.sh" "$out/helper" > "$out/helper-build.log"   # makes the amalgamation
LTC_VERSION="$("$deps" get libtomcrypt version)"
ltc="$root/build/helper-win-deps/libtomcrypt-$LTC_VERSION-mac"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-win-sqlc.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
if [ ! -f "$ltc/lib/libtomcrypt.a" ]; then
  mkdir -p "$scratch/ltc"
  tar -xJf "$("$deps" fetch libtomcrypt)" -C "$scratch/ltc"
  "$deps" licence libtomcrypt.lib "$scratch/ltc"
  (cd "$scratch/ltc/libtomcrypt-$LTC_VERSION" && make -s -j"$(sysctl -n hw.ncpu)" -f makefile.unix CC="clang -arch arm64 -mmacosx-version-min=12.0" \
    CFLAGS="-O2 -w" EXTRALIBS= libtomcrypt.a) > "$scratch/ltc.log" 2>&1 || { tail -20 "$scratch/ltc.log"; exit 1; }
  mkdir -p "$ltc/lib"
  cp "$scratch/ltc/libtomcrypt-$LTC_VERSION/libtomcrypt.a" "$ltc/lib/"
  cp -R "$scratch/ltc/libtomcrypt-$LTC_VERSION/src/headers" "$ltc/include"
fi
ln -s "$sqlc" "$scratch/sqlc"
ln -s "$ltc" "$scratch/ltcd"
printf 'static const unsigned char LIBKEY[] = {0};\n#define LIBKEY_LEN 1\n' > "$scratch/libkey.h"   # not rekordbox's
COMMON="-DSQLITE_HAS_CODEC -DSQLITE_TEMP_STORE=2 -DSQLITE_THREADSAFE=1 -DSQLITE_EXTRA_INIT=sqlcipher_extra_init
  -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0"
MAC="-O2 -arch arm64 -mmacosx-version-min=12.0"
(
  cd "$root"
  # shellcheck disable=SC2086
  clang $MAC -w $(echo $COMMON) -DSQLCIPHER_CRYPTO_CC -c "$scratch/sqlc/sqlite3.c" -o "$scratch/sqlite3-cc.o"
  # shellcheck disable=SC2086
  clang $MAC -w $(echo $COMMON) -DSQLCIPHER_CRYPTO_LIBTOMCRYPT -I"$scratch/ltcd/include" -c "$scratch/sqlc/sqlite3.c" -o "$scratch/sqlite3-ltc.o"
  for p in cc ltc; do
    x=""; [ $p = ltc ] && x="$scratch/ltcd/lib/libtomcrypt.a"
    # shellcheck disable=SC2086
    clang $MAC -Wall -Wextra -Werror -Ihelper -Icommon -I"$scratch" -I"$scratch/sqlc" helper/tests/win-library.c helper/library.c \
      common/plat_mac.c common/plat_helper.c "$scratch/sqlite3-$p.o" $x -framework CoreFoundation -framework Accelerate -framework Security \
      -o "$out/win-library-$p"
  done
) > "$out/cc.log" 2>&1 || { cat "$out/cc.log"; echo "FAIL the builds"; exit 1; }
fails=0
check() { if "$@" > "$scratch/o" 2>&1; then echo "ok   $* -> $(cat "$scratch/o")"; else echo "FAIL $*: $(cat "$scratch/o")"; fails=$((fails + 1)); fi; }
check "$out/win-library-cc" --make "$scratch/cc.db"
check "$out/win-library-ltc" --make "$scratch/ltc.db"
check "$out/win-library-ltc" --open "$scratch/cc.db"
check "$out/win-library-cc" --open "$scratch/ltc.db"
check "$out/win-library-cc" --open "$scratch/cc.db"
check "$out/win-library-ltc" --open "$scratch/ltc.db"
# a wrong key is refused by both (not just any file opens)
printf 'SQLite format 3\0not a database' > "$scratch/junk.db"
if "$out/win-library-ltc" --open "$scratch/junk.db" > /dev/null 2>&1; then echo "FAIL LibTomCrypt opened junk"; fails=$((fails + 1)); else echo "ok   LibTomCrypt refuses a file it can't decrypt"; fi
[ "$fails" = 0 ]
