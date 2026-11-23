# V3, as the user with the full token (the installer is elevated; this step reads the medium app
# and watcher as well): waits for the real 7.2.20 installer to end (rekordbox never started: one
# that appears is ended at once and fails the step), then reads, every half second, how soon the
# open app's Manage shows the second version (mv-spec section 5 V3, under M1): "Stems Cache on in
# 1 of 2 versions", "7.2.19 · On", "7.2.20 · Unsupported" (pass: within 5 s of the installer's
# end), without coming to the foreground; and the watcher's notice "doesn't support rekordbox
# 7.2.20 yet", once (watcher.log's ask), answered OK, and not again within 45 s.
$l = Get-Layout
$wfrom = Get-WatcherLogSize
$app = Get-AppWindow
Check ($null -ne $app) "RB Stems Plus is open (pid $(if ($app) { $app.Id }))"
if (-not $app) { Done }
# another window in front of the app (a console), so "without taking focus" can be read
$fg = Start-Process -FilePath "$env:WINDIR\System32\cmd.exe" -ArgumentList '/k', 'title mv-foreground' -PassThru
Start-Sleep 2
"in front: pid $(Get-ForegroundPid) (the console $($fg.Id), the app $($app.Id))"
function Get-Inst { @(Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(Install_rekordbox|Au_\.exe)' }) }
$t = [Diagnostics.Stopwatch]::StartNew()
$rbSeen = $false
while (@(Get-Inst).Count -and $t.Elapsed.TotalSeconds -lt 500) {
    foreach ($r in @(Get-CimInstance Win32_Process -Filter "Name = 'rekordbox.exe'")) { Stop-Process -Id $r.ProcessId -Force -ErrorAction SilentlyContinue; $rbSeen = $true; "ENDED rekordbox.exe pid $($r.ProcessId) :: $($r.ExecutablePath)" }
    Start-Sleep -Milliseconds 500
}
$ended = -not @(Get-Inst).Count
$endAt = [Diagnostics.Stopwatch]::StartNew()
Check $ended "the 7.2.20 installer ended ($([int]$t.Elapsed.TotalSeconds) s after this step began)"
Check (-not $rbSeen) 'rekordbox was never started by the installer'
if (-not $ended) { Done }
$fgBefore = Get-ForegroundPid
$want = @((Rx (Get-SummaryText 1 2)), (Rx (Get-RowText 'T_WIN_ROW_ON' '7.2.19')), (Rx (Get-RowText 'T_WIN_ROW_UNSUPPORTED' '7.2.20')))
$fgAt = 0
$seen = Wait-Until { $n = Get-MvNames $app.Id; $script:fgAt = Get-ForegroundPid; -not @($want | Where-Object { $w = $_; -not @($n | Where-Object { $_ -match $w }).Count }).Count } 120
$secs = [math]::Round($endAt.Elapsed.TotalSeconds, 1)
Check $seen "Manage shows the summary and both rows ($secs s after the installer ended)"
Check ($seen -and $secs -le 5) "within 5 s of the installer's end ($secs s)"
Show-MvRows $app.Id
Save-MvShot 'refreshed'
"foreground before: pid $fgBefore, at the refresh: pid $script:fgAt, the app: $($app.Id)"
if ($fgBefore -eq $app.Id) { "the app was in front already: the focus check can't be read here" }
else { Check ($script:fgAt -ne $app.Id) 'the refresh did not bring the app to the foreground' }
Stop-Process -Id $fg.Id -Force -ErrorAction SilentlyContinue
# the watcher's notice (M1: 7.2.20 unsupported)
$text = Get-RowText 'T_WIN_W_UNSUPPORTED' '7.2.20'
$w = Find-WatcherDialog (Rx $text) 90
Check ($null -ne $w) "the watcher's notice shows: $text"
if ($w) {
    Show-MvUi $w 'watcher'
    Save-MvShot 'notice'
    Check (Invoke-MvDialogButton $w (Rx $text) (Get-CoreText 'T_OK') 30) 'the notice answered OK'
}
Start-Sleep 45
$asks = Get-WatcherAsks $wfrom '7.2.20'
Check ($asks.Count -eq 1) "watcher.log asked about 7.2.20 once ($($asks.Count))"
Check ($null -eq (Find-WatcherDialog (Rx $text) 1)) 'no second notice'
"watcher.log since this step:"
foreach ($x in ((Read-WatcherLog $wfrom) -split "`r?`n")) { if ($x) { "  watcher.log: $x" } }
"app.log tail:"
Get-Content -LiteralPath (Join-Path $l.Support 'Logs\app.log') -Tail 25 | ForEach-Object { "  app.log: $_" }
Done
