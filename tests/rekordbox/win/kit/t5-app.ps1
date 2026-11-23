# t5-app.ps1: stage's read-only T5 identity check and the dedicated drive's test rows.
# Prelude: $Phase (check, prep, list, finish, rows or show), $T5Serial (owner's pin),
# $Protected (disk identities to refuse), $TestUser (prep), $Wintest and $IdleMinutes (rows).
# Prep creates \rbsp-test and its marker. List and finish compare the surrounding listing
# with prep's saved snapshot. Rows run J28 and the sleeping-disk probe in one session.
$ErrorActionPreference = 'Stop'
$Model = 'Samsung Portable SSD T5'
$Folder = 'rbsp-test'; $Marker = '.rbsp-kit-folder'
$Saved = Join-Path $env:USERPROFILE 'rbsp-kit-admin\t5-app-before.txt'
function Say-Exit([string]$line, [int]$code) { [Console]::Out.WriteLine($line); exit $code }
function Test-ProtectionList($entries) {
    if (-not $entries) { return $false }
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    foreach ($entry in $entries) {
        try {
            $bytes = [Convert]::FromBase64String($entry)
            $identity = $utf8.GetString($bytes)
        } catch { return $false }
        if ([Convert]::ToBase64String($bytes) -cne $entry -or $identity -match '[\p{Cc}]' -or
            $identity -notmatch '^([^|]+)\|([^|]+)$' -or
            [string]::IsNullOrWhiteSpace($Matches[2]) -or $Matches[1].Trim() -cne $Matches[1]) { return $false }
    }
    return $true
}
# a disk's protected key: base64 of "SERIAL|FriendlyName" (not the UniqueId: the T5 and the ROG
# backup disk answer the same UniqueId, 5000000000000001; PC 2026-11-20)
function Get-Key($d) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(([string]$d.SerialNumber).Trim() + '|' + $d.FriendlyName)) }
# built in, whatever git config holds: the owner's ROG backup disk by its model, and any disk with
# an "Acronis" volume; its volumes unreadable is a refusal too (fail closed)
function Test-HardRefused($d) {
    if ($d.FriendlyName -eq 'ROG ESD-S1C') { return "the ROG ESD-S1C (built-in refusal)" }
    try { $labels = @(Get-Partition -DiskNumber $d.Number -ErrorAction Stop | Get-Volume -ErrorAction Stop | ForEach-Object { [string]$_.FileSystemLabel }) }
    catch { return "its volumes can't be read (built-in refusal)" }
    if (@($labels | Where-Object { $_ -like '*Acronis*' }).Count) { return 'it has an Acronis volume (built-in refusal)' }
    return ''
}
# the disk never to touch: built in (above), a protected key, no serial, boot or system
function Test-Refused($d) {
    $hard = Test-HardRefused $d; if ($hard) { return $hard }
    if (-not ([string]$d.SerialNumber).Trim() -or $Protected -ccontains (Get-Key $d)) { return 'a protected disk or no serial' }
    if ($d.IsBoot -or $d.IsSystem) { return 'a boot or system disk' }
    return ''
}
# Select by the owner's pin, independent of disk number, drive letter and prep state.
function Find-T5Disk {
    if (-not $T5Serial) { Say-Exit 'T5: refused: no configured test-drive serial' 4 }
    $t5 = @(Get-Disk | Where-Object {
        $_.FriendlyName -eq $Model -and [string]$_.BusType -eq 'USB' -and
        ([string]$_.SerialNumber).Trim() -ceq $T5Serial
    })
    if ($t5.Count -ne 1) { Say-Exit "T5: expected one '$Model' on USB with the configured serial, found $($t5.Count)" 3 }
    $why = Test-Refused $t5[0]; if ($why) { Say-Exit "T5: refused: disk $($t5[0].Number): $why" 4 }
    return $t5[0]
}
function Find-T5Root($d) {
    $parts = @(Get-Partition -DiskNumber $d.Number | Where-Object { $_.DriveLetter })
    if ($parts.Count -ne 1) { Say-Exit "T5: refused: expected one lettered partition, found $($parts.Count)" 4 }
    return "$($parts[0].DriveLetter):\"
}
function Assert-KitFolder([string]$own) {
    $folderItem = Get-Item -LiteralPath $own -Force -ErrorAction SilentlyContinue
    if (-not $folderItem) { return }
    $markerItem = Get-Item -LiteralPath (Join-Path $own $Marker) -Force -ErrorAction SilentlyContinue
    if (-not $folderItem.PSIsContainer -or ($folderItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        -not $markerItem -or $markerItem.PSIsContainer -or ($markerItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Say-Exit "T5: refused: $own is not the kit's folder with its marker" 4
    }
}
function New-KitFolder([string]$own) {
    if (Test-Path -LiteralPath $own) { return }
    [void](New-Item -ItemType Directory -Path $own)
    [void](New-Item -ItemType File -Path (Join-Path $own $Marker) -Value 'made by the rekordbox test kit')
}
function Get-Listing([string]$root) {
    $own = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath((Join-Path $root $Folder))
    @(Get-ChildItem -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue | Where-Object { $_.FullName -ne $own -and -not $_.FullName.StartsWith($own + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { "$($_.FullName)|$(if ($_.PSIsContainer) { 'dir' } else { $_.Length })|$($_.LastWriteTimeUtc.Ticks)" } | Sort-Object)
}
function Compare-Saved([string]$root) {
    $before = @(Get-Content -LiteralPath $Saved); $after = @(Get-Listing $root)
    $diff = @(Compare-Object $before $after)
    if ($diff.Count) { "CHANGED OUTSIDE OUR FOLDER: $($diff.Count) difference(s): " + (($diff | Select-Object -First 8 | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join '; ') }
    else { "CLEAN: the listing outside our folder matches ($($after.Count) entries)" }
}

function Get-Sha([string[]]$lines) {
    $b = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    -join ([Security.Cryptography.SHA256]::Create().ComputeHash($b) | ForEach-Object { $_.ToString('x2') })
}

if ($MyInvocation.InvocationName -eq '.') { return }   # dot-sourced by the tests: the functions only
if ($Phase -eq 'show') {
    # read only: every disk's number, model, bus, offline state and serial's last 4 characters
    foreach ($d in @(Get-Disk | Sort-Object Number)) {
        $sn = ([string]$d.SerialNumber).Trim(); if ($sn.Length -gt 4) { $sn = $sn.Substring($sn.Length - 4) }
        "DISK $($d.Number) '$($d.FriendlyName)' bus $($d.BusType) offline $($d.IsOffline) serial ...$sn uid-shared $(@(Get-Disk | Where-Object { $_.UniqueId -eq $d.UniqueId }).Count)"
    }
    exit 0
}
if ($Phase -notin 'check', 'prep', 'list', 'finish', 'rows') { Say-Exit "t5-app: unknown phase '$Phase'" 2 }
if (-not (Test-ProtectionList $Protected)) {
    Say-Exit 'T5: refused: no valid protected disk list (rbstemsplus.pcProtectedDisks): no disk action' 4
}
$d5 = Find-T5Disk
if ($Phase -eq 'check') { "T5: verified '$Model' on USB (disk $($d5.Number))"; exit 0 }
$root = Find-T5Root $d5
$own = Join-Path $root $Folder
Assert-KitFolder $own
$v = [IO.DriveInfo]::new($root)
if ($Phase -eq 'rows') {
    "T5: disk $($d5.Number) '$($d5.FriendlyName)' bus $($d5.BusType) $([math]::Round($d5.Size / 1GB)) GB id $(Get-Key $d5)"
    "T5: volume $root '$($v.VolumeLabel)' $($v.DriveFormat) $($v.DriveType)"
} else {
    "T5: volume $root '$($v.VolumeLabel)' $($v.DriveFormat), $([math]::Round($v.AvailableFreeSpace / 1GB)) GB free"
}
switch ($Phase) {
    'prep' {
        $l = @(Get-Listing $root); Set-Content -LiteralPath $Saved -Value $l
        "LISTING saved: $($l.Count) entries outside our folder"
        New-KitFolder $own
        if ($v.DriveFormat -eq 'NTFS') { $o = & icacls.exe $own /grant "${TestUser}:(OI)(CI)M" 2>&1; "ACL: $TestUser may write in $own ($LASTEXITCODE)" }
        "T5FOLDER $own"
    }
    'list' {
        "ROOT: " + ((@(Get-ChildItem -LiteralPath $root -Force | ForEach-Object { $_.Name }) | Sort-Object) -join ', ')
        if (Test-Path -LiteralPath $own) {
            foreach ($c in @(Get-ChildItem -LiteralPath $own -Force)) {
                $f = @(if ($c.PSIsContainer) { Get-ChildItem -LiteralPath $c.FullName -Recurse -Force -File -ErrorAction SilentlyContinue } else { $c })
                "OURS: $($c.Name): $($f.Count) file(s), $([math]::Round((($f | Measure-Object Length -Sum).Sum) / 1MB, 1)) MB"
            }
        } else { "OURS: $own is gone" }
        Compare-Saved $root
    }
    'finish' {
        if (Test-Path -LiteralPath $own) { Remove-Item -LiteralPath $own -Recurse -Force }
        "OURS: gone $(-not (Test-Path -LiteralPath $own))"
        Compare-Saved $root
    }
    'rows' {
        $before = @(Get-Listing $root)
        "LISTING before: $($before.Count) entries, sha $(Get-Sha $before)"
        New-KitFolder $own
        try {
            $exe = Join-Path $env:USERPROFILE 'rbsp-kit-admin\winc-location-wintest.exe'   # copied there by pc-session.sh t5
            $sha = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($sha -ne $Wintest) { "J28: refused: winc-location-wintest.exe hash differs"; exit 4 }
            foreach ($p in $own, $root) { foreach ($l in @(& $exe choice-probe $p 2>&1)) { "J28 ${p}: $l" } }
            "SLEEP: nothing touches $root for $IdleMinutes min"
            Start-Sleep -Seconds ([int]($IdleMinutes * 60))
            foreach ($m in 'free', 'volname') {
                $t = [Diagnostics.Stopwatch]::StartNew(); $out = @(& $exe $m $own 2>&1) -join ' / '
                "SLEEP: first $m after the idle time: $([math]::Round($t.Elapsed.TotalSeconds, 2)) s ($out)"
            }
        } finally {
            if ((Find-T5Root (Find-T5Disk)) -ne $root) { Say-Exit 'T5: refused: the test drive moved during the row' 4 }
            Assert-KitFolder $own
            Remove-Item -LiteralPath (Join-Path $own $Marker) -Force
            Remove-Item -LiteralPath $own   # empty now: only ever our own folder
            $after = @(Get-Listing $root)
            "LISTING after: $($after.Count) entries, sha $(Get-Sha $after)"
            $diff = @(Compare-Object $before $after)
            if ($diff.Count) { "CLEAN: CHANGED: $($diff.Count) difference(s): " + (($diff | Select-Object -First 5 | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join '; ') }
            else { "CLEAN: the listing matches (our folder gone: $(-not (Test-Path -LiteralPath $own)))" }
        }
    }
}
