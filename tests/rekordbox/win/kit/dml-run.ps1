# dml-run.ps1: one helper run of the DirectML bench (midi-auto.ps1 -Do dmlbench | dmlstall), started
# by the session task in the test account's desktop session (where the app runs its helper).
# Reads %TEMP%\rbst\bench\spec-NAME.txt (the helper's arguments, one a line; the helper is always the
# kit's bench\rbsp-prepare.exe) and writes NAME.out (stdout), NAME.err (stderr), then NAME.exit (its
# exit code) last.
param([Parameter(Mandatory = $true)][ValidateSet('cpu', 'dml', 'stall')][string]$Name)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:TEMP 'rbst\bench'
$exe = Join-Path $PSScriptRoot 'bench\rbsp-prepare.exe'
$exit = Join-Path $dir "$Name.exit"
try {
    $a = @(Get-Content -LiteralPath (Join-Path $dir "spec-$Name.txt") -Encoding UTF8)
    $p = Start-Process -FilePath $exe -ArgumentList ($a | ForEach-Object { '"' + $_ + '"' }) -NoNewWindow -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $dir "$Name.out") -RedirectStandardError (Join-Path $dir "$Name.err")
    Set-Content -LiteralPath $exit -Value $p.ExitCode
} catch {
    Add-Content -LiteralPath (Join-Path $dir "$Name.err") -Value ('dml-run: ' + $_.Exception.Message)
    Set-Content -LiteralPath $exit -Value 99
}
