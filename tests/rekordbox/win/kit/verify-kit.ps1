# The PC kit checked on the PC, as the test account: every file of SHA256SUMS (sha256, relative path) is
# there with its hash, with no unlisted files. A corpus upload also rebuilds its long-path copy.
param([switch]$RefreshLongPath)
$ErrorActionPreference = 'Stop'
$kit = Split-Path -Parent $MyInvocation.MyCommand.Path
$sep = [IO.Path]::DirectorySeparatorChar; $ext = if ($sep -eq '\') { '\\?\' } else { '' }
function Files($dir, $prefix = '') {
    foreach ($f in [IO.DirectoryInfo]::new($dir).EnumerateFileSystemInfos()) {
        $rel = "$prefix$($f.Name)"
        if ($f.Name.StartsWith('.') -or ($f.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "dot path or reparse point: $rel" }
        if ($rel -match '^corpus/formats-(previous|incoming-[a-f0-9]{32})$') { continue }
        if ($rel -eq 'corpus/formats' -and [IO.File]::Exists([IO.Path]::Combine($f.FullName, 'RBSP-FORMATS'))) {
            . (Join-Path $kit 'format-lib.ps1')
            $null = Get-FormatFiles (Join-Path $kit 'corpus\formats') -Verify -AllowNoSelection
            continue
        }
        if ($f -is [IO.DirectoryInfo]) { Files "$dir$sep$($f.Name)" "$rel/" } else { $rel }
    }
}
function Hash($p) {
    $fs = [IO.File]::OpenRead($p)
    try { (Get-FileHash -InputStream $fs -Algorithm SHA256).Hash } finally { $fs.Dispose() }
}
$present = @{}; foreach ($p in Files "$ext$kit") { $present.Add($p, $true) }; $present.Remove('SHA256SUMS')
$n = 0; $bad = 0
foreach ($l in [IO.File]::ReadAllLines((Join-Path $kit 'SHA256SUMS'))) {
    if (-not $l.Trim() -or $l.StartsWith('#')) { continue }
    if ($l -notmatch '^([a-f0-9]{64})  (.+)$') { throw "bad manifest line: $l" }
    $sha = $Matches[1]; $rel = $Matches[2]
    if (-not $present.ContainsKey($rel)) { "missing or repeated: $rel"; $bad++; continue }
    $present.Remove($rel)
    if ((Hash "$ext$kit$sep$($rel.Replace([char]'/', $sep))") -ne $sha) { "differs: $rel"; $bad++ }
    $n++
}
$src = Join-Path $kit 'corpus\win\w16-flac-longpath.flac'
$seg = 'rbsp-long-path-segment-0123456789abcdefghij'
$dir = Join-Path $kit 'corpus\long'
while ($dir.Length -lt 250) { $dir = Join-Path $dir $seg }
$dst = Join-Path $dir 'w16-flac-longpath.flac'
$long = $dst.Substring($kit.Length + 1).Replace('\', '/'); $pointer = 'corpus/long-path.txt'
$refresh = $RefreshLongPath -and (Test-Path -LiteralPath $src -PathType Leaf)
if ($refresh) { $present.Remove($long); $present.Remove($pointer) }
if (-not $n) { 'empty SHA256SUMS'; $bad++ }
foreach ($p in $present.Keys) { "unlisted: $p"; $bad++ }
"SHA256SUMS: $n files present, $bad bad"
if ($bad) { exit 1 }
if (-not $refresh) { exit 0 }
[void][IO.Directory]::CreateDirectory("$ext$dir")
[IO.File]::Copy("$ext$src", "$ext$dst", $true)
$h1 = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
$h2 = Hash "$ext$dst"
"long-path copy: $($dst.Length) characters, $(if ($h1 -eq $h2) { 'same bytes' } else { $bad++; 'DIFFERS' })"
if ($bad) { exit 1 }
[IO.File]::WriteAllText((Join-Path $kit 'corpus\long-path.txt'), $dst)
[IO.File]::AppendAllLines((Join-Path $kit 'SHA256SUMS'), [string[]]@("$h2  $long", "$(Hash (Join-Path $kit $pointer))  $pointer"))
"its path is in corpus\long-path.txt"
