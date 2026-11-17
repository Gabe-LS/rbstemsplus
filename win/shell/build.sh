#!/bin/sh
# The Windows shell (app.md §3-4), built on the Mac with FOSS tools only, then its Mac tests:
#   - the generated ids: core/build.sh ids (CoreIds.cs from core's .def tables, CORE_WIN_POLICY);
#   - the core DLLs: core/build.sh dll (x64\ and arm64\rbsp-core.dll, the pinned llvm-mingw);
#   - "RB Stems Plus.exe": WPF on .NET Framework 4.8.1, AnyCPU (native on x64 and ARM64), by the
#     pinned .NET SDK tarball (global.json names it), restored in locked mode from nuget.org
#     (packages.lock.json; the net481 reference assemblies, MIT, build only: nothing from NuGet
#     ships). No MSVC, no Windows SDK;
#   - OUT/app: the exe, its .exe.config and the two DLLs, laid out as the version folder;
#   - the Mac tests (win/shell/tests, unless --no-test): the shell's WPF-free C# (the host table,
#     the queue, the view model, the dialog model, the log) on .NET 10 for macOS from the same
#     SDK, against the core built here as a dylib with CORE_WIN_POLICY and the test hooks; they
#     check the generated ids against that core's (core_ids_hash) first.
#
#   win/shell/build.sh [--relock] [--no-test] [--test-app [--test-base URL --test-key PEM]] [--x64]
#                      [--prepare] [--app-version V] [OUT]             OUT defaults to build/shell
#   --relock: restore unlocked once and rewrite win/shell/packages.lock.json (review the diff)
#   --test-app: also OUT/app-test, a TEST build (SHELL_TEST: TestHooks.cs's triggers for the VM
#               smoke) over the test-hooks core DLLs (the fake helper, RBSP_TEST_PROGRAMS), with
#               fake-prepare\<arch>. Never shipped.
#   --test-base URL --test-key PEM (with --test-app; release.md T12): OUT/app-test downloads from
#               URL (https://, the TEST server: compiled in as the exe's RbspTestUpdateBase
#               metadata, handed to the core by TestHooks.SetUpdateBase) over the core's TEST DLLs
#               (core/build.sh dll --test-hooks --test-key PEM: OUT/core-test), which trust that
#               test key alone. scripts/build-windows.sh refuses a release key; this script
#               doesn't check it.
#   --x64: also OUT/app-x64, the x64-forced exe (PlatformTarget x64: under Prism on ARM64, loading
#               x64\rbsp-core.dll) gated as a release; with --test-app also OUT/app-x64-test.
#               For the Prism pass (app.md §7). Never shipped.
#   --prepare: also OUT/app-prepare, the Prepare release's exe (PREPARE_TABS: the Status and Prepare
#               tabs shown), gated as a release that carries the tabs' code.
#   --app-version V: the exe's and the core DLLs' version V (x.y.z) instead of VERSION's
#               (scripts/build-windows.sh --stamp: a TEST zip at the next version, or a release
#               build stamped with the previous version for release.sh's carry decision).
# Every exe is gated: a release one names no TEST hook or probe marker and no TEST download base,
# a TEST one names every marker, and its PE machine is anycpu (i386 with the CLR's ILONLY flag and
# no 32-bit flag) or amd64 (x64-forced). The first release's exes (app, app-x64) hold no code that
# creates the Status and Prepare tabs (I10, I86: none of its names); the Prepare release's and
# every TEST exe hold all of it.
#
# The SDK is downloaded once into build/dl/, checked against its pinned SHA-256 (release.md §9.2)
# and extracted into build/dotnet/ with its licence files checked; NuGet's cache is build/nuget/.
# Not yet under deps.lock: this script's own curl of the SDK and the reference assemblies' licence
# check are deferred to release.md T1-T6, as deps.lock's header says for the .NET and NuGet rows.
# scripts/build-app.sh never builds win/ (the Mac app's inputs don't change: make mac-unchanged).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

