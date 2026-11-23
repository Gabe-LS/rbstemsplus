# The shell's --uninstall with another copy open (win/shell/LaunchArgs.cs, Program.cs Refuse()):
# the new start shows its TaskDialog (window title "RB Stems Plus", T_APP_NAME; main instruction
# "Already open", T_ALREADY_OPEN) and ends once it is dismissed; the open copy lives on; no
# uninstall starts, nothing changes
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed' }
[void](Start-Process -FilePath $l.Stub -PassThru)
$first = $null
Check (Wait-Until { $script:first = Get-AppWindow; $null -ne $script:first } 90) 'the app is open'
if (-not $first) { Done }
$before = Get-AppTree
$ids = @(Get-AppProcesses | ForEach-Object Id)
$exe = $first.Path
[void](Start-Process -FilePath $l.Stub -ArgumentList '--uninstall' -PassThru)
$title = Get-CoreText 'T_APP_NAME'
$second = $null
$seen = Wait-Until {
    $script:second = @(Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Where-Object {
            $p = $null; try { $p = $_.Path } catch { }
            $ids -notcontains $_.Id -and $p -and $p.Equals($exe, [StringComparison]::OrdinalIgnoreCase) -and $_.MainWindowTitle -eq $title }) | Select-Object -First 1
    $null -ne $script:second
} 60
Check $seen "the --uninstall start (a new process of the current version) shows a window titled $title"
if (-not $second) { Stop-App; Done }
$said = Get-CoreText 'T_ALREADY_OPEN'
Check (Find-UiText $second.Id ('^' + [regex]::Escape($said) + '$') 20) "its dialog says $said"
# its OK, as the user dismisses it: a TaskDialog with only an OK button ignores WM_CLOSE
# (CloseMainWindow), having no Cancel; WM_COMMAND IDOK is its OK button's click (whatever the
# button's name in the system's language)
Add-Type -Namespace RbspT -Name Msg -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);'
$second.Refresh()
$hwnd = $second.MainWindowHandle
Check ($hwnd -ne [IntPtr]::Zero -and [RbspT.Msg]::PostMessageW($hwnd, 0x0111, [IntPtr]1, [IntPtr]::Zero)) "its OK pressed (WM_COMMAND IDOK to $hwnd)"
$ended = $second.WaitForExit(15000)
if (-not $ended) {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [Windows.Automation.AutomationElement]
    $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $second.Id)
    foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
        foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
            "ui: $($e.Current.ControlType.ProgrammaticName) '$($e.Current.Name)'"
        }
    }
}
Check $ended 'dismissed, the second start ends'
Check ($null -ne (Get-Process -Id $first.Id -ErrorAction SilentlyContinue)) "the open copy (pid $($first.Id)) lives on"
Check ((Get-AppTree) -ceq $before) 'no uninstall: nothing changed'
Stop-App
Done
