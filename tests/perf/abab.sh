#!/bin/bash
# One performance session of the Mac app in the tart VM (the private Windows plan's mac-core §2.2): A and
# B interleaved launch by launch (A B, then B A: a drift in the VM falls on both alike), a fixed
# number of launches per scenario (thresholds.txt's @launches mac-A lines), then the verdict.
# EXCLUSIVE: nothing else runs on the host.
#
#   tests/perf/abab.sh LABEL A.zip B.zip [-n LAUNCHES] [--scenarios "perf-start …"]
#                       a gate, then tests/perf/stats.py compare (compare.txt, compare.tsv): exit 0
#                       pass, 1 a regression (each regressed row named), 3 the measurement failed
#   tests/perf/abab.sh LABEL A.zip [-n LAUNCHES] [--scenarios "…"]
#                       A alone (mac-A's reference, tests/perf/g0.sh): exit 0, or 3
# -n sets every scenario's launches (smoke runs); --scenarios runs only those (a scenario that
# isn't in the plan needs -n). One unscored warm-up launch per side comes before each scenario (warmup=SIDE/
# SCENARIO; the first launch after a scenario switch is cold). A launch that fails ends the
# session: the measurement failed (failed= in session.txt, exit 3). A launch during which the Mac
# had input runs again once per session, after 5 idle minutes (disturbed_retried=); a second one
# ends the session (disturbed=, exit 3). An exit 2 is a stop (a refused start, an idle time that
# can't be read).
#
# ABAB_IDLE_WAIT=SECS (default 600; 0 turns the guard off: smoke runs only; tests/perf/idle.sh, PERF_IDLE_CMD its stub):
# before the first launch, wait until the Mac has had no keyboard, mouse or trackpad input for SECS
# (ioreg's HIDIdleTime); while input continues (idle under 60 s), wait for 5 idle minutes before a
# launch; each launch's idle time at its start (idle_at=), the display-sleep state at the start and
# the end. A launch with input during it is disturbed.
#
# Each side's local servers (perf-under-sheet, perf-update) are made from that side's own TEST
# build files and test key: SERVER_SRC_A/SERVER_KEY_A and SERVER_SRC_B/SERVER_KEY_B, else the zip's
# own folder when it holds the model and payload.json (a dist-test/; else SERVER_SRC, else
# dist-test/), and its key next to it (test-keys/ in the folder, as the gate archive keeps it, or
# the checkout's build/test-keys/, else SERVER_KEY). The served payload.json must verify with the
# side's key, or nothing runs.
#
# LABEL is a new folder each session (an existing one is refused: launches of two sessions never
# mix; stats.py also checks every launch's build and clone against session.txt). The VM must be up
# with the harness pushed (tests/harness/vm.sh up, push). Every launch reinstalls the side's app
# first. PERF_IDLE_S, PERF_P12_S and PERF_ROUNDS stay unset for a gate (stats.py refuses a session
# that set them).
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
vm="${ABAB_VM:-$root/tests/harness/vm.sh}"          # ABAB_VM: the selftest's stub
clone_file="${ABAB_CLONE_FILE:-$root/build/harness/clone}"   # ABAB_CLONE_FILE: the selftest's own (vm.sh's pointer)
stats="${ABAB_STATS:-$root/tests/perf/stats.py}"
# shellcheck source=idle.sh
. "$root/tests/perf/idle.sh"
idle=""
die() { echo "abab.sh: $*" >&2; exit 2; }
[ $# -ge 2 ] || die "usage: abab.sh LABEL A.zip [B.zip] [-n LAUNCHES] [--scenarios \"…\"]"
label="$1"; za="$2"; shift 2; zb=""; launches=""; scen=""
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then zb="$1"; shift; fi
while [ $# -gt 0 ]; do
  case "$1" in
    -n) launches="${2:-}"; shift 2 ;;
    --scenarios) scen="${2:-}"; shift 2 ;;
    *) die "unknown $1" ;;
  esac
done
case "$launches" in ''|[1-9]|[1-9][0-9]) ;; *) die "-n LAUNCHES: 1-99" ;; esac
mode=ab sides="A B"; [ -n "$zb" ] || { mode=a sides=A zb="$za"; }
for f in "$za" "$zb"; do [ -f "$f" ] || die "no $f"; done
out="$root/build/harness/results/$label"; ss="$out/session.txt"
[ ! -e "$out" ] || [ -z "$(ls -A "$out" 2>/dev/null)" ] || die "$out exists: a new label for each session (launches of two sessions must never mix)"

