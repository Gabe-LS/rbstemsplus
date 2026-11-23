# midi-auto.ps1: rekordbox driven by MIDI on the test PC, unattended. Run as the test account over
# SSH while that account is signed in (its session runs rekordbox, through the "RBSP session" task).
# pc-session.sh (on the Mac) calls it; each -Do is also usable by hand:
#   -Do ensure            the session task registered, the Loopback A/B pair (recreated if gone), the
#                         mapping, rekordbox, the corpus and OCR checked: one OK/BAD line each
#   -Do ports             the WinMM MIDI ports and the MIDI console
#   -Do learn -Note N -Repeat S   press note N every 2 s for S s (for rekordbox's MIDI LEARN)
#   -Do start | quit      start rekordbox and wait for its first MIDI | close it (pausing deck 1 first)
#   -Do modules           the DirectML/GPU DLLs the running rekordbox loaded (exit 3: no DirectML.dll)
#   -Do shot | windows | closeagent | ort | logoff    one session command (logoff: exit 1 unless the
#                         session task answers that it is signing out)
#   -Do winlist           Win32 window facts and crop paths for rekordbox in the test session
#   -Do press -Keys 'Down x3,Load'   presses, then a screenshot
#   -Do top               sort by title, row 1, a screenshot
#   -Do run [-From NAME] [-To NAME] [-Max N] [-Expect any|hits] [-InjectAt N]
#                         to the playlist, row 1 checked, then each row loaded on deck 1, read back
#                         by OCR and waited for in the bridge's log. -Expect hits: every row must
#                         give hits, no misses and no refused hits (a run after the cache is
#                         filled); with either, a row without bridge activity fails it. -InjectAt N:
#                         the dialog watch's test window opens before row N, which must be one the
#                         run loads (else exit 2); the run must STOP there, marked "(watch test)"
#   -Do watchtest         the dialog watch's own test: a test window opened, seen, closed
#   -Do prepare [-Playlist NAME] [-Minutes N]   our app's Prepare on NAME, waited for
#   -Do uninstallcache    our app's "Uninstall Stems Cache" (rekordbox closed), Pioneer's file checked
#   -Do autoanalysis      Auto Analysis off in rekordbox's Preferences (running rekordbox, owner decision
#                         2026-11-19) | -Do autoanalysis-read: rekordbox3.settings' AutoAnalysis (exit 3 unless 0)
#   -Do dmlbench -From A -To B [-Minutes N]   the kit's timing helper (stage --helper) on rows A..B with
#                         rekordbox closed: its CPU, then --dml, each into a scratch cache; the per-chunk
#                         times and DirectML's lines; the --dml entries copied into the Stems Cache (exit 3:
#                         DirectML fell back to the CPU)
#   -Do dmlstall -From A  best effort: deck 1 (loaded) played while --dml prepares row A; its elapsed
#                         time read by OCR every 2 s (stalled steps counted)
#   -Do appswap -Argument TLS|HTTP|SHA   our app replaced by another TEST release (its own uninstall,
#                         then that release's bootstrap from the Mac's server, byte-checked)
#   -Do appmin | appcheck the black-window repro: our app minimized | restored and checked (exit 3: black)
#   -Do appexit | appopen our app closed by its own Exit (gone within 30 s) | started again; prepare
#                         starts with an appexit too (a fresh app)
#   -Do bundle            the logs, screenshots and mapping zipped in %TEMP%\rbst ("BUNDLE <path>")
#   -Do bundled -Keys ZIP the screenshots in that bundle deleted (once the Mac has unpacked it)
# Stops on its own (a "<step>: STOP:" line): %TEMP%\rbst\midi-stop exists; a new window of rekordbox,
# the rekordbox updater or agent, our app or Windows' error reporting, or such a window already
# open when run or prepare starts. The verified rekordbox update offer and rekordbox's verified news
# window (INFO) and rekordboxAgent's verified news window are closed, and its STEMS engine progress
# window is waited out for up to 60 s (watch-lib.ps1); deck 1 not the expected track; a track's stems
# not done in -MaxTrack s; rekordbox gone; an error. quit handles the same windows, then checks the windows
# against run's baseline (or the start rule) before it touches anything.
# Exit 0 done; 1 a STOP or a failed step; 2 refused (bad arguments, wrong state); 3 an expectation
# (-Expect hits) not met. Log: %TEMP%\rbst\midi-auto.log.
param([Parameter(Mandatory = $true)][ValidateSet('ensure', 'version', 'ports', 'learn', 'start', 'quit', 'shot', 'windows', 'winlist', 'closeagent', 'ort', 'logoff', 'modules', 't2-facts', 't2-probe', 'desktop', 'press', 'top', 'run', 'watchtest', 'prepare', 'uninstallcache', 'bundle', 'bundled', 'autoanalysis', 'autoanalysis-read', 'appexit', 'appopen', 'dmlbench', 'dmlstall', 'preconditions', 'appmin', 'appcheck', 'appswap', 'benchstop', 'playlists', 'library-check', 'stopkeep', 'keptinfo', 'rowkeys', 'keycompare', 'format-import', 'format-reuse', 'format-verify', 'format-uidump', 'format-step', 'format-plan', 'format-run', 'format-hits', 'loc')][string]$Do,
      [string]$From, [string]$To, [Alias('Rows')][string]$RowList = '', [int]$Max = 0, [ValidateSet('any', 'hits', 'keys')][string]$Expect = 'any', [int]$InjectAt = 0, [string]$Activity = '',
      [string]$Playlist = 'RBSP Tests', [int]$Minutes = 60, [string]$Keys, [int]$Note = 0, [int]$Repeat = 0,
      [ValidatePattern('^(m12-[a-z-]+|t2-model-(on|off)|dismiss-rb-error)$')][string]$Step, [string]$Argument, [int]$Seconds = 30,
      [ValidateSet('', 'session2', 'prepare', 'record', 'dumps')][string]$Plan = '',
      [ValidateSet('', 'set', 'thispc', 'pick', 'texts', 'install', 'uninstall', 'netmap', 'netunmap', 'windows')][string]$LocStep = '', [string]$LocArg = '',
      [ValidatePattern('^[a-f0-9]{32}$')][string]$FormatRun, [ValidateRange(1, 9999)][int]$FormatBatch = 1,
      [switch]$FormatPass,
      [ValidatePattern('^[0-9]+:[0-9]+/(absent|[0-9]+:[0-9]+),(absent|[0-9]+:[0-9]+)$')][string]$FormatContinuation,
      [ValidatePattern('^[a-f0-9]{32}$')][string]$EvidenceRun,
      [ValidatePattern('^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}$')][string]$RbVersion,
      [switch]$ConfirmImportRefusals, [switch]$Explore, [switch]$TestUnprovedAac, [switch]$TestUnprovedMp3,
      [switch]$Aac, [switch]$Formats, [switch]$PauseTest, [ValidateRange(0, 90)][int]$Warmup = 0, [int]$StartWait = 25, [int]$Quiet = 10, [int]$MaxTrack = 420)
$ErrorActionPreference = 'Stop'
# rekordbox refuses these (deny icon, PC session 1): 64-bit float WAV, FLAC at 11.025/352.8/384 kHz
$Refused = 'w02-wav-f64-quiet', 'w04-wav-f64-over1', 'w05-flac-24-11025', 'w06-flac-24-352800', 'w07-flac-24-384000'
# rekordbox doesn't import these at all (no row, no icon): raw AAC (ADTS)
$NotImported = 'w11-aac-raw'
# deck 1 shows only the 8.3 short name for the copy; the ~N digit can differ
$LongCopy = 'w16-flac-longpath-copy'
$Aliases = @{ $LongCopy = @('W16-FL~1') }

# the playlist's rows in rekordbox's track-title order: the corpus files' names, sorted
function Rows {
    $files = @(Get-ChildItem $rowDirs -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -notin '.txt', '.tsv', '' } | ForEach-Object { $_.BaseName } | Where-Object { $NotImported -notcontains $_ } | Sort-Object -Unique)
    foreach ($name in $files) {
        $name
        if ($name -eq 'w16-flac-longpath' -and (Test-Path -LiteralPath (Join-Path $Corpus 'long-path.txt'))) { $LongCopy }
    }
}
# the DirectML bench ($BenchDir: %TEMP%\rbst\bench): the helper, model, runtime and rekordbox it
# runs with (the app's own: rekordbox's model file, our root's ort\onnxruntime.dll: Stems Cache
# installed); $null (said) when one is missing
function Bench-Paths([switch]$KeysOnly, [switch]$QuietMissing) {
    $rb = Get-Rb
    if (-not $rb) { if (-not $QuietMissing) { Say "${Do}: no rekordbox installed" }; return $null }
    $ort = if (-not $KeysOnly -and $env:ProgramFiles) { Join-Path $env:ProgramFiles 'RB Stems Plus\ort\onnxruntime.dll' } else { '' }
    $p = [pscustomobject]@{ Rb = $rb.Dir; Exe = (Join-Path $Kit 'bench\rbsp-prepare.exe')
        Model = (Join-Path $env:APPDATA 'Pioneer\rekordbox6\models\demucs3_model\hdemucs.onnx')
        Ort = $ort }
    $need = @($p.Exe, $p.Model)
    if (-not $KeysOnly) { $need += @($p.Ort, (Join-Path $rb.Dir 'DirectML.dll')) }
    foreach ($f in $need) {
        if (-not $f -or -not (Test-Path -LiteralPath $f -PathType Leaf)) {
            if (-not $QuietMissing) { Say "${Do}: $f missing (the timing/research helper: stage --helper; our runtime: Stems Cache installed)" }
            return $null
        }
    }
    return $p
}
function Row-File([string]$name) {
    if ($name -eq $LongCopy) {
        $lp = Join-Path $Corpus 'long-path.txt'
        if (Test-Path -LiteralPath $lp -PathType Leaf) {
            $p = (Get-Content -LiteralPath $lp -Raw).Trim()
            if ($p) { return $p }
        }
        Say "${Do}: row ${name}: missing corpus\long-path.txt"; return $null
    }
    $f = @(Get-ChildItem $rowDirs -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -notin '.txt', '.tsv', '' -and $_.BaseName -eq $name })
    if ($f.Count -ne 1) { Say "${Do}: row ${name}: expected one corpus file, found $($f.Count)"; return $null }
    return $f[0].FullName
}
# the corpus files of rows FROM..TO (rekordbox's title order); nothing (said) if the range isn't there
function Bench-Files([string]$from, [string]$to) {
    $names = @(Rows); $i = [array]::IndexOf($names, $from); $j = [array]::IndexOf($names, $to)
    if ($i -lt 0 -or $j -lt $i) { Say "${Do}: rows '$from'..'$to' not in the corpus"; return }
    foreach ($n in $names[$i..$j]) {
        $p = Row-File $n
        if (-not $p) { return }
        $p
    }
}
function Select-TargetRows([switch]$RequireOne, [string[]]$Names = @(Rows)) {
    $all = $Names
    $seen = @{}; $out = New-Object System.Collections.Generic.List[string]
    function Add-One([string]$n) { if (-not $seen.ContainsKey($n)) { $seen[$n] = $true; $out.Add($n) } }
    if ($RowList) {
        foreach ($part in @($RowList -split ',')) {
            if ($part -match '^([^:]+):([^:]+)$') {
                $i = [array]::IndexOf($all, $Matches[1]); $j = [array]::IndexOf($all, $Matches[2])
                if ($i -lt 0 -or $j -lt $i) { Say "${Do}: rows '$part' not in the corpus"; return @() }
                foreach ($n in $all[$i..$j]) { Add-One $n }
            } else {
                if ($all -notcontains $part) { Say "${Do}: no row named '$part' in the corpus"; return @() }
                Add-One $part
            }
        }
    } elseif ($From) {
        $end = if ($To) { $To } else { $From }
        $i = [array]::IndexOf($all, $From); $j = [array]::IndexOf($all, $end)
        if ($i -lt 0 -or $j -lt $i) { Say "${Do}: rows '$From'..'$end' not in the corpus"; return @() }
        foreach ($n in $all[$i..$j]) { Add-One $n }
    } elseif ($Formats) {
        foreach ($n in @($all | Where-Object { $_ -like 'fmt-*' })) { Add-One $n }
        if (-not $out.Count) { Say "${Do}: no format rows in $Corpus"; return @() }
    } elseif ($RequireOne) {
        Say "${Do}: needs -From, -Rows, or -Formats"; return @()
    }
    return $out.ToArray()
}
$researchKeyFlags = @(
    if ($TestUnprovedAac) { '--test-unproved-aac' }
    if ($TestUnprovedMp3) { '--test-unproved-mp3' }
)
function Get-ResearchKeysArguments($paths, [string]$file) {
    $options = ($researchKeyFlags + '--resample-proved' + '--keys') -join ' '
    '{0} --model "{1}" --rekordbox "{2}" "{3}"' -f $options, $paths.Model, $paths.Rb, $file
}
function Invoke-ResearchKeys($paths, [string]$file, [string]$row) {
    $si = New-Object Diagnostics.ProcessStartInfo $paths.Exe
    $si.Arguments = Get-ResearchKeysArguments $paths $file
    $si.UseShellExecute = $false; $si.RedirectStandardOutput = $true; $si.RedirectStandardError = $true; $si.CreateNoWindow = $true
    $clock = Start-KitTiming
    $p = $null
    try {
        $p = [Diagnostics.Process]::Start($si)
        $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit(300000)) {
            try { $p.Kill() } catch { }
            return @{ TimedOut = $true }
        }
        return @{ ExitCode = $p.ExitCode; Output = $o.Result; Stderr = $e.Result }
    } finally {
        if ($p) { $p.Dispose() }
        Write-KitTiming 'helper-keys' $clock $row
    }
}
function New-ResearchHelperLog([string]$Directory) {
    . (Join-Path $PSScriptRoot 'format-lib.ps1')
    $path = Join-Path $Directory 'helper-stderr.log'
    Assert-FormatPath $path
    [void][IO.Directory]::CreateDirectory($Directory)
    [IO.File]::WriteAllText($path, '')
    return $path
}
function Write-ResearchHelperDiagnostics([string]$Stderr, [string]$Row, [string]$Path) {
    # Match the trace's entire schema so a known prefix cannot admit a path or title.
    $pcm = '[0-9]+ Hz/[0-9]+ ch/[0-9]+ bit'
    $trace = '\AAAC MF type change: stream=[0-9]+ native=0x[0-9a-f]{4,8}/[0-9]+ Hz/[0-9]+ ch ' +
        "PCM before=$pcm after=(?:$pcm|unavailable) " +
        'type_hr=0x[0-9a-f]{8} flags=0x[0-9a-f]{1,8} frames=[0-9]+ sample=[01]\z'
    $label = if ($Row -cmatch '\A(?:fmt-|[0-9]{2}-|w[0-9]+-)[a-zA-Z0-9._~-]+\z') { $Row } else { 'fixture' }
    $lines = @(foreach ($line in $Stderr -split "`r?`n") {
        if ($line -cmatch $trace) { "helper stderr ${label}: $line" }
    })
    if ($lines.Count) {
        [IO.File]::AppendAllLines($Path, [string[]]$lines)
        foreach ($line in $lines) { Say $line }
    }
}
# one helper run NAME (cpu: the CPU; dml, stall: --dml) of FILES into the fresh scratch CACHE, in the
# desktop session (dml-run.ps1), waited for up to SECS (TICK run meanwhile, else 2 s sleeps): its
# exit code; 98 not started, 97 not done in time (then stopped: it would hold our app's ORT)
function Bench-Run([string]$name, [string[]]$files, [string]$cache, $bp, [int]$secs, [scriptblock]$tick) {
    [void](New-Item -ItemType Directory -Force $BenchDir)
    foreach ($x in "$name.out", "$name.err", "$name.exit") { Remove-Item -LiteralPath (Join-Path $BenchDir $x) -ErrorAction SilentlyContinue }
    $kept = Join-Path $BenchDir "kept-$name"
    $cache = Assert-SystemDrivePath $cache; $kept = Assert-SystemDrivePath $kept   # never the owner's backup disk
    foreach ($d in $cache, $kept) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }; [void](New-Item -ItemType Directory -Force $d) }
    $a = @('--model', $bp.Model, '--cache', $cache, '--kept-dir', $kept, '--no-keep', '--ort', $bp.Ort, '--rekordbox', $bp.Rb)
    if ($name -ne 'cpu') { $a += '--dml' }
    Set-Content -LiteralPath (Join-Path $BenchDir "spec-$name.txt") -Value ($a + $files) -Encoding UTF8
    $r = Session 'benchstart' 30 $name; Say "${Do}: $r" | Out-Host   # said, not returned (the exit code is the return)
    if ($r -notmatch "benchstart: $name started") { return 98 }
    $exit = Join-Path $BenchDir "$name.exit"; $t = Get-Date
    $said = 0   # a line a minute: the Mac's idle limit is 10 min
    while (-not (Test-Path -LiteralPath $exit) -and ((Get-Date) - $t).TotalSeconds -lt $secs) {
        if ($tick) { & $tick | Out-Host } else { Start-Sleep 2 }
        $m = [int][math]::Floor(((Get-Date) - $t).TotalMinutes); if ($m -gt $said) { $said = $m; Say "${Do}: $name run: $m min" | Out-Host }
    }
    if (-not (Test-Path -LiteralPath $exit)) { $r = Session 'benchstop' 30; Say "${Do}: $name run not done in $secs s: $r" | Out-Host; return 97 }
    Start-Sleep -Milliseconds 300
    Say "${Do}: $name run done in $([int]((Get-Date) - $t).TotalSeconds) s" | Out-Host
    return [int](@(Get-Content -LiteralPath $exit)[0])
}
# a screenshot no longer needed ('' or missing: nothing to do)
function Drop([string]$p) { if ($p) { Remove-Item -LiteralPath $p -ErrorAction SilentlyContinue } }
function Lines { if (Test-Path -LiteralPath $Bridge) { @(Get-Content -LiteralPath $Bridge).Count } else { 0 } }

