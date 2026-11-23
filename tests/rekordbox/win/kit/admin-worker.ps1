# admin-worker.ps1: Stems Cache's elevated half, run through the owner's admin SSH login instead of
# the app's UAC prompt. The owner's runner checks this script's hash and sets $Verb ('install' or
# 'uninstall') and $Sid (the test account's SID). It runs the test account's own app's worker with
# the app's protocol (win/c/admin_proto.c and core_native_admin.c):
# `VERB --proto 2 --targets T --manifest M --source DIR --force 0 --pipe NAME`, every
# value quoted, from System32, with this process as the worker's parent and the pipe's server (the
# worker refuses any other: worker_win.c). The worker:
#   - the bin\ worker whose SHA-256 is the installed release's windows.app.admin_sha256;
#   - for install only, when bin\ has none: the app folder's admin\RB Stems Plus.exe, hash-checked.
# The source (install): payload.json and .sig from the account's support\installed-release, the
# bridge (the payload's windows.bridge.file, found by its SHA-256 in the account's support and app
# folders) and admin\RB Stems Plus.exe, in the account's %TEMP%\rbsp-admin-source, removed after.
# Output: "worker: ..." (the pipe, with target results decoded), then "WORKER exit N", or
# "REFUSED: why" with exit 2 (nothing run). The UAC hand-off itself is covered by the VM suite
# tests/win/suites/stems-cache-uac.suite, never here.
$ErrorActionPreference = 'Stop'
Trace-KitPhase 'admin-worker: checking worker prerequisites'
function Refuse([string]$why) { "REFUSED: $why"; exit 2 }
function Sha([string]$f) { (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash.ToLowerInvariant() }

function Get-InstallTargets($folders, $compat, [string]$bridgeSha, [string]$root) {
    # Propose scope from the installed release's ORT bindings and the files on disk. The worker
    # independently checks the registered inventory, trust, saved sets and receipt identities.
    $items = @()
    $previous = $null
    foreach ($dir in ($folders | Sort-Object { [version]($_.Name.Substring('rekordbox '.Length)) })) {
        $version = $dir.Name.Substring('rekordbox '.Length)
        $rbExe = Join-Path $dir.FullName 'rekordbox.exe'
        if (-not (Test-Path -LiteralPath $rbExe -PathType Leaf)) { continue }
        $product = (Get-Item -LiteralPath $rbExe).VersionInfo.ProductVersion
        $binding = $compat.ort | Where-Object {
            $v = [string]$_.rekordbox
            $v -and ($product -ceq $v -or ($product -and $product.StartsWith("$v.", [StringComparison]::Ordinal)))
        } | Sort-Object { $_.rekordbox.Length } -Descending | Select-Object -First 1
        $boundSha = $binding.sha256
        if ($boundSha -cnotmatch '^[0-9a-f]{64}$') { continue }
        $live = Join-Path $dir.FullName 'onnxruntime.dll'
        $liveSha = if (Test-Path -LiteralPath $live -PathType Leaf) { Sha $live }
        if ($liveSha -eq $bridgeSha) { continue }
        $exeSha = Sha $rbExe
        if ($liveSha -ne $boundSha) {
            $saved = Join-Path $root "pioneer\$version\onnxruntime.dll"
            if (-not (Test-Path -LiteralPath $saved -PathType Leaf) -or (Sha $saved) -ne $boundSha) { continue }
            if ($liveSha) {
                $receipt = Join-Path $root "receipts\$version"
                if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { continue }
                $fields = Get-Content -LiteralPath $receipt
                if ($fields -cnotcontains "exe_sha256 $exeSha" -or $fields -cnotcontains "original_sha256 $boundSha" -or
                    $fields -cnotcontains "bridge_sha256 $liveSha") { continue }
            }
        }
        if ($previous -eq [version]$version) { Refuse "duplicate rekordbox version $version" }
        $previous = [version]$version
        $items += "${version}:$($exeSha.Substring(0, 16))"
    }
    $token = $items -join ','
    if (-not $items.Count) { Refuse 'no rekordbox targets for this release' }
    if ($items.Count -gt 16 -or $token.Length -ge 512) { Refuse 'the rekordbox target list is too large' }
    return $token
}

function Get-WorkerArgs([string]$verb, [string]$targets, [string]$manifest, [string]$source, [string]$pipe) {
    if ($verb -eq 'uninstall') { $targets = '*'; $manifest = '-' }
    $argsForWorker = @($verb, '--proto', '2', '--targets', $targets, '--manifest', $manifest, '--source', $source, '--force', '0', '--pipe', "\\.\pipe\$pipe")
    return (($argsForWorker | ForEach-Object { '"' + ($_ -replace '(\\+)$', '$1$1') + '"' }) -join ' ')
}

function Format-WorkerLine([string]$line) {
    if ($line -cmatch '^@target ([0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}) (-|0|[1-9][0-9]{0,3}) (installed|already|put-back|kept|not-started|refused)$') {
        $exitText = if ($Matches[2] -ne '-') { " (exit $($Matches[2]))" }
        return "worker: rekordbox $($Matches[1]): $($Matches[3])$exitText"
    }
    return "worker: $line"
}

function Format-WorkerExit([int]$code) {
    switch ($code) {
        15 { 'worker: scope or staged payload changed; rerun preflight before another attempt' }
        16 { 'worker: partial result; see the individual rekordbox results above' }
    }
    "WORKER exit $code"
}

if ($Verb -notin 'install', 'uninstall') { Refuse "verb '$Verb'" }
if ($Sid -notmatch '^S-1-5-21-[0-9-]+$') { Refuse 'no test account SID' }
if ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -eq $Sid) { Refuse 'this is the test account, not the admin login' }
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Refuse 'the admin login has no elevated token' }
# rekordbox in any session counts as open for the worker (exit 5): said here first
$rb = @(Get-Process -Name rekordbox -ErrorAction SilentlyContinue)
if ($rb.Count) { Refuse "rekordbox is running (session $(($rb.SessionId | Sort-Object -Unique) -join ','))" }
$prof = (Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $Sid }).LocalPath
if (-not $prof) { Refuse 'no profile for the test account' }
$support = Join-Path $prof 'AppData\Local\rbstemsplus'
$apps = Join-Path $prof 'AppData\Local\Programs\RB Stems Plus'
$pdir = Join-Path $support 'installed-release'
$pj = Join-Path $pdir 'payload.json'
if (-not (Test-Path -LiteralPath $pj -PathType Leaf) -or -not (Test-Path -LiteralPath "$pj.sig" -PathType Leaf)) { Refuse "no installed release record in $pdir" }
$manifest = if ($Verb -eq 'install') { Sha $pj }
$p = Get-Content -LiteralPath $pj -Raw | ConvertFrom-Json
$own = "$($p.windows.app.admin_sha256)".ToLowerInvariant()
if ($own -notmatch '^[0-9a-f]{64}$') { Refuse 'the installed release names no worker' }
# The protocol names the version folder's X.Y.Z, in numeric version order.
$rbd = @(Get-ChildItem -LiteralPath (Join-Path $env:ProgramFiles 'rekordbox') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^rekordbox [0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}$' })
if (-not $rbd.Count) { Refuse 'rekordbox is not installed' }
$root = Join-Path $env:ProgramFiles 'RB Stems Plus'
$exe = $null
foreach ($w in @(Get-ChildItem -LiteralPath (Join-Path $root 'bin') -Directory -ErrorAction SilentlyContinue | Where-Object Name -match '^[0-9a-f]{12}$')) {
    $f = Join-Path $w.FullName 'RB Stems Plus.exe'
    if ((Test-Path -LiteralPath $f -PathType Leaf) -and (Sha $f) -eq $own) { $exe = $f; break }
}
$copy = @(Get-ChildItem -LiteralPath $apps -Recurse -Filter 'RB Stems Plus.exe' -ErrorAction SilentlyContinue | Where-Object { $_.Directory.Name -eq 'admin' -and (Sha $_.FullName) -eq $own } | Select-Object -First 1)
if (-not $exe) {
    if ($Verb -ne 'install' -or -not $copy.Count) { Refuse "no worker with the installed release's SHA-256 $($own.Substring(0, 12)) in bin\$(if ($Verb -eq 'install') { ' or the app folder' })" }
    $exe = $copy[0].FullName
}
$src = ''
$targets = ''
$tmp = Join-Path $prof 'AppData\Local\Temp\rbsp-admin-source'
$server = $null
try {
    if ($Verb -eq 'install') {
        $bf = "$($p.windows.bridge.file)"; $bs = "$($p.windows.bridge.sha256)".ToLowerInvariant()
        if ($bf -notmatch '^[A-Za-z0-9._-]+$' -or $bs -notmatch '^[0-9a-f]{64}$') { Refuse 'the installed release names no bridge' }
        $targets = Get-InstallTargets $rbd $p.windows.compatible $bs $root
        $bridge = @(Get-ChildItem -LiteralPath $support, $apps -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Length -gt 0 -and $_.Extension -eq '.dll' -and (Sha $_.FullName) -eq $bs } | Select-Object -First 1)
        if (-not $bridge.Count) { Refuse "no file with the bridge's SHA-256 $($bs.Substring(0, 12)) in the account's support or app folder" }
        if (-not $copy.Count) { Refuse 'no app-folder worker copy for the source' }
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
        $src = $tmp
        [void](New-Item -ItemType Directory -Path (Join-Path $tmp 'admin'))
        Copy-Item -LiteralPath $pj, "$pj.sig" -Destination $tmp
        Copy-Item -LiteralPath $bridge[0].FullName -Destination (Join-Path $tmp $bf)
        Copy-Item -LiteralPath $copy[0].FullName -Destination (Join-Path $tmp 'admin\RB Stems Plus.exe')
        if ((Sha (Join-Path $tmp $bf)) -ne $bs) { Refuse 'the staged bridge changed' }
        if ((Sha (Join-Path $tmp 'payload.json')) -ne $manifest) { Refuse 'the staged payload.json changed' }
    }
    $rnd = New-Object byte[] 16; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($rnd)
    $pipe = 'rbstemsplus-admin-' + (($rnd | ForEach-Object { $_.ToString('x2') }) -join '')
    $argline = Get-WorkerArgs $Verb $targets $manifest $src $pipe
    "worker: $exe"
    "args: $argline"
    # $Check: everything found and staged, nothing started (preflight)
    if ($Check) {
        if ($src) { Remove-Item -LiteralPath $tmp -Recurse -Force }
        $src = ''
        'WORKER ready (not started)'
        exit 0
    }
    Trace-KitPhase 'admin-worker: starting worker'
    $server = New-Object IO.Pipes.NamedPipeServerStream($pipe, [IO.Pipes.PipeDirection]::In, 1, [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)
    $si = New-Object Diagnostics.ProcessStartInfo $exe, $argline
    $si.UseShellExecute = $false; $si.WorkingDirectory = [Environment]::SystemDirectory; $si.CreateNoWindow = $true
    $proc = [Diagnostics.Process]::Start($si)
    Trace-KitPhase 'admin-worker: waiting for worker pipe'
    $wait = $server.BeginWaitForConnection($null, $null)
    if ($wait.AsyncWaitHandle.WaitOne(30000)) {
        $server.EndWaitForConnection($wait)
        $r = New-Object IO.StreamReader($server)
        while ($null -ne ($line = $r.ReadLine())) { Format-WorkerLine $line }
    } else { 'worker: (no pipe client in 30 s)' }
    Trace-KitPhase 'admin-worker: waiting for worker exit'
    if (-not $proc.WaitForExit(600000)) { 'worker: still running after 10 min: left as it is'; exit 1 }
    Trace-KitPhase 'admin-worker: worker exited'
    Format-WorkerExit $proc.ExitCode
} finally {
    if ($server) { $server.Dispose() }
    if ($src -and (Test-Path -LiteralPath $tmp)) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}
