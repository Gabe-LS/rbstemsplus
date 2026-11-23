# UN8's end, as SYSTEM: B's copy untouched by A's uninstall (B's processes the same ones, still
# running; B's stub, current and Apps & features entry there); then B's task ended (its stop file,
# then stopped and removed), B's processes ended, and no password file left. The cleanup runs
# whatever the checks say.
$pids = Join-Path $BDir 'pids.txt'
$before = if (Test-Path -LiteralPath $pids) { @([IO.File]::ReadAllLines($pids)) } else { @() }
$now = Get-BProcesses
foreach ($x in $before) {
    $id = [int]($x.Split(' ')[0])
    Check (@($now | Where-Object { $_.ProcessId -eq $id -and "$($_.ProcessId) $($_.ExecutablePath)" -ieq $x }).Count -eq 1) "B's process still runs: $x"
}
$bp = Get-BProfile
if ($bp) {
    $bapps = Join-Path $bp 'AppData\Local\Programs\RB Stems Plus'
    Check ((Test-Path -LiteralPath (Join-Path $bapps 'RB Stems Plus.exe')) -and (Test-Path -LiteralPath (Join-Path $bapps 'current'))) "B's install is there ($bapps)"
    $hive = "Registry::HKEY_USERS\$(Get-BSid)"
    if (Test-Path -LiteralPath $hive) { Check (Test-Path -LiteralPath "$hive\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus") "B's Apps & features entry is there" }
    else { "B's registry hive isn't loaded: its Apps & features entry not read" }
}
if ($VmRun.Step -eq 'b-after-desktop') {
    $desktop = Get-BDesktop
    Check ([bool]$desktop -and @($now | Where-Object SessionId -ne $desktop).Count -eq 0) "B's desktop session still exists with its app and watcher"
}
Set-Content -LiteralPath (Join-Path $BDir 'stop') -Value 'stop' -Encoding ASCII
[void](Wait-Until { (Get-ScheduledTask -TaskName $BTask -ErrorAction SilentlyContinue).State -ne 'Running' } 30)
Stop-TaskAs $BTask
foreach ($p in Get-BProcesses) { "ending B's pid $($p.ProcessId), $($p.ExecutablePath)"; Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
Check (Wait-Until { (Get-BProcesses).Count -eq 0 } 30) "B's processes ended"
Check ($null -eq (Get-ScheduledTask -TaskName $BTask -ErrorAction SilentlyContinue)) "B's task $BTask is gone"
Check (Remove-TestSecrets) 'no password file left'
if (-not $before.Count) { Missing "B's processes (account-b.ps1 recorded none: its log)" }
Done
