# uaclib.ps1: the UAC suites' own helpers (stems-cache-uac.suite, uninstall-uac.suite), after
# lib/vmlib.ps1, worker/rblib.ps1 and uninstall/cases/unlib.ps1: rekordbox's stems model, the
# elevated worker seen from the user's side, the consent prompt seen from there, the app's
# buttons pressed by the core's texts, and what a step leaves for the next (C:\rbtest\uac\).
$UacDir = 'C:\rbtest\uac'
# rekordbox's stems model (win/c/watch.h WATCH_MODELS_BELOW, models.h)
$RbModelDir = if ($UacProfile = Get-UserProfile) { Join-Path $UacProfile 'AppData\Roaming\Pioneer\rekordbox6\models\demucs3_model' } else { $null }

# the app's log (Logs\app.log in the support folder), from byte offset from ('' when none)
function Read-AppLog([long]$from = 0) {
    $f = Join-Path (Get-Layout).Support 'Logs\app.log'
    if (-not (Test-Path -LiteralPath $f)) { return '' }
    $s = New-Object IO.FileStream($f, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]'ReadWrite, Delete'))
    try {
        if ($from -gt $s.Length) { $from = 0 }
        [void]$s.Seek($from, [IO.SeekOrigin]::Begin)
        return (New-Object IO.StreamReader($s, [Text.Encoding]::UTF8)).ReadToEnd()
    } finally { $s.Dispose() }
}
function Get-AppLogSize {
    $f = Join-Path (Get-Layout).Support 'Logs\app.log'
    if (Test-Path -LiteralPath $f) { return (Get-Item -LiteralPath $f).Length }
    return 0
}

# a value a step leaves for a later one (C:\rbtest\uac\NAME), and reads back ($null when none)
function Save-UacValue([string]$name, [string]$value) {
    New-Item -ItemType Directory -Force $UacDir | Out-Null
    Set-Content -LiteralPath (Join-Path $UacDir $name) -Value $value -Encoding UTF8
}
function Read-UacValue([string]$name) {
    $f = Join-Path $UacDir $name
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    return ([string](Get-Content -LiteralPath $f -TotalCount 1)).Trim()
}

