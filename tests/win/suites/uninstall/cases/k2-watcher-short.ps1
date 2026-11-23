# Round 5, k2-short (K2F1, review follow-up): as k2-watcher.ps1 below, but the task starts the
# watcher by its 8.3 short path (C:\Users\GABRIE~1\...\RBSP-W~1.EXE), so its image path in the
# system's list is spelled with aliases; the uninstall must still see it under the apps folder.
# SKIPs when the volume makes no 8.3 names. k2.ps1, k2-stop.ps1 and k2-again.ps1 follow.
# k2-watcher.ps1's: a watcher in another session of the same account. As SYSTEM: a
# scheduled task that runs whether the user is logged on or not (LogonType Password: the account's
# password is empty in the snapshot, and Windows refuses S4U for it too, so the account gets a
# random one here, Set-TestPassword; k2-stop deletes its file) starts the
# installed version folder's rbsp-watch-<arch>.exe as the logged-in user in session 0, with the
# user's limited token (as their own processes have: openable from their console session). The
# console session's watcher (install-4's) keeps running beside it. k2.ps1 then uninstalls;
# k2-stop.ps1 ends this task.
$l = Get-Layout
if (-not $l.Local -or -not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (install-4 failed or was skipped)' }
$user = (Get-CimInstance Win32_ComputerSystem).UserName
$ver = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$arch = if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
$exe = Join-Path $l.Apps "$ver\rbsp-watch-$arch.exe"
Check (Test-Path -LiteralPath $exe -PathType Leaf) "the installed watcher is there ($exe)"
$short = (New-Object -ComObject Scripting.FileSystemObject).GetFile($exe).ShortPath
"its 8.3 path: $short"
if ($short -notmatch '~') { Skip "8.3 names are off on this volume (the short path is the long one: $short)" }
$pwFile = Set-TestPassword $user.Split('\')[-1]
Start-TaskAs $user $pwFile $short '' "rbstems-k2-short-$($VmRun.Run)"
# by path only: started by its short path, its image name may be RBSP-W~1.EXE
$ours = { @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and ($_.ExecutablePath -ieq $exe -or $_.ExecutablePath -ieq $short) }) }
$w = $null
Check (Wait-Until { $script:w = @(& $ours | Where-Object { $_.SessionId -eq 0 }) | Select-Object -First 1; $null -ne $script:w } 60) "a watcher runs from it in session 0$(if ($w) { " (pid $($w.ProcessId), $($w.Name), $($w.ExecutablePath))" })"
if ($w) {
    $o = Invoke-CimMethod -InputObject $w -MethodName GetOwner
    Check ("$($o.Domain)\$($o.User)" -ieq $user) "as $user (its owner: $($o.Domain)\$($o.User))"
}
$con = @(& $ours | Where-Object { $_.SessionId -ne 0 })
Check ($con.Count -ge 1) "the console session's watcher runs beside it ($(@($con | ForEach-Object { "pid $($_.ProcessId) session $($_.SessionId)" }) -join ', '))"
Done
