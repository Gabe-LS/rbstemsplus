# rplib.ps1: report.suite's own helpers, after lib/vmlib.ps1, uninstall/cases/unlib.ps1 and
# uac/uaclib.ps1: the names Create Report must mask, the app's menus by the core's texts, the
# report's zip read back, the issue link caught from the browser's command line.
$RpMark = 'RBVM-MARK-7Q3'                 # planted in every source the report reads
$RpDumpMark = 'RBVM-DUMP-4X8'             # VRR7: stays out of every zip entry
$RpFullName = 'Dana Jones'                # the account's full name, set by plant-sys (net user /fullname)
$RpDir = 'C:\rbtest\report'               # what one step leaves for the next

# the account (the logged-in user's name), its profile folder and that folder's 8.3 alias ('' when
# it has none): what neither the zip nor the link may hold
function Get-RpNames {
    $p = Get-UserProfile
    $short = ''
    try { $s = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($p).ShortPath; if ($s -and $s -ne $p) { $short = $s } } catch { }
    # the account's own name (GetUserNameW's, which may differ from its profile folder's name);
    # as SYSTEM, the signed-in user's
    $me = [Security.Principal.WindowsIdentity]::GetCurrent()
    $acct = if ($me.User.Value -eq 'S-1-5-18') { [string](Get-CimInstance Win32_ComputerSystem).UserName } else { $me.Name }
    $user = ($acct -split '\\')[-1]
    return [pscustomobject]@{ User = $user; Folder = (Split-Path -Leaf $p); Profile = $p; Short = $short; Full = $RpFullName }
}
# the problem line Create Report must take as its last problem, appended to app.log now (the app
# may run: its writer shares the file), stamped as the app stamps its lines ("yyyy-MM-dd
# HH:mm:ss  line"): the full name, the profile folder, its 8.3 alias and the account before the
# 60th character (the title's cut: NS5's mask before the cut), the marker after them; the line
# itself is returned
function Add-PlantedProblem($names) {
    $log = Join-Path (Get-Layout).Support 'Logs\app.log'
    $line = "$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))  rbtest step FAILED for $($names.Full) at $($names.Profile)\x $(if ($names.Short) { "$($names.Short)\y " })$($names.User) $RpMark"
    $b = (New-Object Text.UTF8Encoding $false).GetBytes($line + "`n")   # (the app ends its lines with LF alone)
    $f = New-Object IO.FileStream($log, [IO.FileMode]::Append, [IO.FileAccess]::Write, ([IO.FileShare]'ReadWrite, Delete'))
    try { $f.Write($b, 0, $b.Length) } finally { $f.Dispose() }
    return $line
}
# which of the names text holds, case ignored (an empty list: none)
function Find-RpNames([string]$text, $names) {
    $hits = @()
    foreach ($n in @($names.Full, $names.Profile, $names.Short, $names.User, $names.Folder)) {
        if ($n -and $text.IndexOf($n, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $hits += $n }
    }
    return , $hits
}

# a menu item of the app's window: the top menu, then the item, each by its core text id, retried
# while the core hasn't enabled it yet: a statement (its Check is its output), whether both were
# invoked in $script:MenuOk
function Invoke-AppMenu($app, [string]$top, [string]$item, [int]$secs = 90) {
    $t1 = Get-MenuText $top
    $t2 = Get-MenuText $item
    $ok = $false
    $limit = Get-WaitSecs $secs
    $t = [Diagnostics.Stopwatch]::StartNew()
    while (-not $ok -and $t.Elapsed.TotalSeconds -lt $limit) {
        if (Invoke-UiWhenReady $app.Id $t1 'MenuItem' 5) { $ok = Invoke-UiWhenReady $app.Id $t2 'MenuItem' 5 }
    }
    Check $ok "the menu's $t1 > $t2 chosen"
    $script:MenuOk = $ok
}

# every named element of process pid's windows: "ControlType|Name|Value" lines (Value: an edit's)
function Get-UiLines([int]$procId) {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [Windows.Automation.AutomationElement]
    $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)
    $out = @()
    foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
        foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
            $v = ''
            try { $vp = $e.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern); if ($vp) { $v = $vp.Current.Value } } catch { }
            $out += "$($e.Current.ControlType.ProgrammaticName)|$($e.Current.Name)|$v"
        }
    }
    return $out
}
# the editable fields of process pid's windows, in order (a read-only one, as the window's log,
# left out): their AutomationElements
function Get-UiEdits([int]$procId) {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [Windows.Automation.AutomationElement]
    $cond = New-Object Windows.Automation.AndCondition(
        (New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)),
        (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::Edit)))
    return @($A::RootElement.FindAll([Windows.Automation.TreeScope]::Descendants, $cond) | Where-Object {
            $ro = $true; try { $ro = $_.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).Current.IsReadOnly } catch { }
            -not $ro })
}

