# WC7: exercise the normal shell's real window and mutex in the console session.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OneCopyVm {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
}
'@
Start-AppQuiet
if (-not $script:app) { Done }
$app = $script:app
$exe = $app.Path
$window = $app.MainWindowHandle
[void][OneCopyVm]::ShowWindowAsync($window, 6)
Expect (Wait-Until { [OneCopyVm]::IsIconic($window) } 10) 'the first copy is minimized'
Assert-ShellDesktop
$second = Start-Process -FilePath $exe -PassThru
Expect ($second.WaitForExit(12000) -and $second.ExitCode -eq 0) 'HANDED: the second launch exited quietly'
Expect (Wait-Until { -not [OneCopyVm]::IsIconic($window) -and [OneCopyVm]::GetForegroundWindow() -eq $window } 15) 'HANDED: the original window was restored and brought forward'
Expect ((Get-AppWindow).Id -eq $app.Id) 'the registered-window callback ran in the first copy'
Close-ShellApp $app

# Hold the mutex with no app window. Keep an extra handle across termination so Windows
# preserves the abandoned mutex object for the next launch.
$mutex = 'Local\io.github.rbstemsplus.app'
$ready = Join-Path $ShellWork 'one-copy.ready'
$helper = Join-Path $ShellWork 'one-copy-owner.ps1'
Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
@'
param([string]$Name, [string]$Ready)
$ErrorActionPreference = 'Stop'
$created = $false
# the closed app's mutex object lives on while any handle to it is still open: wait for a fresh one
$deadline = (Get-Date).AddSeconds(10)
do {
    $m = [Threading.Mutex]::new($true, $Name, [ref]$created)
    if (-not $created) { $m.Dispose(); Start-Sleep -Milliseconds 250 }
} until ($created -or (Get-Date) -gt $deadline)
if (-not $created) { exit 1 }
[IO.File]::WriteAllText($Ready, 'owned')
Start-Sleep -Seconds 120
$m.ReleaseMutex()
$m.Dispose()
'@ | Set-Content -LiteralPath $helper -Encoding UTF8
$owner = Start-Process powershell.exe -ArgumentList "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$helper`" $mutex `"$ready`"" -WindowStyle Hidden -PassThru
$keep = $null
$busy = $null
try {
    Expect (Wait-Until { Test-Path -LiteralPath $ready } 15) 'a helper owns the named mutex without registering a window'
    $keep = [Threading.Mutex]::OpenExisting($mutex)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $busy = Start-Process -FilePath $exe -PassThru
    $title = Get-CoreText 'T_ALREADY_OPEN'
    Expect (Find-UiText $busy.Id ('^' + [regex]::Escape($title) + '$') 25) 'BUSY: the second launch reported Already open'
    Expect ($clock.Elapsed.TotalSeconds -ge 9.5 -and $clock.Elapsed.TotalSeconds -lt 25) "BUSY: Already open after $($clock.Elapsed.TotalSeconds) s (10 s mutex wait)"
    Use-ShellUi invoke --pid $busy.Id (Get-CoreText 'T_OK') --type Button --timeout 15
    Expect ($busy.WaitForExit(15000) -and $busy.ExitCode -eq 0) 'BUSY: the dialog closed and no second app started'
    Expect (-not $owner.HasExited) 'the helper still owns the mutex'
    Stop-Process -Id $owner.Id -Force
    Expect ($owner.WaitForExit(10000)) 'the owner died while holding the still-open mutex'
    Start-AppQuiet
    if (-not $script:app) { Done }
    Expect ($script:app.Id -ne $busy.Id) 'HELD: the next copy acquired the abandoned mutex'
    Close-ShellApp $script:app
} finally {
    if (-not $owner.HasExited) { Stop-Process -Id $owner.Id -Force }
    if ($busy -and -not $busy.HasExited) { Stop-Process -Id $busy.Id -Force }
    if ($keep) { $keep.Dispose() }
    Remove-Item -LiteralPath $ready, $helper -Force -ErrorAction SilentlyContinue
}
Done
