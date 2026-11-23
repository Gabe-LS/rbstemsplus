# T17 1-2 with our model and the release build: the four tracks prepared into a fresh cache, set t17
Initialize-T17
$tracks = Get-T17Tracks
$kept = Join-Path $T17 'kept'
$r = Invoke-Prepare 'release' 'ours' (@('--model', (Get-T17Model 'ours'), '--cache', (Join-Path $T17 'cache'), '--kept-dir', $kept,
        '--keep', 't17', '--rekordbox', $T17Rb, '--ort', $T17Ort) + $tracks)
Test-PrepareRun $r 'ours' 't17' $kept $tracks
Done
