# As SYSTEM, before the user step that causes the prompt or the sign-out: the secure-desktop
# responders that step needs, by this step's name (rbui through sysdesk, from SYSTEM tasks, so they
# outlive this step; vmlib's Start-Consent and Start-BlockScreen):
#   arm-no*       UAC's prompt answered No (UP4 refused)
#   arm-yes*      answered Yes
#   arm-details*  answered Yes after "Show more details" (the program's location: J7's "nothing
#                 elevated from the app folder")
#   arm-wc*       Yes 15 s after the prompt shows (the user step has ended by then, and checks
#                 the chain held meanwhile), then the sign-out as soon as the worker runs; the
#                 block screen is answered by the case's settle step (uac/settle.ps1: read for
#                 WC1+WC9, Cancel for WC2, "Sign out anyway" for WC3/WC4 and WC10)
# Each waits up to 300 s for its prompt (the block screen: 240 s). Each stage's time goes to
# out\arm.trace (Trace-Arm), which the consent check and the done step say: a stuck arm step's
# own output is lost with prlctl's (it prints nothing until the step ends)
$s = $VmRun.Step
Trace-Arm $s "start, its includes took $([int]$script:VmClock.Elapsed.TotalSeconds) s"
$me = Get-CimInstance Win32_Process -Filter "ProcessId = $PID" -ErrorAction SilentlyContinue
$wrap = if ($me) { Get-CimInstance Win32_Process -Filter "ProcessId = $($me.ParentProcessId)" -ErrorAction SilentlyContinue }
Trace-Arm $s "its wrapper started $(if ($wrap) { $wrap.CreationDate.ToString('HH:mm:ss.fff') }), this step $(if ($me) { $me.CreationDate.ToString('HH:mm:ss.fff') })"
switch -Regex ($s) {
    '^arm-no' { Start-Consent no; break }
    '^arm-yes' { Start-Consent yes; break }
    '^arm-details' { Start-Consent yes -Details; break }
    # the block screen responder is started by the case's settle step once the sign-out began (one
    # running from before it never found the screen: VM round 4)
    '^arm-wc' { Start-Consent yes -Delay 15 -ThenLogoff; break }
    default { Check $false "no responders for a step named $s"; Done }
}
Trace-Arm $s 'responders started'
$out = Join-Path $UiaDir 'out\consent.txt'
$ok = Wait-Until { (Read-Shared $out) -match 'consent: waiting for the prompt' } 60
Check $ok 'rbui waits for the prompt on the secure desktop (as SYSTEM, the console session)'
Trace-Arm $s "consent responder waiting: $ok"
[void](Wait-SysDesk 'consent' 0)   # what it said so far
Trace-Arm $s 'armed'
Done
