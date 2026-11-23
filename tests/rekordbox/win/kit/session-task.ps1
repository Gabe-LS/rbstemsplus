# session-task.ps1: run by the scheduled task "RBSP session" inside the test account's signed-in
# session (SSH runs in another session and can't start or close rekordbox's window, see the
# screen or drive UI Automation). midi-auto.ps1 -Do ensure registers the task.
# Reads a command, its argument and a request ID from %TEMP%\rbst\task-cmd.txt and writes the ID and
# result to %TEMP%\rbst\task-out.txt:
#   ping            the session is there (the account is signed in); report its session ID
#   start | quit    start rekordbox | Escape in its main window, then ask it to close
#   quitdump        save the redacted UI trees of rekordbox's top-level windows after a close timeout
#   fmtimport       rebuild and export RBSP Formats using rekordbox's UI
#   fmtcancelpicker cancel a verified rekordbox file picker before normal close
#   fmtuidump       save the redacted main UI tree, or explore menus and cancel file pickers
#   fmtstep ACTION  do one guarded UI action and save a redacted dump
#   shot            a screenshot of the whole screen to %TEMP%\rbst\shots\auto-<time>.png
#   uiatree NAME    read only: the UI Automation tree of process NAME's windows in this session
#                   (rekordbox or RB Stems Plus) to %TEMP%\rbst\uiatree-NAME.txt, one element a line
#   prefsopen | prefsclose   rekordbox's Preferences opened by its gear | closed by its X (UIA)
#   prefstab NAME   a Preferences category shown (View, Audio, Analysis, Controller, Keyboard, Advanced)
#   autoanalysisoff Preferences > Analysis > Auto Analysis "Enable" unticked (owner decision
#                   2026-11-19, test account only), screenshots before and after, Preferences closed
#   appexit         our app closed by its own Exit (menu, else its window's close), gone within 30 s
#   appuninstall    our app removed by its own complete uninstall (menu, Uninstall, Keep if asked, Done)
#   appinstall TLS|HTTP|SHA   a TEST release installed by its bootstrap from the Mac's server
#                   (byte-checked against SHA, as M12's install does), its welcome answered
#   appmin | appcheck  our app minimized (UIA) | restored and checked: title, tabs, black share (repro)
#   appstate        read only: our app running, minimized, and the tabs UI Automation sees (none: black)
#   benchstart NAME a DirectML bench run (cpu, dml or stall: dml-run.ps1) started in this session
#   benchstop | benchstate  the kit's own bench helpers (its bench\rbsp-prepare.exe, by path) stopped | counted
#   windows         the visible top-level windows: process | title | size | 0xHANDLE (a dialog shows up here)
#   winlist         read-only Win32 facts and on-screen crops for this session's rekordbox windows
#   dismiss-rb-error  dismiss the verified rekordbox "Unexpected application error" notice
#   closeupdate HWND  verify the rekordbox update offer, invoke its UIA Close button, and verify it is gone
#   closeinfo HWND  verify rekordbox's news window (INFO), close it by UIA, and verify it is gone
#   stemsinit HWND  verify (read only) rekordbox's "Initializing STEMS Engine" progress window
#   closeagentnews HWND  verify rekordboxAgent's news window, close it by UIA, and verify it is gone
#   front           our app's window minimized (UIA), rekordbox's verified main window put in front
#   closeagent      rekordboxAgent's news window closed as its X does (WM_CLOSE to that window only),
#                   only when it is the one such window
#   testwindow      the dialog watch's test: a window titled "RBSP watch test" (closes itself in 5 min)
#   closetest       WM_CLOSE to the "RBSP watch test" windows only
#   app             our app started if it isn't running, and its main window waited for (60 s)
#   uninstallcache  our app's Manage tab > "Uninstall Stems Cache" > the dialog's "Uninstall", by UI Automation
#   prepare NAME    our app's Prepare tab: the playlist NAME selected, then "Prepare" clicked
#   prepstate       our app's Prepare tab: running or idle, and its track rows' spoken names
#   prepstop        our app's Prepare tab: its "Stop" button invoked
#   prepbtn NAME    our app's Prepare tab: its Pause, Resume or Stop button invoked
#   stopkeep NAME   our app's Status tab: kept set NAME selected, Stop Keeping…, confirmed (our app's own UI)
#   hangcheck       our app (read only): its window answers or not (WM_NULL, 5 s), the prepare helpers
#                   running (rbsp-prepare.exe: pid, start, command line), and a minidump of the app
#                   (dbghelp's MiniDumpWriteDump, an OS component) to %TEMP%\rbst\hang\app-<pid>-<time>.dmp
#   m12-*          run an M12 round on this desktop (m12.ps1)
#   loc STEP|ARG    plan N3's rows in our app's own UI (loc-ui.ps1: set, thispc, pick, texts, install,
#                   uninstall, netmap, netunmap)
#   logoff          signs the account out (after writing its answer)
# The owner handles sign-in, licence, security and UAC prompts. Tests name their app confirmations.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'kit-lib.ps1')
$out = Join-Path $B 'task-out.txt'
# a TEST app started from here writes its session trace (repaint-look drawn|black, repaint-software:
# win/shell/TestHooks.cs) to this file; a release ignores the variable
$env:RBSP_TEST_SESSION_TRACE = Join-Path $B 'app-session-trace.txt'
# a TEST app shows its Prepare and Status tabs only with this hook (win/shell/Native.cs PrepareShown;
# the release always does): without it the Prepare round finds no tab (PC 2026-11-20)
$env:RBSP_SHELL_TEST = 'prepare'
$TestTitle = 'RBSP watch test'
$after = $null

