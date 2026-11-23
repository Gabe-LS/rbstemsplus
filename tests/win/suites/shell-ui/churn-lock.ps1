# The healthy unlock (win/shell/Repaint.cs): the app from churn-start, restored and drawn, the
# workstation locked here (LockWorkStation, as Win+L); churn.ps1 as churn-unlock brings the session
# back once (a disconnect and tscon to the console, as SYSTEM), and churn-check.ps1 as unlock-check
# looks at the window after it.
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class RbspLock { [DllImport("user32.dll", SetLastError = true)] public static extern bool LockWorkStation(); }
'@
Assert-ShellDesktop
Expect (Test-Path -LiteralPath $ShellChurnPid) 'churn-start left the app running'
$app = Get-Process -Id ([int](Get-Content -LiteralPath $ShellChurnPid)) -ErrorAction SilentlyContinue
Expect ($null -ne $app) 'the app still runs before the lock'
Test-ShellPicture $app 'before the lock'
Expect ([RbspLock]::LockWorkStation()) 'the workstation is locked'
Done
