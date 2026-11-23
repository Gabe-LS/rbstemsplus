# AF12 / AR4, Install's half: with the app folder's admin\ worker gone, Reinstall Stems Cache >
# Reinstall stops before any elevation: the source folder (which needs the app's own worker)
# can't be prepared, and with a verified installed-release record the install fails its check
# (core_native_admin.c, NA6, AF9): app.log "admin: the source folder couldn't be prepared"; the
# failure's alert (OK); no UAC prompt (arm-guard's responder found none within its 120 s); no
# worker ran; rekordbox's onnxruntime.dll still the bridge, Stems Cache still on.
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$bridge = ([string](Read-TestPayload).windows.bridge.sha256).ToLowerInvariant()
$from = Get-AppLogSize
Open-App
if (-not $script:app) { Done }
Invoke-CacheInstall
$prompt = $false
$workers = 0
$t = [Diagnostics.Stopwatch]::StartNew()
$label = Get-CoreText 'T_BTN_REINSTALL_CACHE'
$ok = Get-CoreText 'T_OK'
$idle = $false
while ($t.Elapsed.TotalSeconds -lt 120) {
    if (Test-ConsentShown) { $prompt = $true }
    $workers = [Math]::Max($workers, @(Get-WorkerProcesses).Count)
    if ((Read-AppLog $from) -match 'the source folder couldn''t be prepared') {
        if (Invoke-UiNamed $script:app.Id $ok 1 'Button') { 'the failure''s alert answered: OK' }
        if (Wait-UiEnabled $script:app.Id $label 1) { $idle = $true; break }
    }
    Start-Sleep -Milliseconds 300
}
$said = Read-AppLog $from
"the app's log meanwhile:"
foreach ($x in ($said -split "`r?`n")) { if ($x) { "  $x" } }
Check ($said -match "admin: the source folder couldn't be prepared") "app.log: admin: the source folder couldn't be prepared"
Check $idle "the app is idle again: $label enabled"
Check (-not $prompt) 'no UAC prompt showed (consent.exe never ran)'
Check ($workers -eq 0) 'no worker ran'
$r = Read-Consent 'consent' 180
Check ($r.Done -and $r.Answered -eq '' -and $r.Code -eq 3) "arm-guard's responder found no prompt (rbui exit $($r.Code): 3, nothing found)"
Check ((Get-Sha $RbOrt) -eq $bridge) "rekordbox's onnxruntime.dll is still the bridge"
Check (Test-CacheOn) 'Stems Cache is still on (rekordbox_model=1)'
Stop-App
Done
