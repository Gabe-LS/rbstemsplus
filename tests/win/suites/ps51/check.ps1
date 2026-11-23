# ps51.suite's step: the kit staged in the share's ps51/ (ps51-gate.sh) copied to C:\rbtest\ps51
# and checked by ps51-check.ps1 in a fresh Windows PowerShell 5.1 (System32's powershell.exe, the
# one the owner's PC runs the kit with). First its self-test: the known-bad scripts in selftest/
# must give exactly the findings selftest/expect.txt lists (else exit 1: the gate can't be trusted).
# Then the kit: the check's exit code is the step's (0 pass, 1 failures, 2 not run).
$src = Join-Path $VmRun.Share 'ps51'
$dst = 'C:\rbtest\ps51'
if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
Copy-Item -LiteralPath $src -Destination $dst -Recurse
"staged kit: $(Get-Content -LiteralPath (Join-Path $dst '.commit') -ErrorAction SilentlyContinue)"
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$check = Join-Path $dst 'ps51-check.ps1'

$self = @(& $ps -NoProfile -ExecutionPolicy Bypass -File $check -Root (Join-Path $dst 'selftest'))
$selfRc = $LASTEXITCODE
foreach ($l in $self) { "selftest| $l" }
$expect = @(Get-Content -LiteralPath (Join-Path $dst 'selftest\expect.txt') | Where-Object { $_ -and $_ -notlike '#*' })
$missing = @($expect | Where-Object { $e = $_; -not @($self | Where-Object { $_.StartsWith($e) }).Count })
if ($selfRc -ne 1 -or $missing.Count) {
    "selftest: FAIL (exit $selfRc, want 1; missing: $($missing -join ' / '))"; exit 1
}
"selftest: PASS ($($expect.Count) expected findings)"

& $ps -NoProfile -ExecutionPolicy Bypass -File $check -Root $dst
exit $LASTEXITCODE
