# V3, as SYSTEM: the real 7.2.20 installer (the owner's download, staged in snapshot (b)) started
# with /S as the logged-in user with the full token (a scheduled task, RunLevel Highest: no
# consent prompt; the ruling: /S, and OK on its language picker, which v3-lang answers), while RB
# Stems Plus is open and idle. Its start time is saved for v3-observe.
$user = (Get-CimInstance Win32_ComputerSystem).UserName
$exe = "C:\Users\$($user.Split('\')[-1])\Downloads\Install_rekordbox_x64_7_2_20.exe"
Check ((Get-Sha $exe) -eq $MvInstaller) "the staged installer's hash ($exe)"
$app = @(Get-AppProcesses | Where-Object { $_.ProcessName -eq 'RB Stems Plus' -and $_.Path -notlike '*\admin\*' })
Check ($app.Count -ge 1) "RB Stems Plus is open (pid $(@($app | ForEach-Object Id) -join ', '))"
$a = New-ScheduledTaskAction -Execute $exe -Argument '/S'
$p = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 40)
Register-ScheduledTask -TaskName 'rbmv-install' -Action $a -Principal $p -Settings $s -Force | Out-Null
Start-ScheduledTask -TaskName 'rbmv-install'
Save-MvValue 'v3.started' ([string][DateTime]::UtcNow.Ticks)
Check $true "started $exe /S at $(Get-Date -Format HH:mm:ss)"
Done
