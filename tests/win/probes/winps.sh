#!/bin/bash
# Runs a PowerShell script (from stdin) in the Parallels VM rb-win-template, without a window on
# the owner's screen. Phase 0 probe tooling (tests/win/vm-run.sh, release T16, replaces it).
#   winps.sh               as SYSTEM (prlctl exec; no window)
#   winps.sh --user        as the logged-in user, through a scheduled task and C:\rbtest\hidden.exe
#                          (prlctl exec --current-user would open a terminal on the owner's screen)
#   winps.sh --user-signed the same through Windows' own conhost.exe --headless, which still runs
#                          when Smart App Control blocks our unsigned hidden.exe
# winps.sh exits with the script's status: a SYSTEM script's own, a user job's exit code, or 124
# when the user job timed out.
# All pass the script through the shared folder (build/win/job, \\Mac\win\job in the VM):
# prlctl limits a command's length. Must run outside the Mac sandbox (prlctl).
VM=202a80c8-4bc4-4e95-890e-a31d6bfd5b70
# the VM shares only the main checkout's build/win (\\Mac\win), also from a worktree
SHARE="$(cd "$(git -C "$(dirname "$0")" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)/build/win"
mkdir -p "$SHARE/job"
run_file() { # runs a script from the share as SYSTEM and returns prlctl's (the script's) status;
  # prlctl sometimes fails to start the job ("PrlJob_GetResult/GetRetCode: Invalid argument")
  # with nothing run: retry then
  local out try rc=0
  for try in 1 2 3; do
    out=$(prlctl exec "$VM" powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "\\\\Mac\\win\\job\\$1" 2>&1 \
          | grep -v 'ps: Operation not permitted'; exit "${PIPESTATUS[0]}")
    rc=$?
    case "$out" in PrlJob_Get*'Invalid argument'*) [ $try -lt 3 ] && continue ;; esac
    break
  done
  printf '%s\n' "$out"
  return $rc
}
case "${1:-}" in
  --user) mode=hidden ;;
  --user-signed) mode=signed ;;
  *)
    (printf '$ProgressPreference="SilentlyContinue"\n'; cat) > "$SHARE/job/sys.ps1"
    run_file sys.ps1
    exit ;;
esac
(printf '$ProgressPreference="SilentlyContinue"\n'; cat) > "$SHARE/job/job.ps1"
{ printf '$mode = "%s"\n' "$mode"; cat <<'EOF'
$d = "C:\rbtest\job"
New-Item -ItemType Directory -Force $d | Out-Null
Remove-Item "$d\out.txt", "$d\out.txt.done" -ErrorAction SilentlyContinue
$user = (Get-CimInstance Win32_ComputerSystem).UserName
# a task still "running" (e.g. its launcher kept alive by a program it started) would ignore
# the new start: end it first
Stop-ScheduledTask -TaskName "rbstems-run" -ErrorAction SilentlyContinue
if ($mode -eq 'signed') {
  # the job's exit code goes into .done, as hidden.exe writes it (1 when it failed without one)
  $cmd = '& \\Mac\win\job\job.ps1 *> C:\rbtest\job\out.txt; $ok = $?; $c = $LASTEXITCODE; if ($null -eq $c) { $c = [int](-not $ok) }; Set-Content C:\rbtest\job\out.txt.done $c'
  $a = New-ScheduledTaskAction -Execute "$env:WINDIR\System32\conhost.exe" -Argument ('--headless powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "' + $cmd + '"')
} else {
  $a = New-ScheduledTaskAction -Execute "C:\rbtest\hidden.exe" -Argument "$d\out.txt powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \\Mac\win\job\job.ps1"
}
$p = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive
$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 30)
Register-ScheduledTask -TaskName "rbstems-run" -Action $a -Principal $p -Settings $s -Force | Out-Null
Start-ScheduledTask -TaskName "rbstems-run"
$t = 0
while (-not (Test-Path "$d\out.txt.done") -and $t -lt 1200) { Start-Sleep -Milliseconds 500; $t++ }
if (Test-Path "$d\out.txt") { Get-Content "$d\out.txt" }
# this script's status, and so winps.sh's: the job's exit code from .done, or 124 on a timeout
if (-not (Test-Path "$d\out.txt.done")) { "TIMEOUT waiting for the user job"; exit 124 }
$code = 0
if (-not [int]::TryParse(([string](Get-Content "$d\out.txt.done" -TotalCount 1)).Trim(), [ref]$code)) { $code = 1 }
exit $code
EOF
} > "$SHARE/job/launch.ps1"
run_file launch.ps1
