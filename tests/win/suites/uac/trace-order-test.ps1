# Host-only unit cases for the trace verdict. No Windows dependencies.
param([string]$Implementation = (Join-Path $PSScriptRoot 'trace-order.ps1'))
$ErrorActionPreference = 'Stop'
. $Implementation
$script:failed = 0
$block = '00:00:00.001 session-trace 20 block'
$answer = '00:00:00.002 session-trace 30 answer'
function Assert-Trace($name, $case, $app, $screen, [bool]$ok) {
    $errors = @(Get-SessionTraceErrors $case $app $screen)
    if (($errors.Count -eq 0) -ne $ok) {
        $script:failed++
        "FAIL $name ($errors)"
    } else { "PASS $name" }
}
Assert-Trace 'cancel after the answer' wc2 "10 query-veto`n31 end-false" "$block`n$answer" $true
Assert-Trace 'early FALSE even with a later cancellation' wc2 "10 query-veto`n25 end-false`n31 end-false" "$block`n$answer" $false
Assert-Trace 'a single FALSE before Cancel' wc2 "10 query-veto`n25 end-false" "$block`n$answer" $false
Assert-Trace 'TRUE after forced sign-out' wc3 "10 query-veto`n31 end-true" "$block`n$answer" $true
Assert-Trace 'missing TRUE after forced sign-out' wc10 '10 query-veto' "$block`n$answer" $false
Assert-Trace 'block screen before the veto' wc3 "21 query-veto`n31 end-true" "$block`n$answer" $false
Assert-Trace 'automatic sign-out without a window left to receive ENDSESSION' wc1 '10 query-veto' $block $true
Assert-Trace 'automatic sign-out never cancels' wc1 "10 query-veto`n25 end-false" $block $false
"session trace: 8 checks, $failed failed"
if ($failed) { exit 1 }
