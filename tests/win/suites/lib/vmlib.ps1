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

# the architectures a test exe runs as here: the machine's, and x64 under Prism on ARM64
function Get-TestArchs {
    $native = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE
    if ($native -eq 'ARM64') { return @('arm64', 'x64') }
    return @('x64')
}
# a staged test exe (vm/<arch>/name) copied to C:\rbtest\bin\<arch>\ with the files beside it it
# needs (a DLL); its local path, or $null when it isn't staged
function Get-TestExe([string]$arch, [string]$name, [string[]]$with = @()) {
    $src = Share "vm/$arch/$name"
    if (-not (Test-Path -LiteralPath $src)) { return $null }
    $dir = "C:\rbtest\bin\$arch"
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($f in @($name) + $with) { $s = Share "vm/$arch/$f"; if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination $dir -Force } }
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
