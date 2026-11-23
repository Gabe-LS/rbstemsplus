# T3c's ruling, VM cases 1, 2, 4, 5: location-win-test unelevated on a fresh C:\rbtest\loc-<arch>
# passes (its long-path case included); a second run on the same folder fails on its mklink and
# "no folder" steps; with the share mapped as a drive (RBSP_TEST_NET_DRIVE) the network case runs
# ("not-local", free bytes > 0). x64 under Prism on ARM64 as well. Case 3 (the VHDX volumes) is
# volumes.suite's.
$net = Get-PSDrive -PSProvider FileSystem | Where-Object { $_.DisplayRoot -like '\\Mac\*' } | Select-Object -First 1
# the user's own drive mappings belong to its desktop logon, not to this scheduled task's: map
# \\Mac\win here for the step (removed at its end) when none is seen; when that can't be mapped
# from this session, the VM's own share \\127.0.0.1\rbnet (the smb-share step's) instead, a
# network drive all the same (DRIVE_REMOTE)
$mapped = $null
if (-not $net) {
    $free = @('R', 'P', 'O', 'N', 'M') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
    foreach ($unc in '\\Mac\win', '\\127.0.0.1\rbnet') {
        if (-not $free) { break }
        if ((Invoke-Exe "$env:WINDIR\System32\net.exe" @('use', "${free}:", $unc, '/persistent:no')) -eq 0) {
            $mapped = $free
            $net = [pscustomobject]@{ Name = $free }
            "mapped ${free}: to $unc for this step"
            break
        }
        "net use couldn't map ${unc}: $script:ExeOut"
    }
}
$ran = $false
foreach ($a in Get-TestArchs) {
    $exe = Get-TestExe $a 'location-win-test.exe'
    if (-not $exe) { Missing "vm/$a/location-win-test.exe isn't staged ($a)"; continue }
    $ran = $true
    $scratch = "C:\rbtest\loc-$a"
    if (Test-Path $scratch) { cmd /c rmdir /s /q $scratch }
    # location-win-test's SCRATCH is an empty folder that is there (it makes only what is inside)
    New-Item -ItemType Directory -Force $scratch | Out-Null
    $envs = @{}
    if ($net) { $envs['RBSP_TEST_NET_DRIVE'] = "$($net.Name):" }
    $rc = Invoke-Exe $exe @($scratch) $envs
    Check ($rc -eq 0) "location-win-test ($a) on a fresh folder: exit 0 (got $rc)"
    if ($net) { Check ($script:ExeOut -notmatch 'no RBSP_TEST_NET_DRIVE') "the network drive $($net.Name): case ran" }
    else { Missing "no network drive mapped in this session (\\Mac\win nor \\127.0.0.1\rbnet): the network case ($a)" }
    $rc = Invoke-Exe $exe @($scratch)
    Check ($rc -ne 0) "a second run on the same folder fails (its mklink and no-folder steps) (got $rc)"
}
if ($mapped) { [void](Invoke-Exe "$env:WINDIR\System32\net.exe" @('use', "${mapped}:", '/delete', '/y')) }
# the fallback is there for this session whichever drive ran the case: \\127.0.0.1\rbnet maps
$free = @('R', 'P', 'O', 'N', 'M') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
if ($free) {
    $rc = Invoke-Exe "$env:WINDIR\System32\net.exe" @('use', "${free}:", '\\127.0.0.1\rbnet', '/persistent:no')
    Check ($rc -eq 0 -and (New-Object IO.DriveInfo "${free}:\").DriveType -eq 'Network') "the fallback \\127.0.0.1\rbnet maps from this session as a network drive (exit $rc)"
    if ($rc -eq 0) { [void](Invoke-Exe "$env:WINDIR\System32\net.exe" @('use', "${free}:", '/delete', '/y')) }
} else { Missing 'no free letter among R P O N M: the \\127.0.0.1\rbnet fallback check' }
if (-not $ran) { Skip 'no location-win-test.exe staged (vm-run.sh stage --build)' }
Done
