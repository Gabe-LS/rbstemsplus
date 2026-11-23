# N4 (the SETTINGS step, native_steps.c native_settings_facts; core/flows.c settings_flow): RB Stems
# Plus > Settings... shows config.ini's numbers in its two fields (planted: max_gb = 25,
# max_days = 45), the location row (This PC) with Change... and Use This PC, and Save, Clear Saved
# Stems, Cancel; a changed field saved goes into config.ini (max_days = 30, max_gb kept); opened
# again, the fields show what was saved; a change cancelled writes nothing and
# isn't shown the next time.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
Stop-App
$cfg = Join-Path $l.Support 'config.ini'
if (Test-Path -LiteralPath $cfg) { "config.ini before: $((Get-Content -Raw -LiteralPath $cfg) -replace "`r?`n", ' | ')" }
[IO.File]::WriteAllText($cfg, "max_gb = 25`r`nmax_days = 45`r`n", (New-Object Text.UTF8Encoding $false))
Start-AppQuiet
if (-not $script:app) { Done }
$app = $script:app
$gbLabel = Get-CoreText 'T_SETTINGS_SIZE_FIELD'
# a statement (its Checks are its output): whether the dialog shows in $script:SettingsOk
function Open-Settings {
    $script:SettingsOk = $false
    Invoke-AppMenu $app 'T_WIN_MENU_APP' 'T_MENU_SETTINGS'
    if (-not $script:MenuOk) { return }
    $script:SettingsOk = Find-UiText $app.Id ('^' + [regex]::Escape($gbLabel) + '$') 60
    Check $script:SettingsOk "the Settings dialog shows ($gbLabel)"
}
Open-Settings
if (-not $script:SettingsOk) { Show-AppState; Stop-App; Done }
$lines = Get-UiLines $app.Id
$lines | Where-Object { $_ -notmatch '^ControlType\.(Pane|Custom|Group|ScrollBar|Thumb)\|' } | ForEach-Object { "ui: $_" }
$edits = Get-UiEdits $app.Id
$vals = @($edits | ForEach-Object { try { $_.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).Current.Value } catch { '?' } })
"the fields: $($vals -join ', ')"
Check ($vals.Count -eq 2 -and $vals[0] -eq '25' -and $vals[1] -eq '45') "its two fields hold config.ini's numbers (25 GB, 45 days)"
$has = { param($id) $t = Get-CoreText $id; [bool](@($lines | Where-Object { $_.Split('|')[1] -eq $t }).Count) }
foreach ($id in 'T_SETTINGS_SIZE_FIELD', 'T_SETTINGS_DAYS_FIELD', 'T_SAVE', 'T_CANCEL', 'T_SETTINGS_CHANGE', 'T_SETTINGS_USE_THIS_MAC', 'T_LOCATION_THIS_MAC') {
    Check (& $has $id) "it shows $(Get-CoreText $id)"
}
$msgText = Get-CoreText 'T_SETTINGS_MSG'
Check (@($lines | Where-Object { $_.Split('|')[1].StartsWith($msgText) }).Count -ge 1) "its message: $msgText"
Check (@($lines | Where-Object { $_.Split('|')[1] -like 'Clear Saved Stems*' }).Count -ge 1) 'its Clear Saved Stems button'
if ($vals.Count -eq 2) {
    try { $edits[1].GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).SetValue('30'); 'days set to 30' } catch { Check $false "the days field couldn't be set: $($_.Exception.Message)" }
    Check (Invoke-UiNamed $app.Id (Get-CoreText 'T_SAVE') 30 'Button') 'Save pressed'
    $saved = Wait-Until { $c = Get-Content -Raw -LiteralPath $cfg -ErrorAction SilentlyContinue; $c -match '(?m)^\s*max_days\s*=\s*30\s*$' } 30
    $c = Get-Content -Raw -LiteralPath $cfg -ErrorAction SilentlyContinue
    "config.ini after Save: $($c -replace "`r?`n", ' | ')"
    Check $saved 'config.ini: max_days = 30'
    Check ($c -match '(?m)^\s*max_gb\s*=\s*25\s*$') 'config.ini: max_gb = 25 kept'
    Open-Settings
    if ($script:SettingsOk) {
        $vals = @(Get-UiEdits $app.Id | ForEach-Object { try { $_.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).Current.Value } catch { '?' } })
        Check ($vals.Count -eq 2 -and $vals[0] -eq '25' -and $vals[1] -eq '30') "opened again, the fields show what was saved ($($vals -join ', '))"
        # VRR5: read back the change before Cancel and check that config.ini wasn't rewritten.
        $before = Get-Content -Raw -LiteralPath $cfg
        $beforeWrite = (Get-Item -LiteralPath $cfg).LastWriteTimeUtc
        $ed = Get-UiEdits $app.Id
        try {
            $value = $ed[1].GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern)
            $value.SetValue('60')
            $v = $value.Current.Value
            Check ($v -eq '60') "days set to 60 before Cancel (read back $v)"
        } catch { Check $false "the days field couldn't be set or read: $($_.Exception.Message)" }
        Check (Invoke-UiNamed $app.Id (Get-CoreText 'T_CANCEL') 30 'Button') 'Cancel pressed'
        Check (Wait-Until { -not (Find-UiText $app.Id ('^' + [regex]::Escape($gbLabel) + '$') 1) } 20) 'the dialog closed'
        Start-Sleep -Seconds 2
        Check ((Get-Content -Raw -LiteralPath $cfg) -ceq $before) 'Cancel wrote nothing (config.ini as before, max_days 30)'
        Check ((Get-Item -LiteralPath $cfg).LastWriteTimeUtc -eq $beforeWrite) 'Cancel left the config.ini write time unchanged'
        Open-Settings
        if ($script:SettingsOk) {
            $vals = @(Get-UiEdits $app.Id | ForEach-Object { try { $_.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).Current.Value } catch { '?' } })
            Check ($vals.Count -eq 2 -and $vals[0] -eq '25' -and $vals[1] -eq '30') "opened once more, the fields show the saved values, not the cancelled 60 ($($vals -join ', '))"
            Check (Invoke-UiNamed $app.Id (Get-CoreText 'T_CANCEL') 30 'Button') 'closed with Cancel'
        }
    }
}
Stop-App
Done
