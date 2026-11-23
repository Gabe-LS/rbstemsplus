#!/bin/sh
# rbsp-admin.exe, the elevated worker (app.md §5.5, release.md §3.4), for Windows x64 only, with
# the pinned llvm-mingw and the release flags (scripts/win-cc.sh): -mwindows -municode, the
# requireAdministrator manifest and a constant version resource (admin.rc: no per-release
# version, I83), -ffunction-sections and --gc-sections (so the shared modules don't pull SHELL32
# or ole32 in), common/ and win/c linked statically. The compiled-in keys come from
# keys/release.pub.pem and keys/backup.pub.pem (as build-app.sh's), or, for a test build, the
# one key --test-key names. VERSION is never read: two builds that differ only in VERSION give
# the same bytes (J18).
#
#   win/admin/build.sh OUT                     the release worker: OUT/rbsp-admin.exe
#   win/admin/build.sh OUT --test-key PEM      a test build (its key, Program Files movable by
#                                              RBSP_TEST_PROGRAMS: PLAT_TEST_PROGRAMS, and the
#                                              worker held by RBSP_TEST_WORKER_HOLD and
#                                              RBSP_TEST_WORKER_MIDHOLD: ADMIN_TEST_HOLD)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
[ $# -ge 1 ] || { sed -n '11,15p' "$0" >&2; exit 2; }
out="$1"; shift
test_key=""
if [ "${1:-}" = "--test-key" ]; then test_key="$2"; shift 2; fi
[ $# -eq 0 ] || { echo "unknown argument: $1" >&2; exit 2; }
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cc="$root/scripts/win-cc.sh"
pf="$("$root/scripts/win-deps.sh" pocketfft)"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror"

# a P-256 public key as the 91-byte uncompressed SPKI sig.c takes (sig_spki_p256), whatever point
# form the PEM holds, in base64; anything else fails
pubkey_b64() {
  text="$(/usr/bin/openssl ec -pubin -in "$1" -noout -text 2>/dev/null)" && printf '%s\n' "$text" | grep -q 'ASN1 OID: prime256v1' || return 1
  [ "$(/usr/bin/openssl ec -pubin -in "$1" -conv_form uncompressed -outform DER 2>/dev/null | wc -c | tr -d ' ')" = 91 ] || return 1
  /usr/bin/openssl ec -pubin -in "$1" -conv_form uncompressed -outform DER 2>/dev/null | /usr/bin/base64 | tr -d '\n'
}

keys="$out/admin_keys.h"
{
  echo "/* Written by win/admin/build.sh: the public keys payload.json's signature is checked with. */"
  echo "static const sig_key_text admin_key_texts[] = {"
  if [ -n "$test_key" ]; then
    k="$(pubkey_b64 "$test_key")" || { echo "$test_key isn't a P-256 public key" >&2; exit 1; }
    echo "    {\"test\", \"$k\"},"
  else
    for name in release backup; do
      f="$root/keys/$name.pub.pem"
      [ -f "$f" ] || { echo "keys/$name.pub.pem is missing" >&2; exit 1; }
      k="$(pubkey_b64 "$f")" || { echo "keys/$name.pub.pem isn't a P-256 public key" >&2; exit 1; }
      echo "    {\"$name\", \"$k\"},"
    done
  fi
  echo "};"
} > "$keys.part"
mv -f "$keys.part" "$keys"
extra=""
[ -z "$test_key" ] || extra="-DPLAT_TEST_PROGRAMS -DADMIN_TEST_HOLD"

cd "$root"   # relative names from here on (the prefix map makes the paths "."): the repo's path has spaces
"$cc" --tool x64 windres -I win/admin win/admin/admin.rc -O coff -o "$out/admin.res.o"
"$cc" x64 -w -ffunction-sections -fdata-sections -c -I"$pf" "$pf/pocketfft.c" -o "$out/pocketfft.o"   # plat_win.c's FFT (gc'd: unused here)
# shellcheck disable=SC2086
"$cc" x64 $WARN -Wno-unused-command-line-argument -mwindows -municode -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -DCORE_WIN_POLICY -DPLAT_NO_BLOCKS $extra -Icore -Icommon -Iwin/c -Iwin/admin -I"$out" -I"$pf" \
  win/admin/main.c win/admin/worker.c win/admin/worker_win.c win/c/inventory.c win/c/receipt.c win/c/pe_code.c \
  win/c/admin_proto.c win/c/sig.c win/c/sig_bcrypt.c win/c/payload.c win/c/payload_store.c win/c/fs_write.c win/c/compat.c win/c/compat_win.c \
  common/plat_win.c core/json.c core/arena.c "$out/admin.res.o" "$out/pocketfft.o" \
  -lntdll -ladvapi32 -lshell32 -lole32 -Wl,-Map="$out/rbsp-admin.exe.map" -o "$out/rbsp-admin.exe"
echo "$out/rbsp-admin.exe"
