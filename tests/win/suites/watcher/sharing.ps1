# SYSTEM: exercise the directory operations under the resident watcher. Leave the snapshot's
# Pioneer files in place after each move. The empty version directory stands in for an uninstall.
$watch = @(Get-Watchers)
Check ($watch.Count -eq 1) 'one watcher runs before the directory operations'
Check (@(Get-Rekordbox).Count -eq 0) 'rekordbox is closed'
if ($script:VmFails) { Done }
$parent = Split-Path $RbDir
$version = Split-Path -Leaf $RbDir
$away = Join-Path $parent 'rbtest-updater-old'
Check (-not (Test-Path -LiteralPath $away)) 'the updater fixture can rename the old version'
if ($script:VmFails) { Done }
try {
    $from = Get-WatcherLogSize
    [IO.Directory]::Move($RbDir, $away)
    Check (Test-Path -LiteralPath (Join-Path $away 'rekordbox.exe')) 'updater: the old version was renamed under the running watcher'
    New-Item -ItemType Directory -Path $RbDir | Out-Null
    Remove-Item -LiteralPath $RbDir
    Check (-not (Test-Path -LiteralPath $RbDir)) 'uninstaller: the version directory can be removed'
    Check (Wait-Until { (Read-WatcherLog $from) -match "rekordbox isn't installed: nothing to ask" } 45) 'the watcher saw the missing version'
} finally {
    if ((Test-Path -LiteralPath $away) -and -not (Test-Path -LiteralPath $RbDir)) {
        [IO.Directory]::Move($away, $RbDir)
    }
}
Check (Test-Path -LiteralPath $RbExe) "$version restored"
$pioneer = Join-Path (Get-UserProfile) 'AppData\Roaming\Pioneer'
foreach ($path in $RbModelDir, (Split-Path $RbModelDir), $pioneer) {
    Check (Test-Path -LiteralPath $path -PathType Container) "sharing fixture exists: $path"
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { continue }
    $away = "$path.rbtest"
    try {
        [IO.Directory]::Move($path, $away)
        Check (-not (Test-Path -LiteralPath $path)) "uninstaller: renamed $path with its subtree"
    } finally {
        if ((Test-Path -LiteralPath $away) -and -not (Test-Path -LiteralPath $path)) {
            [IO.Directory]::Move($away, $path)
        }
    }
}
Check (@(Get-Watchers | Where-Object Id -eq $watch[0].Id).Count -eq 1) 'the same watcher survived all directory operations'
Done
