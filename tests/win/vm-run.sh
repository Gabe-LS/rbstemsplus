#!/bin/bash
# Level 2 of release.md §8 (task T16): runs a suite of PowerShell steps in the Parallels VM
# rb-win-template, from a named snapshot, without a window on the owner's screen; the successor of
# tests/win/probes/winps.sh. Only rb-win-template is ever driven: the owner's "Windows 11" VM is
# refused by name. Must run outside the Mac sandbox (prlctl can't read the process list inside it).
#
#   tests/win/vm-run.sh run SNAPSHOT SUITE [OPTIONS]
#       reverts to SNAPSHOT (its name, or its {id}), switches Pause idle off, starts the VM and
#       waits for `cmd /c ver`; copies hidden.exe in; serves the TEST build over HTTP; runs SUITE's
#       steps (tests/win/suites/SUITE.suite, or a path) as SYSTEM or as the logged-in user, each
#       with its own timeout; collects every step's output and status into
#       build/win/results/<run>/; stops the VM and reverts it to SNAPSHOT again.
#       Exit 0: every step passed; 1: a step failed or timed out; 2: the run couldn't start or
#       the VM couldn't be driven (said).
#     --serve DIR   the folder served at http://10.211.55.2:8766/ (default build/win/dist-test:
#                   the TEST build, scripts/build.sh --test http://10.211.55.2:8766/); inside the
#                   share build/win only. Served when it exists; a suite with "need http" needs it
#     --record      a release run (I76): build/gates/<commit>/vm-win/<suite>@<snapshot id>-<arch>.ok
#                   when every step passed (else .failed; a rerun of the same suite, snapshot and
#                   architecture replaces only that pair), with the commit, the suite, the
#                   snapshot (id and name), the VM's uuid, its architecture, the served payload.json's
#                   SHA-256, the staged repo/ (stage_commit) and vm/ (vm_commit, its files), each
#                   candidate and fixture ("served PATH SHA COMMIT"), the SHA-256 of the suite and
#                   every script it ran, and each step's result. Once the VM's architecture is
#                   known the pair holds an incomplete .failed until the run writes its own. Needs,
#                   checked under the lock and again before the record: a clean checkout whose
#                   HEAD is DIR/payload.json's commit, DIR/payload.json and DIR's bootstrap.ps1,
#                   .sig and zip byte-equal to the checkout's dist-test/, DIR's fault-*/ and
#                   bad-version/ copies made from them byte for byte, the candidates' commit HEAD,
#                   the suite's scripts in the checkout, `stage` run from HEAD (repo/.commit,
#                   repo/shas.txt, vm/shas.txt), and for a `need vm` suite vm/ built by
#                   `stage --build` from HEAD (vm/.commit). HEAD's tests/win/suites and vm-run.sh
#                   are frozen into the run's results folder (src/, by git archive) before the suite
#                   is read: the run parses, hashes and runs those bytes, never the checkout's
#                   (VX3, VX4). VMRUN_PRLCTL or VMRUN_UUID (test variables) need VMRUN_GATES too
#     --keep        leave the VM running after the steps (no stop, no revert): for diagnostics
#   tests/win/vm-run.sh check COMMIT [DIR]
#       the check release.sh's step 4b runs: every row of tests/win/suites/release.list (as
#       committed in COMMIT: SUITE ARCH SNAPSHOT) has one passing record for COMMIT and for the
#       payload.json in DIR (default dist-test/ of this checkout), whose scripts are COMMIT's, and
#       no failed one; a record passes when its arch, stage_commit, steps (COMMIT's suite's, in
#       order, each PASS 0), candidates (served ... COMMIT) and, for a need vm suite, vm_commit
#       are COMMIT's; then it writes build/gates/COMMIT/vm-win.ok (the rows and their records).
#       Exit 0 then, else 1 (said, and no vm-win.ok; a row with no suite yet is "no record").
#       Under the share's lock: 2 while a run holds it (said)
#   tests/win/vm-run.sh sys|user|user-high|user-signed [SECS] < SCRIPT.ps1
#       one script in the running VM, no revert (winps.sh's job): as SYSTEM (prlctl exec, no
#       window), or as the logged-in user through a scheduled task and C:\rbtest\hidden.exe
#       (user; user-high: with the user's full token), or through Windows' own conhost.exe
#       --headless (user-signed: runs when Smart App Control blocks our unsigned hidden.exe).
#       SECS: the timeout (default 600). Exit: the script's status, 124 on a timeout
#   tests/win/vm-run.sh stage [--build]
#       what the suites read from the share: repo/ (HEAD's scripts, tests/win, core/texts.def,
#       win/c/aumid.h; repo/.commit names HEAD, repo/shas.txt lists them) and vm/<arch>/ (the
#       test exes this checkout has built, x64 and arm64, and fixtures.txt; vm/shas.txt lists
#       them), replaced whole; and the TEST build's fixture releases in dist-test/
#       (stage-dist.sh) when it is there. --build: win/c/tests/run.sh, common/tests/win.sh and
#       the core DLL's test-hooks build first, and vm/.commit names HEAD when the checkout was
#       clean and HEAD didn't move. Says what is still missing
#   tests/win/vm-run.sh selftest
#       tests/win/vm-run-test.sh: everything above against a fake prlctl (no VM)
#
# A suite (tests/win/suites/*.suite) has one step per line, '#' starts a comment:
#   MODE SECS NAME SCRIPT        MODE sys|user|user-high|user-signed; SECS at most 600; NAME
#                                [A-Za-z0-9._-]; SCRIPT a .ps1 relative to the suite's folder
#   need http                    the run refuses to start without the served folder
#   need file REL                ... without the file REL in the share (repo/..., vm/x64/...)
#   need candidate REL           ... and REL, a payload.json, is this commit's release (--record)
#   need fixture REL             ... and REL is an older or made-up release (recorded as it is)
#   need vm                      the steps run vm/'s test exes (--record: built from HEAD)
#   include SCRIPT               SCRIPT (beside the suite) goes in front of every step's script
# Each step's script runs with these lines first: $ProgressPreference = 'SilentlyContinue' and
# $VmRun = @{ Url; Run; Share; Results; Step; Secs } (the served URL, the run's id, \\Mac\win, the
# run's results folder as the VM sees it, the step's name, its timeout), then the suite's
# includes. The step
# runs as a child powershell.exe of a host-written wrapper (N-NAME.wrap.ps1), whose last line
# "vm-run-exit RUN N CODE" carries its exit code (none, or one outside 0..255, is 1): the host
# reads that line, which must agree with prlctl's status; a missing line is a FAIL. A step passes
# when CODE is 0; 77 is SKIP (it can't run here, its log says why: never a pass for --record,
# which then writes vm-win.failed with "result skip").
#
# Why (the private Windows plan's phase0-probes §3, the VM notes): `prlctl exec --current-user` opens
# a terminal on the owner's screen, so user steps go through a scheduled task; prlctl limits a
# command's length, so every script goes through the share (\\Mac\win\job\<run>\); prlctl exec
# sometimes fails with "PrlJob_GetResult/GetRetCode: Invalid argument" having run nothing, so a
# start is tried 3 times; the snapshots have Pause idle on, and a paused VM hangs prlctl exec, so
# it is switched off after every revert; hidden.exe isn't in the snapshots, so it is copied in
# after every revert; files the VM writes to the share are root's on the Mac, so the VM writes
# only C:\rbtest and the host collects. Every wait is bounded, at most 10 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")/../.." && pwd)"
root="${VMRUN_ROOT:-$here}"                                   # the checkout a record names
self="$(perl -MCwd -e 'print Cwd::abs_path($ARGV[0])' "${BASH_SOURCE[0]}")"   # this runner (VX7)
PRLCTL="${VMRUN_PRLCTL:-prlctl}"
DEFAULT_UUID=202a80c8-4bc4-4e95-890e-a31d6bfd5b70           # rb-win-template: the one a record may name (VX8)
UUID="${VMRUN_UUID:-$DEFAULT_UUID}"
NAME=rb-win-template
OWNERS_VM='Windows 11'
# the VM shares only the main checkout's build/win (\\Mac\win), also from a worktree
SHARE="${VMRUN_SHARE:-$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win}"
GATES="${VMRUN_GATES:-$root/build/gates}"
SUITES="$here/tests/win/suites"
BIND="${VMRUN_HTTP_BIND:-10.211.55.2}"      # the Mac on the VM's shared network
PORT="${VMRUN_HTTP_PORT:-8766}"
LIMIT=600                                   # the longest wait of any kind, in seconds
BOOT="${VMRUN_BOOT:-600}"                   # how long the VM may take to answer `cmd /c ver`
POLL="${VMRUN_POLL:-5}"                     # seconds between the boot probes
stop() { echo "vm-run.sh: $*" >&2; exit 2; }
say() { echo "[$(date +%H:%M:%S)] $*"; }
sha() { shasum -a 256 "$1" | cut -c1-64; }

# bounded SECS CMD...: CMD in its own process group, stopped after SECS (exit 124 then: the group
# gets TERM, and KILL at most 5 s later), or when this gets INT, TERM or HUP (the same, exit 130);
# else CMD's own status. Whatever CMD left in its group is killed when it ends (a child holding
# our stdout open would keep a $(...) waiting)
bounded() {
  perl -e '
    use POSIX ":sys_wait_h";
    my $s = shift;
    my $pid = fork();
    die "fork: $!\n" unless defined $pid;
    if ($pid == 0) { setpgrp(0, 0); exec { $ARGV[0] } @ARGV or exit 127; }
    sub finish {
        # once: no second signal or the armed alarm may enter it again mid-grace (VX5)
        alarm 0; $SIG{$_} = "IGNORE" for qw(INT TERM HUP ALRM);
        kill "TERM", -$pid;
        for (1 .. 10) { last if waitpid($pid, WNOHANG) != 0; select(undef, undef, undef, 0.5); }
        kill "KILL", -$pid; waitpid($pid, 0); exit $_[0];
    }
    $SIG{ALRM} = sub { finish(124) };
    $SIG{INT} = $SIG{TERM} = $SIG{HUP} = sub { finish(130) };
    alarm $s;
    waitpid($pid, 0);
    my $st = $?;
    alarm 0;
    kill "KILL", -$pid;
    exit(($st & 127) ? 128 + ($st & 127) : $st >> 8);' "$@"
}

