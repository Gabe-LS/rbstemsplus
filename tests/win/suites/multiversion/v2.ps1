# V2: what must never ask for UAC: the app's start (its refresh), Settings opened and cancelled,
# Manage again (no Status view on Windows yet), with Stems Cache on in 7.2.19. app.log has no admin
# run in this batch; v2-uac counts its elevations (0). The app is left open for V3.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed' }
Start-MvBatch 'v2'
Stop-App
Start-AppQuiet
if (-not $script:app) { Done }
$app = $script:app
Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_BTN_REINSTALL_CACHE') 120) 'the app started idle: Reinstall Stems Cache enabled'
Invoke-AppMenu $app 'T_WIN_MENU_APP' 'T_MENU_SETTINGS'
$gb = Get-CoreText 'T_SETTINGS_SIZE_FIELD'
$shown = $script:MenuOk -and (Find-UiText $app.Id (Rx $gb) 60)
Check $shown "the Settings dialog shows ($gb)"
Save-MvShot 'settings'
if ($shown) { Check (Invoke-UiNamed $app.Id (Get-CoreText 'T_CANCEL') 30 'Button') 'Settings cancelled' }
# Status: this Windows build has no Status view (View holds Manage alone: run 3's screenshot), so
# the "open Status" half of V2 is not applicable here
Start-Sleep 5
$runs = Stop-MvBatch 'v2'
Check ($runs -eq 0) "no admin run in app.log (got $runs)"
Done
