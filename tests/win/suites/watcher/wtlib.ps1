# wtlib.ps1: watcher.suite's own helpers, after lib/vmlib.ps1, worker/rblib.ps1,
# uninstall/cases/unlib.ps1 and uac/uaclib.ps1: the watcher's processes and records (the Run value,
# StartupApproved), its log, Demucs v4's buttons, and R1's state files (C:\rbtest\r1\).
$WtRunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$WtApprovedKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run'
$WtName = 'RB Stems Plus Watcher'                 # lifecycle.h LIFE_RUN_NAME
$WtR1Dir = 'C:\rbtest\r1'                         # Pioneer's DLL kept while a foreign one is live
$WtAnswerLock = 'Local\io.github.rbstemsplus.answer'   # the answer lock (app.md section 5.2, N10)

# the watchers running from the apps folder (by full path: Get-AppProcesses)
function Get-Watchers { return @(Get-AppProcesses | Where-Object { $_.ProcessName -like 'rbsp-watch-*' }) }
# the Run value's data ($null when none); StartupApproved's bytes ($null when none)
function Get-RunValue { return (Get-ItemProperty -LiteralPath $WtRunKey -Name $WtName -ErrorAction SilentlyContinue).$WtName }
function Get-ApprovedValue { return (Get-ItemProperty -LiteralPath $WtApprovedKey -Name $WtName -ErrorAction SilentlyContinue).$WtName }
# the support folder's Logs\watcher.log after byte offset from, opened sharing as its writers do
function Read-WatcherLog([long]$from = 0) {
    $f = Join-Path (Get-Layout).Support 'Logs\watcher.log'
    if (-not (Test-Path -LiteralPath $f)) { return '' }
    $s = New-Object IO.FileStream($f, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]'ReadWrite, Delete'))
    try {
        if ($from -gt $s.Length) { return '' }   # VRR6: no new text past EOF
        [void]$s.Seek($from, [IO.SeekOrigin]::Begin)
        return (New-Object IO.StreamReader($s, [Text.Encoding]::UTF8)).ReadToEnd()
    } finally { $s.Dispose() }
}
# VRR6: the length r1-foreign saved before its change ($null when none)
function Get-WatcherLogFrom {
    $f = Join-Path $WtR1Dir 'wl-from'
    if (Test-Path -LiteralPath $f) { return [long]([string](Get-Content -LiteralPath $f -TotalCount 1)).Trim() }
    return $null
}
function Get-WatcherLogSize {
    $f = Join-Path (Get-Layout).Support 'Logs\watcher.log'
    if (Test-Path -LiteralPath $f) { return (Get-Item -LiteralPath $f).Length }
    return 0
}
# a Demucs v4 action through the app's window ($script:app): its button, its confirmation's
# button, then the button that shows once it is done (all core text ids), enabled: a statement
# (its Checks are its output), whether it got there in $script:PlusOk. What shows instead is said
# when it doesn't.
function Invoke-PlusAction([string]$button, [string]$confirm, [string]$after, [int]$secs = 300) {
    $script:PlusOk = $false
    Invoke-AppButton $button $confirm
    if (-not $script:Pressed) { return }
    $label = Get-CoreText $after
    $script:PlusOk = Wait-UiEnabled $script:app.Id $label $secs
    Check $script:PlusOk "done: $label enabled"
    if (-not $script:PlusOk) { Show-AppState }
}
# the watcher's records and processes said, for the log
function Show-WatcherState([string]$when) {
    $a = Get-ApprovedValue
    "$when - Run value: $(Get-RunValue); StartupApproved: $(if ($null -ne $a) { ($a | ForEach-Object { '{0:x2}' -f $_ }) -join ' ' } else { 'none' }); watchers: $(@(Get-Watchers | ForEach-Object { "$($_.Id) $($_.Path)" }) -join '; ')"
}
