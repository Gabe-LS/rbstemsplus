# T13: the track rules on library.c (a dummy key) and manifest.c: the count invariant, c:/ vs C:\
$x = Get-LocalExe 'rbsp-win-rules-test.exe'
$scratch = Join-Path $x.Dir 'scratch'
New-Item -ItemType Directory -Path $scratch | Out-Null
$rc = Invoke-Exe $x.Exe @($scratch)
Check ($rc -eq 0) "Windows' rules on library.c and manifest.c (exit $rc)"
Done
