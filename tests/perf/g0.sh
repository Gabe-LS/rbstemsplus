#!/bin/bash
# The Mac app's performance session in tart (mac-core.md §2.2, the private Windows plan §8.2): EXCLUSIVE
# (nothing else runs on the host), on AC power, outside the sandbox (tart, ssh), from a checkout
# holding mac-A in gates-archive/ (tests/perf/mac-a.sh, built once). Nothing here asks the owner
# anything.
#
#   tests/perf/g0.sh run [B.zip [--scenarios "S…"]]   P15 on the host (information), the VM up,
#                       tests/perf/abab.sh (A alone, or A against B: a gate, about 40 min of
#                       launches; --scenarios: a carried release's smoke set), the VM deleted whatever
#                       happens; A alone: mac-A's reference exported (baselines, below)
#   tests/perf/g0.sh run B.zip --quick [--idle-wait SECS] [--scenarios "S…"]
#                       a REHEARSAL of the gate, not a measurement: the same preflight, VM, servers
#                       and scenarios, one scored launch per scenario and side (abab.sh -n 1), no
#                       P15, its label rehearsal-<date> (rh= in g0-labels.txt), its verdict printed
#                       as information; a rehearsal's exit is 0 when every launch ran (a regression
#                       verdict, and compare's fewer-launches failure, included), else its own
#                       failure (a launch failed or disturbed); missing samples are named. --idle-wait sets the idle guard
#                       (G0_IDLE_WAIT; 0, the guard off, only with --quick)
#   tests/perf/g0.sh baselines [LABEL]   mac-A's reference from a session of A (default: the last
#                       aa= label in build/perf/g0-labels.txt): tests/perf/stats.py baselines into
#                       build/perf/g0-A-baselines.tsv; G0_RESULTS and G0_PERF_DIR, both or neither,
#                       read a copy (its results folder, its build/perf); G0_RULES another rules file
#   tests/perf/g0.sh plan          this text and the plan, nothing run
# Side B for a gate (G2, G3) is the branch's TEST build: RBSTEMSPLUS_MODEL=<the model export>
# scripts/build.sh --test http://192.168.64.1:8765/ at its commit, then g0.sh run dist-test/rbstemsplus-app.zip
# (abab.sh serves B from that dist-test/ folder, its key from build/test-keys/). Since M7's
# switch-over B is the AppKit shell over the core (the probe's mark shell.core tells its launches).
# The launches wait until the Mac has had no input for G0_IDLE_WAIT seconds (600; abab.sh's idle
# guard): a launch during which input came fails the measurement. The displays sleep on their own
# timer. Every step's result goes into build/perf/g0-<date>.log. Exit 0 pass, 1 a regression,
# 3 the measurement failed, 2 a stop.
#
# mac-A's reference (owner decided, 2026-11-12): G0's session of 2026-11-11
# (g0-aa-20261111-1948), as it is: no re-run, no noise decision, its noisy rows noted. Its export
# is build/perf/g0-A-baselines.tsv (a copy in tests/perf/mac-A-g0-20261111.tsv). It is a record:
# every gate runs mac-A again, interleaved with B, in its own session.
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
A="${GATES_ARCHIVE:-$root/gates-archive}"
# G0_TEST=1 (the selftest only): G0_VM, G0_ABAB and G0_P15 replace vm.sh, abab.sh and p15.sh, and
# the host's preflight is skipped; a real run refuses them
vm="${G0_VM:-$root/tests/harness/vm.sh}"; abab="${G0_ABAB:-tests/perf/abab.sh}"; p15="${G0_P15:-tests/perf/p15.sh}"
TART="${TART:-$HOME/Applications/tart.app/Contents/MacOS/tart}"
day="$(date +%Y%m%d)"; stamp="$(date +%Y%m%d-%H%M)"
# (the selftest's run, G0_TEST=1, keeps its log and labels in a G0_PERF_DIR of its own)
if [ "${G0_RESULTS:+1}" != "${G0_PERF_DIR:+1}" ] && ! { [ "${1:-}" = run ] && [ "${G0_TEST:-}" = 1 ] && [ -z "${G0_RESULTS:-}" ]; }; then
  echo "g0.sh: G0_RESULTS and G0_PERF_DIR go together (a copy's results folder and its build/perf), or neither" >&2; exit 2
