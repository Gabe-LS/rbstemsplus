# As SYSTEM, before restore-bin: rekordbox 7.2.19's live onnxruntime.dll (the bridge) replaced by
# a foreign file (System32's version.dll: neither Pioneer's nor ours), so R1 offers Put Back
# Pioneer's File (T_WIN_R1_LIVE_FOREIGN), whose restore takes Pioneer's from Stems Cache's ort\.
# rekordbox is never started.
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
Copy-Item -LiteralPath "$env:WINDIR\System32\version.dll" -Destination $RbOrt -Force
$sha = Get-Sha $RbOrt
Check ($sha -eq (Get-Sha "$env:WINDIR\System32\version.dll") -and $sha -ne $f.PioneerSha) "the live DLL is now a foreign file ($sha)"
Done
