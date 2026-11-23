#!/bin/sh
# The Windows helper's T13-T15 modules on this Mac (helper.md §1, §4, §5), plain and with
# ASan+UBSan, each on a fresh scratch folder:
#  - helper/tests/win-stop.c: the Stop line rule (helper/stdin_stop.c's pure part; its watcher and
#    the Pause/Stop process cases are Windows only: rbsp-win-stop-test.exe, in the VM);
#  - helper/tests/win-rules.c: library.c's count invariant and manifest.c's comparator, with the
#    Mac's track rules and with Windows' (PATHS_WIN_TRACKS), on a fixture library encrypted with a
#    dummy key (never rekordbox's; SQLCipher with CommonCrypto, as the Mac helper's).
# readable.c's Windows half has no Mac form: rbsp-win-readable-test.exe, in the VM.
#
#   helper/tests/win-helper.sh [OUT]      default build/helper-win-helper; exits 1 on a failure
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/build/helper-win-helper}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
SQLCIPHER_VERSION="$(sed -n 's/^SQLCIPHER_VERSION=//p' "$here/../build.sh")"
sqlc="$root/build/helper/sqlcipher-$SQLCIPHER_VERSION"
[ -f "$sqlc/sqlite3.c" ] || "$root/helper/build.sh" "$out/helper" > "$out/helper-build.log"   # makes the amalgamation
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-win-helper.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
ln -s "$sqlc" "$scratch/sqlc"
printf 'static const unsigned char LIBKEY[] = {0};\n#define LIBKEY_LEN 1\n' > "$scratch/libkey.h"   # not rekordbox's
MAC="-arch arm64 -mmacosx-version-min=12.0"
WARN="-Wall -Wextra -Werror"
SQLC="-DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_CC -DSQLITE_TEMP_STORE=2 -DSQLITE_THREADSAFE=1 -DSQLITE_EXTRA_INIT=sqlcipher_extra_init
  -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0"
# shellcheck disable=SC2086
clang $MAC -O2 -w $(echo $SQLC) -c "$scratch/sqlc/sqlite3.c" -o "$scratch/sqlite3.o"
fails=0
run() {   # NAME BINARY [ARGS]
  n="$1"; shift
  if "$@" > "$out/$n.log" 2>&1; then echo "ok   $n ($(tail -1 "$out/$n.log"))"; else cat "$out/$n.log"; echo "FAIL $n"; fails=$((fails + 1)); fi
}
for v in plain asan; do
  case $v in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
  esac
  # shellcheck disable=SC2086
  (cd "$root" && clang $MAC $WARN -std=c11 -Wpedantic $san -Ihelper helper/tests/win-stop.c helper/stdin_stop.c -o "$out/win-stop-$v")
  run "win-stop ($v)" "$out/win-stop-$v"
  for r in mac win; do
    d=""; [ $r = win ] && d="-DPATHS_WIN_TRACKS"
    # shellcheck disable=SC2086
    (cd "$root" && clang $MAC $WARN $san $d -Ihelper -Icommon -I"$scratch" -I"$scratch/sqlc" helper/tests/win-rules.c helper/library.c \
      helper/manifest.c common/path_rules.c common/plat_mac.c common/plat_helper.c "$scratch/sqlite3.o" \
      -framework CoreFoundation -framework Accelerate -framework Security -o "$out/win-rules-$r-$v")
    mkdir -p "$scratch/$r-$v"
    run "win-rules, the $r rules ($v)" "$out/win-rules-$r-$v" "$scratch/$r-$v"
  done
done
[ "$fails" = 0 ]
