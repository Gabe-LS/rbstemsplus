# SYSTEM: change only ort's copy, with the verified bridge still live.
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from worker prep' }
$ort = Join-Path $CacheRoot 'ort\onnxruntime.dll'
$keep = "$ort.rbtest"
if ($VmRun.Step -eq 'r1-restore-ort') {
    if (-not (Test-Path -LiteralPath $keep)) { Skip 'no saved ORT copy' }
    Move-Item -LiteralPath $keep -Destination $ort
    Check ((Get-Sha $ort) -eq $f.PioneerSha) 'the saved ORT is back'
    Done
}
$bridge = ([string](Read-TestPayload).windows.bridge.sha256).ToLowerInvariant()
Check ($bridge -and (Get-Sha $RbOrt) -eq $bridge) 'the TEST bridge is live'
Check ((Get-Sha $ort) -eq $f.PioneerSha) 'Pioneer ORT is present in ort'
Check (@(Get-AppProcesses | Where-Object ProcessName -eq 'RB Stems Plus').Count -eq 0) 'the app is closed before the ORT change'
if ($script:VmFails) { Done }
Set-Content -LiteralPath (Join-Path $WtR1Dir 'wl-from') -Value (Get-WatcherLogSize) -Encoding ASCII
Move-Item -LiteralPath $ort -Destination $keep
Check (-not (Test-Path -LiteralPath $ort)) 'ort is missing its copy'
Check ((Get-Sha $RbOrt) -eq $bridge) 'the live bridge is unchanged'
Done
