# V10's end, as the user: with no rekordbox left, Uninstall Stems Cache (its confirmation's
# Uninstall, then "rekordbox missing" answered Remove) removes Stems Cache's root; v10u-files checks it as SYSTEM with the UAC count.
Open-App
if (-not $script:app) { Done }
$app = $script:app
Show-MvUi $app.Id
Start-MvBatch 'v10u'
Invoke-AppButton 'T_BTN_UNINSTALL_CACHE'
if (Invoke-UiNamed $app.Id (Get-CoreText 'T_UNINSTALL') 10 'Button') { 'its confirmation answered: Uninstall' }
# no rekordbox left: the app asks whether to remove the copy of rekordbox's files it kept
$t = Get-CoreText 'T_ROOT_MISSING_TITLE'
if (Find-UiText $app.Id (Rx $t) 30) {
    Show-MvUi $app.Id
    Save-MvShot 'missing'
    Check (Invoke-MvDialogButton $app.Id (Rx $t) (Get-CoreText 'T_REMOVE') 20) "'$t' answered $(Get-CoreText 'T_REMOVE')"
}
if (Invoke-UiNamed $app.Id (Get-CoreText 'T_UNINSTALL') 5 'Button') { 'a confirmation after it answered: Uninstall' }
Check ($script:Pressed -and (Wait-Until { -not (Test-CacheOn) } 300)) 'Stems Cache off (rekordbox_model no longer 1)'
[void](Wait-Until { (Get-MvWindowCount $app.Id) -eq 1 -and (Wait-UiEnabled $app.Id (Get-CoreText 'T_BTN_INSTALL_CACHE') 1) } 60)
$runs = Stop-MvBatch 'v10u'
"admin runs in app.log: $runs"
Show-MvUi $app.Id
Save-MvShot 'after'
Done
