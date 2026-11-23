# As SYSTEM, last: what the run leaves for the revert said (the VM is reverted anyway): the
# scheduled tasks this suite made gone, rekordbox not running.
Unregister-ScheduledTask -TaskName 'rbmv-install' -Confirm:$false -ErrorAction SilentlyContinue
Check (@(Get-Process -Name rekordbox -ErrorAction SilentlyContinue).Count -eq 0) 'no rekordbox.exe runs'
foreach ($f in @(Get-ChildItem -LiteralPath $MvDir -File -ErrorAction SilentlyContinue)) { "  $($f.Name): $((Get-Content -Raw -LiteralPath $f.FullName).Trim())" }
Done
