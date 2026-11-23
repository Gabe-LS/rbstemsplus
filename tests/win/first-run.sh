#!/bin/bash
# The first supervised real run of tests/win/vm-run.sh (release.md T16/T17): the owner at the Mac
# (Little Snitch's prompts for the HTTP server; the questions below), outside the Mac sandbox, the
# VM rb-win-template free (no other lane on it). Each step is bounded and the script stops at the
# first that fails, saying which and why; every vm-run.sh run stops and reverts the VM on its own
# way out (also when this script's bound ends it), so a stop leaves the VM at its snapshot.
#
#   tests/win/first-run.sh [--dry-run] [--from N]
#     --dry-run   say every step's commands and checks, check what needs no VM; nothing is run,
#                 prlctl is never called
#     --from N    start at step N (after a stop that was fixed)
#
#  0  stage --build: this checkout's test inputs and repo/ into the share
#  1  smoke on "clean Windows": SYSTEM, the user through hidden.exe and through conhost
#     --headless, the served build; then the owner says whether a window showed on the Mac
#  2  smoke's logs: every step's wrapper line (SYSTEM ran -File from \\Mac\win, and its child)
#  3  exit codes through prlctl (first-run/codes.suite): 0, 3 kept; 256 and -196608 are 1
#  4  HTTP to 10.211.55.2:8766 got through: the sha the VM read is the share's payload.json's
#  5  a deliberate timeout (first-run/timeout.suite): TIMEOUT, the guest's job ended
#  6  the VM stopped at its snapshot, the lock released
#  7  platform, bootstrap, uninstall, update, one at a time, no --record: none fails (a SKIP is
#     said); the host can delete dist-test/current afterwards
#  8  Defender's query as SYSTEM: uninstall's defender step passed
#  9  one --record run per row of tests/win/suites/release.list, then vm-run.sh check-vm HEAD: it
#     must fail (exit 1) with exactly one line for each row no suite can pass yet and nothing
#     else: "no record of S" for a row with no suite; "S ... failed: F.failed" for a row whose
#     record, the one this step just made, skipped only a known gap's step (GAPS below)
# Its log: the share's results/first-run-<time>.log. Steps 1 and 7 write the results folders that
# steps 2, 4 and 8 read into the share's results/first-run.state, with HEAD (so --from N reads this
# commit's runs, never just the newest folder).
set -uo pipefail
here="$(cd "$(dirname "$0")/../.." && pwd)"
root="${VMRUN_ROOT:-$here}"
VR="$here/tests/win/vm-run.sh"
FR="$here/tests/win/first-run"
PRLCTL="${VMRUN_PRLCTL:-prlctl}"
UUID="${VMRUN_UUID:-202a80c8-4bc4-4e95-890e-a31d6bfd5b70}"
SHARE="${VMRUN_SHARE:-$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win}"
GATES="${VMRUN_GATES:-$root/build/gates}"   # vm-run.sh's records
STATE="$SHARE/results/first-run.state"
ASK_SECS="${FIRST_RUN_ASK_SECS:-600}"   # how long a question waits for the owner
# SUITE:STEP pairs that SKIP by design. Remove worker:cache-hit when the release gate accepts the
# PC cache-hit record (M6). The current VM suite deliberately skips it, and check fails closed.
GAPS="worker:cache-hit"
dry=0 from=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) dry=1; shift ;;
    --from) [ $# -ge 2 ] && [[ "$2" =~ ^[0-9]$ ]] || { echo "usage: first-run.sh [--dry-run] [--from N]" >&2; exit 2; }; from="$2"; shift 2 ;;
    *) echo "usage: first-run.sh [--dry-run] [--from N]" >&2; exit 2 ;;
  esac
done
say() { echo "[$(date +%H:%M:%S)] $*"; }
fail() { say "STOP at step $STEP: $*"; say "the VM: $(vm_state_said)"; exit 1; }
vm_state_said() { [ "$dry" = 1 ] && { echo "(dry run: not asked)"; return; }; sed -n 's/^[[:space:]]*State:[[:space:]]*//p' <<< "$(bounded 60 "$PRLCTL" list -i "$UUID" 2> /dev/null)" | head -1; }

