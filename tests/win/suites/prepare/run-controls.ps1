# N1.10: Pause and Resume act on the same real helper. Stop writes its set.
# Exit and Update each exercise the separate confirm-and-stop path from a paused run.
$l = Get-Layout
$which = $VmRun.Step
$name = switch ($which) {
    'pause-stop' { 'RBSP Pause' }
    'paused-exit' { 'RBSP Exit' }
    'paused-update' { 'RBSP Update' }
    default { throw "unknown Prepare control case $which" }
}
$helper = $null
try {
    if ($which -eq 'paused-update') {
        $next = (Get-Content -LiteralPath (Need 'dist-test/new/payload.json' 'the newer update fixture') -Raw | ConvertFrom-Json).windows.app.version
        $installed = (Get-Content -LiteralPath $l.Current -Raw).Trim()
        Expect ($next -ne $installed) 'the candidate is a different app version'
        (Get-Item -LiteralPath (Join-Path $l.Support 'payload\payload.json')).LastWriteTime = (Get-Date).AddDays(-2)
    }
    Start-PrepareApp
    $app = $script:ShellApp
    $appPid = $app.Id
    if ($which -eq 'paused-update') {
        $notice = (Get-CoreText 'T_UPDATE_NOTICE').Replace('{version}', $next)
        Expect (Find-UiText $appPid ([regex]::Escape($notice)) 60) 'the app has offered the staged update'
    }
    $log = Join-Path $l.Support 'Logs\app.log'
    $offset = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
    Start-PreparePlaylist $name
    $helperPid = [int]$script:PrepareRun.ProcessId
    $set = Get-PrepareArgument $script:PrepareRun.CommandLine '--keep'
    if ($set -notmatch '^[A-Za-z0-9_-]{1,64}$') { throw 'invalid keep id from the helper' }
    $helper = Get-Process -Id $helperPid
    $null = $helper.Handle
    $keptFile = Join-Path $l.Support "kept\$set.kept"
    $manifest = Join-Path $l.Support "kept\$set.manifest"
    Expect ((-not (Test-Path -LiteralPath $keptFile)) -and (-not (Test-Path -LiteralPath $manifest))) 'the playlist has no previous kept files'
    Use-ShellUi invoke --pid $appPid (Get-CoreText 'T_BTN_PAUSE') --type Button --timeout 15
    Expect (Wait-Until { Test-PrepareSuspended $helper } 12) "Pause suspended all threads of keep helper $helperPid"
    $title = (Get-CoreText 'T_STOP_PREPARING_TITLE').Replace('{playlist}', $name)
    switch ($which) {
        'pause-stop' {
            Use-ShellUi invoke --pid $appPid (Get-CoreText 'T_BTN_RESUME') --type Button --timeout 10
            Expect (Wait-Until { -not $helper.HasExited -and -not (Test-PrepareSuspended $helper) } 10) 'Resume released the same running helper'
            Use-ShellUi invoke --pid $appPid (Get-CoreText 'T_BTN_STOP') --type Button --timeout 10
            Expect ($helper.WaitForExit(30000)) 'Stop let the helper finish'
            Check ($helper.ExitCode -eq 0) 'the stopped helper exited cleanly'
            Check ((Test-Path -LiteralPath $keptFile) -and (Test-Path -LiteralPath $manifest)) 'Stop wrote the kept file and manifest'
            $stopped = [regex]::Escape((Get-CoreText 'T_RUN_STOPPED_USER').Trim())
            Check (Wait-Until { (Read-BridgeLog $log $offset) -match "(?m)prepare: playlist ${set}: [^\r\n]*$stopped" } 15) 'the completion reports that the run was stopped'
            Check (-not $app.HasExited) 'Stop kept the app open'
        }
        'paused-exit' {
            Use-ShellUi menu --pid $appPid (Get-MenuText 'T_WIN_MENU_APP') (Get-MenuText 'T_MENU_QUIT') --timeout 15
            Use-ShellUi invoke --pid $appPid --title $title (Get-CoreText 'T_STOP_AND_QUIT') --type Button --timeout 15
            Expect ($app.WaitForExit(45000)) 'Stop and Exit closed the app'
            Check ($helper.HasExited -and -not (Get-PrepareHelper $appPid $helperPid)) 'no keep helper survived Exit'
        }
        'paused-update' {
            Use-ShellUi menu --pid $appPid (Get-MenuText 'T_WIN_MENU_APP') (Get-MenuText 'T_MENU_UPDATE') --timeout 15
            $confirm = (Get-CoreText 'T_UPDATE_TO').Replace('{version}', $next)
            Use-ShellUi invoke --pid $appPid --title $confirm (Get-CoreText 'T_UPDATE') --type Button --timeout 30
            Check (-not $helper.HasExited -and (Test-PrepareSuspended $helper)) 'the helper stayed paused through update confirmation'
            Use-ShellUi invoke --pid $appPid --title $title (Get-CoreText 'T_STOP_AND_UPDATE') --type Button --timeout 15
            Expect ($helper.WaitForExit(30000) -and $helper.ExitCode -eq 0) 'Stop and Update drained the paused helper'
            Expect (Wait-Until { (Get-Content -LiteralPath $l.Current -Raw -ErrorAction SilentlyContinue).Trim() -eq $next } 240) 'the update selected the candidate version'
            Check ($app.HasExited -and -not (Get-PrepareHelper $appPid $helperPid)) 'the old app and its paused helper are gone'
            Wait-ShellApp
            Check ($script:ShellApp.Path -ieq (Join-Path $l.Apps "$next\RB Stems Plus.exe")) 'the new version owns the app window'
        }
    }
} finally {
    Stop-App
    if ($helper) { $helper.Dispose() }
}
Done
