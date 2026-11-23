$fixture = Get-Content -Raw -LiteralPath $GuardFixture | ConvertFrom-Json
try {
    $events = @(Stop-WorkerTrace)
    $starts = @(Get-WorkerStarts $events)
    foreach ($s in $starts) { "  worker start: pid $($s.Pid) $($s.Image) | $($s.Command)" }
    foreach ($s in @($events | Where-Object { $_.Id -eq 1 -and $_.Image -match '\\consent\.exe$' })) { "  consent.exe start: pid $($s.Pid) | $($s.Command)" }
    Check (@($events | Where-Object Id -eq 1).Count -gt 0) 'the trace captured process starts'
    if ($fixture.Case -eq 'plant-dlls') {
        Check (Test-WorkerRuns $starts @($fixture.App) @('install')) 'the shipped app-folder worker ran the initial install once'
        if ($starts.Count -eq 1) {
            $plants = @($GuardDllNames | ForEach-Object { Join-Path (Split-Path -Parent $fixture.App) $_ })
            Check (Test-WorkerDllSafe $events $starts[0].Pid $plants) "the worker mapped system images and none of the planted DLLs ($($plants -join ', '))"
        }
    } elseif ($fixture.Case -eq 'trace-retry') {
        Check (Test-WorkerRuns $starts @($fixture.Worker) @('uninstall')) 'the bin worker ran the retry once'
    } else {
        Check ($starts.Count -eq 0) 'the worker guard prevented every worker launch'
    }
} finally {
    if ($fixture.Sddl) {
        $acl = Get-Acl -LiteralPath $fixture.Target
        $acl.SetSecurityDescriptorSddlForm($fixture.Sddl)
        Set-Acl -LiteralPath $fixture.Target -AclObject $acl
    }
    if ($fixture.Holder) {
        Set-Content -LiteralPath $GuardStop -Value 'stop'
        Check (Wait-Until { -not (Get-Process -Id $fixture.Holder -ErrorAction SilentlyContinue) } 20) 'the fixture released its handle'
        Check ((Get-Sha $fixture.Worker) -eq (Read-TestPayload).windows.app.admin_sha256) 'the released worker is readable and unchanged'
    }
    if ($fixture.Case -eq 'plant-dlls') {
        foreach ($name in $GuardDllNames) {
            Remove-Item -LiteralPath (Join-Path (Split-Path -Parent $fixture.App) $name) -ErrorAction Stop
        }
    }
    Remove-Item -LiteralPath $WorkerTraceFile, $GuardFixture -ErrorAction SilentlyContinue
}
Done
