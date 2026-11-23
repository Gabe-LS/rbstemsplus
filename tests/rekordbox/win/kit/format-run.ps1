# Build each batch from the verified library snapshot.
function Get-FormatPlan {
    $dir = Join-Path $Corpus 'formats'
    if (-not [IO.Directory]::Exists($dir)) { throw 'formats: stage a verified format subset first' }
    $catalogFile = $null
    $staged = Get-FormatFiles $dir -Verify -CatalogFile ([ref]$catalogFile)
    $catalog = @(Get-FormatCatalog $catalogFile $staged)
    $state = Get-FormatImportState $dir
    $entry = $state.playlist
    if (-not $entry -or $state.name -cne $entry.name) { throw 'formats: verify the imported playlist through the library first' }
    $imported = @($entry.corpusTracks)
    $evidence = "$($entry.id):$($entry.treePath -join ','):$($imported -join ',')"
    $missing = @($staged.Keys | Where-Object { $imported -notcontains $_ } | Sort-Object)
    if ($missing.Count -and -not $ConfirmImportRefusals) {
        Say "formats: absent corpus IDs: $($missing -join ',')"
        throw "formats: the library playlist omits staged files. Use --confirm-import-refusals if rekordbox refused the listed IDs."
    }
    $ordered = @($staged.Keys | Sort-Object)
    $batchSize = 4
    $offset = ($FormatBatch - 1) * $batchSize
    $batchCount = [int][math]::Ceiling($ordered.Count / $batchSize)
    if ($offset -ge $ordered.Count) { throw "formats: choose a batch from 1 to $batchCount" }
    $batch = @($ordered | Select-Object -Skip $offset -First $batchSize)
    return @{
        Batch = $batch; BatchCount = $batchCount; Files = $staged
        Imported = $imported; Playlist = $entry; Catalog = $catalog
        Manifest = Get-FormatManifest $dir
        Evidence = $evidence
    }
}

function ConvertTo-FormatHeading([string]$text) {
    # OCR can misread the parenthesized track count after the playlist name.
    Fold (Norm (($text -split '\(', 2)[0]))
}

function Open-FormatPlaylist([IntPtr]$h, $plan) {
    $s = Watch-Start 'formats' -Allow -Rekordbox -RedactTitles
    if ($s) { Say $s; throw 'formats: the window watch stopped playlist verification' }
    Watch-Save
    Select-FormatPlaylist $h $plan
}

# Walk from the icon column to the imported playlist and verify its heading.
function Select-FormatPlaylist([IntPtr]$h, $plan) {
    if (-not $plan.Imported.Count) { return }
    if ($null -eq $plan.Playlist.otherFormatNames) { throw 'formats: re-verify the import to record the library headings' }
    $path = @($plan.Playlist.treePath)
    if ($path.Count -ne 1 -or $path[0] -notmatch '^[1-9][0-9]{0,2}$') {
        throw 'formats: the imported playlist must have a verified root position for MIDI navigation'
    }
    # The tree starts at All Tracks, then the Playlists root, then the library's first row.
    $playlistRow = 1 + [int]$path[0]
    $listedRows = [int]$plan.Playlist.listedPlaylistRows
    if ($listedRows -lt [int]$path[0]) { throw 'formats: re-verify the import to record the library row count' }
    $front = Session 'front' 60
    Assert-RbDialogReply $front
    if ($front -notmatch '^\S+ front: ok; ') { throw 'formats: could not focus rekordbox' }
    $watch = Watch-Stop 'formats' -RedactTitles
    if ($watch) { Say $watch; throw 'formats: window watch stopped playlist navigation' }
    $navigationClock = Start-KitTiming
    try { Open-TestPlaylist $h $playlistRow -ResetUp ([math]::Max(10, $listedRows + 2)) }
    finally { Write-KitTiming 'midi-navigation' $navigationClock }
    $ocrClock = Start-KitTiming
    $png = ''
    try {
        $heading = ConvertTo-FormatHeading $plan.Playlist.name
        $otherHeadings = @($plan.Playlist.otherFormatNames | ForEach-Object { ConvertTo-FormatHeading $_ } | Sort-Object -Unique)
        $accepted = @{}
        $png = Shot-Path (Session 'shot')
        if ($png) {
            foreach ($reading in (Read-FormatListHeaders $png)) {
                $text = ConvertTo-FormatHeading $reading
                $distance = Dist $text $heading
                if ($distance -gt 2) { continue }
                if ($otherHeadings | Where-Object { (Dist $text $_) -lt ($distance + 2) } | Select-Object -First 1) {
                    continue
                }
                $accepted[$text] = 1 + [int]$accepted[$text]
                if ($accepted[$text] -ge 2) {
                    Say "formats: verified the formats heading at MIDI tree step $playlistRow"
                    return
                }
            }
        }
    } finally {
        Drop $png
        Write-KitTiming 'ocr-heading' $ocrClock
    }
    throw "formats: RBSP Formats heading not verified at MIDI tree step $playlistRow"
}

