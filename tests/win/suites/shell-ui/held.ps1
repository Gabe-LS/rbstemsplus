# UN4 and K1/K3 hold a second image from this apps folder while Settings uninstalls.
# UN4 ends the hold and reads AX_LEFT. K1 leaves its hold and its dialog open for K4's restart.
$l = Get-Layout
$case = $VmRun.Step
Expect (Test-Path -LiteralPath $l.Stub) 'the fixture is installed'
Expect (Wait-Until { @(Get-AppProcesses | Where-Object ProcessName -like 'rbsp-watch-*').Count -eq 1 } 30) 'the installed watcher runs before uninstall'
$heldPath = Join-Path $l.Apps 'w2-held.exe'
Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\ping.exe') -Destination $heldPath
$hold = Start-Process -FilePath $heldPath -ArgumentList '-t', '127.0.0.1' -WindowStyle Hidden -PassThru
$keepOpen = $false
try {
    Expect (Wait-Until { @(Get-AppProcesses | Where-Object Id -eq $hold.Id).Count -eq 1 } 15) "the second image runs from $heldPath"
    $tree = Get-AppTree
    Start-AfterExitWatch
    Start-SettingsUninstall
    $ax = Wait-AfterExit 30
    Expect ($null -ne $ax) 'the uninstall started its after-exit copy'
    $dir = Split-Path -Parent $ax.Path
    $message = Get-CoreText 'T_WIN_UNINSTALL_KEPT'
    Check ($message -notmatch '(?i)w2-held|rbsp-watch|\.exe\b|[a-z]:\\') 'the kept message does not name an executable or a path'
    $first = ($message -split '(?<=\.) ')[0]
    $shown = Find-UiText $ax.Id ([regex]::Escape($first)) 110
    Assert-ShellDesktop
    Expect $shown 'the kept dialog appeared after the wait for the second image'
    $elapsed = [DateTime]::Now - $ax.StartTime
    Check ($elapsed.TotalSeconds -ge 59) "the after-exit waited at least 59 s (of its 60 s limit); observed $([int]$elapsed.TotalSeconds) s"
    Use-ShellUi zorder $ax.Id --above-settings $message
    Check (-not $hold.HasExited) 'the second image still runs'
    Check ((Get-AppTree) -ceq $tree) 'the installed files and current are intact'
    Check (Test-Path -LiteralPath $ShellEntry) 'the Installed apps entry is still present'
    Check (Test-Path -LiteralPath $l.Shortcut) 'the Start menu shortcut is still present'
    Check (@(Get-AppProcesses | Where-Object ProcessName -like 'rbsp-watch-*').Count -eq 0) 'the watcher stopped'
    Check ($null -eq (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'RB Stems Plus Watcher' -ErrorAction SilentlyContinue)) 'the watcher Run value was removed'
    Expect (Test-ShellCleanupValue $dir) "RunOnce was already written while the dialog is open ($dir)"
    if ($case -eq 'k1-kept') {
        if ($script:VmFails -gt 0) { Done }
        Save-ShellCleanup 'k4' $dir
        $keepOpen = $true
    } else {
        Stop-Process -Id $hold.Id
        Use-ShellUi invoke --pid $ax.Id 'OK' --type Button --timeout 15
        Check ($ax.WaitForExit(30000) -and $ax.ExitCode -eq 3) 'UN4 returned AX_LEFT after the kept dialog closed'
        Check (Wait-Until { -not (Test-Path -LiteralPath $dir) } 90) 'UN4 removed its own after-exit temp folder'
    }
} finally {
    if (-not $keepOpen -and -not $hold.HasExited) { Stop-Process -Id $hold.Id -ErrorAction SilentlyContinue }
}
Done
