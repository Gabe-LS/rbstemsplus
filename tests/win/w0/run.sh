#!/bin/bash
# Runs the W0 spike in the Parallels VM rb-win-template (lane W only), without a window on the
# owner's screen: copies OUT/app and OUT/app-release (tests/win/w0/build.sh) into the share
# (the main checkout's build/win/w0, \\Mac\win\w0), installs them as SYSTEM, runs them as the user.
#   tests/win/w0/run.sh [OUT] ['$var = value' ...]   OUT defaults to build/w0; e.g. '$runs = 3'
# Must run outside the Mac sandbox (prlctl). The VM must be up, with Pause idle off.
#   tests/win/w0/run.sh --user-script FILE.ps1   run one script as the user (diagnostics)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
if [ "${1:-}" = "--user-script" ]; then "$root/tests/win/probes/winps.sh" "${W0_USER:---user-signed}" < "$2"; exit; fi
out="$root/build/w0"
if [ $# -gt 0 ] && [ "${1#\$}" = "$1" ]; then out="$(cd "$1" && pwd)"; shift; fi
SHARE="$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win"
winps="$root/tests/win/probes/winps.sh"
rm -rf "$SHARE/w0/app" "$SHARE/w0/app-release"   # results/ is the VM's (root-owned here): install.ps1 clears it
mkdir -p "$SHARE/w0"
cp -R "$out/app" "$out/app-release" "$SHARE/w0/"
"$winps" < "$here/vm/install.ps1"
{ for l in "$@"; do printf '%s\n' "$l"; done; cat "$here/vm/run.ps1"; } | "$winps" "${W0_USER:---user-signed}"
