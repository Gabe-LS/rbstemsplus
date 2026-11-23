# Exercise the update steps on the host with fixture files and mocked Windows operations.
param([string]$Work, [ValidateSet('sticky-stub', 'rollback')][string]$Group = 'sticky-stub')
$ErrorActionPreference = 'Stop'
$steps = Join-Path $PSScriptRoot 'suites/update'
. (Join-Path $steps 'updatelib.ps1')

$testState = @{}
function Check([bool]$ok, [string]$what) { if (-not $ok) { $testState.Failures += $what } }
function Done { throw 'step finished' }
function Skip([string]$why) { $testState.Skipped = $why; Done }
function Assert($ok, $what) { if (-not $ok) { throw $what }; "PASS $what" }
function Run-Step($name) {
    $testState.Failures = @()
    $testState.Skipped = $null
    try { & (Join-Path $steps "$name.ps1"); throw "$name did not call Done" }
    catch { if ($_.Exception.Message -ne 'step finished') { throw } }
}
function Get-Layout { $layout }
function Get-UpdateRelease($name) { $releases[$name] | ConvertFrom-Json }
function Get-NativeUpdateArch { 'arm64' }
function Assert-UpdateDesktop {}
$UpdateApp = [pscustomobject]@{ Id = 42 }
$UpdateApp | Add-Member ScriptMethod WaitForExit { param($ms) $true }
function Wait-UpdateApp {}
function Get-WaitSecs($secs) { $secs }
function Wait-Until($cond, $secs) { & $cond }
function Stop-App {}
function Get-CoreText($id) {
    if ($id -eq 'T_UPDATE_NOTICE') { return 'Available {version}' }
    return $id
}
function Get-MenuText($id) { $id }
function Find-UiText($procId, $pattern, $secs) {
    ('Available 1.5.0' -match $pattern) -or ($about -match $pattern)
}
function Get-ItemProperty($LiteralPath) { @{ DisplayVersion = $displayVersion } }
function Use-UpdateUi {
    if ($args -contains 'T_DONE') {
        Remove-Item -Recurse -LiteralPath $layout.Apps, $layout.Support, $UpdateEntry
    }
}
function Start-Process($FilePath) {
    if ($testState.Lap -eq 'sticky' -and $fetch) { Set-Content -LiteralPath $saved $releases.new }
    if ($testState.Lap -eq 'stub-change' -and $cleanBackup) { Remove-Item -LiteralPath ($layout.Stub + '.old') }
}
function Set-StickyFixture {
    New-Item -ItemType Directory -Force $layout.Apps, (Split-Path $saved) | Out-Null
    Set-Content -LiteralPath $UpdateEntry 'entry'
    Set-Content -LiteralPath $layout.Current '1.4.0'
    Set-Content -LiteralPath $saved $releases.carried
    Set-Content -LiteralPath (Join-Path $layout.Support 'shown-version') "1.4.1`n1.4.0"
}
function Set-StubFixture([string]$backup) {
    New-Item -ItemType Directory -Force $layout.Apps | Out-Null
    Set-Content -NoNewline -LiteralPath $layout.Stub 'new stub'
    Set-Content -NoNewline -LiteralPath ($layout.Stub + '.old') $backup
}

$layout = [pscustomobject]@{
    Apps = Join-Path $Work 'apps'
    Support = Join-Path $Work 'support'
    Stub = Join-Path $Work 'apps/RB Stems Plus.exe'
    Current = Join-Path $Work 'apps/current'
}
$UpdateEntry = Join-Path $Work 'entry'

