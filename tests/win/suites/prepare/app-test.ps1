param([string]$Library = (Join-Path $PSScriptRoot 'app-lib.ps1'))
$ErrorActionPreference = 'Stop'
. $Library

$command = '"C:\Program Files\RB Stems Plus\rbsp-prepare.exe" "--kept-dir-backup" "wrong" "--kept-dir" "C:\App Data\Local\rbstemsplus\kept" "--until" "1893456000"'
if ((Get-PrepareArgument $command '--kept-dir') -cne 'C:\App Data\Local\rbstemsplus\kept' -or
    (Get-PrepareArgument $command '--until') -cne '1893456000' -or
    (Get-PrepareArgument $command '--kept') -or
    (Get-PrepareArgument '"--until" "1893456000"junk' '--until')) {
    throw 'Prepare argv must match complete tokens, preserving spaces inside the value'
}
'PASS Prepare argv'

$log = "18:19  prepare: playlist 42, kept until 2030-01-01`r`n18:20  prepare: playlist 42, kept until Tuesday, 1 January 2030`r`n18:21  prepare: playlist 421, kept until tomorrow`r`n"
if ((Get-PrepareLogDate $log '42') -cne 'Tuesday, 1 January 2030' -or (Get-PrepareLogDate $log '4')) {
    throw 'Prepare dates must use the last log entry for the exact playlist and accept CRLF'
}
'PASS Prepare log date'

$tree = @'
  Text "not a row" pid=34
  DataItem "Playlist 000-sort, -, -, Ready" pid=34
  Text "not another row" pid=34
  DataItem "RBSP PF4, 3, Tuesday, 1 January 2030, Ready" pid=34
'@
$rows = @(Get-PrepareStatusRows $tree)
if ($rows.Count -ne 2 -or $rows[0] -cne 'Playlist 000-sort, -, -, Ready' -or $rows[1] -cne 'RBSP PF4, 3, Tuesday, 1 January 2030, Ready') {
    throw 'Prepare Status must preserve real row order and commas in spoken dates'
}
'PASS Prepare Status rows'

if ((Get-PrepareCapacity 20 20.999 2GB) -cne '21' -or (Get-PrepareCapacity 20 20.999 (2GB - 1)) -or
    (Get-PrepareCapacity 20 21.01 2GB) -or (Get-PrepareCapacity 20 19 2GB)) {
    throw 'Prepare capacity must round up and reserve one GiB, including at the byte boundary'
}
'PASS Prepare capacity'

# In this process only, intercept the launcher to inspect the environment inherited by the stub.
function Start-Process([string]$FilePath) {
    $script:launch = @($FilePath, $env:RBSP_SHELL_TEST)
}
$original = $env:RBSP_SHELL_TEST
try {
    $env:RBSP_SHELL_TEST = 'another-mode'
    Start-PrepareStub 'stub.exe'
    if ($script:launch[0] -cne 'stub.exe' -or $script:launch[1] -cne 'prepare' -or $env:RBSP_SHELL_TEST -cne 'another-mode') {
        throw 'Prepare must enable the TEST tabs for its launch and restore the caller environment'
    }
} finally { $env:RBSP_SHELL_TEST = $original }
'PASS Prepare TEST launch'

$waiting = [pscustomobject]@{ ThreadState = [Diagnostics.ThreadState]::Wait; WaitReason = [Diagnostics.ThreadWaitReason]::Suspended }
$working = [pscustomobject]@{ ThreadState = [Diagnostics.ThreadState]::Running }
$helper = [pscustomobject]@{ Threads = @($waiting); HasExited = $false }
$helper | Add-Member -MemberType ScriptMethod -Name Refresh -Value {}
if (-not (Test-PrepareSuspended $helper)) { throw 'a suspended helper must be recognised' }
$helper.Threads += $working
if (Test-PrepareSuspended $helper) { throw 'a runnable thread means the helper is not fully suspended' }
$helper.Threads = @()
if (Test-PrepareSuspended $helper) { throw 'a helper with no observed threads cannot prove suspension' }
'PASS Prepare pause predicate'
