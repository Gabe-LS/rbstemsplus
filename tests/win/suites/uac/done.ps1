# The UAC suites' last step, as SYSTEM: autologon cleared (the run reverts the VM anyway; this
# keeps a --keep run's VM from signing in by itself), responders that never got their prompt ended,
# the arm steps' trace said
foreach ($n in 'consent', 'blockscreen', 'logoff') { Stop-SysDesk $n }
'the arm steps, as they went (arm.trace):'
[void](Show-ArmTrace 400)
Unregister-ScheduledTask -TaskName 'rbsp-ui-restart' -Confirm:$false -ErrorAction SilentlyContinue
Clear-AutoLogon
Check ((Get-ItemProperty $WinlogonKey).AutoAdminLogon -eq '0') 'autologon cleared'
Done
