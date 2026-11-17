# W13b's VM run of the Status and Prepare tabs (app.md 4.2 and W8b's "Still to do"), as the
# logged-in user (vm/run-w13b.sh: one winps.sh --user-signed job per part), over the installed TEST
# app (vm/install.ps1) and w13b-fixture.ps1's sheets. It drives the probe through its pipe
# (\\.\pipe\rbstemsplus-probe, acks in <Out>\acks: Probe.cs's commands, status.* fields, select-set
# and doubleclick-set) and, for what the probe can't do, real input from outside the app: UI
# Automation (Windows' own UIAutomationClient: scroll, select, focus) and SendInput/SendKeys.
#   -Part spike: the 5,000-row spike, perf mode, -Status big: launch to the window drawn and to
#      5,000 sets shown; scrolling them (UIA and the wheel) sends nothing; a select-set and a UIA
#      select each send exactly one select; doubleclick-set opens the set's playlist on the Prepare
#      tab; the 5,051-row outline with mixed expansion scrolled: no event reaches the core; a
#      5,000-track playlist chosen at full speed and at 1 line/ms; its tracks scrolled (nothing
#      sent). Times are the driver's (its 20 ms polls and the pipe included): the precise ones are
#      perf-<pid>.tsv's (tests/perf/stats.py).
#   -Part pt: the PT review's WPF-side cases (trace mode, -Status small), each a PASS/FAIL line;
#      then the prepare menus' PM1 check: View > Prepare answered on with RBSP_SHELL_TEST=prepare,
#      "err disabled" in a launch without it, and no shell.prepare_mismatch mark in either.
#   -Part all: both in one job, the spike first (run-w13b.sh runs them as two jobs).
#   -Part dog: the watchdog, started by the runner itself (Arm): not for a caller.
# Every step runs in its own try/catch: a step that throws is a FAIL and the next one runs. Lines
# tagged MANUAL or VISUAL are what this run can't decide: the owner's or the VM lane's eyes.
# Results (records, acks, screenshots) go to -Results. Exit code: the number of failed checks.
# Every wait is bounded: the probe's own (3 min at most), each command's ack (30 s by default),
# and the dog: 9 minutes per job (below winps.sh's 10), after which it ends the app, restores high
# contrast, cleans the fixture and ends this runner (no summary line then). It exits as soon as
# this runner does.
# High contrast is set with a NULL scheme (Windows' default contrast theme), and restored with only
# HCF_HIGHCONTRASTON changed from the starting flags; the starting scheme's name isn't put back (a
# known limit).
# Test only. Pure ASCII (Windows PowerShell 5.1 reads this file as ANSI: non-ASCII from [char]).
#   w13b.ps1 [-App DIR] [-Out DIR] [-Results DIR] [-Sets N] [-Part all|spike|pt]
param(
    [string]$App = (Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus\shell-smoke-test'),
    [string]$Out = 'C:\rbtest\w13b',
    [string]$Results = '\\Mac\win\shell\results\w13b',
    [ValidateRange(1, 5000)][int]$Sets = 5000,
    [ValidateSet('all', 'spike', 'pt', 'dog')][string]$Part = 'all',
    # the dog's (-Part dog): the runner's pid, its StartTime.Ticks, the budget, the starting high contrast
    [int]$DogPid = 0,
    [long]$DogTicks = 0,
    [int]$DogMs = 0,
    [int]$HcStart = 0
)
# PowerShell's variable names ignore case: no $script: name below may match a parameter's
# (App, Out, Results, Sets, Part, DogPid, DogTicks, DogMs, HcStart), or it would overwrite it
$ErrorActionPreference = 'Stop'
$fixture = Join-Path $PSScriptRoot 'w13b-fixture.ps1'

$contrastType = @"
using System; using System.Runtime.InteropServices;
public static class W13bContrast {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct HIGHCONTRAST { public int cbSize; public int dwFlags; public IntPtr lpszDefaultScheme; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern bool SystemParametersInfo(int action, int param, ref HIGHCONTRAST v, int winIni);
  // only HCF_HIGHCONTRASTON changes: the other HCF_* bits (the hotkey ones) kept from saved
  public static bool Set(bool on, int saved) {
    var h = new HIGHCONTRAST { cbSize = Marshal.SizeOf(typeof(HIGHCONTRAST)), dwFlags = (saved & ~1) | (on ? 1 : 0) };
    return SystemParametersInfo(0x0043, h.cbSize, ref h, 3);   // SPI_SETHIGHCONTRAST
  }
  public static int Flags() {
    var h = new HIGHCONTRAST { cbSize = Marshal.SizeOf(typeof(HIGHCONTRAST)) };
    SystemParametersInfo(0x0042, h.cbSize, ref h, 0);   // SPI_GETHIGHCONTRAST
    return h.dwFlags;
  }
  public static bool On() { return (Flags() & 1) != 0; }   // HCF_HIGHCONTRASTON
}
"@
Add-Type $contrastType

# ---------------------------------------------------------------- the dog (-Part dog)
# Started by Arm with the runner's pid and start time: it exits at once when that process is gone
# or isn't the same one (a reused pid), and as soon as the runner ends; only past the budget does
# it end the app (the one copy is the runner's), restore high contrast, clean the fixture and end
# the runner. Never anything else: no other process is touched.
if ($Part -eq 'dog') {
    $r = Get-Process -Id $DogPid -ErrorAction SilentlyContinue
    if (-not $r) { exit 0 }
    $ticks = 0L
    try { $ticks = $r.StartTime.Ticks } catch { exit 0 }
    if ($ticks -ne $DogTicks) { exit 0 }
    if ($r.WaitForExit($DogMs)) { exit 0 }
    Stop-Process -Name 'RB Stems Plus' -Force -ErrorAction SilentlyContinue
    try { if ([W13bContrast]::On() -ne (($HcStart -band 1) -ne 0)) { [W13bContrast]::Set(($HcStart -band 1) -ne 0, $HcStart) | Out-Null } } catch { }
    try { & $fixture -App $App -Clean | Out-Null } catch { }
    try { $r.Kill() } catch { }
    exit 3
}

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W13bInput {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static void Click(int x, int y) { SetCursorPos(x, y); mouse_event(2, 0, 0, 0, UIntPtr.Zero); mouse_event(4, 0, 0, 0, UIntPtr.Zero); }
  public static void Wheel(int x, int y, int notches) { SetCursorPos(x, y); mouse_event(0x0800, 0, 0, notches * 120, UIntPtr.Zero); }
}
"@

$fail = 0
function Check($ok, $what) { if ($ok) { "PASS $what" } else { "FAIL $what"; $script:fail++ } }
function Manual($what) { "MANUAL $what" }
# a step: its own try/catch, a throw is a FAIL and the run goes on
function Step([string]$Name, [scriptblock]$Body) {
    try { & $Body } catch { Check $false ("$Name stopped: " + $_.Exception.Message) }
}
New-Item -ItemType Directory -Force -Path $Results | Out-Null
$exe = Join-Path $App 'RB Stems Plus.exe'
$AE = [System.Windows.Automation.AutomationElement]
$CT = [System.Windows.Automation.ControlType]
$TS = [System.Windows.Automation.TreeScope]
$ellipsis = [string][char]0x2026
$stopName = 'Stop Keeping' + $ellipsis
$againName = 'Prepare Again'

# the shots taken (a VISUAL line lists only these); a required one not taken is a FAIL
$script:shots = New-Object System.Collections.Generic.List[string]
function Shot([string]$Name) {
    try {
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        $bmp.Save((Join-Path $Results "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        $script:shots.Add("$Name.png")
        return $true
    } catch { Write-Host "  (shot $($Name): $($_.Exception.Message))"; return $false }
}

# ---------------------------------------------------------------- the dog's arming

$script:dog = $null
function Disarm { if ($script:dog -and -not $script:dog.HasExited) { try { $script:dog.Kill() } catch { } }; $script:dog = $null }
function Arm([int]$Minutes = 9) {
    Disarm
    $me = Get-Process -Id $PID
    # Start-Process joins its arguments with spaces and quotes none: the paths (the app's has
    # spaces) go in quotes
    $script:dog = Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @(
        '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'), '-Part', 'dog',
        '-DogPid', $PID, '-DogTicks', $me.StartTime.Ticks, '-DogMs', ($Minutes * 60000), '-HcStart', $script:hc0,
        # a trailing backslash would escape the closing quote (Windows' argv rules): doubled
        '-App', ('"' + ($App -replace '\\+$', '$0$0') + '"'))
}

# ---------------------------------------------------------------- the app and the probe's pipe

$script:proc = $null; $script:pipe = $null; $script:writer = $null; $script:n = 0; $script:sw = $null
function Start-App([string]$Mode, [string]$Status, [switch]$NoPrepare) {
    Get-Process 'RB Stems Plus' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    & $fixture -App $App -Out $Out -Mode $Mode -Status $Status -Sets $Sets
    if ($LASTEXITCODE -ne 0) { throw "w13b-fixture.ps1 failed ($LASTEXITCODE)" }
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    Get-ChildItem -LiteralPath $Out -File | Remove-Item -Force
    # the Status and Prepare tabs shown (Shell.PrepareShown); -NoPrepare: the first release's window
    if (-not $NoPrepare) { $env:RBSP_SHELL_TEST = 'prepare' }
    $script:sw = [Diagnostics.Stopwatch]::StartNew()
    $script:proc = Start-Process -FilePath $exe -PassThru
    Remove-Item Env:\RBSP_SHELL_TEST -ErrorAction SilentlyContinue
    $script:pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'rbstemsplus-probe', [System.IO.Pipes.PipeDirection]::Out)
    $script:pipe.Connect(30000)
    $script:writer = New-Object System.IO.StreamWriter($script:pipe, (New-Object System.Text.UTF8Encoding($false)))
    $script:writer.AutoFlush = $true
}
function Stop-App([string]$Tag) {
    if ($script:proc -and -not $script:proc.HasExited) {
        Cmd 'flush' 10 | Out-Null
        Cmd 'quit' 5 | Out-Null
        if (-not $script:proc.WaitForExit(15000)) { $script:proc.Kill() }
    }
    try { $script:writer.Dispose(); $script:pipe.Dispose() } catch { }
    $script:writer = $null; $script:pipe = $null
    Get-ChildItem -LiteralPath $Out -File -ErrorAction SilentlyContinue | ForEach-Object { Copy-Item $_.FullName (Join-Path $Results ("$Tag-" + $_.Name)) -Force }
}
function AcksText {
    $p = Join-Path $Out 'acks'
    if (-not (Test-Path -LiteralPath $p)) { return '' }
    $fs = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite')
    try { (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
}
# one command, its ack ("ok ...", "err ...") or $null after TimeoutS
function Cmd([string]$Text, [int]$TimeoutS = 30) {
    $script:n++
    $id = "w$($script:n)"
    $script:writer.WriteLine("$id $Text")
    $until = (Get-Date).AddSeconds($TimeoutS)
    while ((Get-Date) -lt $until) {
        foreach ($l in ((AcksText) -split "`n")) { if ($l.StartsWith("$id ")) { return $l.Substring($id.Length + 1).Trim() } }
        Start-Sleep -Milliseconds 20
    }
    return $null
}
# Write-Host: the information stream, which the job's *> keeps (Out-Host would bypass it)
function Ok([string]$Text, [int]$TimeoutS = 30) { $a = Cmd $Text $TimeoutS; if (-not ($a -like 'ok*')) { Write-Host "  ($($Text): $a)" }; return ($a -like 'ok*') }
function WaitFor([int]$Ms, [string]$Cond) { return (Ok "wait $Ms $Cond" ([int]($Ms / 1000) + 10)) }
function Quiet([int]$Ms = 300, [int]$TimeoutMs = 10000) { return (Ok "quiet $Ms $TimeoutMs" ([int]($TimeoutMs / 1000) + 10)) }
function Count([string]$Name) { $a = Cmd "count $Name"; if ($a -match '^ok (\d+)') { [int]$Matches[1] } else { -1 } }
function Timed([scriptblock]$b) { $t = [Diagnostics.Stopwatch]::StartNew(); $r = @(& $b); return @(, $r) + @([int]$t.ElapsedMilliseconds) }
function AllTrue($list) { return (@($list).Count -gt 0 -and -not (@($list) -contains $false)) }

# trace mode's records: the events the shell sent after the last "cmd" record of Verb Arg
function SentAfter([string]$VerbArg) {
    $f = Get-ChildItem -LiteralPath $Out -Filter 'app-*.jsonl' | Select-Object -First 1
    if (-not $f) { return @() }
    $fs = [IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite')
    try { $lines = (New-Object IO.StreamReader($fs)).ReadToEnd() -split "`n" } finally { $fs.Dispose() }
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '"e":"cmd"') { $o = $lines[$i] | ConvertFrom-Json; if ($o.v -eq $VerbArg) { $start = $i } }
    }
    if ($start -lt 0 -or $start + 1 -ge $lines.Count) { return @() }
    @($lines[($start + 1)..($lines.Count - 1)] | Where-Object { $_ -match '"e":"sent"' } | ForEach-Object { ($_ | ConvertFrom-Json).v | ConvertFrom-Json })
}

# ---------------------------------------------------------------- UI Automation

function Main {
    $c = [System.Windows.Automation.PropertyCondition]::new($AE::ProcessIdProperty, [int]$script:proc.Id)
    $AE::RootElement.FindAll($TS::Children, $c) | Where-Object { $_.Current.ControlType -eq $CT::Window } | Select-Object -First 1
}
function First($root, $type) { if ($null -eq $root) { return $null }; $root.FindFirst($TS::Descendants, [System.Windows.Automation.PropertyCondition]::new($AE::ControlTypeProperty, $type)) }
function All($root, $type) { if ($null -eq $root) { return @() }; @($root.FindAll($TS::Descendants, [System.Windows.Automation.PropertyCondition]::new($AE::ControlTypeProperty, $type))) }
function Item($root, $type, [string]$Prefix) { All $root $type | Where-Object { $_.Current.Name -like "$Prefix*" } | Select-Object -First 1 }
# the selected tab's item: its content's elements are under it (TabItemAutomationPeer), so a
# lookup there finds this tab's table, never another tab's
function TabRoot {
    $tab = First (Main) $CT::Tab
    if (-not $tab) { return $null }
    $sel = $tab.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern).Current.GetSelection()
    if ($sel.Count -lt 1) { return $null }
    return $sel[0]
}
# a ListView with a GridView is a DataGrid to UIA (GridViewAutomationPeer), its rows DataItems
function TabTable { First (TabRoot) $CT::DataGrid }
function Centre($el) { $r = $el.Current.BoundingRectangle; @([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2)) }
function Front { $m = Main; if ($m) { [W13bInput]::SetForegroundWindow([IntPtr]$m.Current.NativeWindowHandle) | Out-Null; Start-Sleep -Milliseconds 200 } }
# a tree row's own line: its bounds' top line (a folder's bounds take its open children too); dx 9
# is the expander's column (19 px in WPF's TreeViewItem template), dx 31 the glyph and title
function RowPoint($el, [int]$Dx) { $r = $el.Current.BoundingRectangle; @([int]($r.X + $Dx), [int]($r.Y + [Math]::Min(10, $r.Height / 2))) }
function ClickAt($p) { Front; [W13bInput]::Click($p[0], $p[1]) }
function DoubleClickAt($p) { Front; [W13bInput]::Click($p[0], $p[1]); Start-Sleep -Milliseconds 60; [W13bInput]::Click($p[0], $p[1]) }
function Keys([string]$k) { Front; [System.Windows.Forms.SendKeys]::SendWait($k) }
# a list's or tree's scroll, by UIA's ScrollPattern, then by the wheel: each step's time to quiet,
# its percent checked (within 1 of the target; the wheel from 0 moves it). Returns the pass flag:
# no ScrollPattern, a refused step, a wrong percent or a step that didn't settle is a fail.
# -Tree (a virtualized hierarchical TreeView, whose extent WPF estimates from the realized rows):
# 25/50/75 only need to rise, each above the one before and below 100; 100 and 0 still within 1.
function ScrollAll($el, [string]$What, [switch]$Tree) {
    $sp = $null
    if (-not $el.TryGetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern, [ref]$sp)) {
        Write-Host "  $($What): no ScrollPattern"
        return $false
    }
    $ok = $true
    $times = @()
    $prev = 0
    foreach ($p in 25, 50, 75, 100, 0) {
        # a hashtable: the block runs in its own scope, where a plain $refused = $true would be lost
        $st = @{ refused = $false }
        $t = Timed {
            try { $sp.SetScrollPercent([System.Windows.Automation.ScrollPattern]::NoScroll, [double]$p) } catch { $st.refused = $true }
            Quiet 200 5000
        }
        $now = $sp.Current.VerticalScrollPercent
        $near = [Math]::Abs($now - $p) -le 1
        if ($Tree -and $p -gt 0 -and $p -lt 100) { $near = $now -gt $prev -and $now -lt 100 }
        $prev = $now
        $good = -not $st.refused -and (AllTrue $t[0]) -and $near
        if (-not $good) { $ok = $false }
        $times += "$($p)%: $($t[1]) ms (at $([Math]::Round($now, 1))%$(if ($st.refused) { ', refused' }))"
    }
    Write-Host "  $What by UIA: $($times -join '; ')"
    $c = Centre $el
    Front
    $t = Timed { for ($i = 0; $i -lt 30; $i++) { [W13bInput]::Wheel($c[0], $c[1], -3) }; Quiet 200 5000 }
    $after = $sp.Current.VerticalScrollPercent
    if (-not ((AllTrue $t[0]) -and $after -gt 0)) { $ok = $false }
    Write-Host "  $What by the wheel (30 x 3 notches down from 0%): $($t[1]) ms to quiet, at $([Math]::Round($after, 1))%"
    return $ok
}

# ================================================================ the spike

function Spike {
    "== the 5,000-row spike (perf mode, $Sets sets)"
    Start-App 'perf' 'big'
    $script:mid = [int][Math]::Floor($Sets / 2)
    $script:midName = 'Set ' + $script:mid.ToString('0000')
    Step 'launch' {
        Check (Ok 'event 30000 windowDrawn 1' 40) "the window drawn ($($script:sw.ElapsedMilliseconds) ms from the launch)"
        Ok 'tab Status' | Out-Null
        $r = WaitFor 180000 "status.table.rows=$Sets"
        Check $r "the Status tab shows $Sets sets ($($script:sw.ElapsedMilliseconds) ms from the launch)"
        Check (Quiet 1500 120000) "the app settles after the sets"
        Check (Shot 'spike-1-status') "shot spike-1-status"
    }
    Step 'the sets scrolled' {
        $list = TabTable
        Check ($null -ne $list) "the sets' table found by UIA in the Status tab (a DataGrid)"
        if ($list) {
            $c0 = Count 'sent'
            $sc = ScrollAll $list 'the sets'
            Check ($sc -and (Count 'sent') -eq $c0) "scrolling $Sets sets: every step lands, nothing sent to the core"
        }
    }
    Step 'select-set' {
        $s0 = Count 'sent:select'
        $t = Timed { Ok "select-set $($script:mid)"; WaitFor 10000 "status.table.sel=$($script:mid)"; Quiet 300 5000 }
        Check ((AllTrue $t[0]) -and (Count 'sent:select') -eq $s0 + 1) "select-set $($script:mid): shown selected, exactly one select sent ($($t[1]) ms)"
    }
    Step 'a UIA select' {
        $list = TabTable
        $vis = All $list $CT::DataItem | Where-Object { -not $_.Current.IsOffscreen } | Select-Object -Skip 2 -First 1
        Check ($null -ne $vis) "a shown row of the sets' table (a DataItem) for a UIA select"
        if ($vis) {
            $s0 = Count 'sent:select'
            $vis.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
            $e = Ok "event 5000 sent:select $($s0 + 1)"
            $q = Quiet 300 5000
            Check ($e -and $q -and (Count 'sent:select') -eq $s0 + 1) "a UIA select of a shown row ($($vis.Current.Name)): exactly one select"
        }
    }
    Step 'doubleclick-set' {
        $t = Timed { Ok "doubleclick-set $($script:mid)"; WaitFor 15000 'tab.sel=1'; WaitFor 15000 "prepare.nameLabel.str=$($script:midName)" }
        Check (AllTrue $t[0]) "doubleclick-set $($script:mid): the Prepare tab with $($script:midName) chosen ($($t[1]) ms)"
        Check (Shot 'spike-2-prepare-again') "shot spike-2-prepare-again"
    }
    Step 'the outline scrolled' {
        Ok 'tab Prepare' | Out-Null
        WaitFor 10000 'tab.sel=1' | Out-Null
        Ok 'expand 100' | Out-Null
        # Library open, every other folder open (mixed expansion)
        for ($f = 0; $f -lt 50; $f++) { Ok ($(if ($f % 2 -eq 0) { 'expand ' } else { 'collapse ' }) + (101 + $f)) 5 | Out-Null }
        Check (Quiet 1000 30000) "the outline settles with mixed expansion"
        $tree = First (TabRoot) $CT::Tree
        Check ($null -ne $tree) "the outline found by UIA in the Prepare tab"
        if ($tree) {
            $c1 = Count 'sent'
            $sc = ScrollAll $tree 'the outline (mixed expansion)' -Tree
            Check ($sc -and (Count 'sent') -eq $c1) "scrolling the outline with mixed expansion: every step lands, no event reaches the core (app.md 4.2's probe test)"
            Check (Shot 'spike-3-outline') "shot spike-3-outline"
        }
    }
    foreach ($pl in @(@(50, 'Big full speed'), @(51, 'Big paced'))) {
        $script:pl = $pl
        Step "playlist $($pl[0])" {
            $id = $script:pl[0]; $name = $script:pl[1]
            $t = Timed { Ok "select $id"; WaitFor 30000 "prepare.nameLabel.str=$name"; WaitFor 60000 'prepare.tracksTable.rows=5000' }
            $q = Timed { Quiet 1500 120000 }
            Check ((AllTrue $t[0]) -and (AllTrue $q[0])) "playlist $id ($name, 5,000 tracks): chosen, rows shown in $($t[1]) ms, quiet $($q[1]) ms later"
        }
    }
    Step 'the tracks scrolled' {
        $c2 = Count 'sent'
        $steps = @()
        $all = $true
        foreach ($row in 250, 1000, 2000, 3000, 4000, 4999, 0) {
            $t = Timed { Ok "scroll $row"; Quiet 200 3000 }
            if (-not (AllTrue $t[0])) { $all = $false }
            $steps += "$($row): $($t[1]) ms"
        }
        Write-Host "  the tracks' scroll steps: $($steps -join '; ')"
        $tracks = TabTable
        Check ($null -ne $tracks) "the tracks' table found by UIA in the Prepare tab (a DataGrid)"
        $sc = $false
        if ($tracks) { $sc = ScrollAll $tracks 'the tracks' }
        Check ($all -and $sc -and (Count 'sent') -eq $c2) "scrolling 5,000 tracks: every step lands, nothing sent to the core"
        Check (Shot 'spike-4-tracks') "shot spike-4-tracks"
    }
    Stop-App 'spike'
}

# ================================================================ the PT review's WPF-side cases

# one select (the folder's own row) and one folder (open false) at the same gen; then the folder shown
function CollapseChecks([string]$Marker, [int]$S0, [int]$F0, [string]$How) {
    $sel = (Count 'sent:select') - $S0
    $fol = (Count 'sent:folder') - $F0
    $after = @(SentAfter $Marker | Where-Object { $_.ev -eq 'select' -or $_.ev -eq 'folder' })
    $s = @($after | Where-Object { $_.ev -eq 'select' }); $f = @($after | Where-Object { $_.ev -eq 'folder' })
    $same = $s.Count -eq 1 -and $f.Count -eq 1 -and $s[0].gen -eq $f[0].gen -and $s[0].row -eq $f[0].row -and $f[0].open -eq $false
    Check ($sel -eq 1 -and $fol -eq 1 -and $same) "$How collapses the folder holding the selection: one select (its row) and one folder (open false) at one gen ($(($after | ForEach-Object { $_.ev + ':' + $_.row + '@' + $_.gen }) -join ' '))"
    Check ((WaitFor 3000 'prepare.info.str=A folder: choose one of its playlists.') -and (WaitFor 3000 'prepare.tracksTable.rows=0') -and (WaitFor 3000 'prepare.prepareBtn.en=0')) "after it ($How): the folder's line, no tracks, Prepare off"
}

function PT {
    "== the PT review's cases (trace mode)"
    Start-App 'trace' 'small'
    Step 'launch' {
        Check (Ok 'event 30000 windowDrawn 1' 40) "the window drawn"
        Check (Quiet 800 15000) "the app settles"
    }
    Step 'the prepare menus with the tabs (PM1)' {
        Check ((Ok 'menu menu.View.Prepare') -and (WaitFor 5000 'tab.sel=1')) "View > Prepare is on with RBSP_SHELL_TEST=prepare: the Prepare tab"
        Check ((Count 'shell.prepare_mismatch') -eq 0) "the core's Prepare menu agrees with the window's tabs (no shell.prepare_mismatch)"
    }
    Step 'tab.sel and the log one-shot' {
        Ok 'tab Prepare' | Out-Null
        Check (WaitFor 5000 'tab.sel=1') "tab.sel reads 1 on the Prepare tab"
        $t0 = Count 'sent:tab'
        $m = Ok 'menu menu.Manage.Show Log'
        Check ($m -and (WaitFor 5000 'tab.sel=2')) "Show Log from the Prepare tab: the Manage tab, by the log's one-shot"
        $q = Quiet 400 5000
        Check ($q -and (Count 'sent:tab') -eq $t0) "the log's one-shot under the Prepare tab sends no {`"ev`":`"tab`"}"
    }
    Step 'Friday chosen' {
        Ok 'tab Prepare' | Out-Null
        WaitFor 5000 'tab.sel=1' | Out-Null
        Ok 'expand 1' 5 | Out-Null
        Check ((Ok 'select 2') -and (WaitFor 15000 'prepare.tracksTable.rows=3') -and (Quiet 800 15000)) "Friday chosen: its 3 tracks"
    }
    Step 'refused days' {
        Check (WaitFor 5000 'prepare.until.days=10') "the picker shows the core's day (10 days)"
        foreach ($d in 0, 400) {
            $u0 = Count 'sent:until'
            Ok "until $d" | Out-Null
            $q = Quiet 400 5000
            Check ($q -and (Count 'sent:until') -eq $u0 + 1 -and (WaitFor 3000 'prepare.until.days=10')) "a refused day ($d days) is sent once and snaps back to the core's"
        }
    }
    Step 'a cleared day' {
        $root = TabRoot
        $picker = Item $root $CT::Custom 'Keep the stems until'
        if (-not $picker) { $picker = Item $root $CT::Edit 'Keep the stems until' }
        $edit = if ($picker -and $picker.Current.ControlType -eq $CT::Edit) { $picker } else { First $picker $CT::Edit }
        Check ($null -ne $edit) "the picker's text field found by UIA (to clear it)"
        if ($edit) {
            $edit.SetFocus()
            $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('')
            $tree = First $root $CT::Tree
            if ($tree) { $tree.SetFocus() }
            Check ((Quiet 400 5000) -and (WaitFor 3000 'prepare.until.days=10')) "a cleared day field snaps back to the core's"
        }
    }
    Manual "the calendar opened after midnight starts at the new tomorrow: with the app open, move the VM's clock past midnight, open the picker's calendar, see the new tomorrow as its first day"

    Step 'real input on the outline' {
        $tree = First (TabRoot) $CT::Tree
        $friday = Item $tree $CT::TreeItem 'Friday'
        Check ($null -ne $friday) "Friday's row found by UIA"
        if (-not $friday) { return }
        $f0 = Count 'sent:folder'
        DoubleClickAt (RowPoint $friday 31)
        Check ((Quiet 500 5000) -and (Count 'sent:folder') -eq $f0) "a double click on a leaf playlist sends no folder"
        $warm = Item $tree $CT::TreeItem 'Warm-up'
        $s0 = Count 'sent:select'; $f0 = Count 'sent:folder'
        ClickAt (RowPoint $warm 31)
        Check ((Quiet 500 5000) -and (Count 'sent:select') -eq $s0 + 1 -and (Count 'sent:folder') -eq $f0) "a click on Warm-up: exactly one select, no folder"
        Keys '{UP}'
        Check ((Quiet 500 5000) -and (Count 'sent:select') -eq $s0 + 2 -and (Count 'sent:folder') -eq $f0) "Up from Warm-up: exactly one more select, no folder"
        # the selection is now inside Sets (Late): a real expander click on Sets collapses the folder that holds it
        $setsRow = Item $tree $CT::TreeItem 'Sets'
        Check ($null -ne $setsRow) "Sets' row found by UIA (its expander)"
        if (-not $setsRow) { return }
        $exp = RowPoint $setsRow 9
        Ok 'mark expander-collapse' | Out-Null
        $s0 = Count 'sent:select'; $f0 = Count 'sent:folder'
        ClickAt $exp
        Check (Quiet 600 5000) "the expander click settles"
        CollapseChecks 'mark expander-collapse' $s0 $f0 'a real expander click'
        $s0 = Count 'sent:select'; $f0 = Count 'sent:folder'
        ClickAt $exp
        Check ((Quiet 600 5000) -and (Count 'sent:folder') -eq $f0 + 1 -and (Count 'sent:select') -eq $s0) "the expander clicked again: exactly one folder (open), no select"
    }
    Step 'the collapse by the probe' {
        Ok 'expand 1' 5 | Out-Null
        Check ((Ok 'select 2') -and (WaitFor 10000 'prepare.tracksTable.rows=3') -and (Quiet 600 10000)) "Friday chosen again"
        $s0 = Count 'sent:select'; $f0 = Count 'sent:folder'
        Ok 'collapse 1' | Out-Null
        Check (Quiet 600 5000) "the probe's collapse settles"
        CollapseChecks 'collapse 1' $s0 $f0 'the probe'
        Ok 'expand 1' | Out-Null
    }
    "VISUAL the same collapse in a Mac A trace ends with the same fields (folder before select there, allowed): compare $Results\pt-app-*.jsonl with the A trace (trace.py)"

    Step 'ST2 tab order' {
        Ok 'tab Status' | Out-Null
        WaitFor 5000 'tab.sel=0' | Out-Null
        Check (WaitFor 30000 'status.table.rows=2') "the Status tab: Friday and Warm-up"
        Ok 'select-set 0' | Out-Null
        Check ((WaitFor 15000 'status.againBtn.en=1') -and (WaitFor 15000 'status.stopBtn.en=1')) "a set selected: Prepare Again and Stop Keeping on"
        $row = All (TabTable) $CT::DataItem | Select-Object -First 1
        Check ($null -ne $row) "a set's row found by UIA (a DataItem in the Status tab)"
        if ($row) {
            $row.SetFocus()
            Keys '{TAB}'
            $f1 = $AE::FocusedElement.Current.Name
            Keys '{TAB}'
            $f2 = $AE::FocusedElement.Current.Name
            Check ($f1 -eq $stopName -and $f2 -eq $againName) "Tab: the table, then Stop Keeping, then Prepare Again (ST2: '$f1', '$f2')"
        }
    }
    Step 'ST3 high contrast' {
        $hc = [W13bContrast]::Set($true, $script:hc0)
        Start-Sleep -Seconds 3
        Check ($hc -and [W13bContrast]::On()) "high contrast on"
        $row = All (TabTable) $CT::DataItem | Select-Object -First 1
        Check ($null -ne $row) "a set's row for ST3's Status shots"
        if ($row) {
            $row.SetFocus(); Start-Sleep -Milliseconds 400
            Check (Shot 'pt-st3-status-focused') "shot pt-st3-status-focused"
            $again = Item (TabRoot) $CT::Button $againName
            if ($again) { $again.SetFocus() }
            Start-Sleep -Milliseconds 400
            Check ($null -ne $again -and (Shot 'pt-st3-status-unfocused')) "shot pt-st3-status-unfocused"
        }
        Ok 'tab Prepare' | Out-Null
        Check ((Ok 'select 2') -and (WaitFor 15000 'prepare.tracksTable.rows=3') -and (Quiet 600 10000)) "Friday chosen for ST3's Prepare shots"
        # friday-b: a row with a Note ("about 5 min"), the gray cell ST3 is about
        $trow = All (TabTable) $CT::DataItem | Select-Object -Skip 1 -First 1
        $parts = if ($trow) { @($trow.Current.Name -split ', ') } else { @() }
        Check ($trow -and $parts.Count -ge 3 -and $parts[0] -like '*friday-b*' -and $parts[-1].Length -gt 0) "friday-b's row has a Note ($(if ($trow) { $trow.Current.Name }))"
        if ($trow) {
            $trow.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
            $trow.SetFocus(); Start-Sleep -Milliseconds 400
            Check (Shot 'pt-st3-prepare-focused') "shot pt-st3-prepare-focused"
            $t2 = First (TabRoot) $CT::Tree; if ($t2) { $t2.SetFocus() }
            Start-Sleep -Milliseconds 400
            Check (Shot 'pt-st3-prepare-unfocused') "shot pt-st3-prepare-unfocused"
        }
        if ([W13bContrast]::On() -ne $script:contrast0) { [W13bContrast]::Set($script:contrast0, $script:hc0) | Out-Null }
        Start-Sleep -Seconds 3
        $st3 = @($script:shots | Where-Object { $_ -like 'pt-st3-*' })
        "VISUAL ST3: in high contrast a selected row's gray cell (the Status tab's State, the Prepare tab's Note) reads in the row's own highlight colour, focused and not: $(if ($st3.Count) { ($st3 | ForEach-Object { "$Results\$_" }) -join ', ' } else { 'no shot taken' })"
    }
    Step 'Return presses Prepare' {
        # last: a run starts, the fake's run-default waits until stopped
        Check ((Ok 'select 2') -and (WaitFor 20000 'prepare.prepareBtn.en=1')) "Friday's estimate done: Prepare on"
        $friday = Item (First (TabRoot) $CT::Tree) $CT::TreeItem 'Friday'
        Check ($null -ne $friday) "Friday's row found for Return"
        if (-not $friday) { return }
        $friday.SetFocus()
        Ok 'mark return' | Out-Null
        $k0 = Count 'sent:click'
        Keys '{ENTER}'
        $e = Ok "event 5000 sent:click $($k0 + 1)"
        $q = Quiet 400 5000
        $clicks = @(SentAfter 'mark return' | Where-Object { $_.ev -eq 'click' })
        Check ($e -and $q -and (Count 'sent:click') -eq $k0 + 1 -and $clicks.Count -eq 1 -and $clicks[0].id -eq 'prepare') "Return presses Prepare, exactly once ($(($clicks | ForEach-Object { $_.id }) -join ','))"
        Check (Shot 'pt-return') "shot pt-return"
    }
    if ($script:proc -and -not $script:proc.HasExited) { $script:proc.Kill() }
    Stop-App 'pt'
    "== the prepare menus without the tabs (PM1: the first release's window, a TEST exe)"
    Start-App 'trace' 'small' -NoPrepare
    Step 'the prepare menus without the tabs (PM1)' {
        Check (Ok 'event 30000 windowDrawn 1' 40) "the window drawn (no RBSP_SHELL_TEST=prepare)"
        Check (Quiet 800 15000) "the app settles (no RBSP_SHELL_TEST=prepare)"
        $a = Cmd 'menu menu.View.Prepare'
        Check ($a -like 'err disabled*') "View > Prepare is off without RBSP_SHELL_TEST=prepare ($a)"
        Check ((Count 'shell.prepare_mismatch') -eq 0) "the core's Prepare menu agrees with the window's tabs without them (no shell.prepare_mismatch)"
    }
    Stop-App 'pt-first'
    Manual "PT8's FlaUI suite (W13a's driver): the counts above come from SendInput, SendKeys and UIA patterns, not FlaUI; the FlaUI run repeats them when its driver lands"
}

# ================================================================ the run

$script:hc0 = [W13bContrast]::Flags(); $script:contrast0 = ($script:hc0 -band 1) -ne 0
try {
    Arm 9
    if ($Part -eq 'all' -or $Part -eq 'spike') { Step 'the spike' { Spike } }
    if ($Part -eq 'all' -or $Part -eq 'pt') { Step 'the PT cases' { PT } }
} finally {
    try { if ($script:proc -and -not $script:proc.HasExited) { $script:proc.Kill() } } catch { }
    try { if ([W13bContrast]::On() -ne $script:contrast0) { [W13bContrast]::Set($script:contrast0, $script:hc0) | Out-Null } } catch { }
    try { & $fixture -App $App -Clean | Out-Null } catch { }
    Disarm
}
"== w13b failures: $fail"
exit $fail
