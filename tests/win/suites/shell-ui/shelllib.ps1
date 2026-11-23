# Shared by shell-ui's SYSTEM and interactive steps, after vmlib and unlib.
$ShellWork = Join-Path (Get-UserProfile) 'rbtest-shell-ui'
$ShellName = 'RB Stems Plus'
$ShellEntry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus'
$ShellCmdIfeo = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\cmd.exe'
$ShellCmdBlock = 'C:\rbtest\shell-ui-no-such-debugger.exe'
$ShellUnc = '\\127.0.0.1\rbsp-shell-ui-tmp'

function Expect([bool]$ok, [string]$what) {
    Check $ok $what
    if (-not $ok) { Done }
}

function Assert-ShellDesktop {
    $session = (Get-Process -Id $PID).SessionId
    $prompt = Get-Process -Name 'consent', 'smartscreen' -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -eq $session -and ($_.Name -eq 'consent' -or $_.MainWindowHandle -ne [IntPtr]::Zero) }
    if ($prompt) { Expect $false 'Windows raised a security prompt. The prompt is left for the VM operator.' }
}

function Use-ShellUi([Parameter(ValueFromRemainingArguments = $true)][string[]]$argv) {
    Assert-ShellDesktop
    $limit = [Array]::IndexOf($argv, '--timeout')
    if ($limit -ge 0) {
        $argv[$limit + 1] = [string](Get-WaitSecs ([int]$argv[$limit + 1]))
        Expect ($argv[$limit + 1] -ne '0') 'time remains in this step for the UI action'
    }
    $rc = Invoke-Rbui @argv
    Assert-ShellDesktop
    Expect ($rc -eq 0) "rbui $($argv -join ' ') (exit $rc)"
}

function Close-ShellApp($app) {
    Expect ($app.CloseMainWindow()) "Close requested for the app (pid $($app.Id))"
    Expect ($app.WaitForExit(30000)) 'the app closed on its own'
}

function Wait-ShellApp {
    $opened = Wait-Until { $script:ShellApp = Get-AppWindow; $null -ne $script:ShellApp } 90
    if (-not $opened) {
        Get-AppProcesses | ForEach-Object { "running: $($_.Id) $($_.ProcessName) window=$($_.MainWindowHandle) $($_.Path)" }
        $log = Join-Path (Get-Layout).Support 'Logs\app.log'
        Get-Content -LiteralPath $log -Tail 30 -ErrorAction SilentlyContinue | ForEach-Object { "app.log: $_" }
    }
    Assert-ShellDesktop
    Expect $opened 'the current app opened in the user session'
}

function Press-UninstallDialogs($app) {
    # a kept uninstall (UN4, K1) already removed the support folder, so the retry's start shows the
    # first-start welcome, and the Settings uninstall waits for that flow (core/flows_win.c)
    if (Find-UiText $app.Id ('^' + [regex]::Escape((Get-CoreText 'T_WELCOME_TITLE')) + '$') 5) {
        Use-ShellUi invoke --pid $app.Id (Get-CoreText 'T_CONTINUE') --type Button --timeout 30
    }
    Use-ShellUi invoke --pid $app.Id (Get-CoreText 'T_UNINSTALL') --type Button --timeout 60
    Use-ShellUi invoke --pid $app.Id (Get-CoreText 'T_DONE') --type Button --timeout 180
    Expect ($app.WaitForExit(30000)) 'the app quit after its uninstall completion dialog'
}

function Start-SettingsUninstall {
    Expect (Test-Path -LiteralPath $ShellEntry) 'the per-user Installed apps entry is present'
    Expect (-not (Get-AppWindow)) 'the app is closed before Settings launches it'
    Use-ShellUi settings-uninstall $ShellName --timeout 90
    Wait-ShellApp
    Press-UninstallDialogs $script:ShellApp
}

