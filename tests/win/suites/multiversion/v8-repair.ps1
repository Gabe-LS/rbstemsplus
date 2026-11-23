# V8, after signing back in. The second folder was swapped, and its receipt was never committed.
Set-WorkerHold 0
$b = Get-ServedBridge
$root = $CacheRoot
Check ((Get-Sha (Get-RbOrtOf '7.2.19')) -eq $b -and (Test-Path -LiteralPath (Join-Path $root 'receipts\7.2.19'))) '7.2.19 still has its bridge and receipt'
Check ((Get-Sha (Get-RbOrtOf '7.2.20')) -eq $b -and -not (Test-Path -LiteralPath (Join-Path $root 'receipts\7.2.20'))) '7.2.20 was swapped and has no receipt'
Open-App
if (-not $script:app) { Done }
$app = $script:app
Test-MvRows $app.Id @((Rx (Get-RowText 'T_WIN_ROW_ON' '7.2.19')), (Rx (Get-RowText 'T_WIN_ROW_REPAIR' '7.2.20'))) 120
Save-MvShot 'needs-repair'
$label = Get-CoreText 'T_WIN_BTN_RESTORE'
Check (Wait-UiEnabled $app.Id $label 60) "$label is there and enabled"
Check (Invoke-UiNamed $app.Id $label 30 'Button') "$label pressed"
Check (Invoke-MvDialogButton $app.Id (Rx (Get-CoreText 'T_WIN_RESTORE_Q')) $label 30) 'confirmed Put Back'
Check (Wait-Until { (Get-Sha (Get-RbOrtOf '7.2.20')) -eq $MvOrtSha['7.2.20'] } 120) '7.2.20 is back to its audited original'
Check ((Get-Sha (Get-RbOrtOf '7.2.19')) -eq $b) '7.2.19 remains bridged'
Check (@(Get-ChildItem -LiteralPath (Get-RbDirOf '7.2.20') -Force -Filter '*.rbsp-new').Count -eq 0) 'no stray .rbsp-new in 7.2.20'
Test-MvRows $app.Id @((Rx (Get-RowText 'T_WIN_ROW_ON' '7.2.19')), (Rx (Get-RowText 'T_WIN_ROW_READY' '7.2.20'))) 120
Done
