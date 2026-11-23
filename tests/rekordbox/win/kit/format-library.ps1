# Keep the installed helper's output in memory. Only the filtered summary leaves this script.
function Find-InstalledPrepare {
    $apps = Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus'
    Get-ChildItem -LiteralPath $apps -Recurse -Filter 'rbsp-prepare.exe' -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

function Invoke-FormatLibraryHelper([string]$exe, [string]$arguments, [datetime]$deadline) {
    $remaining = [int][math]::Min(90000, ($deadline - [datetime]::UtcNow).TotalMilliseconds)
    if ($remaining -le 0) { throw 'formats: library query timed out' }
    $si = New-Object Diagnostics.ProcessStartInfo $exe, $arguments
    $si.UseShellExecute = $false
    $si.CreateNoWindow = $true
    $si.RedirectStandardOutput = $true
    $si.RedirectStandardError = $true
    $si.StandardOutputEncoding = [Text.Encoding]::UTF8
    $p = [Diagnostics.Process]::Start($si)
    try {
        $out = $p.StandardOutput.ReadToEndAsync()
        $err = $p.StandardError.ReadToEndAsync()
        if (-not [Threading.Tasks.Task]::WaitAll(@($out, $err), $remaining) -or -not $p.WaitForExit(1000)) {
            throw 'formats: library query timed out'
        }
        if ($p.ExitCode -ne 0) { throw "formats: installed helper $($arguments.Split(' ')[0]) exited $($p.ExitCode)" }
        foreach ($line in ($out.Result -split '\r?\n')) {
            if ($line) { ConvertFrom-Json -InputObject $line -ErrorAction Stop }
        }
    } finally { $p.Dispose() }
}

function Get-FormatCorpusPaths([string]$corpus) {
    Assert-FormatPath $corpus
    $paths = @{}
    foreach ($sub in 'v1', 'win', 'formats') {
        $dir = Join-Path $corpus $sub
        Assert-FormatPath $dir
        if (-not [IO.Directory]::Exists($dir)) { continue }
        foreach ($file in (Get-ChildItem -LiteralPath $dir -File)) {
            if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint -or $file.Extension -in '', '.txt', '.tsv' -or
                $file.BaseName -cnotmatch '^(?:fmt-|[0-9]{2}-|w[0-9]{2}-)[a-zA-Z0-9._~-]+$') { continue }
            $paths.Add($file.FullName, $file.BaseName)
        }
    }
    $pointer = Join-Path $corpus 'long-path.txt'
    Assert-FormatPath $pointer
    if ([IO.File]::Exists($pointer)) {
        $long = [IO.File]::ReadAllText($pointer).Trim()
        $base = [regex]::Escape((Join-Path $corpus 'long') + [IO.Path]::DirectorySeparatorChar)
        $segment = [regex]::Escape('rbsp-long-path-segment-0123456789abcdefghij' + [IO.Path]::DirectorySeparatorChar)
        if ($long -match "^$base($segment)+w16-flac-longpath\.flac$") {
            $paths.Add($long, 'w16-flac-longpath-copy')
        }
    }
    return $paths
}