# the elevated worker's processes: "RB Stems Plus" processes that are neither the app (its
# version folder's exe) nor the stub, by full path where this side can read it (an elevated
# process's path may be unreadable to the user: listed with an empty Path)
function Get-WorkerProcesses {
    $l = Get-Layout
    return @(Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | ForEach-Object {
            $p = $null; try { $p = $_.Path } catch { }
            $isApp = $p -and ($p.Equals($l.Stub, [StringComparison]::OrdinalIgnoreCase) -or
                ($p.StartsWith($l.Apps + '\', [StringComparison]::OrdinalIgnoreCase) -and -not ($p -match '\\admin\\RB Stems Plus\.exe$')))
            if (-not $isApp) { [pscustomobject]@{ Id = $_.Id; Path = [string]$p; Process = $_ } }
        })
}
# whether UAC's consent prompt shows now (consent.exe runs in the user's session)
function Test-ConsentShown { return @(Get-Process -Name 'consent' -ErrorAction SilentlyContinue).Count -gt 0 }

# a share check on a folder: CreateFileW for reading, sharing write and delete but not read: 32
# (ERROR_SHARING_VIOLATION) when another handle reads it without sharing that, 0 when it opened,
# else the error (5: access denied). The chain the core holds over a bin\ worker during runas
# (app.md W5 A7: the exe, <sha12>, bin, RB Stems Plus, opened for reading, sharing READ only)
# makes <sha12> and bin answer 32; nothing else holds those folders
Add-Type -Namespace RbspUac -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
[DllImport("kernel32.dll")]
public static extern bool CloseHandle(IntPtr h);
public static int OpenNoReadShare(string path) {
    // FILE_LIST_DIRECTORY | SYNCHRONIZE; FILE_SHARE_WRITE | FILE_SHARE_DELETE; OPEN_EXISTING; FILE_FLAG_BACKUP_SEMANTICS
    IntPtr h = CreateFileW(path, 0x00100001, 0x2 | 0x4, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
    if (h == new IntPtr(-1)) return Marshal.GetLastWin32Error();
    CloseHandle(h);
    return 0;
}
'@
function Test-HeldForRead([string]$folder) { return [RbspUac.Native]::OpenNoReadShare($folder) }

# the app opened through the stub when it isn't open (a quiet start: it was welcomed), its process
# in $script:app; a statement (its Check is its output). A start after a sign-out that cut an
# action asks whether to continue it (check_resume's "Action interrupted"): said, and answered
# Cancel, which only clears the marker; the case's after-step judged what was left. WC1's sign-out
# came once the worker's step had ended (its part done); WC3's worker is cut mid-work, after the
# bridge's swap and before its verification and record (its part not done: wc10-ready puts the
# recorded install back)
function Open-App {
    $script:app = Get-AppWindow
    $fresh = -not $script:app
    if ($script:app) { "the app is open (pid $($script:app.Id))" } else { Start-AppQuiet }
    if (-not $script:app) { return }
    $t = Get-CoreText 'T_ACTION_INTERRUPTED'
    if (Find-UiText $script:app.Id ('^' + [regex]::Escape($t) + '$') $(if ($fresh) { 8 } else { 2 })) {
        $line = @((Read-AppLog) -split "`r?`n" | Where-Object { $_ -match 'the last run quit during: ' }) | Select-Object -Last 1
        "the app asks to continue a cut action ($t; app.log: '$($line.Trim())'): Cancel"
        Check (Invoke-UiNamed $script:app.Id (Get-CoreText 'T_CANCEL') 30 'Button') "$t answered Cancel"
    }
}

# one of the app's buttons (a core text id) pressed once it is there and enabled, then its
# confirmation's button (a core text id) when one is given: a statement (its Checks are its
# output), whether both were pressed in $script:Pressed
function Invoke-AppButton([string]$button, [string]$confirm = '', [int]$secs = 120) {
    $script:Pressed = $false
    $label = Get-CoreText $button
    $ok = Wait-UiEnabled $script:app.Id $label $secs
    Check $ok "its $label button is there and enabled"
    if (-not $ok) { Show-AppState; return }
    $ok = Invoke-UiNamed $script:app.Id $label 30 'Button'
    Check $ok "$label pressed"
    if ($ok -and $confirm) { Invoke-Confirm $confirm; return }
    $script:Pressed = $ok
}
# Like Invoke-AppButton, emits the checks and leaves the outcome in $script:Pressed.
function Invoke-Confirm([string]$confirm) {
    $c = Get-CoreText $confirm
    $ok = Invoke-UiNamed $script:app.Id $c 60 'Button'
    Check $ok "its confirmation answered: $c"
    if (-not $ok) { Show-AppState }
    $script:Pressed = $ok
}
# the complete uninstall asked for: the Uninstall RB Stems Plus Completely button where the app has
# one, else (the Windows app) its menu's RB Stems Plus > Uninstall RB Stems Plus... (unlib); then
# its confirmation's Uninstall. $script:Pressed as Invoke-AppButton
function Invoke-UninstallAll {
    $script:Pressed = $false
    if (Wait-UiEnabled $script:app.Id (Get-CoreText 'T_BTN_UNINSTALL_ALL') 5) { Invoke-AppButton 'T_BTN_UNINSTALL_ALL' 'T_UNINSTALL'; return }
    Invoke-MenuUninstall $script:app
    Invoke-Confirm 'T_UNINSTALL'
}
# what the app shows (its windows' named elements) and app.log's last 40 lines, for a failure's
# reading
function Show-AppState {
    try {
        Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
        $A = [Windows.Automation.AutomationElement]
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $script:app.Id)
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
                if ($e.Current.Name) { "ui: $($e.Current.ControlType.ProgrammaticName) '$($e.Current.Name)' enabled=$($e.Current.IsEnabled)" }
            }
        }
    } catch { "ui: unreadable ($($_.Exception.Message))" }
    $alog = Join-Path (Get-Layout).Support 'Logs\app.log'
    if (Test-Path -LiteralPath $alog) { Get-Content -LiteralPath $alog -Tail 40 | ForEach-Object { "app.log: $_" } }
}
# Install Stems Cache > Install, or Reinstall Stems Cache > Reinstall when that is the one shown
function Invoke-CacheInstall {
    if (Wait-UiEnabled $script:app.Id (Get-CoreText 'T_BTN_REINSTALL_CACHE') 5) { Invoke-AppButton 'T_BTN_REINSTALL_CACHE' 'T_REINSTALL' }
    else { Invoke-AppButton 'T_BTN_INSTALL_CACHE' 'T_INSTALL' }
}

# Stems Cache on: rekordbox_model=1 in config.ini (the last thing its install writes)
function Test-CacheOn {
    $cfg = Join-Path (Get-Layout).Support 'config.ini'
    return (Test-Path -LiteralPath $cfg) -and [bool](Select-String -LiteralPath $cfg -Pattern '^\s*rekordbox_model\s*=\s*1\s*$' -Quiet)
}

# the consent responder's result, checked: the arm step before it ended (arm.trace), answered ANSWER, the prompt named the worker's file and
# an unknown publisher (release.md section 8), and no second prompt (rbui's exit 0, not 5); with
# -ThenLogoff, that the sign-out was started after it. A statement: the result in
# $script:ConsentResult
function Test-ConsentResult([string]$answer, [string]$name = 'consent', [switch]$ThenLogoff) {
    # an arm step that never got to its end left the last responder's output: never read as this one's
    $last = Show-ArmTrace
    Check (-not ($last -match '^\S+ arm-[^:]*: ') -or $last -match ': armed$') "the arm step before got to its end (arm.trace: '$last')"
    $r = Read-Consent $name 120
    Check $r.Done "the consent responder ended ($name)"
    Check ($r.Answered -eq $answer) "UAC's prompt answered $answer on the secure desktop (rbui: '$($r.Answered)')"
    Check ($r.Prompt -match 'RB Stems Plus\.exe') "the prompt names RB Stems Plus.exe ($($r.Prompt))"
    Check ($r.Prompt -match 'Unknown') 'and an unknown publisher'
    if ($ThenLogoff) { Check ($r.Text -match 'consent: then: started') 'the sign-out started right after the answer, once the worker ran' }
    else { Check ($r.Code -eq 0) "one prompt only, nothing else (rbui exit $($r.Code))" }
    $script:ConsentResult = $r
}

# Set-WorkerHold SECS [-Mid] (the user): the next elevated TEST worker holds SECS s (win/admin/main.c's
# ADMIN_TEST_HOLD; a release worker reads neither name), so the sign-out cases' block screen can
# show: here the worker's run is under a second, so a vetoed sign-out went on before Windows
# showed it (VM round 3). Where it holds:
#   (no -Mid)  before its work (RBSP_TEST_WORKER_HOLD): WC1 and WC2, where the worker then
#              finishes its work while the sign-out waits or after Cancel
#   -Mid       once its work has begun changing files (RBSP_TEST_WORKER_MIDHOLD, worker.h's
#              admin_test_mid_hold: the install with the bridge swapped into rekordbox, the
#              uninstall with Pioneer's file put back and our folder still there): WC3 and WC10,
#              whose "Sign out anyway" ends the session a few seconds after the worker starts,
#              inside a hold before the work (VM round 5), so the end now lands mid-work
# An elevated process gets a new environment from the user's registry, so the value goes into
# HKCU\Environment (and this step's own environment, for an app it starts); setting one clears the
# other, 0 clears both. A statement
# 20 s: well within the app's 30 s logoff wait after its veto (CloseGate's LogoffWaitMs), so a
# worker held before its work still ends before the app lets the sign-out go on
$WorkerHoldSecs = 20
function Set-WorkerHold([int]$secs, [switch]$Mid) {
    foreach ($n in 'RBSP_TEST_WORKER_HOLD', 'RBSP_TEST_WORKER_MIDHOLD', 'RBSP_TEST_WORKER_MIDHOLD_TARGET') {
        Remove-ItemProperty -LiteralPath 'HKCU:\Environment' -Name $n -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath "Env:\$n" -ErrorAction SilentlyContinue
    }
    if ($secs -le 0) { return }
    $name = if ($Mid) { 'RBSP_TEST_WORKER_MIDHOLD' } else { 'RBSP_TEST_WORKER_HOLD' }
    Set-ItemProperty -LiteralPath 'HKCU:\Environment' -Name $name -Value ([string]$secs) -Type String
    Set-Item -LiteralPath "Env:\$name" -Value ([string]$secs)
    "the next elevated TEST worker holds $secs s $(if ($Mid) { 'once its work has begun changing files' } else { 'before its work' }) ($name in HKCU\Environment)"
}
