# N4: Help > Create Report end to end (app.md section 5.10, W6a; reviews NS2, NS5, NS20). The app
# opened quietly, the report made: "Report saved" naming the zip as the app shows it (masked); the
# zip on the Desktop (FOLDERID_Desktop) with summary.txt, app.log with the planted line, the
# Application log's part with the planted rekordbox.exe error and WER's with the planted
# Report.wer, and no entry naming the account, its full name, its profile folder or that folder's
# 8.3 alias (no crash dump either). The problem line is planted once the app's start has logged
# its update check, so it is the newest. Then Open GitHub Issue: the zip revealed (selected) in
# Explorer and the issue's link opened in the default browser, caught from its command line
# (msedge.exe's, the browser then stopped): the issues/new link of REPORT_ISSUES_URL, its title
# the planted line masked before its cut, its details' Last problem ending with that line, and none of the names in it either.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$names = Get-RpNames
"names to mask: account '$($names.User)', profile '$($names.Profile)', 8.3 alias '$($names.Short)', full name '$($names.Full)'"
$since = Get-Date
Get-Process -Name msedge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-AppQuiet
if (-not $script:app) { Done }
$app = $script:app
# the planted problem must be the newest (the title is the last kept problem's, review VR1): the
# start's own update check logs its payload.json line first (a failure there would be a problem),
# then the line goes in
$from = Get-AppLogSize
$seen = Wait-Until { (Read-AppLog $from) -match '(?m)^\S+ \S+\s+(payload\.json|payload [0-9]|using the payload|update)' } 45
$upd = @((Read-AppLog $from) -split "`r?`n" | Where-Object { $_ -match 'payload|update' })
"the start's update check: $(if ($seen) { $upd -join ' / ' } else { 'no line within 45 s' })"
Start-Sleep -Seconds 2
$planted = Add-PlantedProblem $names
"planted in app.log: $planted"
# VRR1: the raw title's cut must land in the profile folder's name (UTF-16 pairs count once).
$codePoint = '[\uD800-\uDBFF][\uDC00-\uDFFF]|[\s\S]'
$problem = $planted.Substring(21)   # after the timestamp and two spaces
$points = [regex]::Matches($problem, $codePoint)
$profileAt = $problem.IndexOf($names.Profile, [StringComparison]::Ordinal)
$profileEnd = $profileAt + $names.Profile.Length
Check ($profileAt -ge 0 -and $points.Count -ge 60 -and $points[59].Index -ge ($profileEnd - $names.Folder.Length) -and $points[59].Index -lt $profileEnd) 'the planted message has its 60th code point inside the profile folder name'
# what the report should find, as this side sees it now (for a failure's reading)
"now: $((Get-Date).ToString('o'))"
try {
    $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Level = 2; StartTime = (Get-Date).AddMinutes(-30) } -ErrorAction SilentlyContinue | Where-Object { $_.ToXml() -like "*$RpMark*" })
    "planted events of the last 30 minutes: $($ev.Count)$(if ($ev.Count) { ", at $($ev[0].TimeCreated.ToString('o'))" })"
} catch { "events unreadable: $($_.Exception.Message)" }
$wers = @(Get-ChildItem -LiteralPath (Join-Path $l.Local 'Microsoft\Windows\WER\ReportArchive') -Recurse -Filter 'Report.wer' -ErrorAction SilentlyContinue)
$wers | ForEach-Object { "Report.wer: $($_.FullName) written $($_.LastWriteTime.ToString('o'))" }
Invoke-AppMenu $app 'T_MENU_HELP' 'T_MENU_REPORT'
if (-not $script:MenuOk) { Show-AppState; Stop-App; Done }
$saved = Get-CoreText 'T_REPORT_SAVED'
$ok = Find-UiText $app.Id ('^' + [regex]::Escape($saved) + '$') 180
Check $ok "the app says $saved"
if (-not $ok) { Show-AppState; Stop-App; Done }
$msg = @(Get-UiLines $app.Id | Where-Object { $_ -match "\|It's saved as " }) | Select-Object -First 1
"its message: $msg"
Check ($msg -and (Find-RpNames $msg $names).Count -eq 0) 'its message names none of them (the path shown masked)'
$desk = [Environment]::GetFolderPath('Desktop')
$zip = Get-ReportZip $desk $since
Check ($null -ne $zip) "the zip is on the Desktop ($desk)"
if ($zip) {
    "the zip: $($zip.FullName) ($($zip.Length) bytes)"
    Check ($msg -and $msg.Contains($zip.Name)) 'the message names it'
    Test-ReportZip $zip.FullName $names
}
$alog = Read-AppLog
@($alog -split "`r?`n" | Where-Object { $_ -match '  report: ' }) | Select-Object -Last 6 | ForEach-Object { "app.log: $_" }
Check ($alog -match 'report: \d+ crash or hang reports? from the last 7 days, [1-9]\d* included') 'app.log: the crash reports counted, the planted one included'
# Open GitHub Issue: Explorer's window of the Desktop, and the browser with the issue's link
$open = Get-CoreText 'T_OPEN_ISSUE'
Check (Invoke-UiNamed $app.Id $open 30 'Button') "$open pressed"
$url = $null
$got = Wait-Until {
    foreach ($p in @(Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" -ErrorAction SilentlyContinue)) {
        $m = [regex]::Match([string]$p.CommandLine, 'https://github\.com/Gabe-LS/rbstemsplus/issues/new[^"\s]*')
        if ($m.Success) { $script:url = $m.Value; return $true }
    }
    $false
} 60
Check $got 'the default browser (msedge.exe) was started with the issue''s link'
if ($url) {
    $dec = [Uri]::UnescapeDataString($url)
    "the link ($($url.Length) bytes): $dec"
    Check ($url.Length -le 7000 + 200) "at most REPORT_URL_MAX bytes and the base ($($url.Length))"
    # VRR1 / NS5: mask before cutting to 59 code points and an ellipsis if over 60.
    $exp = "rbtest step FAILED for <user> at ~\x $(if ($names.Short) { '~\y ' })<user> $RpMark"
    $points = [regex]::Matches($exp, $codePoint)
    if ($points.Count -gt 60) { $exp = $exp.Substring(0, $points[59].Index) + [char]0x2026 }
    $title = (($dec -split '&body=', 2)[0] -csplit '[?&]title=', 2)[1]
    "its title: $title"
    Check ($title -ceq "Problem report: $exp") "its title is exactly Problem report: $exp"
    $th = Find-RpNames $title $names
    Check ($th.Count -eq 0 -and $title -notmatch 'C:\\Users') "the title names none of them and no C:\Users path$(if ($th.Count) { " (it holds: $($th -join ', '))" })"
    # Last problem: its lines up to the blank line; the planted one is the last (the title's source)
    $m = [regex]::Match($dec, '(?ms)^Last problem:\r?\n(.*?)(\r?\n\r?\n|\r?\n```|\z)')
    $block = @(if ($m.Success) { $m.Groups[1].Value -split '\r?\n' | Where-Object { $_ } })   # (an array even of one line)
    "Last problem's lines: $($block.Count)$(if ($block.Count) { "; first: $($block[0])" })"
    $want = '^\S+ \S+\s+rbtest step FAILED for <user> at ~\\x .*' + [regex]::Escape($RpMark) + '$'
    Check ($block.Count -ge 1 -and $block[-1] -match $want) "its details' Last problem ends with the planted line, masked ($RpMark)"
    if ($block.Count -gt 1) { "INFO other problems before it: $($block[0..($block.Count - 2)] -join ' / ')" }
    $hits = Find-RpNames $dec $names
    Check ($hits.Count -eq 0) "the link names none of them$(if ($hits.Count) { " (it holds: $($hits -join ', '))" })"
}
Start-Sleep -Seconds 2
Get-Process -Name msedge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
try {
    $sh = New-Object -ComObject Shell.Application
    $shown = @($sh.Windows() | Where-Object { $_.Document -and $_.Document.Folder -and $_.Document.Folder.Self.Path -eq $desk })
    "Explorer windows of the Desktop: $($shown.Count)"
    # VRR8: the selected item's full path identifies the zip.
    $sel = @($shown | ForEach-Object { @($_.Document.SelectedItems()) | ForEach-Object { $_.Path } })
    "selected there: $($sel -join ', ')"
    Check ($zip -and $shown.Count -ge 1 -and @($sel | Where-Object { $_ -eq $zip.FullName }).Count -ge 1) 'Explorer shows the Desktop with the zip selected (REVEAL)'
    $shown | ForEach-Object { $_.Quit() }
} catch { Check $false "Explorer's windows couldn't be read: $($_.Exception.Message)" }
Stop-App
Done
