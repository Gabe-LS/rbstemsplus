# As SYSTEM: the release the TEST app sees, dist-test/current/, made a copy of dist-test/<name>/
# (the step's name after "switch-", before an optional shell-ui sweep suffix);
# "switch-clean" deletes current/ (the VM's own files)
$name = ($VmRun.Step -replace '^switch-', '') -replace '-(?:native|x64)-[0-9]+$', ''
# the clock on today's time line, and kept there (report/plant-sys.ps1's way): a reverted snapshot
# starts on its own date, Parallels' sync moves it to the host's, and Windows' time service can
# move it back a minute later (2026-11-22: to "clean Windows"'s 2026-11-09, before the TLS test
# certificate's start, so the app's HTTPS update check failed with HTTP 0). The time service is
# stopped for this reverted VM and the clock set from the run's server (a run has a served URL; the
# host test of the switch has none)
if ($name -ne 'clean' -and $VmRun.Url) {
    Stop-Service -Name w32time -Force -ErrorAction SilentlyContinue
    Set-Service -Name w32time -StartupType Disabled -ErrorAction SilentlyContinue
    Check ((Get-Service -Name w32time).Status -eq 'Stopped') "Windows' time service is stopped"
    try {
        $resp = Invoke-WebRequest -UseBasicParsing -Uri $VmRun.Url -TimeoutSec 20
        $now = [DateTime]::Parse([string]$resp.Headers['Date'], [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AdjustToUniversal)
        "the VM's clock: $((Get-Date).ToUniversalTime().ToString('u')), the server's: $($now.ToString('u'))"
        if ([Math]::Abs(((Get-Date).ToUniversalTime() - $now).TotalSeconds) -gt 60) { Set-Date -Date $now.ToLocalTime() | Out-Null }
        Check ([Math]::Abs(((Get-Date).ToUniversalTime() - $now).TotalSeconds) -lt 120) "the VM's clock agrees with the run's server ($((Get-Date).ToString('o')))"
    } catch { Check $false "the VM's clock couldn't be set from the run's server: $($_.Exception.Message)" }
}
$cur = Share 'dist-test/current'
if (Test-Path -LiteralPath $cur) { Remove-Item -LiteralPath $cur -Recurse -Force }
if ($name -eq 'clean') { Check (-not (Test-Path -LiteralPath $cur)) 'dist-test/current removed'; Done }
$src = Need "dist-test/$name/payload.json" "the release $name"
Copy-Item -LiteralPath (Split-Path $src) -Destination $cur -Recurse -Force
Check (Test-Path -LiteralPath (Join-Path $cur 'payload.json')) "dist-test/current is $name"
"payload_version: $((Get-Content -Raw -LiteralPath (Join-Path $cur 'payload.json') | ConvertFrom-Json).payload_version)"
Done
