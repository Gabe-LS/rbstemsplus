# The app's update (release.md section 7; app.md 5.4's order): the notice for the newer release,
# the menu's "Update RB Stems Plus...", its confirmation; the new app's --swap, its first render, then
# the pointers: `current` and DisplayVersion name the new release, the old version folder goes,
# the watcher runs from the new folder; the Start menu shortcut and the stub's path unchanged (a pin
# survives the update). The saved payload is aged two days first, so the start's check is due.
$l = Get-Layout
$p = Get-Content -Raw -LiteralPath (Share 'dist-test/new/payload.json') | ConvertFrom-Json
$old = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$new = $p.windows.app.version
if ($new -eq $old) { Skip "dist-test/new names the installed app ${old}: not an update" }
$stubHash = (Get-FileHash -LiteralPath $l.Stub).Hash
(Get-Item -LiteralPath (Join-Path $l.Support 'payload\payload.json')).LastWriteTime = (Get-Date).AddDays(-2)
[void](Start-Process -FilePath $l.Stub -PassThru)
$proc = $null
[void](Wait-Until { $script:proc = @(Get-AppProcesses | Where-Object { $_.ProcessName -eq 'RB Stems Plus' -and $_.MainWindowHandle -ne 0 }) | Select-Object -First 1; $null -ne $script:proc } 90)
if (-not $proc) { Check $false 'the app opened'; Done }
$notice = (Get-CoreText 'T_UPDATE_NOTICE').Replace('{version}', $new)
Check (Find-UiText $proc.Id ([regex]::Escape($notice.Substring(0, [Math]::Min(40, $notice.Length)))) 120) "the notice: $notice"
Check (Invoke-UiNamed $proc.Id (Get-CoreText 'T_APP_NAME') 20 'MenuItem') 'the app menu opened'
Check (Invoke-UiNamed $proc.Id (Get-CoreText 'T_MENU_UPDATE') 20 'MenuItem') 'Update RB Stems Plus... chosen'
# its confirmation (core/flows.c: title T_UPDATE_TO, buttons T_UPDATE and Cancel): Update
$confirmed = Invoke-UiNamed $proc.Id (Get-CoreText 'T_UPDATE') 30 'Button'
Check $confirmed 'its confirmation answered: Update'
if (-not $confirmed) { Done }
Check (Wait-Until { (Get-Content -Raw -LiteralPath $l.Current -ErrorAction SilentlyContinue).Trim() -eq $new } 300) "current names $new"
Check (Wait-Until { @(Get-AppProcesses | Where-Object { $_.ProcessName -eq 'RB Stems Plus' -and $_.Path -like "*\$new\*" }).Count -ge 1 } 60) 'the new app runs from its own folder'
Check (Wait-Until { -not (Test-Path -LiteralPath (Join-Path $l.Apps $old)) } 120) "the old folder $old is gone"
$dv = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus').DisplayVersion
Check ($dv -eq $p.payload_version) "DisplayVersion $dv"
Check (Wait-Until { @(Get-AppProcesses | Where-Object { $_.ProcessName -like 'rbsp-watch-*' -and $_.Path -like "*\$new\*" }).Count -ge 1 } 60) 'the watcher runs from the new folder'
$res = Join-Path $l.Support 'update\result'
Check (-not (Test-Path -LiteralPath $res)) 'no failed update recorded'
"the stub $(if ((Get-FileHash -LiteralPath $l.Stub).Hash -eq $stubHash) { 'unchanged' } else { 'replaced (a release that changes the stub)' })"
# whether the release changed the stub or not, the installed one is the new release's own
$arch = if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
$want = Join-Path (Join-Path $l.Apps $new) "stub\$arch\RB Stems Plus.exe"
Check ((Test-Path -LiteralPath $want) -and (Get-FileHash -LiteralPath $l.Stub).Hash -eq (Get-FileHash -LiteralPath $want).Hash) "the stub is $new's ($arch) build"
Check (Test-Path -LiteralPath $l.Shortcut) 'the Start menu shortcut is still there'
Stop-App
Done
