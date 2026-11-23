# pre-lib.ps1 (dot-source it): an auto plan's preconditions, judged from the facts midi-auto.ps1
# -Do preconditions reads on the PC (kept apart so the Mac tests every rule). A plan checks all of
# them in its first minute and stops with every failed one in one list, not halfway.
#   Get-PlanFindings PLAN FACTS   "OK|FIX|NOTE|BAD text" lines: FIX the plan puts right itself,
#                                 BAD stops it
# FACTS: Session (bool: the session task answers), Ort (Get-OrtState), Probe (bool: C:\rbtest is
# there), AutoAnalysis ('0', '1' or '?'), Model ('ours', 'rekordbox' or 'missing'), App (the
# session task's appstate answer, '' unread), RbRunning (bool), Server (the TEST server our app
# downloads from: 'up URL', 'down URL' or '' when its address isn't in the app's log), CacheDir
# (the Stems Cache folder the bridge last named, '' unknown), SystemDrive (e.g. 'C:'), Bench (the
# kit's own bench helpers still running, -1 unread, missing: not read)
function Get-PlanFindings([string]$plan, $f) {
    $turnsModelOn = $plan -in 'session2', 'prepare'
    if ($f.Session) { 'OK the test account''s session task answers' }
    else { 'BAD the test account''s session task does not answer (signed out, or the session not on the console)' }
    if ($f.Ort -eq 'pioneer') { 'OK rekordbox''s onnxruntime.dll is Pioneer''s' }
    else { "BAD rekordbox's onnxruntime.dll is not Pioneer's ($($f.Ort)): pc-session.sh cleanup first" }
    if ($f.Probe) { 'BAD the research probe''s C:\rbtest is there: pc-session.sh probe remove first' } else { 'OK no research probe (C:\rbtest absent)' }
    switch ($f.AutoAnalysis) {
        '0' { 'OK rekordbox''s Auto Analysis off' }
        '1' { if ($plan -eq 'session2') { 'FIX rekordbox''s Auto Analysis on: the plan turns it off in Preferences (its first step)' }
              else { 'NOTE rekordbox''s Auto Analysis on (only session2 needs it off)' } }
        default { 'BAD rekordbox''s Auto Analysis not read from rekordbox3.settings' }
    }
    switch ($f.Model) {
        'ours' { 'OK the stems model: Demucs v4 (ours)' }
        'rekordbox' {
            if ($turnsModelOn -and $f.Server -notlike 'up *') {
                "BAD the stems model: rekordbox's own, and Demucs v4's install downloads from the Mac's TEST server, which doesn't answer ($(if ($f.Server) { $f.Server.Substring(5) } else { 'its address not in the app''s log' })): start it first" }
            elseif ($turnsModelOn) { 'FIX the stems model: rekordbox''s own; the plan installs Demucs v4 first and removes it at the end' }
            elseif ($plan -eq 'record') { 'BAD the stems model: rekordbox''s own; record''s T2 and hits need Demucs v4 (pc-session.sh run t2-model-on)' }
            else { 'NOTE the stems model: rekordbox''s own' }
        }
        default { 'BAD rekordbox''s model file hdemucs.onnx is missing' }
    }
    if (-not $f.App) { if ($f.Session) { 'BAD our app''s state not read' } }
    elseif ($f.App -match 'appstate: not running') { 'OK our app not running (started when needed)' }
    elseif ($f.App -match 'appstate: running pid \d+; minimized (\w+); tabs (\d+)') {
        $min = $Matches[1] -eq 'True'; $tabs = [int]$Matches[2]
        $what = @(); if ($min) { $what += 'minimized' }; if (-not $tabs) { $what += 'no tabs in UI Automation (its window drawn black?)' }
        if (-not $what.Count) { 'OK our app running, its tabs in UI Automation' }
        elseif ($turnsModelOn) { "FIX our app running, $($what -join ', '): the plan closes it by its Exit and starts it again" }
        elseif (-not $tabs) { "NOTE our app running, $($what -join ', '): this plan doesn't drive it" }
        else { 'OK our app running, minimized' }
    } else { "BAD our app's state not understood: $($f.App)" }
    # a bench helper past its time holds our app's ORT: Stems Cache's install and uninstall then fail
    if ($f.Bench -gt 0) { "BAD $($f.Bench) of the kit's bench helpers still running (they hold our app's ORT): pc-session.sh run benchstop first" }
    elseif ($f.Bench -eq -1 -and $f.Session) { 'BAD the kit''s bench helpers not counted' }
    # the kit and the app's cache stay on the system drive (the owner's backup disk is never chosen)
    if ($f.CacheDir -and -not $f.CacheDir.StartsWith($f.SystemDrive.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        "BAD the Stems Cache folder $($f.CacheDir) is not on the system drive $($f.SystemDrive): the kit never uses another drive (the owner's backups)"
    }
    if ($f.RbRunning) { 'NOTE rekordbox already running: the plan starts from it as it is' } else { 'OK rekordbox closed' }
}
# rekordbox's Analysis Setting dialog, from rekordbox3.settings' text (read only): rekordbox shows
# it on a Load unless askAnalysisSettingWindow is 0 (VM 2026-11-22: absent, shown on the first
# Load). The kit never clicks it. @(ok, text)
function Get-RbAskAnalysisRow([string]$settings) {
    $m = [regex]::Matches($settings, '<VALUE\s+name="askAnalysisSettingWindow"\s+val="([^"]*)"\s*/>')
    $fix = 'with rekordbox closed, set askAnalysisSettingWindow=0 in rekordbox3.settings (or the owner ticks its Don''t ask me again)'
    if (-not $settings) { return $false, "rekordbox's Analysis Setting dialog: rekordbox3.settings not read" }
    if ($m.Count -gt 1) { return $false, "rekordbox's Analysis Setting dialog: askAnalysisSettingWindow is in rekordbox3.settings $($m.Count) times" }
    if ($m.Count -eq 1 -and $m[0].Groups[1].Value -ceq '0') {
        return $true, "rekordbox's profile asks for no Analysis Setting dialog (askAnalysisSettingWindow=0)"
    }
    $value = if ($m.Count) { "=$($m[0].Groups[1].Value)" } else { ' missing' }
    return $false, "rekordbox's profile shows its Analysis Setting dialog on a Load (askAnalysisSettingWindow$value), where the kit stops: $fix"
}
# the plan's verdict line and exit: 0 when no BAD line, else 1
function Get-PlanVerdict([string[]]$findings) {
    $bad = @($findings | Where-Object { $_ -like 'BAD *' } | ForEach-Object { $_.Substring(4) })
    $fix = @($findings | Where-Object { $_ -like 'FIX *' }).Count
    if ($bad.Count) { return @("preconditions: $($bad.Count) failed: $($bad -join ' | ')", 1) }
    return @("preconditions: all met ($fix put right by the plan itself)", 0)
}
