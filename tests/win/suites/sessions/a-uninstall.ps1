# UN8: A's complete uninstall from Settings (--uninstall) while B's copy runs. It completes as if
# no one else were there: the after-exit ends AX_DONE (0) well inside its 60 s wait for programs
# still running, which B's processes (refused to this token, review GH1) never start; everything
# of A's is gone; its log (the debug output) has the apps folder deleted and says neither that a
# program still runs, nor that the process list couldn't be read, nor that the watchers couldn't
# all be checked.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$refused = @(Get-RefusedOurs | Where-Object { $_.Error -eq 5 })
foreach ($r in $refused) { "  refused: pid $($r.Id) $($r.Name), session $($r.Session)" }
Check (@($refused | Where-Object { $_.Name -like 'rbsp-watch-*' }).Count -ge 1 -and @($refused | Where-Object { $_.Name -eq 'RB Stems Plus' }).Count -ge 1) "B's app and watcher still run, refused to this token"
Start-DebugLog
$dbg = $script:DebugOn
Stop-App
Start-AfterExitWatch
Start-AppQuiet @('--uninstall')
if (-not $script:app) { Stop-DebugLog; Done }
Complete-Uninstall $script:app
$quit = [Diagnostics.Stopwatch]::StartNew()
$ax = Wait-AfterExit 30
Check ($null -ne $ax) "the after-exit copy runs$(if ($ax) { " (pid $($ax.Id), $($ax.Path))" })"
if ($ax) {
    Check ($ax.WaitForExit(90000)) 'it ends'
    $secs = [int]$quit.Elapsed.TotalSeconds
    if ($ax.HasExited) {
        Check ($ax.ExitCode -eq 0) "its exit code: AX_DONE 0 ($($ax.ExitCode))"
        Check ($secs -lt 45) "it ended $secs s after the app quit: no 60 s wait for a program still running"
    }
}
Test-UninstallGone
$lines = if ($ax) { @(Get-DebugLinesOf $ax.Id) } else { @() }
Stop-DebugLog
foreach ($x in $lines) { "debug: $x" }
if ($dbg -and $ax) {
    Check (@($lines | Where-Object { $_.StartsWith('after-exit: ') -and $_.EndsWith('\Programs\RB Stems Plus deleted') }).Count -eq 1) 'its log: the apps folder deleted'
    Check (@($lines -match 'still runs from|process list').Count -eq 0) 'its log: no program still running, no unreadable process list'
    Check (@($lines -match 'watchers couldn.t all be checked').Count -eq 0) 'its log: every watcher checked (B''s never in doubt)'
}
Done
