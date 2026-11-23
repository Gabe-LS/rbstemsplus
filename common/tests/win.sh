#!/bin/sh
# common/'s Windows layer, from the Mac (bridge.md §4 "On the Mac", T3a-T3d):
#  - common/win_rules.h's unit tests (the NTSTATUS and Win32 maps, the ACL rule, UTF-8/UTF-16,
#    NT paths, names, the mount match, time), run here: plain, and with ASan and UBSan;
#  - the vDSP vs pocketfft comparison (common/tests/irfft_cmp.c), run here;
#  - plat_win.c cross-built with the pinned llvm-mingw (scripts/win-cc.sh) for x64 and arm64 with
#    the core's warnings and -Werror, three ways: as the core and the worker build it
#    (PLAT_NO_BLOCKS), as the bridge and the helper do (-fblocks, BlocksRuntime's Block.h), and as
#    a test build (PLAT_TEST_CLOCK, PLAT_TEST_OWNER, PLAT_TEST_PROGRAMS, and PLAT_TEST_FLUSH:
#    plat_map_flush's injected ERROR_LOCK_VIOLATION, WF3, which no release object and no other
#    build script has);
#  - a DLL of plat_win.c + pocketfft per architecture, and its gates: the imports (KERNEL32,
#    ADVAPI32, ntdll, SHELL32, ole32, api-ms-win-crt-*: never bcrypt, which is loaded by its full
#    path, I31), no pthread_/_Unwind_/__cxa_ symbol (I52), and libmingwex members from a reviewed
#    list only (no Cephes-derived code, release.md §9);
#  - OUT/x64/plat-win-test.exe and OUT/arm64/plat-win-test.exe (common/tests/plat_win.c), each with
#    checked.dll beside it (common/tests/checked_dll.c), for the
#    VM: copy one to C:\rbtest\ and run `plat-win-test.exe C:\rbtest\scratch` there (unelevated);
#  - OUT/x64/location-win-test.exe and OUT/arm64/location-win-test.exe (common/tests/location_win.c,
#    T3c): `location-win-test.exe C:\rbtest\loc` in the VM (unelevated; RBSP_TEST_NET_DRIVE=Z: with
#    a network drive mapped adds its case), and its one-location modes (--make, --check, --gone)
#    for tests/win/suites/volumes.suite; their command line (location_args.h) is tested here;
#  - OUT/x64/location-fake-test.exe and OUT/arm64/location-fake-test.exe (common/tests/location_fake.c:
#    location.c on a fake platform layer, with the mapped network drive case): run anywhere in the
#    VM, no argument.
#
#   common/tests/win.sh [OUT]       default build/common-win; exits 1 on a failure
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
common="$(cd "$here/.." && pwd)"
root="$(cd "$common/.." && pwd)"
out="${1:-$root/build/common-win}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cc="$root/scripts/win-cc.sh"
pf="$("$root/scripts/win-deps.sh" pocketfft)"
brt="$("$root/scripts/win-deps.sh" blocksrt)"
STD="-std=c11"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror"
MAC="-mmacosx-version-min=12.0"
LIBS="-lntdll -ladvapi32 -lshell32 -lole32"   # what a program linking plat_win.c adds
fails=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
has16() { python3 -I -c 'import sys; sys.exit(0 if sys.argv[2].encode("utf-16-le") in open(sys.argv[1], "rb").read() else 1)' "$1" "$2"; }

# ---------------------------------------------------------------- on the Mac
# shellcheck disable=SC2086
(cd "$common" && clang $STD $WARN $MAC -O2 tests/win_rules.c -o "$out/win-rules" \
  && clang $STD $WARN $MAC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined tests/win_rules.c -o "$out/win-rules-asan")
if "$out/win-rules" > "$out/win-rules.log" && "$out/win-rules-asan" >> "$out/win-rules.log" 2>&1; then ok "win_rules ($(tail -1 "$out/win-rules.log"))"
else cat "$out/win-rules.log"; bad "win_rules"; fi

# location-win-test's command line (location_args.h: the volume cases' --make/--check/--gone and
# their path forms), by path_rules.c's Windows rule
# shellcheck disable=SC2086
(cd "$common" && clang $STD $WARN $MAC -O2 tests/location_args.c path_rules.c -o "$out/location-args" \
  && clang $STD $WARN $MAC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined tests/location_args.c path_rules.c -o "$out/location-args-asan")
if "$out/location-args" > "$out/location-args.log" 2>&1 && "$out/location-args-asan" >> "$out/location-args.log" 2>&1; then ok "location_args ($(tail -1 "$out/location-args.log"))"
else cat "$out/location-args.log"; bad "location_args"; fi