# bounded SECS CMD...: as vm-run.sh's: CMD in its own group, TERM after SECS (124), or on INT, TERM
# or HUP (130); then CMD's own cleanup is waited for, as long as vm-run.sh's can take (up to 240 s
# to stop the VM, 600 s to revert it and 240 s to stop it again: 1080 s, VT4), and only then
# KILL. Once CMD ends, whatever it left running in its group is killed too (VT6: a leftover
# holding the output open). With BOUNDED_PIDFILE set, its own pid goes there once its handlers are
# set (vmrun's TERM, VX10)
bounded() {
  perl -e '
    use POSIX ":sys_wait_h";
    my $s = shift;
    my $pid = fork();
    die "fork: $!\n" unless defined $pid;
    if ($pid == 0) { setpgrp(0, 0); exec { $ARGV[0] } @ARGV or exit 127; }
    setpgrp($pid, $pid);            # both sides set the group: it exists before any signal is handled
    $SIG{ALRM} = sub { finish(124) };
    $SIG{INT} = $SIG{TERM} = $SIG{HUP} = sub { finish(130) };
    if (my $pf = $ENV{BOUNDED_PIDFILE}) { my $h; if (open($h, ">", $pf)) { print $h "$$\n"; close $h; } }
    alarm $s;
    sub finish {
        # once: no second signal or the armed alarm may enter it again mid-grace, which would
        # cut the cleanup of vm-run.sh (stop, revert, unlock) short (VX5)
        alarm 0; $SIG{$_} = "IGNORE" for qw(INT TERM HUP ALRM);
        kill "TERM", -$pid;
        for (1 .. 2160) { last if waitpid($pid, WNOHANG) != 0; select(undef, undef, undef, 0.5); }
        kill "KILL", -$pid; waitpid($pid, 0); exit $_[0];
    }
    waitpid($pid, 0);
    my $st = $?;
    alarm 0;
    kill "KILL", -$pid;
    exit(($st & 127) ? 128 + ($st & 127) : $st >> 8);' "$@"
}

# ask QUESTION WANT: the owner's y/n (stdin), WANT the answer that goes on; no answer in time stops
ask() {
  local a=""
  [ "$dry" = 1 ] && { say "would ask: $1 [y/n] (goes on with $2)"; return 0; }
  printf '%s [y/n] ' "$1"
  read -r -t "$ASK_SECS" a || fail "no answer within $ASK_SECS s to: $1"
  case "$a" in y|Y|yes) a=y ;; n|N|no) a=n ;; *) fail "\"$a\" isn't y or n: $1" ;; esac
  [ "$a" = "$2" ] || fail "the answer was $a to: $1"
}

