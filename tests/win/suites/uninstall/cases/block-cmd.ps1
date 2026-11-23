# Round 2's cases, as SYSTEM (both need it): cmd.exe blocked for the next step, through its
# Image File Execution Options Debugger naming an exe that isn't there (so CreateProcess of
# cmd.exe fails, the after-exit's "cmd can't start"); unblock-cmd.ps1 removes it right after
# (vm-run.sh's own steps don't start cmd.exe). And the user's CLR UsageLogs entry replaced by a
# symbolic link to a file of ours (the user can't make one without Developer Mode): the uninstall
# deletes only a plain file there, so the link stays.
$user = Get-UserLocal
if (-not $user) { Skip 'no logged-in user' }
$log = Join-Path $user 'Microsoft\CLR_v4.0\UsageLogs\RB Stems Plus.exe.log'
Set-UsageLogEntry $log 1   # (unlib: planted when the CLR wrote none)
$target = Join-Path $CaseDir 'usagelogs-target.log'
New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
Copy-Item -LiteralPath $log -Destination $target -Force
Remove-Item -LiteralPath $log -Force
New-Item -ItemType SymbolicLink -Path $log -Target $target | Out-Null
$i = Get-Item -LiteralPath $log -Force
Check ([bool]($i.Attributes -band [IO.FileAttributes]::ReparsePoint)) "the UsageLogs entry is a symbolic link ($log -> $target)"
$k = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\cmd.exe'
New-Item -Path $k -Force | Out-Null
Set-ItemProperty -LiteralPath $k -Name Debugger -Value 'C:\rbtest\no-such-debugger.exe'
# review WA2: a run kept (--keep) that stops before unblock-cmd leaves cmd.exe blocked; this undoes it
'undo, as an administrator, if this run stops before unblock-cmd: reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\cmd.exe" /f'
Check ((Get-ItemProperty -LiteralPath $k).Debugger -eq 'C:\rbtest\no-such-debugger.exe') 'cmd.exe blocked (its IFEO Debugger is an exe that is not there)'
Done
