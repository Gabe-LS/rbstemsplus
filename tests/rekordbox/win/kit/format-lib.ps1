# The active subset manifest selects fixtures from the retained format media.
function Get-FormatRefusalPattern {
    '^One or more files were not imported\. Check the log file saved in the ["\u201c]Documents["\u201d] folder\.?$'
}

function Assert-FormatPath([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    $root = [IO.Path]::GetPathRoot($full)
    if ($root -ne ($env:SystemDrive + [IO.Path]::DirectorySeparatorChar)) { throw 'formats: destination must be on the system volume' }
    $p = $root
    foreach ($part in $full.Substring($root.Length).Split([IO.Path]::DirectorySeparatorChar)) {
        if (-not $part) { continue }
        $p = Join-Path $p $part
        try { $a = [IO.File]::GetAttributes($p) }
        catch [IO.FileNotFoundException], [IO.DirectoryNotFoundException] { break }
        if ($a -band [IO.FileAttributes]::ReparsePoint) { throw 'formats: reparse point in destination' }
    }
}

function Open-FormatLock {
    $dir = Join-Path $env:TEMP 'rbst'
    $file = Join-Path $dir 'format-stage.lock'
    Assert-FormatPath $file
    [void][IO.Directory]::CreateDirectory($dir)
    return [IO.File]::Open($file, 'OpenOrCreate', 'ReadWrite', 'None')
}

function Get-FormatFiles([string]$dir, [switch]$Verify, [switch]$Exact, [switch]$AllowNoSelection, [ref]$CatalogFile) {
    Assert-FormatPath $dir
    $files = @([IO.DirectoryInfo]::new($dir).EnumerateFileSystemInfos())
    foreach ($f in $files) {
        if ($f.Attributes -band ([IO.FileAttributes]::Directory -bor [IO.FileAttributes]::ReparsePoint)) {
            throw 'formats: owned directories must contain regular files only'
        }
    }
    $marker = Join-Path $dir 'RBSP-FORMATS'
    if (-not [IO.File]::Exists($marker) -or [IO.File]::ReadAllText($marker) -cne 'RBSP format subset v1') {
        throw 'formats: directory lacks the kit ownership marker. Preserve or move it manually before restaging.'
    }
    $byName = @{}
    foreach ($f in $files) {
        if ($f.Name -in 'RBSP-FORMATS', 'SHA256SUMS') { continue }
        if ($f.Name -cnotmatch '^(?:catalog(?:-[a-f0-9]{64})?\.txt|fmt-[a-zA-Z0-9._~-]+\.[a-zA-Z0-9]+)$' -or $byName.ContainsKey($f.Name)) {
            throw 'formats: unexpected file in subset'
        }
        $byName.Add($f.Name, $f)
    }
    if (-not $Verify) { return }
    $manifest = Join-Path $dir 'SHA256SUMS'
    if ($AllowNoSelection -and -not [IO.File]::Exists($manifest)) { return }
    if (-not [IO.File]::Exists($manifest)) { throw 'formats: no active format selection; stage a verified format subset first' }
    $rows = @{}
    $catalogPath = $null
    foreach ($l in [IO.File]::ReadAllLines($manifest)) {
        if ($l -cnotmatch '^([a-f0-9]{64})  (catalog(?:-[a-f0-9]{64})?\.txt|fmt-[a-zA-Z0-9._~-]+\.[a-zA-Z0-9]+)$') { throw 'formats: invalid subset manifest' }
        $sha = $Matches[1]; $name = $Matches[2]
        $isCatalog = $name -clike 'catalog*.txt'
        if ($isCatalog -and ($catalogPath -or $name -cnotin 'catalog.txt', "catalog-$sha.txt")) { throw 'formats: invalid catalog entry in subset manifest' }
        $f = $byName[$name]
        if (-not $f -or $rows.ContainsKey($f.BaseName) -or (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash -ne $sha) {
            throw "formats: subset does not match its manifest ($name)"
        }
        if ($isCatalog) { $catalogPath = $f.FullName }
        else { $rows.Add($f.BaseName, $f.FullName) }
        $byName.Remove($name)
    }
    if (-not $rows.Count -or ($Exact -and $byName.Count)) { throw 'formats: empty or incomplete subset manifest' }
    if ($CatalogFile) { $CatalogFile.Value = $catalogPath }
    return $rows
}

function Get-FormatCatalog([string]$path, $staged) {
    if (-not $path) { throw 'formats: restage the subset to include the full format catalog' }
    $catalog = [IO.File]::ReadAllLines($path)
    $seen = @{}
    foreach ($id in $catalog) {
        if ($id -cnotmatch '^fmt-[a-z0-9._~-]+$' -or $seen.ContainsKey($id)) { throw 'formats: invalid format catalog' }
        $seen.Add($id, $true)
    }
    foreach ($id in $staged.Keys) {
        if (-not $seen.ContainsKey($id)) { throw 'formats: a staged row is missing from the format catalog' }
    }
    return $catalog
}

function Write-FormatM3U8([string]$path, $staged) {
    Assert-FormatPath $path
    $lines = @('#EXTM3U'; foreach ($id in ($staged.Keys | Sort-Object)) { $staged[$id] })
    # Windows PowerShell's UTF8 means a BOM. Write explicit UTF-8 with Windows line endings.
    [IO.File]::WriteAllText($path, (($lines -join "`r`n") + "`r`n"), [Text.UTF8Encoding]::new($false))
}

function Get-FormatManifest([string]$dir) {
    (Get-FileHash -LiteralPath (Join-Path $dir 'SHA256SUMS')).Hash
}

function Get-FormatImportState([string]$dir) {
    $path = Join-Path $B 'format-import-state.json'
    Assert-FormatPath $path
    if (-not [IO.File]::Exists($path)) { throw 'formats: no automated import was recorded for this subset' }
    try { $state = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($path)) }
    catch { throw 'formats: invalid automated import state' }
    if ($state.manifest -cne (Get-FormatManifest $dir) -or
        $state.name -cnotmatch '^RBSP Formats [a-f0-9]{16}-[a-f0-9]{12}$' -or
        -not $state.name.StartsWith(('RBSP Formats ' + $state.manifest.Substring(0, 16).ToLowerInvariant() + '-'))) {
        throw 'formats: the recorded import is for another subset'
    }
    $state
}

function Write-FormatImportState($state) {
    $path = Join-Path $B 'format-import-state.json'
    $tmp = Join-Path $B ('format-state-' + [guid]::NewGuid().ToString('N') + '.json')
    Assert-FormatPath $path
    Assert-FormatPath $tmp
    try {
        [IO.File]::WriteAllText($tmp, (ConvertTo-Json -InputObject $state -Depth 5))
        if ([IO.File]::Exists($path)) { [IO.File]::Replace($tmp, $path, [NullString]::Value) }
        else { [IO.File]::Move($tmp, $path) }
    } finally { [IO.File]::Delete($tmp) }
}

function Get-FormatImportOutcomes($staged, $imported) {
    foreach ($id in ($staged.Keys | Sort-Object)) {
        if ($imported -contains $id) { "$id IMPORT_ADDED" }
        else { "$id IMPORT_REFUSED" }
    }
}
