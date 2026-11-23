#!/bin/sh
# The Windows bridge, onnxruntime.dll for x64 (rekordbox is x64, also under Prism on ARM64), built on
# the Mac with the pinned llvm-mingw (scripts/win-cc.sh), and its gates (bridge.md §4 item 3, T5).
# This is bridge.md's `build.sh --win`. bridge/build.sh itself is left as it is: it is a Mac input
# (release.md §1.3), and a change to it would cost the next release its carry-forward; it can hand
# `--win` over to this script when it changes anyway (T1b2).
#
#   bridge/win/build.sh [OUT]                 release build into OUT (default build/bridge-win)
#   bridge/win/build.sh --test REAL_PATH OUT  for the tests only: the real library from REAL_PATH (a
#                                             Windows path, e.g. C:\rbtest\ort\onnxruntime.dll)
#                                             instead of the contract path (or, with
#                                             RBSP_TEST_PROGRAMS set, the contract path under that
#                                             folder: PLAT_TEST_PROGRAMS), the running account
#                                             accepted as its owner, RBSTEMS_TEST_HOME read; plus
#                                             OUT/tests/: stubcall.exe, llcheck.exe, llcaller.dll,
#                                             fake-nov18/onnxruntime.dll (GetApi(18) NULL),
#                                             harness.exe (bridge/tests/harness.c's groups, T6:
#                                             run from the test bridge's folder; it imports
#                                             onnxruntime.dll by ordinal only, as rekordbox.exe
#                                             does) and fake/onnxruntime.dll (fake_ort.c, 1.17.0,
#                                             the real library's ordinals 1-4)
#
# The DLL is the Mac bridge's own code (bridge/rbstems_bridge.c, its _WIN32 branches: the Mac's bytes
# don't change, make mac-unchanged) on common/plat_win.c, with the entry points (entry_win.c), libFLAC
# and BlocksRuntime (scripts/win-deps.sh blocksrt, the Phase 0 blocks spike's) linked statically, in
# one translation unit (bridge/win/bridge_win.c). Its caps line is the Mac bridge's. Built and gated
# here; it runs only in the VM (bridge.md T7, T8, T10).
#
# Gates, in the style of bridge/build.sh's checks:
#  - bridge/win/stub_api.inc is what gen_stub.py makes of the vendored header today;
#  - the Mac tests of the stub and of ort_win.h (bridge/win/tests/run.sh);
#  - libFLAC (bridge/build.sh's pinned FLAC_VERSION and FLAC_SHA256) cross-built for x86_64 with
#    --disable-multithreading and FLAC__NO_DLL, into OUT/flac (static; the DLL links it), with no
#    pthread_, _Unwind_ or __cxa_ symbol in it (I52);
#  - exports exactly ordinals 1-5, with their names (no NONAME or forwarded row; bridge/win/pe.sh);
#  - imports only KERNEL32, ADVAPI32, ntdll, SHELL32, ole32 and api-ms-win-crt-* (never bcrypt: it
#    is loaded by its full path, I31), and no delay-loaded import;
#  - no pthread_, _Unwind_ or __cxa_ symbol linked in (I52; the symbol table and the link map);
#  - libmingwex members only from a reviewed list (no Cephes-derived code, release.md §9);
#  - the link's warnings: none but LNK4217 on the four block ABI symbols (BlocksRuntime linked in);
#  - PE32+ x86-64, DllCharacteristics 0x160, no resource section (no VERSIONINFO: MSI, §2);
#  - "rbstems bridge: " and the "rbstems-cache: " caps line present;
#  - a release build has the contract path (<Program Files>\RB Stems Plus\ort\onnxruntime.dll, Program
#    Files from HKLM's ProgramFilesDir: the key and the value's names in UTF-16, no C:\Program Files)
#    and no RBSTEMS_TEST_, RBSP_TEST_ or TEST BUILD (as UTF-8 or UTF-16);
#  - built twice (the second time from a copy of the sources under another path, with a space)
#    gives the same bytes.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
bridge="$(cd "$here/.." && pwd)"
root="$(cd "$bridge/.." && pwd)"
cc="$root/scripts/win-cc.sh"
REAL_ORT='RB Stems Plus\ort\onnxruntime.dll'   # under Windows' own Program Files (common/win_rules.h's WR_PF_REAL_ORT)
LIB=onnxruntime.dll
LIBS="-lntdll -ladvapi32 -lshell32 -lole32"

