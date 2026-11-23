# The sign-out cases' second half (app.md 5.9 WC1-WC10), as the user, by this step's name:
#   wc1-after   WC1+WC9: the sign-out came while the worker ran (the app vetoed it: its log's
#               "logoff: a step must finish"), nothing was pressed on the block screen, and the
#               sign-out went on by itself once the worker ended (signed in again: autologon);
#               the worker finished (rekordbox's onnxruntime.dll is Pioneer's again)
#   wc2-after   WC2: the veto, then Cancel on the block screen: no new sign-in, the same app still
#               open 60 s later (no quit came later); the worker finished. wc2-settle (SYSTEM)
#               ran before it, so a sign-out that went on has ended and autologon signed in again
#   wc3-after   WC3/WC4: the veto, then "Sign out anyway" with the worker held mid-work (the
#               bridge swapped in, not yet verified): signed in again; rekordbox's
#               onnxruntime.dll is whole, Pioneer's or the bridge (never anything else)
#   wc10-after  WC10: Uninstall All, a veto, then "Sign out anyway" with the worker held mid-work
#               (Pioneer's file put back, our folder still there): signed in again; rekordbox's
#               stems model intact, or recovered by the next start's check_resume when the app
#               is still installed; no half-written model (.hdemucs.new, .hdemucs.restore);
#               rekordbox's onnxruntime.dll whole
# session-trace.ps1 separately checks the query, block screen and WM_ENDSESSION.
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
$case = ($VmRun.Step -split '-')[0]
Set-WorkerHold 0   # (press.ps1's: no later worker is held)
$oldPid = Read-UacValue "$case.pid"
$from = [long](Read-UacValue "$case.log")
if (-not $oldPid) { Skip "the $case step left no pid (it failed: its log)" }
$mark = if (Test-Path -LiteralPath $SignoutMark) { New-Object DateTime ([long]((Get-Content -LiteralPath $SignoutMark -TotalCount 1).Trim())), ([DateTimeKind]::Utc) } else { $null }
$me = (Get-Process -Id $PID).SessionId
$explorer = @(Get-Process -Name explorer -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $me }) | Select-Object -First 1
$signedIn = $explorer -and $mark -and $explorer.StartTime.ToUniversalTime() -gt $mark
"the sign-out mark $mark UTC; this session's explorer.exe started $(if ($explorer) { $explorer.StartTime.ToUniversalTime() }) UTC"

Test-ConsentResult yes -ThenLogoff
$lo = Wait-SysDesk 'logoff' 5
Check ($lo.Text -match 'sysdesk: exit 0') 'ExitWindowsEx(EWX_LOGOFF) ran as the user (sysdesk --as user)'
$bs = Wait-SysDesk 'blockscreen' $(if ($case -eq 'wc2') { 240 } else { 10 })
$seen = $bs.Text -match 'blockscreen: seen'
"the block screen: $(if ($seen) { 'seen' } else { 'not seen' })"
if ($seen) { Check ($bs.Code -eq 0) "the block screen names the app and rbui's answer went through (rbui exit $($bs.Code))" }

