#!/bin/bash
# The shell's VM smoke in the Parallels VM rb-win-template (one agent at a time on the VM): copies
# OUT/app and OUT/app-test (win/shell/build.sh --test-app) into the share (the main checkout's build/win/shell, \\Mac\win\shell),
# installs it as SYSTEM, then runs vm/smoke.ps1 as the user (UI Automation, screenshots into
# build/win/shell/results). Must run outside the Mac sandbox (prlctl). The VM must be up, with
# Pause idle off, and C:\rbtest\hidden.exe's job folder as winps.sh needs it.
#   win/shell/vm/run.sh [OUT]     OUT defaults to build/shell
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/shell}"
SHARE="$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win"
winps="$root/tests/win/probes/winps.sh"
rm -rf "$SHARE/shell/app" "$SHARE/shell/app-test"   # results/ is the VM's (root-owned here): install.ps1 clears it
mkdir -p "$SHARE/shell"
[ -d "$out/app-test" ] || { echo "no $out/app-test: build with win/shell/build.sh --test-app" >&2; exit 2; }
cp -R "$out/app" "$out/app-test" "$SHARE/shell/"
"$winps" < "$here/install.ps1"
"$winps" --user-signed < "$here/smoke.ps1"
