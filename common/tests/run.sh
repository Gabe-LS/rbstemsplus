#!/bin/sh
# common/'s unit tests (bridge.md §0a), for this Mac's architecture, with the core's warnings
# (C11, -Wpedantic, -Werror):
#  - plat_mac.c (tests/plat.c), with the fake clock: plain -O2, ASan+UBSan, TSan;
#  - the shared modules (tests/modules.c: cfg_ini.c and marker.c against tests/config-fixtures.txt and
#    tests/marker-fixtures.txt, path_rules.c, entries.c): plain -O2, ASan+UBSan; and with Windows'
#    path rule in cfg_ini.c (PATHS_WIN_FORM) against tests/config-fixtures-win.txt, ASan+UBSan;
#  - a chosen location's checks (location.c, tests/location.c; and tests/location_fake.c on a fake
#    platform layer): plain -O2, ASan+UBSan;
#  - plat_mac.c built as released (no PLAT_TEST_CLOCK): no fake clock in the object;
#  - the core's unit tests built on common/plat.h instead of core/plat_shim.h, the core's sources
#    untouched: its move off the shim is an include change (API-V1-NOTES C6).
#
#   common/tests/run.sh [OUT]       builds into OUT (default build/common); exits 1 on a failure
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
common="$(cd "$here/.." && pwd)"
root="$(cd "$common/.." && pwd)"
out="${1:-$root/build/common}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
STD="-std=c11"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Werror"
MAC="-mmacosx-version-min=12.0"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-common.XXXXXX")"
trap 'chmod -R u+w "$scratch" 2>/dev/null; rm -rf "${scratch:?}"' EXIT
fails=0

run() {   # NAME BINARY: the binary on a fresh scratch folder
  d="$scratch/$1"
  mkdir -p "$d"
  if "$2" "$d"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi
}

# plat_mac.c and its tests, three ways (relative names from common/: the repository's path has spaces)
for v in plain asan tsan; do
  case "$v" in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
    tsan) san="-O1 -g -fsanitize=thread" ;;
  esac
  # shellcheck disable=SC2086
  (cd "$common" && clang $STD $WARN $MAC $san -DPLAT_TEST_CLOCK -DPLAT_TEST_OWNER plat_mac.c plat_helper.c tests/plat.c \
    -framework CoreFoundation -framework Accelerate -o "$out/plat-test-$v")
  run "plat ($v)" "$out/plat-test-$v"
done

# plat_mac.c's pure rules fuzzed with ASan+UBSan (tests/plat_fuzz.c): the fgetattrlist name reply
# and the one-component name rule against win_rules.h's. FUZZ_ITERS per target (default 20000).
# shellcheck disable=SC2086
(cd "$common" && clang $STD $WARN $MAC -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
  tests/plat_fuzz.c -framework CoreFoundation -framework Accelerate -o "$out/plat-fuzz")
d="$scratch/fuzz"; mkdir -p "$d"
if "$out/plat-fuzz" "$d" "${FUZZ_ITERS:-20000}" > "$out/plat-fuzz.log" 2>&1; then echo "ok   plat fuzz ($(head -1 "$out/plat-fuzz.log"))"
else tail -20 "$out/plat-fuzz.log"; echo "FAIL plat fuzz"; fails=$((fails + 1)); fi

# the shared modules, the fixtures as the oracle
for v in plain asan; do
  case "$v" in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
  esac
  # shellcheck disable=SC2086
  (cd "$common" && clang $STD $WARN $MAC $san cfg_ini.c marker.c entries.c path_rules.c plat_mac.c tests/modules.c \
    -framework CoreFoundation -framework Accelerate -o "$out/modules-test-$v")
  d="$scratch/modules-$v"
  mkdir -p "$d"
  if "$out/modules-test-$v" "$d" "$root/tests"; then echo "ok   modules ($v)"; else echo "FAIL modules ($v)"; fails=$((fails + 1)); fi
done
# again with Windows' path rule in cfg_ini.c (PATHS_WIN_FORM, as the Windows builds read cache_dir),
# against tests/config-fixtures-win.txt
# shellcheck disable=SC2086
(cd "$common" && clang $STD $WARN $MAC -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
  -DPATHS_WIN_FORM cfg_ini.c marker.c entries.c path_rules.c plat_mac.c tests/modules.c \
  -framework CoreFoundation -framework Accelerate -o "$out/modules-test-win")
d="$scratch/modules-win"; mkdir -p "$d"
if "$out/modules-test-win" "$d" "$root/tests"; then echo "ok   modules (Windows' path rule, asan)"; else echo "FAIL modules (Windows' path rule)"; fails=$((fails + 1)); fi