$formatProbeLine = '\bKEY (\d+) \S+ ([0-9a-fA-F]{64}) L=(\d+)'
# The probe writes "TIME [THREAD] Run MODEL #N MS ms" after each run, and a KEY on the same thread
# before each stems run. Runs pair with KEYs per thread; a model whose Run paired with a KEY is the
# stems model for the rest of this batch, whatever rekordbox names it (OI-81).
$formatProbeRun = '^\S+ (\S+) Run (\S+) #\d+ \d+ ms( \| .*)?( -> .*)?$'
$script:FormatStemsModels = @{}

function Get-FormatProbeKeys([string]$id, [string[]]$lines) {
    foreach ($line in $lines) {
        if ($line -match $formatProbeLine) {
            "$id $($Matches[1]) $($Matches[2]) $($Matches[3])"
        }
    }
}

function Find-FormatDeckRow([string[]]$readings, [string[]]$catalog, [string[]]$rows) {
    $byTitle = @{}
    foreach ($id in $catalog) {
        $title = Fold (Norm $id)
        if ($byTitle.ContainsKey($title)) { $byTitle[$title] = '' }
        else { $byTitle.Add($title, $id) }
    }
    $votes = @(foreach ($reading in $readings) {
        $id = $byTitle[(Fold (Norm $reading))]
        if ($id) { $id }
    })
    $ranked = @($votes | Group-Object | Sort-Object Count -Descending)
    if (-not $ranked.Count -or $ranked[0].Count -lt 2 -or
        ($ranked.Count -gt 1 -and $ranked[0].Count -eq $ranked[1].Count)) { return -1 }
    return [array]::IndexOf($rows, $ranked[0].Name)
}

function Get-FormatBridgeStamp {
    $parts = foreach ($path in $Bridge, "$Bridge.1") {
        $log = [IO.FileInfo]::new($path)
        if ($log.Exists) { "$($log.Length):$($log.LastWriteTimeUtc.Ticks)" }
        else { 'absent' }
    }
    return $parts -join ','
}

function Wait-FormatSettle([string]$stamp) {
    $until = (Get-Date).AddSeconds([math]::Min(90, $Quiet))
    do {
        if (Test-Path -LiteralPath $Stop) { throw 'formats: measurement stopped while settling a silent Load' }
        if ((Get-FormatBridgeStamp) -cne $stamp) { throw 'formats: late bridge activity after a silent Load' }
        if ((Get-Date) -ge $until) { return }
        Start-Sleep -Milliseconds 500
    } while ($true)
}

function Get-FormatLoadLines([int]$start, [string]$before, [string]$current) {
    $lines = @(Get-Content -LiteralPath $Bridge -ErrorAction SilentlyContinue)
    if (($before -split ',')[1] -cne ($current -split ',')[1]) {
        $old = @(Get-Content -LiteralPath "$Bridge.1" -ErrorAction SilentlyContinue)
        if ($old.Count -lt $start) { throw 'formats: activity log changed during Load' }
        $old | Select-Object -Skip $start
        $lines
    } else {
        if ($lines.Count -lt $start) { throw 'formats: activity log changed during Load' }
        $lines | Select-Object -Skip $start
    }
}

