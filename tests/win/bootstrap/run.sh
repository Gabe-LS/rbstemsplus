#!/bin/sh
# scripts/bootstrap.ps1's tests on the Mac (app.md W12): tests/win/bootstrap/bootstrap.tests.ps1
# in pwsh (PowerShell 7, the FOSS one; release.md 9). Bounded: pwsh is given 10 minutes at most.
#
#   tests/win/bootstrap/run.sh       exits 1 on a failure, 2 when pwsh isn't installed
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
command -v pwsh > /dev/null 2>&1 || { echo "FAIL pwsh isn't installed (brew install powershell)"; exit 2; }
# a watchdog: nothing here may run past 10 minutes
pwsh -NoProfile -NonInteractive -File "$here/bootstrap.tests.ps1" &
pid=$!
# (its output detached, so a pipe on this script ends with the tests; its sleep killed with it)
( sleep 600; kill "$pid" 2> /dev/null ) > /dev/null 2>&1 &
dog=$!
if wait "$pid"; then rc=0; else rc=1; fi
pkill -P "$dog" 2> /dev/null || true
kill "$dog" 2> /dev/null || true
[ "$rc" = 0 ] && echo "ok   bootstrap.ps1 (pwsh)" || echo "FAIL bootstrap.ps1 (pwsh)"
exit "$rc"
