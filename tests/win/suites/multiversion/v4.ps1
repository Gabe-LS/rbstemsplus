# V4 (after switch-m2): the payload refresh. The app started again fetches the served M2 (its
# update check at start-up) and stores it; Manage then shows "7.2.20 · Ready to install"; the
# watcher (Stems Cache chosen) asks once, grouped: "Stems Cache isn't on in rekordbox 7.2.20 yet."
# with Install Now, Remind Me Later and Don't Ask Again; Remind Me Later, then no repeat for 60 s.
# The app is left open for V5.
$l = Get-Layout
$wfrom = Get-WatcherLogSize
$afrom = Get-AppLogSize
Stop-App
# the payload refresh: the start-up's update check runs when the saved payload is over a day old
# (win/c/native_steps.c native_manifest_due; a new unlisted version doesn't make it due while
# Stems Cache is on elsewhere, run 3), so the saved payload.json is made two days old, as a day
# passing would
$saved = Join-Path $l.Support 'payload\payload.json'
if (Test-Path -LiteralPath $saved) { (Get-Item -LiteralPath $saved).LastWriteTime = (Get-Date).AddDays(-2); "the saved payload.json made 2 days old" }
Start-AppQuiet
if (-not $script:app) { Done }
$app = $script:app
Clear-MvOffer $app.Id 20 -Fail
$stored = Join-Path $l.Support 'payload\payload.json'
$got = Wait-Until { (Test-Path -LiteralPath $stored) -and ((Get-Content -Raw -LiteralPath $stored | ConvertFrom-Json).windows.compatible.rekordbox -contains '7.2.20') } 120
Check $got "the app stored M2 (payload\payload.json lists 7.2.20)"
Test-MvRows $app.Id @((Rx (Get-SummaryText 1 2)), (Rx (Get-RowText 'T_WIN_ROW_ON' '7.2.19')), (Rx (Get-RowText 'T_WIN_ROW_READY' '7.2.20'))) 120
Save-MvShot 'ready'
$text = Get-RowText 'T_WIN_W_ADD_CACHE' '7.2.20'
$w = Find-WatcherDialog (Rx $text) 180
Check ($null -ne $w) "the watcher asks: $text"
if ($w) {
    Show-MvUi $w 'watcher'
    Save-MvShot 'ask'
    $names = Get-MvNames $w
    foreach ($b in 'T_WIN_W_INSTALL_NOW', 'T_W_REMIND_LATER', 'T_W_DONT_ASK') { Check ($names -contains (Get-CoreText $b)) "its button $(Get-CoreText $b)" }
    Check (Invoke-MvDialogButton $w (Rx $text) (Get-CoreText 'T_W_REMIND_LATER') 30) 'answered Remind Me Later'
}
Start-Sleep 60
$asks = Get-WatcherAsks $wfrom '7.2.20'
Check ($asks.Count -eq 1) "watcher.log asked about 7.2.20 once in this set ($($asks.Count))"
Check ($null -eq (Find-WatcherDialog (Rx $text) 1)) 'no repeat after Remind Me Later'
Check ((Get-MvWindowCount $app.Id) -eq 1) 'the app asks nothing itself (its main window only)'
foreach ($x in ((Read-WatcherLog $wfrom) -split "`r?`n")) { if ($x) { "  watcher.log: $x" } }
foreach ($x in ((Read-AppLog $afrom) -split "`r?`n")) { if ($x) { "  app.log: $x" } }
Done
