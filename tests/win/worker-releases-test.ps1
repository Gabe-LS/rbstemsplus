param([string]$Helper = (Join-Path $PSScriptRoot 'suites/admin-paths/worker-releases.ps1'))
$ErrorActionPreference = 'Stop'
. $Helper

function Release([string]$version, [string]$worker, [string]$bridge, [string]$file = 'bridge.dll') {
    @{ windows = @{ app = @{ version = $version; admin_sha256 = $worker }; bridge = @{ sha256 = $bridge; file = $file } } }
}
function Assert([bool]$ok, [string]$what) {
    if (-not $ok) { throw $what }
    "PASS $what"
}

$bridge = 'b' * 64
$workerOld = 'a' * 64
$workerNew = 'c' * 64
$old = Release '1.5.9' $workerOld $bridge
$new = Release '1.5.10' $workerNew $bridge
Assert (Test-WorkerReleasePair $old $new) 'the pair allows numeric semver order and an unchanged bridge'
Assert (-not (Test-WorkerReleasePair $new $old)) 'a reversed app update is refused'
Assert (-not (Test-WorkerReleasePair $old (Release '1.5.10' ($workerOld.Substring(0, 63) + 'd') $bridge))) 'same bin folder is refused'
Assert (-not (Test-WorkerReleasePair $old (Release '1.5.10' ('c' * 12) $bridge))) 'a truncated worker hash is refused'
Assert (-not (Test-WorkerReleasePair $old (Release '1.5.10' $workerNew ('e' * 64)))) 'a bridge-changing pair is refused'
Assert (-not (Test-WorkerReleasePair $old (Release '1.5.10' $workerNew $bridge 'other.dll'))) 'a renamed bridge asset is refused'
'worker-releases-test: 6 passed, 0 failed'
