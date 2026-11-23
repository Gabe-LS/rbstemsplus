param([string]$Trace = (Join-Path $PSScriptRoot 'suites/admin-paths/worker-trace.ps1'))
$ErrorActionPreference = 'Stop'
$VmRun = @{ Run = 'host-test' }
. $Trace
$bin = 'C:\Program Files\RB Stems Plus\bin'
$old = "$bin\0123456789ab\RB Stems Plus.exe"
$new = "$bin\fedcba987654\RB Stems Plus.exe"
$app = 'C:\Users\rb\AppData\Local\Programs\RB Stems Plus\1.5.1\admin\RB Stems Plus.exe'
function Event([int]$id, [int]$workerPid, [string]$image, [string]$verb = '') {
    [pscustomobject]@{ Id = $id; Pid = $workerPid; Image = $image; Command = "`"$image`" `"$verb`" `"--proto`" `"1`"" }
}
function Assert([bool]$ok, [string]$what) {
    if (-not $ok) { throw $what }
    "PASS $what"
}
$starts = @(Get-WorkerStarts @(
    (Event 1 12 'C:\Users\rb\AppData\Local\Programs\RB Stems Plus\1.5.1\RB Stems Plus.exe'),
    (Event 1 13 ($old -replace '^C:', '\Device\HarddiskVolume3') 'install-worker'),
    (Event 1 14 $new 'install')
))
Assert (Test-WorkerRuns $starts @($old, $new) @('install-worker', 'install')) 'ordered worker starts with full DOS and device paths'
$substitution = @($starts[0], (Event 1 14 $app 'install'))
Assert (-not (Test-WorkerRuns $substitution @($old, $new) @('install-worker', 'install'))) 'app-folder substitute cannot pass as the bin worker'
$wrongVerb = @($starts[0], (Event 1 14 $new 'uninstall'))
Assert (-not (Test-WorkerRuns $wrongVerb @($old, $new) @('install-worker', 'install'))) 'real worker paths with the wrong action fail'
$plant = 'C:\Users\rb\AppData\Local\Programs\RB Stems Plus\1.5.1\admin\version.dll'
$ntdll = Event 5 13 'C:\Windows\System32\ntdll.dll'
$unrelated = Event 5 16 $plant
Assert (Test-WorkerDllSafe @($ntdll, $unrelated) 13 @($plant)) 'image loads from another process do not count against the worker'
Assert (-not (Test-WorkerDllSafe @($ntdll, (Event 5 13 $plant)) 13 @($plant))) 'the worker loading a planted DLL fails the check'
Assert (-not (Test-WorkerDllSafe @($unrelated) 13 @($plant))) 'absence of positive worker image-load evidence fails the check'
'worker-trace-test: 6 passed, 0 failed'