$log = Read-AppLog $from
$veto = $log -match 'logoff: a step must finish: the session waits for it'
"the app's log since the action ($(if ($log) { 'its lines follow' } else { 'none: gone with the uninstall, or nothing written' })):"
foreach ($x in ($log -split "`r?`n")) { if ($x) { "  $x" } }
# the TEST worker's holds (press.ps1): before its work (WC1, WC2), or mid-work (WC3, WC10)
$preHeld = $log -match 'test build: held \d+ s before the work'
$midHeld = $log -match 'test build: held \d+ s mid-work'
"the worker $(if ($midHeld) { 'was held mid-work (its line above)' } elseif ($preHeld) { 'was held before its work (its line above)' } else { 'was not held: no "test build: held" line (a release worker, the hold not read, or a mid-work hold its run never reached)' })"
# WC3, WC10: the session's end must land once the worker's work has begun changing files (its
# mid-work hold's line) and while it is still held: a session that ended before the work began
# (no such line), or a hold that ended at or before "Sign out anyway" (nothing was cut), leaves
# WHAT testing nothing. CUT says what a cut-off run left undone. A statement
function Test-MidWork([string]$what, [string]$cut) {
    $C = $case.ToUpper()
    if (-not $midHeld) {
        if ($log) { Missing "${C}: the session ended before the worker's work began (no 'test build: held N s mid-work' line since the action), so $what tests nothing" }
        else { Missing "${C}: no app log line since the action, so whether the session ended mid-work can't be told: $what tests nothing" }
        return
    }
    $over = [regex]::Match($log, '(?m)^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\s+test build: mid-work hold over')
    if (-not $over.Success) { "the session ended while the worker was held mid-work: $cut"; return }
    # app.log's times are local, whole seconds; rbui's are UTC, time of day only
    $press = [regex]::Match($bs.Text, '(?m)^(\d\d:\d\d:\d\d\.\d{3}) blockscreen: pressed')
    if (-not $press.Success) { Missing "${C}: the worker's mid-work hold ended and no 'Sign out anyway' press time was read: whether anything was cut can't be told, $what tests nothing"; return }
    $endUtc = [DateTime]::ParseExact($over.Groups[1].Value, 'yyyy-MM-dd HH:mm:ss', [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AssumeLocal).ToUniversalTime()
    $pressTod = [TimeSpan]::ParseExact($press.Groups[1].Value, 'hh\:mm\:ss\.fff', [Globalization.CultureInfo]::InvariantCulture)
    $d = ($pressTod - $endUtc.TimeOfDay).TotalSeconds
    if ($d -gt 43200) { $d -= 86400 } elseif ($d -le -43200) { $d += 86400 }   # across midnight (UTC)
    "the worker's mid-work hold ended at $($endUtc.ToString('HH:mm:ss')) UTC (app.log: whole seconds); 'Sign out anyway' pressed at $($press.Groups[1].Value) UTC"
    if ($d -ge -1) { Missing "${C}: the worker's hold ended before 'Sign out anyway' (within 1 s): nothing was cut, $what tests nothing" }
    else { "the session's end came while the worker was held mid-work; its hold ended after the press and it finished" }
}
$live = Get-Sha $RbOrt
$bridge = ([string](Read-TestPayload).windows.bridge.sha256).ToLowerInvariant()

switch ($case) {
    'wc1' {
        if (-not $veto) { Missing 'WC1: the sign-out came when no step ran (no veto in the log: the worker ended first)' }
        else { Check $veto 'WC1: the app vetoed the sign-out while the worker ran' }
        Check ($bs.Text -notmatch 'blockscreen: pressed') 'nothing pressed on the block screen'
        Check $signedIn 'the sign-out went on by itself: signed in again after it (autologon)'
        Check ($live -eq $f.PioneerSha) "the worker finished: rekordbox's onnxruntime.dll is Pioneer's again"
    }
    'wc2' {
        if (-not $veto) { Missing 'WC2: no veto in the log (the worker ended first): Cancel had nothing to cancel'; Done }
        # a veto whose step ended before Windows showed its block screen (an unheld worker's run
        # here is under a second): nothing to press, and the sign-out went on (VM round 3)
        if (-not $seen) { Missing 'WC2: the vetoed step ended before the block screen showed: no Cancel to press'; Done }
        Check ($bs.Text -match 'blockscreen: pressed') 'Cancel pressed on the block screen'
        Check (-not $signedIn) 'no new sign-in: the sign-out was cancelled'
        "waiting 60 s: no quit may come later"
        Start-Sleep -Seconds ([Math]::Min(60, (Get-WaitSecs 60)))
        $p = Get-Process -Id ([int]$oldPid) -ErrorAction SilentlyContinue
        Check ([bool]$p -and -not $p.HasExited) "the app (pid $oldPid) is still open 60 s after Cancel"
        Check ($null -ne (Get-AppWindow)) 'with its window'
        Check ($live -eq $f.PioneerSha) "the worker finished: rekordbox's onnxruntime.dll is Pioneer's again"
    }
    'wc3' {
        if (-not $veto) { Missing 'WC3: no veto in the log (the worker ended first): "Sign out anyway" had nothing to override' }
        if ($veto -and -not $seen) { Missing 'WC3: the vetoed step ended before the block screen showed: "Sign out anyway" had nothing to override' }
        else { Check ($bs.Text -match 'blockscreen: pressed') '"Sign out anyway" pressed on the block screen' }
        Check $signedIn 'signed in again after it (autologon)'
        Test-MidWork '"whole" and the bounded wait (5.9 item 3)' 'cut off after the bridge''s swap, before its verification and record'
        Check ($live -eq $f.PioneerSha -or $live -eq $bridge) "rekordbox's onnxruntime.dll is whole: Pioneer's or the bridge ($live)"
        if ($log -match 'the session ends anyway') { "WC4: the bounded wait ran out (the log says the session ends anyway)" }
    }
    'wc10' {
        if (-not $veto -and $log) { Missing 'WC10: no veto in the log (the worker ended first)' }
        if (-not $log -and (Test-Path -LiteralPath (Join-Path $l.Support 'Logs\app.log'))) { Missing 'WC10: the app log has no line since the action though the uninstall left it (its writes failed): no veto or hold to judge by' }
        if (-not $seen) { Missing 'WC10: the block screen never showed: "Sign out anyway" had nothing to override' }
        else { Check ($bs.Text -match 'blockscreen: pressed') '"Sign out anyway" pressed on the block screen' }
        Check $signedIn 'signed in again after it (autologon)'
        Check ($live -eq $f.PioneerSha -or $live -eq $bridge) "rekordbox's onnxruntime.dll is whole: Pioneer's or the bridge ($live)"
        if ($log -match 'not put back: the app is quitting') { "the log: rekordbox's model not put back: the app is quitting" }
        Test-MidWork '"model intact"' "cut off in the cache worker, before Uninstall All's model step"
        $atPress = Read-UacValue 'wc10.model'   # (press.ps1's)
        if (-not $f.ModelSha) { Missing "WC10's model case: rekordbox has no stems model here (no hdemucs.onnx at prep)" }
        elseif ($atPress -eq $f.ModelSha) { Missing "WC10's model case: rekordbox had Pioneer's own model at the press (Stems Plus not installed), so Uninstall All had no model to put back: 'model intact' and 'no half-written model' test nothing" }
        else {
            $model = Join-Path $RbModelDir 'hdemucs.onnx'
            $now = Get-Sha $model
            if ($now -ne $f.ModelSha -and (Test-Path -LiteralPath $l.Stub)) {
                "the model isn't as it was ($now): the next start's check_resume"
                Start-AppQuiet
                Check (Wait-Until { (Get-Sha $model) -eq $f.ModelSha } 120) "rekordbox's stems model recovered by the next start's check_resume"
                Stop-App
            } else { Check ($now -eq $f.ModelSha) "rekordbox's stems model is intact ($now)" }
            $half = @(Get-ChildItem -LiteralPath $RbModelDir -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '.hdemucs.*' })
            Check ($half.Count -eq 0) "no half-written model left ($(@($half | ForEach-Object Name) -join ', '))"
        }
    }
    default { Check $false "no case for a step named $($VmRun.Step)" }
}
Done