DOTNET_VERSION=10.0.403
DOTNET_SHA256=826d7ff91fa83b16bccd7edd7822581fe32b5228a4501aabe61d864168129d73
DOTNET_TAR="dotnet-sdk-$DOTNET_VERSION-osx-arm64.tar.gz"
DOTNET_URL="https://builds.dotnet.microsoft.com/dotnet/Sdk/$DOTNET_VERSION/$DOTNET_TAR"
DOTNET_LICENSE_SHA256=cfc21f5e8bd655ae997eec916138b707b1d290b83272c02a95c9f821b8c87310
DOTNET_TPN_SHA256=2dc8f8c5a39401e928b5784ab564eb8b3ceb99ead3df8f260e0cab7e0bbecc7a

relock= test=1 testapp= testbase= testkey= appversion= x64= prepare=
usage() { echo "usage: $0 [--relock] [--no-test] [--test-app [--test-base URL --test-key PEM]] [--x64] [--prepare] [--app-version V] [OUT]" >&2; exit 2; }
while [ $# -gt 0 ]; do
  case "$1" in
    --relock) relock=1; shift ;;
    --no-test) test=; shift ;;
    --test-app) testapp=1; shift ;;
    --test-base) [ $# -ge 2 ] || usage; testbase="$2"; shift 2 ;;
    --test-key) [ $# -ge 2 ] || usage; testkey="$2"; shift 2 ;;
    --app-version) [ $# -ge 2 ] || usage; appversion="$2"; shift 2 ;;
    --x64) x64=1; shift ;;
    --prepare) prepare=1; shift ;;
    -*) usage ;;
    *) break ;;
  esac
done
# the TEST server's base and its key go together, and only into the TEST app
if [ -n "$testbase$testkey" ]; then
  [ -n "$testapp" ] && [ -n "$testbase" ] && [ -n "$testkey" ] || usage
  case "$testbase" in https://*) ;; *) echo "--test-base must be https:// (the core downloads over HTTPS only)" >&2; exit 2 ;; esac
  [ -f "$testkey" ] || { echo "no test key $testkey" >&2; exit 2; }
  testkey="$(cd "$(dirname "$testkey")" && pwd)/$(basename "$testkey")"
fi
[ -z "$appversion" ] || expr "$appversion" : '[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$' > /dev/null || { echo "--app-version must be x.y.z" >&2; exit 2; }
mkdir -p "${1:-$root/build/shell}"
out="$(cd "${1:-$root/build/shell}" && pwd)"

sha() { shasum -a 256 "$1" | cut -d' ' -f1; }

fetch_dotnet() {
  sdk="$root/build/dotnet"
  stamp="$sdk/.rbsp-pinned"
  if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$DOTNET_SHA256" ]; then return 0; fi
  dl="$root/build/dl"
  mkdir -p "$dl"
  tarball="$dl/$DOTNET_TAR"
  if [ ! -f "$tarball" ] || [ "$(sha "$tarball")" != "$DOTNET_SHA256" ]; then
    echo "downloading $DOTNET_URL" >&2
    curl -fsSL -o "$tarball.part" "$DOTNET_URL"
    got="$(sha "$tarball.part")"
    if [ "$got" != "$DOTNET_SHA256" ]; then
      rm -f "$tarball.part"
      echo ".NET SDK tarball: sha256 $got, expected $DOTNET_SHA256" >&2
      exit 1
    fi
    mv -f "$tarball.part" "$tarball"
  fi
  rm -rf "$sdk"
  mkdir -p "$sdk"
  tar -xzf "$tarball" -C "$sdk"
  # the licence travels with the hash (release.md §9.1 rule 2)
  if [ "$(sha "$sdk/LICENSE.txt")" != "$DOTNET_LICENSE_SHA256" ] || [ "$(sha "$sdk/ThirdPartyNotices.txt")" != "$DOTNET_TPN_SHA256" ]; then
    echo "licence changed: re-read it (release.md §9)" >&2
    exit 1
  fi
  printf '%s\n' "$DOTNET_SHA256" > "$stamp"
}

