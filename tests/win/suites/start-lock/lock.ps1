# N12 item 6 (AF14, AF16; app.md section 5.9, release.md section 4): the complete uninstall from the
# app's menu, then at once the bootstrap one-liner. The uninstall's after-exit copy holds the
# install lock (Local\io.github.rbstemsplus.install) from when the app has quit until its app
# cleanup is done; to keep it held for a while here, a copy of PING.EXE runs from inside the apps
# folder, started before the uninstall (about 40 s: the after-exit waits up to 60 s for anything
# running from there). The
# one-liner started meanwhile prints its one notice ($Texts.Waiting) once, waits, then installs as
# always; the uninstall was complete (its after-exit took the lock first, ended with AX_DONE, and
# the install wrote `current` only after the held process ended).
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$wait = Get-BootText 'Waiting'
if (-not $wait) { Skip "the staged bootstrap.ps1 has no Waiting line (an older one)" }
Stop-App
Start-AppQuiet
if (-not $script:app) { Done }
$app = $script:app
$ver = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$ping = Join-Path (Join-Path $l.Apps $ver) 'rbtest-hold.exe'
Copy-Item -LiteralPath "$env:WINDIR\System32\PING.EXE" -Destination $ping
# started before the uninstall (its after-exit, once the app has quit, takes the lock, then
# waits up to 60 s for what runs from the apps folder): about 40 s, so it still runs when the app
# has quit (the uninstall's dialogs take about 10 s) and ends within the after-exit's 60 s
$held = Start-Process -FilePath $ping -ArgumentList '-n', '41', '127.0.0.1' -WindowStyle Hidden -PassThru
$null = $held.Handle
Check (-not $held.HasExited) "a process runs from inside the apps folder ($ping, pid $($held.Id), about 40 s)"
$clock = [Diagnostics.Stopwatch]::StartNew()
Invoke-MenuUninstall $app
Complete-Uninstall $app
"the uninstall's dialogs took $([int]$clock.Elapsed.TotalSeconds) s"
Check ($app.WaitForExit(60000)) 'the app quit after the uninstall'
# the after-exit copy (%TEMP%\rbsp-uninstall-*\RB Stems Plus.exe) runs: it takes the lock right
# after the app's exit, without waiting
$ax = $null
Check (Wait-Until { $script:ax = @(Get-CimInstance Win32_Process -Filter "Name='RB Stems Plus.exe'" -ErrorAction SilentlyContinue | Where-Object { [string]$_.ExecutablePath -like '*\rbsp-uninstall-*\RB Stems Plus.exe' }) | Select-Object -First 1; $null -ne $script:ax } 20) 'the after-exit copy runs'
# its process kept, for its outcome (stub.c: the after-exit's AX_* is its exit code; AX_DONE 0)
$axp = $null
if ($ax) { try { $axp = Get-Process -Id $ax.ProcessId; $null = $axp.Handle } catch { $axp = $null } }
Check ($null -ne $axp) "the after-exit copy's process held (pid $(if ($ax) { $ax.ProcessId }))"
Start-Sleep -Seconds 2
# VRR2 / VRR4: the uninstall keeps the old current during the hold. It is left as it is (the
# after-exit reads it on a partial delete, still_starts): its content and write time are saved,
# and a current that differs during the poll is the install's. The installed tree (the held copy
# included) is the baseline for new entries.
function Get-CurrentStamp {
    if (-not (Test-Path -LiteralPath $l.Current)) { return '-' }
    try { return [IO.File]::ReadAllText($l.Current) + '|' + [IO.File]::GetLastWriteTimeUtc($l.Current).Ticks } catch { return '?' }
}
$curBefore = Get-CurrentStamp
"current before the one-liner: $($curBefore -replace "`r?`n", ' ')"
$before = @(Get-ChildItem -LiteralPath $l.Apps -Recurse -Force | ForEach-Object FullName)
Check (-not $held.HasExited) 'the held process still runs when the one-liner starts'
$limit = Get-WaitSecs 60
$t = [Diagnostics.Stopwatch]::StartNew()
$job = Start-Job -ScriptBlock {
    param($invoke, $url)
    & ([scriptblock]::Create($invoke)) $url
} -ArgumentList ${function:Invoke-OneLiner}.ToString(), $VmRun.Url
$polls = 0
$earlyCurrent = $false
$earlyEntries = @()
while (-not $held.HasExited -and $t.Elapsed.TotalSeconds -lt $limit) {
    $stamp = Get-CurrentStamp
    $current = $stamp -ne '-' -and $stamp -ne '?' -and $stamp -cne $curBefore
    $new = @(Get-ChildItem -LiteralPath $l.Apps -Recurse -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notin $before } | ForEach-Object FullName)
    if ($held.HasExited) { break }
    $polls++
    if ($current) { $earlyCurrent = $true }
    $earlyEntries = @($earlyEntries + $new | Sort-Object -Unique)
    Start-Sleep -Milliseconds 200
}
Check ($polls -gt 0 -and $held.HasExited) 'the hold was polled through to the process exit'
Check (-not $earlyCurrent) 'the install did not write current during the hold (its content and write time as saved)'
Check ($earlyEntries.Count -eq 0) "no new entries under Apps during the hold$(if ($earlyEntries.Count) { ': ' + ($earlyEntries -join ', ') })"
$out = Receive-Job -Job $job -Wait -AutoRemoveJob
$secs = [int]$t.Elapsed.TotalSeconds
"one-liner ($secs s): $out"
$n = ([regex]::Matches($out, [regex]::Escape($wait))).Count
Check ($n -eq 1) "its notice once ($n times): $wait"
Check ($out -notmatch [regex]::Escape((Get-BootText 'Uninstalling'))) 'not the lock-timeout refusal'
Check ($out -match 'RB Stems Plus [0-9]+\.[0-9]+\.[0-9]+ is installed\.\s*$') 'then it installed'
# the after-exit's outcome: done (AX_DONE), everything removed; then the install, after the hold
Check ($null -ne $axp -and $axp.WaitForExit(30000)) 'the after-exit ended'
if ($axp -and $axp.HasExited) { Check ($axp.ExitCode -eq 0) "with AX_DONE (exit code $($axp.ExitCode))" }
$cur = if (Test-Path -LiteralPath $l.Current) { (Get-Content -Raw -LiteralPath $l.Current).Trim() } else { '' }
Check ($cur -match '^[0-9]+\.[0-9]+\.[0-9]+$' -and (Test-Path -LiteralPath (Join-Path (Join-Path $l.Apps $cur) 'RB Stems Plus.exe'))) "installed again: current names $cur"
Check (-not (Test-Path -LiteralPath $ping)) 'the held copy went with the uninstall (it was complete)'
Check (-not (Test-Path -LiteralPath (Join-Path $l.Support 'welcomed'))) 'the support folder was removed before the install (no welcomed left)'
Get-CleanupRunOnce | ForEach-Object { "RunOnce: $_" }
if (-not $held.HasExited) { Stop-Process -Id $held.Id -Force }
Complete-Welcome 90
Stop-App
Done
