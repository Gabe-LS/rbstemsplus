# PF4: observe an actual Prepare/Keep run from this app and its command line, then
# use the same app and playlist after SYSTEM moves the verified rekordbox folder.
function Get-HelperStarts([string]$source) {
    Get-Event -SourceIdentifier $source -ErrorAction SilentlyContinue |
        ForEach-Object { $_.SourceEventArgs.NewEvent }
}

function Test-HelperArgument([string]$command, [string]$flag, [string]$value) {
    return (Get-PrepareArgument $command $flag) -ieq $value
}

$l = Get-Layout
$ready = Join-Path $l.Local "rbtest-pf4-$($VmRun.Run).json"
$prepared = Join-Path $l.Local "rbtest-prepared-$($VmRun.Run).json"
$moved = "$RbDir.rbtest-pf4-$($VmRun.Run)"
$refusal = $VmRun.Step -eq 'helper-path-refused'
$log = Join-Path $l.Support 'Logs\app.log'
$ort = Join-Path $CacheRoot 'ort\onnxruntime.dll'
$source = 'RBSP-PF4-process-start'
$app = $null
$leaveOpen = $false
$subscribed = $false
try {
    if ($refusal) {
        if (-not (Test-Path -LiteralPath $ready)) { Skip 'the positive helper-path step did not finish' }
        $state = Get-Content -Raw -LiteralPath $ready | ConvertFrom-Json
        $app = Get-AppWindow
        $same = $app -and $app.Id -eq $state.Pid -and [string]$app.StartTime.ToUniversalTime().Ticks -eq $state.Start
        Check $same 'the same app window is open after the move'
        if (-not $same) { Done }
        $renamed = (-not (Test-Path -LiteralPath $RbDir)) -and (Test-Path -LiteralPath (Join-Path $moved 'rekordbox.exe'))
        Check $renamed "the verified version folder was moved from $RbDir"
        if (-not $renamed) { Done }
        $button = $state.Button
    } else {
        if (-not (Test-Path -LiteralPath $l.Current)) { Skip 'the Prepare TEST app is missing' }
        if (-not (Test-Path -LiteralPath $RbExe) -or -not (Test-Path -LiteralPath $ort)) { Skip 'needs the PF4 snapshot with rekordbox and Stems Cache ready' }
        $closed = @(Get-Rekordbox).Count -eq 0
        Check $closed 'the fixture rekordbox is closed'
        if (-not $closed) { Done }
        Stop-App
        $version = (Get-Content -Raw -LiteralPath $l.Current).Trim()
        $helperPath = Join-Path $l.Apps "$version\rbsp-prepare.exe"
        if (-not (Test-Path -LiteralPath $helperPath -PathType Leaf)) { Skip 'the TEST app has no Prepare helper' }
        Start-PrepareStub $l.Stub
        $opened = Wait-Until { $script:app = Get-AppWindow; $null -ne $script:app } 45
        Check $opened 'the installed app opened'
        if (-not $opened) { Done }
        $tab = (Invoke-Rbui invoke --pid $app.Id (Get-CoreText 'T_TAB_PREPARE') --type TabItem --timeout 20) -eq 0
        Check $tab 'the Prepare tab opened'
        if (-not $tab) { Done }
        $selected = (Invoke-Rbui invoke --pid $app.Id 'RBSP PF4' --type TreeItem --timeout 30) -eq 0
        Check $selected 'the fixture playlist was selected'
        if (-not $selected) { Done }
        $button = Get-CoreText 'T_BTN_PREPARE'
        Expect (Wait-UiEnabled $app.Id $button 20) 'the listed, ready installation enables Prepare for the fixture playlist'
        Use-ShellUi read --pid $app.Id --timeout 15
        $problems = @(Get-PrepareProblemLines $script:ExeOut)
        Check ($problems.Count -eq 0) "the ready installation exposes no Prepare problems ($($problems -join ', '))"
    }

    # A process-start indication catches a short-lived child that could escape CIM polling.
    $session = $app.SessionId
    $query = "SELECT * FROM Win32_ProcessStartTrace WHERE SessionID = $session AND (ProcessName = 'rbsp-prepare.exe' OR ProcessName = 'PING.EXE')"
    Register-WmiEvent -Query $query -SourceIdentifier $source | Out-Null
    $subscribed = $true
    $offset = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
    $pressed = (Invoke-Rbui invoke --pid $app.Id $button --type Button --timeout 30) -eq 0
    Check $pressed "the app's $button button was pressed"
    if (-not $pressed) { Done }

    if ($refusal) {
        $pattern = "(?m)  (prepare|helper): no verified current rekordbox, the run isn't started\r?$"
        $logged = Wait-Until { (Read-BridgeLog $log $offset) -match $pattern } 15
        Check $logged "a new app.log line says the run wasn't started because no current rekordbox folder could be verified"

        # A known child proves the event subscription is delivering in this step, too.
        $ping = Start-Process -FilePath (Join-Path $env:WINDIR 'System32\PING.EXE') -ArgumentList '-n', '1', '127.0.0.1' -WindowStyle Hidden -PassThru
        try {
            $tracing = Wait-Until { @(Get-HelperStarts $source | Where-Object { $_.ProcessID -eq $ping.Id -and $_.ParentProcessID -eq $PID }).Count -eq 1 } 10
            Check $tracing 'the process-start trace saw the control PING'
        } finally { $ping.Dispose() }
        Start-Sleep -Seconds 3
        $starts = @(Get-HelperStarts $source | Where-Object { $_.ParentProcessID -eq $app.Id -and $_.ProcessName -ieq 'rbsp-prepare.exe' })
        $newLog = Read-BridgeLog $log $offset
        $preflight = $newLog -match "(?m)  prepare: no verified current rekordbox, the run isn't started\r?$"
        if ($preflight) {
            # The early refusal refreshes the track list with --tracks. A --keep attempt would
            # enter start_run and log the playlist and its keep date before spawning the helper.
            Check ($starts.Count -eq 1 -and $newLog -notmatch '(?m)  prepare: playlist .*?, kept until ') 'only the track-list refresh started after the refusal'
        } else {
            Check ($starts.Count -eq 0) 'the native helper guard refused before starting a process'
        }
        Check (-not $app.HasExited) 'the app is still open after the refusal'
    } else {
        $filter = "Name = 'rbsp-prepare.exe' AND ParentProcessId = $($app.Id)"
        $seen = Wait-Until {
            $script:run = Get-CimInstance Win32_Process -Filter $filter |
                Where-Object { $_.CommandLine -match '(?:^|\s)"--keep"\s' } | Select-Object -First 1
            $null -ne $script:run
        } 20
        Check $seen 'CIM observed a Prepare run, identified by --keep and its app parent'
        if (-not $seen) { Done }
        $tracing = Wait-Until { @(Get-HelperStarts $source | Where-Object { $_.ProcessID -eq $run.ProcessId -and $_.ParentProcessID -eq $app.Id }).Count -eq 1 } 10
        Check $tracing 'the process-start trace saw that same helper'
        $rightExe = $run.ExecutablePath -ieq $helperPath
        $rightOrt = Test-HelperArgument $run.CommandLine '--ort' $ort
        $rightRb = Test-HelperArgument $run.CommandLine '--rekordbox' $RbDir
        Check $rightExe "the helper runs from the installed version ($helperPath)"
        Check $rightOrt "--ort names $ort"
        Check $rightRb "--rekordbox names $RbDir"
        $kept = Join-Path $l.Support 'kept'
        $rightKept = Test-HelperArgument $run.CommandLine '--kept-dir' $kept
        Check $rightKept "--kept-dir names $kept"
        if (-not ($tracing -and $rightExe -and $rightOrt -and $rightRb -and $rightKept)) { Done }
        $set = Get-PrepareArgument $run.CommandLine '--keep'
        $until = Get-PrepareArgument $run.CommandLine '--until'
        if ($set -notmatch '^[A-Za-z0-9_-]{1,64}$' -or $until -notmatch '^\d+$') { throw 'missing keep id or until timestamp' }
        $date = Get-PrepareDate $until
        $long = $date.ToLongDateString()
        $seenDate = Wait-Until { (Get-PrepareLogDate (Read-BridgeLog $log $offset) $set) -ceq $long } 15
        Check $seenDate "N4: app.log gives the selected date in $([cultureinfo]::CurrentCulture.Name)'s long format ($long)"
        Check ($long -cne $date.ToString('yyyy-MM-dd', [cultureinfo]::InvariantCulture)) 'the fixture locale distinguishes long date from ISO'

        $finished = Wait-Until { -not (Get-CimInstance Win32_Process -Filter $filter) } 120
        Check $finished 'the first Prepare helper finished'
        if (-not $finished) { Done }
        $foundSet = Wait-Until {
            (Test-Path -LiteralPath (Join-Path $kept "$set.kept") -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $kept "$set.manifest") -PathType Leaf)
        } 15
        Check $foundSet 'the real keep run wrote both kept files'
        if (-not $foundSet) { Done }
        @{ Set = $set; Until = $until } | ConvertTo-Json | Set-Content -LiteralPath $prepared -Encoding UTF8
        $resumed = Wait-Until {
            foreach ($label in @((Get-CoreText 'T_BTN_PREPARE'), (Get-CoreText 'T_KEEP'))) {
                if (Wait-UiEnabled $app.Id $label 1) { $script:button = $label; return $true }
            }
            return $false
        } 15
        Check $resumed 'the same playlist is ready for a second Prepare or Keep'
        if (-not $resumed) { Done }
        @{ Pid = $app.Id; Start = [string]$app.StartTime.ToUniversalTime().Ticks; Button = $button } |
            ConvertTo-Json | Set-Content -LiteralPath $ready -Encoding UTF8
        $leaveOpen = $true
    }
} finally {
    if ($subscribed) {
        Unregister-Event -SourceIdentifier $source
        Remove-Event -SourceIdentifier $source -ErrorAction SilentlyContinue
    }
    if (-not $leaveOpen) { Stop-App }
}
Done