version="$(tr -d '[:space:]' < "$root/VERSION")"
[ -z "$appversion" ] || version="$appversion"

# 1. the generated ids and the core DLLs (the release build: no test hooks)
"$root/core/build.sh" ids "$out/ids" > /dev/null
RBSP_WIN_VERSION="$version" "$root/core/build.sh" dll "$out/core"

# 2. the WPF app
fetch_dotnet
export DOTNET_ROOT="$root/build/dotnet"
export DOTNET_CLI_HOME="$root/build/dotnet-home"
export NUGET_PACKAGES="$root/build/nuget"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=1 DOTNET_GENERATE_ASPNET_CERTIFICATE=0
export MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_UI_LANGUAGE=en
dotnet="$DOTNET_ROOT/dotnet"
lockp="-p:ShellUnlock=0"
if [ -n "$relock" ]; then
  rm -f "$here/packages.lock.json"
  lockp="-p:ShellUnlock=1"
elif [ ! -f "$here/packages.lock.json" ]; then
  echo "win/shell/packages.lock.json is missing: run with --relock once and review it" >&2
  exit 1
fi
# one exe: test 0|1 (SHELL_TEST), x64 0|1 (PlatformTarget x64), prepare 0|1 (PREPARE_TABS, default
# 0), into $here/bin; a TEST exe gets --test-base's URL (ShellTestUpdateBase), a release one an
# empty one. The properties on the command line, which beats the environment
build_exe() {
  ub=
  [ "$1" != 1 ] || ub="$testbase"
  rm -rf "$here/bin" "$here/obj"
  # from the project's folder, so global.json picks the pinned SDK
  (cd "$here" && "$dotnet" build RbStemsPlus.csproj -c Release -nologo -v:minimal --disable-build-servers \
     "-p:ShellIds=$out/ids" "-p:Version=$version" "$lockp" "-p:ShellTest=$1" "-p:ShellX64=$2" "-p:ShellTestUpdateBase=$ub" \
     "-p:ShellPrepare=${3:-0}" -p:UseSharedCompilation=false)
}
# the release exe never carries the TEST hooks nor the probe: their markers, in UTF-16 string
# literals (tr drops the NULs) or in the metadata's UTF-8 names; grep -a: the exe is binary, and
# without it grep finds nothing there. Every TEST exe must carry all of them, so the gate can see them.
test_markers='RBSP_SHELL_TEST|rbstemsplus-probe|ShellProbe|ProbeRecorder|core_test_win_fake_helper'
n_markers=5
has_markers() { LC_ALL=C tr -d '\000' < "$1" | LC_ALL=C grep -a -o -E "$test_markers" | sort -u | wc -l | tr -d ' '; }
# the PE header's machine: amd64 for x64, arm64; a PE32 i386 exe is anycpu only when its CLR
# header says so (Flags: ILONLY set, neither 32BITREQUIRED nor 32BITPREFERRED: the CLR picks the
# process's architecture), else i386-32bit. The whole file is read: the CLR header (data directory
# 14) lies in .text, its RVA mapped to a file offset through the section table.
pe_machine() {
  python3 -I -c 'import struct, sys
b = open(sys.argv[1], "rb").read()
o = struct.unpack_from("<I", b, 0x3c)[0]
m, nsec = struct.unpack_from("<HH", b, o + 4)
opt = o + 24
size_opt = struct.unpack_from("<H", b, o + 20)[0]
name = {0x14c: "i386", 0x8664: "amd64", 0xaa64: "arm64"}.get(m, hex(m))
if m == 0x14c and struct.unpack_from("<H", b, opt)[0] == 0x10b:
    rva, size = struct.unpack_from("<II", b, opt + 96 + 14 * 8)
    flags = None
    for i in range(nsec):
        s = opt + size_opt + 40 * i
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", b, s + 8)
        if size and va <= rva < va + max(vsize, rawsize):
            flags = struct.unpack_from("<I", b, raw + rva - va + 16)[0]
            break
    name = "anycpu" if flags is not None and flags & 1 and not flags & 0x20002 else "i386-32bit"
print(name)' "$1"
}
# the code that creates the Status and Prepare tabs (PrepareView.cs, StatusView.cs and
# MainWindow.AddPrepareTabs: compiled only with PREPARE_TABS or SHELL_TEST): their metadata's UTF-8
# names, all three or none
tabs_marker='AddPrepareTabs|PrepareView|StatusView'
n_tabs=3
tabs_count() { LC_ALL=C grep -a -o -E "$tabs_marker" "$1" | sort -u | wc -l | tr -d ' '; }
# gate EXE KIND MACHINE [TABS]: KIND release (no marker) or test (every marker); TABS yes (the
# tabs' code there: the Prepare release, every TEST exe) or no (the first release: default for a
# release)
gate() {
  n="$(has_markers "$1")"
  if [ "$2" = release ] && [ "$n" != 0 ]; then echo "$1: a release exe carries the TEST hooks or the probe" >&2; exit 1; fi
  if [ "$2" = test ] && [ "$n" != "$n_markers" ]; then echo "$1: a TEST exe lacks a marker the release gate looks for" >&2; exit 1; fi
  tabs="${4:-}"; [ -n "$tabs" ] || { [ "$2" = test ] && tabs=yes || tabs=no; }
  t="$(tabs_count "$1")"
  if [ "$tabs" = no ] && [ "$t" != 0 ]; then echo "$1: a first-release exe can show the Status and Prepare tabs" >&2; exit 1; fi
  if [ "$tabs" = yes ] && [ "$t" != "$n_tabs" ]; then echo "$1: this exe lacks the Status and Prepare tabs' code" >&2; exit 1; fi
  m="$(pe_machine "$1")"
  [ "$m" = "$3" ] || { echo "$1: its PE machine is $m, not $3" >&2; exit 1; }
}
# nor a download base of its own (the TEST server's metadata, the core's test hook): GitHub's only.
# Not one of test_markers: a TEST exe built without --test-base names neither
nobase() {
  if LC_ALL=C tr -d '\000' < "$1" | LC_ALL=C grep -a -c -E 'RbspTestUpdateBase|core_test_update_base' > /dev/null; then
    echo "$1: a release exe carries a TEST download base" >&2; exit 1
  fi
}

