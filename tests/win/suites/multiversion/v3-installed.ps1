# V3, as SYSTEM: what the 7.2.20 installer left: both Uninstall entries, 7.2.20's audited
# rekordbox.exe and Pioneer's onnxruntime.dll in its own folder, 7.2.19's folder unchanged (the
# bridge still), its task removed.
$ti = Get-ScheduledTaskInfo -TaskName 'rbmv-install' -ErrorAction SilentlyContinue
"installer task result: $(if ($ti) { $ti.LastTaskResult })"
Unregister-ScheduledTask -TaskName 'rbmv-install' -Confirm:$false -ErrorAction SilentlyContinue
$u = @(Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ItemProperty $_.PSPath } | Where-Object { $_.DisplayName -like 'rekordbox*' })
foreach ($x in $u) { "uninstall entry: $($x.DisplayName) $($x.DisplayVersion) :: $($x.UninstallString)" }
Check ($u.Count -eq 2) "two rekordbox Uninstall entries ($($u.Count))"
Check ((Get-Sha (Get-RbExeOf '7.2.20')) -eq $MvExeSha['7.2.20']) "7.2.20's rekordbox.exe is the audited one"
Check ((Get-Sha (Get-RbOrtOf '7.2.20')) -eq $MvOrtSha['7.2.20']) "7.2.20's onnxruntime.dll is Pioneer's (3e55ab55...)"
Check ((Get-Sha (Get-RbOrtOf '7.2.19')) -eq (Get-ServedBridge)) "7.2.19's onnxruntime.dll is still the bridge"
Check (@(Get-Process -Name rekordbox -ErrorAction SilentlyContinue).Count -eq 0) 'no rekordbox.exe runs'
Done
