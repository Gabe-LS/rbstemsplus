# The repaint after simulated session and display changes (win/shell/Repaint.cs, TestHooks'
# session-churn): the TEST app sends its own window 3 cycles of a console disconnect, a connect and
# a display change, then one more minimized and restores it. After a short settle the same app
# still runs, UI Automation sees the Manage tab on screen and the window's client area isn't black.
Assert-ShellDesktop
$l = Get-Layout
Expect (Test-Path -LiteralPath $l.Stub) 'the shell fixture is installed'
Expect (-not (Get-AppWindow)) 'the app is closed before the simulated churn'
$app = $null
try {
    Start-ShellRepaint 'session-churn' 3
    $app = $script:ShellApp
    Expect (Wait-Until { (Read-ShellRepaint) -contains 'churn-sent' } 60) 'the app sent itself the simulated churn'
    Start-Sleep -Seconds 8
    Expect (-not $app.HasExited) "the same app (pid $($app.Id)) still runs"
    "repaint nudges: $(@(Read-ShellRepaint | Where-Object { $_ -eq 'repaint-nudge' }).Count)"
    Test-ShellPicture $app 'after the simulated churn'
    Close-ShellApp $app
} finally {
    if ($app -and -not $app.HasExited) { Stop-App }
}
Done