build_exe 0 0
rm -rf "$out/app"
mkdir -p "$out/app/x64" "$out/app/arm64"
cp "$here/bin/RB Stems Plus.exe" "$here/bin/RB Stems Plus.exe.config" "$out/app/"
gate "$out/app/RB Stems Plus.exe" release anycpu
nobase "$out/app/RB Stems Plus.exe"
for a in x64 arm64; do cp "$out/core/$a/rbsp-core.dll" "$out/app/$a/rbsp-core.dll"; done
echo "built $out/app ($version, .NET SDK $("$dotnet" --version)):"
ls -la "$out/app" "$out/app/x64" "$out/app/arm64"
if [ -n "$testapp" ]; then
  build_exe 1 0
  rm -rf "$out/app-test"
  cp -R "$out/app" "$out/app-test"
  cp "$here/bin/RB Stems Plus.exe" "$out/app-test/"
  gate "$out/app-test/RB Stems Plus.exe" test anycpu
  # the TEST app runs over the test-hooks core DLLs (the harness's fake helper, RBSP_TEST_PROGRAMS'
  # fake trees), with the Windows fake helper beside it; the release app's DLLs never take a fake
  # helper (core/build.sh's release gate: no core_test_ export), checked here once more. With
  # --test-base, the core's TEST DLLs that trust the test key alone (OUT/core-test), and the TEST
  # server's base in the exe
  if [ -n "$testbase" ]; then
    LC_ALL=C grep -a -q -F "$testbase" "$out/app-test/RB Stems Plus.exe" || { echo "the TEST exe doesn't name $testbase" >&2; exit 1; }
    RBSP_WIN_VERSION="$version" "$root/core/build.sh" dll "$out/core-test" --test-hooks --test-key "$testkey"
    hooks="$out/core-test/testhooks"
  else
    RBSP_WIN_VERSION="$version" "$root/core/build.sh" dll "$out/core" --test-hooks > /dev/null
    hooks="$out/core/testhooks"
  fi
  for a in x64 arm64; do
    cp "$hooks/$a/rbsp-core.dll" "$out/app-test/$a/rbsp-core.dll"
    if ! LC_ALL=C grep -a -q core_test_win_fake_helper "$out/app-test/$a/rbsp-core.dll"; then echo "the TEST app's $a core DLL takes no fake helper" >&2; exit 1; fi
    if LC_ALL=C grep -a -q core_test_win_fake_helper "$out/app/$a/rbsp-core.dll"; then echo "the release app's $a core DLL takes a fake helper" >&2; exit 1; fi
    mkdir -p "$out/app-test/fake-prepare/$a"
    "$root/scripts/win-cc.sh" "$a" -Wall -Wextra -Werror -municode "$root/tests/harness/fake-prepare-win.c" -o "$out/app-test/fake-prepare/$a/fake-prepare.exe"
  done
  echo "built $out/app-test (TEST: SHELL_TEST, test-hooks core DLLs${testbase:+ trusting the test key alone, downloads from $testbase}, fake-prepare\\<arch>)"
