# UP1: carried first, then fetch a newer app without installing it. Uninstall the old app
# while the newer app is still offered. update/install-carried starts a fresh lap for the swap.
$l = Get-Layout
$carried = Get-UpdateRelease 'carried'
$new = Get-UpdateRelease 'new'
$oldApp = $carried.windows.app.version
$shown = $carried.payload_version
Expect ([version]$oldApp -lt [version]$shown -and [version]$shown -lt [version]$new.payload_version -and
    [version]$oldApp -lt [version]$new.windows.app.version) 'the fixtures distinguish the old app, carried shown version and newer release'
Expect ((Get-Content -Raw -LiteralPath $l.Current).Trim() -eq $oldApp) "current names the carried app $oldApp"
$saved = Join-Path $l.Support 'payload\payload.json'
(Get-Item -LiteralPath $saved).LastWriteTime = (Get-Date).AddDays(-2)
[void](Start-Process -FilePath $l.Stub)
Wait-UpdateApp
$notice = (Get-CoreText 'T_UPDATE_NOTICE').Replace('{version}', $new.windows.app.version)
Expect (Find-UiText $UpdateApp.Id ([regex]::Escape($notice)) 120) "the app offered $($new.windows.app.version) after fetching it"
$fetched = Get-Content -Raw -LiteralPath $saved | ConvertFrom-Json
Check ($fetched.payload_version -eq $new.payload_version) "the saved payload is now $($new.payload_version)"
Test-UpdateAbout $UpdateApp $shown

Use-UpdateUi menu --pid $UpdateApp.Id (Get-MenuText 'T_WIN_MENU_APP') (Get-MenuText 'T_MENU_UNINSTALL') --timeout 30
$confirm = Get-CoreText 'T_UNINSTALL_ALL_Q'
Use-UpdateUi wait-window --pid $UpdateApp.Id --title $confirm --timeout 60
Check ((Get-ItemProperty -LiteralPath $UpdateEntry).DisplayVersion -eq $shown) "Installed apps still shows $shown at the uninstall confirmation"
Check ((Get-Content -Raw -LiteralPath $l.Current).Trim() -eq $oldApp) "the uninstall is for the old app $oldApp"
Use-UpdateUi invoke --pid $UpdateApp.Id --title $confirm (Get-CoreText 'T_UNINSTALL') --type Button --timeout 30
Use-UpdateUi invoke --pid $UpdateApp.Id --title (Get-CoreText 'T_UNINSTALLED_TITLE') (Get-CoreText 'T_DONE') --type Button --timeout 180
Check ($UpdateApp.WaitForExit((Get-WaitSecs 30) * 1000)) 'the app closed after uninstall'
Check (Wait-Until { -not (Test-Path -LiteralPath $l.Apps) -and -not (Test-Path -LiteralPath $UpdateEntry) } 100) 'the old install and its Installed apps entry are gone'
Check (-not (Test-Path -LiteralPath (Join-Path $l.Support 'shown-version'))) 'the old shown-version record is gone'
Done
