# Part A1/B1: tests/win/bootstrap/bootstrap.tests.ps1 in Windows PowerShell 5.1, unelevated, from
# the staged repo (its one-liner child in ConstrainedLanguage included): exit 0, "0 failed"
$suite = Need 'repo/tests/win/bootstrap/bootstrap.tests.ps1' 'vm-run.sh stage'
$eap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$out = & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $suite 2>&1 | Out-String
$rc = $LASTEXITCODE
$ErrorActionPreference = $eap
$out
Check ($PSVersionTable.PSVersion.Major -eq 5) "Windows PowerShell 5.1 ($($PSVersionTable.PSVersion))"
Check ($rc -eq 0) "bootstrap.tests.ps1 exits 0 (got $rc)"
Check ($out -match 'bootstrap\.ps1: \d+ checks, 0 failed') 'its summary: 0 failed'
Done