test_path=
if [ "${1:-}" = "--test" ]; then
  [ $# -eq 3 ] || { echo "usage: $0 --test REAL_PATH OUT" >&2; exit 2; }
  test_path="$2"
  case "$test_path" in *'"'*) echo "REAL_PATH can't contain \"" >&2; exit 2 ;; esac
  case "$test_path" in [A-Za-z]:\\*) ;; *) echo "REAL_PATH must be an absolute Windows path (X:\\...)" >&2; exit 2 ;; esac
  shift 2
fi
out="${1:-$root/build/bridge-win}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
fail() { echo "bridge/win/build.sh: $*" >&2; exit 1; }
pf="$("$root/scripts/win-deps.sh" pocketfft)"
rt="$("$root/scripts/win-deps.sh" blocksrt)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-bridge-win.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT

# ---------------------------------------------------------------- the stub, from the header

printf '#include "onnxruntime_c_api.h"\n' > "$scratch/h.c"
"$cc" --tool x64 clang -I"$bridge" -fsyntax-only -Xclang -ast-dump=json \
  -Xclang -ast-dump-filter=OrtApi "$scratch/h.c" > "$scratch/ast.json"
python3 -I "$here/gen_stub.py" "$scratch/ast.json" > "$scratch/stub_api.inc"
if ! cmp -s "$scratch/stub_api.inc" "$here/stub_api.inc"; then
  cp "$scratch/stub_api.inc" "$out/stub_api.inc.new"
  fail "bridge/win/stub_api.inc isn't what gen_stub.py makes of bridge/onnxruntime_c_api.h: regenerate it ($out/stub_api.inc.new)"
fi
"$here/tests/run.sh" "$out/mac-tests" || fail "the Mac tests of the stub and ort_win.h"

# ---------------------------------------------------------------- libFLAC for Windows

# the pin is the Mac bridge's (one place: bridge/build.sh)
FLAC_VERSION="$(sed -n 's/^FLAC_VERSION=//p' "$bridge/build.sh")"
FLAC_SHA256="$(sed -n 's/^FLAC_SHA256=//p' "$bridge/build.sh")"
tarball="$root/build/flac/flac-$FLAC_VERSION.tar.xz"
mkdir -p "$root/build/flac"
if [ ! -f "$tarball" ] || [ "$(shasum -a 256 "$tarball" | cut -d' ' -f1)" != "$FLAC_SHA256" ]; then
  curl -fsSL -o "$tarball.part" "https://downloads.xiph.org/releases/flac/flac-$FLAC_VERSION.tar.xz"
  [ "$(shasum -a 256 "$tarball.part" | cut -d' ' -f1)" = "$FLAC_SHA256" ] || { rm -f "$tarball.part"; fail "flac tarball: wrong sha256"; }
  mv -f "$tarball.part" "$tarball"
