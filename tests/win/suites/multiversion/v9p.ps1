# V9, as the user: the deleted DLL seen as "7.2.19 · Needs repair" (its reason: the file is
# missing), 7.2.20 On; Put Back Pioneer's File, confirmed, puts the bound original back in
# 7.2.19 only (one batch, one UAC: v9p-files); then 7.2.19 reads Ready to install and 7.2.20 On.
$wfrom = Get-WatcherLogSize
Open-App
if (-not $script:app) { Done }
$app = $script:app
$b = Get-ServedBridge
Test-MvRows $app.Id @((Rx (Get-RowText 'T_WIN_ROW_REPAIR' '7.2.19')), (Rx (Get-RowText 'T_WIN_ROW_ON' '7.2.20')), (Rx (Get-SummaryText 1 2))) 120
$why = Get-RowText 'T_WIN_ROW_R1_MISSING' '7.2.19'
Check (Find-UiText $app.Id (Rx $why) 10) "its reason: $why"
Save-MvShot 'repair'
Start-MvBatch 'v9p'
$label = Get-CoreText 'T_WIN_BTN_RESTORE'
Check (Wait-UiEnabled $app.Id $label 60) "$label is there and enabled"
Check (Invoke-UiNamed $app.Id $label 30 'Button') "$label pressed"
$q = Get-CoreText 'T_WIN_RESTORE_Q'
# one version in scope: the plain message (core/flows_win.c win_confirm_restore names versions only
# for 2 or more, or on a retry); the scope itself is checked by the files afterwards
$msgs = @((Get-CoreText 'T_WIN_RESTORE_MSG'), (Get-RowText 'T_WIN_RESTORE_MSG_VERSION' '7.2.19'))
Check (Find-UiText $app.Id ('^(' + (($msgs | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$') 60) "its question: $($msgs[0]) (or its 7.2.19 form)"
Show-MvUi $app.Id
Save-MvShot 'question'
Check (Invoke-MvDialogButton $app.Id (Rx $q) $label 30) "the question answered: $label"
$back = Wait-Until { (Get-Sha (Get-RbOrtOf '7.2.19')) -eq $MvOrtSha['7.2.19'] } 300
Check $back "7.2.19's onnxruntime.dll is Pioneer's again (e7a702e1...)"
Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_BTN_INSTALL_CACHE') 120) 'the app idle again: Install Stems Cache enabled'
$runs = Stop-MvBatch 'v9p'
"admin runs in app.log: $runs"
Check ((Get-Sha (Get-RbOrtOf '7.2.20')) -eq $b) "7.2.20's is still the bridge (untouched)"
Test-MvRows $app.Id @((Rx (Get-RowText 'T_WIN_ROW_READY' '7.2.19')), (Rx (Get-RowText 'T_WIN_ROW_ON' '7.2.20'))) 60
Check ((Get-MvWindowCount $app.Id) -eq 1) 'no alert left open'
Save-MvShot 'after'
foreach ($x in ((Read-WatcherLog $wfrom) -split "`r?`n")) { if ($x) { "  watcher.log: $x" } }
Done
