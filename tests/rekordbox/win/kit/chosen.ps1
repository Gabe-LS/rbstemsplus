# chosen.ps1: run over the test account's own login (coordinator 2026-11-20). After the kit's admin
# install of Stems Cache, the test account's choice set as our app's own install sets it, so the app
# shows Stems Cache as this account's (not "installed from another account"):
#   -Mode set      %LOCALAPPDATA%\rbstemsplus\installed written with exactly CORE_STEP_SET_CHOSEN's
#                  bytes (core/state.c state_chosen_text: "model=%d\ncache=%d\n", LF, no BOM), the
#                  model flag kept as the file has it (state_chosen: "model=1" anywhere), cache=1. The
#                  file as it was is recorded first (%TEMP%\rbst-kit-chosen.txt), once
#   -Mode restore  (cache uninstall or cleanup) the file put back as recorded (removed when there was none), only when it
#                  still holds what the kit wrote; the record removed either way
# Each answers "chosen: ..." lines; the test account's own files only.
param([Parameter(Mandatory = $true)][ValidateSet('set', 'restore')][string]$Mode)
$ErrorActionPreference = 'Stop'
$support = Join-Path $env:LOCALAPPDATA 'rbstemsplus'
$file = Join-Path $support 'installed'
$rec = Join-Path $env:TEMP 'rbst-kit-chosen.txt'
function B64($b) { if ($null -eq $b) { return 'none' }; return [Convert]::ToBase64String($b) }
function Read-Bytes { if (Test-Path -LiteralPath $file -PathType Leaf) { return , [IO.File]::ReadAllBytes($file) }; return $null }

# core/state.c state_chosen_text's bytes for (model, cache)
function Chosen-Bytes([int]$model, [int]$cache) { return , [Text.Encoding]::ASCII.GetBytes("model=$model`ncache=$cache`n") }

if ($MyInvocation.InvocationName -eq '.') { return }   # dot-sourced by the tests: the functions only
$cur = Read-Bytes
if ($Mode -eq 'set') {
    $model = 0
    if ($null -ne $cur -and [Text.Encoding]::UTF8.GetString($cur).Contains('model=1')) { $model = 1 }
    $new = Chosen-Bytes $model 1
    $prior = if (Test-Path -LiteralPath $rec) { ((Get-Content -LiteralPath $rec) | Where-Object { $_ -like 'prior *' } | Select-Object -First 1) } else { "prior $(B64 $cur)" }
    Set-Content -LiteralPath $rec -Value @($prior, "wrote $(B64 $new)") -Encoding ASCII
    [void](New-Item -ItemType Directory -Force -Path $support)
    [IO.File]::WriteAllBytes($file, $new)
    "chosen: the kit set cache=1 (model=$model kept) in $file; it was $(if ($null -eq $cur) { 'absent' } else { "'$([Text.Encoding]::UTF8.GetString($cur) -replace "`n", '\n')'" })"
    exit 0
}
if (-not (Test-Path -LiteralPath $rec)) { 'chosen: nothing the kit set'; exit 0 }
$lines = @(Get-Content -LiteralPath $rec)
$prior = (($lines | Where-Object { $_ -like 'prior *' } | Select-Object -First 1) -replace '^prior ', '')
$wrote = (($lines | Where-Object { $_ -like 'wrote *' } | Select-Object -First 1) -replace '^wrote ', '')
if ((B64 $cur) -ne $wrote) { "chosen: $file changed since the kit set it (our app wrote it): left as it is" }
elseif ($prior -eq 'none') { Remove-Item -LiteralPath $file -Force; "chosen: the kit's $file removed (there was none)" }
else { [IO.File]::WriteAllBytes($file, [Convert]::FromBase64String($prior)); "chosen: $file put back as it was" }
Remove-Item -LiteralPath $rec -Force
exit 0
