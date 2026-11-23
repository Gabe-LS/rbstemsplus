# V10's end, as SYSTEM: one UAC, and Stems Cache's root gone; nothing of ours left under
# Program Files\rekordbox.
Test-MvUac 'v10u' 1
Test-MvRootGone 30
$rb = Join-Path $env:ProgramFiles 'rekordbox'
"left under $($rb): $(@(Get-ChildItem -LiteralPath $rb -Recurse -Force -ErrorAction SilentlyContinue | Select-Object -First 20 | ForEach-Object { $_.FullName.Substring($rb.Length) }) -join ', ')"
$ours = @(Get-ChildItem -LiteralPath $rb -Recurse -Force -Filter 'onnxruntime.dll' -ErrorAction SilentlyContinue | Where-Object { (Get-Sha $_.FullName) -eq (Get-ServedBridge) })
Check ($ours.Count -eq 0) "no bridge left under $rb"
Done
