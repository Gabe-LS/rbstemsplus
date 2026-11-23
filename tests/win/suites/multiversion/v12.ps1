# V12, as the user: Uninstall Completely the way Windows Settings > Apps starts it (the Uninstall
# entry's UninstallString: the stub with --uninstall), with Stems Cache on in both versions. Its
# confirmation's Uninstall, its end's Done (the batch's admin runs read before Done, app.log
# goes with the app), then the app quits and the after-exit removes it. v12-files checks the rest.
$l = Get-Layout
$u = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus' -ErrorAction SilentlyContinue
Check ($u -and $u.UninstallString -eq "`"$($l.Stub)`" --uninstall") "the Apps entry's UninstallString ($($u.UninstallString))"
if (-not $u) { Done }
Stop-App
Start-MvBatch 'v12'
$p = Start-Process -FilePath $l.Stub -ArgumentList '--uninstall' -PassThru
$script:app = $null
Check (Wait-Until { $script:app = Get-AppWindow; $null -ne $script:app } 60) 'the app started by --uninstall (its window)'
if (-not $script:app) { Done }
$app = $script:app
$asked = $false
$limit = Get-WaitSecs 90
$t = [Diagnostics.Stopwatch]::StartNew()
while (-not $asked -and $t.Elapsed.TotalSeconds -lt $limit) {
    if (Invoke-UiNamed $app.Id (Get-CoreText 'T_UNINSTALL') 1 'Button') { $asked = $true; break }
}
Check $asked 'the complete uninstall asks its confirmation: Uninstall'
Save-MvShot 'asked'
$end = Wait-Until { @(Get-MvNames $app.Id | Where-Object { $_ -eq (Get-CoreText 'T_DONE') }).Count } 300
Check $end 'its end shows Done'
Show-MvUi $app.Id
Save-MvShot 'end'
$runs = Stop-MvBatch 'v12'
"admin runs in app.log: $runs"
Check (Invoke-UiNamed $app.Id (Get-CoreText 'T_DONE') 30 'Button') 'Done pressed'
Check (Wait-Until { $app.HasExited } 90) 'the app quits'
Check (Wait-Until { -not (Test-Path -LiteralPath $l.Apps) } 120) "the apps folder is gone ($($l.Apps))"
Done
