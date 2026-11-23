# (included by prepare.suite) The VM half of T17's first checks (helper.md "T17's first checks"):
# the staged rbsp-prepare.exe builds (helper/tests/prepare-vm-stage.sh) run against the VM's
# installed rekordbox files, which rekordbox itself never opens here. Every step works in one
# folder of the user's LocalAppData (rbsp-t17), so later steps read what earlier ones wrote.
$T17 = Join-Path (Get-UserLocal) 'rbsp-t17'
$T17Rb = 'C:\Program Files\rekordbox\rekordbox 7.2.19'
$T17Ort = Join-Path $T17Rb 'onnxruntime.dll'
$T17Ours = '0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772'
$T17Names = @('20-wav-16-30s.wav', 'w16-flac-longpath.flac', '02-mp3-vbr2.mp3', '05-aac-128.m4a')

# the builds, our model and the corpus copied from the share once per run (the system drive)
function Initialize-T17 {
    $src = Need 'prepare/commit.txt' 'helper/tests/prepare-vm-stage.sh'
    $mark = Join-Path $T17 '.staged'
    if (Test-Path -LiteralPath $mark) { return }
    New-Item -ItemType Directory -Path $T17 -Force | Out-Null
    foreach ($d in 'release', 'timing', 'research', 'model', 'corpus') {
        Copy-Item -LiteralPath (Share "prepare/$d") -Destination $T17 -Recurse -Force
    }
    Copy-Item -LiteralPath (Share 'prepare/rb-keys.txt') -Destination $T17
    "staged from commit $([IO.File]::ReadAllText($src).Trim())"
    [IO.File]::WriteAllText($mark, '')
}
function Get-T17Tracks { return @($T17Names | ForEach-Object { Join-Path $T17 "corpus\$_" }) }
# MODEL: ours (the staged copy) or pioneer (the VM's rekordbox model, from the snapshot)
function Get-T17Model([string]$which) {
    if ($which -eq 'ours') { return Join-Path $T17 'model\stemsplus-model.onnx' }
    $p = Join-Path (Get-UserProfile) 'AppData\Roaming\Pioneer\rekordbox6\models\demucs3_model\hdemucs.onnx'
    if (-not (Test-Path -LiteralPath $p)) { Skip "no Pioneer model at $p (the snapshot 'rekordbox 7.2.19 installed + stems model')" }
    return $p
}
function Get-T17Threads {
    $n = [int]$env:NUMBER_OF_PROCESSORS
    return [Math]::Max(1, [Math]::Min(64, [Math]::Floor($n / 2)))
}

