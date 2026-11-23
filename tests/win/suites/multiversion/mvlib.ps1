# mvlib.ps1: multiversion.suite's own helpers, after lib/vmlib.ps1, worker/rblib.ps1,
# uninstall/cases/unlib.ps1, uac/uaclib.ps1 and watcher/wtlib.ps1: both rekordbox folders and
# their audited hashes (mv-vm-snapshots.md), the served folder's switch, the batches (each one's
# app.log admin runs, and its elevations from the Security log's process creations, as SYSTEM),
# the app's and the watcher's UI read through UI Automation, screenshots, and what a step leaves
# for the next (C:\rbtest\mv\).
$MvDir = 'C:\rbtest\mv'
$MvVersions = @('7.2.19', '7.2.20')
$MvOrtSha = @{ '7.2.19' = 'e7a702e13bd3e487705204d518c96f1f116aa25b0ac584741838e20bc1ff6e9f'; '7.2.20' = '3e55ab558136c0cf413c1e3873faff7c28807bf4b178712a2124e5efd4e03acd' }
$MvExeSha = @{ '7.2.19' = 'c9ed23e974c51c75e2ec069fbd2679a3dbe606be9e79e9cbbd13d6418ab11c37'; '7.2.20' = '37d2b6a57770a42d9604d8872b1d8dae4f1fffa15483189a92ba58f9dd4de9ef' }
$MvInstaller = '018375aae0ace957afed9505f8d4774d1f68b23b301f0aac89c4f19634cd3ab8'   # Install_rekordbox_x64_7_2_20.exe
$MvServe = 'dist-mv'          # the served folder in the share (make-dist.sh)

function Get-RbDirOf([string]$v) { return Join-Path $env:ProgramFiles "rekordbox\rekordbox $v" }
function Get-RbOrtOf([string]$v) { return Join-Path (Get-RbDirOf $v) 'onnxruntime.dll' }
function Get-RbExeOf([string]$v) { return Join-Path (Get-RbDirOf $v) 'rekordbox.exe' }
function Get-Bridge { return ([string](Read-TestPayload).windows.bridge.sha256).ToLowerInvariant() }
# the bridge the app downloads: the served current/'s payload's (both manifests name the same one)
function Get-ServedBridge {
    $f = Share "$MvServe/current/payload.json"
    if (Test-Path -LiteralPath $f) { return ([string](Get-Content -Raw -LiteralPath $f | ConvertFrom-Json).windows.bridge.sha256).ToLowerInvariant() }
    return Get-Bridge
}

function Save-MvValue([string]$name, [string]$value) {
    New-Item -ItemType Directory -Force $MvDir | Out-Null
    Set-Content -LiteralPath (Join-Path $MvDir $name) -Value $value -Encoding UTF8
}
function Read-MvValue([string]$name) {
    $f = Join-Path $MvDir $name
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    return ([string](Get-Content -Raw -LiteralPath $f)).Trim()
}

