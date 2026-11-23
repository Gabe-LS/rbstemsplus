# Before WC10, as the user: Stems Cache on from an elevated install of ours, as the case starts
# from it ("Install Stems Cache > Yes, then Uninstall RB Stems Plus Completely"), with exactly one
# UAC prompt, answered by arm-yes*'s rbui. After WC3's forced sign-out the cache may be on (WC3's
# worker finished: Reinstall, forced), or off with the bridge left in rekordbox (WC3's worker cut
# off mid-work, after its swap, before its record: the usual case). Then Install only opts in
# (core flows.c install_body's opt_in_only: no elevation, no prompt), so a forced Reinstall
# follows, which puts the recorded install back. Each action is waited for to its end (its
# "state: " line, install_body's last); an action with no elevated run fails the step
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (an earlier step failed: its log)' }
if (@(Get-Rekordbox).Count) { Check (Close-Rekordbox 200) 'rekordbox closed first' }
Open-App
if (-not $script:app) { Done }
# one install action pressed, then waited for to its end: a statement, the log since its click in
# $script:ActionLog ($null when nothing was pressed)
function Invoke-ReadyAction([switch]$Reinstall) {
    $script:ActionLog = $null
    $from = Get-AppLogSize
    if ($Reinstall) { Invoke-AppButton 'T_BTN_REINSTALL_CACHE' 'T_REINSTALL' } else { Invoke-CacheInstall }
    if (-not $script:Pressed) { return }
    $ended = Wait-Until { (Read-AppLog $from) -match '(?m)\bstate: ' } 420
    Check $ended "the action ended (its 'state: ' line in the app log)"
    $script:ActionLog = Read-AppLog $from
}
if (Test-CacheOn) { Invoke-ReadyAction -Reinstall } else { Invoke-ReadyAction }
if ($null -eq $script:ActionLog) { Done }
if ($script:ActionLog -match 'Stems Cache: already in rekordbox') {
    'the install only opted in to the bridge WC3 left in rekordbox (no elevation, no record): a forced Reinstall puts the recorded install back'
    Invoke-ReadyAction -Reinstall
    if ($null -eq $script:ActionLog) { Done }
}
Check (Test-CacheOn) 'Stems Cache on (rekordbox_model=1 in config.ini)'
if ($script:ActionLog -match 'admin run \(') { Test-ConsentResult yes }
else { Check $false 'Stems Cache: no elevated install ran (no "admin run (" line since the last click)' }
Done
