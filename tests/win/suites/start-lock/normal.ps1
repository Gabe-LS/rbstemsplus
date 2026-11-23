# N12 item 2's other half: after the refused elevated start, a normal start (the user's own
# filtered token) opens the main window as before: the refusal left nothing behind that stops it
# (no one-copy mutex, no busy marker): the window has its RB Stems Plus menu, and the app's log
# has this start.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$me = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
Check (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) 'this step runs with the filtered token (a normal start)'
$log = Join-Path $l.Support 'Logs\app.log'
$from = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
Start-AppQuiet
if (-not $script:app) { Done }
Check ($script:app.MainWindowTitle -ne '' -and -not (Find-UiText $script:app.Id ('^' + [regex]::Escape((Get-CoreText 'T_CANT_OPEN')) + '$') 3)) "the main window, not a refusal ('$($script:app.MainWindowTitle)')"
Check (Wait-Until { (Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).Length -gt $from } 30) "this start is in app.log"
# the main window itself: its RB Stems Plus menu, a MenuItem (an "Already open" dialog has none)
$menu = Get-MenuText 'T_WIN_MENU_APP'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$A = [Windows.Automation.AutomationElement]
$menuCond = New-Object Windows.Automation.AndCondition(
    (New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $script:app.Id)),
    (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::MenuItem)),
    (New-Object Windows.Automation.PropertyCondition($A::NameProperty, $menu)))
Check (Wait-Until { $null -ne $A::RootElement.FindFirst([Windows.Automation.TreeScope]::Descendants, $menuCond) } 30) "its $menu menu (a MenuItem) is there"
Stop-App
Done