# pocketfft is someone else's code: built with its own warnings off, never patched
# shellcheck disable=SC2086
(cd "$common" && clang $STD $WARN -Wno-missing-prototypes $MAC -O2 -I"$pf" tests/irfft_cmp.c plat_mac.c "$pf/pocketfft.c" \
  -framework CoreFoundation -framework Accelerate -o "$out/irfft-cmp")
if "$out/irfft-cmp" > "$out/irfft-cmp.log" 2>&1; then ok "vDSP vs pocketfft ($(tail -1 "$out/irfft-cmp.log"); $(head -1 "$out/irfft-cmp.log"))"
else cat "$out/irfft-cmp.log"; bad "vDSP vs pocketfft"; fi

# ---------------------------------------------------------------- for Windows
for a in x64 arm64; do
  o="$out/$a"
  mkdir -p "$o"
  # shellcheck disable=SC2086
  if (cd "$common" && "$cc" $a $WARN -Wno-unused-command-line-argument -DPLAT_NO_BLOCKS -I"$pf" -c plat_win.c -o "$o/plat_win.o" \
      && "$cc" $a $WARN -Wno-unused-command-line-argument -fblocks -I"$pf" -I"$brt" -c plat_win.c -o "$o/plat_win-blocks.o" \
      && "$cc" $a $WARN -Wno-unused-command-line-argument -DPLAT_TEST_CLOCK -DPLAT_TEST_OWNER -DPLAT_TEST_PROGRAMS -DPLAT_TEST_FLUSH -DPLAT_TEST_REMOVE_TREE -I"$pf" -c plat_win.c -o "$o/plat_win-test.o") \
      > "$o/cc.log" 2>&1; then ok "$a: plat_win.c builds three ways with -Werror"
  else cat "$o/cc.log"; bad "$a: plat_win.c"; continue; fi
  "$cc" $a -w -c -I"$pf" "$pf/pocketfft.c" -o "$o/pocketfft.o"
  # the DLL a consumer would link it into (exports nothing of its own) and the test program
  # shellcheck disable=SC2086
  "$cc" $a -shared "$o/plat_win.o" "$o/pocketfft.o" $LIBS -Wl,-Map="$o/plat_win.map" -o "$o/plat_win.dll" > "$o/link.log" 2>&1 \
    || { cat "$o/link.log"; bad "$a: link"; continue; }
  # shellcheck disable=SC2086
  (cd "$common" && "$cc" $a $WARN -Wno-unused-command-line-argument -DPLAT_TEST_CLOCK -DPLAT_TEST_OWNER -DPLAT_TEST_PROGRAMS -DPLAT_TEST_FLUSH -DPLAT_TEST_REMOVE_TREE -I"$pf" tests/plat_win.c \
    "$o/plat_win-test.o" "$o/pocketfft.o" $LIBS -o "$o/plat-win-test.exe") > "$o/test-cc.log" 2>&1 \
    || { cat "$o/test-cc.log"; bad "$a: plat-win-test.exe"; continue; }
  # T3c's: a chosen location's checks (location.c), the volume calls with GUID and long paths,
  # cfg_ini.c's reader with Windows' path rule
  # shellcheck disable=SC2086
  (cd "$common" && "$cc" $a $WARN -Wno-unused-command-line-argument -I"$pf" tests/location_win.c location.c marker.c path_rules.c cfg_ini.c \
    "$o/plat_win-test.o" "$o/pocketfft.o" $LIBS -o "$o/location-win-test.exe") > "$o/location-cc.log" 2>&1 \
    || { cat "$o/location-cc.log"; bad "$a: location-win-test.exe"; continue; }
  # location.c on a fake platform layer (tests/location_fake.c), with Windows' network drive case
  # shellcheck disable=SC2086
  (cd "$common" && "$cc" $a $WARN -Wno-unused-command-line-argument tests/location_fake.c location.c marker.c path_rules.c \
    -o "$o/location-fake-test.exe") > "$o/location-fake-cc.log" 2>&1 \
    || { cat "$o/location-fake-cc.log"; bad "$a: location-fake-test.exe"; continue; }
  # the DLL its plat_lib_open_checked cases load, next to it
  (cd "$common" && "$cc" $a -shared -Wall -Wextra -Werror tests/checked_dll.c -o "$o/checked.dll") > "$o/checked-cc.log" 2>&1 \
    || { cat "$o/checked-cc.log"; bad "$a: checked.dll"; continue; }
  dlls="$("$cc" --tool $a llvm-objdump -p "$o/plat_win.dll" | sed -n 's/.*DLL Name: //p' | sort -u | tr '\n' ' ')"
  extra="$(echo "$dlls" | tr ' ' '\n' | grep -v -i -e '^$' -e '^kernel32\.dll$' -e '^advapi32\.dll$' -e '^ntdll\.dll$' -e '^shell32\.dll$' \
    -e '^ole32\.dll$' -e '^api-ms-win-crt-' || true)"
  if [ -z "$extra" ]; then ok "$a: imports $dlls"; else bad "$a: imports outside the allowlist: $extra"; fi
  # libmingwex members: a reviewed allowlist (release.md §9: no Cephes-derived code). dllmain.o is
  # mingw's default DllMain, there only because this test DLL has none of its own; the others are
  # Control Flow Guard support
  case $a in
    x64) want="lib64_libmingwex_a-dllmain.o lib64_libmingwex_a-guard_dispatch.o lib64_libmingwex_a-loadcfg.o lib64_libmingwex_a-mingw_cfguard_support.o " ;;
    *) want="libarm64_libmingwex_a-dllmain.o libarm64_libmingwex_a-loadcfg.o libarm64_libmingwex_a-mingw_cfguard_support.o " ;;
  esac
  mx="$(grep -oE '[A-Za-z0-9_]*libmingwex_a-[A-Za-z0-9_]+\.o' "$o/plat_win.map" | sort -u | tr '\n' ' ')"
  if [ "$mx" = "$want" ]; then ok "$a: libmingwex members: the reviewed list"; else bad "$a: libmingwex members outside the reviewed list: $mx"; fi
  syms="$("$cc" --tool $a llvm-nm "$o/plat_win.dll" 2> /dev/null | grep -cE 'pthread_|_Unwind_|__cxa_' || true)"
  if [ "$syms" = 0 ]; then ok "$a: no pthread_/_Unwind_/__cxa_ symbol"; else bad "$a: $syms pthread_/_Unwind_/__cxa_ symbols"; fi
  if "$cc" --tool $a llvm-nm "$o/plat_win.o" | grep -q fake; then bad "$a: plat_win.o has the fake clock without PLAT_TEST_CLOCK"
  else ok "$a: plat_win.o as released: no fake clock"; fi
  # the Program Files rule's test override (RBSP_TEST_PROGRAMS, UTF-16) only in the test build
  if has16 "$o/plat_win.o" RBSP_TEST_PROGRAMS || has16 "$o/plat_win-blocks.o" RBSP_TEST_PROGRAMS; then bad "$a: plat_win.o as released reads RBSP_TEST_PROGRAMS"
  elif ! has16 "$o/plat_win-test.o" RBSP_TEST_PROGRAMS; then bad "$a: the test build doesn't read RBSP_TEST_PROGRAMS"
  else ok "$a: RBSP_TEST_PROGRAMS in the test build only"; fi
  # plat_map_flush's injected flush faults (PLAT_TEST_FLUSH, WF3) only in the test build
  if "$cc" --tool $a llvm-nm "$o/plat_win.o" "$o/plat_win-blocks.o" "$o/plat_win.dll" 2> /dev/null | grep -q plat_test_flush; then
    bad "$a: plat_win.o as released has the test flush faults"
  elif ! "$cc" --tool $a llvm-nm "$o/plat_win-test.o" | grep -q plat_test_flush_faults; then bad "$a: the test build has no test flush faults"
  else ok "$a: the test flush faults in the test build only"; fi
done
# PLAT_TEST_FLUSH named by its four files alone, anywhere in the tracked tree (no other build, the
# core DLL's, the app's, the watcher's, the worker's, may define it); git grep's 1 is "none", more
# is a failed scan (no repository, say), never a pass
rc=0
others="$(git -C "$root" grep -l PLAT_TEST_FLUSH -- . ':!common/plat_win.c' ':!common/plat_win.h' ':!common/tests/plat_win.c' ':!common/tests/win.sh' 2> /dev/null)" || rc=$?
if [ "$rc" -gt 1 ]; then bad "PLAT_TEST_FLUSH scan: git grep failed ($rc)"
elif [ -z "$others" ]; then ok "PLAT_TEST_FLUSH named by its four files only"; else bad "PLAT_TEST_FLUSH named elsewhere: $others"; fi

if [ "$fails" = 0 ]; then echo "common (Windows): all passed; for the VM: $out/x64/plat-win-test.exe, $out/arm64/plat-win-test.exe, $out/x64/location-win-test.exe, $out/arm64/location-win-test.exe, $out/x64/location-fake-test.exe, $out/arm64/location-fake-test.exe"
else echo "common (Windows): $fails failed"; exit 1; fi
