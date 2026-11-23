# As SYSTEM, before install-stops: a consent responder that answers No to any prompt within 120 s
# (none is expected: Install must stop before elevating). Its end, read by install-stops, says
# whether a prompt came (answered no) or not (nothing found, rbui's exit 3); it also keeps a wrong
# prompt from waiting for ever.
$s = $VmRun.Step
Trace-Arm $s 'start'
Start-Consent no 120
$out = Join-Path $UiaDir 'out\consent.txt'
$ok = Wait-Until { (Read-Shared $out) -match 'consent: waiting for the prompt' } 60
Check $ok 'rbui waits for a prompt on the secure desktop (as SYSTEM, the console session)'
Trace-Arm $s 'armed'
Done