# our app's process, with its main window up (started from its install folder if it isn't running;
# $null if it can't be, or shows no window in 60 s)
function Get-App {
    if (-not (Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue)) {
        $exe = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus') -Recurse -Filter 'RB Stems Plus.exe' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
        if (-not $exe) { return $null }
        Start-Process -FilePath $exe.FullName
    }
    $t = [Diagnostics.Stopwatch]::StartNew()
    do {
        $app = Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if ($app) { Restore-App $app; return $app }
        Start-Sleep 1
    } while ($t.Elapsed.TotalSeconds -lt 60)
    return $null
}
function Uia { Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes }
# our app closed by its own Exit (owner decision 2026-11-19: before rekordbox model changes and the
# Prepare round, and reopened after): its window restored, the menu "RB Stems Plus" > "Exit" invoked
# through UI Automation; when that menu isn't in UI Automation in 10 s, the window's own close (its
# X, which the app's close flow treats as Exit). Its processes in this session must be gone within
# 30 s, else a STOP (a screenshot then); nothing is ever killed.
function Exit-App {
    try {
        $procs = @(Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sid })
        if (-not $procs.Count) { return 'appexit: ok; not running' }
        $app = @($procs | Where-Object { $_.MainWindowHandle -ne 0 })[0]
        if (-not $app) { throw 'our app has no main window' }
        Uia; Restore-App $app
        $cancelled = Close-OurDialog $app   # a confirmation left open (a STOP mid-step) answered Cancel first
        $CT = [Windows.Automation.ControlType]; $how = ''
        $top = Find-El $app 'RB Stems Plus' $CT::MenuItem 10
        $p = $null
        if ($top -and $top.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$p)) {
            $p.Expand()
            if (Invoke-El (Find-El $app 'Exit' $CT::MenuItem 10)) { $how = "the menu's RB Stems Plus > Exit" }
        }
        if (-not $how) {
            $wp = $null
            if (-not ([Windows.Automation.AutomationElement]::FromHandle($app.MainWindowHandle)).TryGetCurrentPattern([Windows.Automation.WindowPattern]::Pattern, [ref]$wp)) { throw 'neither the Exit menu item nor a window pattern' }
            $wp.Close(); $how = "its window's close (no Exit menu item in UI Automation)"
        }
        if ($cancelled) { $how = "its open '$cancelled' cancelled, then $how" }
        $t = [Diagnostics.Stopwatch]::StartNew()
        while (@($procs | Where-Object { -not $_.HasExited }).Count -and $t.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 500; foreach ($q in $procs) { $q.Refresh() } }
        if (@($procs | Where-Object { -not $_.HasExited }).Count) { throw "still running 30 s after $how; $(Shot)" }
        return "appexit: ok; closed by $how in $([int]$t.Elapsed.TotalSeconds) s"
    } catch {
        return 'appexit: ' + ($_.Exception.Message -replace '[\r\n]+', ' ')
    }
}
# our app's own confirmation left open in its main window (a STOP between a button and its dialog):
# answered Cancel through UI Automation; its name, or '' when none is open
function Close-OurDialog($app) {
    $A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
    $main = $A::FromHandle($app.MainWindowHandle)
    $cond = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Window)
    foreach ($d in @($main.FindAll([Windows.Automation.TreeScope]::Children, $cond))) {
        $bc = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($A::NameProperty, 'Cancel')),
            (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)))
        $b = @($d.FindAll([Windows.Automation.TreeScope]::Descendants, $bc))
        if ($b.Count -ne 1) {   # an alert (Download failed): its one OK acknowledges it, nothing more
            $bc = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($A::NameProperty, 'OK')),
                (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)))
            $b = @($d.FindAll([Windows.Automation.TreeScope]::Descendants, $bc))
        }
        if ($b.Count -eq 1 -and (Invoke-El $b[0])) { $name = $d.Current.Name; Start-Sleep 1; return $name }
    }
    return ''
}
# Our app swapped for another TEST build (coordinator 2026-11-19: the black-window fix): the
# installed one removed by its own complete uninstall (menu RB Stems Plus > Uninstall RB Stems
# Plus…, its Uninstall, its Done), then the TEST bootstrap from the Mac's server, byte-checked
# against the hash the Mac gives, as M12's install does; the welcome answered Continue.
# The first one or a button that doesn't come: a throw (the answer says which).
function Find-Named($app, [string]$name, $type, [int]$secs) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    do {
        $app.Refresh(); if ($app.HasExited) { return $null }
        $e = Find-El $app $name $type 0 -Enabled
        if ($e) { return $e }
        Start-Sleep -Milliseconds 500
    } while ($t.Elapsed.TotalSeconds -lt $secs)
    return $null
}
# The WPF dialogs use the app's caption in Win32. Verify the UIA title and the entire visible
# button set on the app's own window before returning the button to invoke.
function Get-UninstallDialog($app, [hashtable]$allowed) {
    $windows = @(Get-DesktopWindows)
    if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable' }
    $dialogs = @()
    foreach ($row in $windows) {
        $process, $rest = $row -split ' \| ', 2
        if ($process -notin $Watched -and $rest -notlike "$TestTitle*" -and $rest -notlike "$UpdateTitle | *") { continue }
        $h = [IntPtr][Convert]::ToInt64(($row -split ' \| ')[-1].Substring(2), 16)
        if ($process -ne 'RB Stems Plus' -or [RbspWin]::Pid($h) -ne $app.Id) {
            throw 'unexpected watched window during app uninstall'
        }
        if ($h -ne $app.MainWindowHandle) { $dialogs += $h }
    }
    if (-not $dialogs.Count) { return $null }
    if ($dialogs.Count -ne 1) { throw 'more than one app dialog during uninstall' }
    $h = $dialogs[0]
    $A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
    try {
        $dialog = $A::FromHandle($h)
        $c = $dialog.Current
        $expected = $allowed[[string]$c.Name]
        if (-not $expected -or $c.Name -cnotin $allowed.Keys -or $c.ProcessId -ne $app.Id -or
            [int64]$c.NativeWindowHandle -ne $h.ToInt64() -or $c.ControlType -ne $CT::Window -or $c.IsOffscreen) {
            throw 'unexpected app dialog during uninstall'
        }
        $cond = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)
        $buttons = @($dialog.FindAll([Windows.Automation.TreeScope]::Descendants, $cond) | Where-Object {
            -not $_.Current.IsOffscreen -and
            [Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($_).Current.ControlType -ne $CT::TitleBar
        })
        $answer = @($buttons | Where-Object { $_.Current.Name -ceq $expected[0] })
        if ($buttons.Count -ne $expected.Count -or $answer.Count -ne 1 -or
            @($buttons | Where-Object { $_.Current.ProcessId -ne $app.Id -or $_.Current.Name -cnotin $expected }).Count) {
            throw 'unexpected app dialog buttons during uninstall'
        }
        if (-not [RbspWin]::OrdinaryDesktop() -or [RbspWin]::Pid($h) -ne $app.Id) { throw 'the app dialog or input desktop changed' }
        return @{ Title = $c.Name; Handle = $h; Button = $answer[0] }
    } catch [Windows.Automation.ElementNotAvailableException] {
        # An app window can disappear between enumeration and the UIA query.
    }
}
function Remove-OurApp {
    try {
        . (Join-Path $PSScriptRoot 'watch-lib.ps1')
        Uia; $CT = [Windows.Automation.ControlType]
        $app = Get-App; if (-not $app) { throw 'our app is not installed or shows no window' }
        Restore-App $app
        $confirmTitle = 'Uninstall RB Stems Plus?'
        $keepTitle = 'Stems Cache from another account'
        # an uninstall already waiting on the other-account question (a STOPped earlier run) is
        # resumed: it counts as confirmed and the finish loop below answers it with Keep
        $confirm = @{ 'Welcome to RB Stems Plus' = @('Continue'); $confirmTitle = @('Uninstall', 'Cancel'); $keepTitle = @('Keep', 'Remove') }
        # the menu item again every 15 s: the core ignores a click while an action (the welcome's)
        # still runs (PC 2026-11-20: "core: a click while an action runs is ignored")
        $t = [Diagnostics.Stopwatch]::StartNew(); $asked = $false; $next = 0
        while (-not $asked -and $t.Elapsed.TotalSeconds -lt 90) {
            $dialog = Get-UninstallDialog $app $confirm
            if ($dialog -and $dialog.Title -ceq $keepTitle) {
                $asked = $true; Say "appuninstall: resuming an uninstall already waiting on '$keepTitle'"
            } elseif ($dialog -and $dialog.Button.Current.IsEnabled) {
                if (-not (Invoke-El $dialog.Button)) { throw 'could not answer the verified app dialog' }
                $asked = $dialog.Title -ceq $confirmTitle
            } elseif (-not $dialog -and $t.Elapsed.TotalSeconds -ge $next) {
                $next = $t.Elapsed.TotalSeconds + 15
                $top = Find-El $app 'RB Stems Plus' $CT::MenuItem 10; $p = $null
                if (-not ($top -and $top.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$p))) { throw 'no RB Stems Plus menu' }
                $p.Expand()
                if (-not (Invoke-El (Find-El $app "Uninstall RB Stems Plus$([char]0x2026)" $CT::MenuItem 10))) { throw 'no Uninstall RB Stems Plus… menu item' }
            }
            if (-not $asked) { Start-Sleep -Milliseconds 500 }
        }
        if (-not $asked) { throw 'no Uninstall confirmation in 90 s' }
        $doneTitle = 'RB Stems Plus is uninstalled'
        $finish = @{ $keepTitle = @('Keep', 'Remove'); $doneTitle = @('Done'); $confirmTitle = $confirm[$confirmTitle] }
        $pending = $dialog.Handle; $kept = $null; $done = $false
        $t = [Diagnostics.Stopwatch]::StartNew()
        while ($t.Elapsed.TotalSeconds -lt 180) {
            $app.Refresh()
            if ($app.HasExited) { break }
            # The previous dialog can remain visible briefly after its button is invoked.
            $dialog = Get-UninstallDialog $app $finish
            if ($dialog -and $dialog.Title -ceq $confirmTitle) {
                if ($pending -ne $dialog.Handle) { throw 'unexpected repeat of the uninstall confirmation' }
            } else {
                $pending = $null
                if ($dialog -and $dialog.Title -ceq $keepTitle -and $kept -and $kept -ne $dialog.Handle) {
                    throw 'unexpected repeat of the Stems Cache dialog'
                }
                if ($dialog -and $dialog.Button.Current.IsEnabled -and $dialog.Title -ceq $keepTitle -and -not $kept) {
                    if (-not (Invoke-El $dialog.Button)) { throw 'could not choose Keep on the verified Stems Cache dialog' }
                    $kept = $dialog.Handle
                    Say "appuninstall: chose Keep on '$keepTitle'"
                } elseif ($dialog -and $dialog.Button.Current.IsEnabled -and $dialog.Title -ceq $doneTitle) {
                    if (-not (Invoke-El $dialog.Button)) { throw 'could not invoke Done on the verified app dialog' }
                    $done = $true; break
                }
            }
            Start-Sleep -Milliseconds 500
        }
        if (-not $done) { throw "no Done in 180 s; $(Shot)" }
        $t = [Diagnostics.Stopwatch]::StartNew()
        while (-not $app.HasExited -and $t.Elapsed.TotalSeconds -lt 90) { Start-Sleep -Milliseconds 500; $app.Refresh() }
        $apps = Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus'
        while ((Test-Path -LiteralPath $apps) -and $t.Elapsed.TotalSeconds -lt 150) { Start-Sleep 1 }
        if (Test-Path -LiteralPath $apps) { throw "the apps folder is still there 150 s on; $(Shot)" }
        return 'appuninstall: ok; removed by its own uninstall'
    } catch { return 'appuninstall: ' + ($_.Exception.Message -replace '[\r\n]+', ' ') }
}
# the installed release's bridge (named and hashed by its installed payload.json) downloaded from
# the TEST server's release path into %LOCALAPPDATA%\rbstemsplus\rbkit-worker-source; throws when
# the bytes differ
function Save-OurBridge([string]$tls) {
    $support = Join-Path $env:LOCALAPPDATA 'rbstemsplus'
    $p = Get-Content -LiteralPath (Join-Path $support 'installed-release\payload.json') -Raw | ConvertFrom-Json
    $b = $p.windows.bridge; $rel = "$($p.payload_version)"
    if ("$($b.file)" -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*\.dll$' -or "$($b.sha256)" -notmatch '^[a-f0-9]{64}$' -or $rel -notmatch '^\d+\.\d+\.\d+$') { throw 'the installed payload names no bridge' }
    $dir = Join-Path $support 'rbkit-worker-source'; [void][IO.Directory]::CreateDirectory($dir)
    $dest = Join-Path $dir $b.file; $tmp = "$dest.part"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $web = New-Object Net.WebClient
    try {
        $web.DownloadFile("$tls/download/v$rel/$($b.file)", $tmp)
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $s = [IO.File]::OpenRead($tmp); try { $got = ([BitConverter]::ToString($sha.ComputeHash($s)) -replace '-', '').ToLowerInvariant() } finally { $s.Dispose() } } finally { $sha.Dispose() }
        if ($got -ne $b.sha256) { throw "the downloaded bridge's SHA-256 $($got.Substring(0, 12)) is not the installed payload's" }
        Move-Item -LiteralPath $tmp -Destination $dest -Force
    } finally { $web.Dispose(); if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force } }
    return "bridge $($b.sha256.Substring(0, 12)) staged"
}
function Install-OurApp([string]$arg) {
    try {
        if ($arg -notmatch '^(https://[0-9A-Za-z.\-]+:\d+(?:/[0-9A-Za-z._\-]+)*)\|(http://[0-9A-Za-z.\-]+:\d+(?:/[0-9A-Za-z._\-]+)*)\|([0-9a-f]{64})$') { throw 'expected TLSBASE|HTTPBASE|SHA256' }
        $tls = $Matches[1]; $http = $Matches[2]; $want = $Matches[3].ToUpperInvariant()
        if (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus')) { throw 'an app is installed: appuninstall first' }
        Remove-Item -LiteralPath $env:RBSP_TEST_SESSION_TRACE -ErrorAction SilentlyContinue   # this release's trace only
        $cmd = @"
`$ErrorActionPreference = 'Stop'
`$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
`$env:RBSTEMSPLUS_BASE_URL = '$http/'
Remove-Item Env:RBSTEMSPLUS_RESTORE -ErrorAction SilentlyContinue
`$web = New-Object Net.WebClient
try {
    `$bytes = `$web.DownloadData('$tls/latest/download/bootstrap.ps1')
    `$sha = [Security.Cryptography.SHA256]::Create()
    try { `$got = [BitConverter]::ToString(`$sha.ComputeHash(`$bytes)).Replace('-', '') } finally { `$sha.Dispose() }
    if (`$got -ne '$want') { throw 'TEST bootstrap hash differs' }
    Invoke-Expression ([Text.Encoding]::ASCII.GetString(`$bytes))
} finally { `$web.Dispose() }
"@
        $log = Join-Path $B 'appinstall-bootstrap.log'
        $p = Start-Process powershell.exe -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))) -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru
        # the handle kept from the start: without it .NET reports no ExitCode after a timed wait (PC
        # 2026-11-20: a bootstrap that installed fine read as "exited " with no code)
        $null = $p.Handle
        if (-not $p.WaitForExit(300000)) { throw "the TEST bootstrap did not finish in 300 s (see $log)" }
        $p.WaitForExit()
        if ($p.ExitCode -ne 0) { throw "the TEST bootstrap exited $($p.ExitCode): $((Get-Content -LiteralPath "$log.err" -ErrorAction SilentlyContinue | Select-Object -Last 2) -join ' ')" }
        Uia; $app = Get-App; if (-not $app) { throw 'installed, but no app window in 60 s' }
        [void](Invoke-El (Find-Named $app 'Continue' ([Windows.Automation.ControlType]::Button) 20))   # its welcome
        $ver = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus' -ErrorAction SilentlyContinue).DisplayVersion
        # a clean bootstrap leaves the bridge download to the app's own Stems Cache install (which
        # needs an administrator): the kit's cache install finds it here, as M12-WorkerSource stages
        # it, checked against the installed manifest (PC 2026-11-20: "no file with the bridge's SHA-256")
        $staged = Save-OurBridge $tls
        return "appinstall: ok; release $ver from $tls (pid $($app.Id)); $staged"
    } catch { return 'appinstall: ' + ($_.Exception.Message -replace '[\r\n]+', ' ') }
}
# The black-window repro (coordinator 2026-11-19): our app's main window in this session
# minimized through its UI Automation window pattern (appmin), or restored and checked (appcheck):
# its title, the tabs UI Automation sees and how much of the window below its title bar is black
# in a screenshot (pixels with every channel under 16, on a 48 x 48 grid)
function Get-OurWindow {
    $p = @(Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sid -and $_.MainWindowHandle -ne 0 })
    if ($p.Count) { return $p[0] } else { return $null }
}
function Hide-OurWindow {
    try {
        $app = Get-OurWindow; if (-not $app) { return 'appmin: not running' }
        Uia; [void](Get-DesktopWindows)
        $wp = $null
        if (-not ([Windows.Automation.AutomationElement]::FromHandle($app.MainWindowHandle)).TryGetCurrentPattern([Windows.Automation.WindowPattern]::Pattern, [ref]$wp)) { throw 'no UI Automation window pattern' }
        $wp.SetWindowVisualState([Windows.Automation.WindowVisualState]::Minimized)
        $t = [Diagnostics.Stopwatch]::StartNew()
        while (-not [RbspWin]::IsIconic($app.MainWindowHandle) -and $t.Elapsed.TotalSeconds -lt 3) { Start-Sleep -Milliseconds 100 }
        return "appmin: minimized $([RbspWin]::IsIconic($app.MainWindowHandle))"
    } catch { return 'appmin: ' + ($_.Exception.Message -replace '[\r\n]+', ' ') }
}
function Test-OurWindow {
    try {
        $app = Get-OurWindow; if (-not $app) { return 'appcheck: not running' }
        Uia; [void](Get-DesktopWindows); Restore-App $app
        Start-Sleep 2; $app.Refresh()
        $A = [Windows.Automation.AutomationElement]
        $cond = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)),
            (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::TabItem)))
        $tabs = @($A::RootElement.FindAll([Windows.Automation.TreeScope]::Descendants, $cond) | ForEach-Object { $_.Current.Name })
        $rowsCond = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)), (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::DataItem)))
        $rows = @($A::RootElement.FindAll([Windows.Automation.TreeScope]::Descendants, $rowsCond)).Count   # the shown tab's table rows (Status sets, Prepare tracks)
        Screen-Dpi
        $r = New-Object RbspWin+RECT
        if (-not [RbspWin]::GetWindowRect($app.MainWindowHandle, [ref]$r)) { throw 'no window rectangle' }
        $shot = Shot; $png = ($shot -split ' ', 2)[1]
        Add-Type -AssemblyName System.Drawing
        $img = [System.Drawing.Bitmap]::FromFile($png)
        try {
            $x0 = [math]::Max(0, $r.L + 8); $x1 = [math]::Min($img.Width - 1, $r.R - 8); $y0 = [math]::Max(0, $r.T + 60); $y1 = [math]::Min($img.Height - 1, $r.B - 8)
            if ($x1 -le $x0 -or $y1 -le $y0) { throw "window rectangle off screen ($($r.L),$($r.T),$($r.R),$($r.B))" }
            $n = 0; $black = 0; $ink = 0
            foreach ($i in 0..47) { foreach ($j in 0..47) {
                $c = $img.GetPixel([int]($x0 + ($x1 - $x0) * $i / 47), [int]($y0 + ($y1 - $y0) * $j / 47)); $n++
                if ($c.R -lt 16 -and $c.G -lt 16 -and $c.B -lt 16) { $black++ }
                if ($c.R -lt 235 -or $c.G -lt 235 -or $c.B -lt 235) { $ink++ } } }   # not near-white: drawn content (a white window has none)
        } finally { $img.Dispose() }
        return "appcheck: title '$($app.MainWindowTitle)'; tabs $($tabs.Count): $($tabs -join ', '); black $([math]::Round($black / $n, 2)) of the window; rows $rows; ink $([math]::Round($ink / $n, 3)); $shot"
    } catch { return 'appcheck: ' + ($_.Exception.Message -replace '[\r\n]+', ' ') }
}
# read only: each element of process NAME's top-level windows in this session (rekordbox's or our
# app's) as "depth|ControlType|Name|AutomationId|ClassName|enabled|offscreen|toggle|rect" lines in
# %TEMP%\rbst\uiatree-NAME.txt (at most 4000); the answer names the file and the count
function Save-UiaTree([string]$name) {
    if ($name -notin 'rekordbox', 'RB Stems Plus') { return "uiatree: not a kit process name: '$name'" }
    Uia
    $A = [Windows.Automation.AutomationElement]; $walker = [Windows.Automation.TreeWalker]::ControlViewWalker
    $file = Join-Path $B ("uiatree-$name.txt" -replace ' ', '-')
    $lines = New-Object System.Collections.Generic.List[string]
    function Walk($e, [int]$d) {
        if ($lines.Count -ge 4000) { return }
        $c = $e.Current; $tg = ''
        $p = $null; if ($e.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$p)) { $tg = [string]$p.Current.ToggleState }
        $lines.Add("$d|$($c.ControlType.ProgrammaticName)|$($c.Name)|$($c.AutomationId)|$($c.ClassName)|$($c.IsEnabled)|$($c.IsOffscreen)|$tg|$($c.BoundingRectangle)")
        $ch = $walker.GetFirstChild($e)
        while ($ch) { Walk $ch ($d + 1); $ch = $walker.GetNextSibling($ch) }
    }
    foreach ($p in @(Get-Process -Name $name -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sid })) {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $p.Id)
        foreach ($top in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) { Walk $top 0 }
    }
    Set-Content -LiteralPath $file -Value $lines -Encoding UTF8
    return "uiatree: $($lines.Count) element(s) in $file"
}
# our app's main window back from the minimized state the front step leaves it in, through its UI
# Automation window pattern as there: minimized, its tabs and buttons are offscreen to Find-El
function Restore-App($app) {
    Uia
    $V = [Windows.Automation.WindowVisualState]; $wp = $null
    if (-not ([Windows.Automation.AutomationElement]::FromHandle($app.MainWindowHandle)).TryGetCurrentPattern([Windows.Automation.WindowPattern]::Pattern, [ref]$wp)) { return }
    if ($wp.Current.WindowVisualState -ne $V::Minimized) { return }
    $wp.SetWindowVisualState($V::Normal)
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($wp.Current.WindowVisualState -eq $V::Minimized -and $t.Elapsed.TotalSeconds -lt 3) { Start-Sleep -Milliseconds 100 }
}
# hangcheck: a white or empty window told apart: the UI thread blocked (no answer to WM_NULL in 5 s)
# or free; the --playlists helper still running or never started; the dump holds every thread's
# stack (and the GC heap, for SOS) either way. Read only toward the app: nothing is clicked or sent
# but WM_NULL.
function Get-HangCheck {
    if (-not ('Rbsp.Hang' -as [type])) { Add-Type -Name Hang -Namespace Rbsp -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint ms, out IntPtr r);
[DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
[DllImport("dbghelp.dll", SetLastError = true)] public static extern bool MiniDumpWriteDump(IntPtr p, uint pid, Microsoft.Win32.SafeHandles.SafeFileHandle f, uint type, IntPtr e, IntPtr u, IntPtr c);
'@ }
    $p = Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    $parts = New-Object System.Collections.Generic.List[string]
    if (-not $p) { $parts.Add('no app window') }
    else {
        $res = [IntPtr]::Zero; $sw = [Diagnostics.Stopwatch]::StartNew()
        $ok = [Rbsp.Hang]::SendMessageTimeoutW($p.MainWindowHandle, 0, [IntPtr]::Zero, [IntPtr]::Zero, 0x2, 5000, [ref]$res) -ne [IntPtr]::Zero   # WM_NULL, SMTO_ABORTIFHUNG
        $parts.Add("app pid $($p.Id): UI thread $(if ($ok) { "answers ($($sw.ElapsedMilliseconds) ms)" } else { "does NOT answer in 5 s" }), hung window $([Rbsp.Hang]::IsHungAppWindow($p.MainWindowHandle)), threads $($p.Threads.Count)")
    }
    $h = @(Get-CimInstance Win32_Process -Filter "Name = 'rbsp-prepare.exe'" -ErrorAction SilentlyContinue)
    $parts.Add("helpers $($h.Count)" + (($h | ForEach-Object { "; pid $($_.ProcessId) since $($_.CreationDate.ToString('HH:mm:ss')): $($_.CommandLine)" }) -join ''))
    if ($p) {
        $d = Join-Path $B 'hang'; [void][IO.Directory]::CreateDirectory($d)
        $f = Join-Path $d ("app-$($p.Id)-" + (Get-Date).ToString('HHmmss') + '.dmp')
        # WithDataSegs | WithHandleData | WithUnloadedModules | WithPrivateReadWriteMemory | WithFullMemoryInfo | WithThreadInfo | IgnoreInaccessibleMemory
        $fs = [IO.File]::Open($f, 'CreateNew', 'ReadWrite', 'None')
        try { $ok = [Rbsp.Hang]::MiniDumpWriteDump($p.Handle, [uint32]$p.Id, $fs.SafeFileHandle, [uint32]0x21A25, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero); $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
        finally { $fs.Dispose() }
        $parts.Add($(if ($ok) { "dump $f ($([math]::Round((Get-Item -LiteralPath $f).Length / 1MB)) MB)" } else { "no dump ($('{0:X}' -f $err))" }))
    }
    'hangcheck: ' + ($parts -join '; ')
}
# the first element of the app's windows (or the windows themselves: a dialog) named NAME of
# control type TYPE (waits up to SECS)
function Find-El($app, [string]$name, $type, [int]$secs, [switch]$Enabled) {
    $A = [Windows.Automation.AutomationElement]
    $t = [Diagnostics.Stopwatch]::StartNew()
    do {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            $nc = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($A::NameProperty, $name)), (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $type)))
            $e = $w.FindFirst([Windows.Automation.TreeScope]::Subtree, $nc)
            if ($e -and -not $e.Current.IsOffscreen -and (-not $Enabled -or $e.Current.IsEnabled)) { return $e }
        }
        if ($secs -gt 0) { Start-Sleep -Milliseconds 500 }
    } while ($t.Elapsed.TotalSeconds -lt $secs)
    return $null
}
# the Prepare tab's playlist row NAME: its spoken name is the outline's title, "NAME  (TRACKS)" for a
# playlist (prepare_tab.c's "\(name)  (\(tracks))", the Mac's), NAME alone for a folder
function Find-Playlist($app, [string]$name, [int]$secs) {
    $A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
    $t = [Diagnostics.Stopwatch]::StartNew()
    do {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::TreeItem)))) {
                $n = $e.Current.Name
                $ours = $n -ceq $name -or ($n.StartsWith($name, [StringComparison]::Ordinal) -and $n.Substring($name.Length) -cmatch '^  \(\d+\)$')
                if ($ours -and -not $e.Current.IsOffscreen) { return $e }
            }
        }
        if ($secs -gt 0) { Start-Sleep -Milliseconds 500 }
    } while ($t.Elapsed.TotalSeconds -lt $secs)
    return $null
}
function Invoke-El($e) { $p = $null; if ($e -and $e.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$p)) { $p.Invoke(); return $true }; return $false }
function Select-El($e) { $p = $null; if ($e -and $e.TryGetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern, [ref]$p)) { $p.Select(); return $true }; return $false }

