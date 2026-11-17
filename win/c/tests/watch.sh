#!/bin/sh
# The watcher's part of win/c's tests (run.sh calls it; app.md §5.7, §7), with run.sh's warnings:
#  - watch.c and watch_facts.c on the Mac (watch_test.c): the notifications' rules, the folders'
#    handles on a fake file system, the schedule, the check on fake operations over the core's own
#    watcher decisions (core/watcher.c, core/state.c, built with CORE_WIN_POLICY as the Windows
#    builds are), and the facts on a scratch tree through plat_mac.c and a fake compat_os; plain
#    (-O2) and with ASan+UBSan, with WATCH_TEST_HOOKS (a text read as not written);
#  - rbsp-watch-x64.exe and rbsp-watch-arm64.exe as they ship (watch_win.c, the GUI subsystem,
#    its manifest and version resource, the core and plat_win.c linked statically), and their
#    gates: imports KERNEL32, ADVAPI32, USER32, SHELL32, ole32, COMCTL32, ntdll and
#    api-ms-win-crt-* only (app.md §7's allowlist for the watcher, I81; bcrypt, wintrust and
#    crypt32 are loaded from System32 at run time, and no core object refers to them), the
#    manifest there (TaskDialogIndirect needs comctl32 v6), the GUI subsystem;
#  - rbsp-watch-test-<arch>.exe, the VM's test build (WATCH_TEST_PATHS: the roots from the
#    environment, --notify-test), never shipped.
#
#   win/c/tests/watch.sh OUT PE     OUT: run.sh's build folder; PE: its PE fixtures. Exits 1 on a failure.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="$(cd "$1" && pwd)"
pe="$(cd "$2" && pwd)"
STD="-std=c11"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror"
MAC="-mmacosx-version-min=14.0"
WCC="scripts/win-cc.sh"
fails=0
cd "$root"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-watch.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT

# the core as the watcher links it (its pure decisions), the win/c modules it uses
CORE="core/watcher.c core/state.c core/text.c core/arena.c core/json.c core/helper_lines.c core/words.c core/schema.c"
WATCH="win/c/watch.c win/c/watch_facts.c win/c/compat.c win/c/payload.c win/c/payload_store.c win/c/fs_write.c win/c/sig.c"
FW="-framework Security -framework CoreFoundation -framework Accelerate"
for v in plain asan; do
  case "$v" in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
  esac
  # shellcheck disable=SC2086
  if clang $STD $WARN $MAC $san -DCORE_WIN_POLICY -DWATCH_TEST_HOOKS -Icore -Icommon $WATCH $CORE common/plat_mac.c win/c/tests/watch_test.c \
       win/c/tests/sig_mac.c $FW -o "$out/winc-watch-test-$v" > "$out/watch-build-$v.log" 2>&1; then
    d="$scratch/$v"; mkdir -p "$d"
    if "$out/winc-watch-test-$v" "$d" "$pe" > "$out/winc-watch-test-$v.log" 2>&1; then echo "ok   the watcher's tests ($v): $(tail -1 "$out/winc-watch-test-$v.log")"
    else tail -30 "$out/winc-watch-test-$v.log"; echo "FAIL the watcher's tests ($v)"; fails=$((fails + 1)); fi
  else
    cat "$out/watch-build-$v.log"; echo "FAIL the watcher's tests ($v) don't build"; fails=$((fails + 1))
  fi
done

# the compiled-in keys, as build-app.sh writes Keys.swift for a release: keys/release.pub.pem and
# keys/backup.pub.pem (public keys), each the base64 of its DER SubjectPublicKeyInfo
keysc="$out/watch_keys.c"
{
  echo "/* Written by win/c/tests/watch.sh: the public keys payload.json's signature is checked with. */"
  echo "#include \"watch.h\""
  echo "const sig_key_text watch_compiled_keys[] = {"
  for name in release backup; do
    k="$(/usr/bin/openssl ec -pubin -in "keys/$name.pub.pem" -outform DER 2>/dev/null | /usr/bin/base64 | tr -d '\n')"
    [ -n "$k" ] || { echo "FAIL keys/$name.pub.pem isn't a public key" >&2; exit 1; }
    echo "    {\"$name\", \"$k\"},"
  done
  echo "};"
  echo "const int watch_n_compiled_keys = 2;"
} > "$keysc"