function Get-FormatExpectedActivity($plan, [bool]$probe) {
    $expected = @{ Rows = @{}; Owners = @{} }
    $paths = Bench-Paths -KeysOnly -QuietMissing
    if (-not $paths) { return $expected }
    foreach ($id in $plan.Batch) {
        $result = Invoke-ResearchKeys $paths $plan.Files[$id] $id
        if ($result.TimedOut -or $result.ExitCode -ne 0) { continue }
        $chunks = @{}
        $ambiguous = $false
        foreach ($line in ($result.Output -split "`r?`n")) {
            if (-not $line -or $line -like "file`t*") { continue }
            if ($line -notmatch '^key\t([0-9a-f]{64})\t\d+\t\d+\t([01])\t') { $chunks.Clear(); break }
            $hash = $Matches[1]
            if ($chunks.ContainsKey($hash)) { $ambiguous = $true }
            if (-not $chunks.ContainsKey($hash) -or $Matches[2] -eq '0') { $chunks[$hash] = $Matches[2] }
        }
        if (-not $chunks.Count) { continue }
        $required = @{}
        foreach ($hash in $chunks.Keys) {
            $token = if ($probe) { $hash } else { $hash.Substring(0, 12) }
            if ($expected.Owners.ContainsKey($token) -and $expected.Owners[$token] -ne $id) {
                $expected.Owners[$token] = ''
            } else { $expected.Owners[$token] = $id }
            if (-not $probe -or $chunks[$hash] -eq '0') {
                if ($required.ContainsKey($token)) { $ambiguous = $true }
                $required[$token] = $true
            }
        }
        if (-not $ambiguous) { $expected.Rows[$id] = $required }
    }
    return $expected
}

function Wait-FormatActivity([string]$id, [int]$attempt, [int]$start, [datetime]$loaded, [string]$before, $expected, [bool]$probe) {
    $quietClock = Start-KitTiming
    $marginClock = $null
    $marginEnd = $null
    try {
        if (-not $expected.Owners.Count) { return (WaitStems $start $loaded) }
        $required = $expected.Rows[$id]
        $fallback = $null -eq $required
        $remaining = $required.Count
        $seen = @{}
        $pending = 0
        $pendingByThread = @{}
        $ignoredRuns = @{}
        $owned = 0
        if ($probe) {
            foreach ($owner in $expected.Owners.Values) {
                if ($owner -eq $id) { $owned++ }
            }
        }
        $sawExact = $false
        $tail = $false
        $stamp = $before
        $lines = @()
        $first = $last = $null
        while ($true) {
            if (Test-Path -LiteralPath $Stop) { return @{ state = 'stop' } }
            $current = Get-FormatBridgeStamp
            if ($current -cne $stamp) {
                $new = @(Get-FormatLoadLines $start $before $current)
                if ($new.Count -lt $lines.Count) { throw 'formats: activity log changed during Load' }
                if ($new.Count -gt $lines.Count) {
                    $last = Get-Date
                    if (-not $first) { $first = $last }
                }
                for ($i = $lines.Count; $i -lt $new.Count; $i++) {
                    $token = $null
                    $kind = ''
                    $thread = $null
                    $line = $new[$i]
                    if ($probe) {
                        if ($line -match $formatProbeLine) { $token = $Matches[2]; $kind = 'exact'; $thread = ($line -split ' ', 3)[1] }
                        elseif ($line -match $formatProbeRun) {
                            $runThread = $Matches[1]; $model = $Matches[2]
                            if ($pendingByThread[$runThread]) {
                                $pendingByThread[$runThread]--
                                $pending--
                                $script:FormatStemsModels[$model] = $true
                                if ($ignoredRuns.ContainsKey($model)) { throw 'formats: late bridge activity during probe Load (Run without KEY)' }
                                continue
                            }
                            # Another model's run (rekordbox's embedding models) with no KEY on its thread
                            if ($script:FormatStemsModels.ContainsKey($model)) { throw 'formats: late bridge activity during probe Load (Run without KEY)' }
                            $ignoredRuns[$model] = 1 + [int]$ignoredRuns[$model]
                            continue
                        }
                    } elseif ($line -match '^\S+ \S+  (hit  |miss )([0-9a-f]{12})(?= |$)') {
                        $token = $Matches[2]
                        $kind = if ($Matches[1] -eq 'miss ') { 'miss' }
                            elseif ($line -match ' \(\d+ samples\)') { 'exact' } else { 'refused' }
                    } elseif ($line -match '^\S+ \S+  near [0-9a-f]{12}(?: rejected \([^)]*\))? for ([0-9a-f]{12}):') {
                        $token = $Matches[1]; $kind = 'near'
                    } elseif ($line -match '^\S+ \S+  timing (?:run=|store )') { continue }
                    $known = $token -and $expected.Owners.ContainsKey($token)
                    if ($known -and $expected.Owners[$token] -ne $id) { throw 'formats: activity from another planned row during Load' }
                    if ($tail -and ($known -or $kind -eq 'miss')) { throw 'formats: late bridge activity after the cache tail' }
                    # The bridge's trailing miss has no planned ID and follows every exact serve.
                    if (-not $fallback -and $kind -eq 'miss' -and -not $known -and $remaining -eq 0 -and -not $tail) {
                        $tail = $true
                        continue
                    }
                    if ($marginEnd) {
                        # The cache writer can report disk space after the tail miss.
                        if ($line -cmatch '^\S+ \S+  cache: [0-9.]+ GB free on its disk ') { continue }
                        throw 'formats: late bridge activity during the completion margin'
                    }
                    # The probe emits KEY before running the model. Track unplanned keys too
                    # so a row using the quiet wait still waits for those runs to finish.
                    if ($probe -and $token) { $pending++; $pendingByThread[$thread] = 1 + [int]$pendingByThread[$thread] }
                    if ($token -and (-not $known -or $kind -ne 'exact')) { $fallback = $true }
                    if (-not $fallback -and $token) {
                        $sawExact = $true
                        if (-not $seen.ContainsKey($token)) {
                            $seen[$token] = $true
                            if ($required.ContainsKey($token)) { $remaining-- }
                        }
                    }
                }
                $lines = $new
                $stamp = $current
            }
            $now = Get-Date
            if (-not $first) {
                if (($now - $loaded).TotalSeconds -ge $StartWait) { return @{ state = 'none' } }
            } else {
                $finished = if ($probe) { -not $pending -and $seen.Count -eq $owned } else { $tail }
                if (-not $fallback -and $sawExact -and -not $remaining -and -not $marginEnd -and $finished) {
                    $marginClock = Start-KitTiming
                    $marginEnd = $now.AddSeconds(2)
                }
                if (($marginEnd -and $now -ge $marginEnd) -or
                    (-not $marginEnd -and -not $pending -and ($now - $last).TotalSeconds -ge $Quiet)) {
                    return @{ state = 'done'; first = ($first - $loaded).TotalSeconds; done = ($last - $loaded).TotalSeconds
                        lines = $lines; completionStamp = $(if ($marginEnd) { $stamp }) }
                }
                if (($now - $loaded).TotalSeconds -ge $MaxTrack) { return @{ state = 'timeout' } }
            }
            Start-Sleep -Milliseconds 500
        }
    } finally {
        if ($marginEnd) { Write-KitTiming 'load-completion-margin' $marginClock $id $attempt }
        else { Write-KitTiming 'load-quiet-wait' $quietClock $id $attempt }
        if ($ignoredRuns.Count) {
            $n = ($ignoredRuns.Values | Measure-Object -Sum).Sum
            $models = ($ignoredRuns.Keys | Sort-Object | ForEach-Object { "$_ x$($ignoredRuns[$_])" }) -join ', '
            Say "formats: $id ignored $n Run line(s) with no KEY on their thread, not of the stems model: $models"
        }
    }
}

