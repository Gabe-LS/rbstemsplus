# N4 and N1.9: two completed Prepare runs while the app is minimised.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class PrepareNoticeWindow {
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
}
"@
$work = Join-Path $env:TEMP ('rbsp-prepare-notices-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$capture = $null
try {
    foreach ($lap in 'A', 'B') {
        Start-PrepareApp
        $app = $script:ShellApp
        $name = "RBSP Notice $lap"
        $title = (Get-CoreText 'T_RUN_PREPARED').Replace('{playlist}', $name)
        $capture = Start-RbuiCapture $work "notice-$lap" "notice `"$title`" --pid $($app.Id) --timeout 110"
        Expect (Wait-Until { Test-Path -LiteralPath $capture.Ready } 10) "notice $lap observer armed"
        Start-PreparePlaylist $name
        $helperPid = [int]$script:PrepareRun.ProcessId
        $hwnd = $app.MainWindowHandle
        Expect ([PrepareNoticeWindow]::ShowWindowAsync($hwnd, 6)) 'requested minimising the app while its helper was running'
        Expect (Wait-Until { [PrepareNoticeWindow]::IsIconic($hwnd) } 5) 'the app is minimised before completion'
        Expect ($null -ne (Get-PrepareHelper $app.Id $helperPid)) 'the same keep helper is still running in the background'
        Expect ($capture.Process.WaitForExit((Get-WaitSecs 115) * 1000)) "notice $lap observer finished"
        Check ($capture.Process.ExitCode -eq 0) "notice $lap has one banner and its tray icon disappears"
        Check ([PrepareNoticeWindow]::IsIconic($hwnd)) 'the app remained in the background'
        Stop-RbuiCapture $capture
        $capture = $null
        Stop-App
    }
} finally {
    Stop-RbuiCapture $capture
    Stop-App
    Remove-Item -LiteralPath $work -Recurse -Force
}
Done