function Get-ChunkSetDifferences($rbSet, $required, $allowed) {
    return @{
        Missing = @($required.Keys | Where-Object { -not $rbSet.ContainsKey($_) }).Count
        Extra = @($rbSet.Keys | Where-Object { -not $allowed.ContainsKey($_) }).Count
    }
}

# deck 1's title, read from a screenshot at 4 scales; the screenshot is kept only on a mismatch
function Deck1 {
    Start-Sleep -Milliseconds 1500
    # rekordbox in front before its screen is read; if it can't be, the run's error STOP
    $f = Session 'front' 60
    Assert-RbDialogReply $f
    if ($f -notmatch '^\S+ front: ok; ') { throw "rekordbox not put in front ($f)" }
    $png = Shot-Path (Session 'shot')
    if (-not $png) { return @{ ts = @(); png = '' } }
    try { $r = Read-Deck1Titles $png } catch { Drop $png; throw }
    if (-not (($r -join '').Trim())) {   # one fresh screenshot before giving up
        Drop $png; Start-Sleep 1
        $png = Shot-Path (Session 'shot'); if ($png) { try { $r = Read-Deck1Titles $png } catch { Drop $png; throw } }
    }
    return @{ ts = $r; png = $png }
}

# the bridge after a load: none (no new line in StartWait s), done (then quiet for Quiet s),
# timeout (still writing after MaxTrack s: its lines would be counted against the next row)
# or stop (midi-stop created meanwhile)
function WaitStems([int]$n0, [datetime]$t0) {
    while ((Lines) -eq $n0 -and ((Get-Date) - $t0).TotalSeconds -lt $StartWait) {
        if (Test-Path -LiteralPath $Stop) { return @{ state = 'stop' } }
        Start-Sleep -Milliseconds 500
    }
    if ((Lines) -eq $n0) { return @{ state = 'none' } }
    $t1 = Get-Date; $n = Lines; $last = Get-Date
    while (((Get-Date) - $last).TotalSeconds -lt $Quiet -and ((Get-Date) - $t0).TotalSeconds -lt $MaxTrack) {
        if (Test-Path -LiteralPath $Stop) { return @{ state = 'stop' } }
        Start-Sleep -Milliseconds 500; $m = Lines; if ($m -ne $n) { $n = $m; $last = Get-Date }
    }
    $st = if (((Get-Date) - $last).TotalSeconds -ge $Quiet) { 'done' } else { 'timeout' }
    return @{ state = $st; first = ($t1 - $t0).TotalSeconds; done = ($last - $t0).TotalSeconds }
}

function Open-TestPlaylist([IntPtr]$h, [int]$playlistRow = 2, [int]$ResetUp = [math]::Max(64, $playlistRow + 2)) {
    # to the playlist (the owner's walk, PC session 1): Area Left x5 reaches the icon column
    # whatever had the focus; Up x10 = the top icon (Collection), Down = Playlists; Area Right =
    # the tree, Up to All Tracks, Down x2 = RBSP Tests (format runs supply their library position);
    # Area Right = its track list. Area Right enters the tree at the playlist rekordbox selected last
    # (kept across restarts), so the Up count must cover the whole tree: 10 stopped short once format
    # imports had put a selected playlist at step 22 (PC 2026-11-23: T2 read another heading and
    # STOPped). Extra Ups stay at the top.
    Press $h 'AreaLeft' 5 250; Press $h 'Up' 10 120; Press $h 'Down'; Start-Sleep -Milliseconds 800
    Press $h 'AreaRight'; Press $h 'Up' $ResetUp 120
    Press $h 'Down' $playlistRow 250; Start-Sleep -Milliseconds 800
    Press $h 'AreaRight'; Start-Sleep -Milliseconds 1500
}

function Test-FormatBrowserHeading([datetime]$deadline) {
    $front = Session 'front' 3
    Assert-RbDialogReply $front
    if ($front -notmatch '^\S+ front: ok; ' -or (Get-Date) -ge $deadline) { return $false }
    $png = Shot-Path (Session 'shot' 3)
    if (-not $png) { return $false }
    try {
        if ((Get-Date) -ge $deadline) { return $false }
        $readings = try { @(Read-FormatListHeaders $png) } catch { @() }
        $seen = @{}
        foreach ($reading in $readings) {
            if ($reading -notmatch '^\s*\S.+\(\s*\d+\s*tracks?\s*\)\s*$') { continue }
            $text = Fold (Norm $reading)
            if ($seen.ContainsKey($text)) { return $true }
            $seen[$text] = $true
        }
        return $false
    } finally { Drop $png }
}

function Wait-FormatBrowser {
    $deadline = ([RbspMidi2]::First).AddSeconds(15)
    $clock = Start-KitTiming
    $ready = $false
    try {
        . (Join-Path $PSScriptRoot 'ocr-lib.ps1')
        while ((Get-Date) -lt $deadline) {
            $s = Watch-Start 'start' -Allow -News -RedactTitles
            if ($s) { Say $s; exit 1 }
            if (-not $ready -and $script:Base -match '^rekordbox \| rekordbox \| 0x[0-9A-F]+$' -and
                (Get-Date) -lt $deadline) { $ready = Test-FormatBrowserHeading $deadline }
            $remainingMs = [int][math]::Ceiling(($deadline - (Get-Date)).TotalMilliseconds)
            if ($remainingMs -gt 0) { Start-Sleep -Milliseconds ([math]::Min(500, $remainingMs)) }
        }
        # The format task still verifies its own UI after the usual startup allowance.
        if (-not $ready) { Say 'start: browser heading still pending after 15 s' }
    } finally { Write-KitTiming 'browser-readiness' $clock }
}

