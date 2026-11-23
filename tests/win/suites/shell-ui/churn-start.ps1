# The owner's PC black window (2026-11-19), first part: the TEST app started with the repaint trace
# on (no trigger) and minimized as RB-KIT's front step leaves it, through UI Automation's window
# pattern. churn.ps1 then moves the session; churn-check.ps1 (as churn-mid and churn-check) looks at
# the window.
Assert-ShellDesktop
$l = Get-Layout
Expect (Test-Path -LiteralPath $l.Stub) 'the shell fixture is installed'
Expect (-not (Get-AppWindow)) 'the app is closed before the session churn'
Remove-Item -LiteralPath $ShellChurnPid, (Join-Path $ShellWork 'churn.mark') -Force -ErrorAction SilentlyContinue
Start-ShellRepaint ''
$app = $script:ShellApp
Add-ShellShot
Test-ShellPicture $app 'at the start'
$wp = $null
$w = [Windows.Automation.AutomationElement]::FromHandle($app.MainWindowHandle)
Expect ($w.TryGetCurrentPattern([Windows.Automation.WindowPattern]::Pattern, [ref]$wp)) 'the window has a UI Automation window pattern'
$wp.SetWindowVisualState([Windows.Automation.WindowVisualState]::Minimized)
Expect (Wait-Until { [RbspShellShot]::IsIconic($app.MainWindowHandle) } 10) 'the window is minimized'
Set-Content -LiteralPath $ShellChurnPid -Value $app.Id -Encoding ASCII
Check ((Get-Content -LiteralPath $ShellChurnPid) -eq [string]$app.Id) "the app (pid $($app.Id), session $($app.SessionId)) is left running for the churn"
Done