# RunOnce names the actual temp copy. GetTempPathW can give the local directory in 8.3 form.
function Test-ShellCleanupValue([string]$dir) {
    $name = Split-Path -Leaf $dir
    if ($name -notmatch '^rbsp-uninstall-([0-9a-f]{16})$') { return $false }
    $valueName = "RB Stems Plus cleanup $($Matches[1])"
    $dirs = @($dir)
    if (-not $dir.StartsWith('\\')) {
        # also the short parent with the long leaf: the product names the copy below %TEMP% as
        # Windows sets it (C:\Users\GABRIE~1\...\Temp), and the leaf as it made it
        try {
            $fso = New-Object -ComObject Scripting.FileSystemObject
            $dirs += $fso.GetFolder($dir).ShortPath
            $dirs += Join-Path $fso.GetFolder((Split-Path -Parent $dir)).ShortPath $name
        } catch { }
    }
    $cmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $wants = @($dirs | ForEach-Object { "$valueName = `"$cmd`" /d /q /s /c `"rd /s /q `"$_`"`"" })
    return @(Get-CleanupRunOnce | Where-Object { $wants -contains $_ }).Count -eq 1
}

function Save-ShellCleanup([string]$case, [string]$dir) {
    Expect (Test-ShellCleanupValue $dir) "$case has a RunOnce cleanup value for $dir"
    Set-Content -LiteralPath (Join-Path $ShellWork "$case.txt") -Value $dir -Encoding UTF8
}

function Test-ShellGroup {
    # Give Explorer time to add a second group if the just-opened window failed to join the pin.
    Start-Sleep -Milliseconds 750
    Use-ShellUi taskbar count $ShellName --timeout 15
    Expect ($script:ExeOut -match "(?m) taskbar: count=1 name=$([regex]::Escape($ShellName))\r?$") 'Explorer exposes one RB Stems Plus taskbar button'
}

function Test-PinnedStub([string]$stub) {
    $folder = Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar'
    $shell = New-Object -ComObject WScript.Shell
    Expect (Wait-Until {
        $links = @(Get-ChildItem -LiteralPath $folder -Filter '*.lnk' -ErrorAction SilentlyContinue |
            Where-Object { $shell.CreateShortcut($_.FullName).TargetPath -ieq $stub })
        $links.Count -eq 1
    } 15) "one persisted pin points at the installed stub ($stub)"
}

function Open-ShellPin {
    Expect (-not (Get-AppWindow)) 'there is no app window before the pin is invoked'
    Use-ShellUi taskbar click $ShellName --timeout 15
    Wait-ShellApp
    Test-ShellGroup
}

# The stable pin path stays in place. On ARM64 the x64 lap puts the same release's
# x64 launcher there for the Prism run, and the updater itself still installs the native stub.
function Select-ShellStub($layout, [string]$version, [string]$arch) {
    $source = Join-Path $layout.Apps "$version\stub\$arch\$ShellName.exe"
    Expect (Test-Path -LiteralPath $source -PathType Leaf) "release $version includes the $arch stub"
    $want = (Get-FileHash -LiteralPath $source).Hash
    if ((Get-FileHash -LiteralPath $layout.Stub).Hash -ne $want) {
        Copy-Item -LiteralPath $source -Destination $layout.Stub -Force
    }
    Expect ((Get-FileHash -LiteralPath $layout.Stub).Hash -eq $want) "the pin's target is the $arch stub from $version"
}

# The picture after session or display changes (win/shell/Repaint.cs). The TEST app writes each
# repaint nudge to its session trace (repaint-nudge; TestHooks' churn-sent); the churn steps keep
# its pid in $ShellChurnPid.
$ShellRepaintTrace = Join-Path $ShellWork 'repaint.trace'
$ShellChurnPid = Join-Path $ShellWork 'churn.pid'

# the app started through the stub with the repaint trace on, RBSP_SHELL_TEST's TRIGGERS and
# RBSP_TEST_CHURN's CYCLES; its process in $script:ShellApp
function Start-ShellRepaint([string]$triggers, [int]$cycles = 0) {
    Remove-Item -LiteralPath $ShellRepaintTrace -Force -ErrorAction SilentlyContinue
    $saved = @($env:RBSP_SHELL_TEST, $env:RBSP_TEST_SESSION_TRACE, $env:RBSP_TEST_CHURN)
    try {
        $env:RBSP_SHELL_TEST = $triggers
        $env:RBSP_TEST_SESSION_TRACE = $ShellRepaintTrace
        $env:RBSP_TEST_CHURN = if ($cycles -gt 0) { [string]$cycles } else { $null }
        Start-Process -FilePath (Get-Layout).Stub
    } finally { $env:RBSP_SHELL_TEST, $env:RBSP_TEST_SESSION_TRACE, $env:RBSP_TEST_CHURN = $saved }
    Wait-ShellApp
}

