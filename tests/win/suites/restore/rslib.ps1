# rslib.ps1: restore.suite's own helpers, after lib/vmlib.ps1 and worker/rblib.ps1 (the suite's
# third include): bootstrap.ps1's -Restore (release.md section 4: $env:RBSTEMSPLUS_RESTORE='1',
# then the one-liner, in an elevated powershell.exe 5.1), and the state of rekordbox's DLL.

# the served bootstrap.ps1 has -Restore, or the step SKIPs (a TEST build from before it)
function Assert-RestoreServed {
    $b = [IO.File]::ReadAllText((Need 'dist-test/bootstrap.ps1' 'the TEST build'))
    if ($b -notmatch 'RBSTEMSPLUS_RESTORE') { Skip "the served bootstrap.ps1 has no -Restore (RBSTEMSPLUS_RESTORE): a TEST build from before release.md T8's -Restore" }
}
# -Restore through the real one-liner in a child powershell.exe (ConstrainedLanguage with
# -Constrained); its output
function Invoke-Restore([switch]$Constrained) {
    if ($Constrained) { return Invoke-OneLiner $VmRun.Url -Constrained -Env @{ RBSTEMSPLUS_RESTORE = '1' } }
    return Invoke-OneLiner $VmRun.Url -Env @{ RBSTEMSPLUS_RESTORE = '1' }
}
# rekordbox's DLL said by what it is: Pioneer's (prep's hash), the bridge (the payload's), absent,
# or another file
function Get-LiveState($facts, [string]$bridge) {
    $s = Get-Sha $RbOrt
    if (-not $s) { return 'absent' }
    if ($s -eq $facts.PioneerSha) { return 'Pioneer''s' }
    if ($s -eq $bridge) { return 'the bridge' }
    return "another file ($s)"
}
# whether this step runs with the user's full token (High mandatory level, as -Restore checks it)
function Test-HighToken { return [bool]((whoami /groups) -match 'S-1-16-12288') }
