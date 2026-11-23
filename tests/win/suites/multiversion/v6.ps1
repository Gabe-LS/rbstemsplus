# V6, as the user: the running gate. The stand-in "rekordbox 7.2.19" runs (v6-standin); Reinstall
# Stems Cache > Reinstall is refused before any change: "Close rekordbox 7.2.19 (also in other
# accounts on this PC), then try again." (or its Try Again form), answered Cancel; no admin run,
# both folders still the bridge. The stand-in is ended at the end.
$app = $null
Open-App
if (-not $script:app) { Done }
$app = $script:app
$b = Get-ServedBridge
$exe = Get-RbExeOf '7.2.19'
$rb = Start-Process -FilePath $exe -PassThru
Check ((Wait-Until { $rb.Refresh(); $rb.MainWindowTitle -eq 'rekordbox' } 30) -and -not $rb.HasExited) "the stand-in rekordbox 7.2.19 runs with its window (pid $($rb.Id), $exe)"
Start-Sleep 5
Show-MvRows $app.Id
"(the stand-in isn't the audited rekordbox.exe, so 7.2.19 may read Needs repair meanwhile: expected here)"
Start-MvBatch 'v6'
# no confirmation expected first: the gate answers before it (uaclib's Invoke-Confirm would count a
# hidden FAIL, its Check's line going into its caller's variable); one that shows is answered
Invoke-AppButton 'T_BTN_REINSTALL_CACHE'
if (Invoke-UiNamed $app.Id (Get-CoreText 'T_REINSTALL') 10 'Button') { 'a Reinstall confirmation came first: answered Reinstall' }
$msgs = @((Get-RowText 'T_WIN_RB_OPEN_VERSION_MSG' '7.2.19'), (Get-RowText 'T_WIN_RB_OPEN_VERSION_TRY' '7.2.19'))
$rx = '^(' + (($msgs | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
$shown = Find-UiText $app.Id $rx 90
Check $shown "the alert names the version: $($msgs[0])"
Show-MvUi $app.Id
Save-MvShot 'gate'
if (-not $shown) {
    $generic = '^(' + [regex]::Escape((Get-CoreText 'T_RB_OPEN_MSG')) + '|' + [regex]::Escape((Get-CoreText 'T_RB_OPEN_TRY_AGAIN')) + ')$'
    "the generic text instead: $(Find-UiText $app.Id $generic 1)"
}
$closed = (Invoke-UiNamed $app.Id (Get-CoreText 'T_CANCEL') 20 'Button') -or (Invoke-UiNamed $app.Id (Get-CoreText 'T_OK') 5 'Button')
Check $closed 'the alert answered (Cancel, or OK)'
Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_BTN_REINSTALL_CACHE') 60) 'the app idle again'
$runs = Stop-MvBatch 'v6'
Check ($runs -eq 0) "no admin run (got $runs)"
foreach ($v in $MvVersions) { Check ((Get-Sha (Get-RbOrtOf $v)) -eq $b) "rekordbox $v's onnxruntime.dll is still the bridge" }
Stop-Process -Id $rb.Id -Force -ErrorAction SilentlyContinue
Check (Wait-Until { $rb.HasExited } 20) 'the stand-in ended'
Done
