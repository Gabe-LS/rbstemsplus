# UP10/UP11: independently stage a candidate for each fault and run its --swap.
$l = Get-Layout
$a = Get-UpdateRelease 'a'
$new = Get-UpdateRelease 'new'
$from = $a.windows.app.version
$to = $new.windows.app.version
Expect ((Get-Content -Raw -LiteralPath $l.Current).Trim() -eq $from) "current names the old app $from"
[void](Start-Process -FilePath $l.Stub)
Wait-UpdateApp
$parentId = $UpdateApp.Id
$started = ([DateTimeOffset]$UpdateApp.StartTime).ToUnixTimeSeconds()
Expect ($UpdateApp.CloseMainWindow() -and $UpdateApp.WaitForExit(30000)) 'the old app closed before --swap'

$source = Split-Path (Need 'dist-test/new/payload.json' 'the update candidate')
New-RollbackFixture $l $from $new $source $parentId $started
$folder = Join-Path $l.Apps $to
$exe = Join-Path $folder 'RB Stems Plus.exe'
$records = Join-Path $l.Support 'update'
$log = Join-Path $l.Support 'Logs\app.log'
$before = @(Get-Content -LiteralPath $log).Count
$saved = Join-Path $l.Support 'installed-release\payload.json'
$savedHash = (Get-FileHash -LiteralPath $saved).Hash
$stubHash = (Get-FileHash -LiteralPath $l.Stub).Hash
$trigger = $null
$why = Get-CoreText 'T_UPDATE_R_SWAP'
switch ($VmRun.Step) {
    'rollback-missing-dll' {
        # both cores: the AnyCPU candidate started from this step's PowerShell runs as that
        # PowerShell does (x64 under emulation on ARM64, as hidden.exe starts it), not natively
        foreach ($arch in 'x64', 'arm64') { Remove-Item -LiteralPath (Join-Path $folder "$arch\rbsp-core.dll") }
        $outcome = '--swap: .*rbsp-core\.dll.*: the app being replaced is started again'
        $why = ''
    }
    'rollback-xaml' {
        $trigger = 'broken-offscreen-xaml'
        $outcome = '--swap: the off-screen WPF start failed \(XamlParseException:'
    }
    'rollback-offscreen-hang' {
        $trigger = 'hang-offscreen'
        $outcome = "--swap: the start didn't finish in time: the update stops here"
    }
    default { Expect $false "unknown rollback lap $($VmRun.Step)" }
}

$savedTrigger = $env:RBSP_SHELL_TEST
try {
    $env:RBSP_SHELL_TEST = $trigger
    $candidate = Start-Process -FilePath $exe -WorkingDirectory $folder -ArgumentList '--swap' -PassThru
} finally {
    $env:RBSP_SHELL_TEST = $savedTrigger
}
$exited = $candidate.WaitForExit((Get-WaitSecs 90) * 1000)
Assert-UpdateDesktop
Expect ($exited -and $candidate.ExitCode -eq 1) 'the candidate aborted its swap with exit 1'
if ($trigger -eq 'hang-offscreen') {
    $seconds = ($candidate.ExitTime - $candidate.StartTime).TotalSeconds
    Check ($seconds -ge 30 -and $seconds -lt 90) "the watchdog ended the hung off-screen start ($seconds seconds)"
}
$lines = @(Get-Content -LiteralPath $log | Select-Object -Skip $before)
Check ([bool]($lines -match $outcome)) "the candidate reached the fault: $outcome"
Check (-not ($lines -match 'update: .+ started; current is ')) 'the candidate did not reach the commit point'
Expect ((Get-Content -Raw -LiteralPath $l.Current).Trim() -eq $from) "current still names $from"
Wait-UpdateApp
Confirm-RollbackResult $UpdateApp $why (Join-Path $records 'result')
foreach ($name in 'pending', 'started') {
    Check (-not (Test-Path -LiteralPath (Join-Path $records $name))) "no $name record remains"
}
Check ((Get-FileHash -LiteralPath $saved).Hash -eq $savedHash) 'the installed release is unchanged'
Check ((Get-FileHash -LiteralPath $l.Stub).Hash -eq $stubHash) 'the stable stub is unchanged'
Check ((Get-ItemProperty -LiteralPath $UpdateEntry).DisplayVersion -eq $a.payload_version) "Installed apps still shows $($a.payload_version)"
Stop-App

# The recovery may start the old app while the failing process still holds its files.
[void](Start-Process -FilePath $l.Stub)
Wait-UpdateApp
Check (Wait-Until { -not (Test-Path -LiteralPath $folder) } 60) 'the failed candidate folder is gone after the next good start'
Stop-App
Done
