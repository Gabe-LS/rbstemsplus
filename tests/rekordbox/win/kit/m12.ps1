# M12's interactive rounds. Dot-sourced only by the least-privilege session task.
# The shell operations use the same rbui commands as tests/win/suites/shell-ui.
$M12Name = 'RB Stems Plus'
$M12Apps = Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus'
$M12Support = Join-Path $env:LOCALAPPDATA 'rbstemsplus'
$M12Stub = Join-Path $M12Apps "$M12Name.exe"
$M12Entry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus'
$M12RunOnce = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
$M12Shortcut = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\$M12Name.lnk"
$M12State = Join-Path $B 'm12-kept.txt'
$M12Session = [Diagnostics.Process]::GetCurrentProcess().SessionId

function Assert-M12([bool]$ok, [string]$what) { if (-not $ok) { throw $what } }
function M12-Desktop {
    $windows = @(Get-DesktopWindows)
    Assert-M12 ([RbspWin]::OrdinaryDesktop()) 'ordinary input desktop unavailable'
    $bad = @($windows -match '^(smartscreen|consent|WerFault(?:Secure)?|LogonUI) \||\| (?:Windows protected your PC|Open File - Security Warning) \|')
    Assert-M12 (-not $bad.Count) ("unexpected security or crash window: " + ($bad -join '; '))
    $rb = @($windows -match '^rekordbox(?:Agent)? \|')
    Assert-M12 (-not $rb.Count) ("rekordbox must be closed: " + ($rb -join '; '))
    $consent = Get-Process -Name consent -ErrorAction SilentlyContinue | Where-Object SessionId -eq $M12Session
    Assert-M12 (-not $consent) 'Windows consent is pending'
}
function Wait-M12([int]$seconds, [scriptblock]$condition, [string]$failure) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        M12-Desktop
        if (& $condition) { return }
        Start-Sleep -Milliseconds 400
    } while ($clock.Elapsed.TotalSeconds -lt $seconds)
    throw $failure
}
function M12-Ui([Parameter(ValueFromRemainingArguments = $true)][string[]]$argv) {
    M12-Desktop
    $exe = Join-Path $Kit 'uia\rbui.exe'
    $list = @(& $exe @argv)
    $rc = $LASTEXITCODE
    M12-Desktop
    Assert-M12 ($rc -eq 0) "rbui $($argv -join ' ') exit ${rc}: $($list -join '; ')"
    return ($list -join "`n")
}
function M12-Apps([string]$exe) {
    Get-Process -Name $M12Name -ErrorAction SilentlyContinue | Where-Object {
        $_.SessionId -eq $M12Session -and $_.Path -ieq $exe -and $_.MainWindowHandle -ne [IntPtr]::Zero
    }
}
function M12-VersionExe([string]$version) { Join-Path $M12Apps "$version\$M12Name.exe" }
function M12-Current { (Get-Content -LiteralPath (Join-Path $M12Apps 'current') -Raw).Trim() }
function M12-Sha($file) { (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
function M12-Payload([string]$slot) {
    $file = Join-Path $Kit "m12\$slot.json"
    $p = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    foreach ($v in $p.payload_version, $p.windows.app.version) {
        Assert-M12 ($v -match '^\d{1,9}\.\d{1,9}\.\d{1,9}$') "invalid $slot TEST release version"
    }
    return @{ File = $file; Release = $p.payload_version; App = $p.windows.app.version }
}
function M12-Installed($p) {
    $installed = Join-Path $M12Support 'installed-release\payload.json'
    if (-not (Test-Path -LiteralPath $installed) -or -not (Test-Path -LiteralPath (Join-Path $M12Apps 'current'))) { return $false }
    $entry = Get-ItemProperty -LiteralPath $M12Entry -ErrorAction SilentlyContinue
    return ((M12-Current) -eq $p.App -and $entry.DisplayVersion -eq $p.Release -and (M12-Sha $installed) -eq (M12-Sha $p.File))
}
function M12-Dialog($app, [string]$title, [string]$button, [int]$seconds) {
    $A = [Windows.Automation.AutomationElement]
    $CT = [Windows.Automation.ControlType]
    Wait-M12 $seconds {
        $dialog = Find-El $app $title $CT::Window 0
        if (-not $dialog) { return $false }
        $cond = New-Object Windows.Automation.AndCondition(
            (New-Object Windows.Automation.PropertyCondition($A::NameProperty, $button)),
            (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $CT::Button)))
        $buttons = @($dialog.FindAll([Windows.Automation.TreeScope]::Descendants, $cond) |
            Where-Object { $_.Current.IsEnabled -and -not $_.Current.IsOffscreen })
        if ($buttons.Count -eq 0) { return $false }
        Assert-M12 ($buttons.Count -eq 1) "ambiguous '$button' in '$title'"
        M12-Desktop
        Assert-M12 (Invoke-El $buttons[0]) "could not invoke '$button' in '$title'"
        return $true
    } "expected app dialog '$title' with '$button'"
}
function M12-App($p) {
    Wait-M12 55 { @(M12-Apps (M12-VersionExe $p.App)).Count -eq 1 } "expected one app window from $($p.App)"
    return (M12-Apps (M12-VersionExe $p.App))
}
function M12-Close($app) {
    M12-Desktop
    # WPF also uses the app name as the Win32 title of a dialog. Check the UIA name.
    $app.Refresh()
    $win = [Windows.Automation.AutomationElement]::FromHandle($app.MainWindowHandle)
    Assert-M12 ($win.Current.Name -eq $M12Name -and $win.Current.IsEnabled) 'app main window disabled or a dialog is open'
    Assert-M12 ($app.CloseMainWindow()) 'the app refused a close request'
    Wait-M12 20 { $app.HasExited } 'the app did not close'
}
function M12-DownloadHash([string]$url) {
    $web = New-Object Net.WebClient
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return [BitConverter]::ToString($sha.ComputeHash($web.DownloadData($url))).Replace('-', '') }
        finally { $sha.Dispose() }
    } finally { $web.Dispose() }
}
function M12-Served($p, [string]$tls) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Assert-M12 ((M12-DownloadHash "https://$tls/latest/download/payload.json") -eq (M12-Sha $p.File)) "TEST server is not serving $($p.Release)"
}
function M12-Install([string]$slot, [string]$http, [string]$tls) {
    $p = M12-Payload $slot
    M12-Served $p $tls
    # The same TEST bootstrap as the one-liner, byte-checked before use. It verifies its release
    # itself. Keep the child separate so its environment and its exit do not change this task.
    Assert-M12 (-not (Test-Path -LiteralPath $M12Apps)) 'installation requires an empty per-user app folder'
    $want = (Get-Content -LiteralPath (Join-Path $Kit "m12\$slot-bootstrap.sha") -Raw).Trim()
    Assert-M12 ($want -match '^[0-9a-f]{64}$') 'bootstrap hash missing'
    $cmd = @"
`$ErrorActionPreference = 'Stop'
`$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
`$env:RBSTEMSPLUS_BASE_URL = 'http://$http/'
Remove-Item Env:RBSTEMSPLUS_RESTORE -ErrorAction SilentlyContinue
`$web = New-Object Net.WebClient
try {
    `$bytes = `$web.DownloadData('https://$tls/latest/download/bootstrap.ps1')
    `$sha = [Security.Cryptography.SHA256]::Create()
    try { `$got = [BitConverter]::ToString(`$sha.ComputeHash(`$bytes)).Replace('-', '') } finally { `$sha.Dispose() }
    if (`$got -ne '$want') { throw 'TEST bootstrap hash differs' }
    Invoke-Expression ([Text.Encoding]::ASCII.GetString(`$bytes))
} finally { `$web.Dispose() }
"@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
    $log = Join-Path $B "m12-$slot-bootstrap.log"
    $err = "$log.err"
    $proc = Start-Process powershell.exe -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError $err -PassThru
    Wait-M12 155 { $proc.HasExited } 'TEST bootstrap did not finish'
    Assert-M12 ($proc.ExitCode -eq 0 -and (M12-Installed $p)) "TEST bootstrap did not install $($p.Release); see $log and $err"
    $app = M12-App $p
    if (-not (Test-Path -LiteralPath (Join-Path $M12Support 'welcomed'))) {
        M12-Dialog $app 'Welcome to RB Stems Plus' 'Continue' 20
        Wait-M12 10 { Test-Path -LiteralPath (Join-Path $M12Support 'welcomed') } 'welcome was not recorded'
    }
    M12-Close $app
    "installed TEST $($p.Release) (app $($p.App))"
}
# A clean bootstrap installs the worker but leaves the bridge download to the app. Stage
# only the bridge named by the verified installed manifest, before the existing admin step.
function M12-WorkerSource([string]$tls) {
    $p = M12-Payload new
    Assert-M12 (M12-Installed $p) 'the worker source requires the new installed TEST payload'
    M12-Served $p $tls
    $bridge = (Get-Content -LiteralPath $p.File -Raw | ConvertFrom-Json).windows.bridge
    Assert-M12 ($bridge.file -match '^[A-Za-z0-9][A-Za-z0-9._-]*\.dll$' -and $bridge.sha256 -match '^[a-f0-9]{64}$') 'the TEST payload does not identify a bridge'
    $dir = Join-Path $M12Support 'rbkit-worker-source'
    [void][IO.Directory]::CreateDirectory($dir)
    $dest = Join-Path $dir $bridge.file
    $tmp = "$dest.part"
    $web = New-Object Net.WebClient
    try {
        $web.DownloadFile("https://$tls/download/v$($p.Release)/$($bridge.file)", $tmp)
        M12-Desktop
        Assert-M12 ((M12-Sha $tmp) -eq $bridge.sha256) 'the downloaded bridge hash differs from the installed TEST manifest'
        Move-Item -LiteralPath $tmp -Destination $dest -Force
    } finally {
        $web.Dispose()
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
    }
    "worker source verified for TEST payload $($p.Release)"
}
function M12-Update([string]$tls) {
    $old = M12-Payload old; $new = M12-Payload new
    Assert-M12 (([version]$new.Release -gt [version]$old.Release) -and ([version]$new.App -ge [version]$old.App)) 'TEST successor must advance the payload and cannot downgrade the app'
    Assert-M12 (M12-Installed $old) 'the old TEST release is no longer installed'
    M12-Served $new $tls
    $dirs = @(Get-ChildItem -LiteralPath $M12Apps -Directory | Sort-Object Name).Name -join ','
    (Get-Item -LiteralPath (Join-Path $M12Support 'payload\payload.json')).LastWriteTime = (Get-Date).AddDays(-2)
    [void](Start-Process -FilePath $M12Stub)
    $app = M12-App $old
    if ($old.App -eq $new.App) {
        Wait-M12 150 { M12-Installed $new } 'carried payload did not reach the installed record and Apps display version'
        Assert-M12 ((@(Get-ChildItem -LiteralPath $M12Apps -Directory | Sort-Object Name).Name -join ',') -ceq $dirs) 'carried payload changed the version directories'
        $ui = M12-Ui read --pid $app.Id --timeout 5
        Assert-M12 ($ui -notmatch 'RB Stems Plus [0-9.]+ is available: choose Update') 'carried payload offered an app update'
        M12-Close $app
        return "carried $($old.Release) -> $($new.Release); same app $($new.App)"
    }
    $notice = "RB Stems Plus $($new.App) is available: choose Update"
    Wait-M12 45 { (M12-Ui read --pid $app.Id --timeout 5).Contains($notice) } "update notice missing for $($new.App)"
    [void](M12-Ui menu --pid $app.Id $M12Name "Update RB Stems Plus$([char]0x2026)" --timeout 15)
    M12-Dialog $app "Update RB Stems Plus to $($new.App)" 'Update' 15
    Wait-M12 125 { (M12-Installed $new) -and $app.HasExited } 'app did not swap to the new TEST release'
    $updated = M12-App $new
    $arch = if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
    $native = Join-Path $M12Apps "$($new.App)\stub\$arch\$M12Name.exe"
    $prefix = (Join-Path $M12Apps $new.App) + '\'
    Wait-M12 15 {
        $watchers = @(Get-Process -Name 'rbsp-watch-*' -ErrorAction SilentlyContinue | Where-Object {
            $_.SessionId -eq $M12Session -and $_.Path -and $_.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
        })
        $watchers.Count -eq 1 -and -not (Test-Path -LiteralPath (Join-Path $M12Apps $old.App))
    } 'old app folder stayed or the new watcher did not start'
    Assert-M12 ((M12-Sha $M12Stub) -eq (M12-Sha $native) -and (Test-Path -LiteralPath $M12Shortcut)) 'updated native stub or Start shortcut missing'
    foreach ($file in 'pending', 'result') {
        Assert-M12 (-not (Test-Path -LiteralPath (Join-Path $M12Support "update\$file"))) "update left $file"
    }
    M12-Close $updated
    "app updated $($old.App) -> $($new.App), installed release $($new.Release)"
}
function M12-Group {
    Start-Sleep -Milliseconds 750
    $out = M12-Ui taskbar count $M12Name --timeout 10
    Assert-M12 ($out -match '(?m)taskbar: count=1 name=RB Stems Plus\r?$') "expected one taskbar group: $out"
}
function M12-Pin {
    $p = M12-Payload new
    Assert-M12 (M12-Installed $p) 'pin test needs the new TEST release'
    [void](Start-Process -FilePath $M12Stub)
    $app = M12-App $p
    M12-Group
    [void](M12-Ui taskbar pin $M12Name --timeout 20)
    M12-Close $app
    $folder = Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar'
    $shell = New-Object -ComObject WScript.Shell
    Wait-M12 15 {
        @(Get-ChildItem -LiteralPath $folder -Filter '*.lnk' | Where-Object {
            $shell.CreateShortcut($_.FullName).TargetPath -ieq $M12Stub
        }).Count -eq 1
    } 'persisted pin does not uniquely target the stable stub'
    [void](M12-Ui taskbar click $M12Name --timeout 15)
    $launched = M12-App $p
    Assert-M12 ($launched.Id -ne $app.Id) 'pin did not start a new app process'
    M12-Group
    M12-Close $launched
    [void](M12-Ui taskbar unpin $M12Name --timeout 20)
    $out = M12-Ui taskbar count $M12Name --timeout 10
    Assert-M12 ($out -match '(?m)taskbar: count=0 name=RB Stems Plus\r?$') "unpin did not clear the group: $out"
    'persisted shell pin launched the stable stub; one taskbar group'
}
function M12-AppTree {
    @(Get-ChildItem -LiteralPath $M12Apps -Recurse -Force | Sort-Object FullName | ForEach-Object {
        "$($_.FullName) $($_.Length) $($_.LastWriteTimeUtc.Ticks)"
    }) -join "`n"
}
function M12-TempCopies {
    Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'rbsp-uninstall-*' |
        Where-Object Name -match '^rbsp-uninstall-[a-f0-9]{16}$'
}
function M12-RunOnceValue([string]$name) {
    (Get-ItemProperty -LiteralPath $M12RunOnce -Name $name -ErrorAction SilentlyContinue).$name
}
function M12-Settings([bool]$holdOpen) {
    $p = M12-Payload new
    Assert-M12 (M12-Installed $p) 'Settings test needs the new TEST release'
    Assert-M12 ((Get-OrtState) -eq 'pioneer') 'Stems Cache must be removed by the admin worker before Settings uninstall'
    $open = @(M12-Apps (M12-VersionExe $p.App))
    Assert-M12 ($open.Count -le 1) 'more than one installed app process before Settings uninstall'
    if ($open.Count) { M12-Close $open[0] }
    $before = @(M12-TempCopies).FullName
    if ($holdOpen) {
        Assert-M12 (-not (Test-Path -LiteralPath $M12State)) 'a previous kept round is still pending'
        $holdPath = Join-Path $M12Apps 'rbkit-held.exe'
        Assert-M12 (-not (Test-Path -LiteralPath $holdPath)) 'the hold fixture already exists'
        Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\ping.exe') -Destination $holdPath
        $hold = Start-Process -FilePath $holdPath -ArgumentList '-t', '127.0.0.1' -WindowStyle Hidden -PassThru
        $tree = M12-AppTree
    } else {
        $state = @(Get-Content -LiteralPath $M12State)
        Assert-M12 ($state.Count -eq 1 -and [IO.Path]::IsPathRooted($state[0])) 'no valid kept-round path'
        $dir = $state[0]
        $name = Split-Path -Leaf $dir
        Assert-M12 ($name -match '^rbsp-uninstall-[a-f0-9]{16}$') 'no valid kept-round folder name'
        $value = 'RB Stems Plus cleanup ' + $name.Substring(15)
        Wait-M12 25 { -not (Test-Path -LiteralPath $dir) -and -not (M12-RunOnceValue $value) } 'RunOnce did not remove the kept round temp copy at logon'
    }
    [void](M12-Ui settings-uninstall $M12Name --timeout 55)
    $app = M12-App $p
    M12-Dialog $app 'Uninstall RB Stems Plus?' 'Uninstall' 20
    M12-Dialog $app 'RB Stems Plus is uninstalled' 'Done' 20
    if (-not $holdOpen) {
        Wait-M12 45 {
            -not (Test-Path -LiteralPath $M12Apps) -and -not (Test-Path -LiteralPath $M12Entry) -and
                -not (Test-Path -LiteralPath $M12Shortcut) -and -not @(M12-TempCopies | Where-Object FullName -notin $before).Count
        } 'Settings retry left an app folder, Installed apps entry, Start shortcut or temp copy'
        Remove-Item -LiteralPath $M12State
        return 'RunOnce removed the kept copy at logon; Settings retry removed the app'
    }
    $kept = @{}
    $message = "RB Stems Plus couldn't be fully uninstalled because part of it is still running, maybe in another session of your Windows account. Restart Windows, then uninstall it again from Apps & features."
    Wait-M12 90 {
        $copies = @(M12-TempCopies | Where-Object FullName -notin $before)
        Assert-M12 ($copies.Count -le 1) 'more than one after-exit copy appeared'
        if ($copies.Count -ne 1) { return $false }
        $kept.Copy = $copies[0]
        $kept.Process = @(M12-Apps (Join-Path $kept.Copy.FullName "$M12Name.exe"))
        return ($kept.Process.Count -eq 1 -and (Get-Date) -ge $kept.Process[0].StartTime.AddSeconds(59))
    } 'kept round did not wait for the held image and show its after-exit window'
    $copy = $kept.Copy
    [void](M12-Ui zorder $kept.Process[0].Id --above-settings $message)
    Assert-M12 (-not $hold.HasExited -and (M12-AppTree) -ceq $tree -and (M12-Installed $p) -and (Test-Path -LiteralPath $M12Shortcut)) 'kept round did not preserve the held image, app tree and Installed apps entry'
    $watchers = @(Get-Process -Name 'rbsp-watch-*' -ErrorAction SilentlyContinue | Where-Object SessionId -eq $M12Session)
    Assert-M12 (-not $watchers.Count) 'the per-user watcher is still running in the kept round'
    $value = 'RB Stems Plus cleanup ' + $copy.Name.Substring(15)
    $dirs = @($copy.FullName, (New-Object -ComObject Scripting.FileSystemObject).GetFolder($copy.FullName).ShortPath)
    $cmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $commands = @($dirs | ForEach-Object { "`"$cmd`" /d /q /s /c `"rd /s /q `"$_`"`"" })
    Assert-M12 ((M12-RunOnceValue $value) -in $commands) 'kept temp folder has no matching RunOnce cleanup'
    Set-Content -LiteralPath $M12State -Value $copy.FullName
    "kept dialog in front of Settings after 59 s; RunOnce registered for $($copy.Name); $(Shot)"
}
function M12-SmartScreen([string]$name) {
    Assert-M12 ($name -match '^[A-Za-z0-9][A-Za-z0-9._-]*\.(exe|msi)$') 'expected an installer basename'
    $path = Join-Path $env:USERPROFILE "Downloads\$name"
    $want = (Get-Content -LiteralPath (Join-Path $Kit 'm12\installer.sha') -Raw).Trim()
    Assert-M12 ($want -match '^[0-9a-f]{64}$' -and (M12-Sha $path) -eq $want) 'downloaded fixture hash differs'
    Assert-M12 ((Get-Content -LiteralPath $path -Stream Zone.Identifier -Raw) -match '(?m)^ZoneId=[34]\r?$') 'fixture is missing the browser download zone'
    $before = @(Get-DesktopWindows)
    # ShellExecute in a child on this desktop: the launch itself may wait on the prompt.
    $literal = "'" + $path.Replace("'", "''") + "'"
    $cmd = "`$ErrorActionPreference = 'Stop'; Start-Process -FilePath $literal"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
    $launcher = Start-Process powershell.exe -ArgumentList '-NoProfile', '-EncodedCommand', $encoded -WindowStyle Hidden -PassThru
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        $new = @(Get-DesktopWindows | Where-Object { $_ -notin $before })
        $smart = @($new -match '^smartscreen \|')
        if ($smart.Count) { throw ('SmartScreen observed; ' + ($smart -join '; ')) }
        Assert-M12 (-not $new.Count) ("SmartScreen not seen before a new window: " + ($new -join '; '))
        Assert-M12 ([RbspWin]::OrdinaryDesktop()) 'SmartScreen not identified; input desktop changed'
        Start-Sleep -Milliseconds 250
    } while ($clock.Elapsed.TotalSeconds -lt 25)
    Assert-M12 ($launcher.HasExited -and $launcher.ExitCode -eq 0) 'SmartScreen not seen; ShellExecute did not return successfully'
    "SmartScreen not seen during 25 s after ShellExecute; $(Shot)"
}
function Invoke-M12([string]$step, [string]$arg) {
    M12-Desktop
    if ($step -ne 'smartscreen') { Uia }
    if ($step -in 'install-old', 'install-new', 'update', 'worker-source') {
        Assert-M12 ($arg -match '^([A-Za-z0-9.-]+:[0-9]+) ([A-Za-z0-9.-]+:[0-9]+)$') 'missing TEST server addresses'
        $http = $Matches[1]; $tls = $Matches[2]
    }
    $result = switch ($step) {
        'install-old' { M12-Install old $http $tls }
        'install-new' { M12-Install new $http $tls }
        'update' { M12-Update $tls }
        'pin' { M12-Pin }
        'worker-source' { M12-WorkerSource $tls }
        'settings-kept' { M12-Settings $true }
        'settings-retry' { M12-Settings $false }
        'smartscreen' { M12-SmartScreen $arg }
        default { throw "unknown M12 step '$step'" }
    }
    "m12-${step}: OK $result"
}