# a chosen location's checks (location.c, T3c) on plat_mac.c: plain and ASan+UBSan
for v in plain asan; do
  case "$v" in
    plain) san="-O2" ;;
    asan) san="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
  esac
  # shellcheck disable=SC2086
  (cd "$common" && clang $STD $WARN $MAC $san location.c marker.c path_rules.c plat_mac.c tests/location.c \
    -framework CoreFoundation -framework Accelerate -o "$out/location-test-$v")
  d="$scratch/location-$v"; mkdir -p "$d"
  if "$out/location-test-$v" "$d"; then echo "ok   location ($v)"; else echo "FAIL location ($v)"; fails=$((fails + 1)); fi
  # and on a fake platform layer (tests/location_fake.c): the states no real volume takes on demand
  # shellcheck disable=SC2086
  (cd "$common" && clang $STD $WARN $MAC $san location.c marker.c path_rules.c tests/location_fake.c -o "$out/location-fake-$v")
  if "$out/location-fake-$v" > "$out/location-fake-$v.log" 2>&1; then echo "ok   location, fake layer ($v; $(tail -1 "$out/location-fake-$v.log"))"
  else cat "$out/location-fake-$v.log"; echo "FAIL location, fake layer ($v)"; fails=$((fails + 1)); fi
done

# as released: compiles with the warnings, has no fake clock, and never accepts the running
# account as an owner (a test of that build, with plat_owner_check's accept_self set)
# shellcheck disable=SC2086
(cd "$common" && clang $STD $WARN $MAC -O2 -c plat_mac.c -o "$out/plat_mac.o")
if nm "$out/plat_mac.o" | grep -q fake; then echo "FAIL plat_mac.o has the fake clock without PLAT_TEST_CLOCK"; fails=$((fails + 1));
else echo "ok   plat_mac.o as released: no fake clock"; fi
# shellcheck disable=SC2086
(cd "$common" && clang $STD $WARN $MAC -O2 plat_mac.c tests/owner_release.c -framework CoreFoundation -framework Accelerate -o "$out/owner-release")
d="$scratch/owner-release"; mkdir -p "$d"
if "$out/owner-release" "$d"; then echo "ok   plat_owner_check as released: accept_self ignored"; else echo "FAIL plat_owner_check as released accepts the running account"; fails=$((fails + 1)); fi

# the core's unit tests on plat.h: plat_shim.h's guard defined, plat.h included first; the source
# and test lists (and the vectors some tests read) come from core/build.sh, so they can't drift
csrcs="$("$root/core/build.sh" srcs)"
ctests="$("$root/core/build.sh" tests)"
"$root/core/build.sh" vectors "$out/core-vectors"
# shellcheck disable=SC2086
(cd "$root/core" && clang $STD $WARN $MAC -O2 -DCORE_TEST_HOOKS -DRBSP_CORE_PLAT_SHIM_H -include ../common/plat.h -I. -I"$out/core-vectors" \
  $csrcs $ctests ../common/plat_mac.c -framework CoreFoundation -framework Accelerate -o "$out/core-test-on-plat")
if "$out/core-test-on-plat" > "$out/core-test-on-plat.log" 2>&1; then echo "ok   core unit tests on plat.h ($(tail -1 "$out/core-test-on-plat.log"))"
else tail -20 "$out/core-test-on-plat.log"; echo "FAIL core unit tests on plat.h"; fails=$((fails + 1)); fi

# WINCORE=1: the core's sources compiled for Windows (x64 and arm64, CORE_WIN_POLICY) against
# common/plat.h with the pinned llvm-mingw (scripts/win-cc.sh fetches it), compile only: there is
# no plat_win.c yet. It needs win-w0's review fixes Q17 (windows.h's CALLBACK and TEXT undefined in
# core/internal.h) and Q36 (TEXT pushed and popped around core.h's X-macros: both of them, the text
# and the slot enums, since texts.def defaults TEXT only when it isn't defined); before them it
# fails on exactly those two macros. Opt-in until those merge (bridge-common part A, review Q17).
if [ "${WINCORE:-}" = 1 ]; then
  wf=0
  for a in x64 arm64; do
    for s in $("$root/core/build.sh" srcs); do
      # shellcheck disable=SC2086
      (cd "$root/core" && "$root/scripts/win-cc.sh" $a $STD $WARN -Wno-unused-command-line-argument -DCORE_WIN_POLICY \
        -DRBSP_CORE_PLAT_SHIM_H -include ../common/plat.h -I. -c "$s" -o /dev/null) > "$out/wincore.log" 2>&1 \
        || { echo "  $a $s:"; grep -m3 error "$out/wincore.log"; wf=$((wf + 1)); }
    done
  done
  if [ "$wf" = 0 ]; then echo "ok   the core on plat.h for Windows (x64, arm64)"; else echo "FAIL the core on plat.h for Windows: $wf sources"; fails=$((fails + 1)); fi
fi

[ "$fails" = 0 ] && echo "common: all passed" || { echo "common: $fails failed"; exit 1; }
