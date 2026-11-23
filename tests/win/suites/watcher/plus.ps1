# N4 (the watcher's registration, LAUNCH_AGENT_LOAD and _UNLOAD; review NS10, NS18): Demucs v4
# through the app's buttons, the watcher's records and process after each, by the step's name:
#   plus-install    the one-liner's Run value removed and its watcher stopped, then Install Demucs
#                   v4: app.log "watcher: installed ..." and "watcher: started", the Run value (the
#                   stub with --watcher) there, a new watcher runs
#   plus-uninstall  an enabled StartupApproved value set, then Uninstall Demucs v4 (nothing chosen
#                   is left): the watcher stopped, the Run value and the StartupApproved value gone
#   plus-again      Install Demucs v4 again: the watcher comes back (app.log: "watcher: installed
#                   ..." and "watcher: started"), the Run value there, a watcher runs
#   startup-off     Startup apps says off (StartupApproved's first byte 03, as Task Manager writes
#                   it), the watcher stopped, then Reinstall Demucs v4: app.log "watcher: turned off
#                   in Startup apps: not started", no watcher starts, the Run value and the off
#                   choice kept
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
if (@(Get-Rekordbox).Count) { Check (Close-Rekordbox 200) 'rekordbox, open in the snapshot, closed first' }
$runWant = "`"$($l.Stub)`" --watcher"
$s = $VmRun.Step
Show-WatcherState 'before'
$beforeIds = @(Get-Watchers | ForEach-Object { $_.Id })
if ($s -eq 'plus-install') {
    # the one-liner registered and started a watcher already: removed first (the Run value, the
    # watcher stopped), so this install's own LAUNCH_AGENT_LOAD writes and starts one (review VR2)
    Stop-App
    Remove-ItemProperty -LiteralPath $WtRunKey -Name $WtName -ErrorAction SilentlyContinue
    foreach ($w in Get-Watchers) { Stop-Process -Id $w.Id -Force -ErrorAction SilentlyContinue }
    Check (Wait-Until { @(Get-Watchers).Count -eq 0 } 20) 'the one-liner''s watcher stopped'
    Check ($null -eq (Get-RunValue)) 'and its Run value removed'
}
if ($s -eq 'plus-uninstall') {
    # an enabled StartupApproved value (as Task Manager writes one), which the unload must remove too
    $on = [byte[]](2, 0, 0, 0) + [BitConverter]::GetBytes([DateTime]::UtcNow.ToFileTimeUtc())
    if (-not (Test-Path -LiteralPath $WtApprovedKey)) { New-Item -Path $WtApprovedKey -Force | Out-Null }
    Set-ItemProperty -LiteralPath $WtApprovedKey -Name $WtName -Value $on -Type Binary
    $a = Get-ApprovedValue
    Check ($null -ne $a -and $a[0] -eq 2) 'a StartupApproved value (enabled) is there before'
}
if ($s -eq 'startup-off') {
    $off = [byte[]](3, 0, 0, 0) + [BitConverter]::GetBytes([DateTime]::UtcNow.ToFileTimeUtc())
    if (-not (Test-Path -LiteralPath $WtApprovedKey)) { New-Item -Path $WtApprovedKey -Force | Out-Null }
    Set-ItemProperty -LiteralPath $WtApprovedKey -Name $WtName -Value $off -Type Binary
    Stop-App
    foreach ($w in Get-Watchers) { Stop-Process -Id $w.Id -Force -ErrorAction SilentlyContinue }
    Check (Wait-Until { @(Get-Watchers).Count -eq 0 } 20) 'the watcher stopped (as at a logon with Startup apps off)'
}
$from = Get-AppLogSize
Open-App
if (-not $script:app) { Done }
$script:PlusOk = $false
switch ($s) {
    'plus-install' { Invoke-PlusAction 'T_BTN_INSTALL_PLUS' 'T_INSTALL' 'T_BTN_UNINSTALL_PLUS' }
    'plus-uninstall' { Invoke-PlusAction 'T_BTN_UNINSTALL_PLUS' 'T_UNINSTALL' 'T_BTN_INSTALL_PLUS' }
    'plus-again' { Invoke-PlusAction 'T_BTN_INSTALL_PLUS' 'T_INSTALL' 'T_BTN_UNINSTALL_PLUS' }
    'startup-off' { Invoke-PlusAction 'T_BTN_REINSTALL_PLUS' 'T_REINSTALL' 'T_BTN_UNINSTALL_PLUS' }
    default { Check $false "no case for a step named $s" }
}
$ok = $script:PlusOk
$said = Read-AppLog $from
($said -split "`r?`n" | Where-Object { $_ -match 'watcher' }) | ForEach-Object { "app.log: $_" }
if ($ok) {
    switch ($s) {
        'plus-install' {
            Check ($said -match "watcher: installed \(the Run value starts this app's watcher\)") 'app.log: watcher: installed'
            Check ($said -match 'watcher: started(?! FAILED)') 'app.log: watcher: started'
            Check ((Get-RunValue) -eq $runWant) "the Run value: $runWant"
            Check (Wait-Until { @(Get-Watchers | Where-Object { $beforeIds -notcontains $_.Id }).Count -ge 1 } 30) 'a new watcher runs (not the one-liner''s)'
        }
        'plus-uninstall' {
            Check (Wait-Until { @(Get-Watchers).Count -eq 0 } 30) 'no watcher runs'
            Check ($null -eq (Get-RunValue)) 'the Run value is gone'
            Check ($null -eq (Get-ApprovedValue)) 'its StartupApproved value is gone'
        }
        'plus-again' {
            Check ($said -match "watcher: installed \(the Run value starts this app's watcher\)") 'app.log: watcher: installed'
            Check ($said -match 'watcher: started(?! FAILED)') 'app.log: watcher: started'
            Check ((Get-RunValue) -eq $runWant) "the Run value is back: $runWant"
            Check (Wait-Until { @(Get-Watchers).Count -ge 1 } 30) 'the watcher came back: it runs'
        }
        'startup-off' {
            Check ($said -match 'watcher: turned off in Startup apps: not started') 'app.log: watcher: turned off in Startup apps: not started'
            Check ($said -notmatch 'watcher: started') 'app.log: no watcher: started'
            Start-Sleep -Seconds 5
            Check (@(Get-Watchers).Count -eq 0) 'no watcher runs'
            Check ((Get-RunValue) -eq $runWant) 'the Run value kept'
            $a = Get-ApprovedValue
            Check ($null -ne $a -and ($a[0] -band 1)) 'Startup apps still says off'
        }
    }
}
Show-WatcherState 'after'
Stop-App
Done