# ---- the VM ---------------------------------------------------------------------------------

# vm_info: prlctl's description of the VM (Name, State, ...); stops when it can't be read
vm_info() { bounded 60 "$PRLCTL" list -i "$UUID" 2> /dev/null || stop "prlctl list -i $UUID failed: is this outside the sandbox?"; }
vm_field() { sed -n "s/^[[:space:]]*$1:[[:space:]]*//p" <<< "$2" | head -1; }
# the one VM this script drives: by its uuid, and its name must be rb-win-template
vm_guard() {
  local info n
  info="$(vm_info)" || exit 2
  n="$(vm_field Name "$info")"
  [ "$n" != "$OWNERS_VM" ] || stop "$UUID is the owner's VM \"$OWNERS_VM\": never driven"
  [ "$n" = "$NAME" ] || stop "$UUID is \"$n\", not $NAME"
}
vm_state() { vm_field State "$(vm_info)"; }

# snap_id NAME-or-{ID}: "ID NAME", the snapshot's id and its name (also for an {ID}: prlctl's);
# stops when there is none or more than one
snap_id() {
  local list id n found=""
  case "$1" in
    \{*\})
      n="$(vm_field Name "$(bounded 60 "$PRLCTL" snapshot-list "$UUID" -i "$1" 2> /dev/null)")"
      [ -n "$n" ] || stop "no snapshot $1 on $NAME"
      echo "$1 $n"; return ;;
  esac
  list="$(bounded 60 "$PRLCTL" snapshot-list "$UUID" 2> /dev/null)" || stop "prlctl snapshot-list failed"
  for id in $(grep -o '{[0-9A-Fa-f-]*}' <<< "$list" | sort -u); do
    n="$(vm_field Name "$(bounded 60 "$PRLCTL" snapshot-list "$UUID" -i "$id" 2> /dev/null)")"
    if [ "$n" = "$1" ]; then
      [ -z "$found" ] || stop "two snapshots are named \"$1\": give its {id}"
      found="$id"
    fi
  done
  [ -n "$found" ] || stop "no snapshot named \"$1\" on $NAME"
  echo "$found $1"
}

# exec_sys GUESTFILE SECS: a script on the share run as SYSTEM, its output on stdout, its status
# returned (124: timed out). A start that failed with nothing run (a failed status, and prlctl's
# PrlJob line the whole output) is tried again, 3 times at most; each attempt's output is kept.
exec_sys() {
  local out rc try
  for try in 1 2 3; do
    out="$(bounded "$2" "$PRLCTL" exec "$UUID" powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$1" 2>&1)"
    rc=$?
    out="$(grep -v 'ps: Operation not permitted' <<< "$out")"
    if [ "$rc" != 0 ] && [ "$try" -lt 3 ] && [ "$(grep -c . <<< "$out")" = 1 ] && [[ "$out" == *PrlJob_Get*'Invalid argument'* ]]; then
      printf 'vm-run: attempt %d, nothing run: %s\n' "$try" "$out"
      say "prlctl didn't start the job: again ($try)" >&2; continue
    fi
    break
  done
  [ -z "$out" ] || printf '%s\n' "$out"
  return "$rc"
}

# revert SNAPSHOT-ID: switch, Pause idle off, started (or resumed), answering `cmd /c ver`
revert() {
  local st t0
  say "revert to $1"
  bounded "$LIMIT" "$PRLCTL" snapshot-switch "$UUID" -i "$1" > /dev/null || stop "prlctl snapshot-switch $1 failed"
  bounded 60 "$PRLCTL" set "$UUID" --pause-idle off > /dev/null || stop "Pause idle couldn't be switched off (prlctl exec would hang)"
  st="$(vm_state)"
  case "$st" in
    running) ;;
    stopped) bounded "$LIMIT" "$PRLCTL" start "$UUID" > /dev/null || stop "prlctl start failed" ;;
    paused|suspended) bounded "$LIMIT" "$PRLCTL" resume "$UUID" > /dev/null || stop "prlctl resume failed" ;;
    *) stop "the VM is \"$st\" after the revert" ;;
  esac
  t0=$(date +%s)
  while :; do
    bounded 30 "$PRLCTL" exec "$UUID" cmd /c ver > /dev/null 2>&1 && break
    [ $(( $(date +%s) - t0 )) -lt "$BOOT" ] || stop "the VM didn't answer cmd /c ver within $BOOT s"
    sleep "$POLL"
  done
  say "the VM answers"
}

# halt: the VM stopped (a clean stop, else killed); bounded
halt() {
  [ "$(vm_state)" = stopped ] && return 0
  bounded 180 "$PRLCTL" stop "$UUID" > /dev/null 2>&1 || bounded 60 "$PRLCTL" stop "$UUID" --kill > /dev/null 2>&1 \
    || { echo "vm-run.sh: the VM couldn't be stopped" >&2; return 1; }
}

# ---- steps ------------------------------------------------------------------------------------

# prelude STEP SECS: the lines every step's script starts with (PowerShell; ' doubled in values)
# (the replacement in an assignment: inside "..." bash 3.2 keeps the backslashes)
q() { local s=${1//\'/\'\'}; printf "'%s'" "$s"; }
prelude() {
  printf '$ProgressPreference = %s\n' "$(q SilentlyContinue)"
  printf 'try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }\n'
  printf '$VmRun = @{ Url = %s; Run = %s; Share = %s; Results = %s; Step = %s; Secs = %d }\n' \
    "$(q "$URL")" "$(q "$RUN")" "$(q '\\Mac\win')" "$(q "\\\\Mac\\win\\results\\$RUN")" "$(q "$1")" "${2:-600}"
}

# wrapper N GUESTSTEP: what prlctl (or the user's scheduled task) runs for step N: the step's script
# as a child powershell.exe, then a last line "vm-run-exit RUN N CODE" and exit CODE, CODE the
# child's exit code mapped into 0..255 (none, or one outside it, is 1: powershell.exe exits
# -196608 for a -File it can't read, which reaches the Mac as 0). The host trusts that line, never
# prlctl's status alone: a missing line is a FAIL
wrapper() {
  printf '$step = %s; $marker = %s\n' "$(q "$2")" "$(q "vm-run-exit $RUN $1")"
  cat <<'EOF'
# vm-run-wrap (tests/win/vm-run.sh)
$ProgressPreference = 'SilentlyContinue'
# the step's output read and passed on as UTF-8 (the log on the Mac is UTF-8)
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }
$c = $null
try {
    & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $step
    $c = $LASTEXITCODE
} catch { "vm-run: the step didn't start: $($_.Exception.Message)" }
if ($null -eq $c -or $c -lt 0 -or $c -gt 255) { $c = 1 }
"$marker $c"
exit $c
EOF
}

# verdict LOG RC N: step N's verdict in V (PASS, SKIP, FAIL, TIMEOUT) and its code in CODE, from
# its wrapper's last line in LOG, which must be there and agree with RC (prlctl's status, or the
# launcher's); a FAIL for either is said at the end of LOG
verdict() {
  local m
  m="$(tr -d '\r' < "$1" | sed -n "s/^vm-run-exit $RUN $3 \([0-9][0-9]*\)\$/\1/p" | tail -1)"
  if [ "$2" = 124 ]; then V=TIMEOUT; CODE=124; return 0; fi
  if [ -z "$m" ]; then
    V=FAIL; CODE=1; echo "vm-run: no \"vm-run-exit $RUN $3\" line (the step's wrapper didn't run to its end; prlctl said $2): FAIL" >> "$1"; return 0
  fi
  if [ "$m" != "$2" ]; then
    V=FAIL; CODE=1; echo "vm-run: the step exited $m, prlctl said $2: FAIL" >> "$1"; return 0
  fi
  CODE="$m"
  case "$m" in 0) V=PASS ;; 77) V=SKIP ;; *) V=FAIL ;; esac
}

