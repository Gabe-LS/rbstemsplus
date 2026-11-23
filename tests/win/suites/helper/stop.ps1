# T15: the Stop on stdin, and Pause by suspending (rbsp-win-stop-test.exe drives a child as the app does)
$x = Get-LocalExe 'rbsp-win-stop-test.exe'
$rc = Invoke-Exe $x.Exe @()
Check ($rc -eq 0) "the stop's cases: EOF, a stop line, other lines, Pause then Stop, Pause and Resume, no stdin (exit $rc)"
Done
