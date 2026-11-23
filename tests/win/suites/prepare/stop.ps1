# T17 7: Stop (stdin closed at the second chunk, as the app's Stop) and Pause then Stop (the
# process suspended, stdin closed, resumed once, as win/c/helper_proc.c does): each ends with a
# stopped track line and a stopped done line, and the kept set and the manifest are written
Initialize-T17
$tracks = Get-T17Tracks
$kept = Join-Path $T17 'kept-s'
$stopAt2 = { param($p, $l, $st) if ($l -match '^\{"at":') { $st.n = 1 + [int]$st.n; if ($st.n -eq 2) { $p.StandardInput.Close() } } }
$pauseStopAt2 = {
    param($p, $l, $st)
    if ($l -match '^\{"at":') {
        $st.n = 1 + [int]$st.n
        if ($st.n -eq 2) {
            $st.suspend = [T17.Nt]::NtSuspendProcess($p.Handle)
            $p.Refresh(); $t0 = $p.TotalProcessorTime
            $p.StandardInput.Close()
            Start-Sleep -Seconds 3
            $p.Refresh(); $st.frozen = ($p.TotalProcessorTime -eq $t0) -and -not $p.HasExited
            $st.resume = [T17.Nt]::NtResumeProcess($p.Handle)
        }
    }
}
foreach ($case in 'stop', 'pause-stop') {
    $keep = if ($case -eq 'stop') { 't17s' } else { 't17ps' }
    $on = if ($case -eq 'stop') { $stopAt2 } else { $pauseStopAt2 }
    $r = Invoke-Prepare 'release' $case (@('--model', (Get-T17Model 'ours'), '--cache', (Join-Path $T17 "cache-$case"), '--kept-dir', $kept,
            '--keep', $keep, '--rekordbox', $T17Rb, '--ort', $T17Ort) + $tracks) -OnLine $on
    if ($case -eq 'pause-stop') {
        Check ($r.State.suspend -eq 0 -and $r.State.resume -eq 0) "pause-stop: suspended and resumed ($($r.State.suspend), $($r.State.resume))"
        Check ([bool]$r.State.frozen) 'pause-stop: no CPU time while suspended with stdin closed'
    }
    Check ($r.Exit -eq 0) "${case}: exit 0 ($($r.Exit))"
    $at = @(Get-AtLines $r).Count
    Check ($r.State.n -ge 2 -and $at -le 3) "${case}: stdin closed at the second chunk, $at at lines"
    $st = @(Get-Results $r 'stopped')
    Check ($st.Count -eq 1 -and $st[0].file -eq $tracks[0]) "${case}: the first track's line says stopped ($($st.Count) stopped lines)"
    $done = Get-DoneLine $r
    Check ($done -and $done.stopped -eq 'stopped' -and $done.kept -eq $keep -and -not $done.kept_error -and -not $done.manifest_error) "${case}: done says stopped, the set kept"
    Check (Test-NoCr (Join-Path $kept "$keep.kept")) "${case}: $keep.kept written, no CR"
    $m = Read-Manifest (Join-Path $kept "$keep.manifest")
    Check ($m -and $m.Files.ContainsKey($tracks[0]) -and $m.Complete[$tracks[0]] -eq '0') "${case}: $keep.manifest written, the first track incomplete"
}
Done
