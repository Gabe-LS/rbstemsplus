# As the logged-in user: not SYSTEM, in the console's session, and no visible window of its own
$who = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$me = Get-Process -Id $PID
"step $($VmRun.Step) as $who in session $($me.SessionId), window handle $($me.MainWindowHandle)"
if ($who -eq 'NT AUTHORITY\SYSTEM') { 'FAIL: ran as SYSTEM'; exit 1 }
if ($me.SessionId -eq 0) { 'FAIL: ran in session 0'; exit 1 }
if ($me.MainWindowHandle -ne [IntPtr]::Zero) {
  Add-Type -Namespace RbspVm -Name Win -MemberDefinition '[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);'
  if ([RbspVm.Win]::IsWindowVisible($me.MainWindowHandle)) { 'FAIL: a visible window'; exit 1 }
}
exit 0
