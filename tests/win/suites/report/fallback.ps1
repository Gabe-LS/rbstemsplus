# N4: Create Report's fallback (report_build.c write_zip): the Desktop refuses new files (this
# account denied FILE_ADD_FILE on it, as a OneDrive or policy refusal does), so the zip goes next to
# the logs (the support folder's Logs\reports\), app.log says why, the message names that place
# (masked), the zip is the same report (its parts, no names); Done closes it. The Desktop's ACL is
# put back after.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$names = Get-RpNames
$desk = [Environment]::GetFolderPath('Desktop')
$me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$ErrorActionPreference = 'Continue'   # (a native command's stderr is no error here)
$r = & icacls.exe $desk /deny "${me}:(WD)" 2>&1 | Out-String
$ErrorActionPreference = 'Stop'
"icacls /deny ${me}:(WD): $($r.Trim())"
$probe = Join-Path $desk ('rbtest-probe-' + [Guid]::NewGuid().ToString('N') + '.txt')
$denied = $false
try { [IO.File]::WriteAllText($probe, 'x') } catch [UnauthorizedAccessException] { $denied = $true }
if (-not $denied) { try { Remove-Item -LiteralPath $probe -Force } catch { } }
Check $denied 'a new file on the Desktop is refused now (access denied)'
try {
    $since = Get-Date
    $from = Get-AppLogSize
    Start-AppQuiet
    if (-not $script:app) { Done }
    $app = $script:app
    Invoke-AppMenu $app 'T_MENU_HELP' 'T_MENU_REPORT'
    if ($script:MenuOk) {
        $saved = Get-CoreText 'T_REPORT_SAVED'
        $ok = Find-UiText $app.Id ('^' + [regex]::Escape($saved) + '$') 180
        Check $ok "the app says $saved"
        if ($ok) {
            $msg = @(Get-UiLines $app.Id | Where-Object { $_ -match "\|It's saved as " }) | Select-Object -First 1
            "its message: $msg"
            Check ($msg -match 'Logs\\reports\\RB Stems Plus report ') 'it names the zip in Logs\reports'
            Check ($msg -and (Find-RpNames $msg $names).Count -eq 0) 'masked: it names none of them'
            $zip = Get-ReportZip (Join-Path $l.Support 'Logs\reports') $since
            Check ($null -ne $zip) 'the zip is in the support folder''s Logs\reports'
            Check ($null -eq (Get-ReportZip $desk $since)) 'none on the Desktop'
            if ($zip) { Test-ReportZip $zip.FullName $names }
            $said = Read-AppLog $from
            Check ($said -match "report: couldn't save it on the Desktop \(error \d+\)") 'app.log: why it isn''t on the Desktop'
            Check (Invoke-UiNamed $app.Id (Get-CoreText 'T_DONE') 30 'Button') 'Done pressed'
            Start-Sleep -Seconds 2
            Check (@(Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" -ErrorAction SilentlyContinue).Count -eq 0) 'Done opens no browser'
        } else { Show-AppState }
    }
    Stop-App
} finally {
    $ErrorActionPreference = 'Continue'
    $r = & icacls.exe $desk /remove:d $me 2>&1 | Out-String
    "icacls /remove:d ${me}: $($r.Trim())"
    "the Desktop's ACL after: $((& icacls.exe $desk 2>&1 | Out-String).Trim() -replace "`r?`n\s*", ' | ')"
}
Done
