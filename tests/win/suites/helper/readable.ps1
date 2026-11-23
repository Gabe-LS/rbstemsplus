# T14: readable.c on Windows (rbsp-win-readable-test.exe): a file, a missing one, a folder, a drive
# that isn't there, the "another drive" rule, the decode's reads bounded; then, for information,
# any OneDrive placeholder the user has (UNC tracks: unc.ps1)
$x = Get-LocalExe 'rbsp-win-readable-test.exe'
$scratch = Join-Path $x.Dir 'scratch'
New-Item -ItemType Directory -Path $scratch | Out-Null
$rc = Invoke-Exe $x.Exe @($scratch)
Check ($rc -eq 0) "readable's cases on the system drive (exit $rc)"
"information: OneDrive placeholders in the user's profile (RECALL_ON_DATA_ACCESS, RECALL_ON_OPEN or OFFLINE):"
$od = @($env:OneDrive, $env:OneDriveConsumer, $env:OneDriveCommercial) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique
$ph = @()
foreach ($d in $od) {
    $ph += @(Get-ChildItem -LiteralPath $d -Recurse -File -Force -ErrorAction SilentlyContinue |
        Where-Object { ([int64]$_.Attributes -band (0x400000 -bor 0x40000 -bor 0x1000)) -ne 0 } | Select-Object -First 3)
}
if ($ph.Count -eq 0) { "  none (OneDrive folders: $(if ($od) { $od -join ', ' } else { 'none' })): the placeholder case isn't testable on this VM" }
else {
    foreach ($f in $ph) {
        $rc = Invoke-Exe $x.Exe @('--path', $f.FullName, '2')
        Check ($rc -eq 0) "a placeholder: both reads ok or asking (exit $rc)"
    }
}
Done