function Get-FormatLibrary([string]$corpus, [string]$name, [string]$subsetPattern, $staged) {
    $exe = Find-InstalledPrepare
    if (-not $exe) { throw 'formats: installed rbsp-prepare.exe is missing' }
    $paths = Get-FormatCorpusPaths $corpus
    $positions = @{}
    $next = @{}
    $deadline = [datetime]::UtcNow.AddSeconds(180)
    $listing = @(Invoke-FormatLibraryHelper $exe.FullName '--playlists' $deadline)
    $formatPlaylists = @($listing | Where-Object { $_.name -like 'RBSP Formats *' })
    foreach ($row in $listing) {
        $id = [string]$row.playlist
        $parent = [string]$row.parent
        if (-not $id -or $positions.ContainsKey($id)) {
            throw 'formats: missing or repeated playlist ID from installed helper'
        }
        $next[$parent] = 1 + $next[$parent]
        $treePath = @($next[$parent])
        if ($positions.ContainsKey($parent)) { $treePath = @($positions[$parent]) + $treePath }
        $positions.Add($id, $treePath)
        if (-not ([string]$row.name).StartsWith('RBSP ', [StringComparison]::Ordinal) -or
            ($name -and $row.name -cne $name)) { continue }
        if ($subsetPattern -and ($row.name -cnotmatch $subsetPattern -or $row.kind -cne 'playlist' -or
            $treePath.Count -ne 1 -or $treePath[0] -gt 999)) { continue }
        $ids = [Collections.Generic.List[string]]::new()
        if ($row.kind -ceq 'playlist') {
            if ($id -cnotmatch '^[A-Za-z0-9{][A-Za-z0-9{}._-]*$') {
                throw 'formats: invalid RBSP playlist ID from installed helper'
            }
            $seen = @{}
            $exact = $true
            foreach ($track in (Invoke-FormatLibraryHelper $exe.FullName "--tracks $id" $deadline)) {
                $path = ([string]$track.file).Replace([IO.Path]::AltDirectorySeparatorChar, [IO.Path]::DirectorySeparatorChar)
                if ($path.StartsWith('\\?\')) { $path = $path.Substring(4) }
                $corpusId = $paths[$path]
                if ($name -or $subsetPattern) {
                    if (-not $corpusId -or $staged[$corpusId] -ne $path -or $seen.ContainsKey($corpusId)) {
                        if ($name) { throw 'formats: library playlist has an unexpected or repeated corpus row' }
                        $exact = $false
                        break
                    }
                    $seen.Add($corpusId, $true)
                }
                if ($corpusId) { $ids.Add($corpusId) }
            }
            if ($subsetPattern -and (-not $exact -or $ids.Count -ne $staged.Count)) { continue }
        }
        $entry = [ordered]@{
            id = $id; name = [string]$row.name; parent = $parent; order = $next[$parent]
            treePath = $treePath; kind = [string]$row.kind; corpusTracks = $ids.ToArray()
        }
        if ($name -or $subsetPattern) {
            $entry.listedPlaylistRows = $listing.Count
            $entry.otherFormatNames = @($formatPlaylists | Where-Object { $_.playlist -cne $id } |
                ForEach-Object { [string]$_.name })
        }
        [pscustomobject]$entry
    }
}

function Save-FormatLibrary([string]$corpus) {
    $entries = @(Get-FormatLibrary $corpus)
    $name = 'library-check-' + [guid]::NewGuid().ToString('N') + '.json'
    $path = Join-Path $B $name
    Assert-FormatPath $path
    $json = ConvertTo-Json -InputObject $entries -Depth 5
    [IO.File]::WriteAllText($path, $json, [Text.UTF8Encoding]::new($false))
    return $name
}

function Find-FormatLibraryPlaylist([string]$corpus, [string]$name, $staged) {
    $matchesByName = @(Get-FormatLibrary $corpus $name -Staged $staged)
    if ($matchesByName.Count -ne 1 -or $matchesByName[0].kind -cne 'playlist') {
        throw "formats: expected exactly one library playlist named '$name'"
    }
    return $matchesByName[0]
}

function Save-FormatLibraryImport($state, $entry, $staged) {
    $accepted = [ordered]@{}
    foreach ($id in $entry.corpusTracks) {
        if (-not $staged.ContainsKey($id) -or $accepted.Contains($id)) {
            throw 'formats: library playlist has an unexpected or repeated corpus ID'
        }
        $accepted.Add($id, $staged[$id])
    }
    if ($state.playlist -and ($state.playlist.id -cne $entry.id -or
        ($state.playlist.corpusTracks -join ',') -cne ($entry.corpusTracks -join ','))) {
        throw 'formats: library playlist identity or ordered corpus changed since import'
    }
    $export = Join-Path $B 'formats-library.m3u8'
    Assert-FormatPath $export
    $lines = @('#EXTM3U'; $accepted.Values)
    [IO.File]::WriteAllText($export, (($lines -join "`r`n") + "`r`n"), [Text.UTF8Encoding]::new($false))
    $state.playlist = $entry
    Write-FormatImportState $state
    return $entry
}

function Confirm-FormatLibraryImport([string]$corpus) {
    $dir = Join-Path $corpus 'formats'
    $state = Get-FormatImportState $dir
    $staged = Get-FormatFiles $dir -Verify
    $entry = Find-FormatLibraryPlaylist $corpus $state.name $staged
    Save-FormatLibraryImport $state $entry $staged
}

function Restore-FormatLibraryImport([string]$corpus) {
    $dir = Join-Path $corpus 'formats'
    $staged = Get-FormatFiles $dir -Verify
    $manifest = Get-FormatManifest $dir
    # The manifest and import nonce in the name mark a playlist created by the kit.
    $pattern = '^RBSP Formats ' + $manifest.Substring(0, 16).ToLowerInvariant() + '-[a-f0-9]{12}$'
    $entry = Get-FormatLibrary $corpus -SubsetPattern $pattern -Staged $staged |
        Sort-Object order -Descending | Select-Object -First 1
    if ($entry) {
        $state = [pscustomobject]@{ manifest = $manifest; name = $entry.name; playlist = $null }
        Save-FormatLibraryImport $state $entry $staged
    }
}
