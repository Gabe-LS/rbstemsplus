# NW6: run on the clean Windows NTFS system volume, unelevated.
foreach ($a in Get-TestArchs) {
    $exe = Get-TestExe $a 'memo-stamp-wintest.exe'
    if (-not $exe) { Missing "vm/$a/memo-stamp-wintest.exe is not staged"; continue }
    $dir = Join-Path 'C:\rbtest' "memo-$($VmRun.Run)-$a"
    [void](New-Item -ItemType Directory -Path $dir)
    try {
        $wof = Join-Path $dir 'compressible.dat'
        $stamp = Join-Path $dir 'stamp.dat'
        [IO.File]::WriteAllText($wof, ('NW6 synthetic text' * 65536))
        [IO.File]::WriteAllText($stamp, 'first')
        $sha = (Get-FileHash -LiteralPath $wof -Algorithm SHA256).Hash.ToLowerInvariant()
        $rc = Invoke-Exe "$env:SystemRoot\System32\compact.exe" @('/c', '/exe:lzx', $wof)
        Check ($rc -eq 0) "NW6 compact ($a): exit 0 (got $rc)"
        if ($rc -ne 0) { continue }
        $rc = Invoke-Exe $exe @($wof, $sha, $stamp)
        Check ($rc -eq 0 -and $script:ExeOut -match '(?m)^\d+ checks, 0 failed$') "NW6 memo stamp ($a): native checks passed (exit $rc)"
    } finally {
        Remove-Item -LiteralPath $dir -Recurse -Force
    }
}
Done