# the plan: "SCENARIO LAUNCHES" entries, in thresholds.txt's order
plan_txt="$(python3 -I "$root/tests/perf/rules.py" plan)" || { echo "abab.sh: $plan_txt" >&2; exit 3; }
entries=()
while read -r s n; do
  [ -n "$s" ] || continue
  if [ -n "$scen" ] && [[ " $scen " != *" $s "* ]]; then continue; fi
  entries+=("$s ${launches:-$n}")
done <<< "$plan_txt"
for s in $scen; do      # scenarios named but not in the plan (the selftest's, a smoke run's)
  printf '%s\n' "${entries[@]+"${entries[@]}"}" | grep -q "^$s " && continue
  [ -n "$launches" ] || die "$s isn't in the plan (thresholds.txt @launches): give -n"
  entries+=("$s $launches")
done
[ ${#entries[@]} -gt 0 ] || die "nothing to run"

side_src() {   # side_src ZIP OVERRIDE
  local d; d="$(cd "$(dirname "$1")" && pwd)"
  if [ -n "$2" ]; then echo "$2"
  elif [ -f "$d/stemsplus-model.onnx" ] && [ -f "$d/payload.json" ]; then echo "$d"
  else echo "${SERVER_SRC:-$root/dist-test}"; fi
}
side_key() {   # side_key ZIP OVERRIDE
  local d; d="$(cd "$(dirname "$1")" && pwd)"
  if [ -n "$2" ]; then echo "$2"
  elif [ -f "$d/test-keys/test-key.pem" ]; then echo "$d/test-keys/test-key.pem"
  elif [ -f "$d/../build/test-keys/test-key.pem" ]; then echo "$(cd "$d/../build/test-keys" && pwd)/test-key.pem"
  else echo "${SERVER_KEY:-$root/build/test-keys/test-key.pem}"; fi
}
src_a="$(side_src "$za" "${SERVER_SRC_A:-}")"; key_a="$(side_key "$za" "${SERVER_KEY_A:-}")"
src_b="$(side_src "$zb" "${SERVER_SRC_B:-}")"; key_b="$(side_key "$zb" "${SERVER_KEY_B:-}")"
[ "$mode" = ab ] || { src_b="$src_a"; key_b="$key_a"; }
verify_side() {   # verify_side SRC KEY: its payload.json verifies with the key that will sign the servers
  [ -f "$1/payload.json" ] && [ -f "$1/stemsplus-model.onnx" ] || { echo "abab.sh: $1 isn't a TEST build folder (payload.json, the model)" >&2; return 1; }
  local pub r; pub="$(mktemp "${TMPDIR:-/tmp}/abab-pub.XXXXXX")"
  /usr/bin/openssl ec -in "$2" -pubout > "$pub" 2>/dev/null \
    && /usr/bin/openssl dgst -sha256 -verify "$pub" -signature "$1/payload.json.sig" "$1/payload.json" >/dev/null 2>&1
  r=$?; rm -f "$pub"
  [ $r = 0 ] || echo "abab.sh: $1/payload.json doesn't verify with $2: that side's servers would be refused" >&2
  return $r
}
verify_side "$src_a" "$key_a" || exit 2
verify_side "$src_b" "$key_b" || exit 2

mkdir -p "$out"
{
  echo "label=$label mode=$mode clone=$(cat "$clone_file" 2>/dev/null || echo rbsp-harness) started=$(date -u +%FT%TZ)"
  echo "A=$za sha256=$(shasum -a 256 "$za" | cut -d' ' -f1)"
  echo "B=$zb sha256=$(shasum -a 256 "$zb" | cut -d' ' -f1)"
  echo "server_A=$src_a key_A=$key_a"
  echo "server_B=$src_b key_B=$key_b"
  echo "scenarios=${scen:-plan} PERF_IDLE_S=${PERF_IDLE_S:-default} PERF_P12_S=${PERF_P12_S:-default} PERF_ROUNDS=${PERF_ROUNDS:-default}"
  for side in $sides; do echo "plan_$side=$(printf '%s\n' "${entries[@]}" | tr ' ' ':' | paste -sd, -)"; done
} > "$ss"

stop() {   # stop RC LINE: LINE into session.txt, and the session ends
  echo "$2" >> "$ss"; echo "abab.sh: $2" >&2
  echo "finished=$(date -u +%FT%TZ) rc=$1" >> "$ss"; exit "$1"
}
wait_s="${ABAB_IDLE_WAIT:-600}"      # a gate always waits for the Mac left alone; 0: off (smoke runs, the selftest)
side_vars() {   # side_vars SIDE: zip, src and key of that side
  if [ "$1" = B ]; then zip="$zb" src="$src_b" key="$key_b"; else zip="$za" src="$src_a" key="$key_a"; fi
}
retried=0       # one disturbed launch per session runs again (the cause is outside the build)
launch() {      # launch SIDE K SCENARIO: one launch; a failed one ends the session (3), and so does
  # a second disturbed one
  local side="$1" k="$2" s="$3" zip src key t0 ok
  side_vars "$side"
  while :; do
    if [ "$wait_s" -gt 0 ]; then
      idle_try || stop 2 "idle_unreadable=$(date -u +%FT%TZ) before $side-$k/$s: '$idle'"
      if [ "$idle" -lt 60 ]; then idle_wait 300 "input continues" "$ss" || stop 2 "idle_unreadable=$(date -u +%FT%TZ) while waiting"; fi
      echo "idle_at=$side-$k $idle" >> "$ss"
    fi
    ok=1; t0="$(date +%s)"
    "$vm" install "$zip" && SERVER_SRC="$src" SERVER_KEY="$key" PERF_MODE=perf "$vm" run "$label/$side-$k" "$s" || ok=0
    echo "dur=$(( $(date +%s) - t0 )) $side-$k/$s" >> "$ss"
    [ "$ok" = 1 ] || stop 3 "failed=$side-$k/$s"      # a crash is a finding: never run again
    [ "$wait_s" -gt 0 ] || return 0
    idle_try || stop 2 "idle_unreadable=$(date -u +%FT%TZ) after $side-$k/$s: '$idle'"
    [ "$idle" -lt $(( $(date +%s) - t0 )) ] || return 0
    [ "$retried" = 0 ] || stop 3 "disturbed=$side-$k/$s (input during the launch: idle $idle s; the session's one retry is spent)"
    # the first disturbance: the same launch again (vm.sh replaces its results) after 5 idle minutes
    retried=1; echo "disturbed_retried=$side-$k/$s (input during the launch: idle $idle s)" >> "$ss"
    idle_wait 300 "a disturbed launch runs again" "$ss" || stop 2 "idle_unreadable=$(date -u +%FT%TZ) before the retry"
  done
}
trap 'echo "abab.sh: interrupted" >&2; exit 2' INT TERM HUP
if [ "$wait_s" -gt 0 ]; then
  idle_wait "$wait_s" "before the first launch" "$ss" || stop 2 "idle_unreadable=$(date -u +%FT%TZ) before the first launch"
  idle_display start "$ss"
fi
for e in "${entries[@]}"; do
  s="${e% *}"
  # one unscored warm-up launch per side before each scenario: its first launch after a switch is
  # cold (G0's perf-report: about 25 s against 19 s), and a 1-launch scenario would be all cold
  for side in $sides; do
    side_vars "$side"
    if "$vm" install "$zip" && SERVER_SRC="$src" SERVER_KEY="$key" PERF_MODE=perf "$vm" run "$label/warmup-$side" "$s"; then
      echo "warmup=$side/$s" >> "$ss"
    else
      echo "warmup_failed=$side/$s" >> "$ss"
    fi
  done
  for k in $(seq "${e##* }"); do
    order="$sides"; [ $((k % 2)) = 0 ] && [ "$mode" = ab ] && order="B A"
    for side in $order; do launch "$side" "$k" "$s"; done
  done
done
[ "$wait_s" -gt 0 ] && idle_display end "$ss"
rc=0
if [ "$mode" = ab ]; then
  python3 -I "$stats" compare "$out" --tsv "$out/compare.tsv" > "$out/compare.txt" 2>&1; rc=$?
  cat "$out/compare.txt"
fi
echo "finished=$(date -u +%FT%TZ) rc=$rc" >> "$ss"      # the session's verdict, the compare's in a gate
exit "$rc"
