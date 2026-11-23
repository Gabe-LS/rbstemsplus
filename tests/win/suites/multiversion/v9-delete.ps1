# V9's second half, as SYSTEM: 7.2.19's onnxruntime.dll (the bridge) deleted, as Defender would
# quarantine it. 7.2.20's is left as it is.
$f = Get-RbOrtOf '7.2.19'
Remove-Item -LiteralPath $f -Force
Check (-not (Test-Path -LiteralPath $f)) "deleted $f"
Check ((Get-Sha (Get-RbOrtOf '7.2.20')) -eq (Get-ServedBridge)) "7.2.20's is still the bridge"
Done