fi
# libtool can't take the repository's path (spaces): the tools through wrappers in the scratch folder
tc="$root/build/llvm-mingw/bin"
printf '#!/bin/sh\nexec "%s" x64 -Wno-unused-command-line-argument "$@"\n' "$cc" > "$scratch/cc"
for t in ar ranlib nm; do printf '#!/bin/sh\nexec "%s/llvm-%s" "$@"\n' "$tc" "$t" > "$scratch/$t"; done
chmod +x "$scratch/cc" "$scratch/ar" "$scratch/ranlib" "$scratch/nm"
mkdir -p "$scratch/flac"
tar -xJf "$tarball" -C "$scratch/flac"
# lt_cv_*: libtool can't tell lld and llvm-ar from MSVC's tools for a mingw host by itself;
# __USE_MINGW_ANSI_STDIO=0 (here and on the DLL's own compile): the UCRT's printf and strtod for
# the plain calls, ours and libFLAC's, as helper/build-win.sh builds its libFLAC. It can't keep
# mingw's own out: libFLAC's share/compat.h calls __mingw_vsnprintf by name, so mingw_vsnprintf,
# pformat and gdtoa are linked in, through the reviewed libmingwex list below
(
  cd "$scratch/flac/flac-$FLAC_VERSION"
  ./configure --quiet --host=x86_64-w64-mingw32 --enable-static --disable-shared --disable-ogg --disable-programs \
    --disable-examples --disable-cpplibs --disable-doxygen-docs --disable-version-from-git --disable-multithreading \
    CC="$scratch/cc" AR="$scratch/ar" RANLIB="$scratch/ranlib" NM="$scratch/nm" \
    lt_cv_ar_interface=ar lt_cv_prog_gnu_ld=yes lt_cv_sys_max_cmd_len=8192 CFLAGS="-O2 -DFLAC__NO_DLL -D__USE_MINGW_ANSI_STDIO=0"
  make -s -j"$(sysctl -n hw.ncpu 2> /dev/null || echo 4)" -C src/libFLAC
) > "$out/flac-build.log" 2>&1 || { tail -30 "$out/flac-build.log" >&2; fail "libFLAC's Windows build"; }
rm -rf "$out/flac"
mkdir -p "$out/flac/lib" "$out/flac/include/FLAC"
cp "$scratch/flac/flac-$FLAC_VERSION/src/libFLAC/.libs/libFLAC.a" "$out/flac/lib/"
cp "$scratch/flac/flac-$FLAC_VERSION/include/FLAC/"*.h "$out/flac/include/FLAC/"
n="$("$tc/llvm-nm" "$out/flac/lib/libFLAC.a" 2> /dev/null | grep -cE 'pthread_|_Unwind_|__cxa_' || true)"
[ "$n" = 0 ] || fail "libFLAC for Windows refers to $n pthread_/_Unwind_/__cxa_ symbols (--disable-multithreading?)"

# ---------------------------------------------------------------- the DLL

# The release flags come from win-cc.sh. A test build: the path as a C string (backslashes doubled)
note="release"
if [ -n "$test_path" ]; then
  esc="$(printf '%s' "$test_path" | sed 's/\\/\\\\/g')"
  note="TEST: real library from $test_path"
