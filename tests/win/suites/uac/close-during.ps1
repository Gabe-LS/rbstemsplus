# WC5 (app.md 5.9 case 5), as the user: Install Stems Cache > Install, UAC answered Yes (arm-yes*),
# and the app's window closed while the elevated worker runs. Close is refused while an action
# runs (the Mac's windowShouldClose rule, core.c core_close_asked): the app and its window stay,
# the worker finishes (rekordbox's onnxruntime.dll is the payload's bridge), no quit comes later,
# and a Close once the action is over quits the app. The worker is held (a TEST worker only), so
# the Close reaches the app while it runs: unheld, it ends in under a second. UP13: how the worker
# looks from the user's side (its path, or none) is said, not judged.
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
if (@(Get-Rekordbox).Count) { Check (Close-Rekordbox 200) 'rekordbox closed first' }
$from = Get-AppLogSize
Set-WorkerHold $WorkerHoldSecs
try {
Open-App
if (-not $script:app) { Done }
$app = $script:app
$null = $app.Handle   # keeps its exit readable
Invoke-CacheInstall
if (-not $script:Pressed) { Done }
$script:w = $null
Check (Wait-Until { $script:w = @(Get-WorkerProcesses) | Select-Object -First 1; $null -ne $script:w } 300) 'the elevated worker runs (UAC answered Yes)'
if (-not $script:w) { "UAC's prompt shows now: $(Test-ConsentShown)"; Show-AppState; Done }
"UP13: the worker as the user sees it: path '$($script:w.Path)'"
$sent = $app.CloseMainWindow()
Check $sent "Close sent: WM_CLOSE posted to the app's main window (enabled, no dialog up)"
if (-not $sent) { Show-AppState }
"Close sent to the app (pid $($app.Id)) as the worker (pid $($script:w.Id)) runs"
Start-Sleep -Seconds 3
$workerRan = [bool](Get-Process -Id $script:w.Id -ErrorAction SilentlyContinue)
if (-not $workerRan) { Missing 'WC5: the worker ended within 3 s of the Close (not held: a release worker, or RBSP_TEST_WORKER_HOLD not read)' }
Check (-not $app.HasExited) 'Close refused while the action runs: the app is still open 3 s later'
Check ($null -ne (Get-AppWindow)) 'with its window'
Check (Wait-Until { -not (Get-Process -Id $script:w.Id -ErrorAction SilentlyContinue) } 180) 'the worker ended'
Check (Wait-Until { (Read-AppLog $from) -match 'ok: install' } 60) "the worker did its work (its 'ok: install' line in the app's log)"
$p = Read-TestPayload
Check ((Get-Sha $RbOrt) -eq ([string]$p.windows.bridge.sha256).ToLowerInvariant()) "the worker finished: rekordbox's onnxruntime.dll is the payload's bridge"
Check (Wait-Until { Test-CacheOn } 120) 'the install finished: Stems Cache on (rekordbox_model=1 in config.ini)'
Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_BTN_UNINSTALL_CACHE') 60) "the action is over: the app's buttons are enabled again"
"waiting 15 s: the refused Close may not come back as a quit"
Start-Sleep -Seconds ([Math]::Min(15, (Get-WaitSecs 15)))
Check (-not $app.HasExited) 'no quit came after the action ended'
Check ($null -ne (Get-AppWindow)) 'the window is still there'
if (-not $app.HasExited) {
    $sent = $app.CloseMainWindow()
    Check $sent 'the second Close sent'
    Check ($sent -and $app.WaitForExit((Get-WaitSecs 60) * 1000)) 'a Close once the action is over quits the app'
}
"the app's log meanwhile:"
foreach ($x in ((Read-AppLog $from) -split "`r?`n")) { if ($x) { "  $x" } }
if (-not $app.HasExited) { Stop-App }
Done
} finally { Set-WorkerHold 0 }   # (read by the worker at its start: no later worker is held, on every exit)