function Screen-Dpi {
    if (-not ('Rbsp.Dpi' -as [type])) { Add-Type -Name Dpi -Namespace Rbsp -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();' }
    [void][Rbsp.Dpi]::SetProcessDPIAware()
}
function Shot {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    Screen-Dpi   # the PC runs at 200 %: capture real pixels, the whole screen
    $s = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $s.Width, $s.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($s.Left, $s.Top, 0, 0, $bmp.Size)
    $f = Join-Path $B ('shots\auto-' + (Get-Date).ToString('HHmmss-fff') + '.png')
    $bmp.Save($f, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
    'shot ' + $f
}
# closed as their X does (WM_CLOSE to that window only); nothing inside is clicked
function Close-Windows([IntPtr[]]$handles) {
    Add-Type -Name Wm -Namespace Rbsp -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);'
    foreach ($h in $handles) { [void][Rbsp.Wm]::PostMessageW($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
}

function Initialize-RbspWin {
    if (-not ('RbspWin' -as [type])) { Add-Type -TypeDefinition @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class RbspWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr dialog, int id);
  [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsChild(IntPtr parent, IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
  static extern IntPtr SendMessageTimeoutW(IntPtr h, uint message, UIntPtr wParam, StringBuilder text,
                                          uint flags, uint timeout, out UIntPtr result);
  [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", ExactSpelling = true)] public static extern int GetWindowLongW(IntPtr h, int index);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint relation);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll", ExactSpelling = true)] public static extern bool PostMessageW(IntPtr h, uint message, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int value, int size);
  [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static int Pid(IntPtr h) {
    uint pid; return GetWindowThreadProcessId(h, out pid) == 0 ? 0 : (int)pid;
  }
  public static List<IntPtr> HandlesForProcess(int pid) {
    var windows = new List<IntPtr>();
    EnumWindows((h, l) => {
      if (Pid(h) == pid) windows.Add(h);
      return true;
    }, IntPtr.Zero);
    return windows;
  }
  public static string WindowClass(IntPtr h) {
    var s = new StringBuilder(256); GetClassNameW(h, s, s.Capacity); return s.ToString();
  }
  public static string WindowCaption(IntPtr h) {
    var s = new StringBuilder(256); GetWindowTextW(h, s, s.Capacity); return s.ToString();
  }
  public static List<IntPtr> DialogChildren(IntPtr dialog, int pid) {
    var children = new List<IntPtr>();
    EnumChildWindows(dialog, (h, l) => {
      if (Pid(h) == pid) children.Add(h);
      return true;
    }, IntPtr.Zero);
    return children;
  }
  public static string DialogControlText(IntPtr dialog, IntPtr h, int pid, int id) {
    if (Pid(dialog) != pid || Pid(h) != pid || !IsChild(dialog, h) || GetDlgCtrlID(h) != id)
      return null;
    // Static labels can share IDC_STATIC (-1), so only resolve positive IDs.
    if (id > 0 && GetDlgItem(GetParent(h), id) != h) return null;
    var s = new StringBuilder(8192);
    int length = GetWindowTextW(h, s, s.Capacity);
    if (length == 0) {
      // GetWindowText cannot read all controls across processes. Bound the WM_GETTEXT fallback.
      UIntPtr count;
      if (SendMessageTimeoutW(h, 0x000D, new UIntPtr((uint)s.Capacity), s, 3, 300, out count) == IntPtr.Zero)
        return null;
      length = (int)count.ToUInt32();
    }
    return length < s.Capacity - 1 && Pid(h) == pid && IsChild(dialog, h) ? s.ToString() : null;
  }
  public static bool? IsCloaked(IntPtr h) {
    int value;
    return DwmGetWindowAttribute(h, 14, out value, 4) == 0 ? (bool?)(value != 0) : null;
  }
  // A screen crop must not collect pixels from a different app above the target.
  public static bool NoForeignOverlap(IntPtr h, int[] allowedPids, int left, int top, int right, int bottom) {
    var seen = new HashSet<IntPtr>();
    for (var w = GetWindow(h, 3); w != IntPtr.Zero; w = GetWindow(w, 3)) { // GW_HWNDPREV
      if (!seen.Add(w)) return false;
      if (!IsWindowVisible(w) || IsIconic(w) || IsCloaked(w) == true || Array.IndexOf(allowedPids, Pid(w)) >= 0) continue;
      RECT over;
      if (!GetWindowRect(w, out over) || (left < over.R && right > over.L && top < over.B && bottom > over.T)) return false;
    }
    return true;
  }
  // The OCR evidence must come from the updater's client area, with no other window over it.
  public static RECT UncoveredClient(IntPtr h) {
    RECT r; POINT p = new POINT();
    if (!IsWindowVisible(h) || !GetClientRect(h, out r) || !ClientToScreen(h, ref p))
      throw new InvalidOperationException("updater client area unavailable");
    r.L += p.X; r.R += p.X; r.T += p.Y; r.B += p.Y;
    var seen = new HashSet<IntPtr>();
    for (var w = GetWindow(h, 3); w != IntPtr.Zero; w = GetWindow(w, 3)) { // GW_HWNDPREV
      if (!seen.Add(w)) throw new InvalidOperationException("window order changed");
      RECT over; int cloaked;
      if (!IsWindowVisible(w) || (DwmGetWindowAttribute(w, 14, out cloaked, 4) == 0 && cloaked != 0)) continue;
      if (!GetWindowRect(w, out over) || (r.L < over.R && r.R > over.L && r.T < over.B && r.B > over.T))
        throw new InvalidOperationException("updater client area obscured");
    }
    return r;
  }
  [DllImport("user32.dll")] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
  [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetUserObjectInformationW(IntPtr h, int index, StringBuilder name, int size, out int needed);
  public static bool OrdinaryDesktop() {
    var h = OpenInputDesktop(0, false, 1);
    if (h == IntPtr.Zero) return false;
    try {
      var name = new StringBuilder(64); int needed;
      return GetUserObjectInformationW(h, 2, name, name.Capacity * sizeof(char), out needed) &&
        String.Equals(name.ToString(), "Default", StringComparison.OrdinalIgnoreCase);
    } finally { CloseDesktop(h); }
  }
  public static List<string> List() { return List(false); }
  public static List<string> List(bool allSizes) {
    var o = new List<string>();
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      var s = new StringBuilder(256); GetWindowTextW(h, s, 256); uint pid; GetWindowThreadProcessId(h, out pid);
      RECT r; GetWindowRect(h, out r);
      if (!allSizes && (r.R - r.L < 50 || r.B - r.T < 50)) return true;
      string n = "?"; try { n = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
      o.Add(n + " | " + s.ToString() + " | " + (r.R - r.L) + "x" + (r.B - r.T) + " | 0x" + h.ToString("X"));
      return true; }, IntPtr.Zero);
    return o; }
}
"@
    }
}

function Get-DesktopWindows([switch]$AllSizes) {
    Initialize-RbspWin
    if ($AllSizes) { [RbspWin]::List($true) } else { [RbspWin]::List() }
}

function Assert-RbNoMessageBox([switch]$AllowPicker) {
    . (Join-Path $Kit 'winlist.ps1')
    if ($AllowPicker) { . (Join-Path $Kit 'format-ui.ps1') }
    $dialogs = @(Get-RbMessageBoxes $sid -AllowPicker:$AllowPicker)
    if ($dialogs.Count) { throw ('rekordbox #32770 message box: ' + ($dialogs -join '; ')) }
}

# rekordbox's "Analysis Setting" dialog (VM 2026-11-22: shown on a Load when the profile has no
# askAnalysisSettingWindow=0): a visible, unowned JUCE window of rekordbox's with no caption, whose
# title band reads exactly "Analysis Setting" (OCR of a screenshot, read only). Its handles; the
# owner has not approved clicking it, so callers only name it in their STOP.
function Get-RbAnalysisSettingDialogs {
    $processes = @(Get-RbProcess $sid)
    if (-not $processes.Count) { return }
    Initialize-RbspWin
    $handles = @(foreach ($process in $processes) {
        foreach ($h in [RbspWin]::HandlesForProcess($process.Id)) {
            if ([RbspWin]::Pid($h) -eq $process.Id -and [RbspWin]::IsWindowVisible($h) -and -not [RbspWin]::IsIconic($h) -and
                [RbspWin]::IsCloaked($h) -eq $false -and [RbspWin]::WindowClass($h) -clike 'JUCE_*' -and
                -not [RbspWin]::WindowCaption($h) -and [RbspWin]::GetWindow($h, 4) -eq [IntPtr]::Zero) { $h }   # GW_OWNER
        }
    })
    if (-not $handles.Count) { return }
    Screen-Dpi   # real pixels, as the screenshot
    $candidates = @(foreach ($h in $handles) {
        $r = New-Object RbspWin+RECT
        if (-not [RbspWin]::GetWindowRect($h, [ref]$r)) { continue }
        # 402x362 at 100 %: a dialog, not the main window or a menu
        if ($r.R - $r.L -lt 250 -or $r.B - $r.T -lt 200 -or $r.R - $r.L -gt 1400 -or $r.B - $r.T -gt 1200) { continue }
        @{ Handle = $h; Rect = $r }
    })
    if (-not $candidates.Count) { return }
    $png = (Shot).Substring(5)
    $screen = [System.Windows.Forms.SystemInformation]::VirtualScreen   # Shot loaded Windows Forms
    if (-not (Get-Command Read-ScreenText -ErrorAction SilentlyContinue)) { . (Join-Path $Kit 'ocr-lib.ps1') }
    foreach ($c in $candidates) {
        $r = $c.Rect
        $band = [math]::Max(24, [int](($r.B - $r.T) * 0.14))
        $x = $r.L - $screen.Left; $y = $r.T - $screen.Top
        if ($x -lt 0 -or $y -lt 0 -or $r.R - $screen.Left -gt $screen.Width -or $y + $band -gt $screen.Height) { continue }
        foreach ($plain in $false, $true) {
            $title = ((Read-ScreenText $png $x $y ($r.R - $r.L) $band -Plain:$plain) -replace '\s+', ' ').Trim()
            if ($title -ceq 'Analysis Setting') { '0x' + $c.Handle.ToString('X'); break }
        }
    }
}

function Assert-RbNoAnalysisSetting {
    $dialogs = @(Get-RbAnalysisSettingDialogs)
    if ($dialogs.Count) { throw ('rekordbox Analysis Setting dialog: ' + ($dialogs -join ', ')) }
}

# The watcher uses a Win32 TaskDialog and assigns its action buttons IDs starting at 100.
function Get-WatcherOfferWindows {
    $watchers = @(Get-Process -Name 'rbsp-watch-x64', 'rbsp-watch-arm64' -ErrorAction SilentlyContinue | Where-Object SessionId -eq $sid)
    if (-not $watchers.Count) { return }
    Initialize-RbspWin
    if (-not [RbspWin]::OrdinaryDesktop()) { throw 'RB Stems Plus watcher: ordinary desktop unavailable' }
    foreach ($watcher in $watchers) {
        foreach ($h in [RbspWin]::HandlesForProcess($watcher.Id)) {
            if ([RbspWin]::Pid($h) -eq $watcher.Id -and [RbspWin]::IsWindowVisible($h)) {
                @{ Handle = $h; Process = $watcher }
            }
        }
    }
}

function Assert-WatcherOfferWindow([IntPtr]$dialog, $watcher) {
    $root = [regex]::Escape($env:LOCALAPPDATA.TrimEnd('\') + '\Programs\RB Stems Plus\')
    $image = $root + '\d+(?:\.\d+){2}\\' + [regex]::Escape($watcher.ProcessName + '.exe') + '$'
    $failed = @(
        if ($watcher.Path -notmatch ('^' + $image)) { 'watcher image' }
        if (-not [RbspWin]::OrdinaryDesktop()) { 'ordinary desktop' }
        if ([RbspWin]::Pid($dialog) -ne $watcher.Id -or [RbspWin]::GetWindow($dialog, 4) -ne [IntPtr]::Zero) { 'dialog ownership' }
        if ([RbspWin]::WindowClass($dialog) -cne '#32770') { 'dialog class' }
        if ([RbspWin]::WindowCaption($dialog) -cne 'RB Stems Plus') { 'dialog caption' }
        if (-not [RbspWin]::IsWindowVisible($dialog)) { 'dialog visibility' }
        if (-not [RbspWin]::IsWindowEnabled($dialog)) { 'dialog enabled' }
        if ([RbspWin]::IsCloaked($dialog) -ne $false) { 'dialog cloak state' }
    )
    if ($failed.Count) { throw ('RB Stems Plus watcher: failed ' + ($failed -join ', ')) }
}

function Get-WatcherOffer([IntPtr]$dialog, $watcher) {
    Assert-WatcherOfferWindow $dialog $watcher
    $buttons = @([RbspWin]::DialogChildren($dialog, $watcher.Id) | Where-Object {
        [RbspWin]::WindowClass($_) -ceq 'Button' -and [RbspWin]::IsWindowVisible($_)
    })
    if ($buttons.Count -ne 3) { throw 'RB Stems Plus watcher: expected three offer buttons' }
    $controls = @(foreach ($button in $buttons) {
        $controlId = [RbspWin]::GetDlgCtrlID($button)
        [pscustomobject]@{ Handle = $button; Id = $controlId; Text = [RbspWin]::DialogControlText($dialog, $button, $watcher.Id, $controlId) }
    })
    $ids = ($controls.Id | Sort-Object) -join ','
    # TaskDialog can expose native Button HWNDs with ID 0. Its action IDs
    # still come from TASKDIALOGCONFIG, including 101 for Remind Me Later.
    $installLabels = 'Install Now', 'Reinstall Now'
    $dismissLabels = "Don't Ask Again for This Version", "Don't Ask Again for These Versions"
    $install = @($controls | Where-Object { $_.Text -cin $installLabels })
    $later = @($controls | Where-Object { $_.Text -ceq 'Remind Me Later' })
    $dismiss = @($controls | Where-Object { $_.Text -cin $dismissLabels })
    $failed = @(
        if ($ids -cnotin '0,0,0', '100,101,102') { 'button IDs' }
        if ($install.Count -ne 1 -or ($ids -ceq '100,101,102' -and $install[0].Id -ne 100)) { 'install text/control' }
        if ($later.Count -ne 1 -or ($ids -ceq '100,101,102' -and $later[0].Id -ne 101)) { 'reminder text/control' }
        if ($dismiss.Count -ne 1 -or ($ids -ceq '100,101,102' -and $dismiss[0].Id -ne 102)) { 'dismiss text/control' }
        if ($ids -ceq '0,0,0' -and ($controls[0].Text -cnotin $installLabels -or
            $controls[1].Text -cne 'Remind Me Later' -or $controls[2].Text -cnotin $dismissLabels)) { 'button order' }
        if ($later.Count -eq 1 -and (-not [RbspWin]::IsWindowEnabled($later[0].Handle) -or
            -not [RbspWin]::IsWindowVisible($later[0].Handle))) { 'reminder button enabled and visible' }
    )
    if ($failed.Count) {
        $knownLabels = $installLabels + 'Remind Me Later' + $dismissLabels
        $observed = [ordered]@{
            caption = 'RB Stems Plus'
            buttons = @(foreach ($control in $controls) {
                [ordered]@{ Id = $control.Id; Text = $(if ($control.Text -cin $knownLabels) { $control.Text } else { '<unrecognized>' }) }
            })
        }
        throw ('RB Stems Plus watcher: failed ' + ($failed -join ', ') + ' | ' + ($observed | ConvertTo-Json -Compress))
    }

    $texts = @(foreach ($child in [RbspWin]::DialogChildren($dialog, $watcher.Id)) {
        if ([RbspWin]::WindowClass($child) -cin 'Static', 'SysLink' -and [RbspWin]::IsWindowVisible($child)) {
            [RbspWin]::DialogControlText($dialog, $child, $watcher.Id, [RbspWin]::GetDlgCtrlID($child))
        }
    })
    $versionPattern = '[0-9]+(?:\.[0-9]+)+'
    $bodyPattern = "\AStems Cache isn't on in rekordbox ($versionPattern(?:, $versionPattern)*) yet\.\z"
    $versions = @(Get-WatcherOfferVersions $texts $bodyPattern)
    if (-not $versions.Count) {
        # TaskDialog can expose its text through UI Automation without readable native labels.
        $uia = Get-WatcherOfferUiaContent $dialog $watcher.Id
        if ($uia.Status -ceq 'read') {
            $texts = @($uia.Elements | Where-Object { $_.Type -cin 'ControlType.Text', 'ControlType.Hyperlink' } |
                ForEach-Object { $_.Text })
            $versions = @(Get-WatcherOfferVersions $texts $bodyPattern)
        }
        if (-not $versions.Count) { throw "RB Stems Plus watcher: offer text did not match (UI Automation: $($uia.Status))" }
    }
    if (@($versions | Sort-Object -Unique).Count -ne $versions.Count) {
        throw 'RB Stems Plus watcher: offer text repeated a version'
    }
    @{ Versions = $versions; Reminder = $later[0] }
}

function Get-WatcherOfferVersions([string[]]$Texts, [string]$BodyPattern) {
    $instruction = @($Texts | Where-Object { $_ -ceq 'rekordbox was updated' })
    $content = @($Texts | Where-Object { $_ -clike "Stems Cache isn't on in rekordbox *" })
    if ($instruction.Count -eq 1 -and $content.Count -eq 1 -and $content[0] -cmatch $BodyPattern) {
        $Matches[1] -split ', '
    }
}

function Get-WatcherOfferUiaContent([IntPtr]$Dialog, [int]$ProcessId) {
    . (Join-Path $Kit 'winlist.ps1')
    Get-RbDirectUiContent $Dialog $ProcessId -RawText
}

function Defer-WatcherOffer {
    $offers = @(Get-WatcherOfferWindows)
    if (-not $offers.Count) { return }
    $installed = @((Get-RbInstalls).Version | Sort-Object -Unique)
    $deferred = @{}
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($offers.Count) {
        if ($offers.Count -ne 1) { throw 'RB Stems Plus watcher: expected one offer window' }
        if ($deferred.Count -ge $installed.Count) { throw 'RB Stems Plus watcher: installed-version deferral limit reached' }
        $dialog = $offers[0].Handle
        $watcher = $offers[0].Process
        $offer = Get-WatcherOffer $dialog $watcher
        foreach ($version in $offer.Versions) {
            if ($version -cnotin $installed) { throw "RB Stems Plus watcher: offered rekordbox $version is not installed" }
            if ($deferred.ContainsKey($version)) { throw "RB Stems Plus watcher: rekordbox $version was already deferred" }
        }

        Assert-WatcherOfferWindow $dialog $watcher
        $button = $offer.Reminder
        if (-not [RbspWin]::IsWindowEnabled($button.Handle) -or -not [RbspWin]::IsWindowVisible($button.Handle) -or
            [RbspWin]::DialogControlText($dialog, $button.Handle, $watcher.Id, $button.Id) -cne 'Remind Me Later') {
            throw 'RB Stems Plus watcher: reminder text/control changed'
        }
        if ($timer.ElapsedMilliseconds -ge 8000) { throw 'RB Stems Plus watcher: deferral timed out' }
        # TDM_CLICK_BUTTON targets the verified TaskDialog and its Remind Me Later action.
        if (-not [RbspWin]::PostMessageW($dialog, 0x0466, [IntPtr]101, [IntPtr]::Zero)) {
            throw 'RB Stems Plus watcher: could not press Remind Me Later'
        }
        while ([RbspWin]::IsWindow($dialog) -and [RbspWin]::Pid($dialog) -eq $watcher.Id) {
            if ($timer.ElapsedMilliseconds -ge 8000) { throw 'RB Stems Plus watcher: the verified offer is still open after Remind Me Later' }
            Start-Sleep -Milliseconds 100
        }
        foreach ($version in $offer.Versions) {
            $deferred[$version] = $true
            Say "RB Stems Plus watcher: deferred rekordbox $version"
        }

        # The next offer can appear after the one we answered has been destroyed.
        $quietUntil = $timer.ElapsedMilliseconds + 1000
        do {
            $offers = @(Get-WatcherOfferWindows)
            if ($offers.Count -or $timer.ElapsedMilliseconds -ge $quietUntil) { break }
            if ($timer.ElapsedMilliseconds -ge 8000) { throw 'RB Stems Plus watcher: deferral timed out' }
            Start-Sleep -Milliseconds 100
        } while ($true)
    }
    'RB Stems Plus watcher: pressed Remind Me Later'
}

function Dismiss-RbError {
    Initialize-RbspWin
    if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable' }
    $processes = @(Get-RbProcess $sid)
    if ($processes.Count -ne 1) { throw 'expected one rekordbox process' }
    Assert-RbDesktopProcess $processes[0]
    $processId = $processes[0].Id
    $dialogs = @([RbspWin]::HandlesForProcess($processId) | Where-Object {
        [RbspWin]::IsWindowVisible($_) -and [RbspWin]::WindowClass($_) -ceq '#32770'
    })
    if ($dialogs.Count -ne 1) { throw 'expected one visible rekordbox Win32 dialog' }
    $dialog = $dialogs[0]
    if ([RbspWin]::WindowCaption($dialog) -cne 'rekordbox' -or -not [RbspWin]::IsWindowEnabled($dialog) -or
        [RbspWin]::GetWindow($dialog, 4) -ne [IntPtr]::Zero -or [RbspWin]::IsCloaked($dialog) -ne $false) {
        throw 'rekordbox error window identity did not match'
    }

    $controls = @(Get-RbDialogControls $dialog $processId)
    $buttons = @($controls | Where-Object { $_.Class -ceq 'Button' })
    if ($buttons.Count -ne 1 -or -not $buttons[0].Visible -or -not $buttons[0].Enabled -or
        ($buttons[0].Style -band 0x0F) -notin 0, 1 -or
        -not $buttons[0].TextRead -or $buttons[0].Text -cne 'OK' -or
        @($controls | Where-Object { $_.Visible -and $_.Class -cnotin 'DirectUIHWND', 'CtrlNotifySink', 'Button' }).Count) {
        throw 'expected a DirectUI message with a single enabled OK button'
    }
    $content = Get-RbDirectUiContent $dialog $processId -RawText -AllElements -ExcludeTitleBar
    $messages = @($content.Elements | Where-Object { $_.Type -ceq 'ControlType.Text' })
    # The DirectUI error icon is an Image in the raw UIA tree.
    $icons = @($content.Elements | Where-Object { $_.Type -ceq 'ControlType.Image' })
    $activeIcons = @($icons | Where-Object {
        $_.Focusable -ne $false -or ($_.Patterns.Count -and
            ($_.Patterns.Count -ne 1 -or $_.Patterns[0] -cne 'ValuePatternIdentifiers.Pattern' -or $_.ReadOnlyValue -ne $true))
    })
    $other = @($content.Elements | Where-Object { $_.Type -cnotin 'ControlType.Text', 'ControlType.Pane', 'ControlType.Group', 'ControlType.Window', 'ControlType.Image' })
    if ($content.Status -cne 'read' -or $messages.Count -ne 1 -or
        -not [string]::Equals($messages[0].Text, 'Unexpected application error', [StringComparison]::Ordinal) -or
        $icons.Count -gt 1 -or $activeIcons.Count -or
        $other.Count -gt 1 -or ($other.Count -eq 1 -and ($other[0].Type -cne 'ControlType.Button' -or
            -not $other[0].Enabled -or -not [string]::Equals($other[0].Text, 'OK', [StringComparison]::Ordinal)))) {
        throw 'rekordbox dialog text or actions did not match the known error'
    }

    $button = [IntPtr][Convert]::ToInt64($buttons[0].Handle.Substring(2), 16)
    if (-not [RbspWin]::OrdinaryDesktop() -or [RbspWin]::WindowClass($dialog) -cne '#32770' -or
        [RbspWin]::WindowClass($button) -cne 'Button' -or -not [RbspWin]::IsWindowVisible($button) -or
        -not [RbspWin]::IsWindowEnabled($button) -or
        [RbspWin]::DialogControlText($dialog, $button, $processId, $buttons[0].Id) -cne 'OK') {
        throw 'the rekordbox error button or desktop changed'
    }
    if (-not [RbspWin]::PostMessageW($button, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)) { throw 'could not post BM_CLICK to OK' }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ([RbspWin]::IsWindow($dialog) -and $timer.Elapsed.TotalSeconds -lt 8) { Start-Sleep -Milliseconds 200 }
    if ([RbspWin]::IsWindow($dialog)) { throw 'the error dialog is still open after BM_CLICK' }
    'BM_CLICK closed the verified OK-only error dialog'
}

function Close-RbUpdate([string]$arg) {
    $details = @()
    try {
        if ($arg -notmatch '^0x([0-9A-F]{1,16})$') { throw 'invalid window handle' }
        $h = [IntPtr][Convert]::ToInt64($Matches[1], 16)
        $title = 'rekordbox update manager'
        $windows = @(Get-DesktopWindows)
        if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable' }
        $windowPattern = '^Upmgr rekordbox \| ' + [regex]::Escape($title) + ' \| \d+x\d+ \| ' + $arg + '$'
        if (-not @($windows -cmatch $windowPattern).Count) { throw 'the updater process name or window title did not match' }
        $processId = [RbspWin]::Pid($h)
        $p = Get-Process -Id $processId
        $install = Join-Path $env:ProgramFiles 'rekordbox'
        $image = '^' + [regex]::Escape($install + '\') + 'rekordbox \d+(\.\d+)+\\Upmgr rekordbox\.exe$'
        if ($p.ProcessName -ne 'Upmgr rekordbox' -or $p.SessionId -ne $sid -or $p.Path -notmatch $image -or
            -not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $p.Path) 'rekordbox.exe') -PathType Leaf)) {
            throw 'the window process image is outside the rekordbox install folders'
        }
        $owner = [RbspWin]::GetWindow($h, 4)   # GW_OWNER, separate from the process that created the dialog
        $ownerHandle = '0x' + $owner.ToString('X')
        $ownerRow = $windows | Where-Object { $_.EndsWith(' | ' + $ownerHandle) } | Select-Object -First 1
        $details += "$arg pid $processId ($($p.Path)), owner $(if ($ownerRow) { $ownerRow.Replace(' | ', ' / ') } else { $ownerHandle })"

        Screen-Dpi
        $rect = [RbspWin]::UncoveredClient($h)
        $before = Shot; $details += "before $before"
        $afterRect = [RbspWin]::UncoveredClient($h)
        if (-not $rect.Equals($afterRect)) { throw 'updater bounds changed during the screenshot' }
        $screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
        $w = $rect.R - $rect.L; $height = $rect.B - $rect.T
        if ($w -le 0 -or $height -le 0 -or $rect.L -lt $screen.Left -or $rect.T -lt $screen.Top -or
            $rect.R -gt $screen.Right -or $rect.B -gt $screen.Bottom) { throw 'updater client area is off screen' }
        . (Join-Path $Kit 'ocr-lib.ps1')
        $message = Read-ScreenText ($before.Substring(5)) ($rect.L - $screen.Left) ($rect.T - $screen.Top) $w $height -Plain
        if ($message -notmatch '\bupdate\s+packages\b') { throw 'the updater text did not contain "update packages"' }
        $versions = [regex]::Matches($message, '\b\d+\.\d+\.\d+(?:\.\d+)?\b')
        if ($versions.Count -ne 2) { throw 'could not read the displayed current and update versions' }
        $details += "$($versions[0].Value) -> $($versions[1].Value)"

        Uia
        $A = [Windows.Automation.AutomationElement]
        $CT = [Windows.Automation.ControlType]
        # The updater is a JUCE window (PC, 7.2.19): its UI Automation element is named and
        # identified after the component, "Upmgr rekordbox", not after the Win32 title checked above.
        # Its Start and Close buttons are JUCE elements of that window, with no window of their own,
        # identified as "<name>.<empty>.Upmgr rekordbox". The offer's text is not exposed (OCR above).
        $uiaName = 'Upmgr rekordbox'
        $dialog = $A::FromHandle($h)
        $c = $dialog.Current
        if ($c.Name -cne $uiaName -or $c.AutomationId -cne $uiaName -or $c.FrameworkId -cne 'JUCE' -or
            $c.ControlType -ne $CT::Window -or [int64]$c.NativeWindowHandle -ne $h.ToInt64() -or
            $c.ProcessId -ne $processId -or $c.IsOffscreen) {
            throw "the UI Automation dialog did not match the window (name '$($c.Name)', id '$($c.AutomationId)', framework '$($c.FrameworkId)')"
        }
        $cond = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)
        $buttons = @($dialog.FindAll([Windows.Automation.TreeScope]::Descendants, $cond) | Where-Object {
            $_.Current.ProcessId -eq $processId -and -not $_.Current.IsOffscreen -and
            [Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($_).Current.ControlType -ne $CT::TitleBar
        })
        $start = @($buttons | Where-Object { $_.Current.Name -ceq 'Start' -and $_.Current.AutomationId -ceq "Start.<empty>.$uiaName" })
        $close = @($buttons | Where-Object { $_.Current.Name -ceq 'Close' -and $_.Current.AutomationId -ceq "Close.<empty>.$uiaName" })
        if ($start.Count -ne 1 -or $close.Count -ne 1 -or -not $close[0].Current.IsEnabled) {
            throw 'expected one Start and one enabled Close button in the dialog'
        }
        if (-not [RbspWin]::OrdinaryDesktop() -or [RbspWin]::Pid($h) -ne $processId -or
            -not @((Get-DesktopWindows) -cmatch $windowPattern).Count) { throw 'the updater window or input desktop changed' }
        try {
            if (-not (Invoke-El $close[0])) { throw 'Close has no Invoke pattern' }
            $details += 'UI Automation invoked Close'
            $t = [Diagnostics.Stopwatch]::StartNew()
            while ([RbspWin]::IsWindow($h) -and $t.Elapsed.TotalSeconds -lt 5) { Start-Sleep -Milliseconds 200 }
            if ([RbspWin]::IsWindow($h)) { throw 'the updater window is still open after Close' }
            if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable after Close' }
        } finally {
            # the screen repaints after the window is destroyed: a shot at once can still show it (PC)
            Start-Sleep -Milliseconds 1000
            $details += 'after ' + (Shot)
        }
        return 'closeupdate: closed; ' + ($details -join '; ')
    } catch {
        return 'closeupdate: ' + ($_.Exception.Message -replace '[\r\n]+', ' ') + $(if ($details.Count) { '; ' + ($details -join '; ') })
    }
}

# rekordbox's own news window ("INFO", owner decision 2026-11-19): rekordbox.exe under the rekordbox
# folder, the title, and its INFORMATION and NOTIFICATION tabs checked by UI Automation, then closed
# through its UI Automation window pattern (what its close box does; no key, no click). A JUCE window
# (PC, 7.2.19): its element is named and identified "INFO", the tabs "<tab>.<empty>.<empty>.INFO".
function Close-RbInfo([string]$arg) {
    $details = @()
    try {
        if ($arg -notmatch '^0x([0-9A-F]{1,16})$') { throw 'invalid window handle' }
        $h = [IntPtr][Convert]::ToInt64($Matches[1], 16)
        $title = 'INFO'
        $windows = @(Get-DesktopWindows)   # first: it defines [RbspWin]
        if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable' }
        $windowPattern = '^rekordbox \| ' + $title + ' \| \d+x\d+ \| ' + $arg + '$'
        if (-not @($windows -cmatch $windowPattern).Count) { throw 'the news window process name or title did not match' }
        $processId = [RbspWin]::Pid($h)
        $p = Get-Process -Id $processId
        $install = Join-Path $env:ProgramFiles 'rekordbox'
        $image = '^' + [regex]::Escape($install + '\') + 'rekordbox \d+(\.\d+)+\\rekordbox\.exe$'
        if ($p.ProcessName -ne 'rekordbox' -or $p.SessionId -ne $sid -or $p.Path -notmatch $image) {
            throw 'the window process image is outside the rekordbox install folders'
        }
        $details += "$arg pid $processId ($($p.Path))"

        Uia
        $A = [Windows.Automation.AutomationElement]
        $CT = [Windows.Automation.ControlType]
        $win = $A::FromHandle($h)
        $c = $win.Current
        if ($c.Name -cne $title -or $c.AutomationId -cne $title -or $c.FrameworkId -cne 'JUCE' -or
            $c.ControlType -ne $CT::Window -or [int64]$c.NativeWindowHandle -ne $h.ToInt64() -or
            $c.ProcessId -ne $processId -or $c.IsOffscreen) {
            throw "the UI Automation window did not match (name '$($c.Name)', id '$($c.AutomationId)', framework '$($c.FrameworkId)')"
        }
        $cond = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)
        $buttons = @($win.FindAll([Windows.Automation.TreeScope]::Descendants, $cond) | Where-Object { $_.Current.ProcessId -eq $processId })
        foreach ($tab in 'INFORMATION', 'NOTIFICATION') {
            if (@($buttons | Where-Object { $_.Current.Name -ceq $tab -and $_.Current.AutomationId -ceq "$tab.<empty>.<empty>.$title" }).Count -ne 1) {
                throw "expected one $tab tab in the news window"
            }
        }
        $wp = $null
        if (-not $win.TryGetCurrentPattern([Windows.Automation.WindowPattern]::Pattern, [ref]$wp)) { throw 'the news window has no UI Automation window pattern' }
        Screen-Dpi
        $details += 'before ' + (Shot)
        if (-not [RbspWin]::OrdinaryDesktop() -or [RbspWin]::Pid($h) -ne $processId -or
            -not @((Get-DesktopWindows) -cmatch $windowPattern).Count) { throw 'the news window or input desktop changed' }
        try {
            $wp.Close()
            $details += 'UI Automation closed the window'
            $t = [Diagnostics.Stopwatch]::StartNew()
            while ([RbspWin]::IsWindow($h) -and [RbspWin]::IsWindowVisible($h) -and $t.Elapsed.TotalSeconds -lt 5) { Start-Sleep -Milliseconds 200 }
            if ([RbspWin]::IsWindow($h) -and [RbspWin]::IsWindowVisible($h)) { throw 'the news window is still open after Close' }
            if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable after Close' }
        } finally {
            Start-Sleep -Milliseconds 1000   # the repaint, as for the updater
            $details += 'after ' + (Shot)
        }
        return 'closeinfo: closed; ' + ($details -join '; ')
    } catch {
        return 'closeinfo: ' + ($_.Exception.Message -replace '[\r\n]+', ' ') + $(if ($details.Count) { '; ' + ($details -join '; ') })
    }
}

# rekordboxAgent's news window (owner decision 2026-11-19), wherever it blocks a step: the agent
# under the rekordbox folder, the title, no owner window, and in UI Automation its Electron window
# (named "rekordboxAgent", class Chrome_WidgetWin_1) exposing no buttons; closed through its UI
# Automation window pattern (what its X does; no key, no click), screenshots before and after.
function Close-RbAgentNews([string]$arg) {
    $details = @()
    try {
        if ($arg -notmatch '^0x([0-9A-F]{1,16})$') { throw 'invalid window handle' }
        $h = [IntPtr][Convert]::ToInt64($Matches[1], 16)
        $title = 'rekordboxAgent'
        $windows = @(Get-DesktopWindows)   # first: it defines [RbspWin]
        if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable' }
        $windowPattern = '^rekordboxAgent \| ' + $title + ' \| \d+x\d+ \| ' + $arg + '$'
        if (-not @($windows -cmatch $windowPattern).Count) { throw 'the agent window process name or title did not match' }
        $processId = [RbspWin]::Pid($h)
        $p = Get-Process -Id $processId
        $install = Join-Path $env:ProgramFiles 'rekordbox'
        $image = '^' + [regex]::Escape($install + '\') + 'rekordbox \d+(\.\d+)+\\rekordboxAgent-win32-x64\\rekordboxAgent\.exe$'
        if ($p.ProcessName -ne 'rekordboxAgent' -or $p.SessionId -ne $sid -or $p.Path -notmatch $image) {
            throw 'the window process image is outside the rekordbox install folders'
        }
        if ([RbspWin]::GetWindow($h, 4) -ne [IntPtr]::Zero) { throw 'the agent window has an owner window (a dialog)' }
        $details += "$arg pid $processId ($($p.Path))"

        Uia
        $A = [Windows.Automation.AutomationElement]
        $CT = [Windows.Automation.ControlType]
        $win = $A::FromHandle($h)
        $c = $win.Current
        if ($c.Name -cne $title -or $c.ClassName -cne 'Chrome_WidgetWin_1' -or $c.ControlType -ne $CT::Window -or
            [int64]$c.NativeWindowHandle -ne $h.ToInt64() -or $c.ProcessId -ne $processId) {
            throw "the UI Automation window did not match (name '$($c.Name)', class '$($c.ClassName)')"
        }
        $cond = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)
        $buttons = @($win.FindAll([Windows.Automation.TreeScope]::Descendants, $cond))
        if ($buttons.Count) { throw "the agent window has buttons: $(($buttons | ForEach-Object { "'" + $_.Current.Name + "'" }) -join ', ')" }
        $wp = $null
        if (-not $win.TryGetCurrentPattern([Windows.Automation.WindowPattern]::Pattern, [ref]$wp)) { throw 'the agent window has no UI Automation window pattern' }
        Screen-Dpi
        $details += 'before ' + (Shot)
        if (-not [RbspWin]::OrdinaryDesktop() -or [RbspWin]::Pid($h) -ne $processId -or
            -not @((Get-DesktopWindows) -cmatch $windowPattern).Count) { throw 'the agent window or input desktop changed' }
        try {
            $wp.Close()
            $details += 'UI Automation closed the window'
            $t = [Diagnostics.Stopwatch]::StartNew()
            while ([RbspWin]::IsWindow($h) -and [RbspWin]::IsWindowVisible($h) -and $t.Elapsed.TotalSeconds -lt 5) { Start-Sleep -Milliseconds 200 }
            if ([RbspWin]::IsWindow($h) -and [RbspWin]::IsWindowVisible($h)) { throw 'the agent window is still open after Close' }
            if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable after Close' }
        } finally {
            Start-Sleep -Milliseconds 1000   # the repaint, as for the updater
            $details += 'after ' + (Shot)
        }
        return 'closeagentnews: closed; ' + ($details -join '; ')
    } catch {
        return 'closeagentnews: ' + ($_.Exception.Message -replace '[\r\n]+', ' ') + $(if ($details.Count) { '; ' + ($details -join '; ') })
    }
}

# rekordbox in front before the kit reads its screen (coordinator decision 2026-11-19): our own app's
# main window minimized through its UI Automation window pattern (never closed), then rekordbox's
# verified main window (rekordbox.exe under the rekordbox folder, title 'rekordbox') focused through
# UI Automation and confirmed as the foreground window. No key, no click.
function Set-RbFront {
    try {
        if ($RbVersion) { Assert-RbProcessVersion $sid (Get-Rb) }
        $windows = @(Get-DesktopWindows)   # first: it defines [RbspWin]
        if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable' }
        Assert-RbNoMessageBox
        Assert-RbNoAnalysisSetting
        Uia
        $A = [Windows.Automation.AutomationElement]
        $min = 0
        foreach ($line in $windows) {
            if ($line -notmatch '^RB Stems Plus \| RB Stems Plus \| \d+x\d+ \| 0x([0-9A-F]+)$') { continue }
            $ah = [IntPtr][Convert]::ToInt64($Matches[1], 16)
            $ap = Get-Process -Id ([RbspWin]::Pid($ah))
            if ($ap.ProcessName -ne 'RB Stems Plus' -or $ap.SessionId -ne $sid) { throw "our app's window belongs to another process" }
            $wp = $null
            if (-not ($A::FromHandle($ah)).TryGetCurrentPattern([Windows.Automation.WindowPattern]::Pattern, [ref]$wp)) { throw "our app's window has no UI Automation window pattern" }
            $wp.SetWindowVisualState([Windows.Automation.WindowVisualState]::Minimized)
            $t = [Diagnostics.Stopwatch]::StartNew()
            while (-not [RbspWin]::IsIconic($ah) -and $t.Elapsed.TotalSeconds -lt 3) { Start-Sleep -Milliseconds 100 }
            if (-not [RbspWin]::IsIconic($ah)) { throw "our app's window did not minimize" }
            $min++
        }
        $rb = @($windows -cmatch '^rekordbox \| rekordbox \| \d+x\d+ \| 0x[0-9A-F]+$')
        if ($rb.Count -ne 1) { throw "expected one rekordbox main window, found $($rb.Count)" }
        $handle = ($rb[0] -split ' \| ')[-1]
        $h = [IntPtr][Convert]::ToInt64($handle.Substring(2), 16)
        $p = Get-Process -Id ([RbspWin]::Pid($h))
        $install = Join-Path $env:ProgramFiles 'rekordbox'
        $image = '^' + [regex]::Escape($install + '\') + 'rekordbox \d+(\.\d+)+\\rekordbox\.exe$'
        if ($p.ProcessName -ne 'rekordbox' -or $p.SessionId -ne $sid -or $p.Path -notmatch $image) {
            throw 'the rekordbox window process image is outside the rekordbox install folders'
        }
        if ([RbspWin]::GetForegroundWindow() -ne $h) { ($A::FromHandle($h)).SetFocus() }
        $t = [Diagnostics.Stopwatch]::StartNew()
        while ([RbspWin]::GetForegroundWindow() -ne $h -and $t.Elapsed.TotalSeconds -lt 3) { Start-Sleep -Milliseconds 100 }
        if ([RbspWin]::GetForegroundWindow() -ne $h) { throw "rekordbox's window $handle is not the foreground window" }
        return "front: ok; our app's window(s) minimized: $min; rekordbox $handle in front"
    } catch {
        return 'front: ' + ($_.Exception.Message -replace '[\r\n]+', ' ')
    }
}

function Assert-RbDesktopProcess($p) {
    $image = '^' + [regex]::Escape((Join-Path $env:ProgramFiles 'rekordbox') + '\') + 'rekordbox \d+(\.\d+)+\\rekordbox\.exe$'
    if ($p.ProcessName -ne 'rekordbox' -or $p.SessionId -ne $sid -or $p.Path -notmatch $image) {
        throw 'the rekordbox window process image is outside the rekordbox install folders'
    }
}

# rekordbox's verified main window in this session (rekordbox.exe under the rekordbox folder, title
# 'rekordbox'): @(handle, process id), or a throw
function Get-RbMain([switch]$FormatMenu, [switch]$StopOnMessageBox) {
    if ($RbVersion) { Assert-RbProcessVersion $sid (Get-Rb) }
    $windows = @(Get-DesktopWindows)   # first: it defines [RbspWin]
    if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable' }
    if ($StopOnMessageBox) { Assert-RbNoMessageBox }
    $rb = @($windows -cmatch '^rekordbox \| rekordbox \| \d+x\d+ \| 0x[0-9A-F]+$')
    if ($FormatMenu -and $rb.Count -gt 1) {
        Uia
        # JUCE's file menu can have the same window caption as the main window.
        $rb = @($rb | Where-Object {
            $candidate = [Windows.Automation.AutomationElement]::FromHandle((Get-FmtHandle $_)).Current
            $candidate.FrameworkId -ceq 'JUCE' -and $candidate.AutomationId -ceq 'rekordbox'
        })
    }
    if ($rb.Count -ne 1) { throw "expected one rekordbox main window, found $($rb.Count)" }
    $h = [IntPtr][Convert]::ToInt64((($rb[0] -split ' \| ')[-1]).Substring(2), 16)
    $processId = [RbspWin]::Pid($h)
    Assert-RbDesktopProcess (Get-Process -Id $processId)
    return @($h, $processId)
}

function Close-RbMain($p) {
    if ($p.MainWindowHandle -eq [IntPtr]::Zero) { Assert-RbNoMessageBox; return $false }
    $h, $processId = Get-RbMain -StopOnMessageBox
    if ($h -ne $p.MainWindowHandle -or $processId -ne $p.Id -or [RbspWin]::Pid($h) -ne $processId -or
        -not [RbspWin]::OrdinaryDesktop()) { throw 'the rekordbox close target changed' }
    # Post Escape to this window's queue ahead of the WM_CLOSE posted by CloseMainWindow.
    if (-not [RbspWin]::PostMessageW($h, 0x100, [IntPtr]0x1B, [IntPtr]0x00010001) -or
        -not [RbspWin]::PostMessageW($h, 0x101, [IntPtr]0x1B, [IntPtr]0xC0010001)) {
        throw 'could not post Escape to the rekordbox main window'
    }
    return $p.CloseMainWindow()
}

# rekordbox's Preferences window of process PID, when one is open: its handle, or $null
function Get-RbPrefs([int]$processId) {
    foreach ($line in @((Get-DesktopWindows) -cmatch '^rekordbox \| Preferences \| \d+x\d+ \| 0x[0-9A-F]+$')) {
        $h = [IntPtr][Convert]::ToInt64((($line -split ' \| ')[-1]).Substring(2), 16)
        if ([RbspWin]::Pid($h) -eq $processId) { return $h }
    }
    return $null
}
# rekordbox's Preferences opened (owner decision 2026-11-19: Auto Analysis off in the test account,
# through rekordbox's own Preferences UI). Its gear button has no UI Automation name: it is the one
# unnamed button on the top row between the plan button (the named button right of MIDI) and
# AudioCpuGraphButton, invoked through UI Automation; the window that opens must be rekordbox's
# "Preferences". Screenshots before and after.
function Open-RbPrefs {
    $details = @()
    try {
        $h, $processId = Get-RbMain
        $open = Get-RbPrefs $processId
        if ($open) { return "prefsopen: ok; already open 0x$($open.ToString('X'))" }
        Uia
        $A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
        $win = $A::FromHandle($h)
        $cond = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)
        $buttons = @($win.FindAll([Windows.Automation.TreeScope]::Descendants, $cond))
        $midi = @($buttons | Where-Object { $_.Current.Name -ceq 'MIDI' }); $cpu = @($buttons | Where-Object { $_.Current.Name -ceq 'AudioCpuGraphButton' })
        if ($midi.Count -ne 1 -or $cpu.Count -ne 1) { throw "expected one MIDI and one AudioCpuGraphButton button, found $($midi.Count) and $($cpu.Count)" }
        $m = $midi[0].Current.BoundingRectangle; $right = $cpu[0].Current.BoundingRectangle.Left; $mid = $m.Top + $m.Height / 2
        $row = @($buttons | Where-Object { $r = $_.Current.BoundingRectangle; [math]::Abs($r.Top + $r.Height / 2 - $mid) -le 10 -and $r.Left -gt $m.Right -and $r.Right -le $right })
        $plan = @($row | Where-Object { $_.Current.Name })
        if ($plan.Count -ne 1) { throw "expected one named button between MIDI and AudioCpuGraphButton, found $($plan.Count)" }
        $gear = @($row | Where-Object { -not $_.Current.Name -and $_.Current.BoundingRectangle.Left -ge $plan[0].Current.BoundingRectangle.Right })
        if ($gear.Count -ne 1) { throw "expected one unnamed button right of '$($plan[0].Current.Name.Trim())', found $($gear.Count)" }
        $details += "gear at $($gear[0].Current.BoundingRectangle)"
        Screen-Dpi
        $details += 'before ' + (Shot)
        $p = $null
        if ($gear[0].TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$p)) { $p.Invoke(); $details += 'invoked' }
        elseif ($gear[0].TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$p)) { $p.Toggle(); $details += 'toggled' }
        else { throw 'the gear button has no invoke or toggle pattern' }
        $t = [Diagnostics.Stopwatch]::StartNew()
        do { Start-Sleep -Milliseconds 300; $open = Get-RbPrefs $processId } while (-not $open -and $t.Elapsed.TotalSeconds -lt 10)
        Start-Sleep 1
        $details += 'after ' + (Shot)
        if (-not $open) { throw 'no Preferences window in 10 s' }
        return "prefsopen: ok; 0x$($open.ToString('X')); " + ($details -join '; ')
    } catch {
        return 'prefsopen: ' + ($_.Exception.Message -replace '[\r\n]+', ' ') + $(if ($details) { '; ' + ($details -join '; ') })
    }
}
# rekordbox's open Preferences window (handle, process id), verified in UI Automation: $null if none
function Get-RbPrefsEl {
    $h, $processId = Get-RbMain
    $open = Get-RbPrefs $processId
    if (-not $open) { return $null }
    Uia
    $win = [Windows.Automation.AutomationElement]::FromHandle($open)
    $c = $win.Current
    if ($c.Name -cne 'Preferences' -or $c.AutomationId -cne 'Preferences' -or $c.FrameworkId -cne 'JUCE' -or $c.ProcessId -ne $processId -or
        [int64]$c.NativeWindowHandle -ne $open.ToInt64()) { throw "the Preferences window did not match in UI Automation (name '$($c.Name)', id '$($c.AutomationId)')" }
    return $win
}
# the Preferences category button NAME (its id "NAME.<empty>.<empty>.Preferences") of window WIN
function Get-RbPrefsCategory($win, [string]$name) {
    $A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
    $cond = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)
    $b = @($win.FindAll([Windows.Automation.TreeScope]::Descendants, $cond) | Where-Object { $_.Current.Name -ceq $name -and $_.Current.AutomationId -ceq "$name.<empty>.<empty>.Preferences" })
    if ($b.Count -ne 1) { throw "expected one '$name' category in Preferences, found $($b.Count)" }
    return $b[0]
}
# the Preferences category NAME shown (its button pressed through UI Automation; read only otherwise)
function Select-RbPrefsTab([string]$name) {
    try {
        if ($name -notin 'View', 'Audio', 'Analysis', 'Controller', 'Keyboard', 'Advanced') { throw "not a Preferences category the kit opens: '$name'" }
        $win = Get-RbPrefsEl
        if (-not $win) { throw 'Preferences is not open' }
        $b = Get-RbPrefsCategory $win $name
        $p = $null
        if ($b.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$p)) {
            if ($p.Current.ToggleState -ne 'On') { $p.Toggle() }
        } elseif ($b.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$p)) { $p.Invoke() }
        else { throw "the '$name' category has no toggle or invoke pattern" }
        Start-Sleep 1
        $b = Get-RbPrefsCategory $win $name
        $p = $null
        if ($b.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$p) -and $p.Current.ToggleState -ne 'On') { throw "the '$name' category did not open" }
        return "prefstab: ok; $name"
    } catch {
        return 'prefstab: ' + ($_.Exception.Message -replace '[\r\n]+', ' ')
    }
}
# Auto Analysis off in rekordbox's Preferences (owner decision 2026-11-19, the test account only):
# Preferences opened by its gear, its Analysis category, the check box of the "Auto
# Analysis" section (named Enable, Disable once off; under that label, above the next) unticked through UI
# Automation, read back, and Preferences closed. Screenshots before and after; nothing else in
# Preferences is touched. rekordbox writes the setting to rekordbox3.settings (midi-auto reads it
# back after its quit).
function Set-RbAutoAnalysisOff {
    $details = @()
    try {
        $o = Open-RbPrefs; $details += $o
        if ($o -notmatch '^prefsopen: ok') { throw 'Preferences did not open' }
        $s = Select-RbPrefsTab 'Analysis'; $details += $s
        if ($s -notmatch '^prefstab: ok') { throw 'the Analysis category did not open' }
        $win = Get-RbPrefsEl
        $A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
        $all = @($win.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition))
        $label = @($all | Where-Object { $_.Current.ControlType -eq $CT::Text -and $_.Current.Name -ceq 'Auto Analysis' })
        if ($label.Count -ne 1) { throw "expected one 'Auto Analysis' label, found $($label.Count)" }
        $l = $label[0].Current.BoundingRectangle
        $next = @($all | Where-Object { $_.Current.ControlType -eq $CT::Text -and $_.Current.BoundingRectangle.Top -gt $l.Bottom } | ForEach-Object { $_.Current.BoundingRectangle.Top } | Sort-Object)
        $limit = if ($next.Count) { $next[0] } else { $l.Bottom + 60 }
        $box = @($all | Where-Object {
            $r = $_.Current.BoundingRectangle
            $_.Current.ControlType -eq $CT::CheckBox -and $_.Current.Name -cin 'Enable', 'Disable' -and $r.Top -ge $l.Bottom -and $r.Bottom -le $limit -and $r.Left -ge $l.Left
        })
        if ($box.Count -ne 1) { throw "expected one Enable/Disable check box in the Auto Analysis section, found $($box.Count)" }
        $p = $null
        if (-not $box[0].TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$p)) { throw 'the Auto Analysis check box has no toggle pattern' }
        $was = [string]$p.Current.ToggleState
        $details += "Auto Analysis was $was"
        if ($was -eq 'On') {
            $details += 'before ' + (Shot)
            $p.Toggle()
            Start-Sleep 1
            $now = [string]$p.Current.ToggleState
            $details += 'after ' + (Shot)
            if ($now -ne 'Off') { throw "the Auto Analysis check box reads $now after unticking" }
            $details += 'now Off'
        } elseif ($was -ne 'Off') { throw "the Auto Analysis check box reads $was" }
        $c = Close-RbPrefs; $details += $c
        if ($c -notmatch '^prefsclose: ok') { throw 'Preferences did not close' }
        return 'autoanalysis: ok; ' + ($details -join '; ')
    } catch {
        return 'autoanalysis: ' + ($_.Exception.Message -replace '[\r\n]+', ' ') + $(if ($details) { '; ' + ($details -join '; ') })
    }
}
# rekordbox's Preferences window closed through its UI Automation window pattern (its X)
function Close-RbPrefs {
    try {
        $h, $processId = Get-RbMain
        $open = Get-RbPrefs $processId
        if (-not $open) { return 'prefsclose: ok; not open' }
        Uia
        $wp = $null
        if (-not ([Windows.Automation.AutomationElement]::FromHandle($open)).TryGetCurrentPattern([Windows.Automation.WindowPattern]::Pattern, [ref]$wp)) { throw 'the Preferences window has no UI Automation window pattern' }
        $wp.Close()
        $t = [Diagnostics.Stopwatch]::StartNew()
        while ((Get-RbPrefs $processId) -and $t.Elapsed.TotalSeconds -lt 10) { Start-Sleep -Milliseconds 300 }
        if (Get-RbPrefs $processId) { throw 'the Preferences window did not close in 10 s' }
        return 'prefsclose: ok; closed'
    } catch {
        return 'prefsclose: ' + ($_.Exception.Message -replace '[\r\n]+', ' ')
    }
}

