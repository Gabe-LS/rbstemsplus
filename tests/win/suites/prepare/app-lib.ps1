# Release checks shared by Prepare and Status fixtures. These read app-owned evidence.
function Get-PrepareArgument([string]$command, [string]$flag) {
    # The native launcher quotes each argument. The paths and ids in these fixtures have no quotes.
    $match = [regex]::Match($command, '(?:^|\s)"' + [regex]::Escape($flag) + '" "([^"]*)"(?=\s|$)')
    if ($match.Success) { $match.Groups[1].Value }
}

function Get-PrepareLogDate([string]$log, [string]$id) {
    $line = [regex]::Matches($log, '(?m)  prepare: playlist ' + [regex]::Escape($id) + ', kept until ([^\r\n]+)\r?$')
    if ($line.Count) { $line[$line.Count - 1].Groups[1].Value }
}

function Get-PrepareDate([string]$seconds) {
    [DateTimeOffset]::FromUnixTimeSeconds([long]$seconds).ToLocalTime().Date
}

function Get-PrepareStatusRows([string]$tree) {
    [regex]::Matches($tree, '(?m)^\s*DataItem "([^"\r\n]+)" [^\r\n]*pid=\d+') |
        ForEach-Object { $_.Groups[1].Value }
}

function Get-PrepareProblemLines([string]$tree) {
    # The UIA Text item's name can have multiple lines. Use the first fixed part of each core text.
    foreach ($id in 'T_PREP_NO_HELPER', 'T_PREP_NO_REKORDBOX', 'T_PREP_NO_CACHE', 'T_PREP_REINSTALL_CACHE',
        'T_PREP_RB_UNLISTED', 'T_PREP_CONFIG_UNREADABLE', 'T_PREP_DISABLED', 'T_PREP_SPEED', 'T_PREP_MODEL_CHANGED', 'T_PREP_NO_FOLDER') {
        $prefix = (Get-CoreText $id) -replace '\{.*$', ''
        if ($prefix -and $tree.Contains($prefix)) { $id }
    }
}

function Start-PrepareStub([string]$stub) {
    $saved = $env:RBSP_SHELL_TEST
    try {
        $env:RBSP_SHELL_TEST = 'prepare'
        Start-Process -FilePath $stub
    } finally { $env:RBSP_SHELL_TEST = $saved }
}

function Start-PrepareApp {
    $script:ShellApp = $null
    $layout = Get-Layout
    if (-not (Test-Path -LiteralPath $layout.Current)) { Skip 'the Prepare TEST app is missing' }
    if (@(Get-Rekordbox).Count) { Expect $false 'the VM fixture requires rekordbox closed throughout' }
    Assert-ShellDesktop
    Expect (-not (Get-AppWindow)) 'the fixture app was closed before launching'
    Start-PrepareStub $layout.Stub
    Wait-ShellApp
    Use-ShellUi invoke --pid $script:ShellApp.Id (Get-CoreText 'T_TAB_PREPARE') --type TabItem --timeout 20
}

function Start-PreparePlaylist([string]$name) {
    $appPid = $script:ShellApp.Id
    Use-ShellUi invoke --pid $appPid $name --type TreeItem --timeout 30
    Use-ShellUi invoke --pid $appPid (Get-CoreText 'T_BTN_PREPARE') --type Button --timeout 30
    $filter = "Name = 'rbsp-prepare.exe' AND ParentProcessId = $appPid"
    Expect (Wait-Until {
        $script:PrepareRun = Get-CimInstance Win32_Process -Filter $filter |
            Where-Object { Get-PrepareArgument $_.CommandLine '--keep' } | Select-Object -First 1
        $null -ne $script:PrepareRun
    } 20) "the app started a keep helper for $name"
}

function Get-PrepareHelper([int]$appPid, [int]$helperPid) {
    Get-CimInstance Win32_Process -Filter "Name = 'rbsp-prepare.exe' AND ProcessId = $helperPid AND ParentProcessId = $appPid"
}

function Test-PrepareSuspended($process) {
    $process.Refresh()
    if ($process.HasExited) { return $false }
    $threads = @($process.Threads)
    try {
        return $threads.Count -gt 0 -and @($threads | Where-Object {
            $_.ThreadState -ne [Diagnostics.ThreadState]::Wait -or $_.WaitReason -ne [Diagnostics.ThreadWaitReason]::Suspended
        }).Count -eq 0
    } catch [InvalidOperationException] { return $false }
}

function Get-PrepareCapacity([decimal]$current, [decimal]$required, [long]$available) {
    $rounded = [Math]::Ceiling($required * 10) / 10
    $headroom = [Math]::Max([decimal]0, $available / [decimal]1GB - 1)
    if ($rounded -gt $current -and $rounded - $current -le $headroom) {
        return $rounded.ToString('0.#', [cultureinfo]::InvariantCulture)
    }
}
