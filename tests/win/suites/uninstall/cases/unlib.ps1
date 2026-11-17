# unlib.ps1: uninstall-cases.suite's own helpers, after lib/vmlib.ps1 (the suite's second include):
# the complete uninstall started from the app's menu or with --uninstall, its dialogs answered by
# the core's texts (as uninstall/complete.ps1), and the state it must leave. The case files the
# plant steps make live in the user's profile (C:\Users\<user>\rbtest-*), outside everything the
# uninstall deletes.
$CaseDir = if ($UnProfile = Get-UserProfile) { Join-Path $UnProfile 'rbtest-cases' } else { 'C:\rbtest\cases' }   # (no user: a step's own Skip says so)

# a UI element (by name and control type) of process pid invoked, retried while it isn't there or
# isn't enabled yet (the core enables a menu item once it is idle), within secs
function Invoke-UiWhenReady([int]$procId, [string]$name, [string]$type, [int]$secs) {
    $limit = Get-WaitSecs $secs
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($t.Elapsed.TotalSeconds -lt $limit) {
        try { if (Invoke-UiNamed $procId $name 1 $type) { return $true } } catch { Start-Sleep -Milliseconds 500 }
    }
    return $false
}
# the app started through the stub (an installed, already welcomed app: a quiet start), with
# args: a statement (its Check is its output), the app's process left in $script:app ($null: none)
function Start-AppQuiet([string[]]$argv = @()) {
    $l = Get-Layout
    if ($argv.Count) { [void](Start-Process -FilePath $l.Stub -ArgumentList $argv -PassThru) } else { [void](Start-Process -FilePath $l.Stub -PassThru) }
    $script:app = $null
    Check (Wait-Until { $script:app = Get-AppWindow; $null -ne $script:app } 90) "the app opened through the stub $($argv -join ' ')(its window)"
}
# the menu's uninstall: RB Stems Plus > Uninstall RB Stems Plus... (core/menus.def M_APP_UNINSTALL,
# its event uninstall_all), each item by the core's text
function Invoke-MenuUninstall($app) {
    $top = Get-CoreText 'T_APP_NAME'
    $item = Get-CoreText 'T_MENU_UNINSTALL'
    $ok = $false
    $limit = Get-WaitSecs 90
    $t = [Diagnostics.Stopwatch]::StartNew()
    while (-not $ok -and $t.Elapsed.TotalSeconds -lt $limit) {
        if (Invoke-UiWhenReady $app.Id $top 'MenuItem' 5) { $ok = Invoke-UiWhenReady $app.Id $item 'MenuItem' 5 }
    }
    Check $ok "the menu's $top > $item chosen"
}
# the complete uninstall's dialogs, once it was asked for: the welcome's Continue whenever it shows,
# the confirmation's Uninstall, the end's Done; then the app quits
function Complete-Uninstall($app) {
    $asked = $false
    $limit = Get-WaitSecs 90
    $t = [Diagnostics.Stopwatch]::StartNew()
    while (-not $asked -and $t.Elapsed.TotalSeconds -lt $limit) {
        if (Invoke-UiNamed $app.Id (Get-CoreText 'T_UNINSTALL') 1 'Button') { $asked = $true; break }
        if (Invoke-UiNamed $app.Id (Get-CoreText 'T_CONTINUE') 1 'Button') { 'the welcome answered: Continue' }
    }
    Check $asked 'the complete uninstall asks its confirmation: Uninstall'
    Check (Invoke-UiNamed $app.Id (Get-CoreText 'T_DONE') 180 'Button') 'its end: Done'
    Check (Wait-Until { $app.HasExited } 90) 'the app quits'
}
# what every complete uninstall leaves gone (uninstall/complete.ps1's checks, the UsageLogs entry
# left to each case)
function Test-UninstallGone {
    $l = Get-Layout
    Check (Wait-Until { -not (Test-Path -LiteralPath $l.Apps) } 150) 'the apps folder is gone'
    Check (Wait-Until { @(Get-AppProcesses).Count -eq 0 } 10) 'nothing runs from it (the watcher stopped)'
    Check (-not (Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus')) 'the Apps & features entry is gone'
    Check ($null -eq (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'RB Stems Plus Watcher' -ErrorAction SilentlyContinue)) 'the Run value is gone'
    Check (-not (Test-Path -LiteralPath $l.Shortcut)) 'the Start menu shortcut is gone'
    Check (-not (Test-Path -LiteralPath $l.Support)) 'the support folder is gone'
}
# the after-exit temp folders (rbsp-uninstall-<16 hex>) in folder tmp, by name
function Get-AfterExitCopies([string]$tmp) {
    return @(Get-ChildItem -LiteralPath $tmp -Directory -Force -Filter 'rbsp-uninstall-*' -ErrorAction SilentlyContinue | ForEach-Object Name)
}
# whether path is a reparse point (a junction or a symbolic link), not followed
function Test-Reparse([string]$path) {
    $i = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    return [bool]($i -and ($i.Attributes -band [IO.FileAttributes]::ReparsePoint))
}
