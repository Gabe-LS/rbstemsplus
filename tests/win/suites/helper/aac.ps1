# T10 / Q3 (helper.md §2): Windows' AAC decoder through aac_mf.c on the AAC fixtures
# (helper/tests/aac-mf-fixtures.sh, staged into helper/aac/ by helper/tests/win-vm-stage.sh).
# rbsp-aac-mf-probe.exe decodes each file 11 times (async as rekordbox reads it, three times;
# blocking reads at 8 stack shifts) in three separate processes: one output per file each time,
# the same in every process. Its lines and the first run's samples (<name>.s16) go to the run's
# results (aac/) for helper/tests/aac-mf-compare.py on the Mac (priming, length, HE-AAC against
# the sources). Then rbsp-decode-dump.exe, the helper's own path: each file is capped at its sample
# table length or is refused by the rate rule (T11) with its line. The plain 22.05 kHz AAC-LC fixture
# measures MF's rate before revisiting the probe's 22.05 kHz exemption.
$want = [ordered]@{   # name: the probe's rate and channels
    'apple-48k.m4a' = '48000 2'; 'apple-cbr128.m4a' = '44100 2'; 'apple-cbr320.m4a' = '44100 2'; 'apple-he.m4a' = '44100 2'
    'apple-hev2.m4a' = '44100 2'; 'apple-long.m4a' = '44100 2'; 'apple-mono.m4a' = '44100 1'; 'apple-mp4.mp4' = '44100 2'
    'apple-short.m4a' = '44100 2'; 'apple-vbr.m4a' = '44100 2'; 'ff-128.m4a' = '44100 2'; 'ff-320.m4a' = '44100 2'
    'ff-aac_at.m4a' = '44100 2'; 'ff-mono.m4a' = '44100 1'; 'ff-noelst.m4a' = '44100 2'; 'ff-short-last.m4a' = '44100 2'
    'ff-video.mp4' = '44100 2'; 'ff-lc-22k.m4a' = $null; 'ff-hop-boundary.m4a' = '44100 2'
}
$proved = [ordered]@{
    'ff-lc-16k-m4a.m4a' = '16000 2'; 'ff-lc-16k-mp4.mp4' = '16000 2'
    'ff-lc-24k-m4a.m4a' = '24000 2'; 'ff-lc-24k-mp4.mp4' = '24000 2'
    'ff-lc-32k-mono-m4a.m4a' = '32000 1'; 'ff-lc-32k-mono-mp4.mp4' = '32000 1'
}
foreach ($n in $proved.Keys) { $want[$n] = $proved[$n] }
$x = Get-LocalExe 'rbsp-aac-mf-probe.exe'
$dd = Get-LocalExe 'rbsp-decode-dump.exe'
$rc = Invoke-Exe $dd.Exe @('--test-chunk')
Check ($rc -eq 0) "rbsp-decode-dump.exe --test-chunk: $script:ExeOut"
$src = Need 'helper/aac' 'helper/tests/win-vm-stage.sh'
$fix = Join-Path $x.Dir 'aac'
New-Item -ItemType Directory -Path $fix | Out-Null
Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Extension -in '.m4a', '.mp4' } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $fix }
$files = @(Get-ChildItem -LiteralPath $fix -File | Sort-Object Name | ForEach-Object { $_.FullName })
$names = @($files | ForEach-Object { Split-Path -Leaf $_ })
Check ((@($names | Sort-Object) -join ' ') -eq (@($want.Keys | Sort-Object) -join ' ')) "the $($want.Count) AAC fixtures copied to the system drive: $($names -join ' ')"
$out = Join-Path $VmRun.Results 'aac'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$runs = @()
for ($i = 1; $i -le 3; $i++) {
    $d = Join-Path $x.Dir "run$i"
    New-Item -ItemType Directory -Path $d | Out-Null
    $t = [Diagnostics.Stopwatch]::StartNew()
    $rc = Invoke-Exe $x.Exe (@($d) + $files)
    Check ($rc -eq 0) "probe run ${i}: exit $rc, $([int]$t.Elapsed.TotalSeconds) s"
    [IO.File]::WriteAllText((Join-Path $out "run$i.txt"), $script:ExeOut + "`n")
    $runs += , @($script:ExeOut -split "`n")
}
Get-ChildItem -LiteralPath (Join-Path $x.Dir 'run1') -Filter '*.s16' | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $out }
# probe columns: 0 name, 1 frames, 2 rate, 3 ch, 9 first_ts, 12 gaps, 16 distinct outputs, 18 type changes
$probe = @{}
foreach ($line in @($runs[0] | Where-Object { $_ -ne '' })) {
    $f = $line -split "`t"
    Check (-not $probe.ContainsKey($f[0])) "$($f[0]): one probe row"
    $probe[$f[0]] = $f
}
foreach ($n in $want.Keys) {
    $f = $probe[$n]
    if (-not $f -or $f.Count -lt 19) { Check $false "${n}: decoded by the probe ($($f -join ' '))"; continue }
    Check ($f[16] -eq '1') "${n}: one output in 11 decodes (async x3, blocking at 8 stack shifts): $($f[16])"
    if ($null -eq $want[$n]) {
        Check ([int]$f[2] -gt 0 -and $f[3] -eq '2') "${n}: MF decoded at $($f[2]) Hz, $($f[3]) channels"
    } else {
        Check ("$($f[2]) $($f[3])" -eq $want[$n]) "${n}: $($f[2]) Hz, $($f[3]) channels (want $($want[$n]))"
    }
    $maySettle = $null -eq $want[$n] -or $proved.Contains($n)
    Check ($f[9] -eq '0' -and $f[12] -eq '0' -and ($f[18] -eq '0' -or ($maySettle -and $f[18] -eq '11'))) "${n}: first sample at 0 ($($f[9])), $($f[12]) gaps, $($f[18]) type changes"
}
Check ($probe.Count -eq $want.Count) "$($probe.Count) probe rows for $($want.Count) fixtures"
$boundary = $probe['ff-hop-boundary.m4a']
Check ($boundary -and $boundary[1] -eq '482304') "ff-hop-boundary.m4a: 471 full packets cross the first hop ($($boundary | Select-Object -Skip 1 -First 1) frames)"
Check (($runs[0] -join "`n") -eq ($runs[1] -join "`n")) 'run 2 (another process): the same lines and outputs as run 1'
Check (($runs[0] -join "`n") -eq ($runs[2] -join "`n")) 'run 3 (another process): the same lines and outputs as run 1'
$dout = Join-Path $dd.Dir 'out'
New-Item -ItemType Directory -Path $dout | Out-Null
$rc = Invoke-Exe $dd.Exe (@('C:\no-rekordbox', $dout) + $files)
Check ($rc -eq 0) "rbsp-decode-dump.exe on the fixtures: exit $rc"
[IO.File]::WriteAllText((Join-Path $out 'decode-dump.txt'), $script:ExeOut + "`n")
$dump = @{}
foreach ($line in @($script:ExeOut -split "`n" | Where-Object { $_ -ne '' })) {
    $n = ($line -split "`t")[0]
    Check (-not $dump.ContainsKey($n)) "${n}: one decode-dump row"
    $dump[$n] = $line
}
$tableFrames = @{
    'ff-128.m4a' = 1324024; 'ff-320.m4a' = 1324024; 'ff-mono.m4a' = 883024
    'ff-noelst.m4a' = 883024; 'ff-short-last.m4a' = 883323; 'ff-video.mp4' = 883024
    'ff-hop-boundary.m4a' = 481714
}
foreach ($n in $want.Keys) {
    $line = "$($dump[$n])"
    if ($n -eq 'apple-48k.m4a') { $ok = $line -match "`tskipped: 48000 Hz: " }
    elseif ($proved.Contains($n)) {
        $rate = ($proved[$n] -split ' ')[0]
        $ok = $line -match "`tskipped: ${rate} Hz: this rekordbox version's resampler isn't proved yet"
    }
    elseif ($n -eq 'ff-lc-22k.m4a' -and $probe[$n] -and $probe[$n][2] -ne '44100') {
        $ok = $line -match "`tskipped: $($probe[$n][2]) Hz( after decoding)?: "
    }
    else {
        $f = $line -split "`t"
        $len = if ($f.Count -eq 4) { [long]$f[3] } else { 0 }
        $raw = if ($probe[$n]) { [long]$probe[$n][1] } else { 0 }
        $ok = $len -gt 0 -and $len -le $raw -and $f[1] -eq '1 tracks' -and
              $f[2] -match "^Media Foundation x1 output, length $len( \(decoded $raw samples, the sample table says \d+\))?$"
        if ($tableFrames.ContainsKey($n)) { $ok = $ok -and $len -eq $tableFrames[$n] }
    }
    Check $ok "decode-dump ${n}: $(($line -split "`t" | Select-Object -Skip 1) -join ' ')"
}
Check ($dump.Count -eq $want.Count) "$($dump.Count) decode-dump rows for $($want.Count) fixtures"
$provedFiles = @($proved.Keys | ForEach-Object { Join-Path $fix $_ })
$rc = Invoke-Exe $dd.Exe (@('--resample-proved', 'C:\no-rekordbox', $dout) + $provedFiles)
Check ($rc -eq 0) "rbsp-decode-dump.exe with proved resampling: exit $rc"
$rows = @($script:ExeOut -split "`n" | Where-Object { $_ -ne '' })
Check ($rows.Count -eq $proved.Count) "$($rows.Count) decode-dump rows with proved resampling"
foreach ($n in $proved.Keys) {
    $lines = @($rows | Where-Object { $_.StartsWith("$n`t") })
    Check ($lines.Count -eq 1 -and $lines[0] -match "`t1 tracks`tMedia Foundation x1 output, length \d+, rekordbox for Windows' resampler`t\d+$") "decode-dump proved ${n}: $lines"
}
Done
