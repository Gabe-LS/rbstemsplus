# T17 1-2 with Pioneer's model and the --test-timing build: set t17p, same cache; the timing
# build's track lines end in power=... ep=cpu
Initialize-T17
$tracks = Get-T17Tracks
$kept = Join-Path $T17 'kept'
$r = Invoke-Prepare 'timing' 'pioneer' (@('--model', (Get-T17Model 'pioneer'), '--cache', (Join-Path $T17 'cache'), '--kept-dir', $kept,
        '--keep', 't17p', '--rekordbox', $T17Rb, '--ort', $T17Ort) + $tracks)
Test-PrepareRun $r 'pioneer' 't17p' $kept $tracks
$tl = @($r.Err -split "`n" | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_ -match '^rbsp-timing track ' })
Check ($tl.Count -eq $tracks.Count) "timing: one track line per track ($($tl.Count))"
foreach ($l in $tl) { Check ($l -match ' power=(ac|battery|unknown) ep=cpu$') "timing: ep=cpu: $l" }
"  INFO power: $(@($tl | ForEach-Object { if ($_ -match ' power=(\S+)') { $Matches[1] } } | Select-Object -Unique) -join ', ')"
Done
