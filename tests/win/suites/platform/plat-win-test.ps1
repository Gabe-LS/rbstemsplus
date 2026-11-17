# MQ4 (models2-rr1's VM cases 1-2): common/tests/plat_win.c's plat-win-test, unelevated, on NTFS
# (every case: MT1's ENOTEMPTY, ESTALE, ELOOP on a junction, ENOTDIR, delete-pending) and on the
# exFAT VHDX in fat mode (the same without the junction), for each architecture here
$letterFile = 'C:\rbtest\vhd\exfat.letter'
$fat = if (Test-Path -LiteralPath $letterFile) { (Get-Content -LiteralPath $letterFile -TotalCount 1).Trim() } else { $null }
$ran = $false
foreach ($a in Get-TestArchs) {
    $exe = Get-TestExe $a 'plat-win-test.exe' @('checked.dll')
    if (-not $exe) { Missing "vm/$a/plat-win-test.exe isn't staged ($a)"; continue }
    $ran = $true
    $scratch = "C:\rbtest\pwt-$a"
    if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
    New-Item -ItemType Directory -Force $scratch | Out-Null
    $rc = Invoke-Exe $exe @($scratch)
    Check ($rc -eq 0) "plat-win-test ($a) on NTFS: exit 0 (got $rc)"
    if ($fat) {
        $fs = "${fat}:\pwt-$a"
        if (Test-Path $fs) { Remove-Item -Recurse -Force $fs }
        New-Item -ItemType Directory -Force $fs | Out-Null
        $rc = Invoke-Exe $exe @($fs, 'fat')
        Check ($rc -eq 0) "plat-win-test ($a) on exFAT, fat mode: exit 0 (got $rc)"
    } else { Missing "no exFAT VHDX (the vhd step's exfat.letter): fat mode ($a)" }
}
if (-not $ran) { Skip 'no plat-win-test.exe staged (common/tests/win.sh, then vm-run.sh stage)' }
Done
