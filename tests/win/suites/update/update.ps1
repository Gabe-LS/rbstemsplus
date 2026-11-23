# The app's update (release.md section 7; app.md 5.4's order): the notice for the newer release,
# the menu's "Update RB Stems Plus...", its confirmation; the new app's --swap, its first render, then
# the pointers: `current` and DisplayVersion name the new release, the old version folder goes,
# the watcher runs from the new folder; the Start menu shortcut and the stub's path unchanged (a pin
# survives the update). The saved payload is aged two days first, so the start's check is due.
$l = Get-Layout
$p = Get-UpdateRelease 'new'
$old = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$new = $p.windows.app.version
if ($new -eq $old) { Skip "dist-test/new names the installed app ${old}: not an update" }
$stubHash = (Get-FileHash -LiteralPath $l.Stub).Hash
$arch = Get-NativeUpdateArch
Expect ($stubHash -eq (Get-UpdateRelease 'a').windows.app.stub_sha256.$arch) "the fixture's original $arch stub is installed before the swap"
(Get-Item -LiteralPath (Join-Path $l.Support 'payload\payload.json')).LastWriteTime = (Get-Date).AddDays(-2)
[void](Start-Process -FilePath $l.Stub)
Wait-UpdateApp
$notice = (Get-CoreText 'T_UPDATE_NOTICE').Replace('{version}', $new)
Expect (Find-UiText $UpdateApp.Id ([regex]::Escape($notice)) 120) "the notice: $notice"
Use-UpdateUi menu --pid $UpdateApp.Id (Get-MenuText 'T_WIN_MENU_APP') (Get-MenuText 'T_MENU_UPDATE') --timeout 30
$confirm = (Get-CoreText 'T_UPDATE_TO').Replace('{version}', $new)
Use-UpdateUi invoke --pid $UpdateApp.Id --title $confirm (Get-CoreText 'T_UPDATE') --type Button --timeout 30
Expect (Wait-Until { (Get-Content -Raw -LiteralPath $l.Current -ErrorAction SilentlyContinue).Trim() -eq $new } 300) "current names $new"
Check (Wait-Until { @(Get-AppProcesses | Where-Object { $_.ProcessName -eq 'RB Stems Plus' -and $_.Path -like "*\$new\*" }).Count -ge 1 } 60) 'the new app runs from its own folder'
Check (Wait-Until { -not (Test-Path -LiteralPath (Join-Path $l.Apps $old)) } 120) "the old folder $old is gone"
$dv = (Get-ItemProperty -LiteralPath $UpdateEntry).DisplayVersion
Check ($dv -eq $p.payload_version) "DisplayVersion $dv"
Check (Wait-Until { @(Get-AppProcesses | Where-Object { $_.ProcessName -like 'rbsp-watch-*' -and $_.Path -like "*\$new\*" }).Count -ge 1 } 60) 'the watcher runs from the new folder'
$res = Join-Path $l.Support 'update\result'
Check (-not (Test-Path -LiteralPath $res)) 'no failed update recorded'
"the stub $(if ((Get-FileHash -LiteralPath $l.Stub).Hash -eq $stubHash) { 'unchanged' } else { 'replaced (a release that changes the stub)' })"
# whether the release changed the stub or not, the installed one is the new release's own
$want = Join-Path (Join-Path $l.Apps $new) "stub\$arch\RB Stems Plus.exe"
Check ((Test-Path -LiteralPath $want) -and (Get-FileHash -LiteralPath $l.Stub).Hash -eq (Get-FileHash -LiteralPath $want).Hash) "the stub is $new's ($arch) build"
Check (Test-Path -LiteralPath $l.Shortcut) 'the Start menu shortcut is still there'
Wait-UpdateApp
Test-UpdateAbout $UpdateApp $p.payload_version
Stop-App
Done