# vmrun SECS ARGS...: vm-run.sh ARGS, bounded, its output streamed to the log as it comes (VT5:
# the lanes' 10-minute checks see it) and kept in $OUT, its status in $RC, its results folder in
# $RES ("" when it made none). The tee and the sed ignore INT, TERM and HUP: a Ctrl-C reaches
# bounded, which ends vm-run.sh and waits for its cleanup, whose lines still have somewhere to go.
# The pipeline runs in the background and this waits for it: an INT, TERM or HUP to this script's
# own pid goes on to bounded as a TERM (its pid from BOUNDED_PIDFILE), and the wait goes on until
# the pipeline ends, vm-run.sh's cleanup with it (VX10: else vm-run.sh would live on)
VMRUN_SIG=0 VMRUN_SENT=0 VMRUN_PIDF=""
vmrun_term() {
  local p
  [ "$VMRUN_SENT" = 0 ] || return 0
  p="$(cat "$VMRUN_PIDF" 2> /dev/null)"
  [[ "$p" =~ ^[1-9][0-9]*$ ]] || return 0
  kill -TERM "$p" 2> /dev/null; VMRUN_SENT=1
}
vmrun() {
  local secs="$1" tmp bg; shift
  if [ "$dry" = 1 ]; then say "would run (at most ${secs}s): vm-run.sh $*"; RC=0; RES=""; OUT=""; return 0; fi
  say "vm-run.sh $* (at most ${secs}s)"
  tmp="$(mktemp "${TMPDIR:-/tmp}/first-run.XXXXXX")" || fail "no temp file"
  VMRUN_PIDF="$tmp.pid" VMRUN_SIG=0 VMRUN_SENT=0; rm -f "$tmp.pid" "$tmp.rc"
  trap 'VMRUN_SIG=1; vmrun_term' INT TERM HUP
  { BOUNDED_PIDFILE="$tmp.pid" bounded "$secs" "$VR" "$@" < /dev/null; echo $? > "$tmp.rc"; } 2>&1 \
    | (trap '' INT TERM HUP; exec tee "$tmp") | (trap '' INT TERM HUP; exec sed -l 's/^/    /') &
  bg=$!
  # a trap ends the wait early: wait again (and send the TERM once bounded's pid is there)
  while kill -0 "$bg" 2> /dev/null; do
    if [ "$VMRUN_SIG" = 1 ] && [ "$VMRUN_SENT" = 0 ]; then vmrun_term; sleep 0.2; else wait "$bg"; fi
  done
  wait "$bg" 2> /dev/null
  trap - INT TERM HUP
  RC="$(cat "$tmp.rc" 2> /dev/null)"; [ -n "$RC" ] || RC=130; [ "$VMRUN_SIG" = 0 ] || RC=130
  OUT="$(cat "$tmp")"; rm -f "$tmp" "$tmp.pid" "$tmp.rc"
  RES="$(sed -n 's/^\[[0-9:]*\] results: //p' <<< "$OUT" | sed 's/ ([0-9]* skipped)$//' | tail -1)"
  [ "$RC" != 124 ] || fail "vm-run.sh $1 went past its ${secs}s bound (ended; it reverts the VM)"
  [ "$RC" != 130 ] || fail "interrupted during vm-run.sh $1 (ended after its cleanup; it reverts the VM)"
}
# state_put KEY DIR: the results folder of step KEY's run, with HEAD, in first-run.state (VT9):
# "KEY HEAD DIR", DIR last so a folder with a space reads back whole (VX2); a line in the old
# "KEY DIR HEAD" order fails closed (its HEAD field isn't HEAD: run the step again)
state_put() {
  [ "$dry" = 1 ] && { say "would record: $1's results folder in $STATE"; return 0; }
  [ -n "$2" ] || fail "the $1 run named no results folder"
  { grep -v "^$1 " "$STATE" 2> /dev/null; printf '%s %s %s\n' "$1" "$HEAD" "$2"; } > "$STATE.tmp" && mv "$STATE.tmp" "$STATE" || fail "can't write $STATE"
}
# state_get KEY: STATE_DIR, the results folder first-run.state names for KEY at this HEAD, else
# stop (run the step that writes it first); never a subshell, so its stop ends the script
state_get() {
  local k="" h="" d=""
  STATE_DIR=""
  [ "$dry" = 1 ] && { STATE_DIR="(the $1 run's results)"; return 0; }
  read -r k h d < <(grep "^$1 " "$STATE" 2> /dev/null | tail -1)
  [ -n "$d" ] || fail "$STATE names no $1 run (run its step first: smoke is step 1, uninstall step 7)"
  [ "$h" = "$HEAD" ] || fail "$STATE's $1 run is of ${h:-?}, not HEAD $HEAD (run its step again)"
  STATE_DIR="$d"
}
# check_ok RC EXPECTED OUT: whether step 9's check result is the one it must be: RC 0 with no row
# expected to fail closed, or RC 1 whose lines (the non-empty ones) are exactly EXPECTED's, each
# once, in any order (VT7); else says why (the lines that weren't expected first, then the
# expected ones that didn't come), exit 1
check_ok() {
  local rc="$1" want got left missing
  want="$(grep . <<< "$2" | LC_ALL=C sort)"; got="$(grep . <<< "$3" | LC_ALL=C sort)"
  if [ "$rc" = 0 ]; then
    [ -z "$want" ] && return 0
    echo "it passed, but must fail closed on: $(tr '\n' ';' <<< "$want")"; return 1
  fi
  [ "$rc" = 1 ] || { echo "status $rc${got:+: $(tr '\n' ';' <<< "$got")}"; return 1; }
  [ -n "$want" ] || { echo "${got:-status 1, nothing said}"; return 1; }
  [ "$got" = "$want" ] && return 0
  left="$(LC_ALL=C comm -13 <(printf '%s\n' "$want") <(printf '%s\n' "$got") | grep . | tr '\n' ';')"
  missing="$(LC_ALL=C comm -23 <(printf '%s\n' "$want") <(printf '%s\n' "$got") | grep . | tr '\n' ';')"
  echo "${left}${missing:+ (not said: $missing)}"
  return 1
}
# expect_line FILE LINE-REGEX WHAT: FILE has a line matching, else stop
expect_line() { [ "$dry" = 1 ] && { say "would check: $3"; return 0; }; grep -qE "$2" "$1" 2> /dev/null || fail "$3 (not in $1)"; say "  ok: $3"; }

