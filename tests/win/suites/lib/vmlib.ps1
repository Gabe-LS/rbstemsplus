# vmlib.ps1: what every T17 suite step starts with (the suites' "include lib/vmlib.ps1"; vm-run.sh
# puts it after the prelude, so $VmRun is set). Windows PowerShell 5.1, as SYSTEM or as the user.
#   Check COND WHAT   one expectation: PASS/FAIL on its own line, counted
#   Skip WHY          the step can't run here (a missing artifact, a manual case): exit 77, SKIP
#   Missing WHAT      one case this step must cover couldn't run (an architecture's exe, a
#                     fixture, a fault folder): counted, the rest still runs
#   Done              the step's verdict: exit 1 when a check failed (a FAIL wins), else 77 (SKIP)
#                     when a case was Missing, else 0 (1 when nothing was checked)
#   Need REL WHY      a file in the share (\\Mac\win\REL) or Skip
# and the facts the suites share: the user's folders (also from SYSTEM), the install's layout, the
# one-liner in a child powershell.exe, Mark of the Web, the trace audit's snapshots.
$ErrorActionPreference = 'Stop'
$script:VmChecks = 0
$script:VmFails = 0
$script:VmMissing = 0

function Check([bool]$ok, [string]$what) {
    $script:VmChecks++
    if ($ok) { "PASS $what" } else { $script:VmFails++; "FAIL $what" }
}
function Skip([string]$why) { "SKIP $why"; exit 77 }
function Missing([string]$what) { $script:VmMissing++; "MISSING $what" }
function Done {
    "$($VmRun.Step): $script:VmChecks checks, $script:VmFails failed, $script:VmMissing missing"
    if ($script:VmFails -gt 0) { exit 1 }
    if ($script:VmMissing -gt 0) { "SKIP $script:VmMissing case(s) missing (above): never a pass"; exit 77 }
    if ($script:VmChecks -eq 0) { exit 1 }
    exit 0
}
function Share([string]$rel) { return '\\Mac\win\' + ($rel -replace '/', '\') }
function Need([string]$rel, [string]$why) {
    $p = Share $rel
    if (-not (Test-Path -LiteralPath $p)) { Skip "needs $rel in the share ($why)" }
    return $p
}

# the logged-in user's profile, also when this step runs as SYSTEM
function Get-UserProfile {
    $me = [Security.Principal.WindowsIdentity]::GetCurrent()
    if ($me.User.Value -ne 'S-1-5-18') { return $env:USERPROFILE }
    $name = (Get-CimInstance Win32_ComputerSystem).UserName
    if (-not $name) { return $null }
    $sid = (New-Object Security.Principal.NTAccount($name)).Translate([Security.Principal.SecurityIdentifier]).Value
    return (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$sid").ProfileImagePath
}
function Get-UserLocal { $p = Get-UserProfile; if ($p) { return Join-Path $p 'AppData\Local' } }

# the per-user install (release.md section 4)
function Get-Layout {
    $local = Get-UserLocal
    $apps = Join-Path $local 'Programs\RB Stems Plus'
    $roaming = Join-Path (Get-UserProfile) 'AppData\Roaming'
    return [pscustomobject]@{
        Local = $local
        Apps = $apps
        Stub = Join-Path $apps 'RB Stems Plus.exe'
        Current = Join-Path $apps 'current'
        Support = Join-Path $local 'rbstemsplus'
        Shortcut = Join-Path $roaming 'Microsoft\Windows\Start Menu\Programs\RB Stems Plus.lnk'
    }
}

# the apps folder as text (each file's path, size and time; `current`'s contents): what a refused
# install must leave as it was
function Get-AppTree {
    $l = Get-Layout
    if (-not (Test-Path -LiteralPath $l.Apps)) { return '-' }
    $lines = foreach ($f in Get-ChildItem -LiteralPath $l.Apps -Recurse -Force | Sort-Object FullName) {
        "$($f.FullName.Substring($l.Apps.Length)) $($f.Length) $($f.LastWriteTimeUtc.Ticks)"
    }
    $cur = if (Test-Path -LiteralPath $l.Current) { (Get-Content -Raw -LiteralPath $l.Current) } else { '-' }
    return ($lines -join "`n") + "`ncurrent=" + $cur
}

# the processes running from an exe under the apps folder, by full path (never by image name)
function Get-AppProcesses {
    $apps = (Get-Layout).Apps + '\'
    return @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
            $p = $null; try { $p = $_.Path } catch { }
            $p -and $p.StartsWith($apps, [StringComparison]::OrdinalIgnoreCase) })
}
# the app itself, never the stub (the same image name one folder up): a process of the current
# version folder's RB Stems Plus.exe (Apps\<current>\) that has a main window; $null when none
function Get-AppWindow {
    $l = Get-Layout
    if (-not (Test-Path -LiteralPath $l.Current)) { return $null }
    $ver = (Get-Content -Raw -LiteralPath $l.Current).Trim()
    if (-not $ver) { return $null }
    $exe = Join-Path (Join-Path $l.Apps $ver) 'RB Stems Plus.exe'
    return @(Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Where-Object {
            $p = $null; try { $p = $_.Path } catch { }
            $p -and $p.Equals($exe, [StringComparison]::OrdinalIgnoreCase) -and $_.MainWindowHandle -ne [IntPtr]::Zero }) | Select-Object -First 1
}
# the first start's welcome (core/flows.c startup_flow: T_WELCOME_TITLE, its one button
# T_CONTINUE), answered on the app's window: Checks that the app opened, that Continue was pressed
# and that the core recorded it (the support folder's "welcomed"). A statement, not a value (its
# Checks are its output): the app's process is left in $script:WelcomeApp ($null when none)
function Complete-Welcome([int]$secs = 90) {
    $l = Get-Layout
    $script:WelcomeApp = $null
    Check (Wait-Until { $script:WelcomeApp = Get-AppWindow; $null -ne $script:WelcomeApp } $secs) 'the app opened through the stub (its window)'
    if (-not $script:WelcomeApp) { return }
    Check (Invoke-UiNamed $script:WelcomeApp.Id (Get-CoreText 'T_CONTINUE') 60 'Button') 'the welcome answered: Continue'
    Check (Wait-Until { Test-Path -LiteralPath (Join-Path $l.Support 'welcomed') } 30) 'the welcome recorded (welcomed in the support folder)'
}
# the app's own processes (not the watcher), asked to close (their main window), then stopped
function Stop-App([int]$secs = 30) {
    $app = @(Get-AppProcesses | Where-Object { $_.ProcessName -eq 'RB Stems Plus' })
    foreach ($p in $app) { try { [void]$p.CloseMainWindow() } catch { } }
    $secs = [Math]::Max(5, (Get-WaitSecs $secs))   # (its own few seconds even at the deadline)
    foreach ($p in $app) { if (-not $p.WaitForExit($secs * 1000)) { "the app (pid $($p.Id)) didn't close: stopped"; Stop-Process -Id $p.Id -Force } }
}
# waits until cond is true, at most secs: whether it became true
# one deadline for every wait of the step (Wait-Until, Invoke-UiNamed, Find-UiText, Stop-App): its
# timeout less 30 s (at least half of it) from its start, so a wait that misses fails its own
# check before vm-run.sh's timeout ends the step as a TIMEOUT
$script:VmClock = [Diagnostics.Stopwatch]::StartNew()
$script:VmBudget = if ($VmRun.Secs) { [Math]::Max([int]$VmRun.Secs - 30, [int]([int]$VmRun.Secs / 2)) } else { 570 }
# Get-WaitSecs SECS: SECS, or what is left before the deadline when that is less
function Get-WaitSecs([int]$secs) {
    $left = [int]($script:VmBudget - $script:VmClock.Elapsed.TotalSeconds)
    return [Math]::Max(0, [Math]::Min($secs, $left))
}
function Wait-Until([scriptblock]$cond, [int]$secs) {
    $secs = Get-WaitSecs $secs
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($t.Elapsed.TotalSeconds -lt $secs) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 500 }
    return [bool](& $cond)
}

