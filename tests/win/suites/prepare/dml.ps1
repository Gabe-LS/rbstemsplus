# T17 6: --dml in the VM (the timing build, our model, the WAV) must end on the CPU (ep=cpu) with
# every chunk written and one fallback line on stderr. In rb-win-template (2026-11-19) rekordbox's
# DirectML.dll 1.14.1 loads and the session is made on Parallels' GPU, but the first run fails
# (a Mul node, 80004005): that chunk runs again on a CPU session, which the run keeps
Initialize-T17
$wav = (Get-T17Tracks)[0]
$r = Invoke-Prepare 'timing' 'dml' @('--dml', '--model', (Get-T17Model 'ours'), '--cache', (Join-Path $T17 'cache-d'), '--kept-dir', (Join-Path $T17 'kept-d'),
        '--keep', 't17d', '--rekordbox', $T17Rb, '--ort', $T17Ort, $wav)
$err = @($r.Err -split "`n" | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_ -ne '' })
$p = @(Get-Results $r 'prepared')
Check ($r.Exit -eq 0 -and $p.Count -eq 1 -and $p[0].written -eq 3 -and $p[0].refused -eq 0) "dml: exit 0 ($($r.Exit)), the track prepared, written $($p[0].written) of 3"
$fb = @($err | Where-Object { $_ -match '^DirectML: .*: the model runs on the CPU$' })
Check ($fb.Count -eq 1) "dml: one fallback line: $($fb -join ' | ')"
$tl = @($err | Where-Object { $_ -match '^rbsp-timing track ' })
Check ($tl.Count -eq 1 -and $tl[0] -match ' ep=cpu$') "dml: the track ran on the CPU: $($tl -join ' | ')"
Done
