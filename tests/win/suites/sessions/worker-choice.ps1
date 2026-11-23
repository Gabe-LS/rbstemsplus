$pair = Get-WorkerReleasePair
$old = $pair.Old.windows.app
$new = $pair.New.windows.app
$binNew = Get-BinWorker $new.admin_sha256
switch ($VmRun.Step) {
    'trace-worker-choice' {
        if ((Get-Sha $binNew) -ne $new.admin_sha256 -or (Test-Path -LiteralPath (Get-BinWorker $old.admin_sha256))) {
            throw 'B''s newer worker must be the available worker'
        }
        Assert-WorkerPromptless
        Start-WorkerTrace
        Check (Test-Path -LiteralPath $WorkerTraceDir) 'A''s worker trace started'
    }
    'worker-choice' {
        if ((Get-Content -LiteralPath (Get-Layout).Current -Raw).Trim() -ne $old.version -or -not (Test-CacheOn)) {
            throw 'A must still be running the old app with the cache selected'
        }
        Invoke-WorkerCacheAction T_BTN_UNINSTALL_CACHE T_UNINSTALL '(?m)ok: uninstall\r?$' -Settled { -not (Test-CacheOn) }
        Check ((Get-Sha $RbOrt) -eq (Read-WorkerFacts).PioneerSha) "Pioneer's DLL is back"
    }
    'check-worker-choice' {
        try {
            $starts = @(Get-WorkerStarts @(Stop-WorkerTrace))
            Check (Test-WorkerRuns $starts @($binNew) @('uninstall')) 'the older app chose B''s newer matching bin worker'
        } finally { Remove-Item -LiteralPath $WorkerTraceFile -ErrorAction SilentlyContinue }
    }
    default { throw "Unknown worker-choice step: $($VmRun.Step)" }
}
Done