# UI Automation, for the dialogs a flow asks (its buttons by their texts, the core's): the first
# element named name in a window of process pid, invoked; whether it was found within secs
function Invoke-UiNamed([int]$procId, [string]$name, [int]$secs, [string]$type = '') {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [Windows.Automation.AutomationElement]
    $secs = Get-WaitSecs $secs
    $t = [Diagnostics.Stopwatch]::StartNew()
    do {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            $nc = New-Object Windows.Automation.PropertyCondition($A::NameProperty, $name)
            if ($type) {
                $tc = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::$type)
                $nc = New-Object Windows.Automation.AndCondition($nc, $tc)
            }
            $e = $w.FindFirst([Windows.Automation.TreeScope]::Descendants, $nc)
            if ($e) {
                $pat = $null
                if ($e.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) { $pat.Invoke(); return $true }
                if ($e.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$pat)) { $pat.Expand(); return $true }
            }
        }
        Start-Sleep -Milliseconds 500
    } while ($t.Elapsed.TotalSeconds -lt $secs)
    return $false
}
# whether a window of process pid shows a text element whose name matches pattern, within secs
function Find-UiText([int]$procId, [string]$pattern, [int]$secs) {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [Windows.Automation.AutomationElement]
    return Wait-Until {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
                if ($e.Current.Name -match $pattern) { return $true }
            }
        }
        $false
    } $secs
}
# a top-level window of process pid whose title is title, within secs
function Find-Window([int]$procId, [string]$title, [int]$secs) {
    return Wait-Until { try { (Get-Process -Id $procId -ErrorAction Stop).MainWindowTitle -eq $title } catch { $false } } $secs
}

# Explicit matrix runs select one architecture. Other runs retain native plus x64 on ARM64.
function Get-TestArchs {
    if ($VmRun.TestArch) { return $VmRun.TestArch }
    $native = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE
    if ($native -eq 'ARM64') { return @('arm64', 'x64') }
    return @('x64')
}
# a staged test exe (vm/<arch>/name) copied to C:\rbtest\bin\<arch>\ with the files beside it it
# needs (a DLL); its local path, or $null when it isn't staged. A copy already there with the same
# SHA-256 is kept: the image of an exe that just ran can stay mapped for a moment, and overwriting
# it then fails ("a file with a user-mapped section open", M4 round 1's volumes run)
function Get-TestExe([string]$arch, [string]$name, [string[]]$with = @()) {
    $src = Share "vm/$arch/$name"
    if (-not (Test-Path -LiteralPath $src)) { return $null }
    $dir = "C:\rbtest\bin\$arch"
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($f in @($name) + $with) { $s = Share "vm/$arch/$f"; $d = Join-Path $dir $f; if ((Test-Path -LiteralPath $s) -and -not ((Test-Path -LiteralPath $d) -and (Get-FileHash -LiteralPath $d).Hash -eq (Get-FileHash -LiteralPath $s).Hash)) { Copy-Item -LiteralPath $s -Destination $dir -Force } }
    return Join-Path $dir $name
}
# runs exe with args, its output straight to this step's stdout (not the pipeline): its exit code,
# and its output's text in $script:ExeOut
function Invoke-Exe([string]$exe, [string[]]$argv, [hashtable]$Env = @{}) {
    $saved = @{}
    foreach ($k in $Env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $Env[$k]) }
    $eap = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = @(& $exe @argv 2>&1 | ForEach-Object { [string]$_ })
        $rc = $LASTEXITCODE
        foreach ($x in $lines) { [Console]::Out.WriteLine("  $x") }
        $script:ExeOut = $lines -join "`n"
        return $rc
    } finally {
        $ErrorActionPreference = $eap
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
}

# Get-Served NAME: a file of the run's served folder (vm-run.sh run --serve DIR's, which isn't
# always the share's dist-test/) as its server sends it, as text; $null when it isn't served
function Get-Served([string]$name) {
    $w = New-Object Net.WebClient
    try { return [Text.Encoding]::UTF8.GetString($w.DownloadData($VmRun.Url + $name)) }
    catch { return $null }
    finally { $w.Dispose() }
}
# Test-Served NAME: whether the run's server sends NAME (a HEAD request: nothing downloaded)
function Test-Served([string]$name) {
    try {
        $r = [Net.WebRequest]::Create($VmRun.Url + $name)
        $r.Method = 'HEAD'
        $r.Timeout = 30000
        $r.GetResponse().Close()
        return $true
    } catch { return $false }
}

