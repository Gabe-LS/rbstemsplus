# T17 6: the run path's exit-2 refusals (review PF2: --rekordbox and --ort must be drive-absolute;
# nothing made or loaded), and a 48 kHz FLAC skipped (resampled files aren't prepared on Windows
# until T11)
Initialize-T17
$wav = (Get-T17Tracks)[0]
$cache = Join-Path $T17 'cache-r'
$base = @('--model', (Get-T17Model 'ours'), '--cache', $cache, '--kept-dir', (Join-Path $T17 'kept-r'), '--keep', 't17r')
foreach ($c in @(@('rb-empty', '--rekordbox', '', '--rekordbox needs a drive-absolute path'),
                 @('rb-relative', '--rekordbox', 'rekordbox 7.2.19', '--rekordbox needs a drive-absolute path'),
                 @('ort-relative', '--ort', 'onnxruntime.dll', '--ort needs a drive-absolute path'))) {
    $argv = if ($c[1] -eq '--rekordbox') { @('--rekordbox', $c[2], '--ort', $T17Ort) } else { @('--rekordbox', $T17Rb, '--ort', $c[2]) }
    $r = Invoke-Prepare 'release' "refuse-$($c[0])" ($base + $argv + @($wav))
    Check ($r.Exit -eq 2 -and $r.Err.Trim() -eq $c[3] -and $r.Lines.Count -eq 0) "$($c[0]): exit 2 ($($r.Exit)), '$($c[3])', no stdout"
    Check (-not (Test-Path -LiteralPath $cache)) "$($c[0]): no cache folder made"
}
$f48 = Join-Path $T17 'corpus\w14-flac-mono-48k.flac'
$r = Invoke-Prepare 'release' 'refuse-48k' ($base + @('--rekordbox', $T17Rb, '--ort', $T17Ort, $f48))
$s = @(Get-Results $r 'skipped')
Check ($r.Exit -eq 0) "48 kHz: exit 0 ($($r.Exit))"
Check ($s.Count -eq 1 -and $s[0].file -eq $f48 -and $s[0].why -match '^48000 Hz: ') "48 kHz: skipped: $($s[0].why)"
$done = Get-DoneLine $r
Check ($done -and $done.runs -eq 0 -and (Get-Results $r 'prepared').Count -eq 0) "48 kHz: nothing prepared, runs $($done.runs)"
Done
