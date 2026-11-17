#!/bin/sh
# The Windows helper's platform calls (common/plat_helper.c, T4) and big_plat.c (T7) on this Mac:
# helper/tests/win-plat.c built on plat_mac.c with common/'s warnings, plain and with ASan+UBSan,
# each run on a fresh scratch folder. Their Windows halves (plat_helper_win.c, and the same test as
# rbsp-win-plat-test.exe) are built by helper/build-win.sh and run in the VM.
#
#   helper/tests/win-plat.sh [OUT]      default build/helper-win-plat; exits 1 on a failure
#
# Run outside a sandbox: the temporary file is made in the account's own temporary folder.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$root/build/helper-win-plat}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
WARN="-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Werror"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-win-plat.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
fails=0
for v in plain asan; do
  case $v in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
  esac
  # shellcheck disable=SC2086
  (cd "$root" && clang $WARN -mmacosx-version-min=12.0 $san -Icommon -Ihelper helper/tests/win-plat.c common/plat_helper.c \
    helper/big_plat.c common/plat_mac.c -framework CoreFoundation -framework Accelerate -o "$out/win-plat-$v")
  mkdir -p "$scratch/$v"
  if "$out/win-plat-$v" "$scratch/$v"; then echo "ok   win-plat ($v)"; else echo "FAIL win-plat ($v)"; fails=$((fails + 1)); fi
done
[ "$fails" = 0 ] || exit 1