fi
build() {   # SRC_BRIDGE_DIR OUT_DLL MAP
  src="$1" o="$2" map="$3"
  set --
  [ -z "$test_path" ] || set -- "$@" "-DRBSTEMS_TEST_REAL_PATH=\"$esc\"" -DPLAT_TEST_OWNER -DPLAT_TEST_PROGRAMS
  # BlocksRuntime as the blocks spike builds it: its own Windows branch passes an int * to
  # InterlockedCompareExchange (32-bit on LLP64, harmless), compiled as it is, its warnings off
  for r in runtime data; do
    "$cc" x64 -w -Wno-incompatible-pointer-types -c -I"$rt" "$rt/$r.c" -o "$o.$r.o" || fail "BlocksRuntime's $r.c"
  done
  # pocketfft (plat_irfft: rebuild_spec's inverse FFT), compiled as it is
  "$cc" x64 -w -c -I"$pf" "$pf/pocketfft.c" -o "$o.pocketfft.o" || fail "pocketfft.c"
  # shellcheck disable=SC2086
  (cd "$src/win" && "$cc" x64 -shared -fblocks -Wall -Wextra -Werror -Wno-unused-command-line-argument -DFLAC__NO_DLL -D__USE_MINGW_ANSI_STDIO=0 "$@" \
    -I.. -I../../common -I"$pf" -I"$rt" -I"$out/flac/include" bridge_win.c "$o.runtime.o" "$o.data.o" "$o.pocketfft.o" onnxruntime.def \
    "$out/flac/lib/libFLAC.a" $LIBS -Wl,-Map="$map" -o "$o") 2> "$o.link.log" || { cat "$o.link.log" >&2; fail "the build failed"; }
}
dll="$out/$LIB"
build "$bridge" "$dll" "$out/$LIB.map"
# the same sources from another folder (with a space in its path): the same bytes
mkdir -p "$scratch/copy of src/bridge/win" "$scratch/copy of src/common" "$scratch/copy of src/win/c" "$scratch/second"
cp "$bridge"/*.h "$bridge"/rbstems_bridge.c "$scratch/copy of src/bridge/"
cp "$here"/*.c "$here"/*.h "$here"/*.inc "$here"/*.def "$scratch/copy of src/bridge/win/"
cp "$root"/common/*.c "$root"/common/*.h "$scratch/copy of src/common/"
cp "$root"/win/c/volumes.c "$root"/win/c/volumes_win.c "$root"/win/c/volumes.h "$scratch/copy of src/win/c/"
build "$scratch/copy of src/bridge" "$scratch/second/$LIB" "$scratch/second/$LIB.map"

# ---------------------------------------------------------------- gates

pe_dump() { "$cc" --tool x64 llvm-objdump "$@"; }   # the pinned llvm-objdump, never the host's
. "$here/pe.sh"   # pe_exports, pe_imports (tested by tests/run.sh)
hdr="$(pe_dump -p "$dll")"
echo "$hdr" | grep -q '^Magic.*020b' || fail "not PE32+"
pe_dump -f "$dll" | grep -qi 'file format coff-x86-64' || fail "not x86-64"
echo "$hdr" | grep -qE '^DllCharacteristics[[:space:]]+00000160$' || fail "DllCharacteristics: $(echo "$hdr" | grep DllCharacteristics)"
# every row of the Export Table, named or not (NONAME: ord:-), and none forwarded
exports="$(printf '%s\n' "$hdr" | pe_exports)" || fail "exports: a forwarded or unknown row"
want="1:OrtGetApiBase 2:OrtSessionOptionsAppendExecutionProviderEx_DML 3:OrtSessionOptionsAppendExecutionProvider_CPU 4:OrtSessionOptionsAppendExecutionProvider_DML 5:rbstems_marker"
[ "$exports" = "$want" ] || fail "exports: $exports"
dlls="$(echo "$hdr" | sed -n 's/.*DLL Name: //p' | sort -u | tr '\n' ' ')"
extra="$(echo "$dlls" | tr ' ' '\n' | grep -v -i -e '^$' -e '^kernel32\.dll$' -e '^advapi32\.dll$' -e '^ntdll\.dll$' -e '^shell32\.dll$' \
  -e '^ole32\.dll$' -e '^api-ms-win-crt-' || true)"
[ -z "$extra" ] || fail "imports outside the allowlist: $extra"
# no delay-loaded import either: the allowlist reads only the static import table
echo "$hdr" | grep -qE '^Entry d 0+ 0+ Delay Import Directory$' || fail "a delay import"
# libmingwex members linked in: an allowlist, so any new member is reviewed first; Cephes-derived
# ones have an unclear licence (release.md §9). The three Control Flow Guard ones, and mingw-w64's
# own vsnprintf, which libFLAC's share/compat.h calls by name (__mingw_vsnprintf) on mingw:
# mingw_vsnprintf, mingw_pformat and David Gay's gdtoa (gdtoa, dmisc, gmisc, misc), under the
# runtime licence file's gdtoa notice (win/NOTICE.txt), not Cephes (helper/build-win.sh's reviewed
# list). Negative check (review B9): one call to erfl in bridge_win.c links erfl.o, expl.o,
# floorl.o and __mingw_raise_matherr_ret.o, and this gate fails
mx="$(grep -oE 'lib64_libmingwex_a-[A-Za-z0-9_]+\.o' "$out/$LIB.map" | sort -u | tr '\n' ' ')"
bad="$(echo "$mx" | tr ' ' '\n' | grep -v -e '^$' -e '^lib64_libmingwex_a-guard_dispatch\.o$' -e '^lib64_libmingwex_a-loadcfg\.o$' \
  -e '^lib64_libmingwex_a-mingw_cfguard_support\.o$' -e '^lib64_libmingwex_a-mingw_vsnprintf\.o$' -e '^lib64_libmingwex_a-mingw_pformat\.o$' \
  -e '^lib64_libmingwex_a-gdtoa\.o$' -e '^lib64_libmingwex_a-dmisc\.o$' -e '^lib64_libmingwex_a-gmisc\.o$' -e '^lib64_libmingwex_a-misc\.o$' || true)"
[ -z "$bad" ] || fail "libmingwex members outside the reviewed list (no Cephes, release.md §9): $(echo $bad)"
syms="$("$cc" --tool x64 llvm-nm "$dll" 2> /dev/null | grep -cE 'pthread_|_Unwind_|__cxa_' || true)"
mapped="$(grep -cE 'pthread_|_Unwind_|__cxa_' "$out/$LIB.map" || true)"
[ "$syms" = 0 ] && [ "$mapped" = 0 ] || fail "pthread_/_Unwind_/__cxa_ symbols linked in ($syms in the symbols, $mapped in the map)"
warn="$(grep -c 'warning' "$dll.link.log" || true)"
l4217="$(grep -cE 'locally defined symbol imported: (_NSConcreteGlobalBlock|_NSConcreteStackBlock|_Block_object_assign|_Block_object_dispose) ' "$dll.link.log" || true)"
[ "$warn" = "$l4217" ] || { cat "$dll.link.log" >&2; fail "link warnings other than the four block symbols' LNK4217"; }
if pe_dump -h "$dll" | grep -q '\.rsrc'; then fail "a resource section (no VERSIONINFO on the bridge)"; fi
grep -aq "rbstems bridge: " "$dll" || fail "the \"rbstems bridge: \" marker string is missing"
grep -aq "rbstems-cache: " "$dll" || fail "the rbstems-cache: caps line is missing"
if [ -z "$test_path" ]; then
  grep -aqF "$REAL_ORT" "$dll" || fail "the contract path's $REAL_ORT is not in the release build"
  python3 -I -c 'import sys; b = open(sys.argv[1], "rb").read(); sys.exit(not all(w.encode("utf-16-le") in b for w in sys.argv[2:]))' "$dll" \
    'SOFTWARE\Microsoft\Windows\CurrentVersion' ProgramFilesDir || fail "the release build doesn't read ProgramFilesDir"
  if grep -aqiF 'C:\Program Files' "$dll"; then fail "a release build names C:\\Program Files"; fi
  if grep -aq "TEST BUILD" "$dll"; then fail "a release build contains the test path override"; fi
  if grep -aqE "RBSTEMS_TEST_|RBSP_TEST_" "$dll"; then fail "a release build reads test settings from the environment"; fi
  # the Windows calls take UTF-16 names: the same words as UTF-16LE
  python3 -I -c 'import sys; b = open(sys.argv[1], "rb").read(); sys.exit(any(w.encode("utf-16-le") in b for w in ("RBSTEMS_TEST_", "RBSP_TEST_", "TEST BUILD", "C:\\Program Files")))' "$dll" \
    || fail "a release build has RBSTEMS_TEST_, RBSP_TEST_, TEST BUILD or C:\\Program Files in UTF-16"
fi
a="$(shasum -a 256 "$dll" | cut -d' ' -f1)"
b="$(shasum -a 256 "$scratch/second/$LIB" | cut -d' ' -f1)"
[ "$a" = "$b" ] || fail "not reproducible: $a here, $b from a copy of the sources"

# ---------------------------------------------------------------- the VM's test programs

if [ -n "$test_path" ]; then
  t="$out/tests"
  mkdir -p "$t"
  (cd "$here/tests" && "$cc" x64 -Wall -Wextra -Werror -Wno-unused-command-line-argument -I../.. stubcall.c -o "$t/stubcall.exe")
  # R1's version mismatch (review B4): a real library whose GetApi(18) is NULL
  mkdir -p "$t/fake-nov18"
  (cd "$here/tests" && "$cc" x64 -shared -Wall -Wextra -Werror -Wno-unused-command-line-argument -I../.. fake_ort_nov18.c "$root/tests/win/probes/ort.def" \
    -o "$t/fake-nov18/onnxruntime.dll" 2> "$t/fake-nov18/link.log") \
    || { cat "$t/fake-nov18/link.log" >&2; fail "the fake library without version 18"; }
  (cd "$root/tests/win/probes" && "$cc" x64 -Wall -Wextra -Werror -I"$bridge" llcheck.c -o "$t/llcheck.exe" \
    && "$cc" x64 -shared -Wall -Wextra -Werror -DCALLER -I"$bridge" llcheck.c -o "$t/llcaller.dll")
  # T6: the harness, linked through a NONAME import library (tests/onnxruntime-ordinal.def), so it
  # imports the bridge by ordinal only, as rekordbox.exe does: checked in its import table
  "$cc" --tool x64 dlltool -d "$here/tests/onnxruntime-ordinal.def" -l "$t/onnxruntime-ordinal.lib" || fail "the ordinal import library"
  (cd "$bridge/tests" && "$cc" x64 -Wall -Wextra -Werror -Wno-unused-command-line-argument -I.. harness.c "$t/onnxruntime-ordinal.lib" \
    -o "$t/harness.exe") || fail "harness.exe didn't build"
  # (every descriptor named onnxruntime.dll in any case: Windows reads them as the one DLL)
  imp="$("$cc" --tool x64 llvm-readobj --coff-imports "$t/harness.exe" | pe_imports onnxruntime.dll)"
  [ "$imp" = "(1) (2) (3) (4)" ] || fail "harness.exe doesn't import onnxruntime.dll by ordinals 1-4 alone: $imp"
  # BW4: harness.c imports with dllimport, so harness.exe has no import thunk for the four: with
  # one, an import's address is the thunk's, and owner() of it names harness.exe, not the bridge
  thunks="$(pe_dump -d "$t/harness.exe" | grep -E '^[0-9a-f]+ <(OrtGetApiBase|OrtSessionOptionsAppendExecutionProvider(Ex_DML|_CPU|_DML))>:' || true)"
  [ -z "$thunks" ] || fail "harness.exe has an import thunk for the bridge (built without ORT_DLL_IMPORT?): $(echo $thunks)"
  # its 1.17.0 stand-in real library, with the real library's ordinals
  mkdir -p "$t/fake"
  (cd "$bridge/tests" && "$cc" x64 -shared -Wall -Wextra -Werror -Wno-unused-command-line-argument fake_ort.c "$root/tests/win/probes/ort.def" \
    -o "$t/fake/onnxruntime.dll" 2> "$t/fake/link.log") || { cat "$t/fake/link.log" >&2; fail "the 1.17.0 fake library"; }
  fx="$(pe_dump -p "$t/fake/onnxruntime.dll" | pe_exports)" || fail "the fake library's exports: a forwarded or unknown row"
  [ "$fx" = "1:OrtGetApiBase 2:OrtSessionOptionsAppendExecutionProviderEx_DML 3:OrtSessionOptionsAppendExecutionProvider_CPU 4:OrtSessionOptionsAppendExecutionProvider_DML" ] \
    || fail "the fake library's exports aren't the real library's ordinals 1-4 alone: $fx"
fi
printf 'built %s (x64, %s; sha256 %s)\n' "$dll" "$note" "$a"
