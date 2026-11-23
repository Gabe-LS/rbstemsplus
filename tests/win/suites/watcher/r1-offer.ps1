# The watcher must open the app from the fixture's own notification and show the matching offer.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$case = switch ($VmRun.Step) {
    'r1-offer-live' { @{ Text = 'T_WIN_R1_LIVE_MISSING'; Log = 'onnxruntime.dll is missing' } }
    'r1-offer-ort'  { @{ Text = 'T_WIN_R1_ORT_MISSING'; Log = 'ort\''s copy of Pioneer''s file is missing' } }
    'r1-offer'      { @{ Text = 'T_WIN_R1_LIVE_FOREIGN'; Log = "onnxruntime.dll is neither Pioneer's nor RB Stems Plus's" } }
}
Show-WatcherState 'before'
Check (@(Get-Watchers).Count -ge 1) 'a watcher runs'
$from = Get-WatcherLogFrom
# VRR6: require the offset saved for this change.
Check ($null -ne $from) 'the fixture saved the watcher.log offset'
if ($null -eq $from) { Done }
"watcher.log read from byte $from"
$opened = Wait-Until { $null -ne (Get-AppWindow) } 120
$wl = Read-WatcherLog $from
($wl -split "`r?`n" | Where-Object { $_ -match 'R1|Pioneer|onnxruntime|not asking|round' } | Select-Object -Last 12) | ForEach-Object { "watcher.log: $_" }
$offer = [regex]::Escape("$($case.Log): opening RB Stems Plus to put Pioneer's file back")
Check ($wl -match $offer) 'watcher.log: the watcher opens RB Stems Plus for R1'
Check $opened 'the app opened (by the watcher, through the stub)'
if (-not $opened) { Done }
$app = Get-AppWindow
$line = Get-CoreText $case.Text
Check (Find-UiText $app.Id ('^' + [regex]::Escape($line) + '$') 60) "R1's line: $line"
Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_WIN_BTN_RESTORE') 30) "$(Get-CoreText 'T_WIN_BTN_RESTORE') shows, enabled"
Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_W_DONT_ASK') 30) "$(Get-CoreText 'T_W_DONT_ASK') shows, enabled"
if ($VmRun.Step -ne 'r1-offer') { Stop-App }
Done
