# MQ4/MR3: run the same native support writer as the core on real NTFS.
foreach ($arch in Get-TestArchs) {
    $exe = Get-TestExe $arch 'winc-location-wintest.exe'
    if (-not $exe) { Missing "winc-location-wintest.exe for $arch"; continue }
    $root = "C:\rbtest\support-errors-$arch"
    New-Item -ItemType Directory -Path $root | Out-Null
    $target = Join-Path $root 'target'
    $junction = Join-Path $root 'junction'
    $file = Join-Path $root 'file'
    New-Item -ItemType Directory -Path $target | Out-Null
    New-Item -ItemType Junction -Path $junction -Target $target | Out-Null
    [IO.Directory]::Delete($target)
    try {
        $rc = Invoke-Exe $exe @('support-error', $junction, 'junction')
        Check ($rc -eq 0) "dangling support junction yields ELOOP ($arch)"
        Check ((Get-Item -LiteralPath $junction -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) "the dangling junction is still present ($arch)"
        Check (-not (Test-Path -LiteralPath $target)) "no target was created ($arch)"
    } finally {
        [IO.Directory]::Delete($junction)
    }
    Set-Content -LiteralPath $file 'keep' -Encoding ASCII
    $rc = Invoke-Exe $exe @('support-error', $file, 'file')
    Check ($rc -eq 0) "regular file at support yields ENOTDIR ($arch)"
    Check ((Get-Content -LiteralPath $file) -ceq 'keep') "the regular file is unchanged ($arch)"
}
Done
