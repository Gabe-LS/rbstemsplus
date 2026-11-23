# Add verified fixtures at their permanent paths, then publish the subset manifest.
param([ValidateSet('prepare', 'publish', 'discard', 'clear')][string]$Mode,
      [ValidatePattern('^[a-f0-9]{32}$')][string]$Transfer,
      [ValidatePattern('^[a-f0-9]{64}$')][string]$ManifestSha)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'format-lib.ps1')
$corpus = Join-Path $PSScriptRoot 'corpus'
$active = Join-Path $corpus 'formats'
$backup = Join-Path $corpus 'formats-previous'
$pending = Join-Path $corpus "formats-incoming-$Transfer"
foreach ($p in $active, $backup, $pending) { Assert-FormatPath $p }
[void][IO.Directory]::CreateDirectory($corpus)
$lock = Open-FormatLock
function Remove-FormatDirectory([string]$dir) {
    Get-FormatFiles $dir
    $marker = Join-Path $dir 'RBSP-FORMATS'
    foreach ($f in [IO.Directory]::EnumerateFiles($dir)) {
        if ($f -ne $marker) { Remove-Item -LiteralPath $f -Force }
    }
    Remove-Item -LiteralPath $marker -Force
    [IO.Directory]::Delete($dir)
}
try {
    foreach ($p in $active, $backup, $pending) { Assert-FormatPath $p }
    if ($Mode -eq 'discard') {
        if (Test-Path -LiteralPath $pending) { Remove-FormatDirectory $pending }
        exit 0
    }
    if (Test-Path -LiteralPath $backup) { throw 'formats: legacy backup directory requires review before staging' }
    $hasActive = Test-Path -LiteralPath $active
    if ($hasActive) { $null = Get-FormatFiles $active -Verify -AllowNoSelection }
    if ($Mode -in 'prepare', 'clear' -and (Test-Path -LiteralPath $pending)) { throw 'formats: transfer folder already exists' }
    if ($Mode -eq 'prepare') {
        [void][IO.Directory]::CreateDirectory($pending)
        [IO.File]::WriteAllText((Join-Path $pending 'RBSP-FORMATS'), 'RBSP format subset v1')
        'FORMATS prepare'
        exit 0
    }
    $activeManifest = Join-Path $active 'SHA256SUMS'
    if ($Mode -eq 'publish') {
        $catalog = $null
        $incoming = Get-FormatFiles $pending -Verify -Exact -CatalogFile ([ref]$catalog)
        $manifest = Join-Path $pending 'SHA256SUMS'
        if ((Get-FileHash -LiteralPath $manifest).Hash -ne $ManifestSha) {
            throw 'formats: the uploaded manifest differs from the local subset'
        }
        if (-not $hasActive) {
            [IO.Directory]::Move($pending, $active)
        } else {
            $new = @{}
            $sources = @($incoming.Values)
            # The catalog filename includes its hash, so the previous manifest stays valid.
            if ($catalog) { $sources += $catalog }
            foreach ($source in $sources) {
                $name = [IO.Path]::GetFileName($source)
                $dest = Join-Path $active $name
                if ([IO.File]::Exists($dest)) {
                    if ((Get-FileHash -LiteralPath $dest).Hash -ne (Get-FileHash -LiteralPath $source).Hash) {
                        throw "formats: SHA-256 mismatch for existing fixture $name"
                    }
                } else { $new.Add($source, $dest) }
            }
            # Validate every collision before adding anything. A partial move leaves the
            # previous manifest valid. A retry checks any files that already arrived.
            foreach ($source in $new.Keys) { [IO.File]::Move($source, $new[$source]) }
            if ([IO.File]::Exists($activeManifest)) { [IO.File]::Replace($manifest, $activeManifest, [NullString]::Value) }
            else { [IO.File]::Move($manifest, $activeManifest) }
            Remove-FormatDirectory $pending
        }
    } elseif ($hasActive) {
        [IO.File]::Delete($activeManifest)
    }
    "FORMATS $Mode"
} finally { $lock.Dispose() }