$formatLock = $null
$commandClock = $null
try {
if ($FormatRun -or $EvidenceRun -or ($Do -eq 'rowkeys' -and $Formats) -or
    $Do -in 'winlist', 'library-check', 'format-import', 'format-reuse', 'format-verify', 'format-uidump', 'format-step', 'format-hits') {
    . (Join-Path $PSScriptRoot 'format-lib.ps1')
    $scratch = Join-Path $env:TEMP 'rbst'
    foreach ($rel in 'shots', 'midi-auto.log', 'task-lock', 'task-cmd.txt', 'task-out.txt', 'task-out.txt.tmp', 'watch-base.txt', 't2') {
        Assert-FormatPath (Join-Path $scratch $rel)
    }
}
. (Join-Path $PSScriptRoot 'kit-lib.ps1')   # inside the try: a library that fails to load is a STOP line too
. (Join-Path $PSScriptRoot 'timing-lib.ps1')
$commandClock = Start-KitTiming
if ($RbVersion) { [void](Get-Rb) }
. (Join-Path $PSScriptRoot 'midi-lib.ps1')
. (Join-Path $PSScriptRoot 'match-lib.ps1')
. (Join-Path $PSScriptRoot 'watch-lib.ps1')
# -Activity: another log stands in for the bridge's (the probe's C:\rbtest\probe.log, -Expect keys)
if ($Activity) { if ($Activity -notmatch '^[A-Za-z]:\\[^"'']+\.log$') { "${Do}: -Activity: a .log path"; exit 2 }; $Bridge = $Activity }
$Stop = Join-Path $B 'midi-stop'
$Corpus = Join-Path $Kit 'corpus'
$rowDirs = @((Join-Path $Corpus 'v1'), (Join-Path $Corpus 'win'))
if ($Formats -or $FormatRun) { $rowDirs += Join-Path $Corpus 'formats' }
$BenchDir = Join-Path $B 'bench'   # the DirectML bench's files (dml-run.ps1 writes there too)
if ($Do -in 'start', 'quit', 'run', 'logoff', 'modules', 'autoanalysis', 'dmlbench', 'dmlstall', 'format-import', 'format-plan', 'format-run', 'format-hits') {
    $r = Session 'ping' 20
    if ($r -notmatch '^\S+ ping: .+ session (\d+)$') { Say "${Do}: STOP: the session task did not report a session ID ($r)"; exit 1 }
    $sid = [int]$Matches[1]
    if ($Do -eq 'start' -or ($RbVersion -and $Do -ne 'logoff')) {
        $install = Get-Rb
        if (-not $install) { throw 'no rekordbox installed' }
        Assert-RbProcessVersion $sid $install
    }
}
switch ($Do) {
    'version' {
        $rb = Get-Rb
        if (-not $rb) { throw 'no rekordbox installed' }
        "REKORDBOX $($rb.Version)"
    }
    'desktop' {
        $r = Session $Step $Seconds $Argument
        Say $r
        if ($r -notmatch ('^\S+ ' + [regex]::Escape($Step) + ': OK(?: |$)')) { exit 1 }
    }
    'ensure' {
        $script:bad = 0
        function Row([bool]$ok, [string]$s) { Say ('{0} {1}' -f $(if ($ok) { 'OK  ' } else { 'BAD ' }), $s); if (-not $ok) { $script:bad++ } }
        # the session task, pointing at this kit (registered by the account itself: no elevation)
        $ps = Join-Path $Kit 'session-task.ps1'
        $targs = "--headless powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$ps`""
        # through schtasks.exe (kit-lib.ps1's Get-TaskState: the CIM cmdlets refuse an SSH logon)
        # 'Continue' here: an absent task's stderr under 'Stop' would end ensure (PS 5.1, a fresh account)
        $x = $null; $q = @(& { $ErrorActionPreference = 'Continue'; & schtasks.exe /Query /TN $TaskName /XML 2> $null })
        if (-not $LASTEXITCODE -and $q.Count) { try { $x = [xml]($q -join "`n") } catch { $x = $null } }
        # the actions as nodes (@() of an XmlElement would list its children instead)
        $ex = @(if ($x) { $x.Task.Actions.ChildNodes | Where-Object { $_.NodeType -eq 'Element' } })
        # PC session 1's task has no -WindowStyle Hidden (conhost --headless hides it anyway), and an
        # SSH logon may not replace it ("Access is denied"): either form is this kit's task
        $old = $targs -replace ' -WindowStyle Hidden', ''
        if ($ex.Count -eq 1 -and $ex[0].LocalName -eq 'Exec' -and $ex[0].Command -eq 'conhost.exe' -and $ex[0].Arguments -in $targs, $old) { Row $true "the task '$TaskName' runs this kit's session-task.ps1" }
        else {
            try {
                $esc = [Security.SecurityElement]::Escape($targs); $who = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value   # by SID: an SSH logon's USERDOMAIN may not resolve
                $def = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <Principals><Principal id="Author"><UserId>$who</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>
  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT5M</ExecutionTimeLimit><Enabled>true</Enabled></Settings>
  <Actions Context="Author"><Exec><Command>conhost.exe</Command><Arguments>$esc</Arguments></Exec></Actions>
</Task>
"@
                $f = Join-Path $B 'session-task.xml'
                [IO.File]::WriteAllText($f, $def, [Text.Encoding]::Unicode)
                $o = & schtasks.exe /Create /TN $TaskName /XML $f /F 2>&1
                if ($LASTEXITCODE) { throw "schtasks /Create: exit $LASTEXITCODE $o" }
                Row $true "the task '$TaskName' registered for this kit"
            } catch { Row $false "the task '$TaskName' could not be registered: $($_.Exception.Message)" }
        }
        $r = Session 'ping' 20; Row ($r -match '^\S+ ping: .+ session \d+$') "the session task answers: $r"
        try { Row $true (Ensure-RbspLoopback) } catch { Row $false $_.Exception.Message }
        if (Test-Path -LiteralPath $Mapping) {
            $m = Get-Content -LiteralPath $Mapping -Raw
            Row ($m -match '9040' -and $m -match '9041') "rekordbox's mapping 'Loopback B.midi.csv' (Browse Down 9040, Load 9041)"
            Row ($m -match '904A' -and $m -match '904B') "the mapping's Area Left/Right (904A, 904B: learned by the owner)"
        } else { Row $false "no rekordbox mapping 'Loopback B.midi.csv' (learn it: README)" }
        $rb = Get-Rb; Row ([bool]$rb) "rekordbox $(if ($rb) { $rb.Version } else { 'not installed' }); onnxruntime.dll: $(Get-OrtState)"
        . (Join-Path $PSScriptRoot 'pre-lib.ps1')
        $settings = try { Get-Content -LiteralPath (Join-Path $env:APPDATA 'Pioneer\rekordbox6\rekordbox3.settings') -Raw -ErrorAction Stop } catch { '' }
        $ok, $text = Get-RbAskAnalysisRow $settings; Row $ok $text
        $n = @(Rows).Count; Row ($n -gt 0) "the corpus: $n rows ($Corpus)"
        try { . (Join-Path $PSScriptRoot 'ocr-lib.ps1'); Row ($null -ne $script:RbspOcr) "Windows OCR: $(if ($script:RbspOcr) { $script:RbspOcr.RecognizerLanguage.LanguageTag } else { 'no engine for the profile languages' })" }
        catch { Row $false "Windows OCR: $($_.Exception.Message)" }
        Say "ensure: $script:bad not OK"
        if ($script:bad) { exit 1 }
    }
    { $_ -in 't2-facts', 't2-probe' } {
        . (Join-Path $PSScriptRoot 't2-round2.ps1')
        if ($Do -eq 't2-facts') { Get-T2Facts } else { Get-T2ProbeEvidence }
    }
    'ports' { Get-RbspPorts; 'midi console: ' + $(if (Find-MidiConsole) { Find-MidiConsole } else { 'not found' }) }
    'learn' {
        if ($Note -lt 0 -or $Note -gt 127 -or $Repeat -le 0) { 'learn: -Note 0-127 -Repeat SECS'; exit 2 }
        $h = Open-RbspOut
        try { $until = (Get-Date).AddSeconds($Repeat); $k = 0; while ((Get-Date) -lt $until) { Press-Note $h $Note; $k++; Start-Sleep -Seconds 2 }; Say "learn: note $Note pressed $k times over $Repeat s" }
        finally { [void][RbspMidi2]::midiOutClose($h) }
    }
    'windows' { Say (Session 'windows') }
    'winlist' {
        $reply = Session 'winlist' 25
        if ($reply -cnotmatch '^\S+ winlist: OK (\{.+\})$') { throw 'no result from the desktop window list' }
        $json = $Matches[1]
        $inventory = ConvertFrom-Json -InputObject $json
        $json
        foreach ($w in $inventory.Windows) {
            if (-not $w.Shot) { continue }
            if ($w.Shot -cnotmatch '^winlist-[a-f0-9]{32}-0x[0-9A-F]+\.png$') { throw 'invalid window crop name' }
            $path = Join-Path (Join-Path $B 'shots') $w.Shot
            Assert-FormatPath $path
            if (-not [IO.File]::Exists($path)) { throw 'window crop missing' }
            "WINLIST SHOT $path"
        }
    }
    'closeagent' { Say (Session 'closeagent'); Start-Sleep 1; Say (Session 'windows') }
    'ort' { Say "ort: $(Get-OrtState)" }
    'modules' {
        # M12's DirectML check: the GPU DLLs the running rekordbox has loaded (exit 3: no DirectML)
        $watch = Watch-Start 'modules' -Allow -Rekordbox
        if ($watch) { Say $watch; exit 1 }
        $p = @(Get-RbProcess $sid)
        if ($p.Count -ne 1) { Say 'modules: expected one rekordbox process in the desktop session'; exit 2 }
        $m = @($p[0].Modules | ForEach-Object { $_.ModuleName } | Where-Object { $_ -match '^(directml|d3d12|d3d10warp|onnxruntime|demucs3)' } | Sort-Object -Unique)
        Say ("modules: rekordbox session $($p[0].SessionId): " + ($m -join ', '))
        if (@($m | Where-Object { $_ -ieq 'DirectML.dll' }).Count) { Say 'modules: DirectML loaded' } else { Say 'modules: DirectML not loaded'; exit 3 }
    }
    'autoanalysis' {
        # Auto Analysis off through rekordbox's Preferences (owner decision 2026-11-19, the test
        # account only); rekordbox must be running; the setting is read back after its quit
        # (-Do autoanalysis-read)
        if (-not (Test-RbRunning $sid)) { Say 'autoanalysis: rekordbox is not running: start it first'; exit 2 }
        $r = Session 'front' 60; Say "autoanalysis: $r"
        if ($r -notmatch 'front: ok') { Say "autoanalysis: STOP: rekordbox not put in front ($r)"; exit 1 }
        $r = Session 'autoanalysisoff' 120; Say $r
        if ($r -notmatch '^\S+ autoanalysis: ok;') { Say "autoanalysis: STOP: $(($r -split ' autoanalysis: ', 2)[-1])"; exit 1 }
    }
    'autoanalysis-read' {
        . (Join-Path $PSScriptRoot 't2-round2.ps1')
        $v = Read-T2Setting (Get-Content -LiteralPath (Join-Path $env:APPDATA 'Pioneer\rekordbox6\rekordbox3.settings') -Raw) 'AutoAnalysis'
        Say "autoanalysis: rekordbox3.settings AutoAnalysis=$v"
        if ($v -ne '0') { exit 3 }
    }
    'dmlbench' {
        # the helper (the kit's timing build) on rows -From..-To, rekordbox closed: its CPU, then
        # --dml, each into its own scratch cache; then the --dml run's entries copied into the
        # account's Stems Cache, over the same keys (run hits then shows rekordbox serving them).
        # Exit 0: a DirectML session ran; 3: DirectML fell back to the CPU; 1: a run failed
        . (Join-Path $PSScriptRoot 'bench-lib.ps1')
        if (Test-RbRunning $sid) { Say 'dmlbench: rekordbox is running: quit it first (the timing runs with rekordbox closed)'; exit 2 }
        $bp = Bench-Paths; if (-not $bp) { exit 2 }
        $files = @(Bench-Files $From $To); if (-not $files.Count) { exit 2 }
        Say "dmlbench: rows $From..$To ($($files.Count) files); rekordbox $($bp.Rb); model $($bp.Model)"
        $sum = @{}
        foreach ($mode in 'cpu', 'dml') {
            $rc = Bench-Run $mode $files (Join-Path $BenchDir "cache-$mode") $bp ($Minutes * 60)
            $err = @(Get-Content -LiteralPath (Join-Path $BenchDir "$mode.err") -ErrorAction SilentlyContinue)
            $s = Get-BenchSummary $err; $sum[$mode] = $s
            $prepared = @(Get-Content -LiteralPath (Join-Path $BenchDir "$mode.out") -ErrorAction SilentlyContinue | Where-Object { $_ -match '"result":"prepared?"' }).Count
            Say "dmlbench: ${mode}: exit $rc; $prepared file(s) prepared; $($s.Chunks) chunks; ep $($s.Ep); ONNX Runtime per chunk mean $($s.OrtMs) ms (median $($s.OrtMedianMs)); model $($s.ModelMs) ms; whole chunk $($s.ChunkMs) ms"
            foreach ($l in $s.DirectML) { Say "dmlbench: ${mode}: $l" }
            if ($rc -ne 0 -or -not $s.Chunks) {
                Say "dmlbench: STOP: the $mode run failed (exit $rc, $($s.Chunks) chunks): $((@($err | Where-Object { $_ -notmatch '^rbsp-timing' }) | Select-Object -Last 3) -join ' | ')"; exit 1
            }
        }
        if ($sum.dml.OrtMs) { Say "dmlbench: DirectML against the CPU: $([math]::Round($sum.cpu.OrtMs / $sum.dml.OrtMs, 2))x per chunk (ONNX Runtime), $([math]::Round($sum.cpu.ChunkMs / $sum.dml.ChunkMs, 2))x whole chunk" }
        # the --dml run's entries into the account's cache (same model folder and keys)
        $src = Join-Path $BenchDir 'cache-dml'; $dst = Assert-SystemDrivePath (Join-Path $env:LOCALAPPDATA 'rbstemsplus\Cache')
        $new = 0; $same = 0; $diff = 0
        foreach ($f in @(Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object { $_.Name -match '^[0-9a-f]{64}\.(flac|desc)$' })) {
            $rel = $f.FullName.Substring($src.Length + 1); $to = Join-Path $dst $rel
            if (Test-Path -LiteralPath $to) { if ((Get-FileHash -LiteralPath $to).Hash -eq (Get-FileHash -LiteralPath $f.FullName).Hash) { $same++ } else { $diff++ } } else { $new++ }
            [void](New-Item -ItemType Directory -Force (Split-Path -Parent $to)); Copy-Item -LiteralPath $f.FullName -Destination $to -Force
        }
        Say "dmlbench: the --dml entries' files copied into $dst ($new new, $diff replaced with other bytes, $same identical)"
        if ($sum.dml.Ep -ne 'dml') { Say 'dmlbench: DirectML did not run the model: the CPU did (its lines above)'; exit 3 }
    }
    'dmlstall' {
        # best effort: deck 1 playing (it must hold a track: run hits --keep-open leaves one) while
        # the helper's --dml prepares another row (-From); deck 1's elapsed time read by OCR every 2 s
        # before, during and after; Play pressed again at the end (paused)
        . (Join-Path $PSScriptRoot 'bench-lib.ps1'); . (Join-Path $PSScriptRoot 'ocr-lib.ps1')
        if (-not (Test-RbRunning $sid)) { Say 'dmlstall: rekordbox is not running'; exit 2 }
        $bp = Bench-Paths; if (-not $bp) { exit 2 }
        $files = @(Bench-Files $From $From); if (-not $files.Count) { exit 2 }
        $r = Session 'front' 60; if ($r -notmatch 'front: ok') { Say "dmlstall: STOP: rekordbox not put in front ($r)"; exit 1 }
        $h = Open-RbspOut
        $samples = New-Object System.Collections.Generic.List[object]; $t0 = Get-Date
        function Sample([string]$phase) {
            $shot = Shot-Path (Session 'shot' 20)
            $txt = if ($shot) { Read-Deck1Time $shot } else { '' }
            $sec = Get-DeckSeconds $txt
            $samples.Add(@(((Get-Date) - $t0).TotalSeconds, $sec)); Say "dmlstall: $phase deck 1 '$($txt.Trim())' = $sec s"
            Drop $shot
        }
        try {
            Press $h 'Play'; Start-Sleep 2
            1..3 | ForEach-Object { Sample 'before'; Start-Sleep 2 }
            $before = Get-Advance $samples.ToArray()
            if (-not $before.Read -or $before.Stalls) { Say "dmlstall: deck 1 not seen advancing before the run (read $($before.Read) of $($before.Samples), stalls $($before.Stalls)): the check is not measurable" }
            $rc = Bench-Run 'stall' $files (Join-Path $BenchDir 'cache-stall') $bp ($Minutes * 60) { Sample 'during'; Start-Sleep 2 }
            1..2 | ForEach-Object { Sample 'after'; Start-Sleep 2 }
        } finally { Press $h 'Play'; [void][RbspMidi2]::midiOutClose($h) }
        $s = Get-BenchSummary @(Get-Content -LiteralPath (Join-Path $BenchDir 'stall.err') -ErrorAction SilentlyContinue)
        $a = Get-Advance $samples.ToArray()
        Say "dmlstall: the --dml run: exit $rc, $($s.Chunks) chunks, ep $($s.Ep), $($s.OrtMs) ms per chunk with rekordbox playing"
        Say "dmlstall: deck 1: $($a.Read) of $($a.Samples) readouts read; $($a.Stalls) stalled step(s); slowest step $($a.Worst)x real time"
        if ($rc -ne 0) { exit 1 }
    }
    'preconditions' {
        # every precondition of auto plan -Plan read at its start (coordinator 2026-11-19): all
        # failures in one list (pre-lib.ps1 judges them); exit 1 if any
        . (Join-Path $PSScriptRoot 'pre-lib.ps1'); . (Join-Path $PSScriptRoot 't2-round2.ps1')
        $f = @{}
        $ping = Session 'ping' 20; $f.Session = $ping -match '^\S+ ping: .+ session (\d+)$'
        $psid = if ($f.Session) { [int]$Matches[1] } else { $null }
        $f.Ort = Get-OrtState
        $f.Probe = Test-Path -LiteralPath 'C:\rbtest'
        try { $f.AutoAnalysis = Read-T2Setting (Get-Content -LiteralPath (Join-Path $env:APPDATA 'Pioneer\rekordbox6\rekordbox3.settings') -Raw) 'AutoAnalysis' } catch { $f.AutoAnalysis = '?' }
        $model = Join-Path $env:APPDATA 'Pioneer\rekordbox6\models\demucs3_model\hdemucs.onnx'
        $f.Model = if (-not (Test-Path -LiteralPath $model)) { 'missing' } elseif ((Get-Item -LiteralPath $model).Length -eq 308572524) { 'ours' } else { 'rekordbox' }   # watch.h WATCH_OUR_MODEL_SIZE
        $f.App = if ($f.Session) { Session 'appstate' 60 } else { '' }
        $f.Bench = if ($f.Session -and (Session 'benchstate' 30) -match 'benchstate: (\d+) running') { [int]$Matches[1] } else { -1 }
        $f.RbRunning = $f.Session -and (Test-RbRunning $psid)
        $f.SystemDrive = $env:SystemDrive
        $cl = @(Get-Content -LiteralPath $Bridge -ErrorAction SilentlyContinue | Select-String -Pattern 'bridge loaded \(.*; cache (.+?), enabled=' | Select-Object -Last 1)
        $f.CacheDir = if ($cl.Count) { $cl[0].Matches[0].Groups[1].Value } else { '' }
        # the TEST server our app downloads from (its log's last "downloads from"), reached over TCP in 3 s
        $f.Server = ''
        if ($Plan -in 'session2', 'prepare') {
            $u = @(Get-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'rbstemsplus\Logs\app.log') -ErrorAction SilentlyContinue | Select-String -Pattern 'downloads from (https?://[^\s/]+)' | Select-Object -Last 1)
            if ($u.Count) {
                $url = $u[0].Matches[0].Groups[1].Value; $uri = [Uri]$url; $c = New-Object Net.Sockets.TcpClient
                $up = try { $c.ConnectAsync($uri.Host, $uri.Port).Wait(3000) -and $c.Connected } catch { $false } finally { $c.Dispose() }
                $f.Server = "$(if ($up) { 'up' } else { 'down' }) $url"
            }
        }
        $lines = @(Get-PlanFindings $Plan $f)
        foreach ($l in $lines) { Say "preconditions: $l" }
        $verdict, $code = Get-PlanVerdict $lines
        Say $verdict; exit $code
    }
    'appswap' {
        # our app swapped for another TEST release (-Argument TLSBASE|HTTPBASE|SHA256 of its
        # bootstrap.ps1): its own complete uninstall, then that release's bootstrap; Stems Cache
        # must be uninstalled first (Pioneer's DLL) and rekordbox closed
        if ((Get-OrtState) -ne 'pioneer') { Say 'appswap: rekordbox''s onnxruntime.dll is not Pioneer''s: cache uninstall first'; exit 2 }
        if (Test-RbRunning) { Say 'appswap: rekordbox is running: quit it first'; exit 2 }
        if (Test-AppSwapCacheRootPresent) {
            Say 'appswap: STOP: the Stems Cache root is still present; clear it through the admin cache uninstall path first'
            exit 1
        }
        # already removed (plan N3's uninstall row, by our app's own uninstall): the install only
        if (-not (Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus') -Recurse -Filter 'RB Stems Plus.exe' -ErrorAction SilentlyContinue)) { Say 'appswap: our app is not installed: the install only' }
        else {
            $r = Session 'appuninstall' 420; Say $r
            if ($r -notmatch '^\S+ appuninstall: ok;') { Say "appswap: STOP: $(($r -split ' appuninstall: ', 2)[-1])"; exit 1 }
        }
        $r = Session 'appinstall' 420 $Argument; Say $r
        if ($r -notmatch '^\S+ appinstall: ok;') { Say "appswap: STOP: $(($r -split ' appinstall: ', 2)[-1])"; exit 1 }
    }
    'loc' {
        # one of plan N3's rows in our app's own UI, in the test account's session (loc-ui.ps1)
        if (-not $LocStep) { Say 'loc: needs -LocStep'; exit 2 }
        $r = "$(Session 'loc' 420 "$LocStep|$LocArg")"
        foreach ($l in @($r -split ' \|\| ')) { Say "loc: $l" }
        if ($r -notmatch "(^|\s)loc ${LocStep}: ok") { exit 1 }
    }
    'rowkeys' {
        # the kit's staged research helper (stage --helper: RBSP_RESEARCH) on selected corpus rows:
        # its --keys lines (cache keys cut to 12 characters) and allowlisted stderr diagnostics
        if ($researchKeyFlags.Count) {
            if ($researchKeyFlags.Count -ne 1) { throw 'formats: choose one codec opt-in for rowkeys' }
            $codec = if ($TestUnprovedAac) { 'AAC' } else { 'MP3' }
            if (-not $Formats -or $From -cnotlike "fmt-$($codec.ToLowerInvariant())-*" -or $To -or $RowList) {
                throw "formats: $codec diagnostics require one $codec row selected with -From"
            }
            $ping = Session 'ping' 20
            if ($ping -notmatch '^\S+ ping: .+ session (\d+)$' -or (Test-RbRunning ([int]$Matches[1]))) {
                throw "formats: $codec diagnostics require rekordbox closed in the test session"
            }
        }
        $bp = Bench-Paths -KeysOnly; if (-not $bp) { exit 2 }
        if ($Formats) {
            $formatLock = Open-FormatLock
            $selected = Get-FormatFiles (Join-Path $Corpus 'formats') -Verify
            $targetRows = @(Select-TargetRows -RequireOne -Names @($selected.Keys | Sort-Object))
        } else { $targetRows = @(Select-TargetRows -RequireOne) }
        if (-not $targetRows.Count) { exit 2 }
        $runDir = Join-Path (Join-Path $B 't2') ('rowkeys-' + [guid]::NewGuid().ToString('N'))
        $stderrPath = New-ResearchHelperLog $runDir
        foreach ($rowName in $targetRows) {
            $file = if ($Formats) { $selected[$rowName] } else { Row-File $rowName }
            if (-not $file) { exit 2 }
            $result = Invoke-ResearchKeys $bp $file $rowName
            if ($result.TimedOut) { throw "rowkeys: $rowName not done in 300 s" }
            $exit = $result.ExitCode
            $output = $result.Output
            Write-ResearchHelperDiagnostics $result.Stderr $rowName $stderrPath
            Say "rowkeys: $rowName exit $exit"
            foreach ($l in @($output -split "`r?`n" | Where-Object { $_ })) {
                $f = $l -split "`t"
                if ($f[0] -eq 'key' -and $f[1] -cmatch '^[0-9a-f]{64}$' -and ($f[2..4] -join ' ') -match '^\d+ \d+ [01]$') {
                    if (-not $researchKeyFlags.Count) { Say "rowkeys: key $($f[1].Substring(0, 12)) version $($f[2]) chunk $($f[3]) silent $($f[4])" }
                }
                elseif ($f[0] -eq 'skipped') {
                    Say "rowkeys: $rowName helper skipped input"
                    if ($researchKeyFlags.Count -and $f.Count -eq 3 -and $f[1] -and $f[2] -eq $file) { Say "rowkeys: refusal: $($f[1])" }
                }
                elseif ($f[0] -ne 'file') { Say "rowkeys: $rowName unexpected helper output"; exit 1 }
            }
            if ($exit -ne 0) { exit 1 }
        }
    }
    { $_ -in 'format-uidump', 'format-step' } {
        $task = if ($Do -eq 'format-step') { 'fmtstep' } else { 'fmtuidump' }
        $mode = if ($task -eq 'fmtstep') { $Argument } elseif ($Explore) { 'explore' } else { '' }
        $reply = Session $task 90 $mode
        $filePattern = '([1-9][0-9]{0,3}) (format-uidump-[a-f0-9]{32}\.jsonl)'
        $failure = $null
        if ($reply -cmatch "^\S+ ${task}: STOP: (formats: .+) \(line \d+\)$") {
            $failure = $Matches[1]
            $capture = if ($task -eq 'fmtstep') { 'step' } elseif ($Explore) { 'explore' }
            if (-not $capture -or $failure -cnotmatch "; $capture capture: $filePattern$") { throw $failure }
        } elseif ($reply -cnotmatch "^\S+ ${task}: OK $filePattern$") {
            throw 'formats: no result from the desktop UI dump task'
        }
        $count = [int]$Matches[1]
        $path = Join-Path $B $Matches[2]
        Assert-FormatPath $path
        $lines = @(Get-Content -LiteralPath $path -Encoding UTF8 -TotalCount 3001)
        if ($count -gt 3000 -or $lines.Count -ne $count) { throw 'formats: incorrect UI dump record count' }
        Say "${Do}: $count element(s) in $path"
        $lines
        if ($failure) { throw $failure }
        exit 0
    }
    'format-import' {
        $reply = Session 'fmtimport' 440
        if ($reply -cmatch '^\S+ fmtimport: STOP: (formats: .+)$') { throw $Matches[1] }
        if ($reply -cnotmatch '^\S+ fmtimport: OK(?:; refusal: (.+))?$') { throw 'formats: no result from the desktop import task' }
        if ($Matches[1]) {
            $refusal = $Matches[1]
            if ($refusal -cnotmatch (Get-FormatRefusalPattern)) { throw 'formats: invalid import-refusal result' }
            Say "formats: import refusal: $refusal"
        }
        exit 0
    }
    { $_ -in 'format-verify', 'format-reuse' } {
        $reply = Session $Do 200
        if ($reply -cmatch "^\S+ ${Do}: STOP: (formats: .+)$") { throw $Matches[1] }
        if ($Do -eq 'format-reuse' -and $reply -cmatch '^\S+ format-reuse: NONE$') {
            'FORMAT REUSE NONE'
            exit 4
        }
        if ($reply -cnotmatch "^\S+ ${Do}: OK$") { throw 'formats: no result from the desktop import verification' }
        $formatLock = Open-FormatLock
        $dir = Join-Path $Corpus 'formats'
        $state = Get-FormatImportState $dir
        $staged = Get-FormatFiles $dir -Verify
        $imported = @($state.playlist.corpusTracks)
        Say "formats: verified $($state.playlist.name) (ID $($state.playlist.id), parent=$($state.playlist.parent), order=$($state.playlist.order), $($imported.Count) corpus tracks)"
        foreach ($outcome in (Get-FormatImportOutcomes $staged $imported)) { Say "formats: $outcome" }
        exit 0
    }
    'format-hits' {
        if (-not (Test-RbRunning $sid)) {
            throw 'formats: cache checks require the verified import and running rekordbox'
        }
        if (Test-Path -LiteralPath $Stop) { throw 'formats: midi-stop is set' }
        if ((Get-OrtState) -notlike 'other *' -or (Test-Path -LiteralPath (Split-Path -Parent $ProbeRows))) {
            throw 'formats: cache checks require Stems Cache with the research probe removed'
        }
        $formatLock = Open-FormatLock
        . (Join-Path $PSScriptRoot 'format-run.ps1')
        . (Join-Path $PSScriptRoot 'ocr-lib.ps1')
        $next = ''
        $unmet = Invoke-FormatBatch (Get-FormatPlan) -Next ([ref]$next)
        if ($FormatPass) { "FORMAT CONTINUATION $next" }
        if ($unmet -gt 0) { exit 3 }
        exit 0
    }
    { $_ -in 'format-plan', 'format-run' } {
        if (-not $FormatRun -or ($Do -eq 'format-run' -and -not (Test-RbRunning $sid))) {
            throw 'formats: requires a run ID and rekordbox running for deck loads'
        }
        if (Test-Path -LiteralPath $Stop) { throw 'formats: midi-stop is set' }
        $formatLock = Open-FormatLock
        . (Join-Path $PSScriptRoot 'format-run.ps1')
        $runDir = Join-Path $B "t2\format-$FormatRun"
        Assert-FormatPath $runDir
        $batchPlan = Get-FormatPlan
        $planned = $batchPlan.Batch -join ','
        $seal = "$($batchPlan.Manifest) $($batchPlan.Evidence) $planned"
        $planFile = Join-Path $runDir 'plan.txt'
        if ($Do -eq 'format-plan') {
            if (Test-Path -LiteralPath $runDir) { throw 'formats: run ID already exists' }
            if (-not (Bench-Paths -KeysOnly)) { exit 2 }
            [void][IO.Directory]::CreateDirectory($runDir)
            [IO.File]::WriteAllText($planFile, $seal)
            Say "formats: subset $($batchPlan.Manifest.Substring(0,12)), batch $FormatBatch of $($batchPlan.BatchCount)"
            "FORMAT PLAN $planned"
        } else {
            Assert-FormatPath $planFile
            if ([IO.File]::ReadAllText($planFile) -cne $seal -or $planned -cne $RowList) { throw 'formats: subset or playlist changed since verification' }
            . (Join-Path $PSScriptRoot 'ocr-lib.ps1')
            $Bridge = Join-Path (Split-Path -Parent $ProbeRows) 'probe.log'
            Assert-FormatPath $Bridge
            Assert-FormatPath "$Bridge.1"
            Assert-FormatPath $ProbeRows
            $Quiet = 30
            $outcomes = Join-Path $runDir 'outcomes.txt'
            $keyFile = Join-Path $runDir 'rows.txt'
            $completeFile = Join-Path $runDir 'complete.txt'
            Assert-FormatPath $completeFile
            if ((Test-Path -LiteralPath $outcomes) -or (Test-Path -LiteralPath $keyFile) -or
                (Test-Path -LiteralPath $completeFile)) { throw 'formats: run ID already measured' }
            [IO.File]::WriteAllText($outcomes, '')
            [IO.File]::WriteAllText($keyFile, '')
            $next = ''
            [void](Invoke-FormatBatch $batchPlan $outcomes $keyFile -Next ([ref]$next))
            [IO.File]::WriteAllText($completeFile, $seal)
            if ($FormatPass) { "FORMAT CONTINUATION $next" }
        }
        exit 0
    }
    'keycompare' {
        if ($TestUnprovedAac -and -not $FormatRun) { throw 'formats: opt-in AAC comparison requires a format run' }
        if ($TestUnprovedMp3 -and -not $FormatRun) { throw 'formats: opt-in MP3 comparison requires a format run' }
        $helperMode = if ($researchKeyFlags.Count) { ($researchKeyFlags -replace '^--') -join '+' } else { 'proved' }
        $bp = Bench-Paths -KeysOnly; if (-not $bp) { exit 2 }
        $targetRows = @(Select-TargetRows -RequireOne)
        if (-not $targetRows.Count) { exit 2 }
        $rowsFile = $ProbeRows
        $t2Dir = Join-Path $B 't2'
        $outcomes = @{}; $staged = @{}
        if ($FormatRun) {
            $formatLock = Open-FormatLock
            $formatDir = Join-Path $Corpus 'formats'
            $staged = Get-FormatFiles $formatDir -Verify
            $t2Dir = Join-Path $t2Dir "format-$FormatRun"
            $rowsFile = Join-Path $t2Dir 'rows.txt'
            $outcomeFile = Join-Path $t2Dir 'outcomes.txt'
            $planFile = Join-Path $t2Dir 'plan.txt'
            $completeFile = Join-Path $t2Dir 'complete.txt'
            foreach ($f in $rowsFile, $outcomeFile, $planFile, $completeFile) { Assert-FormatPath $f }
            if (-not [IO.File]::Exists($completeFile)) { throw 'formats: the format run did not complete' }
            $planText = [IO.File]::ReadAllText($planFile)
            if (-not [string]::Equals([IO.File]::ReadAllText($completeFile), $planText, [StringComparison]::Ordinal)) {
                throw 'formats: the format run did not complete'
            }
            $seal = $planText -split ' '
            if ($seal.Count -ne 3 -or $seal[2] -cne ($targetRows -join ',') -or
                $seal[0] -ne (Get-FileHash -LiteralPath (Join-Path $formatDir 'SHA256SUMS')).Hash) {
                throw 'formats: comparison rows or subset differ from the verified run plan'
            }
            foreach ($l in @(Get-Content -LiteralPath $outcomeFile -ErrorAction SilentlyContinue)) {
                if ($l -cnotmatch '^(fmt-[a-zA-Z0-9._~-]+) (IMPORT_REFUSED|LOAD_UNCONFIRMED|LOAD_REFUSED|NAVIGATION_ERROR|LOADED_NO_KEYS|KEYS)$' -or
                    $targetRows -notcontains $Matches[1] -or $outcomes.ContainsKey($Matches[1])) {
                    throw 'formats: invalid row outcome record'
                }
                $outcome = if ($Matches[2] -eq 'LOAD_REFUSED') { 'LOAD_UNCONFIRMED' } else { $Matches[2] }
                $outcomes.Add($Matches[1], $outcome)
            }
        } elseif ($EvidenceRun) {
            $proof = Join-Path (Split-Path -Parent $ProbeRows) 'real\format-run.txt'
            Assert-FormatPath $proof
            Assert-FormatPath $ProbeRows
            if (-not [IO.File]::Exists($proof) -or [IO.File]::ReadAllText($proof) -cne $EvidenceRun) {
                throw 'keycompare: the probe evidence belongs to another run or has no run ID'
            }
            $t2Dir = Join-Path $t2Dir "compare-$EvidenceRun"
            Assert-FormatPath $t2Dir
            if (Test-Path -LiteralPath $t2Dir) { throw 'keycompare: evidence run ID already compared' }
            [void][IO.Directory]::CreateDirectory($t2Dir)
            $rowsFile = Join-Path $t2Dir 'rows.txt'
            [IO.File]::Copy($ProbeRows, $rowsFile)
        }
        $rbByRow = @{}
        $hashEvidence = [Collections.Generic.List[string]]::new()
        if ($FormatRun) {
            $hashPath = Join-Path $t2Dir 'chunk-hashes.tsv'
            Assert-FormatPath $hashPath
            $hashEvidence.Add("row`tsource`thash`tsilent")
        }
        foreach ($l in @(Get-Content -LiteralPath $rowsFile -ErrorAction SilentlyContinue)) {
            if ($l -match '^(\S+)\s+(\d+)\s+([0-9a-fA-F]{64})\s+(\d+)$' -and $targetRows -contains $Matches[1]) {
                $id = $Matches[1]
                if (-not $rbByRow.ContainsKey($id)) { $rbByRow[$id] = @{} }
                $hash = $Matches[3].ToLowerInvariant()
                $rbByRow[$id][$hash] = $true
                if ($FormatRun) { $hashEvidence.Add("$id`trekordbox`t$hash`t") }
            }
        }
        $reportPath = Join-Path $t2Dir 'keycompare.tsv'
        if ($FormatRun -or $EvidenceRun) { Assert-FormatPath $reportPath }
        $stderrPath = New-ResearchHelperLog $t2Dir
        $report = New-Object System.Collections.Generic.List[string]
        $columns = "row`tverdict`trb_keys`thelper_nonsilent`thelper_silent`tmatched`tmissing`textra`tfirst_ok`tlast_ok`tdetail"
        if ($FormatRun) { $columns += "`trekordbox_outcome`thelper_outcome`thelper_mode" }
        $report.Add($columns)
        $ok = 0; $bad = 0
        $formatResults = [Collections.Generic.List[object]]::new()
        $helperByRow = @{}
        foreach ($rowName in $targetRows) {
            if ($FormatRun) { Say "keycompare: reading helper evidence for $rowName" }
            $rbSet = if ($rbByRow.ContainsKey($rowName)) { $rbByRow[$rowName] } else { @{} }
            $rbOutcome = if ($FormatRun) { $outcomes[$rowName] } elseif ($rbSet.Count) { 'KEYS' } else { '' }
            $verdict = ''; $detail = ''; $matched = 0; $missingCount = 0; $extraCount = 0; $firstOk = $false; $lastOk = $false
            $hAll = @{}; $hNsSet = @{}; $hSilent = 0; $hNs = New-Object System.Collections.Generic.List[string]
            $rbIssue = if (-not $rbOutcome -or $rbOutcome -eq 'LOADED_NO_KEYS' -or ($rbOutcome -eq 'KEYS') -ne ($rbSet.Count -gt 0)) {
                'MISSING_EVIDENCE'
            } elseif ($rbOutcome -in 'IMPORT_REFUSED', 'LOAD_UNCONFIRMED', 'NAVIGATION_ERROR') {
                $rbOutcome
            }
            if ($rbIssue -and -not $FormatRun) {
                $verdict = $rbIssue
            } else {
                $file = if ($FormatRun) { $staged[$rowName] } else { Row-File $rowName }
                if (-not $file) { $verdict = 'MISSING_FIXTURE' }
                else {
                    $si = New-Object Diagnostics.ProcessStartInfo $bp.Exe
                    $si.Arguments = Get-ResearchKeysArguments $bp $file
                    $si.UseShellExecute = $false; $si.RedirectStandardOutput = $true; $si.RedirectStandardError = $true; $si.CreateNoWindow = $true
                    $keyClock = Start-KitTiming
                    $p = $null
                    try {
                        $p = [Diagnostics.Process]::Start($si)
                        $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
                        $exited = $p.WaitForExit(300000)
                        if ($exited) { Write-ResearchHelperDiagnostics $e.Result $rowName $stderrPath }
                        if (-not $exited) {
                            if ($FormatRun) { throw "formats: the research helper is still running after 300 s for $rowName" }
                            try { $p.Kill() } catch { }
                            $verdict = 'HELPER_TIMEOUT'
                        } elseif ($p.ExitCode -ne 0) {
                            $verdict = 'HELPER_ERROR'; $detail = "exit $($p.ExitCode)"
                        } else {
                            foreach ($l in @("$($o.Result)" -split "`r?`n" | Where-Object { $_ })) {
                                $f = $l -split "`t"
                                if ($f[0] -eq 'skipped') { $verdict = 'HELPER_REFUSED'; break }
                                if ($f[0] -eq 'file') { continue }
                                if ($f[0] -ne 'key' -or $f[1] -notmatch '^[0-9a-f]{64}$' -or $f[4] -notin '0', '1') {
                                    $verdict = 'HELPER_OUTPUT'; break
                                }
                                $hk = $f[1].ToLowerInvariant(); $hAll[$hk] = $true
                                if ($FormatRun) { $hashEvidence.Add("$rowName`thelper`t$hk`t$($f[4])") }
                                if ($f[4] -eq '0') { $hNs.Add($hk); $hNsSet[$hk] = $true } else { $hSilent++ }
                            }
                        }
                    } finally {
                        if ($p) { $p.Dispose() }
                        Write-KitTiming 'helper-keys' $keyClock $rowName
                    }
                }
            }
            if (-not $verdict) {
                $differences = Get-ChunkSetDifferences $rbSet $hNsSet $hAll
                $missingCount = $differences.Missing
                $extraCount = $differences.Extra
                $matched = $hNsSet.Count - $missingCount
                $firstOk = -not $hNs.Count -or $rbSet.ContainsKey($hNs[0])
                $lastOk = -not $hNs.Count -or $rbSet.ContainsKey($hNs[$hNs.Count - 1])
                $verdict = if (-not $hAll.Count) { 'HELPER_NO_EVIDENCE' }
                    elseif ($missingCount -or $extraCount) { 'MISMATCH' } else { 'MATCH' }
            }
            $helperOutcome = if ($verdict -in 'MATCH', 'MISMATCH') { 'KEYS' } else { $verdict }
            if ($rbIssue) { $verdict = $rbIssue }
            $counts = "$($rbSet.Count)`t$($hNsSet.Count)`t$hSilent`t$matched`t$missingCount`t$extraCount`t$firstOk`t$lastOk"
            $summary = "(rb $($rbSet.Count), helper $($hNsSet.Count)+${hSilent}s, missing $missingCount, extra $extraCount, first $firstOk, last $lastOk)"
            if ($FormatRun) {
                if ($helperOutcome -eq 'KEYS') { $helperByRow.Add($rowName, @{ Required = $hNsSet; Allowed = $hAll }) }
                $formatResults.Add([pscustomobject]@{
                    Row = $rowName; Verdict = $verdict; Detail = $detail; Counts = $counts; Summary = $summary
                    RbOutcome = $rbOutcome; HelperOutcome = $helperOutcome
                })
                continue
            }
            if ($verdict -eq 'MATCH') { $ok++ } else { $bad++ }
            $report.Add("$rowName`t$verdict`t$counts`t$detail")
            Say "keycompare: $rowName : $verdict $summary $detail"
        }
        foreach ($result in $formatResults) {
            $rowName = $result.Row
            $rbSet = $rbByRow[$rowName]
            $verdict = $result.Verdict
            $detail = $result.Detail
            if ($rbSet.Count) {
                $otherMatches = @(foreach ($other in $targetRows) {
                    if ($other -eq $rowName -or -not $helperByRow.ContainsKey($other)) { continue }
                    $helper = $helperByRow[$other]
                    $differences = Get-ChunkSetDifferences $rbSet $helper.Required $helper.Allowed
                    if (-not $differences.Missing -and -not $differences.Extra) { $other }
                })
                if ($otherMatches.Count) {
                    $verdict = 'WRONG_TRACK'
                    $detail = 'matched helper evidence for ' + ($otherMatches -join ',')
                }
            }
            if ($verdict -eq 'MATCH') { $ok++ } else { $bad++ }
            $report.Add("$rowName`t$verdict`t$($result.Counts)`t$detail`t$($result.RbOutcome)`t$($result.HelperOutcome)`t$helperMode")
            Say "keycompare: $rowName : $verdict $($result.Summary) $detail"
        }
        if ($FormatRun) { [IO.File]::WriteAllLines($hashPath, $hashEvidence) }
        [IO.File]::WriteAllLines($reportPath, $report)
        if (Test-Path -LiteralPath $rowsFile -PathType Leaf) {
            $evId = (Get-FileHash -LiteralPath $rowsFile -Algorithm SHA256).Hash.ToLowerInvariant().Substring(0, 12)
            Say "keycompare: evidence $evId"
        }
        Say "keycompare: $ok OK, $bad not OK"
        if ($FormatRun) { "FORMAT REPORT $reportPath" }
        if ($bad -gt 0) { exit 3 }
    }
    'keptinfo' {
        # read only: our app's kept folder (names, sizes; a .kept file's header lines, never its key
        # lines; a .manifest's line count), then the Status tab as UI Automation sees it, then
        # app.log's last lines
        $kd = Join-Path $env:LOCALAPPDATA 'rbstemsplus\kept'
        if (-not (Test-Path -LiteralPath $kd)) { Say "keptinfo: no folder $kd" }
        else {
            $fs = @(Get-ChildItem -LiteralPath $kd -Force -ErrorAction SilentlyContinue)
            Say "keptinfo: $kd holds $($fs.Count) item(s)"
            foreach ($f in $fs) {
                $extra = ''
                if ($f.Name -like '*.kept') { $h = @(Get-Content -LiteralPath $f.FullName -TotalCount 2 -ErrorAction SilentlyContinue); $extra = "; header: $($h -join ' / ')" }
                elseif ($f.Name -like '*.manifest') { $extra = "; $(@(Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue | Where-Object { $_ -like 'file *' }).Count) 'file' line(s)" }
                Say "keptinfo:   $($f.Name) $(if ($f.PSIsContainer) { '<dir>' } else { "$($f.Length) bytes" }) $($f.LastWriteTime.ToString('s'))$extra"
            }
        }
        # the Status tab in front (statusdump selects it), then the whole tree written by the proven
        # uiatree dump; its list lines shown
        Say ('keptinfo: ' + (Session 'statusdump' 120))
        Say ('keptinfo: ' + (Session 'uiatree' 180 'RB Stems Plus'))
        $tf = Join-Path $B 'uiatree-RB-Stems-Plus.txt'
        $tl = @(Get-Content -LiteralPath $tf -ErrorAction SilentlyContinue)
        Say "keptinfo: uiatree $($tl.Count) line(s)"
        foreach ($l in @($tl | Where-Object { $_ -match 'List|DataGrid|Tab|Stop Keeping|Status|Prepared' } | Select-Object -First 40)) { Say "keptinfo: tree: $($l.Substring(0, [math]::Min(160, $l.Length)))" }
        $log = Join-Path $env:LOCALAPPDATA 'rbstemsplus\Logs\app.log'
        foreach ($l in @(Get-Content -LiteralPath $log -Tail 8 -ErrorAction SilentlyContinue)) { Say "keptinfo: app.log: $l" }
    }
    'stopkeep' {
        # the kept set -Playlist dropped through our app's own Stop Keeping… (Status tab)
        $r = Session 'stopkeep' 150 $Playlist; Say $r
        if ($r -notmatch 'stopkeep: ok;') { Say ('stopkeep: STOP: ' + (Session 'shot')); exit 1 }
    }
    'library-check' {
        $reply = Session 'library-check' 200
        if ($reply -cmatch '^\S+ library-check: STOP: (formats: .+)$') { throw $Matches[1] }
        if ($reply -cnotmatch '^\S+ library-check: OK (library-check-[a-f0-9]{32}\.json)$') {
            throw 'formats: no result from the desktop library query'
        }
        $path = Join-Path $B $Matches[1]
        Assert-FormatPath $path
        foreach ($entry in (ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($path)))) {
            if (-not $entry) { continue }
            $name = $entry.name -replace '[\x00-\x1f\x7f]', ' '
            Say "library-check: $($entry.id) $name | parent=$($entry.parent) order=$($entry.order) | $($entry.corpusTracks.Count) corpus tracks"
        }
        "LIBRARY CHECK $path"
        exit 0
    }
    'playlists' {
        # the installed helper's --playlists on its own, as the core runs it (no other argument; the
        # clean environment of win/c/helper_proc_win.c: SystemRoot, TEMP and TMP only), then once
        # more with this account's whole environment: exit code, time, stdout lines, whether the
        # test playlist is listed, and stderr in short. Prints no playlist names but -Playlist's
        . (Join-Path $PSScriptRoot 'format-library.ps1')
        $exe = Find-InstalledPrepare
        if (-not $exe) { Say 'playlists: installed rbsp-prepare.exe is missing'; exit 2 }
        Say "playlists: $($exe.FullName); rekordbox $(if (Test-RbRunning) { 'running' } else { 'closed' })"
        $bad = 0
        foreach ($clean in $true, $false) {
            $si = New-Object Diagnostics.ProcessStartInfo $exe.FullName, '--playlists'
            $si.UseShellExecute = $false; $si.RedirectStandardOutput = $true; $si.RedirectStandardError = $true; $si.CreateNoWindow = $true
            if ($clean) {
                $keep = @{ SystemRoot = $env:SystemRoot; TEMP = (Join-Path $env:LOCALAPPDATA 'Temp'); TMP = (Join-Path $env:LOCALAPPDATA 'Temp') }
                $si.EnvironmentVariables.Clear(); foreach ($k in $keep.Keys) { $si.EnvironmentVariables[$k] = $keep[$k] }
            }
            $t = [Diagnostics.Stopwatch]::StartNew(); $p = [Diagnostics.Process]::Start($si)
            $out = $p.StandardOutput.ReadToEndAsync(); $err = $p.StandardError.ReadToEndAsync()
            $done = $p.WaitForExit(90000)
            if (-not $done) { try { $p.Kill() } catch { } ; [void]$p.WaitForExit(5000) }
            $o = @("$($out.Result)" -split "`r?`n" | Where-Object { $_ })
            $e = ("$($err.Result)" -replace '\s+', ' ').Trim()
            $listed = @($o | Where-Object { $_ -match ('"name":\s*"' + [regex]::Escape($Playlist) + '"') }).Count
            $code = if ($done) { $p.ExitCode } else { 'none (stopped after 90 s)' }
            Say "playlists: $(if ($clean) { 'clean environment' } else { 'whole environment' }): exit $code in $([math]::Round($t.Elapsed.TotalSeconds, 1)) s; $($o.Count) stdout line(s); '$Playlist' listed $listed; stderr: $(if ($e) { $e.Substring(0, [math]::Min(300, $e.Length)) } else { '(none)' })"
            if (-not $done -or $p.ExitCode -ne 0 -or -not $listed) { $bad++ }
        }
        if ($bad) { exit 3 }
    }
    'benchstop' {
        # the kit's own bench helpers stopped; then each run's stderr in short (a stopped run's too)
        . (Join-Path $PSScriptRoot 'bench-lib.ps1')
        $r = Session 'benchstop' 60; Say $r
        foreach ($e in @(Get-ChildItem -LiteralPath $BenchDir -Filter '*.err' -File -ErrorAction SilentlyContinue)) {
            $l = @(Get-Content -LiteralPath $e.FullName -ErrorAction SilentlyContinue); $s = Get-BenchSummary $l
            $x = @(Get-Content -LiteralPath ([IO.Path]::ChangeExtension($e.FullName, '.exit')) -ErrorAction SilentlyContinue)
            Say "benchstop: $($e.Name): exit $(if ($x.Count) { $x[0] } else { 'none' }); $($s.Chunks) chunks; ep $($s.Ep); $((@($l | Where-Object { $_ -notmatch '^rbsp-timing' }) | Select-Object -Last 3) -join ' | ')"
        }
        if ($r -notmatch 'benchstop: \d+ stopped, 0 left') { exit 1 }
    }
    'appmin' { $r = Session 'appmin' 60; Say $r; if ($r -notmatch 'appmin: minimized True') { exit 1 } }
    'appcheck' {
        # the black-window repro's check: exit 3 when the window is black (90 % or more) or UI
        # Automation sees no tab; 1 when it can't be checked
        $r = Session 'appcheck' 90; Say $r
        # a TEST app's own repaint trace so far (session-task.ps1 sets RBSP_TEST_SESSION_TRACE)
        $tr = @(Get-Content -LiteralPath (Join-Path $B 'app-session-trace.txt') -ErrorAction SilentlyContinue)
        if ($tr.Count) {
            $n = { param($p) @($tr | Where-Object { $_ -match $p }).Count }
            Say "appcheck: the app's repaint trace: $(& $n 'repaint-nudge') nudges; looks drawn $(& $n 'repaint-look drawn'), black $(& $n 'repaint-look black'), unknown $(& $n 'repaint-look unknown'); software fallback $(& $n 'repaint-software')"
        }
        $al = @(Get-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'rbstemsplus\Logs\app.log') -ErrorAction SilentlyContinue)
        Say "appcheck: app.log 'session render target reset' lines: $(@($al | Where-Object { $_ -match 'session render target reset' }).Count)"
        if ($r -notmatch 'appcheck: title .*; tabs (\d+):.*; black ([\d.]+) of the window') { exit 1 }
        if ([int]$Matches[1] -eq 0 -or [double]$Matches[2] -ge 0.9) { exit 3 }
        if ($r -match '; ink ([\d.]+);' -and [double]$Matches[1] -lt 0.005) { exit 4 }   # white: nothing drawn below the title bar
    }
    'appexit' {
        $r = Session 'appexit' 60; Say $r
        if ($r -notmatch '^\S+ appexit: ok;') { Say "appexit: STOP: $(($r -split ' appexit: ', 2)[-1])"; exit 1 }
    }
    'appopen' {
        $r = Session 'app' 90; Say "appopen: $r"
        if ($r -notmatch 'app: running') { Say "appopen: STOP: our app did not start ($r)"; exit 1 }
    }
    'logoff' { if (Test-RbRunning $sid) { Say 'logoff: rekordbox is running: quit it first'; exit 2 }; $r = Session 'logoff' 20; Say $r; if ($r -notmatch 'logoff: signing out') { exit 1 } }
    'press' {
        if (-not $Keys) { 'press: -Keys "Down x3,Load"'; exit 2 }
        $h = Open-RbspOut
        try { foreach ($p in $Keys -split ',') { $w, $n = $p.Trim() -split 'x'; if (-not $n) { $n = 1 }; Press $h $w.Trim() ([int]$n) 250; Start-Sleep -Milliseconds 400 } }
        finally { [void][RbspMidi2]::midiOutClose($h) }
        Say ("press ${Keys}: " + (Session 'shot'))
    }
    { $_ -in 'start', 'shot' } {
        if ($Do -eq 'start' -and (Test-RbRunning $sid)) {
            $s = Watch-Start 'start' -Allow -Rekordbox; if ($s) { Say $s; exit 1 }
            Say 'start: rekordbox already running'; break
        }
        if ($Do -eq 'start') {
            Remove-Item -LiteralPath $WatchBase -ErrorAction SilentlyContinue   # a new rekordbox: run's baseline is stale
            Say "start: $(Ensure-RbspLoopback)"   # the pair goes with a MIDI service restart
        }
        $hin = if ($Do -eq 'start') { Open-RbspIn } else { $null }
        $r = Session $Do; Say "${Do}: $r"
        if ($Do -eq 'start') {
            # rekordbox listens only after it has sent its first MIDI (its lights); its browser is
            # built a few seconds later: wait for both
            $t = Get-Date; $probed = $null
            try {
                if ($r -notmatch '^\S+ (started rekordbox [0-9.]+(?:; rbtest InputDirector: \d+ closed normally)?(?:; RB Stems Plus watcher: pressed Remind Me Later)?|already running)$') { throw "session task did not start rekordbox ($r)" }
                while ([RbspMidi2]::First -eq [DateTime]::MinValue -and ((Get-Date) - $t).TotalSeconds -lt 120) {
                    # The splash may still be up. Check for an update offer while waiting for MIDI.
                    $s = Watch-Start 'start' -Allow -News -BeforeMidi; if ($s) { Say $s; exit 1 }
                    Start-Sleep -Milliseconds 500
                    # no lights after 30 s (both decks empty send none, the PC 2026-11-19): Browse Down
                    # then Up (no net move) every 10 s; rekordbox's answer counts as its first MIDI
                    if (((Get-Date) - $t).TotalSeconds -ge 30 -and (-not $probed -or ((Get-Date) - $probed).TotalSeconds -ge 10)) {
                        if (-not $probed) { Say 'start: no lights from rekordbox in 30 s: Browse Down and Up to see whether it answers' }
                        $probed = Get-Date; $ho = Open-RbspOut
                        try { Press $ho 'Down'; Start-Sleep -Milliseconds 300; Press $ho 'Up' } finally { [void][RbspMidi2]::midiOutClose($ho) }
                    }
                }
            } finally {
                [void][RbspMidi2]::midiInStop($hin); [void][RbspMidi2]::midiInClose($hin)
            }
            if ([RbspMidi2]::First -eq [DateTime]::MinValue) { Say ('start: STOP: no MIDI from rekordbox in 120 s (a dialog? the mapping?); ' + (Session 'windows') + '; ' + (Session 'shot')); exit 1 }
            $midiSeconds = ([RbspMidi2]::First - $t).TotalSeconds
            if ($Formats) {
                Say ("start: rekordbox's first MIDI after {0:N0} s; checking its browser" -f $midiSeconds)
                Wait-FormatBrowser
            } else {
                Say ("start: rekordbox's first MIDI after {0:N0} s; waiting 15 s for its browser" -f $midiSeconds)
                Start-Sleep -Seconds 15
            }
            # rekordboxAgent's news window: seen and shot first, closed only when it is the only one
            $s = Watch-Start 'start' -Allow -Rekordbox -News -RedactTitles:$Formats; if ($s) { Say $s; exit 1 }
            Say ('start: ' + (Session 'windows')); Say ('start: ' + (Session 'shot')); Say (Session 'closeagent')
            Say 'start: ready'
        }
    }
    'quit' {
        if (-not (Test-RbRunning $sid)) { Say 'quit: rekordbox not running'; Remove-Item -LiteralPath $WatchBase -ErrorAction SilentlyContinue; break }
        $windows = WinList -AllowPicker
        if ($windows -cmatch '^rekordbox \| (?:Open|Import Playlist|Import Track|Save As|Export a playlist to a file for music apps \(\*\.m3u8\)) \| 0x[0-9A-F]+$') {
            $r = Session 'fmtcancelpicker' 60
            Assert-RbDialogReply $r
            if ($r -cnotmatch '^\S+ fmtcancelpicker: (?:cancelled|no picker)$') { Say "quit: STOP: verified picker cancellation failed ($r)"; exit 1 }
            Say "quit: $r"
        }
        # Any remaining dialog is still handled by the normal quit guard.
        $s = Watch-Quit -RedactTitles:$Formats; if ($s) { Say $s; exit 1 }
        $hin = Open-RbspIn; $h = Open-RbspOut
        # deck 1's PLAY light: a loaded, paused deck blinks it every 0.5 s; playing is a light that
        # stays on. Watch it for 2 s.
        Start-Sleep -Seconds 2
        $play = @([RbspMidi2]::Got.ToArray() | Where-Object { ($_.Item2 -band 0xF0) -in 0x80, 0x90 -and $_.Item3 -eq $RbspNote.Play })
        # the windows again before Play: a dialog may have come up during the sample
        $s = Watch-Quit -RedactTitles:$Formats
        if ($s) { [void][RbspMidi2]::midiOutClose($h); [void][RbspMidi2]::midiInStop($hin); [void][RbspMidi2]::midiInClose($hin); Say $s; exit 1 }
        if ($play.Count -le 1 -and [RbspMidi2]::Last[$RbspNote.Play] -gt 0) { Say 'quit: deck 1 is playing: PlayPause first'; Press $h 'Play'; Start-Sleep 1 }
        else { Say "quit: deck 1 not playing ($($play.Count) PLAY light changes in 2 s)" }
        [void][RbspMidi2]::midiOutClose($h); [void][RbspMidi2]::midiInStop($hin); [void][RbspMidi2]::midiInClose($hin)
        $s = Watch-Quit -RedactTitles:$Formats; if ($s) { Say $s; exit 1 }
        $r = Session 'quit'
        Assert-RbDialogReply $r
        Say "quit: $r"
        if ($r -match '^\S+ error:') { throw "session task refused to close rekordbox ($r)" }
        if ($r -cnotmatch $RbCloseAcceptedPattern) { Say "quit: STOP: rekordbox did not accept the close request ($r)"; exit 1 }
        $t = Get-Date
        while (Test-RbRunning $sid) {
            if ($null -eq (WinList)) { Say (Watch-Failed 'quit'); exit 1 }
            if (((Get-Date) - $t).TotalSeconds -ge 90) { break }
            Start-Sleep 1
        }
        if (Test-RbRunning $sid) {
            $reply = Session 'quitdump'
            $dump = if ($reply -cmatch '^\S+ quitdump: OK (format-uidump-[a-f0-9]{32}\.jsonl)$') {
                'redacted UI dump ' + $Matches[1]
            } else { 'redacted UI dump unavailable' }
            Say "quit: STOP: rekordbox still running after 90 s; $dump"
            exit 1
        }
        Remove-Item -LiteralPath $WatchBase -ErrorAction SilentlyContinue
        Say ('quit: rekordbox closed after {0:N0} s' -f ((Get-Date) - $t).TotalSeconds)
    }
    'top' {
        $h = Open-RbspOut
        try { Press $h 'SortSong'; Start-Sleep 1; Press $h 'Up' 45 120 } finally { [void][RbspMidi2]::midiOutClose($h) }
        Say ('top: sorted, row 1; ' + (Session 'shot'))
    }
    'watchtest' {
        # the dialog watch's own test, with nothing of rekordbox's touched: the test window must be
        # seen as new (the same STOP line a run writes, which the Mac turns into a notification),
        # then closed by the test
        $s = Watch-Start 'watchtest'; if ($s) { Say $s; exit 1 }
        Say ('watchtest: windows at the start: ' + ($script:Base -join '; '))
        Say ('watchtest: ' + (Session 'testwindow')); Start-Sleep 3
        $s = Watch-Stop 'watchtest'; $seen = $s -like '*: STOP: new window(s): * (watch test);*'
        if ($s) { Say $s }
        Say ('watchtest: ' + (Session 'closetest')); Start-Sleep 2
        $now = WinList
        if ($null -eq $now) { Say 'watchtest: FAIL: the window list failed after the test window was closed'; exit 1 }
        $left = @($now | Where-Object { $_ -like "* | $TestTitle* | *" }).Count
        if ($seen -and -not $left) { Say 'watchtest: PASS: the watch saw the test window, and it is closed' }
        else { Say "watchtest: FAIL: the test window seen: $seen, still open: $left"; exit 1 }
    }
    'prepare' {
        Say "prepare: rekordbox $(if (Test-RbRunning) { 'running' } else { 'closed' }); playlist '$Playlist', up to $Minutes min"
        # a fresh start of our app first, by its own Exit (owner decision 2026-11-19), then its
        # window up, so that it is in the baseline
        $r = Session 'appexit' 60; Say "prepare: $r"
        if ($r -notmatch '^\S+ appexit: ok;') { Say "prepare: STOP: our app did not exit ($r)"; exit 1 }
        $r = Session 'app' 90; Say "prepare: $r"
        if ($r -notmatch 'app: running') { Say ('prepare: STOP: our app has no window; ' + (Session 'shot')); exit 1 }
        $s = Watch-Start 'prepare' -Allow -RedactTitles:$Formats; if ($s) { Say $s; exit 1 }
        Say ('prepare: windows at the start: ' + ($script:Base -join '; '))
        $r = Session 'prepare' 180 $Playlist; Say $r
        if ($r -notmatch 'clicked') {
            if ($Formats) { Say 'prepare: STOP: the imported format playlist could not be prepared'; exit 1 }
            # the shot first (the picture as it was), then whether the UI thread answers, the helpers, a dump
            $f = Session 'shot'; Say ('prepare: ' + (Session 'hangcheck' 180))
            Say "prepare: STOP: Prepare could not be started; $f"; exit 1
        }
        $t = Get-Date; $idle = 0; $seenRun = $false; $st = ''; $said = 0; $paused = $false
        # Prepare stopped by its Stop button, then idle within 60 s (a timeout or midi-stop; a dialog
        # STOP leaves everything as it is)
        # the cache folder the bridge last named and the kept sets' folder
        function Get-PrepDirs {
            $cl = @(Get-Content -LiteralPath $Bridge -ErrorAction SilentlyContinue | Select-String -Pattern 'bridge loaded \(.*; cache (.+?), enabled=' | Select-Object -Last 1)
            @((Join-Path $env:LOCALAPPDATA 'rbstemsplus\kept')) + @(if ($cl.Count) { $cl[0].Matches[0].Groups[1].Value })
        }
        # Pause; the helper's threads all suspended and the stems written unchanged for 20 s; Resume
        function Pause-Test {
            function Snap { (@(Get-ChildItem -LiteralPath (Get-PrepDirs) -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name)=$($_.Length)" }) | Sort-Object) -join ';' }
            function Helpers { @(Get-Process -Name 'rbsp-prepare' -ErrorAction SilentlyContinue) }
            Say ('prepare: ' + (Session 'prepbtn' 30 'Pause')); Start-Sleep 3
            $h = Helpers; $sus = @($h | ForEach-Object { $p = $_; $p.Refresh(); @($p.Threads | Where-Object { -not ($_.ThreadState -eq 'Wait' -and $_.WaitReason -eq 'Suspended') }).Count })
            Say "prepare: paused: $($h.Count) helper(s); threads not suspended: $($sus -join ',')"
            $a = Snap; $ra = Session 'prepstate' 30; Start-Sleep 20; $b = Snap; $rb = Session 'prepstate' 30
            Say "prepare: 20 s paused: stems $(if ($a -eq $b) { 'unchanged' } else { 'CHANGED' }); rows $(if ($ra -eq $rb) { 'unchanged' } else { 'changed' })"
            Say ('prepare: ' + (Session 'prepbtn' 30 'Resume')); Start-Sleep 30
            $c = Snap; Say "prepare: 30 s after Resume: stems $(if ($c -ne $b) { 'moving' } else { 'not moved' }); helpers $((Helpers).Count)"
        }
        function Stop-Prepare {
            $s = Watch-Stop 'prepare' -RedactTitles:$Formats; if ($s) { Say $s; exit 1 }
            Say ('prepare: ' + (Session 'prepstop' 30)); $u = Get-Date
            do {
                Start-Sleep 5
                $left = 60 - ((Get-Date) - $u).TotalSeconds
                $x = Session 'prepstate' ([math]::Max(5, $left))
            } while ($x -notmatch 'prepstate: idle' -and ((Get-Date) - $u).TotalSeconds -lt 60)
            if ($x -match 'prepstate: idle') { Say 'prepare: stopped, idle' } else { Say "prepare: BAD: not idle 60 s after Stop: $x" }
        }
        while (((Get-Date) - $t).TotalMinutes -lt $Minutes) {
            Start-Sleep -Seconds 20
            if (Test-Path -LiteralPath $Stop) {
                Say 'prepare: STOP: midi-stop'
                Stop-Prepare
                exit 1
            }
            $s = Watch-Stop 'prepare' -RedactTitles:$Formats; if ($s) { Say $s; exit 1 }
            $st = Session 'prepstate' 60
            if ($st -match 'prepstate: running') { $seenRun = $true; $idle = 0 } elseif ($st -match 'prepstate: idle') { $idle++ } else { Say $st }
            if ($PauseTest -and -not $paused -and ((Get-Date) - $t).TotalSeconds -ge 60) {
                $paused = $true; Pause-Test; Say 'prepare: Stop after the pause test'; Stop-Prepare
                $part = @(Get-ChildItem -LiteralPath (Get-PrepDirs) -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch '^[0-9a-f]{64}\.(flac|desc)$' -and $_.Name -notmatch '\.(json|txt|log|kept|manifest|sha256)$' })
                Say "prepare: after Stop: $($part.Count) partial file(s) in the cache and kept folders$(if ($part.Count) { ': ' + (($part | Select-Object -First 5 | ForEach-Object Name) -join ', ') })"
                if ($part.Count) { exit 1 }; exit 0
            }
            # a line a minute: the Mac stops a step silent for 10 min (PC 2026-11-20: a 36-track
            # Prepare outlived it)
            $m = [int][math]::Floor(((Get-Date) - $t).TotalMinutes); if ($m -gt $said) { $said = $m; Say "prepare: $m min; $(($st -split ' prepstate: ', 2)[-1])" }
            # done: idle twice in a row, after it was seen running (or a minute in, for a quick one)
            if ($idle -ge 2 -and ($seenRun -or ((Get-Date) - $t).TotalSeconds -gt 60)) { break }
        }
        Say $st
        if ($idle -lt 2) {
            $reason = "prepare: STOP: still running after $Minutes min"
            if (-not $Formats) { $reason += '; ' + (Session 'shot') }
            Say $reason
            Stop-Prepare
            exit 1
        }
        $done = 'prepare: done after {0:N1} min' -f ((Get-Date) - $t).TotalMinutes
        if (-not $Formats) { $done += '; ' + (Session 'shot') }
        Say $done
    }
    'uninstallcache' {
        if (Test-RbRunning) { Say 'uninstallcache: rekordbox is running: quit it first'; exit 2 }
        Say (Session 'uninstallcache' 120)
        $t = Get-Date; do { Start-Sleep 3; $s = Get-OrtState } while ($s -ne 'pioneer' -and ((Get-Date) - $t).TotalSeconds -lt 120)
        if ($s -eq 'pioneer') { Say "uninstallcache: rekordbox's onnxruntime.dll is Pioneer's again" }
        else { Say "uninstallcache: STOP: rekordbox's onnxruntime.dll is $s (a UAC prompt waiting for the owner?); $(Session 'shot')"; exit 1 }
    }
    'bundle' {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $d = Join-Path $B "collect-$stamp"; [void][IO.Directory]::CreateDirectory((Join-Path $d shots)); [void][IO.Directory]::CreateDirectory((Join-Path $d applogs))
        foreach ($f in $Log, (Join-Path $B 'task-out.txt'), $Mapping) { if (Test-Path -LiteralPath $f) { Copy-Item -LiteralPath $f -Destination $d } }
        $t2 = Join-Path $B 't2'
        if (Test-Path -LiteralPath $t2) { Copy-Item -LiteralPath $t2 -Destination $d -Recurse }
        $hang = Join-Path $B 'hang'   # hangcheck's dumps: moved into the bundle (each is taken once)
        if (Test-Path -LiteralPath $hang) { Get-ChildItem -LiteralPath $hang -File | ForEach-Object { [void][IO.Directory]::CreateDirectory((Join-Path $d hang)); Move-Item -LiteralPath $_.FullName -Destination (Join-Path $d hang) } }
        Get-ChildItem -LiteralPath $B -Filter 'm12-*-bootstrap.log*' -File | Copy-Item -Destination $d
        # copies first: the app's logs may be open for writing
        Get-ChildItem -LiteralPath $AppLogs -File -ErrorAction SilentlyContinue | ForEach-Object {
            try { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $d applogs) } catch { Say "bundle: BAD: $($_.Exception.Message)" } }
        $shots = @(Get-ChildItem -LiteralPath (Join-Path $B 'shots') -File -ErrorAction SilentlyContinue)
        foreach ($s in $shots) { Copy-Item -LiteralPath $s.FullName -Destination (Join-Path $d shots) }
        # entry names with '/' (Windows PowerShell's Compress-Archive writes '\'); the screenshots stay
        # on the PC until the Mac has unpacked the zip (-Do bundled)
        Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
        $z = [IO.Compression.ZipFile]::Open("$d.zip", 'Create')
        try { foreach ($f in Get-ChildItem -LiteralPath $d -Recurse -File) { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $f.FullName, $f.FullName.Substring($d.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/')) } }
        finally { $z.Dispose() }
        Remove-Item -LiteralPath $d -Recurse -Force
        "BUNDLE $d.zip"
    }
    'bundled' {
        if ($Keys -notmatch '^collect-\d{8}-\d{6}\.zip$') { 'bundled: -Keys collect-<time>.zip'; exit 2 }
        Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
        $z = [IO.Compression.ZipFile]::OpenRead((Join-Path $B $Keys)); $n = 0
        try { foreach ($e in $z.Entries) { if ($e.FullName -match '^shots/([^/]+)$') { Remove-Item -LiteralPath (Join-Path (Join-Path $B shots) $Matches[1]) -ErrorAction SilentlyContinue; $n++ } } }
        finally { $z.Dispose() }
        Say "bundled: $n screenshots in $Keys deleted from the PC"
    }
    'run' {
        Remove-Item -LiteralPath $Stop -ErrorAction SilentlyContinue
        if (-not (Test-RbRunning $sid)) { Say 'run: rekordbox not running'; exit 2 }
        $rows = @(Rows)
        if (-not $rows.Count) { Say "run: no corpus in $Corpus"; exit 2 }
        if (-not $From) { $From = $rows[0] }
        $i0 = [array]::IndexOf($rows, $From); if ($i0 -lt 0) { Say "run: no row named '$From'"; exit 2 }
        $i1 = if ($To) { [array]::IndexOf($rows, $To) } else { $rows.Count - 1 }
        $w16 = [array]::IndexOf($rows, $LongCopy) - 1
        $script:w16Pending = $w16 -ge 0
        if ($script:w16Pending -and $i0 -eq ($w16 + 1) -and $i1 -eq $w16) { $i0, $i1 = $i1, $i0 }
        if ($i1 -lt $i0) { Say "run: bad -To '$To'"; exit 2 }
        $selected = @($rows[$i0..$i1])
        if ($Formats) { Say 'run: use auto dumps --formats-only for format measurement'; exit 2 }
        if ($Aac) {
            $selected = @($selected -match '^(0[456]-aac-|w10-mp4-video-first$|w1[78]-aac-)')
            if ($selected -notcontains 'w10-mp4-video-first') { throw 'the AAC round requires w10-mp4-video-first' }
            $i1 = [array]::IndexOf($rows, $selected[-1])
        }
        # If a range names only one w16, the first of the pair may be a probe to find its order.
        $first = if ($script:w16Pending -and $i0 -eq ($w16 + 1)) { $w16 } else { $i0 }
        $last = if ($script:w16Pending -and $i1 -eq $w16) { $w16 + 1 } else { $i1 }
        # -InjectAt: a row this run loads, else the test window would never be reached
        if ($InjectAt) {
            $k = $InjectAt - 1
            if ($k -lt $first -or $k -gt $last) { Say "run: -InjectAt ${InjectAt}: not in rows $($first + 1)-$($last + 1)"; exit 2 }
            if ($Refused -contains $rows[$k]) { Say "run: -InjectAt ${InjectAt}: $($rows[$k]) is stepped over (rekordbox refuses it)"; exit 2 }
            $n = @($rows[$first..$k] | Where-Object { $selected -contains $_ -and $Refused -notcontains $_ }).Count
            if ($Max -gt 0 -and $n -gt $Max) { Say "run: -InjectAt ${InjectAt}: past -Max $Max (it would be load $n)"; exit 2 }
        }
        # the list header must be the playlist's, with its track count
        $want = "$Playlist ($($rows.Count) Tracks)"
        . (Join-Path $PSScriptRoot 'ocr-lib.ps1')
        # Both w16 entries have the same playlist title. The first readback fixes their order.
        function RowVote($ts, [int]$i) {
            $j = Vote-Row $ts $rows $Aliases
            if ($script:w16Pending -and $i -eq $w16 -and $j -in ($w16..($w16 + 1))) {
                if ($j -ne $i) { $rows[$i], $rows[$j] = $rows[$j], $rows[$i] }
                $script:w16Pending = $false
                $j = $i
            }
            return $j
        }
        # the bridge's lines since line n0, counted (match-lib.ps1)
        function Count([int]$n0) {
            $lines = @(Get-Content -LiteralPath $Bridge -ErrorAction SilentlyContinue)
            if ($lines.Count -lt $n0) {
                $old = @(Get-Content -LiteralPath "$Bridge.1" -ErrorAction SilentlyContinue)
                if ($old.Count -lt $n0) { return $null }
                return Count-Bridge (@($old | Select-Object -Skip $n0) + $lines)
            }
            Count-Bridge @($lines | Select-Object -Skip $n0)
        }
        # a row's load said and checked: no bridge activity fails either -Expect; -Expect hits wants
        # hits, no misses and no refused hits
        function Tally([int]$i, [string]$name, [int]$n0, $w) {
            if ($w.state -eq 'none') { $script:expBad++; Say "row $($i + 1) $name : EXPECT FAIL: loaded (read back), but no bridge activity in $StartWait s"; return }
            if ($Expect -eq 'keys') {
                # the probe's log: the chunks' KEY lines this load wrote (each has its mix dump)
                $kl = @(Get-Content -LiteralPath $Bridge -ErrorAction SilentlyContinue | Select-Object -Skip $n0 | Where-Object { $_ -match '\bKEY \d+ ' })
                $k = $kl.Count
                # which row each chunk's dump belongs to, for assembling a track on the Mac (rows.txt
                # beside the probe's log: "ROW MS KEY L")
                $rt = Join-Path (Split-Path -Parent $Bridge) 'rows.txt'
                foreach ($l in $kl) { if ($l -match '\bKEY (\d+) \S+ ([0-9a-f]+) L=(\d+)') { Add-Content -LiteralPath $rt -Value "$name $($Matches[1]) $($Matches[2]) $($Matches[3])" } }
                Say ('row {0} {1} : read back OK, first {2:N1} s, done {3:N1} s, {4} KEY lines' -f ($i + 1), $name, $w.first, $w.done, $k)
                if ($k) { $script:expOk++ } else { $script:expBad++; Say "row $($i + 1) $name : EXPECT FAIL: no KEY line" }
                return
            }
            $c = Count $n0
            if ($null -eq $c) { $script:expBad++; Say "row $($i + 1) $name : unmeasured (bridge log rotated and its earlier lines are unavailable)"; return }
            Say ('row {0} {1} : read back OK, first {2:N1} s, done {3:N1} s, {4} misses, {5} hits, {6} refused, {7} near' -f ($i + 1), $name, $w.first, $w.done, $c.miss, $c.hit, $c.refused, $c.near)
            if ($Expect -eq 'hits' -and ($c.miss -or $c.refused -or -not $c.hit)) { $script:expBad++; Say "row $($i + 1) $name : EXPECT FAIL: $($c.miss) misses, $($c.refused) refused, $($c.hit) hits (want 0 misses, 0 refused, hits)" }
            elseif ($Expect -eq 'any' -and ($c.hit + $c.miss + $c.refused + $c.near) -eq 0) { $script:expBad++; Say "row $($i + 1) $name : EXPECT FAIL: no classified bridge result" }
            else { $script:expOk++ }
        }
        # A probe doesn't count toward the requested rows. With -Expect hits it still needs a hit
        # and must have no misses or refusals, even outside -From..-To.
        function ProbeTally([int]$j, [int]$n0, $w) {
            if ($Expect -ne 'hits') { return }
            $row = if ($j -ge 0) { "$($j + 1) $($rows[$j])" } else { "unknown corpus row" }
            $why = if ($w.state -eq 'none') { "no bridge activity in $StartWait s" }
                   elseif ($w.state -eq 'done') {
                       $c = Count $n0
                       if ($null -eq $c) { $script:expBad++; Say "row $row : unmeasured (the probe's earlier bridge lines are unavailable)"; return }
                       if ($c.miss -or $c.refused -or -not $c.hit) { "$($c.miss) misses, $($c.refused) refused, $($c.hit) hits" }
                   }
            if ($why) {
                $script:expBad++
                Say "row $row : EXPECT FAIL (the sort check's load): $why"
            }
        }
        # -InjectAt: the test window, opened once before its row; Guard: the watch before a load (its
        # STOP line, or ''); at the -InjectAt row, a miss of the test window is a STOP too
        $script:injected = $false; $script:stopTest = $false
        function Inject([int]$row) { if ($InjectAt -eq $row -and -not $script:injected) { $script:injected = $true; Say ("run: the watch's test window before row ${row}: " + (Session 'testwindow')); Start-Sleep 3 } }
        function Guard([int]$row) {
            $s = Watch-Stop 'run'
            if ($InjectAt -eq $row) {
                if ($s -like '*(watch test);*') { $script:stopTest = $true }
                elseif (-not $s) { $s = "run: STOP: the watch missed its test window before row $row (watch test FAILED); " + (Session 'shot') }
            }
            return $s
        }
        function Halt([string]$s) { Say $s; if ($InjectAt) { Say ('run: ' + (Session 'closetest')) }; $script:rc = 1 }
        # the watch's baseline before any MIDI: only rekordbox's main window (and our app's) open
        $s = Watch-Start 'run' -Allow -Rekordbox; if ($s) { Say $s; exit 1 }
        Watch-Save; Say ('run: windows at the start: ' + ($script:Base -join '; '))
        if ($Warmup) {
            Say "run: waiting $Warmup s before the first load"
            $ready = (Get-Date).AddSeconds($Warmup)
            while ((Get-Date) -lt $ready) {
                if (Test-Path -LiteralPath $Stop) { Say 'run: STOP: midi-stop during warmup'; exit 1 }
                if (-not (Test-RbRunning $sid)) { Say 'run: STOP: rekordbox closed during warmup'; exit 1 }
                $s = Watch-Stop 'run'; if ($s) { Say $s; exit 1 }
                Start-Sleep 2
            }
        }
        $h = Open-RbspOut
        $rc = 0; $script:expOk = 0; $script:expBad = 0
        $deadline = if ($PSBoundParameters.ContainsKey('Minutes')) { (Get-Date).AddMinutes($Minutes) } else { $null }
        try { do {
            Open-TestPlaylist $h
            $f = Session 'front' 60
            Assert-RbDialogReply $f
            if ($f -notmatch '^\S+ front: ok; ') { Say ("run: STOP: rekordbox not put in front ($f); " + (Session 'shot')); $rc = 1; break }
            Say "run: $f"
            $png = Shot-Path (Session 'shot')
            $hdr = if ($png) { Read-ListHeader $png } else { '' }
            Drop $png
            if ((Norm $hdr) -ne (Norm $want)) { Say "run: STOP: expected playlist header $want"; $rc = 1; break }
            Say "run: on the playlist (header '$want')"
            # row 1 must be rows[0] (A to Z). Give OCR two reads per direction and flip at most once.
            $ok = $false; $halted = $false; $flipped = $false; $try = 0
            while ($try -lt 2) {
                $try++
                $s = Watch-Stop 'run'; if ($s) { Halt $s; $halted = $true; break }
                Press $h 'Up' $rows.Count 80; Start-Sleep -Milliseconds 700   # more than the list's rows: the top, whatever was selected
                Inject 1; $n0 = Lines
                $s = Guard 1; if ($s) { Halt $s; $halted = $true; break }
                $t0 = Get-Date; Press $h 'Load'; $d = Deck1
                Drop $d.png
                $w1 = WaitStems $n0 $t0
                Assert-RbLoadWindows
                $j = RowVote $d.ts 0
                if ($w1.state -eq 'stop') { Halt 'run: STOP: midi-stop'; $halted = $true; break }
                if ($w1.state -eq 'timeout') { Halt "run: STOP: the sort check's load: the bridge still writing after $MaxTrack s"; $halted = $true; break }
                if ($j -eq 0) {
                    $ok = $true; Say "run: row 1 is $($rows[0])"; $row1 = @{ n0 = $n0; w = $w1 }
                    if ($first -gt 0 -or $selected -notcontains $rows[0]) { ProbeTally 0 $n0 $w1 }   # row 1 isn't counted below
                    break
                }
                ProbeTally $j $n0 $w1
                if ($j -lt 0) {
                    if ($try -lt 2) { Say "run: row 1 read is inconclusive; reloading without changing sort" }
                    continue
                }
                if ($flipped) { break }
                Say "run: row 1 is $($rows[$j]); expected $($rows[0]): SortSONG (try $try)"
                $s = Watch-Stop 'run'; if ($s) { Halt $s; $halted = $true; break }
                Press $h 'SortSong'; Start-Sleep 1
                $flipped = $true; $try = 0
            }
            if ($halted) { break }
            if (-not $ok) { Say ('run: STOP: could not put row 1 first; ' + (Session 'shot')); $rc = 1; break }
            $at = 0   # the selected row
            Say "run: rows $($first + 1)-$($last + 1) of $($rows.Count) ($From .. $($selected[-1])), expect $Expect, bridge.log $(Lines) lines"
            $loads = 0; $halted = $false
            for ($i = $first; $i -le $last; $i++) {
                if (Test-Path -LiteralPath $Stop) { Say 'run: STOP: midi-stop'; $rc = 1; break }
                if ($deadline -and (Get-Date) -gt $deadline) { Say "run: STOP: -Minutes $Minutes reached"; $rc = 1; break }
                if (-not (Test-RbRunning $sid)) { Say 'run: STOP: rekordbox is gone'; $rc = 1; break }
                $name = $rows[$i]
                if ($selected -notcontains $name -and ($Aac -or -not $script:w16Pending -or $i -ne $w16)) { continue }
                if ($Refused -contains $name) { Say "row $($i + 1) $name : stepped over (rekordbox refuses it)"; continue }
                if ($Max -gt 0 -and $loads -ge $Max) { Say "run: -Max $Max loads reached"; break }
                $s = Watch-Stop 'run'; if ($s) { Halt $s; break }
                if ($i -eq 0) { Tally 0 $name $row1.n0 $row1.w; $loads++; continue }   # loaded by the sort check
                $right = $false
                $earlier = -1
                foreach ($try in 1..4) {
                    # a retry comes after a wait for the bridge: the windows again first
                    if ($try -gt 1) { $s = Watch-Stop 'run'; if ($s) { Halt $s; $halted = $true; break } }
                    if ($try -eq 3 -and $j -ge 0 -and $j -lt $i -and $j -eq $earlier) {
                        Say "row $($i + 1) $name : deck 1 still has $($rows[$j]): stepping again"; $at = $j
                    } elseif ($try -ge 3) {
                        Say "row $($i + 1) $name : finding the row from the top again"
                        Press $h 'Up' $rows.Count 80; Start-Sleep -Milliseconds 700; $at = 0
                    }
                    if ($i -gt $at) { Press $h 'Down' ($i - $at) 300; Start-Sleep -Milliseconds 700; $at = $i }
                    Inject ($i + 1); $n0 = Lines
                    $s = Guard ($i + 1); if ($s) { Halt $s; $halted = $true; break }
                    $t0 = Get-Date; Press $h 'Load'; $d = Deck1
                    Drop $d.png
                    $j = RowVote $d.ts $i
                    if ($j -eq $i) { $name = $rows[$i]; $right = $true; break }
                    $wr = WaitStems $n0 $t0
                    Assert-RbLoadWindows
                    ProbeTally $j $n0 $wr
                    if ($wr.state -eq 'stop') { Halt 'run: STOP: midi-stop'; $halted = $true; break }
                    if ($wr.state -eq 'timeout') { Halt "run: STOP: row $($i + 1) $name : the recovery load: the bridge still writing after $MaxTrack s"; $halted = $true; break }
                    if ($try -eq 1) {
                        $earlier = $j; Say "row $($i + 1) $name : deck 1 did not match the expected corpus row; trying Load again"
                    }
                }
                if ($halted) { break }
                if (-not $right) { Say 'run: STOP: deck 1 is not the expected track'; $rc = 1; break }
                $w = WaitStems $n0 $t0
                Assert-RbLoadWindows
                if ($w.state -eq 'stop') { Halt 'run: STOP: midi-stop'; break }
                if ($w.state -eq 'timeout') { Halt "run: STOP: row $($i + 1) $name : the bridge still writing after $MaxTrack s (its lines would be counted against the next row)"; break }
                if ($selected -contains $name) { $loads++; Tally $i $name $n0 $w }
                else { ProbeTally $i $n0 $w }
            }
        } while ($false) } finally { [void][RbspMidi2]::midiOutClose($h) }
        # after the last row (or -Max): midi-stop not created meanwhile, rekordbox still there, and no new window
        if (-not $rc -and (Test-Path -LiteralPath $Stop)) { Say 'run: STOP: midi-stop'; $rc = 1 }
        if (-not $rc) {
            if (-not (Test-RbRunning $sid)) { Say 'run: STOP: rekordbox is gone'; $rc = 1 }
            else { $s = Watch-Stop 'run'; if ($s) { Say $s; $rc = 1 } }
        }
        if ($InjectAt -and -not $script:stopTest -and -not $rc) { Say "run: the watch test FAILED: no test STOP at row $InjectAt"; $rc = 1 }
        Say "run: expect ${Expect}: $script:expOk rows OK, $script:expBad not"; if ($script:expBad -and -not $rc) { $rc = 3 }
        Say 'run: end'
        exit $rc
    }
} } catch {
    # an error is a STOP: nothing more is done, rekordbox is left as it is
    if ($_.Exception.Message -clike 'rekordbox #32770 message box: *') {
        Say "${Do}: STOP: $($_.Exception.Message)"
        exit 1
    }
    # not on the owner's dialog allow-list (D-1122-3): named, never clicked
    if ($_.Exception.Message -cmatch '^rekordbox Analysis Setting dialog: (0x[0-9A-F]+(?:, 0x[0-9A-F]+)*)$') {
        Say ("${Do}: STOP: rekordbox's Analysis Setting dialog is open ($($Matches[1])); the kit never clicks it (not approved). " +
            "rekordbox can't close while it's open: revert the VM, or the owner answers it on the PC. Then set the profile so " +
            "rekordbox doesn't ask: run autoanalysis-off, and askAnalysisSettingWindow=0 in rekordbox3.settings with rekordbox closed (preflight checks it)")
        exit 1
    }
    $why = if (($FormatRun -or $Do -in 'library-check', 'format-import', 'format-reuse', 'format-verify', 'format-uidump', 'format-step', 'format-hits') -and $_.Exception.Message -notlike 'formats: *') { 'format operation failed' } else { $_.Exception.Message }
    if ($EvidenceRun -and $why -notlike 'keycompare: *') { $why = 'comparison evidence could not be read or saved' }
    $m = "${Do}: STOP: error: $why ($(if ($_.InvocationInfo.ScriptName) { Split-Path -Leaf $_.InvocationInfo.ScriptName } else { 'midi-auto.ps1' }) line $($_.InvocationInfo.ScriptLineNumber))"
    if (Get-Command Say -CommandType Function -ErrorAction SilentlyContinue) { Say $m } else { Write-Output $m }   # kit-lib.ps1 may not have loaded
    exit 1
} finally {
    if ($formatLock) { $formatLock.Dispose() }
    if ($commandClock) { Write-KitTiming $Do $commandClock }
}
