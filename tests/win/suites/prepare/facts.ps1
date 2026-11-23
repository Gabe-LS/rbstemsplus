# N1.2, .3, .7 and .8. The installed app uses its normal Windows facts and payload.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Current)) { Skip 'the Prepare TEST app is missing' }
$ort = Join-Path $CacheRoot 'ort\onnxruntime.dll'
if (-not (Test-Path -LiteralPath $ort -PathType Leaf)) { Skip 'this case needs the root Stems Cache ORT copy' }
$case = $VmRun.Step
$hidden = $helper = $source = $null
try {
    if ($case -eq 'facts-missing') {
        $version = (Get-Content -LiteralPath $l.Current -Raw).Trim()
        if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'invalid current version' }
        $helper = Join-Path $l.Apps "$version\rbsp-prepare.exe"
        if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) { Skip 'the fixture needs a helper to take away' }
        $hidden = "$helper.rbtest-$($VmRun.Run)"
        Expect (-not (Test-Path -LiteralPath $hidden)) 'the helper backup path is unused'
        Expect (-not (Get-AppWindow)) 'the fixture app is closed before moving its helper'
        Move-Item -LiteralPath $helper -Destination $hidden
        $source = 'RBSP-prepare-fact-start'
        $session = (Get-Process -Id $PID).SessionId
        Register-WmiEvent -Query "SELECT * FROM Win32_ProcessStartTrace WHERE SessionID = $session AND (ProcessName = 'rbsp-prepare.exe' OR ProcessName = 'PING.EXE')" -SourceIdentifier $source | Out-Null
    }

    Start-PrepareApp
    $appPid = $script:ShellApp.Id
    $want = switch ($case) {
        'facts-missing' { 'T_PREP_NO_HELPER' }
        'facts-speed' { 'T_PREP_SPEED' }
        'facts-unlisted' { 'T_PREP_RB_UNLISTED' }
        'facts-entry' { 'T_PREP_REINSTALL_CACHE' }
        default { throw "unrecognised Prepare facts fixture $case" }
    }
    $expected = [regex]::Escape((Get-CoreText $want)).Replace('\{rekordbox}', '[^\r\n"]+?')
    $seen = Wait-Until {
        $rc = Invoke-Rbui read --pid $appPid --timeout 2
        $rc -eq 0 -and [regex]::Matches($script:ExeOut, $expected).Count -eq 1
    } 25
    Check $seen "exactly one Prepare problem matching $want"
    if ($case -eq 'facts-unlisted') {
        $problems = @(Get-PrepareProblemLines $script:ExeOut)
        Check ($problems.Count -eq 1 -and $problems[0] -eq $want) 'the unlisted version has no other Prepare problem, including no reinstall line'
    }
    if ($case -eq 'facts-missing') {
        $control = Start-Process -FilePath (Join-Path $env:WINDIR 'System32\PING.EXE') -ArgumentList '-n', '1', '127.0.0.1' -WindowStyle Hidden -PassThru
        try {
            Check (Wait-Until {
                @(Get-Event -SourceIdentifier $source -ErrorAction SilentlyContinue | Where-Object {
                    $_.SourceEventArgs.NewEvent.ProcessID -eq $control.Id -and $_.SourceEventArgs.NewEvent.ParentProcessID -eq $PID
                }).Count -eq 1
            } 10) 'the process-start subscriber received its control event'
        } finally { $control.Dispose() }
        $started = @(Get-Event -SourceIdentifier $source -ErrorAction SilentlyContinue | Where-Object {
            $event = $_.SourceEventArgs.NewEvent
            $event.ProcessName -ieq 'rbsp-prepare.exe' -and $event.ParentProcessID -eq $appPid
        })
        Check ($started.Count -eq 0) 'the app started no helper without the executable'
        Check (-not (Wait-UiEnabled $appPid (Get-CoreText 'T_BTN_PREPARE') 2)) 'Prepare is disabled without the helper'
    }
} finally {
    Stop-App
    if ($source) {
        Unregister-Event -SourceIdentifier $source -ErrorAction SilentlyContinue
        Remove-Event -SourceIdentifier $source -ErrorAction SilentlyContinue
    }
    if ($hidden -and (Test-Path -LiteralPath $hidden) -and -not (Test-Path -LiteralPath $helper)) {
        Move-Item -LiteralPath $hidden -Destination $helper
    }
}
Done
