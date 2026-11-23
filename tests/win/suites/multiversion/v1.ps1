# V1 (M1): Install Stems Cache with 7.2.19 only, as the user would: the main window's Install
# Stems Cache, its confirmation's Install. One version: today's compact layout (no summary, no
# version rows). The batch's app.log admin runs are counted here, its elevations by v1-files.
# The app is left open for V2 and V3.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed)' }
Open-App
if (-not $script:app) { Done }
Check (Wait-UiEnabled $script:app.Id (Get-CoreText 'T_BTN_INSTALL_CACHE') 120) 'Install Stems Cache is there and enabled (7.2.19 listed in M1)'
$sum = (Get-CoreText 'T_WIN_CACHE_SUMMARY') -replace '\{[nm]\}', '\d+'
$ui = Get-MvUi $script:app.Id
Check (@($ui | Where-Object { ($_ -split '\|')[1] -match "^$sum$" }).Count -eq 0) 'one version: no "Stems Cache on in N of M versions" summary'
Check (@($ui | Where-Object { $_ -match '\|RbRow_' -or ($_ -split '\|')[1] -match ' \u00B7 (On|Ready to install|Unsupported|Needs repair)' }).Count -eq 0) 'one version: no version rows (the compact layout)'
Save-MvShot 'before'
Start-MvBatch 'v1'
Invoke-AppButton 'T_BTN_INSTALL_CACHE' 'T_INSTALL'
Start-Sleep 5
[void]($script:Pressed -and (Wait-Until { (Test-CacheOn) -or (Get-MvWindowCount $script:app.Id) -gt 1 } 420))
$on = Test-CacheOn
Check $on 'Stems Cache on after the install (rekordbox_model=1)'
Check (Wait-UiEnabled $script:app.Id (Get-CoreText 'T_BTN_REINSTALL_CACHE') 60) 'the app idle again: Reinstall Stems Cache enabled'
$runs = Stop-MvBatch 'v1'
"admin runs in app.log: $runs"
$b = Get-ServedBridge
Show-MvFolders $b
Check ((Get-Sha (Get-RbOrtOf '7.2.19')) -eq $b) "rekordbox 7.2.19's onnxruntime.dll is the bridge"
$ui = Get-MvUi $script:app.Id
Check (@($ui | Where-Object { $_ -match '\|RbRow_' -or ($_ -split '\|')[1] -match "^$sum$" }).Count -eq 0) 'after the install, still the compact layout'
Save-MvShot 'after'
if (-not $on) { Show-AppState }
Done
