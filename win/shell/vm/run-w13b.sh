#!/bin/bash
# W13b's VM run (vm/w13b.ps1) in the Parallels VM rb-win-template (one agent at a time on the VM):
# copies OUT/app and OUT/app-test (win/shell/build.sh --test-app) into the share (the main
# checkout's build/win/shell, \\Mac\win\shell), installs it as SYSTEM, copies w13b.ps1 and its
# fixtures (w13b-fixture.ps1, prepare-fixture.ps1) to the share's job folder (\\Mac\win\job:
# winps.sh stages only job.ps1 there), then runs one winps.sh --user-signed job per part: the
# spike, then the PT cases. Each job stays below winps.sh's 10-minute wait (w13b.ps1's dog ends it
# at 9). Results (records, acks, screenshots) go to build/win/shell/results/w13b. Must run outside
# the Mac sandbox (prlctl). The VM must be up, with Pause idle off.
#   win/shell/vm/run-w13b.sh [OUT]     OUT defaults to build/shell
# Exit status: 0 when both parts passed, else the first failing part's (its failed checks, or
# winps.sh's 124 on a timeout).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/shell}"
SHARE="$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win"
winps="$root/tests/win/probes/winps.sh"
rm -rf "$SHARE/shell/app" "$SHARE/shell/app-test"   # results/ is the VM's (root-owned here): install.ps1 clears it
mkdir -p "$SHARE/shell" "$SHARE/job"
[ -d "$out/app-test" ] || { echo "no $out/app-test: build with win/shell/build.sh --test-app" >&2; exit 2; }
cp -R "$out/app" "$out/app-test" "$SHARE/shell/"
[ -f "$SHARE/vm/uia/rbui/rbui.exe" ] || { echo "stage rbui with vm-run.sh stage --build" >&2; exit 2; }
cp "$here/w13b.ps1" "$here/w13b-checks.ps1" "$here/w13b-fixture.ps1" "$here/prepare-fixture.ps1" \
  "$root/tests/harness/traces/A/prepare-choose-estimate.jsonl" "$SHARE/job/"
"$winps" < "$here/install.ps1"
rc=0
for part in spike pt; do
  echo "== w13b -Part $part"
  prc=0
  printf '& \\\\Mac\\win\\job\\w13b.ps1 -Part %s; exit $LASTEXITCODE\n' "$part" | "$winps" --user-signed || prc=$?
  echo "== w13b -Part $part: exit $prc"
  [ "$rc" -ne 0 ] || rc=$prc
done
exit "$rc"
