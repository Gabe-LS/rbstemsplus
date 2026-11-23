# probe.ps1: the research probe (helper.md section 6; never shipped) in rekordbox's place, for T2's
# mix dumps, through the owner's admin SSH login. pc-session.sh sends this file's text as the
# owner's script after a line that sets $Mode, $Sid (the test account's SID) and $ProbeSha (the
# staged probe's SHA-256, computed on the Mac), and optionally $RbVersion; never run from the test
# account's %TEMP%. $ResolveOnly identifies the install before the driver asks it to quit.
#   state    rekordbox's onnxruntime.dll (pioneer, probe, other) and C:\rbtest's dump count
#   install  rekordbox closed and its DLL Pioneer's (Stems Cache not installed): Pioneer's DLL to
#            C:\rbtest\real\ (where the probe loads it from; writable by administrators only), the
#            probe from the account's staged kit (probe\onnxruntime.dll) checked against $ProbeSha
#            and put in its place; C:\rbtest\dump made, the account allowed to write C:\rbtest
#            (the probe's log and dumps, written by rekordbox as the account)
#   remove   rekordbox closed: Pioneer's DLL back (its SHA-256 and signature checked), then
#            C:\rbtest removed (pc-session.sh copies the dumps and the log first)
# Output ends with "PROBE pioneer|probe|other ..." or "REFUSED: why". Exit 0 done; 1 failed; 2 refused.
$ErrorActionPreference = 'Stop'
$Pioneer = @{
    '7.2.19' = 'e7a702e13bd3e487705204d518c96f1f116aa25b0ac584741838e20bc1ff6e9f'
    '7.2.20' = '3e55ab558136c0cf413c1e3873faff7c28807bf4b178712a2124e5efd4e03acd'
}
function Refuse([string]$why) { "REFUSED: $why"; exit 2 }
if ($Mode -notin 'state', 'install', 'remove') { Refuse 'mode' }
if ($Sid -notmatch '^S-1-5-21-[0-9-]+$') { Refuse 'no test account SID' }
if ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -eq $Sid) { Refuse 'this is the test account, not the admin login' }
function Sha([string]$f) { (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash.ToLowerInvariant() }
function Assert-ProbePath([string]$path) {
    $root = [IO.Path]::GetPathRoot($path)
    if ($root -ne ($env:SystemDrive + [char]92)) { Refuse 'probe path off the system volume' }
    $p = $root
    foreach ($part in $path.Substring($root.Length).Split([char]92)) {
        $p = Join-Path $p $part
        try { $a = [IO.File]::GetAttributes($p) }
        catch [IO.FileNotFoundException], [IO.DirectoryNotFoundException] { break }
        if ($a -band [IO.FileAttributes]::ReparsePoint) { Refuse 'reparse point in probe path' }
    }
}
# One backup slot is shared by all installs and by every admin runner.
$mutex = [Threading.Mutex]::new($false, 'Global\RBSP Kit Probe')
$locked = $false
try {
try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
if (-not $locked) { Refuse 'another probe command is active' }
Trace-KitPhase 'probe: before rekordbox lookup'
if ($RbVersion -and $RbVersion -notmatch '^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}$') { Refuse 'invalid rekordbox version' }
$base = 'C:\rbtest'
$real = Join-Path $base 'real\onnxruntime.dll'
$target = Join-Path $base 'real\rekordbox-version.txt'
# Scoped runs reserve a fresh probe directory, so cleanup can distinguish a partial
# installation owned by this run from a probe or evidence that was already present.
$runMarker = Join-Path $base 'real\format-run.txt'
if ($EvidenceRun -and $EvidenceRun -cnotmatch '^[a-f0-9]{32}$') { Refuse 'invalid probe evidence run ID' }
$st = Join-Path $base 'stage\onnxruntime.dll'
$dump = Join-Path $base 'dump'
foreach ($path in $real, $target, $runMarker, $st, $dump) { Assert-ProbePath $path }
if ($EvidenceRun -and $Mode -eq 'install' -and (Test-Path -LiteralPath $base)) {
    Refuse 'scoped runs require a fresh probe evidence directory'
}
$ownsRun = -not $EvidenceRun -or ((Test-Path -LiteralPath $runMarker -PathType Leaf) -and (Get-Content -LiteralPath $runMarker -Raw) -ceq $EvidenceRun)
$saved = $null
if ($ownsRun -and $Mode -in 'install', 'remove') {
    if (Test-Path -LiteralPath $target) {
        $saved = Get-Content -LiteralPath $target -Raw
        if (-not $saved -or -not $Pioneer.ContainsKey($saved)) { Refuse 'invalid saved rekordbox version' }
    } elseif (Test-Path -LiteralPath $real) {
        # Kits before version selection saved just the DLL. Its hash identifies the install.
        $hash = Sha $real
        $versions = @($Pioneer.Keys | Where-Object { $Pioneer[$_] -eq $hash })
        if ($versions.Count -ne 1) { Refuse 'the saved Pioneer DLL does not identify one rekordbox version' }
        $saved = $versions[0]
    }
}
if ($RbVersion -and $saved -and $RbVersion -ne $saved) { Refuse "the probe backup belongs to rekordbox $saved" }
$requested = if ($RbVersion) { $RbVersion } elseif ($Mode -eq 'remove') { $saved }
$d = @(Get-ChildItem -LiteralPath (Join-Path $env:ProgramFiles 'rekordbox') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^rekordbox (\d+(\.\d+)+)$' -and
        (-not $requested -or $_.Name -eq "rekordbox $requested") -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'rekordbox.exe') -PathType Leaf) } |
    Sort-Object { [version]($_.Name -replace '^rekordbox ', '') })