# the newest "RB Stems Plus report *.zip" in dir made after since ($null when none)
function Get-ReportZip([string]$dir, [datetime]$since) {
    if (-not (Test-Path -LiteralPath $dir)) { return $null }
    return @(Get-ChildItem -LiteralPath $dir -File -Filter 'RB Stems Plus report *.zip' -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $since.AddSeconds(-2) } | Sort-Object LastWriteTime -Descending) | Select-Object -First 1
}
# a zip's entries as @{ name = text } (UTF-8)
function Read-ZipTexts([string]$path) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $h = [ordered]@{}
    $z = [IO.Compression.ZipFile]::OpenRead($path)
    try {
        foreach ($e in $z.Entries) {
            $r = New-Object IO.StreamReader($e.Open(), [Text.Encoding]::UTF8)
            try { $h[$e.FullName] = $r.ReadToEnd() } finally { $r.Dispose() }
        }
    } finally { $z.Dispose() }
    return $h
}
# the checks on a report zip: its entries (summary, the logs, the event log's and WER's parts with
# the planted marker) and no name in any entry; a statement (its Checks are its output)
function Test-ReportZip([string]$path, $names) {
    $h = Read-ZipTexts $path
    "the zip's entries: $(@($h.Keys) -join ', ')"
    $find = { param($like) @($h.Keys | Where-Object { $_ -like $like }) | Select-Object -First 1 }
    $sum = & $find '*summary.txt'
    Check ($null -ne $sum) 'summary.txt is in it'
    $alog = & $find '*app.log'
    Check ($alog -and $h[$alog].Contains($RpMark)) "app.log is in it, with the planted line ($RpMark)"
    $ev = & $find '*event-log-errors.txt'
    Check ($null -ne $ev) 'event-log-errors.txt is in it'
    if ($ev) {
        Check ($h[$ev].Contains($RpMark)) "the Application log's planted rekordbox.exe error is in it ($RpMark)"
        if (-not $h[$ev].Contains($RpMark)) { "  event-log-errors.txt: $($h[$ev].Substring(0, [Math]::Min(600, $h[$ev].Length)))" }
    }
    $wer = & $find '*crash-reports/*-AppCrash_rekordbox*'
    # VRR7: check the WER text and catch dump contents under any entry name.
    Check ($wer -and $h[$wer].Contains($RpMark) -and $h[$wer].Contains('EventType=APPCRASH')) "the planted APPCRASH Report.wer is in it ($wer)"
    Check (@($h.Values | Where-Object { $_.Contains($RpDumpMark) }).Count -eq 0) "no entry holds the planted dump marker ($RpDumpMark)"
    # never a crash dump (app.md section 5.10: only files named exactly Report.wer): the planted
    # memory.hdmp and x.dmp beside it stay out
    $crash = @($h.Keys | Where-Object { $_ -like '*crash-reports/*' })
    Check (@($crash | Where-Object { $_ -notlike '*.wer.txt' }).Count -eq 0) "every crash-reports/ entry is a Report.wer's text ($($crash.Count))"
    $dumps = @($h.Keys | Where-Object { $_ -match '\.(dmp|mdmp|hdmp)$' -or $_ -match 'memory\.hdmp|x\.dmp' })
    Check ($dumps.Count -eq 0) "no crash dump in it$(if ($dumps.Count) { ": $($dumps -join ', ')" })"
    $bad = 0
    foreach ($k in $h.Keys) {
        $hits = Find-RpNames $h[$k] $names
        if ($hits.Count) {
            $bad++
            "  $k holds: $($hits -join ', ')"
            foreach ($n in $hits) {
                $i = $h[$k].IndexOf($n, [StringComparison]::OrdinalIgnoreCase)
                "    ...$($h[$k].Substring([Math]::Max(0, $i - 80), [Math]::Min(200, $h[$k].Length - [Math]::Max(0, $i - 80))))..."
            }
        }
    }
    Check ($bad -eq 0) "no entry names the account, its full name ($($names.Full)), its profile folder or that folder's 8.3 alias ($($names.Short))"
}
