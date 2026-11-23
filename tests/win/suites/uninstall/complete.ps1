# T11 end to end: the stub with --uninstall (what Settings > Apps starts) opens straight into the
# complete uninstall; its confirmation and its end dialog are answered through UI Automation by
# the core's own button texts (Continue for a welcome first, Uninstall, Done). Then the app
# quits and the after-exit copy, from %TEMP%\rbsp-uninstall-<16 hex>\, removes the Windows
# records, stops the watcher, deletes the UsageLogs entry and the apps folder; its temp folder
# goes within a minute (cmd.exe) or at the next logon (RunOnce, disclosed). Defender's detections
# since the uninstall began are the defender step's (as SYSTEM: uninstall/defender.ps1).
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed' }
Stop-App
# when the uninstall began, for the defender step (as SYSTEM: the user's token can't query Defender)
New-Item -ItemType Directory -Force $AuditDir | Out-Null
Set-Content -LiteralPath (Join-Path $AuditDir 'uninstall-since.txt') -Value ([DateTime]::UtcNow.Ticks) -Encoding ASCII
$p = Start-Process -FilePath $l.Stub -ArgumentList '--uninstall' -PassThru
$app = $null
# the app itself (the version folder's exe with a window), never the stub that started it
Check (Wait-Until { $script:app = Get-AppWindow; $null -ne $script:app } 60) 'the app started by --uninstall (its window)'
if (-not $app) { Done }
# one 90 s loop: the welcome's Continue whenever it shows (a first start), until the complete
# uninstall's confirmation shows and is answered with Uninstall
$asked = $false
$limit = Get-WaitSecs 90
$t = [Diagnostics.Stopwatch]::StartNew()
while (-not $asked -and $t.Elapsed.TotalSeconds -lt $limit) {
    if (Invoke-UiNamed $app.Id (Get-CoreText 'T_UNINSTALL') 1 'Button') { $asked = $true; break }
    if (Invoke-UiNamed $app.Id (Get-CoreText 'T_CONTINUE') 1 'Button') { 'the welcome answered: Continue' }
}
Check $asked 'the complete uninstall asks its confirmation: Uninstall'
$done = Invoke-UiNamed $app.Id (Get-CoreText 'T_DONE') 180 'Button'
Check $done 'its end: Done'
if (-not $done) {
    # what shows instead (the window's texts) and the app's log, for the failure's reading
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [Windows.Automation.AutomationElement]
    $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)
    foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
        foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
            if ($e.Current.Name) { "ui: $($e.Current.ControlType.ProgrammaticName) '$($e.Current.Name)'" }
        }
    }
    $alog = Join-Path $l.Support 'Logs\app.log'
    if (Test-Path -LiteralPath $alog) { Get-Content -LiteralPath $alog -Tail 40 | ForEach-Object { "app.log: $_" } }
}
Check (Wait-Until { $app.HasExited } 90) 'the app quits'
$tmp = Join-Path $l.Local 'Temp'
$copy = @(Get-ChildItem -LiteralPath $tmp -Directory -Filter 'rbsp-uninstall-*' -ErrorAction SilentlyContinue)
"after-exit copies: $(($copy | ForEach-Object Name) -join ', ')"
Check (Wait-Until { -not (Test-Path -LiteralPath $l.Apps) } 150) 'the apps folder is gone'
Check (Wait-Until { @(Get-AppProcesses).Count -eq 0 } 10) 'nothing runs from it (the watcher stopped)'
Check (-not (Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus')) 'the Apps & features entry is gone'
Check ($null -eq (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'RB Stems Plus Watcher' -ErrorAction SilentlyContinue)) 'the Run value is gone'
Check (-not (Test-Path -LiteralPath $l.Shortcut)) 'the Start menu shortcut is gone'
Check (-not (Test-Path -LiteralPath (Join-Path $l.Local 'Microsoft\CLR_v4.0\UsageLogs\RB Stems Plus.exe.log'))) 'the UsageLogs entry is gone'
Check (-not (Test-Path -LiteralPath $l.Support)) 'the support folder is gone (REMOVE_OWN_FOLDERS)'
$gone = Wait-Until { @(Get-ChildItem -LiteralPath $tmp -Directory -Filter 'rbsp-uninstall-*' -ErrorAction SilentlyContinue).Count -eq 0 } 90
# the cleanup values (UN13: "RB Stems Plus cleanup <16 hex>", one per temp folder), each said
$cleanup = @(Get-CleanupRunOnce)
foreach ($c in $cleanup) { "RunOnce: $c" }
if (-not $gone -and $cleanup.Count -gt 0) { "DISCLOSED: the temp folder(s) go at the next logon ($($cleanup.Count) RunOnce cleanup value(s))" }
Check ($gone -or $cleanup.Count -gt 0) 'the after-exit temp folder is gone, or set to go at the next logon (RunOnce)'
Done