fi
# the x64-forced exes for the Prism pass on the ARM64 VM (app.md §7): the release one gated as a
# release, the TEST one (with --test-app) as the probe and FlaUI suites run it
if [ -n "$x64" ]; then
  build_exe 0 1
  rm -rf "$out/app-x64"
  cp -R "$out/app" "$out/app-x64"
  cp "$here/bin/RB Stems Plus.exe" "$out/app-x64/"
  gate "$out/app-x64/RB Stems Plus.exe" release amd64
  nobase "$out/app-x64/RB Stems Plus.exe"
  echo "built $out/app-x64 (x64-forced)"
  if [ -n "$testapp" ]; then
    build_exe 1 1
    rm -rf "$out/app-x64-test"
    cp -R "$out/app-test" "$out/app-x64-test"
    cp "$here/bin/RB Stems Plus.exe" "$out/app-x64-test/"
    gate "$out/app-x64-test/RB Stems Plus.exe" test amd64
    echo "built $out/app-x64-test (x64-forced, TEST: SHELL_TEST)"
  fi
fi
# the Prepare release's exe (the Status and Prepare tabs shown), gated as a release with their code
if [ -n "$prepare" ]; then
  build_exe 0 0 1
  rm -rf "$out/app-prepare"
  cp -R "$out/app" "$out/app-prepare"
  cp "$here/bin/RB Stems Plus.exe" "$out/app-prepare/"
  gate "$out/app-prepare/RB Stems Plus.exe" release anycpu yes
  echo "built $out/app-prepare (the Prepare release: the Status and Prepare tabs)"
fi
rm -rf "$here/bin" "$here/obj"

# 3. the Mac tests
[ -n "$test" ] || exit 0
mkdir -p "$out/test"
# the core as a dylib for this Mac, as the core's host test build compiles it (CORE_WIN_POLICY,
# the test hooks; no "native" Windows code: the tests' host runs post_work itself)
# shellcheck disable=SC2046
(cd "$root/core" && clang -std=c11 -O1 -g -DCORE_TEST_HOOKS -DCORE_WIN_POLICY -I. $("$root/core/build.sh" srcs) \
   -dynamiclib -lpthread -o "$out/test/librbsp-core.dylib")
(cd "$here/tests" && "$dotnet" build ShellTests.csproj -c Release -nologo -v:minimal --disable-build-servers \
   "-p:ShellIds=$out/ids" "-p:OutputPath=$out/test/bin/" "-p:BaseIntermediateOutputPath=$out/test/obj/" \
   -p:UseSharedCompilation=false)
"$dotnet" "$out/test/bin/ShellTests.dll" "$out/test/librbsp-core.dylib"
