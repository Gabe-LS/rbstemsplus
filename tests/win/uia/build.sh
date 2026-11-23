#!/bin/sh
# The VM suites' UI driver (the by-hand plan's W1), built on the Mac with FOSS tools only, test
# only, never shipped:
#   - OUT/rbui/: rbui.exe (rbui/Program.cs: FlaUI 5.0.0 over UIA3, net481 AnyCPU, native on ARM64)
#     with rbui.exe.config, FlaUI.Core.dll, FlaUI.UIA3.dll and Interop.UIAutomationClient.dll and
#     the packages' LICENSE files. The three nupkgs are deps.lock's flaui-core, flaui-uia3 and
#     interop-uiautomationclient rows: fetched through scripts/deps.sh (a copy already in build/flaui/,
#     this checkout's or the main one's, is used when its SHA-256 matches; nothing is downloaded
#     then), each SHA-256 and LICENSE.txt checked, their lib/net48 (net45 for the interop) DLLs
#     referenced by path. No NuGet restore of them (FlaUI.Core's System.Management dependency is the
#     framework's own on net481). The pinned .NET SDK (as win/shell/build.sh), restored in locked
#     mode for the net481 reference assemblies only (rbui/packages.lock.json);
#   - OUT/x64/sysdesk.exe, OUT/arm64/sysdesk.exe (sysdesk.c, the pinned llvm-mingw through
#     scripts/win-cc.sh): a program run as SYSTEM on the secure desktop, or as the user, in the
#     console session.
#
#   tests/win/uia/build.sh [--relock] [OUT]      OUT defaults to build/uia
#   --relock: restore unlocked once and rewrite rbui/packages.lock.json (review the diff)
#
# What each reaches (the spike checks the first two before anything depends on them):
#   - rbui consent/blockscreen through sysdesk (SYSTEM, winsta0\winlogon, console session): UAC's
#     consent prompt (consent.exe) and the block screen of a sign-out or restart (LogonUI), read
#     and pressed with UIA; a screenshot of that desktop;
#   - rbui's other verbs as the user (a user step): any window of the user's desktop that UIA
#     exposes (WPF, Win32, XAML), its menus' popups included. Not an elevated window (UIPI: no
#     uiAccess), not Explorer's taskbar context menus' timing (W2), not rekordbox's own
#     drawn controls (W5's spike);
#   - not reached: the sign-in screen's password box (autologon signs in instead: vmlib's
#     Set-AutoLogon), a Ctrl+Alt+Del screen (SAS), a UAC credential prompt for a standard user
#     (the suites' account is an administrator), a session other than the console's.
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
mkdir -p "${1:-$root/build/uia}"
out="$(cd "${1:-$root/build/uia}" && pwd)"
deps="$root/scripts/deps.sh"

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

# 1. the three nupkgs, by deps.lock's pins: a copy in build/flaui/ (this checkout's, or the main
# checkout's when this is a worktree) is placed where deps.sh keeps the download, and deps.sh
# fetch keeps it only when its SHA-256 is the pinned one (else it downloads, and checks that)
main="$(cd "$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)" || main="$root"
libs="$out/lib"
rm -rf "$libs" "$out/rbui"
mkdir -p "$libs" "$out/rbui"
for n in flaui-core flaui-uia3 interop-uiautomationclient; do
  url="$("$deps" get "$n" url)"
  f="$(basename "$url")"
  if [ ! -f "$root/build/dl/$n/$f" ]; then
    for s in "$root/build/flaui/$f" "$main/build/flaui/$f"; do
      if [ -f "$s" ]; then mkdir -p "$root/build/dl/$n"; cp "$s" "$root/build/dl/$n/$f"; break; fi
    done
  fi
  pkg="$("$deps" fetch "$n")"
  [ "$(sha "$pkg")" = "$("$deps" get "$n" sha256)" ] || { echo "$n: $pkg isn't the pinned one" >&2; exit 1; }
  x="$out/pkg/$n"
  rm -rf "$x"; mkdir -p "$x"
  (cd "$x" && unzip -q "$pkg" 'lib/*' LICENSE.txt)
  "$deps" licence "$n.lib" "$x"
  case "$n" in
    interop-uiautomationclient) cp "$x/lib/net45/Interop.UIAutomationClient.dll" "$libs/" ;;
    flaui-core) cp "$x/lib/net48/FlaUI.Core.dll" "$libs/" ;;
    flaui-uia3) cp "$x/lib/net48/FlaUI.UIA3.dll" "$libs/" ;;
  esac
  cp "$x/LICENSE.txt" "$out/rbui/LICENSE-$n.txt"
done

# 2. rbui.exe
fetch_dotnet
export DOTNET_ROOT="$root/build/dotnet"
export DOTNET_CLI_HOME="$root/build/dotnet-home"
export NUGET_PACKAGES="$root/build/nuget"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=1 DOTNET_GENERATE_ASPNET_CERTIFICATE=0
export MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_UI_LANGUAGE=en
dotnet="$DOTNET_ROOT/dotnet"
app="$here/rbui"
lockp="-p:RbuiUnlock=0"
if [ -n "$relock" ]; then
  rm -f "$app/packages.lock.json"
  lockp="-p:RbuiUnlock=1"
elif [ ! -f "$app/packages.lock.json" ]; then
  echo "tests/win/uia/rbui/packages.lock.json is missing: run with --relock once and review it" >&2
  exit 1
fi
rm -rf "$app/bin" "$app/obj"
# from the project's folder, so global.json picks the pinned SDK
(cd "$app" && "$dotnet" build rbui.csproj -c Release -nologo -v:minimal --disable-build-servers \
   "-p:RbuiLibs=$libs" "$lockp" -p:UseSharedCompilation=false)
for f in rbui.exe rbui.exe.config FlaUI.Core.dll FlaUI.UIA3.dll Interop.UIAutomationClient.dll; do
  [ -f "$app/bin/$f" ] || { echo "the build left no $f" >&2; exit 1; }
  cp "$app/bin/$f" "$out/rbui/"
done
rm -rf "$app/bin" "$app/obj"
# AnyCPU: a PE32 with the CLR's ILONLY flag and no 32-bit-required flag (native on x64 and ARM64)
"$root/scripts/win-cc.sh" --tool x64 llvm-readobj --file-headers "$out/rbui/rbui.exe" 2> /dev/null | grep -q 'Machine: IMAGE_FILE_MACHINE_I386' \
  || { echo "rbui.exe isn't an AnyCPU PE (i386 machine field)" >&2; exit 1; }

# 3. sysdesk.exe, both architectures
for a in x64 arm64; do
  mkdir -p "$out/$a"
  "$root/scripts/win-cc.sh" "$a" -Wall -Wextra -Werror -municode "$here/sysdesk.c" -lwtsapi32 -luserenv -ladvapi32 -o "$out/$a/sysdesk.exe"
done
echo "built $out:"
ls -la "$out/rbui" "$out/x64" "$out/arm64"