# rbsp-watch-<arch>.exe as it ships: the GUI subsystem, its manifest and version resource, the core
# (an archive: only the decisions it uses are linked), plat_win.c, compat_win.c and sig_bcrypt.c
pf="$(scripts/win-deps.sh pocketfft)"
ver="$(tr -d '[:space:]' < VERSION)"
v1="${ver%%.*}"; rest="${ver#*.}"; v2="${rest%%.*}"; v3="${rest#*.}"; v3="${v3%%[!0-9]*}"
CORESRC="arena dialog dialog_texts flow flows helper_lines ids json lights mask menus model schema selftest state text watcher words core dump"
for a in x64 arm64; do
  d="$out/watch-$a"
  mkdir -p "$d"
  lib="$d/libcore.a"
  rm -f "$lib"
  ok=1
  for s in $CORESRC; do
    # shellcheck disable=SC2086
    "$WCC" "$a" $WARN -Wno-unused-command-line-argument -DCORE_WIN_POLICY -Icore -c "core/$s.c" -o "$d/core-$s.o" > "$d/build.log" 2>&1 \
      || { cat "$d/build.log"; echo "FAIL core/$s.c for Windows $a"; ok=0; break; }
  done
  # plat_win.c (PLAT_NO_BLOCKS, as the core and the worker build it) and pocketfft (its own warnings
  # off, never patched), as common/tests/win.sh builds them; in the archive too
  # shellcheck disable=SC2086
  [ "$ok" = 1 ] && { (cd common && "$root/$WCC" "$a" $WARN -Wno-unused-command-line-argument -DPLAT_NO_BLOCKS -I"$pf" -c plat_win.c -o "$d/core-plat_win.o") \
                       > "$d/build.log" 2>&1 && "$WCC" "$a" -w -c -I"$pf" "$pf/pocketfft.c" -o "$d/core-pocketfft.o" >> "$d/build.log" 2>&1 \
                       || { cat "$d/build.log"; echo "FAIL plat_win.c for Windows $a"; ok=0; }; }
  [ "$ok" = 1 ] && { "$WCC" --tool "$a" llvm-ar rcs "$lib" "$d"/core-*.o > "$d/build.log" 2>&1 || { cat "$d/build.log"; ok=0; }; }
  exe="$d/rbsp-watch-$a.exe"
  # the resources compiled next to their generated header (the repository's path has spaces)
  printf '#define V1 %s\n#define V2 %s\n#define V3 %s\n#define VSTR "%s"\n' "$v1" "$v2" "$v3" "$ver" > "$d/watch_version.h"
  cp win/c/watch.rc win/c/watch.manifest "$d/"
  # shellcheck disable=SC2086
  if [ "$ok" = 1 ] \
     && (cd "$d" && "$root/$WCC" --tool "$a" windres watch.rc -O coff -o watch.res.o) > "$d/build.log" 2>&1 \
     && "$WCC" "$a" $WARN -Wno-unused-command-line-argument -municode -mwindows -DCORE_WIN_POLICY "-DRBSP_VERSION=\"$ver\"" -Icore -Icommon -Iwin/c \
          win/c/watch_win.c win/c/watch.c win/c/watch_facts.c win/c/compat.c win/c/compat_win.c win/c/payload.c win/c/payload_store.c win/c/fs_write.c \
          win/c/sig.c win/c/sig_bcrypt.c "$keysc" "$d/watch.res.o" "$lib" \
          -lcomctl32 -lshell32 -lole32 -luuid -ladvapi32 -luser32 -lntdll -o "$exe" >> "$d/build.log" 2>&1; then
    imports="$("$WCC" --tool "$a" llvm-objdump -p "$exe" | sed -n 's/^ *DLL Name: //p' | sort -u | tr '\n' ' ')"
    # shellcheck disable=SC2086
    bad="$(printf '%s\n' $imports | grep -viE '^(KERNEL32|ADVAPI32|USER32|SHELL32|ole32|COMCTL32|ntdll|api-ms-win-crt-[a-z0-9-]+)\.dll$' || true)"
    # shellcheck disable=SC2086
    if ! printf '%s\n' $imports | grep -qx 'KERNEL32.dll'; then echo "FAIL rbsp-watch-$a.exe: no KERNEL32.dll in its imports: $imports"; fails=$((fails + 1))
    elif [ -n "$bad" ]; then echo "FAIL rbsp-watch-$a.exe imports $bad"; fails=$((fails + 1))
    elif "$WCC" --tool "$a" llvm-nm -u "$d"/core-*.o "$d/watch.res.o" 2>/dev/null | grep -E '(__imp_)?(BCrypt|WinVerifyTrust|WTHelper|Cert[A-Z]|Crypt[A-Z])'; then
      echo "FAIL the core's objects refer to a crypto function"; fails=$((fails + 1))
    elif ! "$WCC" --tool "$a" llvm-readobj --coff-resources "$exe" | grep -q 'Type: MANIFEST (ID 24)'; then
      echo "FAIL rbsp-watch-$a.exe has no manifest (TaskDialogIndirect needs comctl32 v6)"; fails=$((fails + 1))
    elif ! "$WCC" --tool "$a" llvm-objdump -p "$exe" | grep -q 'Subsystem.*(Windows GUI)'; then
      echo "FAIL rbsp-watch-$a.exe isn't a GUI subsystem exe"; fails=$((fails + 1))
    else
      echo "ok   rbsp-watch-$a.exe ($(wc -c < "$exe" | tr -d ' ') bytes) imports: $imports"
    fi
  else
    cat "$d/build.log"; echo "FAIL rbsp-watch-$a.exe doesn't build"; fails=$((fails + 1))
  fi
  # the VM's test build (W13a): Program Files from RBSP_TEST_PROGRAMS (plat_win.c's override,
  # PLAT_TEST_PROGRAMS, linked ahead of the archive's), the others from RBSP_TEST_APPDATA and
  # RBSP_TEST_LOCALAPPDATA, and --notify-test; never shipped
  # shellcheck disable=SC2086
  if [ "$ok" = 1 ] && (cd common && "$root/$WCC" "$a" $WARN -Wno-unused-command-line-argument -DPLAT_NO_BLOCKS -DPLAT_TEST_PROGRAMS -I"$pf" \
       -c plat_win.c -o "$d/test-plat_win.o") > "$d/test-build.log" 2>&1 \
     && "$WCC" "$a" $WARN -Wno-unused-command-line-argument -municode -mwindows -DCORE_WIN_POLICY -DWATCH_TEST_PATHS \
       "-DRBSP_VERSION=\"$ver\"" -Icore -Icommon -Iwin/c win/c/watch_win.c win/c/watch.c win/c/watch_facts.c win/c/compat.c \
       win/c/compat_win.c win/c/payload.c win/c/payload_store.c win/c/fs_write.c win/c/sig.c win/c/sig_bcrypt.c "$keysc" "$d/watch.res.o" "$d/test-plat_win.o" "$lib" \
       -lcomctl32 -lshell32 -lole32 -luuid -ladvapi32 -luser32 -lntdll -o "$d/rbsp-watch-test-$a.exe" >> "$d/test-build.log" 2>&1; then
    echo "ok   rbsp-watch-test-$a.exe (the VM's test build) builds"
    # Program Files: ProgramFilesDir read by both, the override in the test build only
    has16() { python3 -I -c 'import sys; sys.exit(0 if sys.argv[2].encode("utf-16-le") in open(sys.argv[1], "rb").read() else 1)' "$1" "$2"; }
    if has16 "$exe" RBSP_TEST_PROGRAMS; then echo "FAIL rbsp-watch-$a.exe reads RBSP_TEST_PROGRAMS"; fails=$((fails + 1))
    elif ! has16 "$d/rbsp-watch-test-$a.exe" RBSP_TEST_PROGRAMS; then echo "FAIL rbsp-watch-test-$a.exe doesn't read RBSP_TEST_PROGRAMS"; fails=$((fails + 1))
    elif ! has16 "$exe" ProgramFilesDir || ! has16 "$d/rbsp-watch-test-$a.exe" ProgramFilesDir; then echo "FAIL rbsp-watch ($a) doesn't read ProgramFilesDir"; fails=$((fails + 1))
    else echo "ok   rbsp-watch ($a): Program Files from ProgramFilesDir, RBSP_TEST_PROGRAMS in the test build only"; fi
  else
    cat "$d/test-build.log"; echo "FAIL rbsp-watch-test-$a.exe doesn't build"; fails=$((fails + 1))
  fi
done

[ "$fails" = 0 ] && echo "watch: all passed" || { echo "watch: $fails failed"; exit 1; }