function Get-FormatCacheOutcome([string[]]$lines, $wait) {
    if ($wait.state -eq 'none') { return 'NO_CACHE_EVIDENCE' }
    $c = Count-Bridge $lines
    $status = if ($c.hit) { 'CACHE_HIT' } elseif ($c.miss -or $c.refused) { 'CACHE_MISS' } else { 'NO_CACHE_EVIDENCE' }
    $ms = @($lines | ForEach-Object {
        if ($_ -match '\btiming run=hit total_ns=(\d+)\b') { [long]$Matches[1] / 1000000 }
    })
    $timing = if ($ms.Count) {
        $maxMs = ($ms | Measure-Object -Maximum).Maximum
        ' slowest-hit-ms=' + $maxMs.ToString('F2', [Globalization.CultureInfo]::InvariantCulture)
    } else { '' }
    # Use the T2 load-to-activity timing as well as bridge timing when the latter is available.
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $first = $wait.first.ToString('F1', $culture)
    $done = $wait.done.ToString('F1', $culture)
    return "$status exact=$($c.hit) miss=$($c.miss) refused=$($c.refused) near=$($c.near)$timing first-activity-s=$first last-activity-s=$done"
}

function Assert-FormatLoadReady([datetime]$deadline) {
    if ((Get-Date) -ge $deadline -or (Test-Path -LiteralPath $Stop) -or -not (Test-RbRunning $sid)) {
        throw 'formats: measurement stopped before load'
    }
}

