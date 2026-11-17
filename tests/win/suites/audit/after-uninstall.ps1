# The trace audit after the complete uninstall (release.md section 8, T11): everything as in the
# "before" snapshot (taken before the install), every rekordbox version folder by hash and their
# list included. The one
# disclosed exception: when cmd.exe couldn't delete an after-exit temp folder, its RunOnce value
# ("RB Stems Plus cleanup <16 hex>", one per folder) and that folder stay until the next logon
# (afterexit.h), and the audit says so, each value found.
$before = Read-Snapshot 'before'
if (-not $before) { Skip 'no "before" snapshot: run audit-before first in this run' }
$now = Get-Snapshot
$cleanup = @(Get-CleanupRunOnce)
foreach ($c in $cleanup) { "DISCLOSED: RunOnce $c" }
foreach ($k in $before.Keys) {
    $b = $before[$k]; $n = [string]$now[$k]
    if ($k -eq 'temp copies' -and $cleanup.Count -gt 0 -and $n) {
        "DISCLOSED: the temp folder(s) $n stay until the next logon ($($cleanup.Count) RunOnce cleanup value(s))"
        continue
    }
    if ($k -eq $CleanupKey -and $n -ne '-') { continue }   # said above
    Check ($n -ceq $b) "$k as before ($b)$(if ($n -cne $b) { ", now $n" })"
}
Done
