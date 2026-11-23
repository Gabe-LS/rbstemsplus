#!/bin/bash
# The rekordbox test kit's Windows PowerShell 5.1 gate on the Parallels VM rb-win-template: copies
# this checkout's kit (kit/*.ps1, kitps-run.ps1) and ps51-check.ps1 to the share's ps51/ (\\Mac\win\ps51)
# with the check's self-test (ps51-selftest/: known-bad scripts and the findings they must give),
# and runs tests/win/suites/ps51.suite with tests/win/vm-run.sh (revert to SNAPSHOT, run, revert and
# stop). Never starts rekordbox. Run it outside the Mac sandbox (prlctl). Prints the check's findings.
#   tests/rekordbox/win/ps51-gate.sh [SNAPSHOT]     default "clean Windows (shut down)"
# Exit: vm-run.sh's (0 the check passed, 1 it found errors, 2 the VM run couldn't start).
set -uo pipefail
here="$(cd "$(dirname "$0")/../../.." && pwd)"
share="${VMRUN_SHARE:-$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win}"
dst="$share/ps51"
rm -rf "$dst" && mkdir -p "$dst/kit" || { echo "ps51-gate.sh: can't stage $dst" >&2; exit 2; }
cp "$here"/tests/rekordbox/win/kit/*.ps1 "$dst/kit/" &&
  cp "$here/tests/rekordbox/win/kitps-run.ps1" "$here/tests/rekordbox/win/ps51-check.ps1" "$dst/" &&
  cp -R "$here/tests/rekordbox/win/ps51-selftest" "$dst/selftest" ||
  { echo "ps51-gate.sh: can't copy the kit" >&2; exit 2; }
dirty=""; git -C "$here" diff --quiet HEAD -- tests/rekordbox/win || dirty=" (with uncommitted changes)"
echo "$(git -C "$here" rev-parse --short=12 HEAD)$dirty" > "$dst/.commit"
log="$(mktemp "${TMPDIR:-/tmp}/ps51-gate.XXXXXX")"
"$here/tests/win/vm-run.sh" run "${1:-clean Windows (shut down)}" ps51 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
results="$(sed -n 's/^\[[0-9:]*\] results: //p' "$log" | sed 's/ ([0-9]* skipped)$//' | tail -1)"
rm -f "$log"
if [ -n "$results" ] && [ -f "$results/1-ps51.log" ]; then
  echo "--- ps51-check on the VM ($results/1-ps51.log):"
  tr -d '\r' < "$results/1-ps51.log" | grep -E '^(staged kit:|selftest: |ps51-check:|PARSE |ADDTYPE |STATIC )'
fi
exit "$rc"