if (-not ('T17.Nt' -as [type])) {
    Add-Type -Namespace T17 -Name Nt -MemberDefinition @'
[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
[DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
'@
}

# Invoke-Prepare BUILD LABEL ARGV [ONLINE]: BUILD's rbsp-prepare.exe with ARGV, stdin a pipe held
# open until the process ends (as the app's). Without ONLINE, stdout is read whole (its raw text,
# CRs included, in .Raw) and the process's priority class is sampled every 500 ms (.Prio); with
# ONLINE, a scriptblock called as & ONLINE $proc $line $state for each stdout line as it comes.
# stdout and stderr go to the run's results as LABEL.out and LABEL.err. Returns a hashtable:
# Exit, Lines, Raw, Err, Prio, Json (the stdout lines parsed), Ms
function Invoke-Prepare([string]$build, [string]$label, [string[]]$argv, [scriptblock]$OnLine = $null, [int]$secs = 560) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $T17 "$build\rbsp-prepare.exe"
    $psi.Arguments = (@($argv | ForEach-Object { ConvertTo-ArgText $_ })) -join ' '
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $utf8 = New-Object Text.UTF8Encoding($false)
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding = $utf8
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $r = @{ Exit = $null; Lines = @(); Raw = ''; Err = ''; Prio = @(); Json = @(); State = @{} }
    if ($OnLine) {
        while ($null -ne ($l = $p.StandardOutput.ReadLine())) {
            $r.Lines += $l
            & $OnLine $p $l $r.State
            if ($sw.Elapsed.TotalSeconds -gt $secs) { $p.Kill(); break }
        }
    } else {
        $outTask = $p.StandardOutput.ReadToEndAsync()
        while (-not $outTask.Wait(500)) {
            try { $p.Refresh(); if (-not $p.HasExited) { $r.Prio += [string]$p.PriorityClass } } catch { }
            if ($sw.Elapsed.TotalSeconds -gt $secs) { try { $p.Kill() } catch { }; break }
        }
        $r.Raw = $outTask.Result
        $r.Lines = @($r.Raw -split "`n" | Where-Object { $_ -ne '' } | ForEach-Object { $_.TrimEnd("`r") })
    }
    try { $p.StandardInput.Close() } catch { }
    if (-not $p.WaitForExit(60000)) { try { $p.Kill() } catch { } }
    $p.WaitForExit()
    $r.Exit = $p.ExitCode
    $r.Err = $errTask.Result
    $r.Ms = [int]$sw.Elapsed.TotalMilliseconds
    foreach ($l in $r.Lines) { try { $r.Json += , ($l | ConvertFrom-Json) } catch { $r.Json += , $null } }
    New-Item -ItemType Directory -Path $VmRun.Results -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $VmRun.Results "$label.out"), ($r.Lines -join "`n") + "`n")
    [IO.File]::WriteAllText((Join-Path $VmRun.Results "$label.err"), $r.Err)
    [Console]::Out.WriteLine("  [$label] exit $($r.Exit), $([int]($r.Ms / 1000)) s, $($r.Lines.Count) stdout lines")
    foreach ($l in $r.Lines) { [Console]::Out.WriteLine("    $l") }
    foreach ($l in @($r.Err -split "`n" | Where-Object { $_ -ne '' })) { [Console]::Out.WriteLine("    err: $($l.TrimEnd("`r"))") }
    return $r
}

# the run's result lines by kind
function Get-Results($r, [string]$result) { return @($r.Json | Where-Object { $_ -and $_.result -eq $result }) }
function Get-AtLines($r) { return @($r.Json | Where-Object { $_ -and $null -ne $_.at }) }
function Get-DoneLine($r) { $d = @($r.Json | Where-Object { $_ -and $null -ne $_.done }); if ($d.Count) { return $d[-1] } }

# a manifest: @{ Head = lines before the first file; Files = @{ path = @(keys) } }. On Windows a
# file line may carry its source rate before the path ("hz=N", helper/manifest.c's put; Hz =
# @{ path = N }) and its keys may be followed by a "decoder ..." line, which is skipped
function Read-Manifest([string]$path) {
    $m = @{ Head = @(); Files = @{}; Complete = @{}; Hz = @{} }
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $lines = [IO.File]::ReadAllText($path) -split "`n"
    $i = 0
    while ($i -lt $lines.Count) {
        $l = $lines[$i]
        if ($l -match '^file (-?\d+) (-?\d+) (\d) (\d+) (?:hz=(\d+) )?(.*)$') {
            $n = [int]$Matches[4]; $f = $Matches[6]; $c = $Matches[3]
            if ($Matches[5]) { $m.Hz[$f] = [int]$Matches[5] }
            $m.Files[$f] = @($lines[($i + 1)..($i + $n)] | Where-Object { $_ -ne '' })
            if ($n -eq 0) { $m.Files[$f] = @() }
            $m.Complete[$f] = $c
            $i += $n + 1
        } else {
            if ($m.Files.Count -eq 0 -and $l -ne '') { $m.Head += $l }
            $i++
        }
    }
    return $m
}
function Test-NoCr([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    return -not ([IO.File]::ReadAllBytes($path) -contains 13)
}

# T17 2: one model's three-plus-one-track run, its lines, its files and its priority; the
# manifest read is left in $script:T17Manifest
function Test-PrepareRun($r, [string]$model, [string]$keep, [string]$kept, [string[]]$tracks) {
    Check ($r.Exit -eq 0) "${keep}: exit 0 ($($r.Exit))"
    Check (-not $r.Raw.Contains("`r")) "${keep}: no CR in stdout"
    $prep = @(Get-Results $r 'prepared')
    Check ($prep.Count -eq $tracks.Count) "${keep}: $($prep.Count) prepared lines for $($tracks.Count) tracks"
    $at = @(Get-AtLines $r)
    $chunks = 0; $runs = 0
    for ($fi = 0; $fi -lt $tracks.Count; $fi++) {
        $line = @($prep | Where-Object { $_.file -eq $tracks[$fi] })
        $nat = @($at | Where-Object { $_.at -eq $fi }).Count
        if ($line.Count -ne 1) { Check $false "${keep}: one prepared line for $($tracks[$fi])"; continue }
        $x = $line[0]
        $chunks += $nat; $runs += $x.written + $x.refused
        Check ($nat -gt 0 -and $x.written + $x.silent + $x.refused + $x.already -eq $nat) "${keep} $(Split-Path -Leaf $tracks[$fi]): $nat at lines = written $($x.written) + silent $($x.silent) + refused $($x.refused) + already $($x.already) ($($x.how))"
        if ($model -eq 'ours') { Check ($x.refused -eq 0) "${keep} $(Split-Path -Leaf $tracks[$fi]): refused 0 with our model" }
        else { "  INFO $keep $(Split-Path -Leaf $tracks[$fi]): Pioneer's model, $($x.refused) of $nat chunks refused (the rebuilt bar on the CPU)" }
    }
    $done = Get-DoneLine $r
    Check ($done -and $done.done -eq $tracks.Count -and -not $done.stopped) "${keep}: done $($done.done), not stopped"
    Check ($done -and $done.runs -eq $runs -and $runs -gt 0) "${keep}: runs $($done.runs) = the chunks run ($runs of $chunks)"
    Check ($done -and $done.kept -eq $keep -and -not $done.kept_error -and -not $done.manifest_error) "${keep}: the set kept, no kept or manifest error"
    $kf = Join-Path $kept "$keep.kept"; $mf = Join-Path $kept "$keep.manifest"
    Check (Test-NoCr $kf) "${keep}: $keep.kept written, no CR"
    Check (Test-NoCr $mf) "${keep}: $keep.manifest written, no CR"
    $m = Read-Manifest $mf
    $th = Get-T17Threads
    Check ($m -and ($m.Head -contains 'helper w1')) "${keep}: the manifest's helper w1 ($(@($m.Head) -join ' | '))"
    Check ($m -and @($m.Head | Where-Object { $_ -match "^rate \d+ [\d.]+ $th$" }).Count -eq 1) "${keep}: the manifest's threads = NUMBER_OF_PROCESSORS / 2 = $th"
    Check (-not ($r.Err -match 'DirectML')) "${keep}: no DirectML line on stderr"
    $prio = @($r.Prio | Select-Object -Unique)
    Check ($prio -contains 'BelowNormal' -and ($r.Prio | Select-Object -Last 1) -eq 'BelowNormal') "${keep}: PriorityClass BelowNormal during the run (seen: $($prio -join ', '))"
    $script:T17Manifest = $m
}