if (-not $d.Count) { if ($requested) { Refuse "rekordbox $requested is not installed" }; Refuse 'rekordbox is not installed' }
$ver = $d[-1].Name -replace '^rekordbox ', ''
$want = $Pioneer[$ver]; if (-not $want) { Refuse "no Pioneer hash known for rekordbox $ver" }
$dll = Join-Path $d[-1].FullName 'onnxruntime.dll'
Assert-ProbePath $dll
if ($saved -and $saved -ne $ver) { Refuse "the probe backup belongs to rekordbox $saved" }
function State {
    $s = Sha $dll
    $what = if ($s -eq $want) { 'pioneer' } elseif ($ProbeSha -and $s -eq $ProbeSha) { 'probe' } else { 'other ' + $s.Substring(0, 8) }
    $n = @(Get-ChildItem -LiteralPath $dump -Filter *.mix -ErrorAction SilentlyContinue).Count
    "PROBE $what (rekordbox $ver; $n mix dumps)"
}
function Restore-Pioneer {
    Trace-KitPhase 'probe: checking DLL before restore'
    $s = if ([IO.File]::Exists($dll)) { Sha $dll } else { 'missing' }
    if ($s -ne $want) {
        $short = if ($s -eq 'missing') { $s } else { $s.Substring(0, 8) }
        if (-not (Test-Path -LiteralPath $real) -or (Sha $real) -ne $want) { throw "no checked Pioneer DLL to put back (rekordbox's is $short)" }
        if ((Get-AuthenticodeSignature -LiteralPath $real).Status -ne 'Valid') { throw "Pioneer's saved DLL has no valid signature" }
        Trace-KitPhase 'probe: restoring DLL'
        Copy-Item -LiteralPath $real -Destination $dll -Force
        if ((Sha $dll) -ne $want) { throw "Pioneer's DLL not back" }
    }
    Trace-KitPhase 'probe: restore verified'
}
if ($Mode -eq 'state') { State; exit 0 }
if ($Mode -eq 'remove' -and -not $ownsRun) {
    # Install may have refused before it touched the runtime. Do not remove someone else's probe.
    if ((Sha $dll) -ne $want) { Refuse 'this run does not own this runtime change' }
    State; exit 0
}
if ($ResolveOnly) {
    if ($Mode -eq 'remove' -and -not $EvidenceRun -and -not $saved -and
        (Test-Path -LiteralPath $dll -PathType Leaf) -and (Sha $dll) -eq $want) { State; exit 0 }
    "PROBE TARGET $ver"; exit 0
}
$rb = @(Get-Process -Name rekordbox -ErrorAction SilentlyContinue)
if ($rb.Count) { Refuse "rekordbox is running (session $(($rb.SessionId | Sort-Object -Unique) -join ','))" }
$user = ([Security.Principal.SecurityIdentifier]$Sid).Translate([Security.Principal.NTAccount]).Value
if ($Mode -eq 'install') {
    Trace-KitPhase 'probe: checking installation'
    if ($ProbeSha -notmatch '^[0-9a-f]{64}$') { Refuse 'no probe SHA-256' }
    $s = Sha $dll
    if ($s -eq $ProbeSha) {
        if ($EvidenceRun) { Refuse 'an existing probe must be removed before a scoped run' }
        if (-not $saved) { Refuse 'probe already installed without a saved Pioneer DLL' }
        if ($NoDumps -and (Test-Path -LiteralPath $dump)) { Refuse 'probe already collecting dumps; fetch and remove it first' }
        if (-not $NoDumps) { [void][IO.Directory]::CreateDirectory($dump) }
        State; exit 0
    }
    if ($saved) { Refuse 'a previous probe backup still exists; remove it first' }
    if ($s -ne $want) { Refuse "rekordbox's onnxruntime.dll isn't Pioneer's ($($s.Substring(0, 8))): Stems Cache installed?" }
    $prof = (Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $Sid }).LocalPath
    $src = Join-Path $prof 'AppData\Local\Temp\rbsp-kit\probe\onnxruntime.dll'
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { Refuse 'no staged probe (stage --probe)' }
    if ($NoDumps -and (Test-Path -LiteralPath $base)) { Refuse 'probe evidence directory already exists; preserve the previous evidence first' }
    [void](New-Item -ItemType Directory -Force (Join-Path $base 'real'), (Join-Path $base 'stage'))
    if (-not $NoDumps) { [void][IO.Directory]::CreateDirectory($dump) }
    # C:\rbtest: the account may write (the probe's log and dumps); real\ and stage\ administrators only
    & icacls.exe $base /grant "${user}:(OI)(CI)M" | Out-Null
    if ($LASTEXITCODE) { Refuse 'could not grant access to probe evidence' }
    foreach ($sub in 'real', 'stage') {
        & icacls.exe (Join-Path $base $sub) /inheritance:r /grant:r '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
        if ($LASTEXITCODE) { Refuse 'could not protect the probe backup or staging directory' }
    }
    function Reserve([string]$path, [string]$value) {
        $reservation = [IO.File]::Open($path, 'CreateNew', 'Write', 'None')
        try {
            $bytes = [Text.Encoding]::ASCII.GetBytes($value)
            $reservation.Write($bytes, 0, $bytes.Length)
        } finally { $reservation.Dispose() }
    }
    if ($EvidenceRun) { Reserve $runMarker $EvidenceRun }
    Reserve $target $ver
    # the copy checked where the account can't change it, then placed
    Trace-KitPhase 'probe: installing DLL'
    Copy-Item -LiteralPath $src -Destination $st -Force
    if ((Sha $st) -ne $ProbeSha) { Remove-Item -LiteralPath $st; Refuse "the staged probe isn't the one the Mac built ($ProbeSha)" }
    Copy-Item -LiteralPath $dll -Destination $real -Force
    if ((Sha $real) -ne $want) { Refuse "the copy of Pioneer's DLL in $real doesn't check out" }
    try {
        Copy-Item -LiteralPath $st -Destination $dll -Force
        Remove-Item -LiteralPath $st
        Trace-KitPhase 'probe: checking installed DLL'
        if ((Sha $dll) -ne $ProbeSha) { throw 'probe not in place' }
        State
    } catch {
        try { Restore-Pioneer }
        catch { [Console]::Error.WriteLine('probe: immediate restore failed; use probe remove') }
        throw
    }
    exit 0
}
# remove
try { Restore-Pioneer } catch { Refuse $_.Exception.Message }
if (Test-Path -LiteralPath $base) {
    function Assert-ProbeTree([string]$dir) {
        foreach ($f in [IO.DirectoryInfo]::new($dir).EnumerateFileSystemInfos()) {
            if ($f.Attributes -band [IO.FileAttributes]::ReparsePoint) { Refuse 'reparse point in probe evidence' }
            if ($f -is [IO.DirectoryInfo]) { Assert-ProbeTree $f.FullName }
        }
    }
    Assert-ProbeTree $base
    Remove-Item -LiteralPath $base -Recurse -Force
}
State
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
