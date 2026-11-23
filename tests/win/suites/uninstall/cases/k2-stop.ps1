# Round 4 (K2): k2-watcher.ps1's task stopped and removed, as SYSTEM; whatever watcher of the
# user's apps folder still runs in session 0 ended. Then k2-again.ps1 can uninstall.
$l = Get-Layout
# k2-stop ends k2-watcher's task, k2-short-stop k2-watcher-short's
$task = if ($VmRun.Step -like 'k2-short-*') { "rbstems-k2-short-$($VmRun.Run)" } else { "rbstems-k2-$($VmRun.Run)" }
Stop-TaskAs $task
# under the apps folder by its long or its 8.3 path (a watcher started by its short path may be
# named RBSP-W~1.EXE)
$appsShort = if ($l.Apps -and (Test-Path -LiteralPath $l.Apps)) { (New-Object -ComObject Scripting.FileSystemObject).GetFolder($l.Apps).ShortPath } else { '' }
$left = { @(Get-CimInstance Win32_Process | Where-Object {
            $_.SessionId -eq 0 -and $_.ExecutablePath -and $l.Apps -and ($_.ExecutablePath.StartsWith($l.Apps + '\', [StringComparison]::OrdinalIgnoreCase) -or
                ($appsShort -and $_.ExecutablePath.StartsWith($appsShort + '\', [StringComparison]::OrdinalIgnoreCase))) }) }
foreach ($p in & $left) { "ending the watcher in session 0: pid $($p.ProcessId), $($p.ExecutablePath)"; Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
Check (Wait-Until { (& $left).Count -eq 0 } 30) 'no watcher of the apps folder left in session 0'
Check ($null -eq (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue)) "the task $task is gone"
Check (Remove-TestSecrets) "k2-watcher's password file is gone"
Done