# rekordbox's progress window while it loads its STEMS engine (owner decision 2026-11-19): only
# read. rekordbox.exe under the rekordbox folder, the exact title, and in UI Automation a window of
# that process with no buttons of its own (a frame's caption buttons aside). The watch waits it
# out; nothing is ever clicked or closed. The answer describes it (its UIA name, id, framework).
function Test-RbStemsInit([string]$arg) {
    try {
        if ($arg -notmatch '^0x([0-9A-F]{1,16})$') { throw 'invalid window handle' }
        $h = [IntPtr][Convert]::ToInt64($Matches[1], 16)
        $title = 'Initializing STEMS Engine'
        $windows = @(Get-DesktopWindows)   # first: it defines [RbspWin]
        $windowPattern = '^rekordbox \| ' + [regex]::Escape($title) + ' \| \d+x\d+ \| ' + $arg + '$'
        if (-not @($windows -cmatch $windowPattern).Count) {
            # it usually lasts 5-10 s: one closed since the watch listed it is gone, not a mismatch
            if (-not [RbspWin]::IsWindow($h) -or -not [RbspWin]::IsWindowVisible($h)) { return "stemsinit: gone; $arg" }
            throw 'the progress window process name or title did not match'
        }
        $processId = [RbspWin]::Pid($h)
        $p = Get-Process -Id $processId
        $install = Join-Path $env:ProgramFiles 'rekordbox'
        $image = '^' + [regex]::Escape($install + '\') + 'rekordbox \d+(\.\d+)+\\rekordbox\.exe$'
        if ($p.ProcessName -ne 'rekordbox' -or $p.SessionId -ne $sid -or $p.Path -notmatch $image) {
            throw 'the window process image is outside the rekordbox install folders'
        }
        Uia
        $A = [Windows.Automation.AutomationElement]
        $CT = [Windows.Automation.ControlType]
        $win = $A::FromHandle($h)
        $c = $win.Current
        if ($c.ProcessId -ne $processId -or [int64]$c.NativeWindowHandle -ne $h.ToInt64()) { throw 'the UI Automation window did not match' }
        $cond = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)
        $buttons = @($win.FindAll([Windows.Automation.TreeScope]::Descendants, $cond) | Where-Object {
            [Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($_).Current.ControlType -ne $CT::TitleBar
        })
        if ($buttons.Count) { throw "the progress window has buttons: $(($buttons | ForEach-Object { "'" + $_.Current.Name + "'" }) -join ', ')" }
        return "stemsinit: ok; $arg pid $processId ($($p.Path)); UIA $($c.ControlType.ProgrammaticName) '$($c.Name)' id '$($c.AutomationId)' framework '$($c.FrameworkId)', no buttons"
    } catch {
        # closed while it was checked (UIA then answers for a window that no longer is): gone too
        if ($windows -and $h -and (-not [RbspWin]::IsWindow($h) -or -not [RbspWin]::IsWindowVisible($h))) { return "stemsinit: gone; $arg" }
        return 'stemsinit: ' + ($_.Exception.Message -replace '[\r\n]+', ' ')
    }
}

function Close-TestInputDirector([int]$sessionId) {
    if ($env:USERNAME -ne 'rbtest' -or $sessionId -le 0) { return }
    $closed = 0
    foreach ($p in @(Get-Process -Name InputDirector -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sessionId })) {
        # Hold the process handle across the ownership check and the wait so this targets the same instance.
        [void]$p.Handle
        if ($p.HasExited) { continue }
        $instance = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($p.Id) AND SessionId = $sessionId AND Name = 'InputDirector.exe'"
        if (-not $instance -and $p.HasExited) { continue }
        if (-not $instance) { throw 'could not verify the InputDirector process in the rbtest session' }
        $owner = Invoke-CimMethod -InputObject $instance -MethodName GetOwner
        if ($owner.ReturnValue -ne 0) { throw 'could not verify the InputDirector process owner' }
        if ($owner.User -ne 'rbtest' -or $owner.Domain -ne $env:COMPUTERNAME -or $p.HasExited) { continue }
        [void]$p.CloseMainWindow()
        if (-not $p.WaitForExit(10000) -and -not $p.HasExited) {
            throw 'InputDirector did not close normally in the rbtest session after 10 s'
        }
        $closed++
    }
    if ($closed) { "rbtest InputDirector: $closed closed normally" }
}