# ------------------------------------------------------------------ batches and their UACs
# Start-MvBatch NAME (the user): app.log's length and the time now, for the batch's count
function Start-MvBatch([string]$name) {
    Save-MvValue "$name.from" ([string](Get-AppLogSize))
    Save-MvValue "$name.t0" ([string][DateTime]::UtcNow.Ticks)
}
# Stop-MvBatch NAME: the end time saved; app.log's lines since the start, its "admin run (...)"
# lines counted (each attempt is one runas, mv-spec section 5) and said; their number returned
function Stop-MvBatch([string]$name) {
    Save-MvValue "$name.t1" ([string][DateTime]::UtcNow.Ticks)
    $from = [long](Read-MvValue "$name.from")
    $lines = @((Read-AppLog $from) -split "`r?`n" | Where-Object { $_ })
    $runs = @($lines | Where-Object { $_ -match 'admin run \(' })
    Save-MvValue "$name.adminruns" ([string]$runs.Count)
    [Console]::Out.WriteLine("app.log since the batch $name began ($($lines.Count) lines):")
    foreach ($x in $lines) { [Console]::Out.WriteLine("  app.log: $x") }
    return $runs.Count
}
# (SYSTEM) the elevations between the batch's start and end: the Security log's process creations
# (4688, auditing on since mv/prep) of an "RB Stems Plus.exe" with a full elevated token
# (%%1937) whose creator isn't itself under Stems Cache's root (a worker's own child is no new
# prompt): what UAC would have asked for. Each said; the list returned
function Get-MvElevations([string]$name) {
    $t0 = Read-MvValue "$name.t0"; $t1 = Read-MvValue "$name.t1"
    if (-not $t0) { return $null }
    $start = ([DateTime]::new([long]$t0, [DateTimeKind]::Utc)).ToLocalTime().AddSeconds(-1)
    $end = if ($t1) { ([DateTime]::new([long]$t1, [DateTimeKind]::Utc)).ToLocalTime().AddSeconds(2) } else { Get-Date }
    $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Security'; Id = 4688; StartTime = $start; EndTime = $end } -ErrorAction SilentlyContinue)
    $root = (Join-Path $env:ProgramFiles 'RB Stems Plus') + '\'
    $out = @()
    foreach ($e in $ev) {
        $x = [xml]$e.ToXml()
        $d = @{}
        foreach ($n in $x.Event.EventData.Data) { $d[[string]$n.Name] = [string]$n.'#text' }
        if (-not ($d.NewProcessName -like '*\RB Stems Plus.exe')) { continue }
        $elev = $d.TokenElevationType -eq '%%1937'
        $child = $d.ParentProcessName -and $d.ParentProcessName.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)
        "  4688 $($e.TimeCreated.ToString('HH:mm:ss.fff')) $($d.NewProcessName) elevation $($d.TokenElevationType) label $($d.MandatoryLabel) creator $($d.ParentProcessName)$(if ($elev -and -not $child) { '  <- a UAC elevation' })"
        if ($elev -and -not $child) { $out += $d.NewProcessName }
    }
    $script:MvElevations = $out
}
# (SYSTEM) one batch's UAC count checked: the elevations seen equal want, and app.log's admin runs
function Test-MvUac([string]$name, [int]$want) {
    Get-MvElevations $name
    $runs = Read-MvValue "$name.adminruns"
    $n = @($script:MvElevations).Count
    Save-MvValue "$name.uac" ([string]$n)
    Check ($n -eq $want) "batch $($name): $n UAC elevation(s) (want $want; app.log admin runs $runs)"
}

# ------------------------------------------------------------------ UI
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
# every named element of process pid's top-level windows: "ControlType|Name|AutomationId"
function Get-MvUi([int]$procId) {
    $A = [Windows.Automation.AutomationElement]
    $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)
    $out = @()
    try {
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            $out += "Window|$($w.Current.Name)|"
            foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
                if ($e.Current.Name -or $e.Current.AutomationId -like 'RbRow_*') { $out += "$($e.Current.ControlType.ProgrammaticName -replace '^ControlType\.', '')|$($e.Current.Name)|$($e.Current.AutomationId)" }
            }
        }
    } catch { $out += "unreadable|$($_.Exception.Message)|" }
    return , $out
}
# the names shown (any control type) in pid's windows
function Get-MvNames([int]$procId) {
    $ui = Get-MvUi $procId   # (one array: Get-MvUi returns it whole)
    return @(foreach ($x in $ui) { $n = ($x -split '\|')[1]; if ($n) { $n } })
}
function Show-MvUi([int]$procId, [string]$who = 'ui') { foreach ($x in (Get-MvUi $procId)) { [Console]::Out.WriteLine("  $($who): $x") } }
# waits until every regex in want matches some name in pid's windows (and none in not): whether it did
function Wait-MvNames([int]$procId, [string[]]$want, [int]$secs, [string[]]$not = @()) {
    return Wait-Until {
        $n = Get-MvNames $procId
        $ok = $true
        foreach ($w in $want) { if (-not @($n | Where-Object { $_ -match $w }).Count) { $ok = $false; break } }
        if ($ok) { foreach ($w in $not) { if (@($n | Where-Object { $_ -match $w }).Count) { $ok = $false; break } } }
        $ok
    } $secs
}
function Rx([string]$s) { return '^' + [regex]::Escape($s) + '$' }
function Get-RowText([string]$id, [string]$v) { return (Get-CoreText $id).Replace('{rekordbox}', $v) }
function Get-SummaryText([int]$n, [int]$m) { return (Get-CoreText 'T_WIN_CACHE_SUMMARY').Replace('{n}', "$n").Replace('{m}', "$m") }

