# Run after each case's existing responder and outcome checks. No UI action here.
$case = ($VmRun.Step -split '-')[0]
$path = Read-UacValue "$case.trace"
if (-not $path) { Skip "no $case app trace path from the press step" }
$app = Read-Shared $path
$screen = Read-Shared (Join-Path $UiaDir 'out\blockscreen.txt')
if (-not $app) { Skip 'no shell session trace (no TEST hook or no delivered query)' }
$app
$screen -split "`r?`n" | Where-Object { $_ -match 'session-trace' }
if ($app -notmatch 'query-veto' -or $screen -notmatch 'session-trace \d+ block') {
    Skip 'the veto and visible block screen did not both occur'
}
$errors = @(Get-SessionTraceErrors $case $app $screen)
foreach ($errorLine in $errors) { Check $false $errorLine }
Check ($errors.Count -eq 0) "${case}: the veto preceded the block screen and the end messages follow the observed choice"
Remove-Item -LiteralPath $path -Force
Done
