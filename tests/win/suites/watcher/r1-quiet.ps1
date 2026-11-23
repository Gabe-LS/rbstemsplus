# N10: after r1-foreign-2 deleted the version folder's live DLL (R1's "missing": a new state the
# watcher hasn't opened the app for, so only Don't Ask Again holds it back), the watcher's next R1
# round finds r1:7.2.19 in dismissed and doesn't open RB Stems Plus: watcher.log, from r1-foreign-2's
# saved length, says "...onnxruntime.dll is missing: not opening RB Stems Plus (Don't Ask Again for
# this version)", and no app window shows meanwhile.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
Check (@(Get-Watchers).Count -ge 1) 'a watcher runs'
$from = Get-WatcherLogFrom
# VRR6: require the offset saved for this change.
Check ($null -ne $from) 'r1-foreign-2 saved the watcher.log offset'
if ($null -eq $from) { Done }
"watcher.log read from byte $from (r1-foreign-2's change)"
$re = "onnxruntime\.dll is missing: not opening RB Stems Plus \(Don't Ask Again for this version\)"
$quiet = Wait-Until { (Read-WatcherLog $from) -match $re } 120
$wl = Read-WatcherLog $from
($wl -split "`r?`n" | Where-Object { $_ -match "Pioneer|onnxruntime|not opening|opening RB" } | Select-Object -Last 8) | ForEach-Object { "watcher.log: $_" }
Check $quiet "watcher.log, this round: onnxruntime.dll is missing: not opening RB Stems Plus (Don't Ask Again for this version)"
Start-Sleep -Seconds 10
Check ($null -eq (Get-AppWindow)) 'and the app didn''t open'
Stop-App
Done
