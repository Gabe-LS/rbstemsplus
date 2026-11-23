<#
rbstate.ps1: the test account's per-user rekordbox state, snapshotted and put back.
Run as the test account, rekordbox closed. Reads its rekordbox folders; writes only under
%TEMP%\rbst\ except in -Mode Restore without -DryRun, which puts a snapshot back into the roots.
Never machine-wide state: no HKLM, no ProgramData, no Program Files (activation stays as it is).
Prints counts, sizes and paths of the account's own files only; never file content, never the db key.

  -Mode Snapshot                    copy every root into %TEMP%\rbst\<time>\, hash both sides,
                                    write manifest.tsv (root, relative path, size, sha256), roots.tsv
                                    (root id, path, present) and reg-*.reg (HKCU\Software\Pioneer)
  -Mode Verify -Snapshot <time>     hash the snapshot's own copies against its manifest
  -Mode Restore -Snapshot <time>    put the snapshot back: each root's current folder is moved to
            [-DryRun]               %TEMP%\rbst\<time>-pre-restore-<now>\ first (so a restore can be
                                    undone), then the snapshot's files are copied in and every
                                    root is hashed against the manifest (no extra file, none
                                    missing); HKCU\Software\Pioneer likewise. -DryRun (or -WhatIf)
                                    changes nothing: it says, per root, what would be removed,
                                    replaced or added, and checks the snapshot first
  -Mode Inventory -Name <label>     a list of every file under the profile (but %TEMP%) with size and
                                    write time, ProgramData's Pioneer folders, and an export of
                                    HKCU\Software, into %TEMP%\rbst\inv-<label>\ (for the
                                    empty-library launch's writes: before and after)
  -Mode Diff -Name <a> -Other <b>   what changed between two inventories: files added, changed,
                                    removed, and registry keys added or removed
Exit 0: done and verified; 1: a check failed (said); 2: refused (rekordbox running, bad arguments).
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Snapshot', 'Verify', 'Restore', 'Inventory', 'Diff')][string]$Mode,
    [string]$Snapshot,
    [string]$Name,
    [string]$Other,
    [switch]$DryRun,
    [switch]$List
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
if ($WhatIfPreference) { $DryRun = $true }

$Base = Join-Path $env:TEMP 'rbst'
# the per-user roots rekordbox 7 writes for this account (found 2026-11-18: options.json's db-path,
# analysis-data-root-path and settings-root-path all under %APPDATA%\Pioneer), plus PioneerLog
# (PC session 1: a launch and a track load wrote PSvLink/NFSd logs there, outside roots 1-4).
# A snapshot keeps its own roots.tsv: a restore of an older snapshot (roots 1-4) leaves root 5 as it is.
$Roots = [ordered]@{
    '1' = Join-Path $env:APPDATA 'Pioneer'
    '2' = Join-Path $env:USERPROFILE 'Music\PioneerDJ'
    '3' = Join-Path $env:USERPROFILE 'Documents\rekordbox'
    '4' = Join-Path $env:LOCALAPPDATA 'Pioneer'
    '5' = Join-Path $env:APPDATA 'PioneerLog'
}
$RegKey = 'HKCU\Software\Pioneer'
$RbProcs = 'rekordbox', 'rekordboxAgent', 'edb_streamd', 'rbHttpServer', 'rbinit'

function Say([string]$s) { Write-Host $s }
function Fail([string]$s) { Write-Host "FAIL: $s"; exit 1 }
function Refuse([string]$s) { Write-Host "REFUSED: $s"; exit 2 }

function Assert-Closed {
    $p = @(Get-Process -Name $RbProcs -ErrorAction SilentlyContinue)
    if ($p.Count) { Refuse "rekordbox is running ($(($p | ForEach-Object { $_.ProcessName }) -join ', ')): quit it first (its db and sidecars must be quiet)" }
}

function Get-Tree([string]$root) {
    # every file under root, relative path, size; reparse points reported, never followed
    $out = New-Object System.Collections.Generic.List[object]
    if (-not (Test-Path -LiteralPath $root)) { return $out }
    $stack = New-Object System.Collections.Generic.Stack[string]
    $stack.Push($root)
    while ($stack.Count) {
        $d = $stack.Pop()
        foreach ($e in Get-ChildItem -LiteralPath $d -Force) {
            if ($e.Attributes -band [IO.FileAttributes]::ReparsePoint) { Say "  note: a reparse point, not followed: $($e.FullName)"; continue }
            if ($e.PSIsContainer) { $stack.Push($e.FullName) }
            else { $out.Add([pscustomobject]@{ Rel = $e.FullName.Substring($root.Length).TrimStart('\'); Full = $e.FullName; Size = $e.Length; Time = $e.LastWriteTimeUtc }) }
        }
    }
    return $out.ToArray()
}

function Hash([string]$p) { return (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() }

function Read-Manifest([string]$dir) {
    $m = @{}
    foreach ($l in [IO.File]::ReadAllLines((Join-Path $dir 'manifest.tsv'))) {
        if (-not $l -or $l.StartsWith('#')) { continue }
        $f = $l.Split("`t")
        $m["$($f[0])`t$($f[1])"] = [pscustomobject]@{ Root = $f[0]; Rel = $f[1]; Size = [int64]$f[2]; Sha = $f[3] }
    }
    return $m
}

function Read-Roots([string]$dir) {
    $r = [ordered]@{}
    foreach ($l in [IO.File]::ReadAllLines((Join-Path $dir 'roots.tsv'))) {
        if (-not $l -or $l.StartsWith('#')) { continue }
        $f = $l.Split("`t")
        $r[$f[0]] = [pscustomobject]@{ Path = $f[1]; Present = ($f[2] -eq 'present') }
    }
    return $r
}

function Get-SnapDir {
    if (-not $Snapshot) { Refuse '-Snapshot <time> is needed (a folder name under %TEMP%\rbst)' }
    $d = Join-Path $Base $Snapshot
    if (-not (Test-Path -LiteralPath (Join-Path $d 'manifest.tsv'))) { Refuse "no snapshot $d (no manifest.tsv)" }
    return $d
}

function Test-Snapshot([string]$dir) {
    $m = Read-Manifest $dir
    $bad = 0
    foreach ($e in $m.Values) {
        $p = Join-Path (Join-Path $dir $e.Root) $e.Rel
        if (-not (Test-Path -LiteralPath $p)) { Say "  missing in the snapshot: $($e.Root)\$($e.Rel)"; $bad++; continue }
        if ((Hash $p) -ne $e.Sha) { Say "  damaged in the snapshot: $($e.Root)\$($e.Rel)"; $bad++ }
    }
    Say "snapshot $(Split-Path -Leaf $dir): $($m.Count) files checked against manifest.tsv, $bad bad"
    return $bad
}

switch ($Mode) {
'Snapshot' {
    Assert-Closed
    $ts = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dir = Join-Path $Base $ts
    if (Test-Path -LiteralPath $dir) { Refuse "$dir is already there" }
    [void][IO.Directory]::CreateDirectory($dir)
    $man = New-Object System.Collections.Generic.List[string]
    $man.Add("# root`trelative path`tsize`tsha256 (rbstate.ps1 snapshot $ts, $env:USERDOMAIN\$env:USERNAME)")
    $rootLines = New-Object System.Collections.Generic.List[string]
    $rootLines.Add("# id`tpath`tpresent|absent")
    $files = 0; $bytes = 0
    foreach ($id in $Roots.Keys) {
        $root = $Roots[$id]
        if (-not (Test-Path -LiteralPath $root)) { $rootLines.Add("$id`t$root`tabsent"); Say "root $id $root : absent (the restore removes it if it appears)"; continue }
        $rootLines.Add("$id`t$root`tpresent")
        $t = @(Get-Tree $root)
        $n = 0; $b = 0
        foreach ($f in $t) {
            $src = Hash $f.Full
            $dst = Join-Path (Join-Path $dir $id) $f.Rel
            [void][IO.Directory]::CreateDirectory((Split-Path -Parent $dst))
            Copy-Item -LiteralPath $f.Full -Destination $dst -Force
            $cp = Hash $dst
            if ($cp -ne $src) { Fail "the copy of $root\$($f.Rel) differs from its source" }
            # the source again: a file that changed while copied means rekordbox (or a helper) wrote it
            if ((Hash $f.Full) -ne $src) { Fail "$root\$($f.Rel) changed while it was copied: something writes there (rekordbox's helpers?)" }
            $man.Add("$id`t$($f.Rel)`t$($f.Size)`t$src")
            $n++; $b += $f.Size
        }
        Say "root $id $root : $n files, $([math]::Round($b / 1MB, 1)) MB, copied and hashed both sides"
        $files += $n; $bytes += $b
    }
    $regFile = Join-Path $dir 'reg-pioneer.reg'
    if (Test-Path 'HKCU:\Software\Pioneer') {
        & reg.exe export $RegKey $regFile /y | Out-Null
        if ($LASTEXITCODE) { Fail "reg export $RegKey failed ($LASTEXITCODE)" }
        $rootLines.Add("reg`t$RegKey`tpresent"); Say "registry $RegKey : exported"
    } else { $rootLines.Add("reg`t$RegKey`tabsent"); Say "registry $RegKey : absent" }
    [IO.File]::WriteAllLines((Join-Path $dir 'manifest.tsv'), $man)
    [IO.File]::WriteAllLines((Join-Path $dir 'roots.tsv'), $rootLines)
    Say "manifest.tsv sha256 $(Hash (Join-Path $dir 'manifest.tsv'))"
    Assert-Closed
    Say "SNAPSHOT $ts : $files files, $([math]::Round($bytes / 1MB, 1)) MB in $dir"
    exit 0
}
'Verify' {
    $dir = Get-SnapDir
    if (Test-Snapshot $dir) { Fail 'the snapshot does not match its manifest' }
    exit 0
}
'Restore' {
    Assert-Closed
    $dir = Get-SnapDir
    Say "$(if ($DryRun) { 'DRY RUN (nothing changed)' } else { 'RESTORE' }) from $dir"
    if (Test-Snapshot $dir) { Fail 'the snapshot does not match its manifest: nothing restored' }
    $m = Read-Manifest $dir
    $r = Read-Roots $dir
    $now = Get-Date -Format 'yyyyMMdd-HHmmss'
    $aside = Join-Path $Base "$Snapshot-pre-restore-$now"
    foreach ($id in $r.Keys) {
        if ($id -eq 'reg') { continue }
        $root = $r[$id].Path
        $want = @($m.Values | Where-Object { $_.Root -eq $id })
        $have = @(Get-Tree $root)
        $haveBy = @{}; foreach ($f in $have) { $haveBy[$f.Rel] = $f }
        $wantBy = @{}; foreach ($e in $want) { $wantBy[$e.Rel] = $e }
        $extra = @($have | Where-Object { -not $wantBy.ContainsKey($_.Rel) })
        $missing = @($want | Where-Object { -not $haveBy.ContainsKey($_.Rel) })
        $changed = @($want | Where-Object { $haveBy.ContainsKey($_.Rel) -and (($haveBy[$_.Rel].Size -ne $_.Size) -or ((Hash $haveBy[$_.Rel].Full) -ne $_.Sha)) })
        $state = if ($r[$id].Present) { 'present in the snapshot' } else { 'absent in the snapshot' }
        Say "root $id $root ($state): now $($have.Count) files; would remove $($extra.Count), replace $($changed.Count), add back $($missing.Count)"
        if ($List) {
            foreach ($f in $extra) { Say "    remove  $($f.Rel)" }
            foreach ($f in $changed) { Say "    replace $($f.Rel)" }
            foreach ($f in $missing) { Say "    add     $($f.Rel)" }
        }
        if ($DryRun) { continue }
        if (Test-Path -LiteralPath $root) {
            [void][IO.Directory]::CreateDirectory($aside)
            Move-Item -LiteralPath $root -Destination (Join-Path $aside $id)
            Say "  the current folder moved aside to $aside\$id"
        }
        if ($r[$id].Present) {
            [void][IO.Directory]::CreateDirectory($root)
            foreach ($e in $want) {
                $dst = Join-Path $root $e.Rel
                [void][IO.Directory]::CreateDirectory((Split-Path -Parent $dst))
                Copy-Item -LiteralPath (Join-Path (Join-Path $dir $id) $e.Rel) -Destination $dst -Force
            }
            # empty folders in the snapshot are not recorded; rekordbox makes its own again
        }
    }
    $reg = $r['reg']
    $regNow = Test-Path 'HKCU:\Software\Pioneer'
    Say "registry $RegKey : $(if ($reg.Present) { 'present' } else { 'absent' }) in the snapshot, $(if ($regNow) { 'present' } else { 'absent' }) now"
    if (-not $DryRun) {
        if ($regNow) {
            [void][IO.Directory]::CreateDirectory($aside)
            & reg.exe export $RegKey (Join-Path $aside 'reg-pioneer.reg') /y | Out-Null
            & reg.exe delete $RegKey /f | Out-Null
            if ($LASTEXITCODE) { Fail "reg delete $RegKey failed" }
        }
        if ($reg.Present) { & reg.exe import (Join-Path $dir 'reg-pioneer.reg') 2>&1 | Out-Null; if ($LASTEXITCODE) { Fail "reg import failed" } }
    }
    if ($DryRun) { Say 'DRY RUN done: nothing changed'; exit 0 }
    # verify: every root is exactly the snapshot
    $bad = 0
    foreach ($id in $r.Keys) {
        if ($id -eq 'reg') { continue }
        $root = $r[$id].Path
        $have = @(Get-Tree $root)
        $want = @($m.Values | Where-Object { $_.Root -eq $id })
        if (-not $r[$id].Present) { if (Test-Path -LiteralPath $root) { Say "  $root should be absent"; $bad++ }; continue }
        if ($have.Count -ne $want.Count) { Say "  $root holds $($have.Count) files, the snapshot $($want.Count)"; $bad++ }
        foreach ($e in $want) { $p = Join-Path $root $e.Rel; if (-not (Test-Path -LiteralPath $p) -or (Hash $p) -ne $e.Sha) { Say "  differs: $id\$($e.Rel)"; $bad++ } }
    }
    if ($bad) { Fail "the restore doesn't match the snapshot ($bad); the state before it is in $aside" }
    Say "RESTORED $Snapshot and verified ($($m.Count) files); the state before it is in $aside (delete it once happy)"
    exit 0
}
'Inventory' {
    if (-not $Name -or $Name -notmatch '^[A-Za-z0-9_-]+$') { Refuse '-Name <label> (letters, digits, - and _)' }
    $dir = Join-Path $Base "inv-$Name"
    if (Test-Path -LiteralPath $dir) { Refuse "$dir is already there" }
    [void][IO.Directory]::CreateDirectory($dir)
    $tmp = $env:TEMP.TrimEnd('\') + '\'
    # %TEMP% isn't inventoried (rbstate's own folder is there); a profile inside %TEMP% (the
    # self-test's fake one) skips only rbstate's folder
    if ($env:USERPROFILE.StartsWith($tmp, [StringComparison]::OrdinalIgnoreCase)) { $tmp = $Base.TrimEnd('\') + '\' }
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($f in Get-ChildItem -LiteralPath $env:USERPROFILE -Recurse -Force -File -ErrorAction SilentlyContinue) {
        if ($f.FullName.StartsWith($tmp, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $lines.Add("$($f.FullName)`t$($f.Length)`t$($f.LastWriteTimeUtc.Ticks)")
    }
    foreach ($pd in @((Join-Path $env:ProgramData 'Pioneer'), (Join-Path $env:ProgramData 'rekordbox'), (Join-Path $env:PUBLIC 'Documents\Pioneer'))) {
        foreach ($f in Get-ChildItem -LiteralPath $pd -Recurse -Force -File -ErrorAction SilentlyContinue) { $lines.Add("$($f.FullName)`t$($f.Length)`t$($f.LastWriteTimeUtc.Ticks)") }
    }
    [IO.File]::WriteAllLines((Join-Path $dir 'files.tsv'), $lines)
    & reg.exe export 'HKCU\Software' (Join-Path $dir 'hkcu-software.reg') /y | Out-Null
    Say "INVENTORY $Name : $($lines.Count) files (profile but %TEMP%, ProgramData's Pioneer), HKCU\Software exported, in $dir"
    exit 0
}
'Diff' {
    if (-not $Name -or -not $Other) { Refuse '-Name <before> -Other <after>' }
    $a = Join-Path $Base "inv-$Name"; $b = Join-Path $Base "inv-$Other"
    foreach ($d in $a, $b) { if (-not (Test-Path -LiteralPath (Join-Path $d 'files.tsv'))) { Refuse "no inventory $d" } }
    function Load([string]$p) { $h = @{}; foreach ($l in [IO.File]::ReadAllLines($p)) { $f = $l.Split("`t"); $h[$f[0]] = "$($f[1])`t$($f[2])" }; return $h }
    $x = Load (Join-Path $a 'files.tsv'); $y = Load (Join-Path $b 'files.tsv')
    $added = @($y.Keys | Where-Object { -not $x.ContainsKey($_) } | Sort-Object)
    $removed = @($x.Keys | Where-Object { -not $y.ContainsKey($_) } | Sort-Object)
    $changed = @($y.Keys | Where-Object { $x.ContainsKey($_) -and $x[$_] -ne $y[$_] } | Sort-Object)
    Say "files: $($added.Count) added, $($changed.Count) changed, $($removed.Count) removed"
    foreach ($p in $added) { Say "  + $p" }
    foreach ($p in $changed) { Say "  ~ $p" }
    foreach ($p in $removed) { Say "  - $p" }
    function Keys([string]$p) { $k = @{}; foreach ($l in [IO.File]::ReadAllLines($p, [Text.Encoding]::Unicode)) { if ($l.StartsWith('[')) { $k[$l] = 1 } }; return $k }
    $ka = Keys (Join-Path $a 'hkcu-software.reg'); $kb = Keys (Join-Path $b 'hkcu-software.reg')
    $kAdd = @($kb.Keys | Where-Object { -not $ka.ContainsKey($_) } | Sort-Object)
    $kDel = @($ka.Keys | Where-Object { -not $kb.ContainsKey($_) } | Sort-Object)
    Say "HKCU\Software keys: $($kAdd.Count) added, $($kDel.Count) removed (values inside a key are not compared)"
    foreach ($k in $kAdd) { Say "  + $k" }
    foreach ($k in $kDel) { Say "  - $k" }
    function In-Roots([string]$p) { foreach ($r in $Roots.Values) { if ($p.StartsWith($r.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { return $true } }; return $false }
    $outside = @($added + $changed | Where-Object { -not (In-Roots $_) })
    Say "written outside rbstate's roots: $($outside.Count) (each one is either Windows' own or a root to add)"
    exit 0
}
}
