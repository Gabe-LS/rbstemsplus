#!/bin/sh
# The W0 spike (app.md §3 S0, §9 W0; mac-core §9 G1.5, G1.9, G1.10; release.md T6), built on the
# Mac with FOSS tools only:
#   - the core as x64\rbsp-core.dll and arm64\rbsp-core.dll (core/build.sh dll --test-hooks,
#     the pinned llvm-mingw through scripts/win-cc.sh);
#   - shimtest-x64.exe and shimtest-arm64.exe (core/plat_shim.h's Windows branch on its own);
#   - the net481 WPF test app with XAML, by the pinned .NET SDK tarball (global.json names it),
#     restored in locked mode (packages.lock.json): "RB Stems Plus W0.exe" (AnyCPU, Prefer32Bit
#     off: native arm64 on ARM64 Windows) and "RB Stems Plus W0 x64.exe" (the x64-forced build:
#     x64 under emulation on ARM64). No NuGet package ships: the reference assemblies are build-only.
#   - OUT/app-release: the same exes over the release DLLs (core/build.sh dll, no test hooks), for
#     the start-up sequence alone (the exe's --startup).
#
#   tests/win/w0/build.sh [--relock] [OUT]   OUT defaults to build/w0
#   --relock: restore unlocked once and rewrite app/packages.lock.json (review the diff)
#
# The SDK is downloaded once into build/dl/, checked against its pinned SHA-256 (release.md §9.2)
# and extracted into build/dotnet/ with its licence files checked; NuGet's cache is build/nuget/.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"

DOTNET_VERSION=10.0.403
DOTNET_SHA256=826d7ff91fa83b16bccd7edd7822581fe32b5228a4501aabe61d864168129d73
DOTNET_TAR="dotnet-sdk-$DOTNET_VERSION-osx-arm64.tar.gz"
DOTNET_URL="https://builds.dotnet.microsoft.com/dotnet/Sdk/$DOTNET_VERSION/$DOTNET_TAR"
DOTNET_LICENSE_SHA256=cfc21f5e8bd655ae997eec916138b707b1d290b83272c02a95c9f821b8c87310
DOTNET_TPN_SHA256=2dc8f8c5a39401e928b5784ab564eb8b3ceb99ead3df8f260e0cab7e0bbecc7a

relock=
if [ "${1:-}" = "--relock" ]; then relock=1; shift; fi
mkdir -p "${1:-$root/build/w0}"
out="$(cd "${1:-$root/build/w0}" && pwd)"

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

# 1. the core DLLs, with the test hooks (a TEST build: never shipped), and the generated ids
"$root/core/build.sh" dll "$out/core" --test-hooks
"$root/core/build.sh" ids "$out/ids" > /dev/null
for a in x64 arm64; do
  mkdir -p "$out/app/$a"
  cp "$out/core/testhooks/$a/rbsp-core.dll" "$out/app/$a/rbsp-core.dll"
done

# 2. the plat_shim test
for a in x64 arm64; do
  (cd "$here" && "$root/scripts/win-cc.sh" "$a" -Wall -Wextra -Werror shimtest.c -o "$out/app/shimtest-$a.exe")
done

# 3. the WPF app
fetch_dotnet
export DOTNET_ROOT="$root/build/dotnet"
export DOTNET_CLI_HOME="$root/build/dotnet-home"
export NUGET_PACKAGES="$root/build/nuget"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=1 DOTNET_GENERATE_ASPNET_CERTIFICATE=0
export MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_UI_LANGUAGE=en
dotnet="$DOTNET_ROOT/dotnet"
app="$here/app"
lockp="-p:W0Unlock=0"
if [ -n "$relock" ]; then
  rm -f "$app/packages.lock.json"
  lockp="-p:W0Unlock=1"
elif [ ! -f "$app/packages.lock.json" ]; then
  echo "tests/win/w0/app/packages.lock.json is missing: run with --relock once and review it" >&2
  exit 1
fi
rm -rf "$app/bin" "$app/obj"
for arch in AnyCPU x64; do
  # from the project's folder, so global.json picks the pinned SDK
  (cd "$app" && "$dotnet" build W0Spike.csproj -c Release -nologo -v:minimal --disable-build-servers \
     "-p:W0Ids=$out/ids" "-p:W0Arch=$arch" "$lockp" -p:UseSharedCompilation=false)
  lockp="-p:W0Unlock=0"
  cp "$app/bin/$arch/"*.exe "$app/bin/$arch/"*.exe.config "$out/app/"
done
"$dotnet" --version > "$out/app/sdk-version.txt"

# 4. the same exes over the release DLLs (no test hooks): the start-up sequence only (--startup)
"$root/core/build.sh" dll "$out/core-release"
rm -rf "$out/app-release"
for a in x64 arm64; do
  mkdir -p "$out/app-release/$a"
  cp "$out/core-release/$a/rbsp-core.dll" "$out/app-release/$a/rbsp-core.dll"
done
cp "$out/app/"*.exe.config "$out/app-release/"
for f in "$out/app/RB Stems Plus W0"*.exe; do cp "$f" "$out/app-release/"; done
echo "built $out/app:"
ls -la "$out/app"