if ($Group -eq 'rollback') {
    . (Join-Path $steps 'rollbacklib.ps1')
    $source = Join-Path $Work 'release'
    $records = Join-Path $layout.Support 'update'
    $pending = Join-Path $records 'pending'
    $result = Join-Path $records 'result'
    $staged = Join-Path $layout.Apps '1.5.0'
    New-Item -ItemType Directory -Force $source, $layout.Apps | Out-Null
    $exe = Join-Path $source 'RB Stems Plus.exe'
    Set-Content -LiteralPath $exe 'stand-in for the candidate shell'
    $zip = Join-Path $source 'update.zip'
    Compress-Archive -LiteralPath $exe -DestinationPath $zip
    Set-Content -LiteralPath (Join-Path $source 'payload.json') '{}'
    Set-Content -LiteralPath (Join-Path $source 'payload.json.sig') 'fixture'
    $hash = (Get-FileHash -LiteralPath $zip).Hash
    $release = @{ windows = @{ app = @{ version = '1.5.0'; zip = 'update.zip'; sha256 = $hash } } }

    function Run-Fixture {
        $testState.Failures = @()
        try { New-RollbackFixture $layout '1.4.0' $release $source 1234 1770000000 }
        catch { if ($_.Exception.Message -ne 'step finished') { throw } }
    }
    Run-Fixture
    $actual = [Convert]::ToBase64String([IO.File]::ReadAllBytes($pending))
    $exact = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("1.4.0`n1.5.0`n1234`n1770000000`n"))
    Assert ($actual -ceq $exact) 'UP10 pending has the exact four LF-terminated fields without a BOM'
    Assert ((Test-Path -LiteralPath (Join-Path $staged 'RB Stems Plus.exe')) -and
        (Test-Path -LiteralPath (Join-Path $records 'payload.json'))) 'UP10 stages the real zip layout and its payload alongside pending'
    Remove-Item -Recurse -LiteralPath $staged
    Run-Fixture
    Assert ($testState.Failures -contains 'no stale pending record') 'UP10 refuses to overwrite pending from an earlier update'
    Remove-Item -LiteralPath $pending
    $release.windows.app.sha256 = '0' * 64
    Run-Fixture
    Assert ($testState.Failures -contains 'the candidate zip matches its payload') 'UP10 refuses a zip whose hash differs from its payload'
    $release.windows.app.sha256 = $hash
    $release.windows.app.zip = '../release/update.zip'
    Run-Fixture
    Assert ($testState.Failures -contains 'the candidate payload names a plain zip and SHA-256') 'UP10 refuses a zip path outside the fixture name'

    function Start-Process($FilePath, $WorkingDirectory, $ArgumentList, [switch]$PassThru) {
        if ($testState.Lap -eq 'broken-current') {
            $value = if (Test-Path -LiteralPath $layout.Current) { (Get-Content -Raw -LiteralPath $layout.Current).Trim() } else { 'missing' }
            $testState.Pointers += $value
            return $brokenStub
        }
        if ($testState.Lap -eq 'rollback') {
            if ($ArgumentList -ne '--swap') { return }
            $testState.Trigger = $env:RBSP_SHELL_TEST
            Add-Content -LiteralPath $log "--swap: the start didn't finish in time: the update stops here"
            Remove-Item -LiteralPath $pending
            Remove-Item -Recurse -LiteralPath $WorkingDirectory
            return $mockCandidate
        }
        $testState.Starts++
        if (Test-Path -LiteralPath $result) {
            $testState.Message = if (-not $testState.SuppressFirst -and (Get-Content -Raw -LiteralPath $result) -ceq "failed swap`n") { 'Update failure. Swap failed.' }
            if (-not $testState.KeepResult) { Remove-Item -LiteralPath $result }
        } else { $testState.Message = if ($testState.Repeat) { 'Update failed' } }
    }
    function Get-AppProcesses {}
    function Get-CoreText($id) {
        switch ($id) {
            'T_UPDATE_FAILED' { 'Update failed' }
            'T_UPDATE_FAILED_MSG' { 'Update failure.{why}' }
            'T_UPDATE_R_SWAP' { ' Swap failed.' }
            default { $id }
        }
    }
    function Find-UiText($procId, $pattern, $secs) {
        if ($testState.Lap -eq 'broken-current') { return -not $testState.BrokenMessageMissing }
        return $testState.Message -match $pattern -or ('1.4.0' -match $pattern)
    }
    $brokenStub = [pscustomobject]@{ Id = 51; HasExited = $true; ExitCode = 1 }
    $brokenStub | Add-Member ScriptMethod WaitForExit { param($ms) $true }
    $testState.Lap = 'broken-current'
    # Use different line endings from Set-Content to ensure the exact original survives.
    $original = "1.4.0`r`n"
    [IO.File]::WriteAllText($layout.Current, $original)
    $testState.Pointers = @()
    Run-Step broken-current
    Assert ($testState.Failures.Count -eq 0 -and ($testState.Pointers -join ',') -ceq 'missing,bad/version,999999999.0.0,1.4.0') 'UP9 drives three corrupt pointers then launches with the original'
    $testState.BrokenMessageMissing = $true
    Run-Step broken-current
    Assert ($testState.Failures.Count -eq 1 -and (Test-Path -LiteralPath $layout.Current) -and
        [IO.File]::ReadAllText($layout.Current) -ceq $original) 'UP9 restores the exact current record on early failure'

    $testState.Lap = 'once-only'
    $testState.Starts = 0
    $releases = @{ a = '{"payload_version":"1.4.0"}' }
    Run-Step once-only
    Assert ($testState.Failures.Count -eq 0 -and $testState.Starts -eq 2) 'UP12 requires a visible first result and a second launch'
    $testState.SuppressFirst = $true
    Run-Step once-only
    Assert ($testState.Failures -contains 'the recovered app said: Update failure. Swap failed.') 'UP12 rejects a result silently discarded at first start'
    $testState.SuppressFirst = $false
    $testState.KeepResult = $true
    Run-Step once-only
    Assert ($testState.Failures -contains 'the update result was consumed') 'UP12 rejects an unconsumed result'
    Remove-Item -LiteralPath $result
    $testState.KeepResult = $false
    $testState.Repeat = $true
    Run-Step once-only
    Assert ($testState.Failures -contains 'the next launch did not repeat the update failure dialog') 'UP12 rejects a failure dialog repeated at the next start'

    function Need($path, $why) { Join-Path $source 'payload.json' }
    $testState.Lap = 'rollback'
    $VmRun = @{ Step = 'rollback-offscreen-hang' }
    $release.windows.app.zip = 'update.zip'
    $releases.new = $release | ConvertTo-Json -Depth 5
    $releases.a = '{"payload_version":"1.4.0","windows":{"app":{"version":"1.4.0"}}}'
    $displayVersion = '1.4.0'
    $log = Join-Path $layout.Support 'Logs/app.log'
    $installed = Join-Path $layout.Support 'installed-release/payload.json'
    New-Item -ItemType Directory -Force (Split-Path $log), (Split-Path $installed) | Out-Null
    Set-Content -LiteralPath $log 'before the swap'
    Set-Content -LiteralPath $installed $releases.a
    Set-Content -LiteralPath $layout.Stub 'stand-in for the stable stub'
    $UpdateApp | Add-Member NoteProperty StartTime ([DateTime]'2026-01-01')
    $UpdateApp | Add-Member ScriptMethod CloseMainWindow { $true }
    $mockCandidate = [pscustomobject]@{ StartTime = $UpdateApp.StartTime; ExitCode = 1; ExitTime = $UpdateApp.StartTime.AddSeconds(35) }
    $mockCandidate | Add-Member ScriptMethod WaitForExit { param($ms) $true }
    $testState.Message = 'Update failure. Swap failed.'
    $originalTrigger = $env:RBSP_SHELL_TEST
    try {
        $env:RBSP_SHELL_TEST = 'original trigger'
        Run-Step rollback
        Assert ($testState.Failures.Count -eq 0 -and $testState.Trigger -ceq 'hang-offscreen' -and
            $env:RBSP_SHELL_TEST -ceq 'original trigger') 'UP11 sets the off-screen hang trigger for the swap and restores the environment'
        $mockCandidate.ExitTime = $mockCandidate.StartTime.AddSeconds(1)
        Run-Step rollback
        Assert (($testState.Failures -join '`n') -match 'the watchdog ended the hung off-screen start') 'UP11 rejects a candidate that exits too soon to exercise the watchdog'
    } finally { $env:RBSP_SHELL_TEST = $originalTrigger }
    exit 0
}

