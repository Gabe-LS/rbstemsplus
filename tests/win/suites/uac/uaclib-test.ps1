# Run with pwsh -NoProfile -File tests/win/suites/uac/uaclib-test.ps1.
$ErrorActionPreference = 'Stop'
if (-not (Get-PSDrive C -ErrorAction SilentlyContinue)) { New-PSDrive -Name C -PSProvider FileSystem -Root $PSScriptRoot | Out-Null }
. "$PSScriptRoot/../lib/vmlib.ps1"
function Get-UserProfile { $null }
. "$PSScriptRoot/uaclib.ps1"

function Get-CoreText([string]$id) { $id }
function Wait-UiEnabled { $true }
function Invoke-UiNamed($id, $label, $seconds, $type) { $label -ne 'T_INSTALL' -or $script:Confirm }
function Show-AppState { 'diagnostic: current UI' }

$script:app = @{ Id = 7 }
$script:Confirm = $false
$lines = @(Invoke-AppButton T_BTN_INSTALL_CACHE T_INSTALL)
if ($script:Pressed -ne $false -or $lines -notcontains 'FAIL its confirmation answered: T_INSTALL' -or
    $lines -notcontains 'diagnostic: current UI' -or $script:VmFails -ne 1) {
    throw 'A missing confirmation must print the failure and leave Pressed false.'
}
$script:Confirm = $true
$lines = @(Invoke-AppButton T_BTN_INSTALL_CACHE T_INSTALL)
if ($script:Pressed -ne $true -or $lines -notcontains 'PASS its confirmation answered: T_INSTALL' -or $script:VmFails -ne 1) {
    throw 'A successful confirmation must print the check and leave Pressed true.'
}
'ok   Invoke-AppButton reports confirmation success and failure'