fi
perf="${G0_PERF_DIR:-$root/build/perf}"
r="${G0_RESULTS:-$root/build/harness/results}"
# the selftest's run (G0_TEST=1) keeps its sessions' folders in a folder of its own
if [ "${G0_TEST:-}" = 1 ] && [ -n "${G0_TEST_RESULTS:-}" ]; then r="$G0_TEST_RESULTS"; fi
log="$perf/g0-$day.log"
labels="$perf/g0-labels.txt"
zip="$A/mac-A/rbstemsplus-app.zip"
rules=(); [ -n "${G0_RULES:-}" ] && rules=(--rules "$G0_RULES")
say() { echo "[$(date '+%F %T')] $*" | tee -a "$log"; }
die() { say "STOP: $*"; exit 2; }
worst=0
step() {   # step NAME COMMAND…: runs it, its output into the log, its result printed
  local name="$1" rc; shift
  say "$name: $*"
  "$@" >> "$log" 2>&1; rc=$?
  case $rc in 0) say "$name: ok" ;; 1) say "$name: REGRESSION (1)" ;; 3) say "$name: MEASUREMENT FAILED (3)" ;; *) say "$name: FAILED ($rc)" ;; esac
  [ "$rc" -gt "$worst" ] && worst=$rc
  return $rc
}
label() { echo "$1=$2" >> "$labels"; echo "$2"; }
last() { sed -n "s/^$1=//p" "$labels" 2>/dev/null | tail -1; }
baselines() {   # baselines LABEL: mac-A's reference from that session
  step "mac-A's reference" python3 -I tests/perf/stats.py baselines "$r/$1" --a A --zip "$zip" --p15 "$perf/p15.txt" \
    --tsv "$perf/g0-A-baselines.tsv" ${rules[@]+"${rules[@]}"}
}

preflight() {
  if [ "${G0_TEST:-}" = 1 ]; then
    [ -n "${G0_VM:-}" ] && [ -n "${G0_ABAB:-}" ] && [ -n "${G0_P15:-}" ] || die "G0_TEST needs G0_VM, G0_ABAB and G0_P15 (stubs)"
    say "preflight skipped (G0_TEST: the selftest's stubs)"; return 0
  fi
  [ -z "${G0_VM:-}${G0_ABAB:-}${G0_P15:-}" ] || die "G0_VM, G0_ABAB and G0_P15 are the selftest's (G0_TEST)"
  [ -z "${G0_RESULTS:-}${G0_RULES:-}" ] || die "G0_RESULTS, G0_PERF_DIR and G0_RULES are for g0.sh baselines"
  [ -f "$zip" ] || die "no mac-A in $A (tests/perf/mac-a.sh first)"
  (cd "$A" && shasum -a 256 -c --quiet SHA256SUMS) || die "the gate archive doesn't match its SHA256SUMS: stop (never rebuild a baseline)"
  pmset -g batt | grep -q "AC Power" || die "not on AC power"
  if "$TART" list 2>/dev/null | awk 'NR > 1 && $NF == "running"' | grep -q .; then die "a tart VM is running already (tart list): one VM at a time"; fi
  if curl -s -o /dev/null --max-time 2 http://192.168.64.1:8765/; then die "something serves 192.168.64.1:8765 already"; fi
  # mac-A's own files and key serve its local servers (perf-under-sheet, perf-update), with the
  # model (the same export for every release: its SHA-256 checked)
  local model="${RBSTEMSPLUS_MODEL:-$root/dist-test/stemsplus-model.onnx}" src="$root/build/harness/macA-src"
  [ "$(shasum -a 256 "$model" 2>/dev/null | cut -d' ' -f1)" = 0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772 ] \
    || die "no model export at $model (RBSTEMSPLUS_MODEL, as for mac-a.sh)"
  mkdir -p "$src" && cp "$A"/mac-A/{rbstemsplus-app.zip,libonnxruntime.1.18.0.dylib,payload.json,payload.json.sig} "$src/" \
    && ln -sf "$model" "$src/stemsplus-model.onnx" || die "mac-A's server folder couldn't be made in $src"
  mkdir -p "$src/test-keys" && cp "$A/mac-A/test-keys/test-key.pem" "$src/test-keys/" || die "mac-A's test key couldn't be copied"
  export SERVER_SRC_A="$src" SERVER_KEY_A="$A/mac-A/test-keys/test-key.pem"
  export ABAB_IDLE_WAIT="${G0_IDLE_WAIT:-600}"
  # the host stays awake for the whole session; the displays sleep on their own timer (no -d)
  caffeinate -ims -w $$ &
}

