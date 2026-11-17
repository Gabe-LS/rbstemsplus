# -Restore puts Pioneer's onnxruntime.dll back into rekordbox's folder (release.md section 4),
# run elevated (user-high: the user's full token, no prompt) through the real one-liner. The steps
# that use this script: restore (the bridge in place), restore-missing (the live DLL deleted
# first, R4.I71: the verified copy moved into place) and restore-constrained (the same in
# ConstrainedLanguage, R4.I69). After it: Pioneer's file, verified by its hash.
Assert-RestoreServed
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
$bridge = ([string](Read-TestPayload).windows.bridge.sha256).ToLowerInvariant()
Check (Test-HighToken) 'this step runs elevated (High mandatory level)'
$before = Get-LiveState $f $bridge
"before: rekordbox's onnxruntime.dll is $before"
$want = if ($VmRun.Step -eq 'restore-missing') { 'absent' } else { 'the bridge' }
if ($before -ne $want) { Skip "rekordbox's DLL is $before, not $want (the step before failed: its log)" }
$constrained = $VmRun.Step -eq 'restore-constrained'
$out = Invoke-Restore -Constrained:$constrained
"-Restore$(if ($constrained) { ' (ConstrainedLanguage)' }): $out"
Check ((Get-LiveState $f $bridge) -eq 'Pioneer''s') "Pioneer's onnxruntime.dll is back ($(Get-LiveState $f $bridge))"
Check (@(Get-ChildItem -LiteralPath $RbDir -Force -File | Where-Object { $_.Name -like 'onnxruntime.dll?*' }).Count -eq 0) 'no temporary copy left beside it'
Done
