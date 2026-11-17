# T3c's ruling, VM cases 1, 2, 4, 5: location-win-test unelevated on a fresh C:\rbtest\loc-<arch>
# passes (its long-path case included); a second run on the same folder fails on its mklink and
# "no folder" steps; with the share mapped as a drive (RBSP_TEST_NET_DRIVE) the network case runs
# ("not-local", free bytes > 0). x64 under Prism on ARM64 as well. Case 3 (the VHDX volumes) is
# volumes.suite's.
$net = Get-PSDrive -PSProvider FileSystem | Where-Object { $_.DisplayRoot -like '\\Mac\*' } | Select-Object -First 1
# the user's own drive mappings belong to its desktop logon, not to this scheduled task's: map
# \\Mac\win here for the step (removed at its end) when none is seen
$mapped = $null
if (-not $net) {
    $free = @('R', 'P', 'O', 'N', 'M') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
    if ($free -and (Invoke-Exe "$env:WINDIR\System32\net.exe" @('use', "${free}:", '\\Mac\win', '/persistent:no')) -eq 0) {
        $mapped = $free
        $net = [pscustomobject]@{ Name = $free }
        "mapped ${free}: to \\Mac\win for this step"
    } else { "net use couldn't map \\Mac\win: $script:ExeOut" }
}
$ran = $false
foreach ($a in Get-TestArchs) {
    $exe = Get-TestExe $a 'location-win-test.exe'
    if (-not $exe) { Missing "vm/$a/location-win-test.exe isn't staged ($a)"; continue }
    $ran = $true
    $scratch = "C:\rbtest\loc-$a"
    if (Test-Path $scratch) { cmd /c rmdir /s /q $scratch }
    $envs = @{}
    if ($net) { $envs['RBSP_TEST_NET_DRIVE'] = "$($net.Name):" }
    $rc = Invoke-Exe $exe @($scratch) $envs
    Check ($rc -eq 0) "location-win-test ($a) on a fresh folder: exit 0 (got $rc)"
    if ($net) { Check ($script:ExeOut -notmatch 'no RBSP_TEST_NET_DRIVE') "the network drive $($net.Name): case ran" }
    else { Missing "no drive mapped to \\Mac in this session: the network case ($a)" }
    $rc = Invoke-Exe $exe @($scratch)
    Check ($rc -ne 0) "a second run on the same folder fails (its mklink and no-folder steps) (got $rc)"
}
if ($mapped) { [void](Invoke-Exe "$env:WINDIR\System32\net.exe" @('use', "${mapped}:", '/delete', '/y')) }
if (-not $ran) { Skip 'no location-win-test.exe staged (vm-run.sh stage --build)' }
Done
