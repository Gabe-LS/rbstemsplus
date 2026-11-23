# W13a's UAC "No" row with Stems Cache installed (N12, review AF8's rule: a user stop changes
# nothing): Uninstall Stems Cache > Uninstall, and UAC's prompt answered No on the secure desktop
# (arm-no's rbui). One prompt; app.log "UAC declined ...: stopped" and "stopped: Stems Cache's
# choice kept"; nothing changed: rekordbox's onnxruntime.dll still the bridge, Stems Cache still
# on (rekordbox_model=1), its root there, no worker left running; the app idle again.
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$bridge = ([string](Read-TestPayload).windows.bridge.sha256).ToLowerInvariant()
if ((Get-Sha $RbOrt) -ne $bridge) { Skip "rekordbox's onnxruntime.dll isn't the bridge (cache-install failed: its log)" }
$from = Get-AppLogSize
Open-App
if (-not $script:app) { Done }
Invoke-AppButton 'T_BTN_UNINSTALL_CACHE' 'T_UNINSTALL'
if (-not $script:Pressed) { Done }
Check (Wait-Until { Test-ConsentShown } 120) "UAC's prompt shows (consent.exe)"
Test-ConsentResult no
Check (Wait-Until { -not (Test-ConsentShown) } 30) 'and is gone'
Check (Wait-UiEnabled $script:app.Id (Get-CoreText 'T_BTN_UNINSTALL_CACHE') 120) 'the app is idle again: Uninstall Stems Cache enabled'
$said = Read-AppLog $from
"the app's log meanwhile:"
foreach ($x in ($said -split "`r?`n")) { if ($x) { "  $x" } }
Check ($said -match 'UAC declined, or the app is quitting: stopped') 'app.log: UAC declined: stopped'
Check ($said -match "stopped: Stems Cache's choice kept") "app.log: stopped: Stems Cache's choice kept"
Check ((Get-Sha $RbOrt) -eq $bridge) "rekordbox's onnxruntime.dll is still the bridge"
Check (Test-CacheOn) 'Stems Cache is still on (rekordbox_model=1)'
Check (Test-Path -LiteralPath $CacheRoot) "Stems Cache's root is still there ($CacheRoot)"
Check (@(Get-WorkerProcesses).Count -eq 0) 'no worker runs'
Stop-App
Done
