# location.suite, after folder-mount: run LOCATION_PROBE's path checks on internal NTFS (N3R17).
foreach ($a in Get-LocArchs) {
    $root = Join-Path $env:TEMP "rbsp-n3-$a-$([guid]::NewGuid())"
    $junction = Join-Path $root 'junction'
    $target = Join-Path $root 'target'
    $marked = Join-Path $root 'marked'
    $belowMarker = Join-Path $marked 'child'
    $belowCache = Join-Path $root "$CacheName\child"
    try {
        New-Item -ItemType Directory -Force -Path "$target\child", $belowMarker, $belowCache -ErrorAction Stop | Out-Null
        New-Item -ItemType Junction -Path $junction -Target $target -ErrorAction Stop | Out-Null
        Set-Content -LiteralPath "$marked\.rbstemsplus-cache" -Value 'x' -Encoding ASCII -ErrorAction Stop

        Expect (Loc $a 'choice-probe' 'choice' @("$junction\child", '--internal-ok')) '^0 6\|' "LOCATION_PROBE through a junction ($a)"
        Expect (Loc $a 'choice-probe' 'choice' @($belowMarker, '--internal-ok')) '^0 5\|' "LOCATION_PROBE below a marker ($a)"
        Expect (Loc $a 'choice-probe' 'choice' @($belowCache, '--internal-ok')) '^0 5\|' "LOCATION_PROBE below our cache folder ($a)"
        Expect (Loc $a 'choice-probe' 'choice' @('C:\mnt\n3a', '--internal-ok')) '^0 6\|' "LOCATION_PROBE of the volume mount point ($a)"
    } finally {
        # Remove only the junction before recursively deleting the scratch tree.
        if (Test-Path -LiteralPath $junction) { [IO.Directory]::Delete($junction) }
        if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    }
}
Done
