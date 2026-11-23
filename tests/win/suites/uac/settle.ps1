# A sign-out case's middle (wc1-settle, wc2-settle, wc3-settle, wc10-settle), as SYSTEM (session 0,
# which a sign-out never ends), after the user step that pressed the action:
#  - the block screen answered: once the sign-out the consent responder started has begun, the
#    block screen responder starts (read for WC1, Cancel for WC2, "Sign out anyway" for WC3 and
#    WC10; arm-wc* start none). One that runs from before the sign-out never found the screen (VM round 4: arm's rbui polled through
#    the whole 30 s screen and saw nothing, while an rbui started during it listed LogonUI's
#    "Blocked shutdown" window at once);
#  - the sign-out settled before the after step runs as the user: Cancel keeps the session; a
#    sign-out that goes on (Sign out anyway, or no block screen: a worker that ended first) ends
#    it, and a user step running meanwhile would end with it (wc2-after did, VM round 3), so this
#    waits for autologon's sign-in then.
# The after step judges the case; this step only says what happened
$case = ($VmRun.Step -split '-')[0]
$action = @{ wc1 = 'read'; wc2 = 'cancel'; wc3 = 'anyway'; wc10 = 'anyway'; v8 = 'anyway' }[$case]
if (-not $action) { Check $false "no case for a step named $($VmRun.Step)"; Done }
$mark = if (Test-Path -LiteralPath $SignoutMark) { New-Object DateTime ([long]((Get-Content -LiteralPath $SignoutMark -TotalCount 1).Trim())), ([DateTimeKind]::Utc) } else { $null }
if (-not $mark) { Skip 'no sign-out mark (the arm step failed: its log)' }
function Test-SysDeskDone([string]$name) { return Test-Path -LiteralPath (Join-Path $UiaDir "out\$name.txt.done") }
# the explorer.exe of a session that was there before the sign-out started
function Get-EarlierExplorer {
    return @(Get-CimInstance Win32_Process -Filter "Name = 'explorer.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.SessionId -ne 0 -and $_.CreationDate.ToUniversalTime() -le $mark })
}
$before = @(Get-EarlierExplorer).Count
"the sign-out mark $mark UTC; explorer.exe of a session from before it: $before"
Check (Wait-Until { Test-SysDeskDone 'consent' } 120) 'the consent responder ended'
$consent = Read-Shared (Join-Path $UiaDir 'out\consent.txt')
if ($consent -notmatch 'consent: then: started') {
    'the consent responder started no sign-out (its output, said by the after step)'
    Done
}
Check (Wait-Until { Test-SysDeskDone 'logoff' } 90) 'the sign-out (ExitWindowsEx as the user) was started'
if ($case -eq 'v8') {
    $from = [long](Read-UacValue 'v8.log')
    Check (Wait-Until { (Read-AppLog $from) -match 'mid-work: the bridge swapped into rekordbox 7\.2\.20, not yet verified' } 15) 'the second swap reached its test hold'
}
Stop-SysDesk 'blockscreen'   # (an earlier case's, still running or not)
Remove-Item -LiteralPath (Join-Path $UiaDir 'out\blockscreen.txt'), (Join-Path $UiaDir 'out\blockscreen.txt.done') -Force -ErrorAction SilentlyContinue
Start-BlockScreen $action 120
Check (Wait-Until { Test-SysDeskDone 'blockscreen' } 200) 'the block screen responder ended'
$bs = Read-Shared (Join-Path $UiaDir 'out\blockscreen.txt')
$png = Join-Path $UiaDir 'out\blockscreen.png'
if (Test-Path -LiteralPath $png) { try { Copy-Item -LiteralPath $png -Destination (Join-Path $VmRun.Results "$case-blockscreen.png") -Force; "its screenshot: $case-blockscreen.png in the run's results" } catch { "its screenshot wasn't copied: $_" } }
"the block screen: $(if ($bs -match 'blockscreen: pressed') { 'seen, pressed' } elseif ($bs -match 'blockscreen: seen') { 'seen' } else { 'not seen' })"
# a sign-out that went on ends the session it came from (its explorer.exe gone); a cancelled one
# leaves it
$ended = ($before -eq 0) -or (Wait-Until { @(Get-EarlierExplorer).Count -eq 0 } 45)
if ($ended) {
    if ($before -eq 0) { 'the earlier session was already gone (it ended before this step started)' }
    'the session ended: the sign-out went on; waiting for autologon to sign in again'
    Check (Wait-UserLogon 240) 'signed in again (autologon), so the after step runs as the user'
} else { 'the session is still there: the sign-out was cancelled' }
if ($case -eq 'v8') {
    Check ($ended -and $bs -match 'blockscreen: pressed') 'sign-out went on through the block screen'
    Set-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -Name ConsentPromptBehaviorAdmin -Value 0 -Type DWord
}
Done