$saved = Join-Path $layout.Support 'payload/payload.json'
$releases = @{
    carried = '{"payload_version":"1.4.1","windows":{"app":{"version":"1.4.0"}}}'
    new = '{"payload_version":"1.5.0","windows":{"app":{"version":"1.5.0"}}}'
}
$testState.Lap = 'sticky'
$about = '1.4.1'; $displayVersion = '1.4.1'; $fetch = $true
Set-StickyFixture
Run-Step sticky
Assert ($testState.Failures.Count -eq 0 -and -not $testState.Skipped) 'UP1 accepts the carried version after a real change to the saved payload'
$about = '1.4.0'; $displayVersion = '1.5.0'
Set-StickyFixture
Run-Step sticky
Assert ($testState.Failures -contains 'About shows 1.4.1') 'UP1 rejects About falling back to the build version'
Assert ($testState.Failures -contains 'Installed apps still shows 1.4.1 at the uninstall confirmation') 'UP1 rejects Installed apps promoting an uninstalled update'
$about = '1.4.1'; $displayVersion = '1.4.1'; $fetch = $false
Set-StickyFixture
Run-Step sticky
Assert ($testState.Failures -contains 'the saved payload is now 1.5.0') 'UP1 requires the newer payload on disk even if the notice is visible'

$testState.Lap = 'stub-change'
$cleanBackup = $true
Set-StubFixture 'old stub'
$oldHash = (Get-FileHash -LiteralPath ($layout.Stub + '.old')).Hash
$newHash = (Get-FileHash -LiteralPath $layout.Stub).Hash
$releases.a = "{`"windows`":{`"app`":{`"stub_sha256`":{`"arm64`":`"$oldHash`"}}}}"
$releases.new = $releases.a.Replace($oldHash, $newHash)
Run-Step stub-change
Assert ($testState.Failures.Count -eq 0 -and -not $testState.Skipped) 'UP3 accepts a changed stub and removal of its verified backup'
Set-StubFixture 'new stub'
Run-Step stub-change
Assert ($testState.Failures -contains 'the .old backup is the previous release stub') 'UP3 rejects the new bytes in the old stub backup'
$cleanBackup = $false
Set-StubFixture 'old stub'
Run-Step stub-change
Assert ($testState.Failures -contains 'the next successful start removed the .old stub') 'UP3 rejects a backup left after restart'
$releases.new = $releases.a
Run-Step stub-change
Assert ($testState.Failures.Count -eq 0 -and $testState.Skipped -match 'same arm64 stub hash') 'UP3 skips equal fixture hashes'