STEP=-
step() { STEP="$1"; shift; [ "$STEP" -ge "$from" ] || return 1; say "== step $STEP: $*"; return 0; }

# ---- what needs no VM
[ -x "$VR" ] || { echo "first-run.sh: no $VR" >&2; exit 2; }
[ -d "$SHARE" ] || { echo "first-run.sh: no share $SHARE" >&2; exit 2; }
[ -f "$SHARE/dist-test/payload.json" ] || { echo "first-run.sh: no TEST build in $SHARE/dist-test (smoke serves it)" >&2; exit 2; }
[ -f "$SHARE/probe/hidden.exe" ] || { echo "first-run.sh: no $SHARE/probe/hidden.exe (build/win/probe/hidden.c)" >&2; exit 2; }
if [ "$dry" = 0 ]; then
  mkdir -p "$SHARE/results"
  log="$SHARE/results/first-run-$(date +%Y%m%d-%H%M%S).log"
  # its tee ignores INT, TERM and HUP: a Ctrl-C mustn't end it before vm-run.sh's cleanup has said
  # its lines (they would die of SIGPIPE)
  exec > >(trap '' INT TERM HUP; exec tee -a "$log") 2>&1
  say "log: $log"
fi
HEAD="$(git -C "$root" rev-parse HEAD)"
say "first run of ${HEAD:0:9} against $UUID, share $SHARE$([ "$dry" = 0 ] || echo ' (DRY RUN)')"
ask "Are you at the Mac to answer Little Snitch and these questions, with no other lane on rb-win-template?" y

if step 0 "stage --build (outside the Mac sandbox)"; then
  vmrun 1800 stage --build
  [ "$RC" = 0 ] || fail "stage --build failed"
fi

if step 1 "smoke on \"clean Windows\""; then
  vmrun 1800 run "clean Windows" smoke
  [ "$RC" = 0 ] || fail "the smoke run didn't pass (exit $RC)"
  state_put smoke "$RES"
  ask "Did any window or terminal show on the Mac's screen during the smoke run?" n
fi

if step 2 "smoke's logs: each step's wrapper line"; then
  state_get smoke; smoke_res="$STATE_DIR"
  for s in 1-facts 2-user 3-user-signed 4-http; do
    expect_line "${smoke_res%/}/$s.log" "^vm-run-exit [^ ]+ ${s%%-*} 0" "$s ran to its wrapper's end with 0"
  done
  expect_line "${smoke_res%/}/results.txt" "^step sys facts PASS 0 " "facts ran as SYSTEM from \\\\Mac\\win (it fails otherwise)"
