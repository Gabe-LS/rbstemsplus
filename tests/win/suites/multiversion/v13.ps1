# V13, as the user: Cancel UAC in a Stems Cache batch. Reinstall Stems Cache > Reinstall; UAC's
# prompt answered No by arm-no-v13's responder: one prompt, then nothing changed (both folders
# still the bridge, every receipt as it was), no alert (today's rule: a refused prompt says
# nothing), the app idle again.
$b = Get-ServedBridge
Open-App
if (-not $script:app) { Done }
$app = $script:app
Start-MvBatch 'v13'
Invoke-AppButton 'T_BTN_REINSTALL_CACHE' 'T_REINSTALL'
if ($script:Pressed) {
    Check (Wait-Until { Test-ConsentShown } 120) "UAC's prompt shows (consent.exe)"
    Test-ConsentResult no
    Check (Wait-Until { -not (Test-ConsentShown) } 30) 'and is gone'
}
Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_BTN_REINSTALL_CACHE') 120) 'the app idle again'
Start-Sleep 3
Check ((Get-MvWindowCount $app.Id) -eq 1) 'no alert (the main window only)'
Show-MvUi $app.Id
Save-MvShot 'after'
foreach ($v in $MvVersions) { Check ((Get-Sha (Get-RbOrtOf $v)) -eq $b) "rekordbox $v's onnxruntime.dll is still the bridge" }
Check (@(Get-WorkerProcesses).Count -eq 0) 'no worker runs'
$runs = Stop-MvBatch 'v13'
"admin runs in app.log: $runs (each one a prompt: this one refused)"
Done
