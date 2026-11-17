# Part A2/B2 and SAC: the real one-liner against the served TEST build. With Smart App Control On
# (state 1) it refuses with the SAC line and installs nothing; off or in Evaluation (0, 2) it
# installs: the layout (the version folder, the stub of this PC's architecture, `current`), the
# updater's records, the Run value, the Apps & features entry, the Start menu shortcut with the
# AppUserModelID; no Mark of the Web on anything installed (MOTW); the watcher and the app
# started through the stub, its welcome answered and recorded. The app is closed again at the end
# (the watcher keeps running).
$l = Get-Layout
$sac = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
"Smart App Control state: $sac"
$t = [Diagnostics.Stopwatch]::StartNew()
$out = Invoke-OneLiner $VmRun.Url
"one-liner ($([int]$t.Elapsed.TotalSeconds) s): $out"
if ($sac -eq 1) {
    Check ($out -ceq (Get-BootText 'SacOn')) 'Smart App Control On: the SAC line alone (every release refuses alike)'
    Check (-not (Test-Path -LiteralPath $l.Stub)) 'and nothing installed'
    Done
}
Check ($out -match 'RB Stems Plus [0-9]+\.[0-9]+\.[0-9]+ is installed\.\s*$') "Smart App Control $(if ($sac -eq 2) { 'in Evaluation' } else { 'off' }): installed"
$ver = if (Test-Path -LiteralPath $l.Current) { (Get-Content -Raw -LiteralPath $l.Current).Trim() } else { '' }
$vdir = Join-Path $l.Apps $ver
Check ($ver -match '^[0-9]+\.[0-9]+\.[0-9]+$' -and (Test-Path -LiteralPath (Join-Path $vdir 'RB Stems Plus.exe'))) "current names a version folder with the app ($ver)"
$arch = if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
$want = Join-Path $vdir "stub\$arch\RB Stems Plus.exe"
Check ((Test-Path -LiteralPath $l.Stub) -and (Get-FileHash -LiteralPath $l.Stub).Hash -eq (Get-FileHash -LiteralPath $want).Hash) "the stub is this PC's ($arch) build"
foreach ($r in @('installed-release\payload.json', 'installed-release\payload.json.sig', 'payload\payload.json', 'payload-version')) {
    Check (Test-Path -LiteralPath (Join-Path $l.Support $r)) "the updater's record $r"
}
$run = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'RB Stems Plus Watcher' -ErrorAction SilentlyContinue).'RB Stems Plus Watcher'
Check ($run -eq "`"$($l.Stub)`" --watcher") "the Run value: the stub with --watcher ($run)"
$u = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus' -ErrorAction SilentlyContinue
Check ($u -and $u.DisplayName -eq 'RB Stems Plus' -and $u.Publisher -eq 'Gabe-LS' -and $u.UninstallString -eq "`"$($l.Stub)`" --uninstall") 'the Apps & features entry'
"DisplayVersion: $($u.DisplayVersion)"
$aumid = $null
if (Test-Path -LiteralPath $l.Shortcut) {
    $sh = New-Object -ComObject Shell.Application
    $aumid = $sh.NameSpace((Split-Path $l.Shortcut)).ParseName((Split-Path -Leaf $l.Shortcut)).ExtendedProperty('System.AppUserModel.ID')
}
Check ($aumid -eq 'io.github.rbstemsplus.app') "the Start menu shortcut with the AppUserModelID ($aumid)"
$motw = @(Get-ChildItem -LiteralPath $l.Apps -Recurse -File -Force | Where-Object { Get-Motw $_.FullName })
Check ($motw.Count -eq 0) "no Mark of the Web on the installed files ($($motw.Count) carry one)"
Check (Wait-Until { @(Get-AppProcesses | Where-Object { $_.ProcessName -like 'rbsp-watch-*' }).Count -ge 1 } 30) 'the watcher runs (started through the stub)'
# the first start asks the welcome before anything else (the update check included): answered,
# so the later steps (update.suite's checks, uninstall.suite's --uninstall) see a started app
Complete-Welcome 90
Stop-App
Done