fi

if step 3 "exit codes through prlctl and the wrapper"; then
  vmrun 1800 run "clean Windows" "$FR/codes.suite"
  [ "$dry" = 1 ] || [ "$RC" = 1 ] || fail "the codes run should end 1 (its failures are expected), it ended $RC"
  expect_line "$RES/results.txt" "^step sys zero PASS 0 " "exit 0 is PASS 0"
  expect_line "$RES/results.txt" "^step sys three FAIL 3 " "exit 3 reaches the Mac as 3"
  expect_line "$RES/results.txt" "^step sys big FAIL 1 " "exit 256 is FAIL 1, not the 0 it truncates to"
  expect_line "$RES/results.txt" "^step sys unreadable FAIL 1 " "a child's -196608 (a -File it can't read) is FAIL 1"
fi

if step 4 "HTTP to the Mac's server: the VM read the share's payload.json"; then
  state_get smoke; smoke_res="$STATE_DIR"
  want="$(shasum -a 256 "$SHARE/dist-test/payload.json" | cut -c1-64)"
  expect_line "${smoke_res%/}/4-http.log" "^sha256 $want" "the VM's GET of payload.json has the share's sha256 ($want)"
fi

if step 5 "a deliberate timeout"; then
  vmrun 900 run "clean Windows" "$FR/timeout.suite"
  [ "$dry" = 1 ] || [ "$RC" = 1 ] || fail "the timeout run should end 1, it ended $RC"
  expect_line "$RES/results.txt" "^step sys sleeper TIMEOUT 124 " "the 10 s step is a TIMEOUT"
  expect_line "$RES/1-kill.log" "^vm-run-kill: 0 left" "its processes in the VM were ended"
fi

if step 6 "the VM stopped at its snapshot, the lock released"; then
  if [ "$dry" = 1 ]; then say "would check: prlctl list -i $UUID says stopped; no $SHARE/.vm-run.lock"
  else
    st="$(vm_state_said)"
    [ "$st" = stopped ] || fail "the VM is \"$st\", not stopped"
    [ ! -e "$SHARE/.vm-run.lock" ] || fail "$SHARE/.vm-run.lock is still there"
    say "  ok: the VM is stopped, no lock"
  fi
fi

if step 7 "platform, bootstrap, uninstall, update (no --record)"; then
  for s in platform bootstrap uninstall update; do
    vmrun 5400 run "clean Windows" "$s"
    [ "$RC" = 0 ] || fail "the $s run didn't pass (exit $RC): its results in ${RES:-the output above}"
    [ "$dry" = 1 ] || ! grep -q " SKIP " "$RES/results.txt" || say "  $s: SKIPs (inputs missing, said in each log): $(grep ' SKIP ' "$RES/results.txt" | awk '{print $3}' | tr '\n' ' ')"
    [ "$s" != uninstall ] || state_put uninstall "$RES"
  done
  if [ "$dry" = 1 ]; then say "would check: the host can delete $SHARE/dist-test/current"
  elif [ -e "$SHARE/dist-test/current" ]; then
    rm -rf "$SHARE/dist-test/current" 2> /dev/null || fail "the host can't delete $SHARE/dist-test/current (root's: see update.suite's header)"
    say "  ok: dist-test/current deleted by the host"
  else say "  ok: no dist-test/current left"
  fi
fi

if step 8 "Defender's query as SYSTEM"; then
  state_get uninstall
  expect_line "${STATE_DIR%/}/results.txt" "^step sys defender PASS 0 " "the defender step passed"
fi

# only_gaps SUITE REC: REC is a skip record of this HEAD whose steps all passed but SUITE's known
# gaps, each a SKIP (77), at least one; nothing else in it went wrong (no note)
only_gaps() {
  local st n=0
  grep -qx "commit $head" "$2" 2> /dev/null && grep -qx 'result skip' "$2" && ! grep -q '^note ' "$2" || return 1
  while read -r _ _ st v code _; do
    if [ "$v $code" = "PASS 0" ]; then continue; fi
    [ "$v $code" = "SKIP 77" ] && [[ " $GAPS " == *" $1:$st "* ]] || return 1
    n=$((n + 1))
  done < <(grep '^step ' "$2")
  [ "$n" -gt 0 ]
}

