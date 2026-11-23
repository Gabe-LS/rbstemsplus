# Round 4 (K2, owner 2026-11-16): a watcher in another session of the same account. As SYSTEM: a
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
$pwFile = Set-TestPassword $user.Split('\')[-1]
Start-TaskAs $user $pwFile $exe '' "rbstems-k2-$($VmRun.Run)"
$ours = { @(Get-CimInstance Win32_Process -Filter "Name = 'rbsp-watch-$arch.exe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $exe }) }
$w = $null
Check (Wait-Until { $script:w = @(& $ours | Where-Object { $_.SessionId -eq 0 }) | Select-Object -First 1; $null -ne $script:w } 60) "a watcher runs from it in session 0$(if ($w) { " (pid $($w.ProcessId))" })"
if ($w) {
    $o = Invoke-CimMethod -InputObject $w -MethodName GetOwner
    Check ("$($o.Domain)\$($o.User)" -ieq $user) "as $user (its owner: $($o.Domain)\$($o.User))"
}
$con = @(& $ours | Where-Object { $_.SessionId -ne 0 })
Check ($con.Count -ge 1) "the console session's watcher runs beside it ($(@($con | ForEach-Object { "pid $($_.ProcessId) session $($_.SessionId)" }) -join ', '))"
Done
