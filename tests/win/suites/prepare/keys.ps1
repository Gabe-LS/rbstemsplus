# T17 4: the research build's --keys on the four tracks. Ours: exactly the entries the ours step
# wrote (t17.manifest). Pioneer's: the pioneer step's entries are among them, the others are the
# chunks it refused, and every key of the WAV, FLAC and MP3 is one rekordbox itself computed on the
# owner's PC (rb-keys.txt, staged from PC session 1: the host's prediction; the AAC's only shown)
Initialize-T17
$tracks = Get-T17Tracks
$rbk = @{}
foreach ($k in [IO.File]::ReadAllLines((Join-Path $T17 'rb-keys.txt'))) { if ($k) { $rbk[$k] = 1 } }
foreach ($which in 'ours', 'pioneer') {
    $keep = if ($which -eq 'ours') { 't17' } else { 't17p' }
    $m = Read-Manifest (Join-Path $T17 "kept\$keep.manifest")
    if (-not $m) { Check $false "keys ${which}: the $which step left $keep.manifest"; continue }
    $r = Invoke-Prepare 'research' "keys-$which" (@('--keys', '--model', (Get-T17Model $which), '--rekordbox', $T17Rb) + $tracks)
    Check ($r.Exit -eq 0) "keys ${which}: exit 0 ($($r.Exit))"
    $rows = @($r.Lines | ForEach-Object { , ($_ -split "`t") })
    foreach ($t in $tracks) {
        $n = Split-Path -Leaf $t
        $fl = @($rows | Where-Object { $_[0] -eq 'file' -and $_[-1] -eq $t })
        Check ($fl.Count -eq 1) "keys $which ${n}: decoded ($(($fl | ForEach-Object { $_[1..3] -join ' ' }) -join '; '))"
        $ks = @($rows | Where-Object { $_[0] -eq 'key' -and $_[5] -eq $t -and $_[4] -eq '0' } | ForEach-Object { $_[1] })
        $mk = @($m.Files[$t])
        if ($which -eq 'ours') {
            Check ($ks.Count -gt 0 -and ((@($ks | Sort-Object) -join ',') -eq (@($mk | Sort-Object) -join ','))) "keys ours ${n}: the $($ks.Count) keys are the $($mk.Count) entries written"
        } else {
            $extra = @($mk | Where-Object { $ks -notcontains $_ })
            Check ($ks.Count -gt 0 -and $mk.Count -gt 0 -and $extra.Count -eq 0) "keys pioneer ${n}: the $($mk.Count) entries written are among the $($ks.Count) keys"
            $unseen = @($ks | Where-Object { -not $rbk.ContainsKey($_.Substring(0, 12)) })
            $what = "keys pioneer ${n}: $($ks.Count - $unseen.Count) of $($ks.Count) keys computed by rekordbox on the PC ($($unseen.Count) not: $(($unseen | ForEach-Object { $_.Substring(0, 12) }) -join ' '))"
            # AAC is Windows' own Media Foundation decode, which rekordbox uses too: its keys are the
            # machine's. The VM's (Windows 11 ARM64 26H2, the x64 helper emulated) decodes
            # 05-aac-128.m4a differently from the PC's (2026-11-19: half the samples, up to 0.68
            # apart), so the PC's keys are no prediction for it here
            if ($n -match '\.m4a$') { "  INFO $what (AAC: not compared, the OS's decoder)" }
            else { Check ($unseen.Count -eq 0) $what }
        }
    }
}
Done