# the trace's steps, in order (its tick stamps dropped)
function Read-ShellRepaint {
    if (-not (Test-Path -LiteralPath $ShellRepaintTrace)) { return @() }
    return @(Get-Content -LiteralPath $ShellRepaintTrace | ForEach-Object { ($_ -split ' ', 2)[1] })
}

function Add-ShellShot {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
    if ('RbspShellShot' -as [type]) { return }
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public static class RbspShellShot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    // the window's client area as the screen shows it (real pixels), saved to PNG; returns the
    // share of a 64 x 64 grid of samples that isn't black (0,0,0), or -1 when it can't be taken
    public static double Shot(IntPtr h, string png) {
        SetProcessDPIAware();
        RECT r; POINT p = new POINT();
        if (!GetClientRect(h, out r) || !ClientToScreen(h, ref p) || r.r <= 0 || r.b <= 0) return -1;
        using (var bmp = new Bitmap(r.r, r.b)) {
            using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(p.x, p.y, 0, 0, bmp.Size);
            bmp.Save(png, System.Drawing.Imaging.ImageFormat.Png);
            int lit = 0;
            for (int i = 0; i < 64; i++)
                for (int j = 0; j < 64; j++) {
                    Color c = bmp.GetPixel(i * (r.r - 1) / 63, j * (r.b - 1) / 63);
                    if (c.R != 0 || c.G != 0 || c.B != 0) lit++;
                }
            return lit / 4096.0;
        }
    }
}
'@
}

# the window restored if minimized (UI Automation's window pattern, as RB-KIT restores it) and
# given the focus
function Restore-ShellWindow($app, [string]$label) {
    Add-ShellShot
    $h = $app.MainWindowHandle
    $w = [Windows.Automation.AutomationElement]::FromHandle($h)
    $wp = $null
    if ([RbspShellShot]::IsIconic($h) -and $w.TryGetCurrentPattern([Windows.Automation.WindowPattern]::Pattern, [ref]$wp)) {
        $wp.SetWindowVisualState([Windows.Automation.WindowVisualState]::Normal)
    }
    Expect (Wait-Until { -not [RbspShellShot]::IsIconic($h) } 10) "${label}: the window is restored"
    try { $w.SetFocus() } catch { }
}

# The window drawn, as the PC's black window wasn't: restored (UI Automation's window pattern, as
# RB-KIT restores it) and in front, its Manage tab on screen to UI Automation, its client area not
# black on the screen (saved beside the step's results as <step>-<label>.png).
function Test-ShellPicture($app, [string]$label) {
    Add-ShellShot
    $app.Refresh()
    $h = $app.MainWindowHandle
    Expect ($h -ne [IntPtr]::Zero) "${label}: the app still has its window"
    $A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
    $w = $A::FromHandle($h)
    Restore-ShellWindow $app $label
    $manage = Get-CoreText 'T_TAB_MANAGE'
    $tabCond = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($A::NameProperty, $manage)),
        (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::TabItem)))
    Check (Wait-Until { $t = $w.FindFirst([Windows.Automation.TreeScope]::Descendants, $tabCond); $t -and -not $t.Current.IsOffscreen } 15) "${label}: the $manage tab is on screen to UI Automation"
    New-Item -ItemType Directory -Path $VmRun.Results -Force | Out-Null
    $png = Join-Path $VmRun.Results ("$($VmRun.Step)-" + ($label -replace '[^A-Za-z0-9]+', '-') + '.png')
    $script:ShellLit = -1
    [void](Wait-Until { $script:ShellLit = [RbspShellShot]::Shot($h, $png); $script:ShellLit -ge 0.05 } 5)
    Check ($script:ShellLit -ge 0.05) "${label}: the window's client area is drawn ($([int]($script:ShellLit * 100)) % of 4096 samples not black; $png)"
}

# the repaint steps after the trace's first N lines
function Get-ShellRepaintSince([int]$n) { $t = Read-ShellRepaint; if ($t.Count -le $n) { return @() }; return @($t[$n..($t.Count - 1)]) }