# launcher MODE JOB OUT SECS: the SYSTEM script that runs the user's JOB (a script on the share)
# through a scheduled task, waits for it at most SECS, prints its output and exits with its status
launcher() {
  printf '$mode = %s; $job = %s; $out = %s; $secs = %d; $task = %s\n' "$(q "$1")" "$(q "$2")" "$(q "$3")" "$4" "$(q "rbstems-run-$RUN")"
  cat <<'EOF'
$ProgressPreference = 'SilentlyContinue'
# the job's log passed on as UTF-8 (the Mac's log is UTF-8)
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item $out, "$out.done" -ErrorAction SilentlyContinue
$user = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $user) { 'vm-run: nobody is logged in'; exit 3 }
# the run's own task (rbstems-run-<run>), never another run's; one still "running" (a program it
# started lives on) would ignore the new start: ended first
Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
$ps = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass"
if ($mode -eq 'user-signed') {
  # the job's exit code into .done, as hidden.exe writes it: always (finally), 1 when it failed
  # without one or threw
  $cmd = "`$c = 1; try { & '$job' *> '$out'; `$c = `$LASTEXITCODE; if (`$null -eq `$c) { `$c = 1 } } catch { `$c = 1 } finally { Set-Content '$out.done' `$c }"
  $a = New-ScheduledTaskAction -Execute "$env:WINDIR\System32\conhost.exe" -Argument ("--headless $ps -Command `"" + $cmd + "`"")
} else {
  if (-not (Test-Path 'C:\rbtest\hidden.exe')) { 'vm-run: C:\rbtest\hidden.exe is missing'; exit 3 }
  $a = New-ScheduledTaskAction -Execute 'C:\rbtest\hidden.exe' -Argument "$out $ps -File $job"
}
if ($mode -eq 'user-high') { $p = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest }
else { $p = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive }
$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds ($secs + 60))
$code = 1
try {
  Register-ScheduledTask -TaskName $task -Action $a -Principal $p -Settings $s -Force | Out-Null
  Start-ScheduledTask -TaskName $task
  $t = 0
  while (-not (Test-Path "$out.done") -and $t -lt (2 * $secs)) { Start-Sleep -Milliseconds 500; $t++ }
  # hidden.exe's log is the wrapper's UTF-8 output; user-signed's (*>) is UTF-16 with a BOM,
  # which Get-Content finds by itself
  if (Test-Path $out) { if ($mode -eq 'user-signed') { Get-Content $out } else { Get-Content $out -Encoding UTF8 } }
  if (-not (Test-Path "$out.done")) {
    Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
    "vm-run: TIMEOUT waiting for the user job ($secs s)"; $code = 124
  } else {
    # the job's code, 1 when it is unreadable or outside 0..255 (-196608, or a multiple of 256,
    # would reach the Mac as 0)
    $first = [string](Get-Content "$out.done" -TotalCount 1)
    if ($first.Trim() -match '^-?[0-9]{1,10}$') { $code = [long]$first.Trim() }
    if ($code -lt 0 -or $code -gt 255) { $code = 1 }
  }
} finally {
  Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
}
exit ([int]$code)
EOF
}

# step MODE SECS NAME BODYFILE N: the step's scripts on the share (N-NAME.ps1, the step; its
# wrapper N-NAME.wrap.ps1; a user step's launcher), run; output to stdout; status (see verdict)
step() {
  local inc mode="$1" secs="$2" name="$3" body="$4" n="$5" jobs="$SHARE/job/$RUN" g="\\\\Mac\\win\\job\\$RUN"
  mkdir -p "$jobs" || return 2
  { prelude "$name" "$secs"; while IFS= read -r inc; do [ -z "$inc" ] || cat "$inc"; done <<< "${INCLUDES:-}"; cat "$body"; } > "$jobs/$n-$name.ps1" || return 2
  wrapper "$n" "$g\\$n-$name.ps1" > "$jobs/$n-$name.wrap.ps1" || return 2
  if [ "$mode" = sys ]; then
    exec_sys "$g\\$n-$name.wrap.ps1" "$secs"
    return
  fi
  launcher "$mode" "$g\\$n-$name.wrap.ps1" "C:\\rbtest\\job\\$RUN\\$n-$name.txt" "$secs" > "$jobs/$n-$name.launch.ps1" || return 2
  # the launcher's own wait is SECS; prlctl gets a minute more to register, collect and return
  exec_sys "$g\\$n-$name.launch.ps1" "$((secs + 60))"
}

# killjob N: after step N timed out, as SYSTEM: every process whose command line names step N's
# scripts (\job\<run>\N-: its wrapper, the step, hidden.exe, its launcher) ended, and the run's
# scheduled task; 0 when none is left (its log: RESULTS/N-kill.log)
killjob() {
  local f="$SHARE/job/$RUN/kill-$1.ps1"
  # the step's own scripts only (\job\<run>\<N>-...): never this kill script nor its parents
  { printf '$job = %s; $task = %s\n' "$(q "\\job\\$RUN\\$1-")" "$(q "rbstems-run-$RUN")"; cat <<'EOF'
# vm-run-kill (tests/win/vm-run.sh): the timed-out step's processes, never this one
$ProgressPreference = 'SilentlyContinue'
$mine = $PID
function Left { @(Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne $mine -and $_.CommandLine -and $_.CommandLine.Contains($job) }) }
Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
foreach ($p in Left) { "ending $($p.ProcessId) $($p.Name)"; Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2
$left = Left
foreach ($p in $left) { "still running: $($p.ProcessId) $($p.Name)" }
"vm-run-kill: $($left.Count) left"
if ($left.Count) { exit 1 }
exit 0
EOF
  } > "$f" || return 1
  exec_sys "\\\\Mac\\win\\job\\$RUN\\kill-$1.ps1" 60 > "$RESULTS/$1-kill.log" 2>&1 || return 1
  tr -d '\r' < "$RESULTS/$1-kill.log" | grep -qx 'vm-run-kill: 0 left'
}

# parse SUITEFILE: STEPS (one "MODE SECS NAME SCRIPT" a line, SCRIPT absolute), NEED_HTTP, NEEDS
# (the share's files the suite needs, relative, one a line) and INCLUDES (the scripts every step
# starts with, absolute paths, one a line); stops on any line it can't take
parse() {
  local dir line mode secs name script rest i=0
  dir="$(cd "$(dirname "$1")" && pwd)"
  STEPS=""; NEED_HTTP=0; NEED_VM=0; NEEDS=""; INCLUDES=""; CANDIDATES=""; FIXTURES=""
  while IFS= read -r line || [ -n "$line" ]; do
    i=$((i + 1))
    line="${line%%#*}"
    read -r mode secs name script rest <<< "$line"
    [ -n "${mode:-}" ] || continue
    if [ "$mode" = need ]; then
      if [ "$secs" = http ] && [ -z "${name:-}" ]; then NEED_HTTP=1; continue; fi
      # need vm: the steps run the share's vm/ (stage --build's test exes): --record needs them
      # built from HEAD
      if [ "$secs" = vm ] && [ -z "${name:-}" ]; then NEED_VM=1; continue; fi
      # need file REL: a file in the share (staged by the lane: vm-run.sh stage, the TEST build);
      # need candidate REL: a payload.json of a release this commit makes (--record: its commit is
      # HEAD); need fixture REL: one of an older or made-up release (recorded, not bound to HEAD)
      case "$secs" in file|candidate|fixture) ;; *) secs="" ;; esac
      [ -n "$secs" ] && [ -n "${name:-}" ] && [ -z "${script:-}" ] && [[ "$name" =~ ^[A-Za-z0-9._/-]+$ ]] && [[ "$name" != *..* ]] \
        || stop "$1:$i: \"need http\", \"need vm\" or \"need file|candidate|fixture REL\" (REL plain, no ..)"
      NEEDS+="$name"$'\n'
      [ "$secs" != candidate ] || CANDIDATES+="$name"$'\n'
      [ "$secs" != fixture ] || FIXTURES+="$name"$'\n'
      continue
    fi
    if [ "$mode" = include ]; then
      # include SCRIPT: a .ps1 beside the suite, put in front of every step's script
      [ -n "${secs:-}" ] && [ -z "${name:-}" ] && [ -f "$dir/$secs" ] \
        || stop "$1:$i: \"include SCRIPT\" (a file beside the suite)"
      INCLUDES+="$dir/$secs"$'\n'; continue
    fi
    case "$mode" in sys|user|user-high|user-signed) ;; *) stop "$1:$i: no mode \"$mode\"" ;; esac
    [[ "$secs" =~ ^[0-9]+$ ]] && [ "$secs" -ge 1 ] && [ "$secs" -le "$LIMIT" ] || stop "$1:$i: the timeout must be 1 to $LIMIT seconds"
    [[ "${name:-}" =~ ^[A-Za-z0-9._-]+$ ]] || stop "$1:$i: a step's name is [A-Za-z0-9._-]"
    [ -z "${rest:-}" ] || stop "$1:$i: too many fields"
    [ -n "${script:-}" ] && [ -f "$dir/$script" ] || stop "$1:$i: no script $dir/${script:-}"
    grep -q "^[^ ]* [^ ]* $name " <<< "$STEPS" && stop "$1:$i: two steps named $name"
    STEPS+="$mode $secs $name $dir/$script"$'\n'
  done < "$1"
  [ -n "$STEPS" ] || stop "$1 has no step"
}

# ---- the HTTP server --------------------------------------------------------------------------

HTTP_PID=""
SERVED_SHA=""   # the served payload.json's SHA-256 when the server started ("" without one)
# port_taken: something answers on BIND:PORT already (an old run's server, another program's)
port_taken() { curl -s -o /dev/null --max-time 2 "$URL"; }
# served_same DIR: the payload.json the server sends is DIR's, byte for byte (true without one)
served_same() {
  [ -f "$1/payload.json" ] || return 0
  [ "$(curl -fsS --max-time 10 "${URL}payload.json" 2> /dev/null | shasum -a 256 | cut -c1-64)" = "$(sha "$1/payload.json")" ]
}
# serve DIR: our own server on BIND:PORT, ready when its process says it bound the port ("Serving
# HTTP on", unbuffered) and lives; refused when the port answers before it starts; the payload.json
# it sends checked against DIR's
serve() {
  local t=0
  port_taken && stop "something already answers on $URL: stop it first (lsof -nP -iTCP:$PORT)"
  # http.server's handler, but nothing through a link (a path whose real path isn't itself: 404),
  # so nothing outside DIR is ever served
  python3 -I -u -c '
import functools, http.server, os, sys
root = os.path.realpath(sys.argv[3])
class Handler(http.server.SimpleHTTPRequestHandler):
    def send_head(self):
        p = self.translate_path(self.path)
        if os.path.realpath(p) != os.path.normpath(p):
            self.send_error(404, "not served: a link")
            return None
        # a folder answers with its index page when it has one: never one that is a link
        if os.path.isdir(p):
            for i in ("index.html", "index.htm"):
                q = os.path.join(p, i)
                if os.path.lexists(q) and os.path.islink(q):
                    self.send_error(404, "not served: a link")
                    return None
        return super().send_head()
srv = http.server.ThreadingHTTPServer((sys.argv[1], int(sys.argv[2])), functools.partial(Handler, directory=root))
print("Serving HTTP on %s port %s (%s)" % (sys.argv[1], sys.argv[2], root), flush=True)
srv.serve_forever()' "$BIND" "$PORT" "$1" > "$RESULTS/http.log" 2>&1 &
  HTTP_PID=$!
  echo "$HTTP_PID" > "$RESULTS/http.pid"
  until grep -q '^Serving HTTP on' "$RESULTS/http.log" 2> /dev/null && kill -0 "$HTTP_PID" 2> /dev/null; do
    kill -0 "$HTTP_PID" 2> /dev/null || stop "the HTTP server didn't start on $BIND:$PORT (see $RESULTS/http.log)"
    t=$((t + 1)); [ "$t" -lt 60 ] || stop "the HTTP server didn't say it serves $URL within 30 s (see $RESULTS/http.log)"
    sleep 0.5
  done
  # an answer of any status (/ may 404: an index page that is a link)
  curl -sS -o /dev/null --max-time 5 "$URL" || stop "the HTTP server doesn't answer on $URL"
  kill -0 "$HTTP_PID" 2> /dev/null || stop "the HTTP server ended (see $RESULTS/http.log)"
  served_same "$1" || stop "the payload.json served on $URL isn't $1/payload.json"
  [ ! -f "$1/payload.json" ] || SERVED_SHA="$(sha "$1/payload.json")"
  say "serving $1 at $URL"
}
http_alive() { [ -n "$HTTP_PID" ] && kill -0 "$HTTP_PID" 2> /dev/null; }
unserve() { [ -z "$HTTP_PID" ] || { kill "$HTTP_PID" 2> /dev/null; wait "$HTTP_PID" 2> /dev/null; HTTP_PID=""; }; }

# ---- one run at a time ------------------------------------------------------------------------

LOCK="$SHARE/.vm-run.lock"
LOCKED=0
# lock: the share's lock, a folder holding the owner's pid. Taken by renaming a folder that already
# holds our pid onto the lock (rename(2) fails onto a folder that isn't empty), so a lock never
# shows an empty pid while taken; a lock whose pid is gone is moved aside (one taker wins the
# move) and taken; an empty one younger than 10 s is someone taking it the old way: refused.
# lock() and unlock() run one at a time across processes: under a flock on $LOCK.guard (a file
# never renamed or removed), held on fd 9 from guard_on to guard_off (or this process's exit)
guard_on() {
  exec 9>> "$LOCK.guard" || stop "can't open $LOCK.guard"
  perl -e 'use Fcntl ":flock"; open(my $f, ">&=", 9) or exit 2; $SIG{ALRM} = sub { exit 1 }; alarm 30; flock($f, LOCK_EX) or exit 1;' \
    || stop "the lock's guard $LOCK.guard is held for 30 s: another vm-run.sh hangs while it takes the lock"
}
guard_off() { perl -e 'use Fcntl ":flock"; open(my $f, ">&=", 9) or exit 0; flock($f, LOCK_UN);' 9>&9; exec 9>&-; }
# alive PID: whether PID runs; EPERM (another user's, or a sandboxed caller's view of an
# unsandboxed process) counts as running, never as gone (VX6); a pid that isn't a positive number
# isn't one (perl's kill 0 on 0 or text would signal our own group)
alive() { [[ "$1" =~ ^[1-9][0-9]*$ ]] && perl -e 'exit(kill(0, $ARGV[0]) || $!{EPERM} ? 0 : 1)' "$1"; }
lock() {
  local p try mt new="$LOCK.new.$$" old="$LOCK.old.$$"
  mkdir -p "$SHARE" || stop "no share $SHARE"
  guard_on
  rm -rf "$new"; mkdir "$new" && echo $$ > "$new/pid" || stop "can't make $new"
  for try in 1 2 3; do
    p="$(cat "$LOCK/pid" 2> /dev/null)"
    if [ -z "$p" ] && [ -d "$LOCK" ]; then   # (a rename would replace an empty folder)
      mt="$(stat -f %m "$LOCK" 2> /dev/null || echo 0)"
      if [ $(( $(date +%s) - mt )) -lt 10 ]; then rm -rf "$new"; stop "another vm-run.sh is taking $LOCK (no pid yet): try again"; fi
    fi
    if perl -e 'rename($ARGV[0], $ARGV[1]) or exit 1' "$new" "$LOCK" 2> /dev/null; then LOCKED=1; guard_off; return 0; fi
    p="$(cat "$LOCK/pid" 2> /dev/null)"
    if [ -n "$p" ] && alive "$p"; then rm -rf "$new"; stop "another vm-run.sh (pid $p) drives the VM"; fi
    # stale: aside, then ours in; the lock moved aside must be the one judged stale
    mv "$LOCK" "$old" 2> /dev/null || continue
    if [ "$(cat "$old/pid" 2> /dev/null)" != "$p" ]; then
      perl -e 'rename($ARGV[0], $ARGV[1])' "$old" "$LOCK" 2> /dev/null; rm -rf "$new"; stop "$LOCK changed hands while it was taken over: try again"
    fi
    rm -rf "$old"
  done
  rm -rf "$new"; stop "can't take $LOCK"
}
# unlock: only our own lock
unlock() {
  [ "$LOCKED" = 1 ] || return 0
  ( guard_on; [ "$(cat "$LOCK/pid" 2> /dev/null)" = "$$" ] && rm -rf "$LOCK"; guard_off )
  LOCKED=0
}
# cleanup (on every exit): the HTTP server stopped; once a run's revert began, the VM stopped and
# reverted again (bounded) unless --keep or the run did it itself; then the lock released. A
# --record run that stopped before its revert leaves no results folder (its frozen src/ goes)
REVERT_ID=""; KEEP=0; RESTORED=0; FROZEN=""
cleanup() {
  unserve
  if [ -n "$FROZEN" ] && [ -z "$REVERT_ID" ]; then rm -rf "$FROZEN"; rmdir "$RESULTS" 2> /dev/null; FROZEN=""; fi
  if [ -n "$REVERT_ID" ] && [ "$KEEP" = 0 ] && [ "$RESTORED" = 0 ]; then
    RESTORED=1
    echo "vm-run.sh: the run ended early: the VM is stopped and reverted to $REVERT_ID" >&2
    ( halt && bounded "$LIMIT" "$PRLCTL" snapshot-switch "$UUID" -i "$REVERT_ID" > /dev/null ) \
      || echo "vm-run.sh: the VM couldn't be stopped and reverted: do it by hand (prlctl stop/snapshot-switch $UUID)" >&2
  fi
  unlock
}

# ---- the record -------------------------------------------------------------------------------

# A --record run writes build/gates/<commit>/vm-win/<suite>@<snapshot id>-<arch>.ok, or .failed:
# one record per suite, snapshot and architecture, a rerun of the same three replacing only its
# own pair. It names the commit, the suite, the snapshot, the VM's architecture, the served
# payload.json's SHA-256, the staged repo/'s commit and vm/'s files, the SHA-256 of the suite and of
# every script it ran ("file PATH SHA", PATH in the checkout), each assembled step's, and each
# step's result. check COMMIT turns them into build/gates/<commit>/vm-win.ok.

# rec_key SUITE SNAPSHOT-ID ARCH: the record's name, without .ok/.failed
rec_key() { local i="${2//[\{\}]/}"; echo "$1@$i-$3"; }
# pjson FILE KEY: a top-level string of a JSON file ("" when it isn't one)
pjson() { python3 -I -c 'import json, sys; v = json.load(open(sys.argv[1])).get(sys.argv[2], ""); print(v if isinstance(v, str) else "")' "$1" "$2"; }
# tree_shas DIR: "SHA DIR/..." for every file stage put in the share's DIR (vm or repo: what
# DIR/shas.txt holds; DIR/.commit and the list itself aside)
tree_shas() { (cd "$SHARE" 2> /dev/null && find "$1" -type f ! -name 'shas.txt*' ! -path "$1/.commit" 2> /dev/null | LC_ALL=C sort | while IFS= read -r f; do echo "$(sha "$f") $f"; done); }
vm_shas() { tree_shas vm; }
repo_shas() { tree_shas repo; }
# served_list DIR: "served PATH SHA COMMIT" for each candidate and fixture the suite names (COMMIT:
# its payload.json's commit, "-" without one)
served_list() {
  local p c
  while IFS= read -r p; do
    [ -n "$p" ] || continue
    c="$(pjson "$SHARE/$p" commit 2> /dev/null)"
    echo "served $p $(sha "$SHARE/$p" 2> /dev/null) ${c:--}"
  done <<< "$CANDIDATES$FIXTURES"
}
# dist_rules DIR: the served TEST build and the fixture releases made from it are what they must be
# (says why not, exit 1): DIR's bootstrap.ps1, payload.json.sig and zip byte-equal to the
# checkout's dist-test/; each fault-*/ file DIR's, the faulted one DIR's plus one byte "X";
# bad-version/'s bootstrap.ps1 and zip DIR's
dist_rules() {
  local d="$1" r="$root/dist-test" zip f o fault t
  zip="$(python3 -I -c 'import json, sys; z = json.load(open(sys.argv[1])).get("windows", {}).get("app", {}).get("zip", ""); print(z if isinstance(z, str) else "")' "$d/payload.json" 2> /dev/null)" || zip=""
  case "$zip" in */*|..*) echo "the zip's name isn't plain: $zip"; return 1 ;; esac
  for f in bootstrap.ps1 payload.json.sig $zip; do
    [ -f "$d/$f" ] || continue
    cmp -s "$d/$f" "$r/$f" || { echo "the share's dist-test/$f isn't $r/$f"; return 1; }
  done
  for fault in zip json sig; do
    o="$d/fault-$fault"
    [ -d "$o" ] || continue
    case "$fault" in zip) t="$zip" ;; json) t=payload.json ;; sig) t=payload.json.sig ;; esac
    for f in "$o"/*; do
      [ -e "$f" ] || continue
      f="$(basename "$f")"
      case " bootstrap.ps1 payload.json payload.json.sig $zip " in *" $f "*) ;; *) echo "fault-$fault/$f isn't one of dist-test's files"; return 1 ;; esac
      [ -f "$d/$f" ] || { echo "fault-$fault/$f isn't one of dist-test's files (dist-test has none)"; return 1; }
      if [ "$f" = "$t" ]; then
        cmp -s "$o/$f" <(cat "$d/$f"; printf X) || { echo "fault-$fault/$f isn't dist-test's plus one byte"; return 1; }
      else
        cmp -s "$o/$f" "$d/$f" || { echo "fault-$fault/$f isn't dist-test's"; return 1; }
      fi
    done
  done
  # bad-version/ (VT8): DIR's release but for windows.app.version: the same zip name, its
  # bootstrap.ps1 and zip there and DIR's byte for byte, its payload.json DIR's once both are
  # parsed and windows.app.version is taken out of each (stage-dist.sh writes the JSON again), the
  # version itself another; nothing else in the folder but its payload.json.sig
  if [ -d "$d/bad-version" ]; then
    local b="$d/bad-version" bz rc
    [ -n "$zip" ] || { echo "bad-version/ is there but dist-test/payload.json names no windows.app.zip"; return 1; }
    [ -f "$b/payload.json" ] || { echo "bad-version/ has no payload.json"; return 1; }
    bz="$(python3 -I -c 'import json, sys; z = json.load(open(sys.argv[1])).get("windows", {}).get("app", {}).get("zip", ""); print(z if isinstance(z, str) else "")' "$b/payload.json" 2> /dev/null)" || bz=""
    [ "$bz" = "$zip" ] || { echo "bad-version/payload.json names the zip \"$bz\", not dist-test's $zip"; return 1; }
    for f in bootstrap.ps1 "$zip"; do
      [ -f "$b/$f" ] || { echo "bad-version/ has no $f"; return 1; }
      cmp -s "$b/$f" "$d/$f" || { echo "bad-version/$f isn't dist-test's"; return 1; }
    done
    for f in "$b"/* "$b"/.[!.]*; do
      [ -e "$f" ] || continue
      case "$(basename "$f")" in bootstrap.ps1|payload.json|payload.json.sig|"$zip") ;; *) echo "bad-version/$(basename "$f") isn't one of a release's files"; return 1 ;; esac
    done
    python3 -I -c '
import json, sys
def version_out(p):
    w = p.get("windows") if isinstance(p, dict) else None
    a = w.get("app") if isinstance(w, dict) else None
    if not isinstance(a, dict) or "version" not in a:
        sys.exit(3)
    return a.pop("version")
try:
    d, b = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
except Exception:
    sys.exit(4)
vd, vb = version_out(d), version_out(b)
sys.exit(0 if d == b and vd != vb else (5 if d != b else 6))' "$d/payload.json" "$b/payload.json" 2> /dev/null
    rc=$?
    case "$rc" in
      0) ;;
      3) echo "bad-version/payload.json or dist-test's has no windows.app.version"; return 1 ;;
      4) echo "bad-version/payload.json or dist-test's isn't JSON"; return 1 ;;
      5) echo "bad-version/payload.json differs from dist-test's in more than windows.app.version"; return 1 ;;
      6) echo "bad-version/payload.json names dist-test's own windows.app.version"; return 1 ;;
      *) echo "bad-version/payload.json couldn't be compared (exit $rc)"; return 1 ;;
    esac
  fi
  return 0
}
# sources SUITEFILE: "file PATH SHA" for the suite, its includes, its step scripts and this
# vm-run.sh (RUNNER: the file that runs, VX7, or under --record its frozen copy), PATH in SRC_ROOT
# (--record: the run's frozen src/, whose paths are the commit's, VX3/VX4; else the checkout), or
# "outside FILE" for one that isn't in it (--record refuses those: VT2, a record names the runner
# that made it)
SRC_ROOT=""; RUNNER="$self"
sources() {
  local f abs rr; rr="$(cd "${SRC_ROOT:-$root}" && pwd -P)"
  { echo "$1"; printf '%s' "$INCLUDES"; cut -d' ' -f4- <<< "$STEPS"; echo "$RUNNER"; } | awk 'NF && !seen[$0]++' | while IFS= read -r f; do
    abs="$(cd "$(dirname "$f")" && pwd -P)/$(basename "$f")"
    case "$abs" in "$rr"/*) echo "file ${abs#"$rr"/} $(sha "$abs")" ;; *) echo "outside $abs" ;; esac
  done
}
# suite_files COMMIT SUITE: the files a record of SUITE must name, from COMMIT's suite: the suite,
# its includes, each step's script (once) and tests/win/vm-run.sh, one a line, sorted
suite_files() {
  git -C "$root" show "$1:tests/win/suites/$2.suite" 2> /dev/null | python3 -I -c '
import os, sys
base = "tests/win/suites"
out = {base + "/" + sys.argv[1] + ".suite", "tests/win/vm-run.sh"}
for line in sys.stdin:
    f = line.split("#", 1)[0].split()
    if len(f) == 2 and f[0] == "include":
        out.add(os.path.normpath(base + "/" + f[1]))
    elif len(f) == 4 and f[0] in ("sys", "user", "user-high", "user-signed"):
        out.add(os.path.normpath(base + "/" + f[3]))
print("\n".join(sorted(out)))' "$2"
}
# repo_tree_bad COMMIT: says why the share's repo/ isn't COMMIT's archived tree, exit 0; exit 1
# when it is: each file's blob id against git ls-tree, the two file lists compared both ways
REPO_PATHS="scripts tests/win core/texts.def win/c/aumid.h"
repo_tree_bad() {
  local want got paths first base blobs
  # "PATH BLOB" a line, sorted: COMMIT's (ls-tree) and the share's (hash-object of each file)
  # shellcheck disable=SC2086
  want="$(git -C "$root" -c core.quotePath=false ls-tree -r "$1" -- $REPO_PATHS | awk -F '\t' '{ split($1, a, " "); if (a[2] == "blob") print $2, a[3] }' | LC_ALL=C sort)"
  paths="$(cd "$SHARE/repo" 2> /dev/null && find . -type f ! -path ./shas.txt ! -path ./shas.txt.tmp ! -path ./.commit | sed 's|^\./||' | LC_ALL=C sort)"
  got=""
  if [ -n "$paths" ]; then
    # absolute paths: --stdin-paths reads a relative one from the top of an enclosing checkout
    # (the default share is inside the main one), never from repo/ (VX1)
    base="$(cd "$SHARE/repo" && pwd -P)/" || { echo "couldn't hash the share's repo/"; return 0; }
    blobs="$(printf '%s\n' "$paths" | awk -v b="$base" '{ print b $0 }' | git hash-object --no-filters --stdin-paths 2> /dev/null)" \
      && [ "$(grep -c . <<< "$blobs")" = "$(grep -c . <<< "$paths")" ] || { echo "couldn't hash the share's repo/"; return 0; }
    got="$(P="$paths" awk 'BEGIN { split(ENVIRON["P"], p, "\n") } { print p[NR], $0 }' <<< "$blobs" | LC_ALL=C sort)"
  fi
  [ -n "$want" ] || { echo "$1 has none of $REPO_PATHS"; return 0; }
  [ "$want" = "$got" ] && return 1
  # the first path that is in one and not the other (or has another blob there)
  first="$({ sed 's/^/W /' <<< "$want"; [ -z "$got" ] || sed 's/^/G /' <<< "$got"; } | awk '
    $1 == "W" { w[$2] = $3 } $1 == "G" { g[$2] = $3 }
    END { for (k in w) if (!(k in g) || g[k] != w[k]) { print k; exit } for (k in g) if (!(k in w)) { print k; exit } }')"
  echo "the share's repo/ isn't $1's tree (first: ${first:-?})"
  return 0
}

# rec_head: a record's first lines, from cmd_run's values (the ones saved under the lock)
rec_head() {
  echo "commit $commit"
  echo "suite $sname"
  echo "snapshot $id $snapname"
  echo "vm_uuid $UUID"
  echo "arch $arch"
  echo "payload_sha256 $payload"
  echo "stage_commit $saved_stage"
  echo "vm_commit $saved_vmc"
  echo "repo_sha256 $(printf '%s\n' "$saved_repo" | shasum -a 256 | cut -c1-64)"
  echo "run $RUN"
  echo "date $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '%s\n' "$srcs"
  [ -z "$saved_served" ] || printf '%s\n' "$saved_served"
  [ -z "$saved_vm" ] || sed 's/^/vm /' <<< "$saved_vm"
}

# rec_for DIR SUITE ARCH SNAPSHOT EXT: the records DIR/SUITE@*-ARCH.EXT whose snapshot is SNAPSHOT
# (its name, or its {id}), one a line
rec_for() {
  local f s
  for f in "$1/$2@"*"-$3.$5"; do
    [ -f "$f" ] || continue
    s="$(sed -n 's/^snapshot //p' "$f" | head -1)"
    if [ "${s%% *}" = "$4" ] || [ "${s#* }" = "$4" ]; then echo "$f"; fi
  done
}
# rec_bad REC COMMIT PAYLOAD-SHA SUITE ARCH: says why REC isn't a pass of SUITE on ARCH for COMMIT
# and that payload, exit 0; exit 1 (nothing said) when it is one
rec_bad() {
  local r="$1" c="$2" path want got suite p
  grep -qx "commit $c" "$r" || { echo "it isn't $c's"; return 0; }
  grep -qx "suite $4" "$r" || { echo "it isn't suite $4's"; return 0; }
  grep -qx "arch $5" "$r" || { echo "it isn't $5's"; return 0; }
  # made on rb-win-template itself, never on a VMRUN_UUID's VM (VX8)
  grep -qx "vm_uuid $DEFAULT_UUID" "$r" || { echo "it wasn't made on $NAME ($DEFAULT_UUID)"; return 0; }
  grep -qx "payload_sha256 $3" "$r" || { echo "it was made for another payload.json"; return 0; }
  grep -qx "stage_commit $c" "$r" || { echo "its share's repo/ wasn't staged from $c"; return 0; }
  # exactly one result line, and it is a pass (VT2)
  [ "$(grep -c '^result ' "$r")" = 1 ] && grep -qx 'result pass' "$r" || { echo "it isn't one pass ($(grep '^result ' "$r" | tr '\n' ' '))"; return 0; }
  grep -q '^step ' "$r" || { echo "it has no step"; return 0; }
  ! grep '^step ' "$r" | grep -qv '^step [^ ]* [^ ]* PASS 0 ' || { echo "a step in it didn't pass"; return 0; }
  ! grep -q '^outside ' "$r" || { echo "it ran a file from outside the checkout"; return 0; }
  # its files are exactly what COMMIT's suite runs (and vm-run.sh), each named once (VT2)
  want="$(suite_files "$c" "$4")"
  [ -n "$want" ] || { echo "$c has no tests/win/suites/$4.suite"; return 0; }
  got="$(sed -n 's/^file \([^ ]*\) .*/\1/p' "$r" | LC_ALL=C sort)"
  [ "$got" = "$want" ] || { echo "its files aren't exactly $c's suite's, each once (it names: $(tr '\n' ' ' <<< "$got"))"; return 0; }
  while read -r path want; do
    git -C "$root" cat-file -e "$c:$path" 2> /dev/null || { echo "$c has no $path"; return 0; }
    got="$(git -C "$root" show "$c:$path" | shasum -a 256 | cut -c1-64)"
    [ "$got" = "$want" ] || { echo "the $path it ran isn't $c's"; return 0; }
  done < <(sed -n 's/^file //p' "$r")
  # what COMMIT's suite says: its steps, in order; its candidates bound to COMMIT; its vm/
  suite="$(git -C "$root" show "$c:tests/win/suites/$4.suite" | sed 's/#.*//')"
  [ "$(awk '$1 ~ /^(sys|user|user-high|user-signed)$/ { print $1, $3 }' <<< "$suite")" = "$(awk '$1 == "step" { print $2, $3 }' "$r")" ] \
    || { echo "its steps aren't the suite's, in its order"; return 0; }
  while read -r p; do
    [ -n "$p" ] || continue
    grep -qE "^served $p [0-9a-f]{64} $c\$" "$r" || { echo "its $p isn't $c's release"; return 0; }
  done < <(awk '$1 == "need" && $2 == "candidate" { print $3 }' <<< "$suite")
  if awk '$1 == "need" && $2 == "vm" { f = 1 } END { exit !f }' <<< "$suite"; then
    grep -qx "vm_commit $c" "$r" || { echo "its vm/ wasn't built from $c"; return 0; }
  fi
  return 1
}

# check COMMIT DIR: 0 when every row (SUITE ARCH SNAPSHOT) of tests/win/suites/release.list as
# committed in COMMIT has one passing record for COMMIT, DIR/payload.json and COMMIT's scripts, and
# no failed one; then build/gates/COMMIT/vm-win.ok lists them. Else 1, said, and no vm-win.ok
check() {
  local c="$1" dir="$2" d="$GATES/$1/vm-win" list psha suite arch snap recs fails why n=0 bad=0 rows=""
  [[ "$c" =~ ^[0-9a-f]{40}$ ]] || { echo "vm-run.sh check: \"$c\" isn't a full commit id"; return 1; }
  # under the share's lock (VT1): no --record run writes a record while they are read (a run that
  # holds it: lock stops, exit 2, saying so)
  lock
  trap cleanup EXIT
  rm -f "$GATES/$c/vm-win.ok"
  [ -f "$dir/payload.json" ] || { echo "vm-run.sh check: no $dir/payload.json"; return 1; }
  psha="$(sha "$dir/payload.json")"
  list="$(git -C "$root" show "$c:tests/win/suites/release.list" 2> /dev/null)" || { echo "vm-run.sh check: $c has no tests/win/suites/release.list"; return 1; }
  while read -r suite arch snap; do
    case "$suite" in ''|'#'*) continue ;; esac
    n=$((n + 1))
    fails="$(rec_for "$d" "$suite" "$arch" "$snap" failed)"
    if [ -n "$fails" ]; then echo "vm-run.sh check: $suite on \"$snap\" ($arch) failed: $(basename "$(head -1 <<< "$fails")")"; bad=1; continue; fi
    recs="$(rec_for "$d" "$suite" "$arch" "$snap" ok)"
    if [ -z "$recs" ]; then echo "vm-run.sh check: no record of $suite on \"$snap\" ($arch) in $d"; bad=1; continue; fi
    if [ "$(wc -l <<< "$recs")" -gt 1 ]; then echo "vm-run.sh check: two records of $suite on \"$snap\" ($arch): $(tr '\n' ' ' <<< "$recs")"; bad=1; continue; fi
    if why="$(rec_bad "$recs" "$c" "$psha" "$suite" "$arch")"; then echo "vm-run.sh check: $(basename "$recs"): $why"; bad=1; continue; fi
    rows+="row $suite $arch $snap: $(basename "$recs")"$'\n'
  done <<< "$list"
  [ "$n" -gt 0 ] || { echo "vm-run.sh check: $c's release.list has no row"; return 1; }
  [ "$bad" = 0 ] || return 1
  { echo "commit $c"; echo "payload_sha256 $psha"; printf '%s' "$rows"; echo "result pass"; } > "$GATES/$c/vm-win.ok.tmp" 2> /dev/null \
    && mv "$GATES/$c/vm-win.ok.tmp" "$GATES/$c/vm-win.ok" 2> /dev/null \
    || { echo "vm-run.sh check: can't write $GATES/$c/vm-win.ok"; return 1; }
  echo "vm-run.sh check: $c passed ($n rows of release.list): $GATES/$c/vm-win.ok"
}

# ---- commands ---------------------------------------------------------------------------------

cmd_one() {   # sys|user|user-high|user-signed [SECS] < script
  local mode="$1" secs="${2:-$LIMIT}" body
  [[ "$secs" =~ ^[0-9]+$ ]] && [ "$secs" -ge 1 ] && [ "$secs" -le "$LIMIT" ] || stop "the timeout must be 1 to $LIMIT seconds"
  vm_guard
  # one at a time with the runs (a run's user step and this would share the VM's session)
  lock
  trap cleanup EXIT
  RUN="one-$(date +%Y%m%d-%H%M%S)-$$"; URL="http://$BIND:$PORT/"
  body="$(mktemp "${TMPDIR:-/tmp}/vm-run.XXXXXX")" || stop "no temp file"
  cat > "$body"
  step "$mode" "$secs" "$mode" "$body" 1 > "$body.out" 2>&1
  verdict "$body.out" "$?" 1
  cat "$body.out"
  rm -f "$body" "$body.out" "$SHARE/job/$RUN/1-$mode.ps1" "$SHARE/job/$RUN/1-$mode.wrap.ps1" "$SHARE/job/$RUN/1-$mode.launch.ps1"; rmdir "$SHARE/job/$RUN" 2> /dev/null
  return "$CODE"
}

cmd_run() {
  local snap="" suite="" served="$SHARE/dist-test" record=0 keep=0
  [ $# -ge 2 ] || stop "usage: vm-run.sh run SNAPSHOT SUITE [--serve DIR] [--record] [--keep]"
  snap="$1"; suite="$2"; shift 2
  while [ $# -gt 0 ]; do
    case "$1" in
      --serve) [ $# -ge 2 ] || stop "--serve needs a folder"; served="$2"; shift 2 ;;
      --record) record=1; shift ;;
      --keep) keep=1; shift ;;
      *) stop "unknown option $1" ;;
    esac
  done
  [ -f "$suite" ] || suite="$SUITES/$suite.suite"
  [ -f "$suite" ] || stop "no suite $suite"
  # the run's id goes into paths the VM's command lines carry unquoted: [A-Za-z0-9._-] only
  local sn; sn="$(basename "$suite" .suite)"; sn="${sn//[^A-Za-z0-9._-]/_}"
  RUN="$(date +%Y%m%d-%H%M%S)-$sn-$$"
  RESULTS="$SHARE/results/$RUN"
  local commit="" rr rel
  if [ "$record" = 1 ]; then
    rr="$(cd "$root" && pwd -P)"
    # the record names tests/win/vm-run.sh: only that file may write it, never a copy (VX7)
    [ "$self" = "$rr/tests/win/vm-run.sh" ] || stop "--record: this vm-run.sh ($self) isn't the checkout's $root/tests/win/vm-run.sh"
    # a record is rb-win-template's, in build/gates: another prlctl or VM only with a gates folder
    # of its own (VX8)
    [ -z "${VMRUN_PRLCTL:-}${VMRUN_UUID:-}" ] || [ -n "${VMRUN_GATES:-}" ] \
      || stop "--record: VMRUN_PRLCTL or VMRUN_UUID is set (a test variable) without VMRUN_GATES: unset it, or give the records a folder of their own"
    # the commit, then its suites and this runner frozen, before the suite is read: the run parses,
    # hashes and runs those bytes, whatever the checkout does meanwhile (VX3, VX4)
    [ -z "$(git -C "$root" status --porcelain)" ] || stop "--record needs a clean checkout ($root)"
    commit="$(git -C "$root" rev-parse HEAD)" && [[ "$commit" =~ ^[0-9a-f]{40}$ ]] || stop "no commit in $root"
    rel="$(cd "$(dirname "$suite")" && pwd -P)/$(basename "$suite")"
    case "$rel" in "$rr"/tests/win/suites/*) rel="${rel#"$rr"/}" ;; *) stop "--record: the suite must be in the checkout's tests/win/suites ($root): $rel" ;; esac
    git -C "$root" cat-file -e "$commit:$rel" 2> /dev/null || stop "--record: $rel isn't in HEAD $commit"
    [ -d "$SHARE" ] || stop "no share $SHARE"
    trap cleanup EXIT
    FROZEN="$RESULTS/src"; mkdir -p "$FROZEN" || stop "can't make $FROZEN"
    git -C "$root" archive "$commit" tests/win/suites tests/win/vm-run.sh | tar -x -C "$RESULTS/src" || stop "--record: git archive of $commit's tests/win/suites failed"
    cmp -s "$self" "$RESULTS/src/tests/win/vm-run.sh" || stop "--record: this vm-run.sh isn't HEAD $commit's"
    SRC_ROOT="$RESULTS/src"; RUNNER="$SRC_ROOT/tests/win/vm-run.sh"; suite="$SRC_ROOT/$rel"
  fi
  parse "$suite"
  local share_abs; share_abs="$(cd "$SHARE" 2> /dev/null && pwd -P)" || stop "no share $SHARE"
  if [ -d "$served" ]; then
    served="$(cd "$served" && pwd -P)"
    case "$served/" in "$share_abs"/*) ;; *) stop "--serve must be inside the share ($SHARE): $served" ;; esac
  else
    [ "$NEED_HTTP" = 0 ] || stop "$(basename "$suite") needs the served folder $served"
    [ "$record" = 0 ] || stop "--record needs the served folder $served"
    served=""
  fi
  # one run at a time, from before --record's checks: no stage can change the share under them
  lock
  trap cleanup EXIT
  trap 'echo "vm-run.sh: interrupted" >&2; exit 130' INT TERM HUP
  local payload="" pc srcs="" why p saved_stage="" saved_vmc="" saved_vm="" saved_repo="" saved_served=""
  if [ "$record" = 1 ]; then
    # still the frozen commit, and clean, under the lock
    [ -z "$(git -C "$root" status --porcelain)" ] || stop "--record needs a clean checkout ($root)"
    [ "$(git -C "$root" rev-parse HEAD)" = "$commit" ] || stop "--record: HEAD moved from $commit before the run took the lock"
    [ -f "$served/payload.json" ] || stop "--record needs $served/payload.json"
    # the TEST build is this commit's (as tests/mac/g3.sh asks): never a stale or -dirty one
    pc="$(pjson "$served/payload.json" commit)" || stop "--record: $served/payload.json isn't JSON"
    [[ "$pc" =~ ^[0-9a-f]{40}$ ]] || stop "--record: the served payload.json's commit is \"$pc\": the TEST build of a committed tree (no -dirty) first"
    [ "$pc" = "$commit" ] || stop "--record: the served payload.json was built from $pc, not HEAD $commit"
    cmp -s "$served/payload.json" "$root/dist-test/payload.json" || stop "--record: the served payload.json isn't $root/dist-test/payload.json (copy this checkout's TEST build into the share)"
    [ "$(cat "$SHARE/repo/.commit" 2> /dev/null)" = "$commit" ] || stop "--record: the share's repo/ wasn't staged from HEAD $commit (vm-run.sh stage)"
    [ -f "$SHARE/repo/shas.txt" ] && [ "$(repo_shas)" = "$(cat "$SHARE/repo/shas.txt")" ] || stop "--record: the share's repo/ isn't what vm-run.sh stage wrote (stage again)"
    # and it is HEAD's tree, file for file both ways (VT3: repo/.commit alone could name another)
    why="$(repo_tree_bad "$commit")" && stop "--record: $why (vm-run.sh stage)"
    [ -f "$SHARE/vm/shas.txt" ] && [ "$(vm_shas)" = "$(cat "$SHARE/vm/shas.txt")" ] || stop "--record: the share's vm/ isn't what vm-run.sh stage wrote (stage again)"
    # a suite that runs vm/'s test exes needs them built from HEAD (stage --build at a clean HEAD)
    [ "$NEED_VM" = 0 ] || [ "$(cat "$SHARE/vm/.commit" 2> /dev/null)" = "$commit" ] \
      || stop "--record: $(basename "$suite") runs vm/'s test exes and they weren't built from HEAD $commit (vm-run.sh stage --build)"
    # its candidates are this commit's releases; its fixtures are recorded as they are
    while IFS= read -r p; do
      [ -n "$p" ] || continue
      [ -f "$SHARE/$p" ] || stop "--record: $(basename "$suite") needs $SHARE/$p"
      pc="$(pjson "$SHARE/$p" commit 2> /dev/null)"
      [ "$pc" = "$commit" ] || stop "--record: $p (a candidate) was built from \"$pc\", not HEAD $commit"
    done <<< "$CANDIDATES"
    why="$(dist_rules "$served")" || stop "--record: $why"
    srcs="$(sources "$suite")"
    ! grep -q '^outside ' <<< "$srcs" || stop "--record: the suite's scripts must be in the checkout's tests/win/suites ($root): $(sed -n 's/^outside //p' <<< "$srcs" | head -1)"
    payload="$(sha "$served/payload.json")"
    # what the record will name, as it is now (under the lock): checked again before it is written
    saved_stage="$(cat "$SHARE/repo/.commit")"
    saved_vmc="$(cat "$SHARE/vm/.commit" 2> /dev/null)"; saved_vmc="${saved_vmc:--}"
    saved_vm="$(vm_shas)"; saved_repo="$(repo_shas)"; saved_served="$(served_list)"
  fi
  local nf
  while IFS= read -r nf; do
    [ -z "$nf" ] || [ -e "$SHARE/$nf" ] || stop "$(basename "$suite") needs $SHARE/$nf (staged by the lane)"
  done <<< "$NEEDS"
  if grep -q '^user' <<< "$STEPS"; then
    ! grep -qE '^user(-high)? ' <<< "$STEPS" || [ -f "$SHARE/probe/hidden.exe" ] || stop "user steps need $SHARE/probe/hidden.exe (build/win/probe/hidden.c)"
  fi
  URL="http://$BIND:$PORT/"
  if [ -n "$served" ] && port_taken; then stop "something already answers on $URL: stop it first (lsof -nP -iTCP:$PORT)"; fi
  vm_guard
  local id snapname sid; sid="$(snap_id "$snap")" || exit 2
  id="${sid%% *}"; snapname="${sid#* }"
  KEEP="$keep"
  mkdir -p "$RESULTS" || stop "can't make $RESULTS"
  say "run $RUN: $(basename "$suite") from \"$snapname\" $id"
  REVERT_ID="$id"   # from here on, any exit stops and reverts the VM (cleanup)
  revert "$id"
  # hidden.exe isn't in the snapshots
  local prep="$SHARE/job/$RUN/0-prepare.ps1"
  mkdir -p "$SHARE/job/$RUN"
  { prelude prepare 120; cat <<'EOF'
New-Item -ItemType Directory -Force 'C:\rbtest', "C:\rbtest\job\$($VmRun.Run)" | Out-Null
if (Test-Path '\\Mac\win\probe\hidden.exe') { Copy-Item '\\Mac\win\probe\hidden.exe' 'C:\rbtest\hidden.exe' -Force }
$arch = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE
"prepared: hidden.exe $(Test-Path 'C:\rbtest\hidden.exe') arch $arch"
EOF
  } > "$prep"
  exec_sys "\\\\Mac\\win\\job\\$RUN\\0-prepare.ps1" 120 > "$RESULTS/0-prepare.log" 2>&1 || stop "the VM couldn't be prepared (see $RESULTS/0-prepare.log)"
  # its own last line, not prlctl's status alone (a script powershell.exe can't read exits 0 here)
  tr -d '\r' < "$RESULTS/0-prepare.log" | grep -q '^prepared: ' || stop "the VM's prepare step said nothing (see $RESULTS/0-prepare.log)"
  # the VM's own architecture (the record's): ARM64 or AMD64
  local arch
  case "$(tr -d '\r' < "$RESULTS/0-prepare.log" | sed -n 's/^prepared: .* arch \([A-Za-z0-9_]*\)$/\1/p' | tail -1)" in
    ARM64) arch=arm64 ;; AMD64) arch=x64 ;; *) arch="" ;;
  esac
  [ -n "$arch" ] || [ "$record" = 0 ] || stop "--record: the VM's architecture is unknown (see $RESULTS/0-prepare.log)"
  say "the VM is ${arch:-of an unknown architecture}"
  # from here a --record run always leaves its record: first an incomplete one (a failure, for a
  # run that is interrupted or stops), replaced at the end; vm-win.ok is check's, made again
  local d="" key="" sname r=""
  if [ "$record" = 1 ]; then
    sname="$(basename "$suite" .suite)"; d="$GATES/$commit/vm-win"; key="$(rec_key "$sname" "$id" "$arch")"
    mkdir -p "$d" || stop "can't make $d"
    rm -f "$d/$key.ok" "$d/$key.failed" "$GATES/$commit/vm-win.ok"
    { rec_head; echo "result incomplete"; } > "$d/$key.failed.tmp" && mv "$d/$key.failed.tmp" "$d/$key.failed" || stop "can't write $d/$key.failed"
  fi
  [ -z "$served" ] || serve "$served"
  local n=0 failed=0 skipped=0 mode secs name script rc t0 secs_taken verdict
  : > "$RESULTS/results.txt"
  while read -r mode secs name script; do
    [ -n "$mode" ] || continue
    n=$((n + 1))
    say "step $n: $name ($mode, at most $secs s)"
    t0=$(date +%s)
    step "$mode" "$secs" "$name" "$script" "$n" > "$RESULTS/$n-$name.log" 2>&1 < /dev/null
    rc=$?
    secs_taken=$(( $(date +%s) - t0 ))
    verdict "$RESULTS/$n-$name.log" "$rc" "$n"
    # 77: the step says it can't run here (SKIP, its reason in its log; never a pass for a record)
    case "$V" in SKIP) skipped=$((skipped + 1)) ;; PASS) ;; *) failed=1 ;; esac
    echo "step $mode $name $V $CODE ${secs_taken}s" >> "$RESULTS/results.txt"
    say "  $V ($CODE, ${secs_taken}s)"
    # a timed-out step lives on in the VM (only our prlctl was stopped): ended before the next
    if [ "$V" = TIMEOUT ] && ! killjob "$n" < /dev/null; then
      echo "note step $n's processes couldn't be ended in the VM: the run stopped" >> "$RESULTS/results.txt"
      say "step $n timed out and its processes couldn't be ended in the VM (see $RESULTS/$n-kill.log): the run stops"
      failed=2; break
    fi
    if [ -n "$served" ] && ! http_alive; then
      echo "note the HTTP server ended during step $n ($name): the run stopped" >> "$RESULTS/results.txt"
      say "the HTTP server ended during step $n: the run stops (see $RESULTS/http.log)"; failed=1; break
    fi
  done <<< "$STEPS"
  # what was served at the end is what was checked at the start, from our own server
  if [ -n "$served" ] && [ -n "$SERVED_SHA" ]; then
    if ! http_alive || [ "$(sha "$served/payload.json" 2> /dev/null)" != "$SERVED_SHA" ] || ! served_same "$served"; then
      echo "note the served payload.json changed during the run (or its server ended)" >> "$RESULTS/results.txt"
      say "the served payload.json changed during the run: FAIL"; failed=1
    fi
  fi
  unserve
  if [ "$keep" = 0 ]; then
    say "stop and revert"
    halt || failed=2
    bounded "$LIMIT" "$PRLCTL" snapshot-switch "$UUID" -i "$id" > /dev/null || { echo "vm-run.sh: the revert after the run failed" >&2; failed=2; }
    RESTORED=1
  else
    RESTORED=1
    say "--keep: the VM is left running"
  fi
  if [ "$record" = 1 ]; then
    # what the record names is still what ran: the checkout, its HEAD, the payload, the scripts,
    # the staged repo/ and vm/ (their files, not their own lists), the candidates and fixtures
    local again="" f
    [ -z "$(git -C "$root" status --porcelain)" ] || again+=" the checkout changed;"
    [ "$(git -C "$root" rev-parse HEAD)" = "$commit" ] || again+=" HEAD moved;"
    [ "$(sha "$served/payload.json" 2> /dev/null)" = "$payload" ] || again+=" the served payload.json changed;"
    [ "$(sources "$suite")" = "$srcs" ] || again+=" the suite's scripts changed;"
    [ "$(cat "$SHARE/repo/.commit" 2> /dev/null)" = "$saved_stage" ] && [ "$(repo_shas)" = "$saved_repo" ] || again+=" the share's repo/ changed;"
    f="$(cat "$SHARE/vm/.commit" 2> /dev/null)"
    [ "${f:--}" = "$saved_vmc" ] && [ "$(vm_shas)" = "$saved_vm" ] || again+=" the share's vm/ changed;"
    [ "$(served_list)" = "$saved_served" ] || again+=" a candidate or fixture release changed;"
    why="$(dist_rules "$served")" || again+=" $why;"
    if [ -n "$again" ]; then
      echo "note during the run:$again no pass" >> "$RESULTS/results.txt"
      say "during the run:$again the record is a failure"; [ "$failed" != 0 ] || failed=1
    fi
    # this suite's own pair only (other suites' records stay); the incomplete one replaced
    r="$d/$key.failed"
    [ "$failed" = 0 ] && [ "$skipped" = 0 ] && r="$d/$key.ok"   # a skipped step is no pass
    {
      rec_head
      for f in "$SHARE/job/$RUN"/[0-9]*-*.ps1; do
        case "$f" in *.wrap.ps1|*.launch.ps1|*/0-prepare.ps1) continue ;; esac
        [ -f "$f" ] && echo "assembled $(basename "$f") $(sha "$f")"
      done
      cat "$RESULTS/results.txt"
      if [ "$failed" != 0 ]; then echo "result fail"; elif [ "$skipped" != 0 ]; then echo "result skip"; else echo "result pass"; fi
    } > "$r.tmp" || stop "can't write $r"
    rm -f "$d/$key.failed" "$d/$key.ok"
    mv "$r.tmp" "$r" || stop "can't write $r"
    say "record: $r (vm-run.sh check $commit: all of tests/win/suites/release.list)"
  fi
  say "results: $RESULTS$([ "$skipped" = 0 ] || echo " ($skipped skipped)")"
  [ "$failed" = 0 ] && return 0
  [ "$failed" = 2 ] && return 2
  return 1
}

# stage: what the suites read from the share, from this checkout (committed files only: HEAD's),
# replaced whole: repo/ (scripts, tests/win, core/texts.def, win/c/aumid.h, by git archive) and
# vm/<arch>/ (the test exes this checkout has built, each when it is there: win/c/tests/run.sh's
# winc-wintest, winc-app-wintest and rbsp-watch-test, common/tests/win.sh's plat-win-test and
# checked.dll, location-win-test; the core DLL's test-hooks build). With --build it builds them
# first, both architectures (the x64 ones run under Prism on an ARM64 VM). When the share holds a
# signed TEST build in dist-test/, its fixture releases too (stage-dist.sh: the faults,
# bad-version/). Prints what it staged and what is still missing (the steps that need it SKIP).
VM_FILES="build/winc/win-ARCH/winc-wintest.exe build/winc/win-ARCH/winc-app-wintest.exe build/winc/watch-ARCH/rbsp-watch-test-ARCH.exe
  build/common-win/ARCH/plat-win-test.exe build/common-win/ARCH/checked.dll build/common-win/ARCH/location-win-test.exe
  build/core-dll/testhooks/ARCH/rbsp-core.dll"
# build_inputs: the builders of what stage copies, in this checkout, each into its build/ folder
# and its log in build/vm-inputs/; stops on the first that fails (nothing in the share touched)
build_inputs() {
  local logs="$root/build/vm-inputs" b name
  mkdir -p "$logs" || stop "can't make $logs"
  for b in "common/tests/win.sh build/common-win" "win/c/tests/run.sh build/winc" "core/build.sh dll build/core-dll --test-hooks"; do
    name="$(basename "${b%% *}" .sh)-$(basename "$(dirname "${b%% *}")")"
    say "build: $b"
    # shellcheck disable=SC2086
    (cd "$root" && $b) > "$logs/$name.log" 2>&1 || stop "$b failed (see $logs/$name.log): nothing staged"
  done
}
cmd_stage() {
  local build=0 built=""
  case "${1:-}" in --build) build=1 ;; "") ;; *) stop "usage: vm-run.sh stage [--build]" ;; esac
  if [ "$build" = 1 ]; then
    # vm/.commit (what --record asks of a suite that runs vm/) only for exes built here, now, from
    # a clean HEAD that didn't move while they were built
    built="$(git -C "$root" rev-parse HEAD)"
    [ -z "$(git -C "$root" status --porcelain)" ] || built=""
    build_inputs
    [ "$(git -C "$root" rev-parse HEAD)" = "$built" ] && [ -z "$(git -C "$root" status --porcelain)" ] || built=""
    [ -n "$built" ] || echo "the checkout isn't clean (or HEAD moved): vm/ gets no .commit, so --record refuses the suites that run it"
  fi
  [ -d "$SHARE" ] || mkdir -p "$SHARE" || stop "no share $SHARE"
  # never while a run reads the share
  lock
  trap cleanup EXIT
  # HEAD read once (VT3): the commit archived is the commit repo/.commit names, whatever HEAD does
  # meanwhile; vm/.commit only when it is also the commit the exes were built from
  local c; c="$(git -C "$root" rev-parse HEAD)" && [[ "$c" =~ ^[0-9a-f]{40}$ ]] || stop "no commit in $root"
  if [ "$build" = 1 ] && [ -n "$built" ] && [ "$built" != "$c" ]; then
    echo "HEAD is ${c:0:9} now, not ${built:0:9} the exes were built from: vm/ gets no .commit"; built=""
  fi
  rm -rf "$SHARE/repo" "$SHARE/vm" || stop "can't clear the share's repo/ and vm/ (files the VM wrote are root's: delete them from the VM)"
  mkdir -p "$SHARE/repo" || stop "can't make $SHARE/repo"
  # shellcheck disable=SC2086
  git -C "$root" archive "$c" $REPO_PATHS | tar -x -C "$SHARE/repo" || stop "git archive of $c failed"
  echo "$c" > "$SHARE/repo/.commit" || stop "can't write $SHARE/repo/.commit"
  repo_shas > "$SHARE/repo/shas.txt.tmp" && mv "$SHARE/repo/shas.txt.tmp" "$SHARE/repo/shas.txt" || stop "can't write $SHARE/repo/shas.txt"
  echo "staged repo/ from ${c:0:9}"
  local a f n=0 missing=""
  for a in x64 arm64; do
    mkdir -p "$SHARE/vm/$a"
    for f in $VM_FILES; do
      f="${f//ARCH/$a}"
      if [ -f "$root/$f" ]; then cp "$root/$f" "$SHARE/vm/$a/" && n=$((n + 1)) && echo "staged vm/$a/$(basename "$f")"
      else missing+="vm/$a/$(basename "$f") ($f)"$'\n'; fi
    done
  done
  if [ -f "$root/build/winc/fixtures.txt" ]; then cp "$root/build/winc/fixtures.txt" "$SHARE/vm/" && echo "staged vm/fixtures.txt"
  else missing+="vm/fixtures.txt (build/winc/fixtures.txt: win/c/tests/run.sh)"$'\n'; fi
  # what --record names, and checks is still there
  vm_shas > "$SHARE/vm/shas.txt.tmp" && mv "$SHARE/vm/shas.txt.tmp" "$SHARE/vm/shas.txt" || stop "can't write $SHARE/vm/shas.txt"
  if [ -n "$built" ]; then echo "$built" > "$SHARE/vm/.commit" && echo "vm/ built from $built (vm/.commit)"; fi
  # the TEST build's fixture releases (their key: this checkout's build/test-keys, or the main
  # checkout's; never a release key, stage-dist.sh refuses one)
  if [ -f "$SHARE/dist-test/payload.json.sig" ]; then
    local keys=(--key "$root/build/test-keys")
    [ "$root" != "$here" ] || keys+=(--key "$(cd "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/test-keys")
    "$here/tests/win/suites/stage-dist.sh" "$SHARE" "${keys[@]}" || stop "stage-dist.sh couldn't make the TEST build's fixture releases"
  else
    missing+="dist-test/ fault-*/ and bad-version/ (no signed TEST build in $SHARE/dist-test yet)"$'\n'
  fi
  [ "$n" -gt 0 ] || echo "no test exe built here (vm-run.sh stage --build builds them)"
  if [ -n "$missing" ]; then
    echo "still missing (the steps that need it SKIP):"
    while IFS= read -r f; do [ -z "$f" ] || echo "  $f"; done <<< "$missing"
  fi
}

case "${1:-}" in
  run) shift; cmd_run "$@" ;;
  stage) shift; cmd_stage "$@" ;;
  check) [ $# -ge 2 ] || stop "usage: vm-run.sh check COMMIT [DIR]"; check "$2" "${3:-$root/dist-test}" ;;
  sys|user|user-high|user-signed) cmd_one "$@" ;;
  selftest) exec "$here/tests/win/vm-run-test.sh" ;;
  *) awk 'NR > 1 && /^#/ { print; next } NR > 1 { exit }' "$0"; exit 2 ;;
esac
