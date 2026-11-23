# NAR and Q13. The suite's ordinary step log includes the tree, UIA events and registry diff.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'the shell fixture is not installed' }
$settings = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Notifications\Settings'
$aumid = 'io.github.rbstemsplus.app'
$native = (Get-TestArchs | Select-Object -First 1) -replace '-prism$'
$wintest = Get-TestExe $native 'winc-app-wintest.exe'
if (-not $wintest) { Skip 'the app platform test executable is missing' }
$work = Join-Path $env:TEMP ('rbsp-a11y-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$sentinel = Join-Path $settings ('rbsp.q13.control.' + [guid]::NewGuid().ToString('N'))
$app = $events = $notice = $balloon = $null
$desk = [Environment]::GetFolderPath('Desktop')
$priorReports = @(Get-ChildItem -LiteralPath $desk -Filter 'RB Stems Plus report *.zip' -File | ForEach-Object FullName)
try {
    Assert-ShellDesktop
    Expect (-not (Get-AppWindow)) 'the fixture app is closed before the accessibility step'
    Start-Process -FilePath $l.Stub
    Wait-ShellApp
    $app = $script:ShellApp
    Use-ShellUi tree --pid $app.Id --depth 16
    $tree = $script:ExeOut
    $off = Get-CoreText 'T_LIGHT_AX_OFF'
    Check (Test-A11yNode $tree 'Text' $off (Get-CoreText 'T_PLUS_OFF')) 'Demucs light exposes its core name and help text'
    Check (Test-A11yNode $tree 'Text' $off (Get-CoreText 'T_CACHE_OFF')) 'Cache light exposes its core name and help text'
    Check (Test-A11yNode $tree 'Button' (Get-CoreText 'T_BTN_INSTALL_CACHE')) 'the Manage install action is named'
    Check (Test-A11yNode $tree 'TabItem' (Get-CoreText 'T_TAB_MANAGE')) 'the Manage tab is named'

    $events = Start-RbuiCapture $work events "events --pid $($app.Id) --seconds 50"
    Expect (Wait-Until { Test-Path -LiteralPath $events.Ready } 10) 'UIA event subscriptions are armed'
    Use-ShellUi menu --pid $app.Id (Get-MenuText 'T_MENU_HELP') (Get-MenuText 'T_MENU_REPORT') --timeout 15
    $creating = [regex]::Escape((Get-CoreText 'T_CREATING_REPORT'))
    Check (Wait-Until { (Read-RbuiCapture $events) -match "LiveRegionChanged Text `"$creating`" [^\r\n]* live=Polite" } 20) 'UIA receives the core report status announcement'
    $saved = Get-CoreText 'T_REPORT_SAVED'
    Use-ShellUi wait-window --pid $app.Id --title $saved --timeout 30
    Use-ShellUi tree --pid $app.Id --title $saved --depth 12
    Check (Test-A11yNode $script:ExeOut 'Window' $saved) 'the app-owned report dialog has its core title'
    Check ($script:ExeOut -match '(?m)Window .* help="It''s saved as ') 'the dialog exposes its report message as help text'
    Check ((Read-RbuiCapture $events) -match 'FocusChanged .*pid=') 'UIA receives focus events from the shell'
    Use-ShellUi invoke --pid $app.Id --title $saved (Get-CoreText 'T_DONE') --type Button --timeout 15
    Close-ShellApp $app

    $before = @(Get-NoticeRecords $settings)
    if ($before -contains $aumid) {
        Missing "Q13 needs a clean starting record for $aumid to prove the balloon created it"
        Done
    }
    New-Item -Path $sentinel | Out-Null
    New-ItemProperty -LiteralPath $sentinel -Name Q13Control -Value 'intact' | Out-Null
    $title = 'RBSP Q13 ' + [guid]::NewGuid().ToString('N').Substring(0, 10)
    $notice = Start-RbuiCapture $work notice "notice `"$title`" --process winc-app-wintest --timeout 45"
    Expect (Wait-Until { Test-Path -LiteralPath $notice.Ready } 10) 'the notification observer is armed'
    $balloon = Start-Process -FilePath $wintest -ArgumentList "--balloon `"$title`"" -NoNewWindow -PassThru
    Complete-NoticeCheck $notice $balloon $settings $sentinel $before $aumid $wintest
} finally {
    Stop-RbuiCapture $events
    Stop-RbuiCapture $notice
    if ($balloon -and -not $balloon.HasExited) { $balloon.Kill(); [void]$balloon.WaitForExit(5000) }
    if ($app -and -not $app.HasExited) { Stop-App }
    if (Test-Path -LiteralPath $sentinel) { Remove-Item -LiteralPath $sentinel -Recurse -Force }
    Get-ChildItem -LiteralPath $desk -Filter 'RB Stems Plus report *.zip' -File |
        Where-Object { $priorReports -notcontains $_.FullName } | Remove-Item -Force
    Remove-Item -LiteralPath $work -Recurse -Force
}
Done
