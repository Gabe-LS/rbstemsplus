# A Stems Cache batch through the open app (its name: the step's), as the user: v5, v8-retry, v9i
# and v10i press Install Stems Cache > Install. Done when every version is bridged and Manage shows both
# rows On ("Stems Cache on in 2 of 2 versions"). The batch's admin runs from app.log; its
# elevations are the -files step's.
$name = $VmRun.Step
$l = Get-Layout
Open-App
if (-not $script:app) { Done }
$app = $script:app
Clear-MvOffer $app.Id 3
$b = Get-ServedBridge
Show-MvRows $app.Id
Show-MvFolders $b
Save-MvShot 'before'
Start-MvBatch $name
Invoke-AppButton 'T_BTN_INSTALL_CACHE' 'T_INSTALL'
$vs = @($MvVersions | Where-Object { Test-Path -LiteralPath (Get-RbDirOf $_) })
# done, or an alert shows (a second window, once the confirmation has closed): read at once
Start-Sleep 5
[void]($script:Pressed -and (Wait-Until { (-not @($vs | Where-Object { (Get-Sha (Get-RbOrtOf $_)) -ne $b }).Count) -or (Get-MvWindowCount $app.Id) -gt 1 } 420))
$done = $script:Pressed -and -not @($vs | Where-Object { (Get-Sha (Get-RbOrtOf $_)) -ne $b }).Count
Check $done "every installed version's onnxruntime.dll is the bridge ($($vs -join ', '))"
Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_BTN_REINSTALL_CACHE') 120) 'the app idle again: Reinstall Stems Cache enabled'
Check (Test-CacheOn) 'Stems Cache on (rekordbox_model=1)'
$runs = Stop-MvBatch $name
"admin runs in app.log: $runs"
Show-MvFolders $b
$rows = @($vs | ForEach-Object { Rx (Get-RowText 'T_WIN_ROW_ON' $_) })
if ($vs.Count -ge 2) { $rows += Rx (Get-SummaryText $vs.Count $vs.Count) }
Test-MvRows $app.Id $rows 60
Check ((Get-MvWindowCount $app.Id) -eq 1) 'no alert left open'
Save-MvShot 'after'
if (-not $done) { Show-AppState }
Done
