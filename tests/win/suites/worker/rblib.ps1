# rblib.ps1: worker.suite's own facts, after lib/vmlib.ps1 (the suite's second include): rekordbox
# 7.2.19's folder, Stems Cache's root, the prep step's saved facts, the TEST payload, bridge.log,
# and rekordbox started and closed in the user's session (tests/win/probes/vm/quitrun.ps1's way).
$RbDir = Join-Path $env:ProgramFiles 'rekordbox\rekordbox 7.2.19'
$RbExe = Join-Path $RbDir 'rekordbox.exe'
$RbOrt = Join-Path $RbDir 'onnxruntime.dll'
$CacheRoot = Join-Path $env:ProgramFiles 'RB Stems Plus'   # app.md 5.5: the worker's root
$WorkerFacts = 'C:\rbtest\worker\facts.json'               # written by prep (SYSTEM), read after

# a file's SHA-256 in lower case, or $null when it isn't there
function Get-Sha([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}
# prep's facts (PioneerSha: rekordbox's own onnxruntime.dll before the install; Version), or $null
function Read-WorkerFacts {
    if (-not (Test-Path -LiteralPath $WorkerFacts)) { return $null }
    return (Get-Content -Raw -LiteralPath $WorkerFacts | ConvertFrom-Json)
}
# a file of the run's served folder as the run's server sends it (vm-run.sh --serve: dist-test/
# by default, or another folder of the share), as text
function Read-ServedText([string]$name) {
    $w = New-Object Net.WebClient
    try { return [Text.Encoding]::UTF8.GetString($w.DownloadData($VmRun.Url + $name)) }
    catch { throw "the served $name ($($VmRun.Url)$name) couldn't be read: $($_.Exception.Message)" }
    finally { $w.Dispose() }
}
# a file of the run's served folder saved to dest, as the run's server sends it
function Save-ServedFile([string]$name, [string]$dest) {
    $w = New-Object Net.WebClient
    try { $w.DownloadFile($VmRun.Url + $name, $dest) }
    catch { throw "the served $name ($($VmRun.Url)$name) couldn't be saved to ${dest}: $($_.Exception.Message)" }
    finally { $w.Dispose() }
}
# the served TEST build's payload.json (the file vm-run.sh serves and records), never dist-test/'s
# when the run serves another folder
function Read-TestPayload {
    return (Read-ServedText 'payload.json' | ConvertFrom-Json)
}
# bridge.log's text after byte offset from ('' when there is none), opened as its readers must
# (share read, write and delete: bridge.md, the fsync/rename row)
function Read-BridgeLog([string]$path, [long]$from = 0) {
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    $s = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]'ReadWrite, Delete'))
    try {
        if ($from -gt $s.Length) { $from = 0 }   # rotated meanwhile: all of the new file
        [void]$s.Seek($from, [IO.SeekOrigin]::Begin)
        return (New-Object IO.StreamReader($s, [Text.Encoding]::UTF8)).ReadToEnd()
    } finally { $s.Dispose() }
}
# the running rekordbox.exe processes of this version folder, by full path (Win32_Process: an x64
# process's path also from an ARM64 PowerShell), as Process objects
function Get-Rekordbox {
    $ids = @(Get-CimInstance Win32_Process -Filter "Name = 'rekordbox.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($RbExe, [StringComparison]::OrdinalIgnoreCase) } |
            ForEach-Object { $_.ProcessId })
    return @($ids | ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
}
# rekordbox asked to close (its main window, as a user would; nothing plays, so no dialog) and
# waited for, at most secs: its process lingers about 75 s after its window closes, then ends,
# often with its own crash at exit (0xC0000005, phase0-probes.md 2.2). Whether every one ended
# (the only value: each one's exit is said straight to the step's stdout, not the pipeline)
function Close-Rekordbox([int]$secs = 200) {
    $all = $true
    foreach ($p in @(Get-Rekordbox)) {
        $null = $p.Handle   # keeps its exit code readable
        $t = [Diagnostics.Stopwatch]::StartNew()
        [Console]::Out.WriteLine("close sent to rekordbox (pid $($p.Id)): $($p.CloseMainWindow())")
        if ($p.WaitForExit((Get-WaitSecs $secs) * 1000)) {
            [Console]::Out.WriteLine(("  ended $([int]$t.Elapsed.TotalSeconds) s after the close, code 0x{0:X8}" -f $p.ExitCode))
        } else { [Console]::Out.WriteLine("  still running $([int]$t.Elapsed.TotalSeconds) s after the close"); $all = $false }
    }
    return $all
}
# a button (by its name, the core's text) of process pid that is there and enabled, within secs
# (Invoke-UiNamed presses whatever it finds first; a button the core hasn't enabled yet would
# throw)
function Wait-UiEnabled([int]$procId, [string]$name, [int]$secs) {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [Windows.Automation.AutomationElement]
    return Wait-Until {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)
        $nc = New-Object Windows.Automation.AndCondition(
            (New-Object Windows.Automation.PropertyCondition($A::NameProperty, $name)),
            (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            $e = $w.FindFirst([Windows.Automation.TreeScope]::Descendants, $nc)
            if ($e -and $e.Current.IsEnabled) { return $true }
        }
        $false
    } $secs
}
