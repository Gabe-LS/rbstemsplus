# As SYSTEM: Defender's detections since the complete uninstall began (complete.ps1 wrote the
# time): none expected. A query that fails is a FAIL, never "nothing detected".
$f = Join-Path $AuditDir 'uninstall-since.txt'
if (-not (Test-Path -LiteralPath $f)) { Skip 'no uninstall-since.txt: the complete step did not start the uninstall' }
$since = [long]((Get-Content -LiteralPath $f -TotalCount 1).Trim())
$det = $null
try {
    $det = @(Get-MpThreatDetection -ErrorAction Stop | Where-Object { $_.InitialDetectionTime.ToUniversalTime().Ticks -ge $since })
} catch {
    Check $false "Defender's detections read (Get-MpThreatDetection: $($_.Exception.Message))"
    Done
}
foreach ($d in $det) { "Defender: $($d.ThreatID) $($d.Resources -join ', ')" }
Check ($det.Count -eq 0) "Defender detected nothing during the uninstall ($($det.Count))"
Done