# Match the row's snapshot name or ID as vm-run.sh's rec_for does, then require that the sole
# matching record belongs to the run step 9 just made.
record_for_run() {
  local suite="$1" arch="$2" snap="$3" run_id="$4" r s
  local recs=()
  for r in "$GATES/$head/vm-win/$suite@"*"-$arch.ok" "$GATES/$head/vm-win/$suite@"*"-$arch.failed"; do
    [ -f "$r" ] || continue
    s="$(sed -n 's/^snapshot //p' "$r" | head -1)"
    if [ "${s%% *}" = "$snap" ] || [ "${s#* }" = "$snap" ]; then recs+=("$r"); fi
  done
  [ "${#recs[@]}" = 1 ] || fail "$suite on \"$snap\" ($arch) has ${#recs[@]} records in $GATES/$head/vm-win, expected one"
  record="${recs[0]}"
  [ -n "$run_id" ] && [ "$(grep -c '^run ' "$record")" = 1 ] && grep -Fxq "run $run_id" "$record" \
    || fail "$suite's record ${record##*/} isn't the run step 9 just made (${run_id:-no run ID})"
}

if step 9 "the record: each row of release.list with --record, then check"; then
  head="$HEAD"
  [ "$dry" = 1 ] || [ "$(git -C "$root" rev-parse HEAD)" = "$head" ] || fail "HEAD moved during the run (it was ${head:0:9})"
  [ "$dry" = 1 ] || [ -z "$(git -C "$root" status --porcelain)" ] || fail "--record needs a clean checkout ($root)"
  nosuite="" gaps="" expected=""
  while read -r suite arch snap; do
    case "$suite" in ''|'#'*) continue ;; esac
    # a row whose suite isn't written yet can't be run: check fails closed on it, with exactly
    # this line
    if ! git -C "$root" cat-file -e "$head:tests/win/suites/$suite.suite" 2> /dev/null; then
      say "  $suite on \"$snap\" ($arch): no $suite.suite yet, not run"; nosuite+=" $suite"
      expected+="vm-run.sh check: no record of $suite on \"$snap\" ($arch) in $GATES/$head/vm-win"$'\n'
      continue
    fi
    vmrun 5400 run "$snap" "$root/tests/win/suites/$suite.suite" --arch "$arch" --record
    [ "$RC" = 0 ] || fail "the --record run of $suite on \"$snap\" ($arch) didn't pass (exit $RC)"
    [ "$dry" = 0 ] || continue
    record_for_run "$suite" "$arch" "$snap" "${RES##*/}"
    if [[ "$record" == *.failed ]] && only_gaps "$suite" "$record"; then
      gaps+=" $suite"
      expected+="vm-run.sh check: $suite on \"$snap\" ($arch) failed: ${record##*/}"$'\n'
    fi
  done < <(git -C "$root" show "$head:tests/win/suites/release.list")
  vmrun 120 check-vm "$head"
  if [ "$dry" = 0 ]; then
    # exit 1 with exactly the expected lines, one per such row, and at least one; or a pass with
    # none expected (check_ok); anything else stops
    why="$(check_ok "$RC" "$expected" "$OUT")" || fail "vm-run.sh check $head failed: $why"
    if [ -n "$nosuite$gaps" ]; then say "  ok: check fails closed only on the rows no suite can pass yet:${nosuite:+ no suite:$nosuite;}${gaps:+ only a known gap skipped ($GAPS):$gaps} (as release.md section 8 asks)"
    else say "  ok: check passed"; fi
  fi
fi
STEP=-
say "first run: every step passed$([ "$dry" = 0 ] || echo ' (DRY RUN: nothing was run)')"
