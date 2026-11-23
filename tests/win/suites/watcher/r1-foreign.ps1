# As SYSTEM (N10's setup): rekordbox 7.2.19's live onnxruntime.dll made neither Pioneer's nor ours
# (a copy of System32's version.dll, signed by Microsoft, never AlphaTheta), which is R1's
# T_WIN_R1_LIVE_FOREIGN in a verified version folder (rekordbox is never started: nothing loads
# it). Pioneer's own is kept in C:\rbtest\r1\ for r1-restore. The step name r1-foreign-2 deletes the
# live DLL instead (R1's "missing", a state with another key than "foreign"): a change in the
# version folder, for the watcher's next R1 round. Each saves watcher.log's length first
# (C:\rbtest\r1\wl-from), where the user step after it reads from.
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
New-Item -ItemType Directory -Force $WtR1Dir | Out-Null
$keep = Join-Path $WtR1Dir 'onnxruntime.dll'
if (-not (Test-Path -LiteralPath $keep)) {
    Check ((Get-Sha $RbOrt) -eq $f.PioneerSha) "the live DLL is Pioneer's before ($RbOrt)"
    Copy-Item -LiteralPath $RbOrt -Destination $keep
}
Check (@(Get-AppProcesses | Where-Object ProcessName -eq 'RB Stems Plus').Count -eq 0) 'the app is closed before the live DLL change'
if ($script:VmFails) { Done }
# where the next user step reads watcher.log from: only the rounds this change brings
$wl = Get-WatcherLogSize
Set-Content -LiteralPath (Join-Path $WtR1Dir 'wl-from') -Value $wl -Encoding ASCII
"watcher.log is $wl bytes before the change"
if ($VmRun.Step -in 'r1-missing-live', 'r1-foreign-2') {
    # another state (R1's key: live=missing, not live=foreign), so only Don't Ask Again can keep
    # the watcher from opening the app for it
    Remove-Item -LiteralPath $RbOrt -Force
    Check (-not (Test-Path -LiteralPath $RbOrt)) "the live DLL is deleted now ($RbOrt)"
    Done
}
$src = "$env:WINDIR\System32\version.dll"
Copy-Item -LiteralPath $src -Destination $RbOrt -Force
$sha = Get-Sha $RbOrt
Check ($sha -eq (Get-Sha $src) -and $sha -ne $f.PioneerSha) "the live DLL is now a copy of $src ($sha)"
$sig = Get-AuthenticodeSignature -LiteralPath $RbOrt
"its signature: $($sig.Status) $($sig.SignerCertificate.Subject)"
Check (-not ([string]$sig.SignerCertificate.Subject -match 'AlphaTheta')) 'not AlphaTheta''s'
Done
