# T17 3: the ours run again, unchanged (every track "unchanged since it was prepared", no model
# run), and --estimate on the same files: every track ready
Initialize-T17
$tracks = Get-T17Tracks
$kept = Join-Path $T17 'kept'
$base = @('--model', (Get-T17Model 'ours'), '--cache', (Join-Path $T17 'cache'), '--kept-dir', $kept, '--keep', 't17', '--rekordbox', $T17Rb, '--ort', $T17Ort)
if (-not (Test-Path -LiteralPath (Join-Path $kept 't17.manifest'))) { Check $false 'the ours step left t17.manifest'; Done }
$r = Invoke-Prepare 'release' 'again' ($base + $tracks)
Check ($r.Exit -eq 0) "again: exit 0 ($($r.Exit))"
$un = @(Get-Results $r 'prepared' | Where-Object { $_.how -eq 'unchanged since it was prepared' -and $_.written -eq 0 })
Check ($un.Count -eq $tracks.Count) "again: $($un.Count) of $($tracks.Count) tracks unchanged since they were prepared"
Check ((Get-AtLines $r).Count -eq 0) 'again: no chunk run (no at line)'
$done = Get-DoneLine $r
Check ($done -and $done.runs -eq 0 -and $done.done -eq $tracks.Count -and -not $done.stopped) "again: done $($done.done), runs $($done.runs)"
$r = Invoke-Prepare 'release' 'estimate' (@('--estimate') + $base + $tracks)
Check ($r.Exit -eq 0) "estimate: exit 0 ($($r.Exit))"
$e = @($r.Json | Where-Object { $_ -and $null -ne $_.estimate })
$e = if ($e.Count) { $e[-1] } else { $null }
Check ($e -and $e.ready -eq $tracks.Count -and $e.prepare -eq 0 -and $e.skipped -eq 0) "estimate: ready $($e.ready) of $($tracks.Count), prepare $($e.prepare), skipped $($e.skipped)"
Done