# a watcher's dialog whose names match pattern, within secs: its pid ($null when none)
function Find-WatcherDialog([string]$pattern, [int]$secs) {
    $script:MvWatcherPid = $null
    [void](Wait-Until {
            foreach ($w in @(Get-Watchers)) {
                if (@(Get-MvNames $w.Id | Where-Object { $_ -match $pattern }).Count) { $script:MvWatcherPid = $w.Id; return $true }
            }
            $false } $secs)
    return $script:MvWatcherPid
}

Add-Type -Namespace RbspMv -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@
# the foreground window's process id (0 when none)
function Get-ForegroundPid {
    $h = [RbspMv.U]::GetForegroundWindow()
    if ($h -eq [IntPtr]::Zero) { return 0 }
    $p = [uint32]0
    [void][RbspMv.U]::GetWindowThreadProcessId($h, [ref]$p)
    return [int]$p
}

# the user's screen (GDI: it sees what UI Automation reads) into the run's results folder
function Save-MvShot([string]$label) {
    try {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
        $out = Join-Path $VmRun.Results "shot-$($VmRun.Step)-$label.png"
        $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        [Console]::Out.WriteLine("screenshot: $out")
    } catch { [Console]::Out.WriteLine("screenshot $label failed: $($_.Exception.Message)") }
}

# both folders' state, for the log: each onnxruntime.dll's hash and what it is
function Show-MvFolders([string]$bridge) {
    foreach ($v in $MvVersions) {
        $s = Get-Sha (Get-RbOrtOf $v)
        $what = if (-not $s) { 'none' } elseif ($s -eq $bridge) { 'the bridge' } elseif ($s -eq $MvOrtSha[$v]) { "Pioneer's" } else { 'other' }
        [Console]::Out.WriteLine("  rekordbox $v onnxruntime.dll: $what ($s)")
    }
}
# the app's rows and summary as the main window shows them (names matching "N · " or the summary)
function Show-MvRows([int]$procId) {
    $sum = (Get-CoreText 'T_WIN_CACHE_SUMMARY') -replace '\{[nm]\}', '\d+'
    foreach ($x in (Get-MvUi $procId)) { if ($x -match ' \u00B7 |RbRow_' -or ($x -split '\|')[1] -match $sum) { [Console]::Out.WriteLine("  row: $x") } }
}

# a tab of the main window selected by its core text (SelectionItem): whether it was
function Select-MvTab([int]$procId, [string]$id, [int]$secs = 30) {
    $name = Get-CoreText $id
    $A = [Windows.Automation.AutomationElement]
    return Wait-Until {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            $nc = New-Object Windows.Automation.AndCondition(
                (New-Object Windows.Automation.PropertyCondition($A::NameProperty, $name)),
                (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::TabItem)))
            $e = $w.FindFirst([Windows.Automation.TreeScope]::Descendants, $nc)
            if ($e) { $e.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); return $true }
        }
        $false
    } $secs
}

