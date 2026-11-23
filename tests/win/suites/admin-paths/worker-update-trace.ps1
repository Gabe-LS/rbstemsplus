$pair = Get-WorkerReleasePair
$old = $pair.Old.windows.app
$new = $pair.New.windows.app
$binOld = Get-BinWorker $old.admin_sha256
$binNew = Get-BinWorker $new.admin_sha256
switch -Wildcard ($VmRun.Step) {
    'trace-*' {
        Assert-WorkerPromptless
        Start-WorkerTrace
        Check (Test-Path -LiteralPath $WorkerTraceDir) 'the worker trace started'
        if ($VmRun.Step -eq 'trace-replacement') {
            $exe = Get-AppWorker
            if ((Get-Sha $exe) -ne $new.admin_sha256 -or (Test-Path -LiteralPath "$exe.rbsp-original")) {
                throw 'The fixture needs the genuine updated app-folder worker'
            }
            Move-Item -LiteralPath $exe -Destination "$exe.rbsp-original"
            Copy-Item -LiteralPath "$env:WINDIR\System32\whoami.exe" -Destination $exe
            Check ((Get-Sha $exe) -eq (Get-Sha "$env:WINDIR\System32\whoami.exe")) 'the replacement is a harmless executable with different bytes'
        }
    }
    'check-*' {
        try {
            $events = @(Stop-WorkerTrace)
            $starts = @(Get-WorkerStarts $events)
            Check (@($events | Where-Object Id -eq 1).Count -gt 0) 'the trace captured process starts'
            foreach ($s in $starts) { "worker: $($s.Image) $($s.Command)" }
            switch ($VmRun.Step) {
                'check-old-choice' {
                    Check (Test-WorkerRuns $starts @($binOld) @('install')) 'the fetched payload did not choose the worker'
                }
                'check-app-update' {
                    Check ($starts.Count -eq 0) 'the app update did not start any worker'
                }
                'check-replacement' {
                    Check (Test-WorkerRuns $starts @($binOld) @('install-worker')) 'only the old bin worker ran; the replacement never started'
                }
                'check-successor' {
                    Check (Test-WorkerRuns $starts @($binOld, $binNew) @('install-worker', 'install')) 'the old bin worker installed the new one, which then ran the action'
                }
                'check-uninstall-first' {
                    if (Test-WorkerRuns $starts @($binOld) @('uninstall')) {
                        Missing 'UP7 ruling pending: the old same-protocol bin worker uninstalled in one elevation; release.md describes two'
                    } else {
                        Check (Test-WorkerRuns $starts @($binOld, $binNew) @('install-worker', 'uninstall')) 'the two elevations, both from bin, as described by release.md'
                    }
                }
                default { throw "Unknown worker trace check: $($VmRun.Step)" }
            }
        } finally {
            if ($VmRun.Step -eq 'check-replacement') {
                $exe = Get-AppWorker
                if (Test-Path -LiteralPath "$exe.rbsp-original") {
                    Move-Item -LiteralPath "$exe.rbsp-original" -Destination $exe -Force
                    Check ((Get-Sha $exe) -eq $new.admin_sha256) 'the original app-folder worker was restored'
                }
            }
            Remove-Item -LiteralPath $WorkerTraceFile -ErrorAction SilentlyContinue
        }
    }
    default { throw "Unknown worker trace step: $($VmRun.Step)" }
}
Done