# the one-liner in a child powershell.exe 5.1 (the real `irm | iex` path, as release.md section 8
# asks), against base (the served TEST build) or GitHub when base is empty; its output
function Invoke-OneLiner([string]$base, [switch]$Constrained, [hashtable]$Env = @{}) {
    # UTF-8 both ways (the SAC line's U+203A would come back as ? or > through the OEM code page):
    # the child writes it, the parent reads it; first, before ConstrainedLanguage
    $cmd = 'try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }; $ProgressPreference = ''SilentlyContinue''; '
    if ($Constrained) { $cmd += '$ExecutionContext.SessionState.LanguageMode = ''ConstrainedLanguage''; ' }
    if ($base) { $cmd += 'irm ($env:RBSTEMSPLUS_BASE_URL + ''bootstrap.ps1'') | iex' }
    else { $cmd += 'irm https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/bootstrap.ps1 | iex' }
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
    $saved = @{}
    $all = @{ RBSTEMSPLUS_BASE_URL = $base } + $Env
    foreach ($k in $all.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $all[$k]) }
    $eap = $ErrorActionPreference
    $oe = $null
    try { $oe = [Console]::OutputEncoding; [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }
    try {
        $ErrorActionPreference = 'Continue'
        $o = (& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand $enc 2>&1 | Out-String)
    } finally {
        $ErrorActionPreference = $eap
        if ($oe) { try { [Console]::OutputEncoding = $oe } catch { } }
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    return ($o -replace "$([char]27)\[[0-9;?]*[A-Za-z]", '').Trim()
}

# Mark of the Web on a file: its Zone.Identifier stream, or $null
function Get-Motw([string]$path) {
    return Get-Item -LiteralPath $path -Stream * -ErrorAction SilentlyContinue | Where-Object { $_.Stream -eq 'Zone.Identifier' }
}

# a core text by id, from the staged repo's core/texts.def (the Windows column when there is one)
function Get-CoreText([string]$id) {
    $defs = [IO.File]::ReadAllText((Need 'repo/core/texts.def' 'vm-run.sh stage'))
    $m = [regex]::Match($defs, "TEXT\($id,\s*(?:all|win|mac),\s*`"((?:[^`"\\]|\\.)*)`",\s*`"((?:[^`"\\]|\\.)*)`"\)")
    if (-not $m.Success) { return $null }
    $t = if ($m.Groups[2].Value) { $m.Groups[2].Value } else { $m.Groups[1].Value }
    return $t.Replace('\"', '"')
}

# a menus.def title as UI Automation names its MenuItem: WPF's access key "_" dropped ("__" is one
# "_"), as the shell's Texts.Plain does
function Get-MenuText([string]$id) {
    $t = Get-CoreText $id
    if ($null -eq $t) { return $null }
    return [regex]::Replace($t, '_(.)', '$1')
}

# one of bootstrap.ps1's lines ($Texts: SacOn, Language, OldWindows, Elevated, AppOpen, Failed, ...),
# from the staged repo's scripts/bootstrap.ps1
function Get-BootText([string]$key) {
    $src = [IO.File]::ReadAllText((Need 'repo/scripts/bootstrap.ps1' 'vm-run.sh stage'))
    $m = [regex]::Match($src, "(?m)^\s+$key\s+=\s+(['`"])(.*)\1\s*$")
    if (-not $m.Success) { return $null }
    $t = $m.Groups[2].Value
    if ($m.Groups[1].Value -eq '"') { $t = $t.Replace('$Gt', [string][char]0x203A) } else { $t = $t.Replace("''", "'") }
    return $t
}

# ------------------------------------------------------------------ the trace audit (release.md section 8)

$AuditDir = 'C:\rbtest\audit'
$RunOnceKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
$CleanupKey = "values $RunOnceKey\RB Stems Plus cleanup*"   # the snapshot's key for them

# the after-exit's RunOnce cleanup values (one per temp folder it couldn't delete: "RB Stems Plus
# cleanup <16 hex>", w6/win-uninstaller's UN13): "NAME = DATA" each, by name; none, an empty array
function Get-CleanupRunOnce {
    if (-not (Test-Path -LiteralPath $RunOnceKey)) { return @() }
    $item = Get-Item -LiteralPath $RunOnceKey
    return @($item.GetValueNames() | Where-Object { $_.StartsWith('RB Stems Plus cleanup', [StringComparison]::OrdinalIgnoreCase) } |
            Sort-Object | ForEach-Object { "$_ = $($item.GetValue($_))" })
}

# what an install leaves: HKCU's records, the user's folders and files of ours, the temp copies,
# RunOnce; and Program Files\RB Stems Plus (Stems Cache) with every rekordbox version folder by hash
function Get-Snapshot {
    $l = Get-Layout
    $h = [ordered]@{}
    $keys = @(
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Notifications\Settings\io.github.rbstemsplus.app')
    foreach ($k in $keys) {
        $h["key $k"] = if (Test-Path -LiteralPath $k) { ((Get-ItemProperty -LiteralPath $k | Out-String) -split "`r?`n" | Where-Object { $_ -and $_ -notmatch '^PS' }) -join ';' } else { '-' }
    }
    foreach ($v in @(
            @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Run', 'RB Stems Plus Watcher'),
            @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run', 'RB Stems Plus Watcher'))) {
        $p = Get-ItemProperty -LiteralPath $v[0] -Name $v[1] -ErrorAction SilentlyContinue
        $h["value $($v[0])\$($v[1])"] = if ($p) { [string]$p.($v[1]) } else { '-' }
    }
    $c = @(Get-CleanupRunOnce)
    $h[$CleanupKey] = if ($c.Count) { $c -join '; ' } else { '-' }
    foreach ($d in @($l.Apps, $l.Support, $l.Shortcut, (Join-Path $l.Local 'Microsoft\CLR_v4.0\UsageLogs\RB Stems Plus.exe.log'),
            (Join-Path $l.Local 'Microsoft\CLR_v4.0_32\UsageLogs\RB Stems Plus.exe.log'), (Join-Path $env:ProgramFiles 'RB Stems Plus'))) {
        $h["path $d"] = if (Test-Path -LiteralPath $d) { 'present' } else { '-' }
    }
    $tmp = Join-Path $l.Local 'Temp'
    $h['temp copies'] = (@(Get-ChildItem -LiteralPath $tmp -Directory -Filter 'rbsp-uninstall-*' -ErrorAction SilentlyContinue) | ForEach-Object Name) -join ','
    # every rekordbox version folder by its tree's hash (each file's path and SHA-256), and their
    # list: no folder at all ('-') isn't an empty one ('none')
    $rbRoot = Join-Path $env:ProgramFiles 'rekordbox'
    if (-not (Test-Path -LiteralPath $rbRoot)) { $h['rekordbox versions'] = '-' }
    else {
        $vers = @(Get-ChildItem -LiteralPath $rbRoot -Directory -Force | Sort-Object Name)
        $h['rekordbox versions'] = if ($vers.Count) { ($vers | ForEach-Object Name) -join ',' } else { 'none' }
        $sha = [Security.Cryptography.SHA256]::Create()
        foreach ($rb in $vers) {
            $lines = foreach ($f in Get-ChildItem -LiteralPath $rb.FullName -Recurse -File -Force | Sort-Object FullName) {
                $s = [IO.File]::OpenRead($f.FullName)
                try { $x = [BitConverter]::ToString($sha.ComputeHash($s)).Replace('-', '') } finally { $s.Dispose() }
                $f.FullName.Substring($rb.FullName.Length) + ' ' + $x
            }
            $h["rekordbox tree $($rb.Name)"] = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes((@($lines) -join "`n")))).Replace('-', '')
        }
    }
    return $h
}
function Save-Snapshot([string]$name) {
    New-Item -ItemType Directory -Force $AuditDir | Out-Null
    $s = Get-Snapshot
    ($s | ConvertTo-Json) | Set-Content -LiteralPath (Join-Path $AuditDir "$name.json") -Encoding UTF8
    foreach ($k in $s.Keys) { "  $k = $($s[$k])" }
}
function Read-Snapshot([string]$name) {
    $f = Join-Path $AuditDir "$name.json"
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    $o = Get-Content -Raw -LiteralPath $f | ConvertFrom-Json
    $h = [ordered]@{}
    foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = [string]$p.Value }
    return $h
}

# ------------------------------------------------------------------ accounts and sessions (W4: UN8, K2)

# what the account and session steps call natively, compiled once per step: the debug-output
# listener (DBWIN_BUFFER, what DebugView reads: the after-exit's lines, w_log), OpenProcess's
# error for a pid from this token, and an account right granted through LSA (as SYSTEM)
function Add-VmNative {
    if ('RbspVm.Native' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Threading;
namespace RbspVm {
public static class Native {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateEventW(IntPtr sa, bool manual, bool initial, string name);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileMappingW(IntPtr file, IntPtr sa, uint protect, uint high, uint low, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr MapViewOfFile(IntPtr map, uint access, uint high, uint low, UIntPtr bytes);
    [DllImport("kernel32.dll")] static extern bool UnmapViewOfFile(IntPtr view);
    [DllImport("kernel32.dll")] static extern bool SetEvent(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

    static IntPtr ready = IntPtr.Zero, data = IntPtr.Zero, map = IntPtr.Zero, view = IntPtr.Zero;
    static Thread reader;
    static volatile bool stopping;
    static readonly List<string> lines = new List<string>();

    // the debug output of this session's processes from now on, each line "<pid> <text>": null
    // when listening, else why not (another listener, DebugView, already holds the buffer)
    public static string DebugStart() {
        if (reader != null) return null;
        ready = CreateEventW(IntPtr.Zero, false, false, "DBWIN_BUFFER_READY");
        int e = Marshal.GetLastWin32Error();
        if (ready == IntPtr.Zero) return "DBWIN_BUFFER_READY couldn't be made (error " + e + ")";
        if (e == 183) { DebugClose(); return "another debug-output listener runs (DBWIN_BUFFER_READY exists)"; }
        data = CreateEventW(IntPtr.Zero, false, false, "DBWIN_DATA_READY");
        e = Marshal.GetLastWin32Error();
        if (data == IntPtr.Zero) { DebugClose(); return "DBWIN_DATA_READY couldn't be made (error " + e + ")"; }
        map = CreateFileMappingW(new IntPtr(-1), IntPtr.Zero, 4, 0, 4096, "DBWIN_BUFFER");
        e = Marshal.GetLastWin32Error();
        if (map == IntPtr.Zero) { DebugClose(); return "DBWIN_BUFFER couldn't be made (error " + e + ")"; }
        view = MapViewOfFile(map, 4, 0, 0, new UIntPtr(4096));
        if (view == IntPtr.Zero) { e = Marshal.GetLastWin32Error(); DebugClose(); return "DBWIN_BUFFER couldn't be mapped (error " + e + ")"; }
        stopping = false;
        reader = new Thread(Read);
        reader.IsBackground = true;
        reader.Start();
        return null;
    }
    static void Read() {
        byte[] b = new byte[4092];
        SetEvent(ready);
        while (!stopping) {
            if (WaitForSingleObject(data, 200) != 0) continue;
            int pid = Marshal.ReadInt32(view);
            Marshal.Copy(new IntPtr(view.ToInt64() + 4), b, 0, b.Length);
            int n = Array.IndexOf(b, (byte)0);
            if (n < 0) n = b.Length;
            string s = Encoding.UTF8.GetString(b, 0, n).TrimEnd('\r', '\n');
            if (s.Length > 0) lock (lines) lines.Add(pid + " " + s);
            SetEvent(ready);
        }
    }
    public static string[] DebugLines() { lock (lines) return lines.ToArray(); }
    public static void DebugStop() {
        stopping = true;
        if (reader != null) reader.Join(2000);
        reader = null;
        DebugClose();
    }
    static void DebugClose() {
        if (view != IntPtr.Zero) UnmapViewOfFile(view);
        foreach (IntPtr h in new IntPtr[] { map, data, ready }) if (h != IntPtr.Zero) CloseHandle(h);
        view = map = data = ready = IntPtr.Zero;
    }

    // OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) from this token: 0 when it opens, else its error
    public static int OpenError(int pid) {
        IntPtr h = OpenProcess(0x1000, false, pid);
        if (h == IntPtr.Zero) return Marshal.GetLastWin32Error();
        CloseHandle(h);
        return 0;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool QueryFullProcessImageNameW(IntPtr h, uint flags, StringBuilder name, ref uint size);
    // the image path of pid as PROCESS_QUERY_LIMITED_INFORMATION reads it (the uninstall's own
    // way: no PROCESS_VM_READ, which Process.Path needs, and a process of another logon session
    // of the same user doesn't grant), or null
    public static string ImagePath(int pid) {
        IntPtr h = OpenProcess(0x1000, false, pid);
        if (h == IntPtr.Zero) return null;
        try {
            StringBuilder b = new StringBuilder(32768);
            uint n = (uint)b.Capacity;
            return QueryFullProcessImageNameW(h, 0, b, ref n) ? b.ToString(0, (int)n) : null;
        } finally { CloseHandle(h); }
    }

    [StructLayout(LayoutKind.Sequential)]
    struct LsaString { public ushort Length; public ushort MaximumLength; public IntPtr Buffer; }
    [StructLayout(LayoutKind.Sequential)]
    struct LsaAttributes { public int Length; public IntPtr RootDirectory; public IntPtr ObjectName; public uint Attributes; public IntPtr SecurityDescriptor; public IntPtr SecurityQualityOfService; }
    [DllImport("advapi32.dll")] static extern uint LsaOpenPolicy(IntPtr system, ref LsaAttributes attributes, uint access, out IntPtr policy);
    [DllImport("advapi32.dll")] static extern uint LsaAddAccountRights(IntPtr policy, byte[] sid, LsaString[] rights, uint count);
    [DllImport("advapi32.dll")] static extern uint LsaClose(IntPtr policy);
    [DllImport("advapi32.dll")] static extern int LsaNtStatusToWinError(uint status);

    // the account right (SeBatchLogonRight, ...) granted to the SID: 0, else the Win32 error
    public static int GrantRight(string sid, string right) {
        SecurityIdentifier s = new SecurityIdentifier(sid);
        byte[] b = new byte[s.BinaryLength];
        s.GetBinaryForm(b, 0);
        LsaAttributes a = new LsaAttributes();
        a.Length = Marshal.SizeOf(typeof(LsaAttributes));
        IntPtr p;
        uint st = LsaOpenPolicy(IntPtr.Zero, ref a, 0x00000810, out p);   // POLICY_CREATE_ACCOUNT | POLICY_LOOKUP_NAMES
        if (st != 0) return LsaNtStatusToWinError(st);
        LsaString[] r = new LsaString[1];
        r[0].Buffer = Marshal.StringToHGlobalUni(right);
        r[0].Length = (ushort)(right.Length * 2);
        r[0].MaximumLength = (ushort)(right.Length * 2 + 2);
        try { st = LsaAddAccountRights(p, b, r, 1); }
        finally { Marshal.FreeHGlobal(r[0].Buffer); LsaClose(p); }
        return st == 0 ? 0 : LsaNtStatusToWinError(st);
    }
}
}
'@
}

# the debug output (the after-exit's log: OutputDebugStringA, no log file) of this step's session
# from now on. A statement: listening, or Missing (another listener, or none could start), with
# $script:DebugOn saying which
$script:DebugOn = $false
function Start-DebugLog {
    Add-VmNative
    $why = [RbspVm.Native]::DebugStart()
    $script:DebugOn = -not $why
    if ($why) { Missing "the debug output (the after-exit's log): $why" } else { 'listening to the debug output (DBWIN)' }
}
# its lines so far, "<pid> <text>" each
function Get-DebugLines { if ($script:DebugOn) { return @([RbspVm.Native]::DebugLines()) }; return @() }
# the lines of process procId, its pid taken off
function Get-DebugLinesOf([int]$procId) {
    $p = "$procId "
    return @(Get-DebugLines | Where-Object { $_.StartsWith($p) } | ForEach-Object { $_.Substring($p.Length) })
}
function Stop-DebugLog { if ($script:DebugOn) { [RbspVm.Native]::DebugStop() } }

# a test account's password file lives in the run's own folder in the VM, in a folder only SYSTEM
# can open; Remove-TestSecrets deletes it (and the VM is reverted after the run)
function Get-SecretDir { return "C:\rbtest\job\$($VmRun.Run)\secret" }
# New-TestAccount NAME, as SYSTEM: a local standard account (Users). Its password is made here from
# 24 random bytes, never printed and never on a command line: it goes to its file only, for
# Start-TaskAs. The file's path
function New-TestAccount([string]$name) {
    $f = New-SecretPassword $name
    $sec = ConvertTo-SecureString ([IO.File]::ReadAllText($f)) -AsPlainText -Force
    New-LocalUser -Name $name -Password $sec -PasswordNeverExpires -AccountNeverExpires -Description 'RB Stems Plus VM test account' | Out-Null
    # Users, by its SID (any display language); already a member is fine
    try { Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $name } catch { if ($_.FullyQualifiedErrorId -notlike 'MemberExists*') { throw } }
    return $f
}
# Set-TestPassword NAME, as SYSTEM: an existing local account (the signed-in user's, whose password
# is empty in the snapshots: Windows refuses a task logon without one, S4U included, "Limit local
# account use of blank passwords to console logon only") given a new random password, made and
# kept as New-TestAccount's; the signed-in session goes on, and an autologon naming the account
# gets the new password. In this reverted VM only. The file's path
function Set-TestPassword([string]$name) {
    $f = New-SecretPassword $name
    $sec = ConvertTo-SecureString ([IO.File]::ReadAllText($f)) -AsPlainText -Force
    Set-LocalUser -Name $name -Password $sec
    $wl = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
    $w = Get-ItemProperty -LiteralPath $wl -ErrorAction SilentlyContinue
    if ($w -and $w.AutoAdminLogon -eq '1' -and $w.DefaultUserName -ieq $name) { Set-ItemProperty -LiteralPath $wl -Name DefaultPassword -Value ([IO.File]::ReadAllText($f)) }
    return $f
}
# New-SecretPassword NAME: a password made from 24 random bytes into NAME.pw in the SYSTEM-only
# secret folder (never printed, never on a command line). The file's path
function New-SecretPassword([string]$name) {
    $dir = Get-SecretDir
    New-Item -ItemType Directory -Force $dir | Out-Null
    $sys = New-Object Security.Principal.SecurityIdentifier 'S-1-5-18'
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetOwner($sys)
    $acl.SetAccessRuleProtection($true, $false)
    $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sys, 'FullControl', 'ContainerInherit, ObjectInherit', 'None', 'Allow')))
    Set-Acl -LiteralPath $dir -AclObject $acl
    $b = New-Object byte[] 24
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    $rng.GetBytes($b)
    $rng.Dispose()
    $f = Join-Path $dir "$name.pw"
    # every character class Windows' complexity rule counts, whatever the random part holds
    [IO.File]::WriteAllText($f, 'Rb-7' + [Convert]::ToBase64String($b), [Text.Encoding]::ASCII)
    [Array]::Clear($b, 0, $b.Length)
    return $f
}
# whether the run's password files are gone (deleted now)
function Remove-TestSecrets {
    $dir = Get-SecretDir
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    return -not (Test-Path -LiteralPath $dir)
}
# Grant-BatchLogon ACCOUNT, as SYSTEM: "Log on as a batch job" (SeBatchLogonRight), which a task that
# runs whether the user is logged on or not needs (Task Scheduler may grant it by itself): the
# Win32 error, 0 granted
function Grant-BatchLogon([string]$account) {
    Add-VmNative
    $sid = (New-Object Security.Principal.NTAccount($account)).Translate([Security.Principal.SecurityIdentifier]).Value
    return [RbspVm.Native]::GrantRight($sid, 'SeBatchLogonRight')
}
# Start-TaskAs USER PWFILE EXE ARGS TASK, as SYSTEM: the scheduled task TASK runs EXE ARGS as USER
# "whether the user is logged on or not" (session 0, its own logon session) with the limited token,
# as the user's own processes have (no elevation: they stay openable from the user's other
# sessions): LogonType Password, the password read from PWFILE in this process (never on a command
# line, never printed), or S4U (no password) when PWFILE is ''. Started; a statement (its lines say
# the task's state). Stop-TaskAs ends and removes it
function Start-TaskAs([string]$user, [string]$pwFile, [string]$exe, [string]$argv, [string]$task) {
    $e = Grant-BatchLogon $user
    "batch logon right for ${user}: $(if ($e) { "not granted here (error $e)" } else { 'granted' })"
    $a = if ($argv) { New-ScheduledTaskAction -Execute $exe -Argument $argv } else { New-ScheduledTaskAction -Execute $exe }
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Hours 1)
    if ($pwFile) {
        $pw = [IO.File]::ReadAllText($pwFile)
        try { Register-ScheduledTask -TaskName $task -Action $a -Settings $s -User $user -Password $pw -RunLevel Limited -Force | Out-Null }
        finally { $pw = $null }
    } else {
        $p = New-ScheduledTaskPrincipal -UserId $user -LogonType S4U -RunLevel Limited
        Register-ScheduledTask -TaskName $task -Action $a -Principal $p -Settings $s -Force | Out-Null
    }
    Start-ScheduledTask -TaskName $task
    Start-Sleep -Seconds 2
    $i = Get-ScheduledTaskInfo -TaskName $task -ErrorAction SilentlyContinue
    "task $task as $user ($(if ($pwFile) { 'password' } else { 'S4U' }) logon): $((Get-ScheduledTask -TaskName $task).State), last result 0x$('{0:X8}' -f [int64]$i.LastTaskResult)"
}
function Stop-TaskAs([string]$task) {
    Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
}

# ------------------------------------------------------------------ the UI driver (by-hand plan W1)
# rbui (FlaUI over UIA3, tests/win/uia/rbui) and sysdesk (tests/win/uia/sysdesk.c), staged by
# vm-run.sh stage into vm/uia/rbui/ and vm/<arch>/sysdesk.exe. sysdesk runs rbui as SYSTEM on the
# secure desktop (winsta0\winlogon) of the console session, which is how UAC's consent prompt and
# the sign-out block screen are read and answered; it is started from a SYSTEM scheduled task, so
# it outlives the sys step that starts it and answers what a later user step causes. Its output
# and exit code land in C:\rbtest\uia\out\NAME.txt(.done), which the user steps read too.
$UiaDir = 'C:\rbtest\uia'
$Rbui = Join-Path $UiaDir 'rbui\rbui.exe'
$SysDesk = Join-Path $UiaDir 'sysdesk.exe'
$SignoutMark = 'C:\rbtest\signout.mark'   # vm-run.sh's "wait SECS NAME logon" reads it

# Install-Uia (SYSTEM): rbui and this machine's sysdesk copied from the share into C:\rbtest\uia,
# which only SYSTEM and Administrators may change (what sysdesk is given runs as SYSTEM) and
# Users may read (their steps read out\): whether both are there
function Install-Uia {
    $arch = if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
    $src = Share 'vm/uia/rbui'
    $sd = Share "vm/$arch/sysdesk.exe"
    if (-not (Test-Path -LiteralPath (Join-Path $src 'rbui.exe')) -or -not (Test-Path -LiteralPath $sd)) { return $false }
    New-Item -ItemType Directory -Force (Join-Path $UiaDir 'rbui'), (Join-Path $UiaDir 'out') | Out-Null
    & icacls.exe $UiaDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
    Copy-Item -Path (Join-Path $src '*') -Destination (Join-Path $UiaDir 'rbui') -Force
    Copy-Item -LiteralPath $sd -Destination $SysDesk -Force
    return (Test-Path -LiteralPath $Rbui) -and (Test-Path -LiteralPath $SysDesk)
}

# one argument as CommandLineToArgvW reads it back
function ConvertTo-ArgText([string]$a) {
    if ($a -and $a -notmatch '[\s"]') { return $a }
    return '"' + ($a -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

# Start-SystemTask NAME EXE ARGV SECS (SYSTEM): EXE run once as SYSTEM by a scheduled task
# (rbsp-ui-NAME), so it outlives this step; at most SECS + 120 s
function Start-SystemTask([string]$name, [string]$exe, [string[]]$argv, [int]$secs) {
    $task = "rbsp-ui-$name"
    Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
    $a = New-ScheduledTaskAction -Execute $exe -Argument (($argv | ForEach-Object { ConvertTo-ArgText ([string]$_) }) -join ' ')
    $p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds ($secs + 120))
    Register-ScheduledTask -TaskName $task -Action $a -Principal $p -Settings $s -Force | Out-Null
    Start-ScheduledTask -TaskName $task
}

# Start-SystemScript NAME SCRIPT SECS (SYSTEM): a PowerShell script (text) run once as SYSTEM by a
# scheduled task, as Start-SystemTask (-EncodedCommand: no quoting, any number of lines)
function Start-SystemScript([string]$name, [string]$script, [int]$secs) {
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
    Start-SystemTask $name "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" @('-NoProfile', '-NonInteractive', '-EncodedCommand', $enc) $secs
}

# Start-SysDesk NAME DESKTOP AS SECS EXE ARGV (SYSTEM): sysdesk runs EXE ARGV in the console
# session, on winsta0\DESKTOP (winlogon or default), as AS (system or user), at most SECS; its
# output in C:\rbtest\uia\out\NAME.txt, its code in NAME.txt.done (Wait-SysDesk)
function Start-SysDesk([string]$name, [string]$desktop, [string]$as, [int]$secs, [string]$exe, [string[]]$argv) {
    $out = Join-Path $UiaDir "out\$name.txt"
    # an earlier NAME in its last seconds (a consent responder's --after watch) ends by itself first:
    # stopped mid-write, its output could stay behind and be read as the new one's (VM round 3)
    if ((Test-Path -LiteralPath $out) -and -not (Test-Path -LiteralPath "$out.done")) {
        Trace-Arm $VmRun.Step "sysdesk ${name}: an earlier one still runs: waiting up to 45 s for its end"
        [void](Wait-Until { Test-Path -LiteralPath "$out.done" } 45)
    }
    Trace-Arm $VmRun.Step "sysdesk ${name}: stopping an earlier one"
    Stop-SysDesk $name
    $gone = Wait-Until {
        Remove-Item -LiteralPath $out, "$out.done" -Force -ErrorAction SilentlyContinue
        -not (Test-Path -LiteralPath $out) -and -not (Test-Path -LiteralPath "$out.done")
    } 30
    Trace-Arm $VmRun.Step "sysdesk ${name}: the earlier output removed: $gone"
    Check $gone "sysdesk ${name}: the earlier one's output removed ($out)"
    Trace-Arm $VmRun.Step "sysdesk ${name}: starting its task"
    Start-SystemTask $name $SysDesk (@('--desktop', $desktop, '--as', $as, '--out', $out, '--timeout', [string]$secs, '--', $exe) + $argv) $secs
    Trace-Arm $VmRun.Step "sysdesk ${name}: task started"
    "sysdesk $name started: $exe $($argv -join ' ') ($desktop, as $as, at most $secs s)"
}

# Stop-SysDesk NAME (SYSTEM): an earlier NAME still running (a responder whose prompt never came)
# ended with everything it started, its task removed. The process tree is read before the task is
# stopped: stopping it ends sysdesk alone, and its rbui, left without a parent, went on answering
# (and holding NAME.txt) where the next responder should (VM run 20261117-171603, WC10)
function Stop-SysDesk([string]$name) {
    $out = Join-Path $UiaDir "out\$name.txt"
    $all = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
    Stop-ScheduledTask -TaskName "rbsp-ui-$name" -ErrorAction SilentlyContinue
    foreach ($d in @($all | Where-Object { $_.Name -eq 'sysdesk.exe' -and [string]$_.CommandLine -like "*$out*" })) {
        # what it started, and what those started (rbui's --then), deepest last
        $tree = @($d.ProcessId); $i = 0
        while ($i -lt $tree.Count) { $p = $tree[$i]; $tree += @($all | Where-Object { $_.ParentProcessId -eq $p -and $tree -notcontains $_.ProcessId } | ForEach-Object { $_.ProcessId }); $i++ }
        foreach ($c in @($tree | Select-Object -Skip 1)) { Stop-Process -Id $c -Force -ErrorAction SilentlyContinue }
        Stop-Process -Id $d.ProcessId -Force -ErrorAction SilentlyContinue
        [Console]::Out.WriteLine("sysdesk ${name}: an earlier one (pid $($d.ProcessId)) ended")
    }
    Unregister-ScheduledTask -TaskName "rbsp-ui-$name" -Confirm:$false -ErrorAction SilentlyContinue
}

# Read-Shared PATH: a file another process may still be writing (a responder's output, a hold's
# log), read without denying it write or delete; '' when there is none or it can't be opened yet
function Read-Shared([string]$path) {
    try {
        $fs = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        try { return (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
    } catch { return '' }
}
# Test-PendingOnly PATH (either side): PATH and everything under it are boot-time deletes
# (PendingFileRenameOperations(2), MoveFileExW's delay-until-reboot: Windows 11 writes them
# "*1\??\C:\..."), each item said; whether so (a path that isn't there: $false)
function Test-PendingOnly([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $sm = Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -ErrorAction SilentlyContinue
    $del = @(@($sm.PendingFileRenameOperations) + @($sm.PendingFileRenameOperations2) | Where-Object { $_ } | ForEach-Object { ($_ -replace '^\*\d+', '') -replace '^\\\?\?\\', '' })
    $items = @($path) + @(Get-ChildItem -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    foreach ($i in $items) { [Console]::Out.WriteLine("  $i $(if ($del -contains $i) { '(deleted at the next restart)' } else { '(NOT scheduled)' })") }
    return @($items | Where-Object { $del -notcontains $_ }).Count -eq 0
}
# Trace-Arm STEP TEXT (SYSTEM): one timed line in C:\rbtest\uia\out\arm.trace, the UAC suites'
# record of where an arm step (and the responders it starts) got to; Show-ArmTrace says it
$ArmTrace = Join-Path $UiaDir 'out\arm.trace'
function Trace-Arm([string]$step, [string]$text) {
    try {
        $fs = New-Object IO.FileStream($ArmTrace, [IO.FileMode]::Append, [IO.FileAccess]::Write, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        try { $w = New-Object IO.StreamWriter($fs); $w.WriteLine("$((Get-Date).ToString('HH:mm:ss.fff')) ${step}: $text"); $w.Flush() } finally { $fs.Dispose() }
    } catch { }
}
# Show-ArmTrace [LINES] (either side): arm.trace's last LINES lines on this step's stdout; the
# last line (an arm step that got to its end says "armed")
function Show-ArmTrace([int]$lines = 8) {
    $t = @((Read-Shared $ArmTrace) -split "`r?`n" | Where-Object { $_ })
    foreach ($l in @($t | Select-Object -Last $lines)) { [Console]::Out.WriteLine("  [arm.trace] $l") }
    if ($t.Count) { return $t[-1] } else { return '' }
}
# Wait-SysDesk NAME SECS (SYSTEM or the user): NAME's end waited for, at most SECS: @{ Done; Code;
# Text } (Code $null and Done $false while it runs); its output said on this step's stdout; the
# task removed once it ended (SYSTEM)
function Wait-SysDesk([string]$name, [int]$secs) {
    $out = Join-Path $UiaDir "out\$name.txt"
    $done = Wait-Until { Test-Path -LiteralPath "$out.done" } $secs
    $text = Read-Shared $out
    foreach ($l in ($text -split "`r?`n")) { if ($l) { [Console]::Out.WriteLine("  [$name] $l") } }
    $code = $null
    if ($done) {
        $first = [string](Get-Content -LiteralPath "$out.done" -TotalCount 1)
        if ($first.Trim() -match '^-?[0-9]+$') { $code = [int]$first.Trim() }
        try { Unregister-ScheduledTask -TaskName "rbsp-ui-$name" -Confirm:$false -ErrorAction SilentlyContinue } catch { }   # (the user may not)
    }
    return [pscustomobject]@{ Done = $done; Code = $code; Text = $text }
}

# Start-Consent ANSWER [SECS] [NAME] [-Delay S] [-After S] [-Details] [-ThenLogoff] (SYSTEM): rbui
# on the secure desktop waits up to SECS for UAC's consent prompt, reads it and answers yes or
# no, but only a prompt that names the worker's file, RB Stems Plus.exe (release.md section 8: an
# unknown publisher's prompt names the program); any other prompt is refused (exit 4). -Delay
# holds the answer (a step checks the chain held meanwhile); -After watches that many seconds more
# and refuses a second prompt (exit 5); -Details opens "Show more details" (the program's
# location). -ThenLogoff: after the yes, as soon as the worker runs ($WorkerImageRe, polled every
# 50 ms), the user signs out (sysdesk --as user, Get-LogoffCommand: ExitWindowsEx(EWX_LOGOFF), its
# output in out\logoff.txt), the sign-out mark written now: the sign-out comes while the worker
# runs, which a later step can't time (no -After then: the session ends). Not shutdown.exe /l: in
# VM round 4 its sign-out ended the session about 5 s after the app's veto, killing the held
# worker; ExitWindowsEx's waited for the app
$WorkerImageRe = '\\RB Stems Plus\\bin\\[0-9a-f]{12}\\RB Stems Plus\.exe$|\\admin\\RB Stems Plus\.exe$'
function Start-Consent([ValidateSet('yes', 'no')][string]$answer, [int]$secs = 300, [string]$name = 'consent', [int]$Delay = 0, [int]$After = 20, [switch]$Details, [switch]$ThenLogoff) {
    if ($ThenLogoff) { $After = 0 }
    $argv = @('consent', $answer, '--expect', 'RB Stems Plus.exe', '--timeout', [string]$secs, '--delay', [string]$Delay, '--after', [string]$After)
    if ($Details) { $argv += '--details' }
    if ($ThenLogoff) {
        Save-SignoutMark
        $lo = Join-Path $UiaDir 'out\logoff.txt'
        Remove-Item -LiteralPath $lo, "$lo.done" -Force -ErrorAction SilentlyContinue
        $argv += @('--then', $SysDesk, '--then-when', $WorkerImageRe, '--then-args',
            ("--desktop default --as user --out $lo --timeout 60 -- " + (Get-LogoffCommand)))
    }
    Start-SysDesk $name 'winlogon' 'system' ($secs + $Delay + $After + 60) $Rbui $argv
}

# Get-LogoffArgv: the sign-out every suite starts as the user, as @(EXE, ARGS...): powershell.exe
# calling ExitWindowsEx(EWX_LOGOFF) itself, no flag (not EWX_FORCE, not EWX_FORCEIFHUNG), so an
# app's veto holds it and Windows shows its block screen. Its exit code is ExitWindowsEx's error
# (0: started). Get-LogoffCommand: the same as one command line (rbui's --then-args)
function Get-LogoffArgv {
    $ps = '$ProgressPreference = ''SilentlyContinue''; Add-Type -Namespace RbspLogoff -Name U -MemberDefinition ''[DllImport("user32.dll", SetLastError = true)] public static extern bool ExitWindowsEx(uint flags, uint reason);''; ' +
        'if ([RbspLogoff.U]::ExitWindowsEx(0, 0)) { ''ExitWindowsEx(EWX_LOGOFF): started''; exit 0 }; $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error(); "ExitWindowsEx(EWX_LOGOFF): error $e"; exit $e'
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($ps))
    return @((Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'), '-NoProfile', '-NonInteractive', '-EncodedCommand', $enc)
}
function Get-LogoffCommand {
    $a = Get-LogoffArgv
    return (ConvertTo-ArgText $a[0]) + ' ' + ($a[1..($a.Count - 1)] -join ' ')
}

# Read-Consent [NAME] [SECS] (either side): the consent responder's end: @{ Done; Code; Text;
# Answered (yes, no or ''); Prompt (the prompt's text) }
function Read-Consent([string]$name = 'consent', [int]$secs = 120) {
    $r = Wait-SysDesk $name $secs
    $m = [regex]::Match($r.Text, '(?m)^\d\d:\d\d:\d\d\.\d{3} consent: answered (yes|no):')
    $p = [regex]::Match($r.Text, '(?m)^\d\d:\d\d:\d\d\.\d{3} consent: text: (.*)$')
    $r | Add-Member Answered $(if ($m.Success) { $m.Groups[1].Value } else { '' })
    $r | Add-Member Prompt $(if ($p.Success) { $p.Groups[1].Value.Trim() } else { '' })
    return $r
}

# Start-BlockScreen ACTION [SECS] [NAME] (SYSTEM): rbui on the secure desktop waits up to SECS for
# Windows' block screen (an app holds the sign-out), reads it (it must name the app) and presses
# Cancel or "Sign out anyway" (ACTION cancel or anyway; read presses nothing), its screenshot in
# out\NAME.png. The sign-out cases start it from their settle step once the sign-out began (one
# started before it never found the screen: VM round 4)
function Start-BlockScreen([ValidateSet('read', 'cancel', 'anyway')][string]$action, [int]$secs = 180, [string]$name = 'blockscreen') {
    Remove-Item -LiteralPath (Join-Path $UiaDir "out\$name.png") -Force -ErrorAction SilentlyContinue   # (an earlier case's)
    Start-SysDesk $name 'winlogon' 'system' ($secs + 60) $Rbui @('blockscreen', $action, '--expect', (Get-CoreText 'T_APP_NAME'), '--timeout', [string]$secs, '--shot', (Join-Path $UiaDir "out\$name.png"))
}

# the sign-out mark: the time vm-run.sh's wait step and Wait-UserLogon count a new sign-in from
function Save-SignoutMark {
    New-Item -ItemType Directory -Force (Split-Path $SignoutMark) | Out-Null
    Set-Content -LiteralPath $SignoutMark -Value ([DateTime]::UtcNow.Ticks) -Encoding ASCII
}

# Start-Logoff (SYSTEM): the user signs out as Start > Sign out does (ExitWindowsEx EWX_LOGOFF,
# not forced: Get-LogoffArgv run as the user in the console session, through sysdesk, its output
# in out\logoff.txt), after the sign-out mark. Not shutdown.exe /l: its sign-out ended the session
# about 5 s after an app's veto (VM round 4). The next suite line is "wait SECS NAME logon"
# unless the sign-out is cancelled
function Start-Logoff {
    Save-SignoutMark
    $a = Get-LogoffArgv
    Start-SysDesk 'logoff' 'default' 'user' 60 $a[0] $a[1..($a.Count - 1)]
}

# Start-Restart [SECS] (SYSTEM): Windows restarts SECS (default 5) seconds after this step ends,
# from a SYSTEM task (shutdown.exe /r /t 0: apps are asked, not forced), after the sign-out mark;
# the next suite line is "wait SECS NAME logon"
function Start-Restart([int]$secs = 5) {
    Save-SignoutMark
    Start-SystemScript 'restart' "Start-Sleep -Seconds $secs; & `"$env:WINDIR\System32\shutdown.exe`" /r /t 0" 60
    "restart in $secs s"
}

# Set-AutoLogon (SYSTEM): Winlogon signs the current user in by itself after a sign-out or a
# restart (AutoAdminLogon 1, ForceAutoLogon 1, DefaultUserName, DefaultPassword). The password is
# made here: random, set on the signed-in account (a local one: a Microsoft account's can't be
# set here, so the step SKIPs), never printed, never written to the share or a log. In this
# reverted VM only: the run's revert undoes the password and the values, and no snapshot is ever
# taken while they are set (a --keep run's VM is left with them: revert it before any snapshot).
# The admin's reset loses what DPAPI kept under the old password (rekordbox's stored login):
# nothing after it in these suites starts rekordbox. Whether it is set
$WinlogonKey = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
function Set-AutoLogon {
    $name = (Get-CimInstance Win32_ComputerSystem).UserName
    if (-not $name) { return $false }
    $dom, $user = $name.Split('\', 2)
    $lu = Get-LocalUser -Name $user -ErrorAction SilentlyContinue
    if (-not $lu -or [string]$lu.PrincipalSource -ne 'Local' -or $dom -ne $env:COMPUTERNAME) {
        Skip "autologon: the signed-in account $name isn't a local account (PrincipalSource $(if ($lu) { $lu.PrincipalSource } else { 'none' })): its password can't be set here"
    }
    # 24 random bytes as base64, plus one of each class (any complexity policy)
    $b = New-Object byte[] 24
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($b) } finally { $rng.Dispose() }
    $pw = [Convert]::ToBase64String($b) + 'aZ7%'
    try {
        Set-LocalUser -Name $user -Password (ConvertTo-SecureString $pw -AsPlainText -Force)
        Set-ItemProperty -LiteralPath $WinlogonKey -Name DefaultUserName -Value $user
        Set-ItemProperty -LiteralPath $WinlogonKey -Name DefaultDomainName -Value $dom
        Set-ItemProperty -LiteralPath $WinlogonKey -Name DefaultPassword -Value $pw
    } finally { $pw = $null; [Array]::Clear($b, 0, $b.Length) }
    Set-ItemProperty -LiteralPath $WinlogonKey -Name AutoAdminLogon -Value '1'
    Set-ItemProperty -LiteralPath $WinlogonKey -Name ForceAutoLogon -Value '1'
    # Windows 11 hides the password sign-in (and so autologon) while passwordless sign-in is on
    $pl = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\PasswordLess\Device'
    if (Test-Path -LiteralPath $pl) { Set-ItemProperty -LiteralPath $pl -Name DevicePasswordLessBuildVersion -Value 0 -Type DWord }
    [Console]::Out.WriteLine("autologon set for $name (a new random password, not shown)")
    return $true
}
function Clear-AutoLogon {
    Set-ItemProperty -LiteralPath $WinlogonKey -Name AutoAdminLogon -Value '0'
    foreach ($v in 'DefaultPassword', 'ForceAutoLogon') { Remove-ItemProperty -LiteralPath $WinlogonKey -Name $v -ErrorAction SilentlyContinue }
    'autologon cleared'
}

# Wait-UserLogon SECS (SYSTEM): the user signed in again after the sign-out mark (an explorer.exe
# in a user session started after it), within SECS: whether so (vm-run.sh's wait step does the
# same from the host, across a restart)
function Wait-UserLogon([int]$secs) {
    if (-not (Test-Path -LiteralPath $SignoutMark)) { return $false }
    $since = New-Object DateTime ([long]((Get-Content -LiteralPath $SignoutMark -TotalCount 1).Trim())), ([DateTimeKind]::Utc)
    return Wait-Until {
        (Get-CimInstance Win32_ComputerSystem).UserName -and
        @(Get-CimInstance Win32_Process -Filter "Name = 'explorer.exe'" | Where-Object { $_.SessionId -ne 0 -and $_.CreationDate.ToUniversalTime() -gt $since }).Count -gt 0
    } $secs
}

# Invoke-Rbui VERB ARGS... (either side): rbui run here (the user's desktop for a user step), its
# output on this step's stdout: its exit code (0 done, 3 nothing found; rbui's header)
function Invoke-Rbui([Parameter(ValueFromRemainingArguments = $true)][string[]]$argv) {
    $exe = if (Test-Path -LiteralPath $Rbui) { $Rbui } else { Share 'vm/uia/rbui/rbui.exe' }
    return Invoke-Exe $exe $argv
}
