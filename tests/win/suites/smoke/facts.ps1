# As SYSTEM: who runs this, the Windows build, Smart App Control's state, and hidden.exe in place
# for the user steps
$who = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$sac = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
"run $($VmRun.Run), step $($VmRun.Step)"
"as $who"
"Windows build $($cv.CurrentBuildNumber).$($cv.UBR) $($cv.DisplayVersion) $env:PROCESSOR_ARCHITECTURE"
"Smart App Control state $sac"
"hidden.exe $(Test-Path 'C:\rbtest\hidden.exe')"
if ($who -ne 'NT AUTHORITY\SYSTEM') { 'FAIL: not SYSTEM'; exit 1 }
exit 0
