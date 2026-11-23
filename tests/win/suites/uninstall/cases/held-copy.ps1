# UN5/UN7: run a complete uninstall and capture its after-exit trace. An extra process in
# Apps makes the after-exit wait until the file handle and the PING in the copy folder are armed.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'the round 6 install is missing' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('rbtest-un7-' + [guid]::NewGuid().ToString('N'))
$version = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$blockerExe = Join-Path $l.Apps "$version\rbtest-un7.exe"
$pingExe = Join-Path $env:WINDIR 'System32\PING.EXE'
$usage = Join-Path $l.Local 'Microsoft\CLR_v4.0\UsageLogs\RB Stems Plus.exe.log'
$ax = $blocker = $ping = $hold = $null
# round 3 (clr-junction) leaves Microsoft\CLR_v4.0 a junction, which the uninstall rightly
# leaves with its entry: this round needs the plain folder back (the link only is removed)
$clr = Join-Path $l.Local 'Microsoft\CLR_v4.0'
if (Test-Reparse $clr) {
    [IO.Directory]::Delete($clr, $false)
    New-Item -ItemType Directory -Path $clr | Out-Null
}
Check (-not (Test-Reparse $clr)) 'CLR_v4.0 is a plain folder for this round'
try {
    Stop-App
    New-Item -ItemType Directory -Path $temp | Out-Null
    $savedTemp, $savedTmp = $env:TEMP, $env:TMP
    try {
        $env:TEMP = $env:TMP = $temp
        Start-AppQuiet @('--uninstall')
    } finally { $env:TEMP, $env:TMP = $savedTemp, $savedTmp }
    if (-not $script:app) { Done }
    $app = $script:app
    Set-UsageLogEntry $usage
    $confirmed = (Invoke-Rbui invoke --pid $app.Id (Get-CoreText 'T_UNINSTALL') --type Button --timeout 45) -eq 0
    Check $confirmed 'the app uninstall was confirmed'
    if (-not $confirmed) { Done }

    Start-DebugLog
    Start-AfterExitWatch (Get-WaitSecs 170) $temp
    Copy-Item -LiteralPath $pingExe -Destination $blockerExe
    $blocker = Start-Process -FilePath $blockerExe -ArgumentList '-n', '121', '127.0.0.1' -WindowStyle Hidden -PassThru
    [void]$blocker.Handle
    $answered = (Invoke-Rbui invoke --pid $app.Id (Get-CoreText 'T_DONE') --type Button --timeout 45) -eq 0
    Check $answered 'the app uninstall finished and its Done button was pressed'
    if (-not $answered) { Done }
    Check (Wait-Until { $app.HasExited } 30) 'the app exited'
    $ax = Wait-AfterExit 20
    Check ($ax -and -not $ax.HasExited -and -not $blocker.HasExited) 'the after-exit copy is running while the Apps blocker is alive'
    if (-not $ax -or $ax.HasExited -or $blocker.HasExited) { Done }
    $copyExe = $ax.Path
    $folder = Split-Path -Parent $copyExe
    $leaf = Split-Path -Leaf $folder
    $hold = [IO.File]::Open($copyExe, 'Open', 'Read', 'ReadWrite')
    $holdClock = [Diagnostics.Stopwatch]::StartNew()
    $ping = Start-Process -FilePath $pingExe -ArgumentList '-n', '10', '127.0.0.1' -WorkingDirectory $folder -WindowStyle Hidden -PassThru
    [void]$ping.Handle
    $cleanupClock = [Diagnostics.Stopwatch]::StartNew()
    $blocker.Kill()
    $loop = Wait-Until {
        $script:cleanupCmd = Get-CimInstance Win32_Process -Filter "Name = 'cmd.exe' AND ParentProcessId = $($ax.Id)" |
            Where-Object { $_.CommandLine -like "*$leaf*" } | Select-Object -First 1
        $null -ne $script:cleanupCmd
    } 7
    Check ($loop -and -not $ping.HasExited -and (Test-Path -LiteralPath $copyExe)) 'the cleanup cmd loop started while the exe and PING held the copy folder'
    $holdMs = [Math]::Max(0, 10000 - $holdClock.ElapsedMilliseconds)
    Start-Sleep -Milliseconds $holdMs
    $hold.Dispose()
    $hold = $null
    Check ($ping.WaitForExit((Get-WaitSecs 5) * 1000) -and $ping.ExitCode -eq 0) 'PING -n 10 completed from the copy folder'
    $ended = $ax.WaitForExit((Get-WaitSecs 15) * 1000)
    Check ($ended -and $ax.ExitCode -eq 0) 'the after-exit returns AX_DONE (0)'
    $left = [Math]::Max(0, [int][Math]::Floor(70 - $cleanupClock.Elapsed.TotalSeconds))
    Check ((Wait-Until { -not (Test-Path -LiteralPath $folder) } $left) -and
        $cleanupClock.Elapsed.TotalSeconds -le 70) 'the copy folder is gone within 70 s of releasing the Apps blocker'
    Check (@(Get-CleanupRunOnce | Where-Object { $_ -like "*$leaf*" }).Count -eq 0) 'no RunOnce fallback for the copy folder'
    Test-UninstallGone
    Check (-not (Test-Path -LiteralPath $usage)) 'the UsageLogs entry is gone'
    if ($script:DebugOn) {
        $lines = @(Get-DebugLinesOf $ax.Id)
        $lines | ForEach-Object { "debug: $_" }
        $waiting = [Array]::IndexOf($lines, 'after-exit: waiting for RB Stems Plus to quit')
        $deleted = [Array]::IndexOf($lines, "after-exit: $($l.Apps) deleted")
        Check ($waiting -ge 0 -and $deleted -gt $waiting -and @($lines -match "couldn.t|RunOnce|nothing deleted").Count -eq 0) 'DBWIN records the wait then the successful Apps deletion'
    }
} finally {
    if ($hold) { $hold.Dispose() }
    [void](Wait-AfterExit 0)
    Stop-DebugLog
    foreach ($p in @($blocker, $ping)) {
        if ($p) {
            if (-not $p.HasExited) { $p.Kill(); [void]$p.WaitForExit(5000) }
            $p.Dispose()
        }
    }
    if ($ax) { $ax.Dispose() }
    Stop-App
    Remove-Item -LiteralPath $blockerExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
Done
