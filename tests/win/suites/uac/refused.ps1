# UP4, UAC refused (release.md section 8): Install Stems Cache > Install, and UAC's prompt answered
# No on the secure desktop (arm-no's rbui). One prompt, no second one; nothing changed: no Stems
# Cache root, rekordbox's onnxruntime.dll still Pioneer's, Stems Cache not on, no worker left
# running; the app idle again (Install Stems Cache enabled).
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
if (@(Get-Rekordbox).Count) { Check (Close-Rekordbox 200) 'rekordbox, open in the snapshot, closed first' }
$from = Get-AppLogSize
Open-App
if (-not $script:app) { Done }
Invoke-AppButton 'T_BTN_INSTALL_CACHE' 'T_INSTALL'
if (-not $script:Pressed) { Done }
Check (Wait-Until { Test-ConsentShown } 120) "UAC's prompt shows (consent.exe)"
Test-ConsentResult no
Check (Wait-Until { -not (Test-ConsentShown) } 30) 'and is gone'
Check (Wait-UiEnabled $script:app.Id (Get-CoreText 'T_BTN_INSTALL_CACHE') 120) 'the app is idle again: Install Stems Cache enabled'
Check ((Get-Sha $RbOrt) -eq $f.PioneerSha) "rekordbox's onnxruntime.dll is still Pioneer's"
Check (-not (Test-Path -LiteralPath $CacheRoot)) "no Stems Cache root ($CacheRoot)"
Check (-not (Test-CacheOn)) 'Stems Cache is not on'
Check (@(Get-WorkerProcesses).Count -eq 0) 'no worker runs'
"the app's log meanwhile:"
foreach ($x in ((Read-AppLog $from) -split "`r?`n")) { if ($x) { "  $x" } }
Stop-App
Done