function Get-FormatProcessStamp {
    $process = @(Get-RbProcess $sid)
    if ($process.Count -ne 1) { throw 'formats: expected one rekordbox process during the format pass' }
    return "$($process[0].Id):$($process[0].StartTime.ToUniversalTime().Ticks)"
}

function Assert-FormatPassBridge([string]$stamp) {
    if ($stamp -and (Get-FormatBridgeStamp) -cne $stamp) { throw 'formats: late bridge activity between format batches' }
}

function Invoke-FormatBatch($plan, [string]$outcomes, [string]$keyFile, [ref]$Next) {
    $rows = $plan.Imported
    $unmet = 0
    $processStamp = $null
    $previousStamp = $null
    if ($FormatContinuation -and -not $FormatPass) { throw 'formats: a continuation requires a format pass' }
    if ($FormatPass) {
        $processStamp = Get-FormatProcessStamp
        if ($FormatContinuation) {
            $previousProcess, $previousStamp = $FormatContinuation -split '/', 2
            if ($previousProcess -cne $processStamp) { throw 'formats: rekordbox changed during the format pass' }
            Assert-FormatPassBridge $previousStamp
        }
    }
    $expected = Get-FormatExpectedActivity $plan ([bool]$keyFile)
    $h = Open-RbspOut
    try {
        Open-FormatPlaylist $h $plan
        $deadline = (Get-Date).AddMinutes(35)
        $at = -1
        $headingChecked = $true
        $reverse = $false
        $settled = $null
        $completed = $null
        foreach ($id in $plan.Batch) {
            $i = [array]::IndexOf($rows, $id)
            $outcome = 'IMPORT_REFUSED'
            if ($i -ge 0) {
                $outcome = 'LOAD_UNCONFIRMED'
                foreach ($attempt in 1..2) {
                    Assert-FormatLoadReady $deadline
                    $watch = Watch-Stop 'formats' -RedactTitles
                    if ($watch) { Say $watch; throw 'formats: window watch stopped browser navigation' }
                    $target = if ($reverse) { $rows.Count - 1 - $i } else { $i }
                    # After a confirmed Load, the next row one below is the track list re-entered
                    # and a single Down. The exact deck match below still decides whether the Load counts.
                    $navMode = if ($attempt -eq 1 -and $at -ge 0 -and $target -eq $at + 1) { 'relative' } else { 'reset' }
                    $navigationClock = Start-KitTiming
                    try {
                        if ($navMode -eq 'reset') {
                            # The batch's first row uses the walk just verified. Retries and rows
                            # after an unconfirmed Load walk to the playlist again.
                            if (-not $headingChecked) {
                                Select-FormatPlaylist $h $plan
                                Assert-FormatLoadReady $deadline
                            }
                            $headingChecked = $false
                            Press $h 'Up' $rows.Count 80; Start-Sleep -Milliseconds 700
                            $at = 0
                        }
                        Assert-FormatLoadReady $deadline
                        if ($navMode -eq 'relative') {
                            # OI-82 (VM 2026-11-22): after a Load, Browse Down no longer moved the track
                            # list (deck 1 kept the row above; a wake note, 8 s of settle time or
                            # rekordbox put in front did not help). The walk's area steps put the
                            # focus back on the track list, its selection kept.
                            Press $h 'AreaLeft' 5 250; Start-Sleep -Milliseconds 800
                            Press $h 'AreaRight'; Start-Sleep -Milliseconds 800
                            Press $h 'AreaRight'; Start-Sleep -Milliseconds 1500
                        }
                        if ($target -gt $at) { Press $h 'Down' ($target - $at) 300; Start-Sleep -Milliseconds 700 }
                        # Allow the browser to settle after navigation before the first probe Load.
                        if ($keyFile -and $attempt -eq 1) { Start-Sleep -Seconds 2 }
                    } finally { Write-KitTiming 'midi-navigation' $navigationClock $id $attempt "nav-mode=$navMode" }
                    $watch = Watch-Stop 'formats' -RedactTitles
                    if ($watch) { Say $watch; throw 'formats: window watch stopped the load' }
                    $loadClock = Start-KitTiming
                    try {
                        Assert-FormatLoadReady $deadline
                        $stamp = Get-FormatBridgeStamp
                        if ($settled -and $stamp -cne $settled) { throw 'formats: late bridge activity after a silent Load' }
                        if ($completed -and $stamp -cne $completed) { throw 'formats: late bridge activity after a completed Load' }
                        if ($previousStamp -and $stamp -cne $previousStamp) { throw 'formats: late bridge activity between format batches' }
                        $previousStamp = $null
                        $n0 = Lines; $t0 = Get-Date
                        if ((Get-FormatBridgeStamp) -cne $stamp) { throw 'formats: activity log changed before Load' }
                        Press $h 'Load'
                        $ocrClock = Start-KitTiming
                        try {
                            $d = Deck1
                            Drop $d.png
                            $j = Find-FormatDeckRow $d.ts $plan.Catalog $rows
                        } finally { Write-KitTiming 'ocr-deck' $ocrClock $id $attempt }
                        $activityClock = Start-KitTiming
                        try { $w = Wait-FormatActivity $id $attempt $n0 $t0 $stamp $expected ([bool]$keyFile) }
                        finally { Write-KitTiming 'load-activity-wait' $activityClock $id $attempt }
                        Assert-RbLoadWindows
                        $completed = $w.completionStamp
                        if ($completed -and (Get-FormatBridgeStamp) -cne $completed) { throw 'formats: late bridge activity after a completed Load' }
                        if ($w.state -in 'stop', 'timeout') { throw 'formats: the activity log did not reach its quiet period' }
                        if ($w.state -eq 'none') {
                            $settleClock = Start-KitTiming
                            try { Wait-FormatSettle $stamp }
                            finally { Write-KitTiming 'load-settle-wait' $settleClock $id $attempt }
                            $settled = $stamp
                            Assert-RbLoadWindows
                        } else { $settled = $null }
                    } finally { Write-KitTiming 'load' $loadClock $id $attempt }
                    if ($j -eq $i) {
                        $lines = @()
                        if ($w.state -ne 'none') {
                            $lines = if ($null -ne $w.lines) { $w.lines } else { @(Get-FormatLoadLines $n0 $stamp (Get-FormatBridgeStamp)) }
                        }
                        if ($keyFile) {
                            $keys = @(Get-FormatProbeKeys $id $lines)
                            $outcome = 'LOADED_NO_KEYS'
                            if ($keys.Count) {
                                Add-Content -LiteralPath $keyFile -Value $keys
                                $outcome = 'KEYS'
                            }
                        } else {
                            $outcome = Get-FormatCacheOutcome $lines $w
                        }
                        $at = $target
                        break
                    }
                    # A read at the mirror position suggests a descending list. Test it by
                    # a full reset and verifying the target's identity on the retry.
                    if ($attempt -eq 1 -and $j -eq ($rows.Count - 1 - $i)) {
                        $reverse = -not $reverse
                    }
                    $at = -1
                    if ($attempt -eq 1) {
                        # what deck 1 read as: a catalog row (by its fixture name) or none, never the OCR text
                        $read = if ($j -ge 0) { $rows[$j] } else { 'no single catalog title' }
                        Say "formats: $id deck identity did not match (deck 1 read as $read); retrying from the top"
                    }
                }
            }
            if ($outcomes) { Add-Content -LiteralPath $outcomes -Value "$id $outcome" }
            elseif ($outcome -notmatch '^CACHE_HIT exact=[1-9][0-9]* miss=[01] refused=0\b') { $unmet++ }
            Say "formats: $id $outcome"
        }
        if ((Test-Path -LiteralPath $Stop) -or -not (Test-RbRunning $sid)) {
            throw 'formats: measurement stopped after load'
        }
        $endStamp = $null
        if ($FormatPass) {
            $endStamp = if ($settled) { $settled } elseif ($previousStamp) { $previousStamp } else { Get-FormatBridgeStamp }
        }
        $watch = Watch-Stop 'formats' -RedactTitles
        if ($watch) { Say $watch; throw 'formats: window watch stopped after load' }
        if ($completed -and (Get-FormatBridgeStamp) -cne $completed) { throw 'formats: late bridge activity after a completed Load' }
        if (-not (Test-RbRunning $sid)) { throw 'formats: rekordbox exited after load' }
        if ($FormatPass) {
            if ((Get-FormatProcessStamp) -cne $processStamp) { throw 'formats: rekordbox changed during the format pass' }
            Assert-FormatPassBridge $endStamp
            $Next.Value = "$processStamp/$endStamp"
        }
    } finally { [void][RbspMidi2]::midiOutClose($h) }
    return $unmet
}