# the button named button in the top-level window of pid that also shows a name matching
# textRegex (a dialog, not the main window that may hold a button of the same name), invoked:
# whether it was, within secs
function Invoke-MvDialogButton([int]$procId, [string]$textRegex, [string]$button, [int]$secs = 60) {
    $A = [Windows.Automation.AutomationElement]
    return Wait-Until {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            $all = $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
            $hit = $false; $btn = $null
            foreach ($e in $all) {
                if ($e.Current.Name -match $textRegex) { $hit = $true }
                # a Button, or a task dialog's command button (a Pane, CommandButton_<id>)
                if (-not $btn -and $e.Current.Name -eq $button -and ($e.Current.ControlType -eq [Windows.Automation.ControlType]::Button -or $e.Current.AutomationId -like 'CommandButton_*')) { $btn = $e }
            }
            if ($hit -and $btn -and $btn.Current.AutomationId -match '^CommandButton_(\d+)$') {
                # a task dialog's button (no Invoke pattern on its pane): TDM_CLICK_BUTTON with its id
                [void][RbspMv.U]::PostMessage([IntPtr]$w.Current.NativeWindowHandle, 0x466, [IntPtr][int]$Matches[1], [IntPtr]::Zero)
                return $true
            }
            if ($hit -and $btn) {
                $pat = $null
                if ($btn.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) { $pat.Invoke(); return $true }
            }
        }
        $false
    } $secs
}
# how many windows pid shows: its top-level windows and the dialogs inside them (the shell's
# alerts are Window elements in the main window's tree); the main window alone: 1
function Get-MvWindowCount([int]$procId) {
    $A = [Windows.Automation.AutomationElement]
    $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)
    $wc = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::Window)
    $n = 0
    foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) { $n += 1 + @($w.FindAll([Windows.Automation.TreeScope]::Descendants, $wc)).Count }
    return $n
}
# the start-up's "Stems Cache update" offer (core/flows.c offer_cache_update), when the app shows
# it within secs: said (a finding: with one version bridged and another not, the start-up reads
# only the newest folder's onnxruntime.dll, CORE_STEP_BRIDGE_SHA), screenshot, answered Not Now
# so the start-up goes on (its update check waits behind it). -Fail makes it a failed check.
# A statement; whether it showed in $script:MvOffered
function Clear-MvOffer([int]$procId, [int]$secs = 15, [switch]$Fail) {
    $t = Get-CoreText 'T_CACHE_UPDATE_TITLE'
    $script:MvOffered = Find-UiText $procId (Rx $t) $secs
    if (-not $script:MvOffered) { if ($Fail) { Check $true "the start-up asks nothing (no '$t' offer)" }; return }
    if ($Fail) { Check $false "the start-up asks nothing: it offers '$t' (finding F1: the newest folder's DLL read as the installed bridge)" }
    else { "FINDING F1 again: the start-up offers '$t'" }
    Save-MvShot 'offer'
    $line = @((Read-AppLog) -split "`r?`n" | Where-Object { $_ -match 'an update is available' }) | Select-Object -Last 1
    "  app.log: $line"
    Check (Invoke-MvDialogButton $procId (Rx $t) (Get-CoreText 'T_NOT_NOW') 20) "the offer answered $(Get-CoreText 'T_NOT_NOW')"
}
# the rows the main window must show: each regex a name (Rx of a row text), within secs; said
function Test-MvRows([int]$procId, [string[]]$want, [int]$secs = 60, [string[]]$not = @()) {
    $script:MvRowsOk = Wait-MvNames $procId $want $secs $not
    Check $script:MvRowsOk "Manage shows $(($want | ForEach-Object { $_.Trim('^', '$') -replace '\\', '' }) -join ' / ')$(if ($not.Count) { " and not $(($not | ForEach-Object { $_.Trim('^', '$') -replace '\\', '' }) -join ' / ')" })"
    Show-MvRows $procId
}
function Get-WatcherAsks([long]$from, [string]$v) {
    return @((Read-WatcherLog $from) -split "`r?`n" | Where-Object { $_ -match "asking about Stems Cache in rekordbox .*$([regex]::Escape($v))" })
}

# (SYSTEM) Stems Cache's root gone, or going at the next restart: the worker can't delete its own
# running exe, so it leaves its last file (and the folders above it) to a delete at the next
# restart (app.log: "the last file of ... is removed at the next restart"; Session Manager's
# PendingFileRenameOperations). Said and checked
function Test-MvRootGone([int]$secs = 30) {
    if (Wait-Until { -not (Test-Path -LiteralPath $CacheRoot) } $secs) { Check $true "Stems Cache's root is gone ($CacheRoot)"; return }
    $left = @(Get-ChildItem -LiteralPath $CacheRoot -Recurse -Force -ErrorAction SilentlyContinue)
    foreach ($f in $left) { "  left: $($f.FullName.Substring($CacheRoot.Length))" }
    $pend = @((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations | Where-Object { $_ })
    $ours = @($pend | Where-Object { $_ -like "*\RB Stems Plus*" })
    foreach ($x in $ours) { "  pending at the next restart: $x" }
    $files = @($left | Where-Object { -not $_.PSIsContainer })
    $allPending = -not @($files | Where-Object { $p = $_.FullName; -not @($ours | Where-Object { $_ -like "*$p" }).Count }).Count
    Check ($files.Count -le 1 -and $allPending -and $ours.Count -ge 1) "Stems Cache's root is gone but for its last file(s), each deleted at the next restart ($($files.Count) file(s) left)"
}
