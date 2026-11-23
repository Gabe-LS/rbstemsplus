# Host test for the generated scheduled task. Windows task commands are replaced by in-memory
# stubs. No PowerShell process, scheduled task or VM starts.
param([string]$Runner = (Join-Path $PSScriptRoot 'vm-run.sh'))
$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw -LiteralPath $Runner
if ($source -cnotmatch '(?s)\nlauncher\(\) \{.*?cat <<''EOF''\r?\n(.*?)\r?\nEOF') {
    throw 'Missing launcher body'
}
# Return the normal final exit status to the test harness instead of exiting its PowerShell.
$launcher = [scriptblock]::Create(($Matches[1] -replace '(?m)^exit \(\[int\]\$code\)$', 'return ([int]$code)'))
function Get-CimInstance { @{ UserName = 'LAB\vm-user' } }
function New-Item { }
function Remove-Item { }
function Stop-ScheduledTask { }
function New-ScheduledTaskSettingsSet { }
function Start-ScheduledTask { }
function Unregister-ScheduledTask { }
function Test-Path($Path) { return $Path -ne 'C:\rbtest\hidden.exe' }
function New-ScheduledTaskAction($Execute, $Argument) { @{ Execute = $Execute; Argument = $Argument } }
function New-ScheduledTaskPrincipal($UserId, $LogonType, $RunLevel) { @{ UserId = $UserId; LogonType = $LogonType; RunLevel = $RunLevel } }
function Register-ScheduledTask($TaskName, $Action, $Principal, $Settings, [switch]$Force) {
    $script:action = $Action
    $script:principal = $Principal
}
function Get-Content($Path, $Encoding, $TotalCount) {
    if ($Path -eq "$out.done") { return '0' }
    if ($Encoding) { throw 'The signed job log needs BOM detection' }
}
$mode = 'user-signed-high'
$out = 'out.txt'
$secs = 5
$task = 'test-task'
$env:WINDIR = 'C:\Windows'
$job = 'test-step.ps1'
$status = & $launcher
if ($status -ne 0 -or $action.Execute -ne 'C:\Windows\System32\conhost.exe' -or
    $action.Argument -notmatch "^--headless .*'test-step\.ps1'.*'out\.txt\.done'" -or
    $principal.RunLevel -ne 'Highest' -or $principal.UserId -ne 'LAB\vm-user' -or
    $principal.LogonType -ne 'Interactive') {
    throw 'user-signed-high must use signed conhost and the interactive user Highest principal'
}
'vm-launcher-test: 1 passed, 0 failed'
