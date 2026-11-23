$pair = Get-WorkerReleasePair
$l = Get-Layout
$old = $pair.Old.windows.app
$new = $pair.New.windows.app
$current = (Get-Content -LiteralPath $l.Current -Raw).Trim()
$binOld = Get-BinWorker $old.admin_sha256
$binNew = Get-BinWorker $new.admin_sha256
switch ($VmRun.Step) {
    'initial-worker' {
        if ($current -ne $old.version) { throw 'The old app must be installed first' }
        Invoke-WorkerCacheAction T_BTN_INSTALL_CACHE T_INSTALL '(?m)ok: install\r?$' -Settled { Test-CacheOn }
        Check ((Get-Sha $binOld) -eq $old.admin_sha256) 'the old app installed its own worker'
    }
    'old-app-new-payload' {
        if ($current -ne $old.version -or (Get-Sha $binOld) -ne $old.admin_sha256) { throw 'The old app and worker must still be installed' }
        $saved = Join-Path $l.Support 'payload\payload.json'
        (Get-Item -LiteralPath $saved).LastWriteTime = (Get-Date).AddDays(-2)
        Start-AppQuiet
        if (-not $script:app) { Done }
        Check (Wait-Until {
            (Get-Content -LiteralPath $saved -Raw | ConvertFrom-Json).windows.app.admin_sha256 -eq $new.admin_sha256
        } 120) 'the old app fetched the newer worker-changing payload'
        Check ((Get-Content -LiteralPath (Join-Path $l.Support 'installed-release\payload.json') -Raw | ConvertFrom-Json).windows.app.admin_sha256 -eq $old.admin_sha256) 'the installed-release pair still names the old worker'
        Invoke-WorkerCacheAction T_BTN_REINSTALL_CACHE T_REINSTALL '(?m)ok: install\r?$'
        Check ((Get-Sha $binOld) -eq $old.admin_sha256 -and -not (Test-Path -LiteralPath $binNew)) 'the old worker stayed, no new worker installed'
    }
    'update-app' {
        if ($current -ne $old.version -or (Get-Sha $binOld) -ne $old.admin_sha256 -or (Test-Path -LiteralPath $binNew)) {
            throw 'The old app and only its worker must be installed before updating'
        }
        (Get-Item -LiteralPath (Join-Path $l.Support 'payload\payload.json')).LastWriteTime = (Get-Date).AddDays(-2)
        Start-AppQuiet
        if (-not $script:app) { Done }
        $notice = (Get-CoreText 'T_UPDATE_NOTICE').Replace('{version}', $new.version)
        Check (Find-UiText $script:app.Id ([regex]::Escape($notice.Substring(0, [Math]::Min(40, $notice.Length)))) 120) "the app offers $($new.version)"
        $picked = (Invoke-Rbui menu --pid $script:app.Id (Get-MenuText 'T_WIN_MENU_APP') (Get-MenuText 'T_MENU_UPDATE') --timeout 30) -eq 0
        Check $picked 'the app menu Update command was invoked'
        if (-not $picked) { Done }
        Check (Invoke-WorkerButton $script:app.Id (Get-CoreText 'T_UPDATE')) 'the app update confirmation was pressed'
        Check (Wait-Until { (Get-Content -LiteralPath $l.Current -Raw).Trim() -eq $new.version } 240) 'the app switched to the newer version'
        Check ((Get-Sha (Get-AppWorker)) -eq $new.admin_sha256) 'the updated app ships the new worker'
        Check ((Get-Sha $binOld) -eq $old.admin_sha256 -and -not (Test-Path -LiteralPath $binNew)) 'the app update left the old bin worker in place'
        Stop-App
    }
    'refuse-replaced-worker' {
        if ($current -ne $new.version -or (Get-Sha (Get-AppWorker)) -ne (Get-Sha "$env:WINDIR\System32\whoami.exe")) {
            throw 'The app-worker replacement fixture is missing'
        }
        Invoke-WorkerCacheAction T_BTN_REINSTALL_CACHE T_REINSTALL "(?ms)the app's worker isn't the one payload\.json names: nothing changed.*FAILED: install \[exit 4\]" -DismissFailure
        Check ((Get-Sha $binOld) -eq $old.admin_sha256 -and -not (Test-Path -LiteralPath $binNew)) 'the replacement did not enter bin'
        Check ((Get-Sha $RbOrt) -eq $pair.Old.windows.bridge.sha256) 'the live DLL was unchanged'
    }
    'install-successor' {
        if ($current -ne $new.version -or (Get-Sha (Get-AppWorker)) -ne $new.admin_sha256) { throw 'The real new app worker is required' }
        Invoke-WorkerCacheAction T_BTN_REINSTALL_CACHE T_REINSTALL '(?m)ok: install\r?$'
        Check ((Get-Sha $binNew) -eq $new.admin_sha256) 'the new worker is installed in the shared bin'
        Check ((Get-Sha $RbOrt) -eq $pair.New.windows.bridge.sha256) 'the released bridge remains live'
    }
    'uninstall-first' {
        if ($current -ne $new.version -or (Get-Sha $binOld) -ne $old.admin_sha256 -or (Test-Path -LiteralPath $binNew)) {
            throw 'Uninstall must be the first elevated action by the updated app'
        }
        Invoke-WorkerCacheAction T_BTN_UNINSTALL_CACHE T_UNINSTALL '(?m)ok: uninstall\r?$' -Settled { -not (Test-CacheOn) }
        Check ((Get-Sha $RbOrt) -eq (Read-WorkerFacts).PioneerSha) "Pioneer's DLL is back"
    }
    default { throw "Unknown worker update action: $($VmRun.Step)" }
}
Done
