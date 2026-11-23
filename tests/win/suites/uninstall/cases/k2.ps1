# Round 4 (K2): the uninstall as Settings > Apps starts it (--uninstall) while a watcher of the same
# account runs in another session (k2-watcher.ps1's, session 0). Its stop event is Local\ to that
# session, so it is left running (review UN7: ending it would skip its act_lock writes), and after
# its 60 s the after-exit keeps the app: AX_LEFT (3); the files, the Apps & features entry and the
# Start menu shortcut kept, the Run value gone; its log (the debug output) says the watchers
# couldn't all be checked and names the apps folder a program still runs from; the RunOnce value
# for its temp folder is written before the kept dialog (T_WIN_UNINSTALL_KEPT, clean_at_logon: a
# restart with the dialog open still cleans up), which shows, in the user's session, and is
# closed here; then cmd.exe deletes the temp folder. k2-stop.ps1 then ends that watcher and
# k2-again.ps1 uninstalls again.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (install-4 failed or was skipped)' }
$me = (Get-Process -Id $PID).SessionId
# k2-watcher's (it checked, as SYSTEM, that it runs from the apps folder). From this session its
# path may be unreadable: a process of the user's other logon session (the task's) doesn't grant
# even PROCESS_QUERY_LIMITED_INFORMATION here (VM round 2: error 5), which is the uninstall's own
# view of it too, so it is said, not checked
Add-VmNative
# by its long name, or its 8.3 one when k2-watcher-short started it by its short path
$w0 = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq 0 -and ($_.Name -like 'rbsp-watch-*' -or $_.Name -like 'RBSP-W~*') }) | Select-Object -First 1
Check ($null -ne $w0) "the watcher in session 0 (k2-watcher's) runs, seen from session $me$(if ($w0) { ": pid $($w0.Id)" })"
if ($w0) {
    $p0 = [RbspVm.Native]::ImagePath($w0.Id)
    "its path from session ${me}: $(if ($p0) { $p0 } else { "unreadable (OpenProcess error $([RbspVm.Native]::OpenError($w0.Id)))" })"
}
if (-not $w0) { Done }
Start-DebugLog
$dbg = $script:DebugOn
Stop-App
Start-AfterExitWatch
Start-AppQuiet @('--uninstall')
if (-not $script:app) { Stop-DebugLog; Done }
Complete-Uninstall $script:app
$ax = Wait-AfterExit 60
Check ($null -ne $ax) "the after-exit copy runs$(if ($ax) { " (pid $($ax.Id), $($ax.Path))" })"
if (-not $ax) { Stop-DebugLog; Done }
$tmpDir = [IO.Path]::GetDirectoryName($ax.Path)
$name = [IO.Path]::GetFileName($tmpDir)
$kept = Get-CoreText 'T_WIN_UNINSTALL_KEPT'
# by its first sentence: a MessageBox's text can reach UI Automation with "Apps & features"'s
# ampersand read as a mnemonic (VM round 3: the whole text was never matched in 150 s)
$first = ($kept -split '(?<=\.) ')[0]
$shown = Find-UiText $ax.Id ([regex]::Escape($first)) 150
Check $shown "after its 60 s, the kept dialog shows: $kept"
if (-not $shown) {
    # what UI Automation sees of the after-exit copy, and any window named as the app
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [Windows.Automation.AutomationElement]
    foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, [Windows.Automation.Condition]::TrueCondition)) {
        if ($w.Current.ProcessId -ne $ax.Id -and $w.Current.Name -ne (Get-CoreText 'T_APP_NAME')) { continue }
        "ui: window '$($w.Current.Name)' pid $($w.Current.ProcessId) class $($w.Current.ClassName)"
        foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) { "ui:   $($e.Current.ControlType.ProgrammaticName) '$($e.Current.Name)'" }
    }
    "the after-exit copy's session: $((Get-Process -Id $ax.Id -ErrorAction SilentlyContinue).SessionId), this step's: $me"
}
# while it shows: the RunOnce value (written before it), and what it keeps
$cmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
$want = "RB Stems Plus cleanup $($name.Substring(15)) = `"$cmd`" /d /q /s /c `"rd /s /q `"$tmpDir`"`""
$values = @(Get-CleanupRunOnce)
foreach ($v in $values) { "RunOnce: $v" }
Check (@($values | Where-Object { $_ -ieq $want }).Count -eq 1) "the RunOnce value for its temp folder is there before the dialog is closed: $want"
$ver = if (Test-Path -LiteralPath $l.Current) { (Get-Content -Raw -LiteralPath $l.Current).Trim() } else { '' }
Check ((Test-Path -LiteralPath $l.Stub) -and $ver -and (Test-Path -LiteralPath (Join-Path $l.Apps "$ver\RB Stems Plus.exe"))) "the app's files kept (the stub, current = $ver, its RB Stems Plus.exe)"
Check (Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus') 'the Apps & features entry kept'
Check (Test-Path -LiteralPath $l.Shortcut) 'the Start menu shortcut kept'
Check ($null -eq (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'RB Stems Plus Watcher' -ErrorAction SilentlyContinue)) 'the Run value is gone (removed first)'
Check ($null -ne (Get-Process -Id $w0.Id -ErrorAction SilentlyContinue)) "the watcher in session 0 (pid $($w0.Id)) still runs: left, not ended"
# closed as its OK would close it; the after-exit then ends
if ($shown) { [void]$ax.CloseMainWindow() }
Check ($ax.WaitForExit(60000)) 'the after-exit ends once the dialog is closed'
if ($ax.HasExited) { Check ($ax.ExitCode -eq 3) "its exit code: AX_LEFT 3 ($($ax.ExitCode))" }
Check (Wait-Until { -not (Test-Path -LiteralPath $tmpDir) } 90) "its temp folder goes (cmd.exe): $tmpDir"
$lines = @(Get-DebugLinesOf $ax.Id)
Stop-DebugLog
foreach ($x in $lines) { "debug: $x" }
if ($dbg) {
    Check ($lines -contains "after-exit: the watchers couldn't all be checked") "its log: the watchers couldn't all be checked (the one in session 0 left running)"
    $still = @($lines | Where-Object { $_.StartsWith('after-exit: a program still runs from ') -and $_.EndsWith('\Programs\RB Stems Plus: its files, the Apps & features entry and the UsageLogs entry kept; uninstall again once it is closed') })
    Check ($still.Count -eq 1 -and $still[0].IndexOf($l.Apps, [StringComparison]::OrdinalIgnoreCase) -ge 0) "its log names the folder the watcher runs from ($($l.Apps)): a program still runs from it, all kept"
    Check ($lines -contains 'after-exit: telling the user to restart Windows, then uninstall again') 'its log: the kept dialog requested'
    Check (@($lines -match 'couldn.t be set to go').Count -eq 0) 'its log: the temp folder''s cleanup set (no "couldn''t be set to go")'
}
Done