# the perf scenarios' own forms (P10 60 s, P12 300 s, their own rounds): nothing overrides them
unset PERF_IDLE_S PERF_P12_S PERF_ROUNDS
case "${1:-}" in
  plan) sed -n '2,/^set -uo/p' "$0" | sed '$d'; echo "the plan (launches per side):"; python3 -I tests/perf/rules.py plan; exit $? ;;
  run)
    zb=""; sc=(); bad=0; quick=0; idle=""
    shift
    if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then zb="$1"; shift; fi
    while [ $# -gt 0 ]; do
      case "$1" in
        --scenarios) [ $# -ge 2 ] && [ -n "$2" ] || bad=1; sc=(--scenarios "${2:-}"); shift 2 || shift ;;
        --quick) quick=1; shift ;;
        --idle-wait) idle="${2:-}"; [[ "$idle" =~ ^[0-9]+$ ]] || bad=1; shift 2 || shift ;;
        *) bad=1; shift ;;
      esac
    done
    [ -n "$zb" ] || [ ${#sc[@]} = 0 ] || bad=1                  # --scenarios only with a B
    [ -n "$zb" ] || [ "$quick" = 0 ] || bad=1                   # a rehearsal is of a gate: A against B
    [ "$idle" != 0 ] || [ "$quick" = 1 ] || bad=1               # the guard off: rehearsals only
    [ "$bad" = 0 ] || { echo "g0.sh run [B.zip [--scenarios \"S…\"] [--quick [--idle-wait SECS]]]: --scenarios and --quick only with a B, --idle-wait 0 only with --quick" >&2; exit 2; }
    [ -z "$idle" ] || G0_IDLE_WAIT="$idle"
    export ABAB_IDLE_WAIT="${G0_IDLE_WAIT:-600}"
    [ -z "$zb" ] || [ -f "$zb" ] || { echo "g0.sh run: no $zb" >&2; exit 2; }
    [ -z "${G0_RESULTS:-}" ] || { echo "g0.sh run: G0_RESULTS and G0_PERF_DIR are for g0.sh baselines" >&2; exit 2; }
    mkdir -p "$perf"
    preflight
    # P15 is information only: its result never becomes the verdict (a rehearsal skips it)
    if [ "$quick" = 1 ]; then say "REHEARSAL (--quick): not a measurement; one launch per scenario and side, idle guard ${ABAB_IDLE_WAIT}s"
    else
      say "P15 (host, before the VM): $p15 $perf/p15.txt"
      if "$p15" "$perf/p15.txt" >> "$log" 2>&1; then say "P15: ok"; else say "P15: FAILED (information, not the verdict)"; fi
    fi
    vm_up=1; trap '[ "${vm_up:-0}" = 1 ] && "$vm" delete >> "$log" 2>&1' EXIT
    trap 'say "STOP: interrupted"; exit 2' INT TERM HUP
    "$vm" up "g0-$day" >> "$log" 2>&1 || die "the VM didn't come up (or DJ isn't logged in: the owner's login, never worked around)"
    "$vm" push "$zip" >> "$log" 2>&1 || die "the harness or mac-A couldn't be pushed"
    if [ "$quick" = 1 ]; then lab="$(label rh "rehearsal-$stamp")"
    elif [ -n "$zb" ]; then lab="$(label ab "gate-$stamp")"; else lab="$(label aa "g0-aa-$stamp")"; fi
    n=(); [ "$quick" = 0 ] || n=(-n 1)
    step "the session" "$abab" "$lab" "$zip" ${zb:+"$zb"} ${n[@]+"${n[@]}"} ${sc[@]+"${sc[@]}"}; rc=$?
    # a rehearsal's verdict is information: every launch ran (0 or 1) is a passed rehearsal
    if [ "$quick" = 1 ] && [ "$rc" = 1 ]; then say "rehearsal: every launch ran; its verdict (1) is information, not a gate"; worst=0; rc=0; fi
    # ... and so is compare's measurement failure when no launch failed or was disturbed: a rehearsal
    # has fewer launches than the plan by design. A missing sample is named (a probe gap or a
    # finding to look at), never the exit.
    if [ "$quick" = 1 ] && [ "$rc" = 3 ] && [ -f "$r/$lab/session.txt" ] && ! grep -qE '^(failed|disturbed|warmup_failed)=' "$r/$lab/session.txt"; then
      gaps="$(sed -n 's/^compare: MEASUREMENT FAILED: //p' "$r/$lab/compare.txt" 2>/dev/null | tr ';' '\n' | sed 's/^ *//' | grep -v 'launches, the plan has' | paste -sd ';' -)"
      [ -z "$gaps" ] || say "rehearsal: missing samples to look at (information): $gaps"
      say "rehearsal: every launch ran; compare's measurement failure (fewer launches than the plan) is information, not a gate"
      worst=0; rc=0
    fi
    if "$vm" delete >> "$log" 2>&1; then vm_up=0; else say "the VM couldn't be deleted (tart list)"; [ "$worst" -lt 3 ] && worst=3; fi
    [ -n "$zb" ] || [ "$rc" != 0 ] || baselines "$lab"
    say "done: $r/$lab (exit $worst)"; exit "$worst" ;;
  baselines)
    mkdir -p "$perf"; lab="${2:-$(last aa)}"; [ -n "$lab" ] || die "no aa= label in $labels"
    baselines "$lab"; exit "$worst" ;;
  *) sed -n '2,/^set -uo/p' "$0" | sed '$d'; exit 2 ;;
esac
