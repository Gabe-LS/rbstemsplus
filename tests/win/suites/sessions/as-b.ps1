# UN8's B side: not a step. sessions.suite's account-b.ps1 writes it with vmlib.ps1 in front ($VmRun
# and $BDir first) and runs it as rbB through a task, LogonType Password: B's own logon session, in
# session 0. The one-liner against the served TEST build into B's profile, then B's app and watcher
# from B's apps folder (B's app waits at its welcome: nobody answers it); the lines into ready.txt
# for account-b.ps1; then it waits, keeping B's logon session (and its processes' profile), until
# b-after.ps1 writes stop (at most 30 minutes).
$lines = @(try {
        "as $([Security.Principal.WindowsIdentity]::GetCurrent().Name) in session $((Get-Process -Id $PID).SessionId)"
        $l = Get-Layout
        $out = Invoke-OneLiner $VmRun.Url
        "one-liner: $out"
        Check ($out -match 'RB Stems Plus [0-9]+\.[0-9]+\.[0-9]+ is installed\.\s*$') 'the one-liner installed'
        Check (Wait-Until { @(Get-AppProcesses | Where-Object { $_.ProcessName -like 'rbsp-watch-*' }).Count -ge 1 } 60) "the watcher runs from $($l.Apps)"
        Check (Wait-Until { @(Get-AppProcesses | Where-Object { $_.ProcessName -eq 'RB Stems Plus' -and $_.Path -ine $l.Stub }).Count -ge 1 } 90) 'the app runs from its version folder'
        Check (Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus') 'its Apps & features entry'
    } catch { "FAIL as-b.ps1 stopped: $($_.Exception.Message)" }) | ForEach-Object { [string]$_ }
[IO.File]::WriteAllLines((Join-Path $BDir 'ready.tmp'), [string[]]$lines)
Move-Item -LiteralPath (Join-Path $BDir 'ready.tmp') -Destination (Join-Path $BDir 'ready.txt') -Force
$t = [Diagnostics.Stopwatch]::StartNew()
while (-not (Test-Path -LiteralPath (Join-Path $BDir 'stop')) -and $t.Elapsed.TotalMinutes -lt 30) { Start-Sleep -Seconds 1 }
exit 0
