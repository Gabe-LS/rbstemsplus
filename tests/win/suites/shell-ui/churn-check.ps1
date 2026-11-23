# The owner's PC black window (2026-11-19): the look after churn.ps1's session moves. Run three
# times by the suite: churn-mid after the moves with the window minimized (restored here as RB-KIT
# restores it, then left restored), churn-check after the moves with it restored, unlock-check
# after the healthy lock and unlock (the app closed after). Each: the same app still runs and,
# after a short settle (the repaint round's nudges end 6 s after a change), UI Automation sees the
# Manage tab on screen and the screen shows the window's client area not black.
Assert-ShellDesktop
Expect (Test-Path -LiteralPath $ShellChurnPid) 'churn-start left the app running'
$appId = [int](Get-Content -LiteralPath $ShellChurnPid)
$app = Get-Process -Id $appId -ErrorAction SilentlyContinue
Expect ($null -ne $app) "the same app (pid $appId) lived through the session moves"
$last = $VmRun.Step -eq 'unlock-check'
try {
    Add-ShellShot
    $minimized = [RbspShellShot]::IsIconic($app.MainWindowHandle)
    if ($VmRun.Step -eq 'churn-mid') { Check $minimized 'the window stayed minimized through the moves' }
    else { Check (-not $minimized) 'the window stayed restored through the moves' }
    Restore-ShellWindow $app 'after the session moves'
    Start-Sleep -Seconds 8
    Test-ShellPicture $app 'after the session moves'
    "repaint nudges so far: $(@(Read-ShellRepaint | Where-Object { $_ -eq 'repaint-nudge' }).Count)"
    if ($last) { Close-ShellApp $app }
} finally {
    if ($last -and $app -and -not $app.HasExited) { Stop-App }
}
Done
