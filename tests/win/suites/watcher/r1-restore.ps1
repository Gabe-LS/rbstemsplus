# As SYSTEM: Pioneer's onnxruntime.dll put back (r1-foreign kept it), so the later rows start from
# R1 ok
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
$keep = Join-Path $WtR1Dir 'onnxruntime.dll'
if (-not (Test-Path -LiteralPath $keep)) { Skip 'r1-foreign kept nothing' }
Copy-Item -LiteralPath $keep -Destination $RbOrt -Force
Check ((Get-Sha $RbOrt) -eq $f.PioneerSha) "rekordbox's onnxruntime.dll is Pioneer's again"
Done