try {
    $sid = [Diagnostics.Process]::GetCurrentProcess().SessionId
    $lines = @(Get-Content -LiteralPath (Join-Path $B 'task-cmd.txt') -TotalCount 4)
    if ($lines.Count -lt 3 -or -not $lines[2]) { throw 'no request ID' }
    $cmd = $lines[0].Trim(); $arg = $lines[1].Trim(); $id = $lines[2]
    $RbVersion = if ($lines.Count -eq 4) { $lines[3] } else { '' }
    if ($RbVersion -and $RbVersion -notmatch '^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}$') { throw 'invalid rekordbox version' }
    switch ($cmd) {
        'ping' { $r = "ping: $env:USERNAME's session $sid" }
        'app' { $a = Get-App; $r = if ($a) { "app: running, its window up (pid $($a.Id))" } else { 'app: not running: not installed, or no window in 60 s' } }
        'start' {
            $rb = Get-Rb
            if (-not $rb) { $r = 'no rekordbox installed' }
            else {
                Assert-RbProcessVersion $sid $rb
                if (Test-RbRunning $sid) { $r = 'already running' }
                else {
                    $inputDirector = Close-TestInputDirector $sid
                    $watcherNotice = Defer-WatcherOffer
                    try { Start-Process -FilePath $rb.Exe -WorkingDirectory $rb.Dir }
                    catch {
                        if ($watcherNotice) { throw "$watcherNotice; rekordbox start failed: $($_.Exception.Message)" }
                        throw
                    }
                    $r = "started rekordbox $($rb.Version)$(if ($inputDirector) { "; $inputDirector" })$(if ($watcherNotice) { "; $watcherNotice" })"
                }
            }
        }
        'quit' {
            if ($RbVersion) { Assert-RbProcessVersion $sid (Get-Rb) }
            $ps = @(Get-RbProcess $sid)
            if ($ps.Count -gt 1) { throw 'expected one rekordbox process to close' }
            if (-not $ps.Count) { $r = 'not running' } else { $r = 'close asked: ' + (Close-RbMain $ps[0]) }
        }
        'quitdump' {
            if ($RbVersion) { Assert-RbProcessVersion $sid (Get-Rb) }
            $ps = @(Get-RbProcess $sid)
            if ($ps.Count -ne 1) { throw 'expected one rekordbox process for the close dump' }
            Assert-RbDesktopProcess $ps[0]
            . (Join-Path $Kit 'format-lib.ps1')
            . (Join-Path $Kit 'format-ui.ps1')
            $r = 'quitdump: OK ' + (Save-FmtCloseDump $ps[0].Id)
        }
        'fmtcancelpicker' {
            Assert-RbNoMessageBox -AllowPicker
            . (Join-Path $Kit 'format-ui.ps1')
            $r = 'fmtcancelpicker: ' + (Invoke-RbFormatTask $cmd $arg)
        }
        { $_ -in 'library-check', 'format-verify', 'format-reuse' } {
            try {
                if ($RbVersion) { Assert-RbProcessVersion $sid (Get-Rb) }
                . (Join-Path $Kit 'format-lib.ps1')
                . (Join-Path $Kit 'format-library.ps1')
                $corpus = Join-Path $Kit 'corpus'
                if ($cmd -eq 'library-check') {
                    $r = 'library-check: OK ' + (Save-FormatLibrary $corpus)
                } else {
                    $lock = Open-FormatLock
                    try {
                        if ($cmd -eq 'format-reuse') {
                            if (Test-RbRunning $sid) { throw 'formats: close rekordbox before reusing a library playlist' }
                            $entry = Restore-FormatLibraryImport $corpus
                            $r = if ($entry) { 'format-reuse: OK' } else { 'format-reuse: NONE' }
                        } else {
                            $null = Confirm-FormatLibraryImport $corpus
                            $r = 'format-verify: OK'
                        }
                    } finally { $lock.Dispose() }
                }
            } catch {
                $why = if ($_.Exception.Message -clike 'formats: *') { $_.Exception.Message } else { 'formats: library query failed' }
                $r = "${cmd}: STOP: $why"
            }
        }
        { $_ -in 'fmtimport', 'fmtuidump', 'fmtstep' } {
            try {
                . (Join-Path $Kit 'format-lib.ps1')
                . (Join-Path $Kit 'format-ui.ps1')
                if ($cmd -eq 'fmtuidump') {
                    $r = switch -CaseSensitive ($arg) {
                        '' { Save-FmtUiDump }
                        'explore' { Invoke-RbFormatTask $cmd $arg }
                        default { throw 'formats: invalid UI dump mode' }
                    }
                }
                elseif ($cmd -eq 'fmtstep') { $r = Invoke-RbFormatTask $cmd $arg }
                else {
                    $refusal = Invoke-RbFormatTask $cmd $arg
                    $r = "${cmd}: OK"
                    if ($refusal) { $r += "; refusal: $refusal" }
                }
            } catch {
                $why = if ($_.Exception.Message -clike 'formats: *') { $_.Exception.Message } else { 'formats: Windows automation failed' }
                $r = "${cmd}: STOP: $why (line $($_.InvocationInfo.ScriptLineNumber))"
            }
        }
        'shot' { $r = Shot }
        'winlist' {
            if ($RbVersion) { Assert-RbProcessVersion $sid (Get-Rb) }
            . (Join-Path $Kit 'format-lib.ps1')
            . (Join-Path $Kit 'format-ui.ps1')
            . (Join-Path $Kit 'winlist.ps1')
            $r = 'winlist: OK ' + (Get-RbWindowList $sid)
        }
        'dismiss-rb-error' {
            if (-not $RbVersion) { throw 'dismiss-rb-error requires a rekordbox version' }
            Assert-RbProcessVersion $sid (Get-Rb)
            . (Join-Path $Kit 'winlist.ps1')
            $r = 'dismiss-rb-error: OK ' + (Dismiss-RbError)
        }
        'uiatree' { $r = Save-UiaTree $arg }
        'loc' { . (Join-Path $PSScriptRoot 'loc-ui.ps1'); $r = Invoke-LocStep $arg }
        'prefsopen' { $r = Open-RbPrefs }
        'prefsclose' { $r = Close-RbPrefs }
        'prefstab' { $r = Select-RbPrefsTab $arg }
        'autoanalysisoff' { $r = Set-RbAutoAnalysisOff }
        'appexit' { $r = Exit-App }
        'appuninstall' { $r = Remove-OurApp }
        'appinstall' { $r = Install-OurApp $arg }
        'appmin' { $r = Hide-OurWindow }
        'appcheck' { $r = Test-OurWindow }
        'appstate' {
            # read only: our app in this session: running, minimized, and whether UI Automation sees
            # its tabs (none: the window drawn black, 2026-11-19)
            $procs = @(Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sid -and $_.MainWindowHandle -ne 0 })
            if (-not $procs.Count) { $r = 'appstate: not running' }
            else {
                Uia; [void](Get-DesktopWindows)
                $A = [Windows.Automation.AutomationElement]
                $cond = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procs[0].Id)),
                    (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::TabItem)))
                $tabs = @($A::RootElement.FindAll([Windows.Automation.TreeScope]::Descendants, $cond) | ForEach-Object { $_.Current.Name })
                $r = "appstate: running pid $($procs[0].Id); minimized $([RbspWin]::IsIconic($procs[0].MainWindowHandle)); tabs $($tabs.Count): $($tabs -join ', ')"
            }
        }
        'benchstart' {
            # one DirectML bench run (dml-run.ps1 NAME) started in this session, not waited for
            if ($arg -notin 'cpu', 'dml', 'stall') { $r = "benchstart: not a bench run: '$arg'" }
            elseif (-not (Test-Path -LiteralPath (Join-Path $Kit 'bench\rbsp-prepare.exe'))) { $r = 'benchstart: no bench\rbsp-prepare.exe in the kit (stage --helper)' }
            elseif (-not (Test-Path -LiteralPath (Join-Path $B "bench\spec-$arg.txt"))) { $r = "benchstart: no bench\spec-$arg.txt" }
            else {
                $p = Start-Process -FilePath powershell.exe -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', (Join-Path $Kit 'dml-run.ps1'), '-Name', $arg -WindowStyle Hidden -PassThru
                $r = "benchstart: $arg started (pid $($p.Id))"
            }
        }
        { $_ -in 'benchstop', 'benchstate' } {
            # only the kit's own helper, matched by its full path (a run past its time holds our
            # app's ORT, and the next cache install or uninstall then can't replace it: PC 2026-11-20)
            $exe = Join-Path $Kit 'bench\rbsp-prepare.exe'
            $ps = @(Get-Process -Name 'rbsp-prepare' -ErrorAction SilentlyContinue | Where-Object { $_.Path -and [string]::Equals($_.Path, $exe, [StringComparison]::OrdinalIgnoreCase) })
            if ($cmd -eq 'benchstate') { $r = "benchstate: $($ps.Count) running" }
            else {
                foreach ($p in $ps) { try { Stop-Process -InputObject $p -Force -ErrorAction Stop; [void]$p.WaitForExit(15000) } catch { } }
                $left = @($ps | Where-Object { -not $_.HasExited }).Count
                $r = "benchstop: $($ps.Count - $left) stopped, $left left"
            }
        }
        'closeupdate' { $r = Close-RbUpdate $arg }
        'closeinfo' { $r = Close-RbInfo $arg }
        'stemsinit' { $r = Test-RbStemsInit $arg }
        'closeagentnews' { $r = Close-RbAgentNews $arg }
        'front' { $r = Set-RbFront }
        'closetest' {
            $w = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle -eq $TestTitle })
            Close-Windows $w.MainWindowHandle
            $r = "closetest: $($w.Count) window(s) asked to close"
        }
        'testwindow' {
            # a plain window in this session, as a dialog would appear: the dialog watch must stop on it
            $ps = "Add-Type -AssemblyName System.Windows.Forms; `$f = New-Object Windows.Forms.Form; `$f.Text = '$TestTitle'; `$f.Width = 480; `$f.Height = 200; " +
                  "`$t = New-Object Windows.Forms.Timer; `$t.Interval = 300000; `$t.Add_Tick({ `$f.Close() }); `$t.Start(); [void]`$f.ShowDialog()"
            $p = Start-Process -FilePath powershell.exe -ArgumentList '-NoProfile', '-WindowStyle', 'Hidden', '-Command', $ps -PassThru
            $r = "testwindow: '$TestTitle' opened (pid $($p.Id))"
        }
        'uninstallcache' {
            # our own dialog only; rekordbox must be closed (midi-auto.ps1 checks)
            # the button is on the Manage tab; the confirmation's "Uninstall" is looked for in its
            # dialog only (named by its title for UI Automation)
            Uia; $app = Get-App; $CT = [Windows.Automation.ControlType]; $A = [Windows.Automation.AutomationElement]
            if (-not $app) { $r = 'uninstallcache: the app is not running and could not be started' }
            elseif (-not (Select-El (Find-El $app 'Manage' $CT::TabItem 30))) { $r = 'uninstallcache: no Manage tab in 30 s' }
            elseif (-not (Invoke-El (Find-El $app 'Uninstall Stems Cache' $CT::Button 30 -Enabled))) { $r = 'uninstallcache: no enabled "Uninstall Stems Cache" button in 30 s' }
            elseif (-not ($dlg = Find-El $app 'Uninstall Stems Cache?' $CT::Window 30)) { $r = 'uninstallcache: no "Uninstall Stems Cache?" dialog in 30 s' }
            else {
                $nc = New-Object Windows.Automation.AndCondition((New-Object Windows.Automation.PropertyCondition($A::NameProperty, 'Uninstall')), (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)))
                $e = $dlg.FindFirst([Windows.Automation.TreeScope]::Descendants, $nc)
                if ($e -and $e.Current.IsEnabled -and (Invoke-El $e)) { $r = 'uninstallcache: Manage > Uninstall Stems Cache > Uninstall clicked' }
                else { $r = 'uninstallcache: the dialog has no enabled "Uninstall" button' }
            }
        }
        'prepare' {
            Uia; $app = Get-App; $CT = [Windows.Automation.ControlType]
            if (-not $arg) { $r = 'prepare: no playlist name' }
            elseif (-not $app) { $r = 'prepare: the app is not running and could not be started' }
            elseif (-not (Select-El (Find-El $app 'Prepare' $CT::TabItem 30))) { $r = 'prepare: no Prepare tab in 30 s' }
            elseif (-not (Select-El (Find-Playlist $app $arg 60))) { $r = "prepare: no playlist '$arg' in the Prepare tab's list in 60 s" }
            else {
                $e = Find-El $app 'Prepare' $CT::Button 60 -Enabled
                if (-not $e) { $e = Find-El $app 'Prepare Again' $CT::Button 5 -Enabled }
                if (Invoke-El $e) { $r = "prepare: '$arg' selected, '$($e.Current.Name)' clicked" } else { $r = "prepare: no enabled Prepare button for '$arg' in 60 s" }
            }
        }
        'stopkeep' {
            # our app's own Stop Keeping…: the Status tab's row (a GridView DataItem) of playlist ARG selected, the button,
            # then its confirmation's "Stop Keeping"; ok once the row is gone (30 s)
            Uia; $app = Get-App; $CT = [Windows.Automation.ControlType]; $A = [Windows.Automation.AutomationElement]
            function Get-SetRow { $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)
                foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
                    foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::DataItem)))) {
                        if ($e.Current.Name.StartsWith($arg, [StringComparison]::Ordinal) -and -not $e.Current.IsOffscreen) { return $e } } }
                return $null }
            if (-not $arg) { $r = 'stopkeep: no playlist name' }
            elseif (-not $app) { $r = 'stopkeep: the app is not running and could not be started' }
            elseif (-not (Select-El (Find-El $app 'Status' $CT::TabItem 30))) { $r = 'stopkeep: no Status tab in 30 s' }
            else {
                $t = [Diagnostics.Stopwatch]::StartNew(); $row = $null
                while (-not $row -and $t.Elapsed.TotalSeconds -lt 30) { $row = Get-SetRow; if (-not $row) { Start-Sleep -Milliseconds 500 } }
                if (-not $row) {
                    # what the Status tab's list does show (names cut at 40), to tell an empty list from a name mismatch
                    $seen = @(); $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)
                    foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
                        foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::DataItem)))) {
                            $n = $e.Current.Name; $seen += "'$($n.Substring(0, [math]::Min(40, $n.Length)))'$(if ($e.Current.IsOffscreen) { ' (offscreen)' })" } }
                    $r = "stopkeep: no kept set '$arg' in the Status tab in 30 s; list items $($seen.Count): $(($seen | Select-Object -First 6) -join ', ')"
                }
                elseif (-not (Select-El $row)) { $r = 'stopkeep: its row could not be selected' }
                elseif (-not (Invoke-El (Find-El $app "Stop Keeping$([char]0x2026)" $CT::Button 10 -Enabled))) { $r = 'stopkeep: no enabled Stop Keeping… button' }
                elseif (-not (Invoke-El (Find-Named $app 'Stop Keeping' $CT::Button 20))) { $r = 'stopkeep: no Stop Keeping confirmation in 20 s' }
                else {
                    $t.Restart(); while ((Get-SetRow) -and $t.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 500 }
                    $r = if (Get-SetRow) { "stopkeep: '$arg' still listed 30 s after Stop Keeping" } else { "stopkeep: ok; '$arg' no longer kept" }
                }
            }
        }
        'statusdump' {
            # read only: the Status tab selected, then what UI Automation sees in it: the table, its
            # items and the Stop Keeping… button's state
            Uia; $app = Get-App; $CT = [Windows.Automation.ControlType]; $A = [Windows.Automation.AutomationElement]
            if (-not $app) { $r = 'statusdump: the app is not running' }
            elseif (-not (Select-El (Find-El $app 'Status' $CT::TabItem 30))) { $r = 'statusdump: no Status tab in 30 s' }
            else {
                Start-Sleep 5
                $elements = @(); $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)
                $lists = New-Object Windows.Automation.OrCondition((New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::List)), (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::DataGrid)))
                foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
                    foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, $lists)) {
                        $c = $e.Current; $kids = @($e.FindAll([Windows.Automation.TreeScope]::Children, [Windows.Automation.Condition]::TrueCondition))
                        $names = @($kids | Select-Object -First 4 | ForEach-Object { $n = $_.Current.Name; "'$($n.Substring(0, [math]::Min(40, $n.Length)))'" })
                        $elements += "$($c.ControlType.ProgrammaticName -replace 'ControlType\.', '') '$($c.Name)' offscreen=$($c.IsOffscreen) children=$($kids.Count) $($names -join ',')"
                    }
                    $b = Find-El $app "Stop Keeping$([char]0x2026)" $CT::Button 0
                    if ($b) { $elements += "Button 'Stop Keeping…' enabled=$($b.Current.IsEnabled) offscreen=$($b.Current.IsOffscreen)" }
                }
                $r = "statusdump: $($elements.Count) element(s): $($elements -join ' || ')"
            }
        }
        'prepbtn' {
            # one of the Prepare tab's run buttons (Pause, Resume, Stop) invoked, when enabled
            Uia; $app = Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Select-Object -First 1
            $r = if ($arg -notin 'Pause', 'Resume', 'Stop') { "prepbtn: not a run button: '$arg'" } elseif (-not $app) { 'prepbtn: the app is not running' }
                 elseif (Invoke-El (Find-El $app $arg ([Windows.Automation.ControlType]::Button) 10 -Enabled)) { "prepbtn: $arg clicked" } else { "prepbtn: no enabled $arg button" }
        }
        'prepstop' {
            Uia; $app = Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Select-Object -First 1
            $r = if (-not $app) { 'prepstop: the app is not running' } elseif (Invoke-El (Find-El $app 'Stop' ([Windows.Automation.ControlType]::Button) 10 -Enabled)) { 'prepstop: Stop clicked' } else { 'prepstop: no enabled Stop button' }
        }
        'prepstate' {
            Uia; $app = Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $app) { $r = 'prepstate: the app is not running' }
            else {
                $CT = [Windows.Automation.ControlType]; $A = [Windows.Automation.AutomationElement]
                $running = [bool](Find-El $app 'Stop' $CT::Button 0)
                $rows = New-Object System.Collections.Generic.List[string]
                $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)
                foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
                    foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::DataItem)))) { $rows.Add($e.Current.Name) }
                }
                $r = "prepstate: $(if ($running) { 'running' } else { 'idle' }) rows $($rows.Count): " + ($rows -join ' || ')
            }
        }
        { $_ -in 'windows', 'closeagent' } {
            $w = @(Get-DesktopWindows)
            if ($cmd -eq 'windows') {
                if ($arg -cin 'watch', 'watch-quit') {
                    Assert-RbNoMessageBox -AllowPicker:($arg -ceq 'watch-quit')
                    Assert-RbNoAnalysisSetting
                }
                $r = 'windows: ' + ($w -join ' || ')
            }
            else {
                $agents = @(foreach ($line in $w) {
                    if ($line -match '^rekordboxAgent \| rekordboxAgent \| \d+x\d+ \| 0x([0-9A-F]+)$') { [IntPtr][Convert]::ToInt64($Matches[1], 16) }
                })
                if ($agents.Count -eq 1) {
                    $f = Shot; Close-Windows $agents
                    $r = "closeagent: 1 window(s) asked to close; $f"
                } elseif ($agents.Count -gt 1) { $r = "closeagent: $($agents.Count) rekordboxAgent windows: none closed" }
                else { $r = 'closeagent: 0 window(s) asked to close' }
            }
        }
        { $_ -like 'm12-*' -or $_ -in 't2-model-on', 't2-model-off' } {
            try {
                if ($cmd -like 'm12-*') {
                    . (Join-Path $Kit 'm12.ps1')
                    $r = Invoke-M12 ($cmd.Substring(4)) $arg
                } else {
                    . (Join-Path $Kit 't2-round2.ps1')
                    $r = Set-T2Model ($cmd -eq 't2-model-on')
                }
            } catch {
                $r = "${cmd}: STOP: $($_.Exception.Message)"
                try { $r += '; ' + (Shot) } catch { $r += '; screenshot failed: ' + $_.Exception.Message }
            }
        }
        'hangcheck' { $r = Get-HangCheck }
        'logoff' { $r = 'logoff: signing out'; $after = { & shutdown.exe /l } }
        default { $r = "unknown command '$cmd'" }
    }
} catch { $r = 'error: ' + $_.Exception.Message }
$tmp = "$out.tmp"
Set-Content -LiteralPath $tmp -Value @($id, ((Get-Date).ToString('HH:mm:ss') + ' ' + $r))
[IO.File]::Move($tmp, $out)
if ($after) { & $after }
