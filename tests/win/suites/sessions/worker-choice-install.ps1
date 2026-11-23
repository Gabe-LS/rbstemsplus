# SYSTEM stages B's verified installed-release inputs through the shipped workers. B's app and
# watcher are already running in its own logon session. Its noninteractive desktop needs no clicks.
# The production worker still authenticates its parent, checks the payload and installs to bin.
function New-FixtureWorkerStartInfo([string]$exe, [string]$verb, [string]$source, [string]$manifest, [string]$name) {
    $targets = '*'
    $force = '0'
    if ($verb -eq 'install') {
        $sha = Get-Sha $RbExe
        if ($sha -cnotmatch '^[0-9a-f]{64}$') { throw 'The rekordbox fixture EXE could not be hashed' }
        $targets = "7.2.19:$($sha.Substring(0, 16))"
        $force = '1' # A already installed this same bridge and its receipt. Reinstall it with B's worker.
    }
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WorkingDirectory = [Environment]::SystemDirectory
    $arguments = @($verb, '--proto', '2', '--targets', $targets, '--manifest', $manifest, '--source', $source, '--force', $force, '--pipe', "\\.\pipe\$name")
    $info.Arguments = ($arguments | ForEach-Object { '"' + ($_ -replace '(\\+)$', '$1$1') + '"' }) -join ' '
    return $info
}

function Confirm-FixtureWorkerResult([string]$verb, [int]$want, [int]$code, [string]$log) {
    "B's $verb fixture (exit ${code}):`n$log"
    $targets = @(foreach ($line in ($log -split '\r?\n')) {
        if ($line -cmatch '^@target ([0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}) (-|0|[1-9][0-9]{0,3}) (installed|already|put-back|kept|not-started|refused)$') {
            [pscustomobject]@{ Version = $Matches[1]; Code = $Matches[2]; Result = $Matches[3] }
        }
    })
    switch ($code) {
        15 { throw "$verb scope or staged payload changed (exit 15)" }
        16 {
            $details = ($targets | ForEach-Object { "$($_.Version) $($_.Code) $($_.Result)" }) -join ', '
            throw "$verb only partially completed (exit 16): $details"
        }
    }
    if ($code -ne $want) { throw "$verb needed exit $want" }
    if ($verb -eq 'install' -and ($targets.Count -ne 1 -or $targets[0].Version -ne '7.2.19' -or
        $targets[0].Code -ne '0' -or $targets[0].Result -ne 'installed')) {
        throw 'Expected an installed result for rekordbox 7.2.19'
    }
}

function Invoke-FixtureWorker([string]$exe, [string]$verb, [int]$want, [string]$source, [string]$manifest) {
    $name = 'rbstemsplus-admin-' + [guid]::NewGuid().ToString('N')
    $pipe = [IO.Pipes.NamedPipeServerStream]::new($name, 'In', 1, 'Byte', 'Asynchronous')
    $proc = $reader = $null
    try {
        $connection = $pipe.WaitForConnectionAsync()
        $info = New-FixtureWorkerStartInfo $exe $verb $source $manifest $name
        $proc = [Diagnostics.Process]::Start($info)
        if (-not $connection.Wait(10000)) { throw "$verb did not open its parent pipe" }
        $reader = [IO.StreamReader]::new($pipe, [Text.Encoding]::UTF8)
        $log = $reader.ReadToEndAsync()
        if (-not $proc.WaitForExit(120000) -or -not $log.Wait(5000)) { throw "$verb did not finish" }
        Confirm-FixtureWorkerResult $verb $want $proc.ExitCode $log.Result
    } finally {
        if ($proc) {
            if (-not $proc.HasExited) { $proc.Kill(); [void]$proc.WaitForExit(5000) }
            $proc.Dispose()
        }
        if ($reader) { $reader.Dispose() }
        $pipe.Dispose()
    }
}
Assert-WorkerPromptless
$pair = Get-WorkerReleasePair
$old = $pair.Old.windows.app
$new = $pair.New.windows.app
$binOld = Get-BinWorker $old.admin_sha256
$binNew = Get-BinWorker $new.admin_sha256
if ((Get-Sha $binOld) -ne $old.admin_sha256 -or (Test-Path -LiteralPath $binNew)) { throw 'Only A''s original worker must be in bin' }
$profile = Get-BProfile
if (-not $profile -or @(Get-BProcesses).Count -lt 2) { Skip 'needs B''s independent app and watcher from account-b-worker-choice' }
$local = Join-Path $profile 'AppData\Local'
$apps = Join-Path $local 'Programs\RB Stems Plus'
$release = Join-Path $local 'rbstemsplus\installed-release'
if ((Get-Content -LiteralPath (Join-Path $apps 'current') -Raw).Trim() -ne $new.version) { throw 'B must have the newer app' }
$source = Join-Path $WorkerTraceDir 'b-source'
$sourceAdmin = Join-Path $source 'admin'
New-Item -ItemType Directory -Path $sourceAdmin -Force | Out-Null
try {
    $manifest = Get-Sha (Share 'dist-test/new/payload.json')
    foreach ($name in 'payload.json', 'payload.json.sig') {
        $from = Join-Path $release $name
        if ((Get-Sha $from) -ne (Get-Sha (Share "dist-test/new/$name"))) { throw "B's $name differs from the new fixture" }
        Copy-Item -LiteralPath $from -Destination $source
    }
    if ((Get-Sha (Join-Path $source 'payload.json')) -ne $manifest) { throw 'The staged payload.json differs from the new fixture' }
    $appWorker = Join-Path $apps "$($new.version)\admin\RB Stems Plus.exe"
    if ((Get-Sha $appWorker) -ne $new.admin_sha256) { throw 'B''s app-folder worker differs from the new fixture' }
    Copy-Item -LiteralPath $appWorker -Destination $sourceAdmin
    $file = $pair.New.windows.bridge.file
    if ($file -notmatch '^[A-Za-z0-9_-][A-Za-z0-9._-]*\.dll$') { throw 'Invalid B bridge fixture name' }
    $bridge = Need "dist-test/new/$file" 'the new release bridge fixture'
    if ((Get-Sha $bridge) -ne $pair.New.windows.bridge.sha256) { throw 'B''s bridge differs from the new fixture' }
    Copy-Item -LiteralPath $bridge -Destination $source

    Invoke-FixtureWorker $binOld 'install-worker' 13 $source $manifest
    if ((Get-Sha $binNew) -ne $new.admin_sha256) { throw 'B''s worker did not reach bin' }
    Invoke-FixtureWorker $binNew 'install' 0 $source $manifest
    Check (-not (Test-Path -LiteralPath $binOld)) 'B''s new worker pruned A''s old bin worker'
    Check ((Get-Sha $binNew) -eq $new.admin_sha256) 'only B''s released worker remains for A''s action'
    Check ((Get-Sha $RbOrt) -eq $pair.New.windows.bridge.sha256) 'B''s released bridge is installed'
} finally {
    Remove-Item -LiteralPath $source -Recurse -Force
}
Done
