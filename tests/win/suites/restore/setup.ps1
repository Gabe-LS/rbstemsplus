# restore.suite's set-ups, as SYSTEM (rekordbox's folder is admin-only), by the step's name:
#   live-deleted   rekordbox's onnxruntime.dll deleted (as Defender would, R4.I71)
#   junction       the bridge put back (the served TEST build's bridge file), then rekordbox's
#                  version folder renamed to "<name>.real" and a junction made at its name
#   unjunction     the junction removed and the folder renamed back, the bridge put back
# rekordbox isn't running (cache-install closed it; nothing here starts it).
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
$p = Read-TestPayload
$bridge = ([string]$p.windows.bridge.sha256).ToLowerInvariant()
$real = "$RbDir.real"
function Put-Bridge {
    $src = Need "dist-test/$([string]$p.windows.bridge.file)" 'the TEST build'
    Copy-Item -LiteralPath $src -Destination $RbOrt -Force
    Check ((Get-Sha $RbOrt) -eq $bridge) "the bridge put back as rekordbox's onnxruntime.dll"
}
Check (@(Get-Rekordbox).Count -eq 0) 'rekordbox is not running'
switch ($VmRun.Step) {
    'live-deleted' {
        Remove-Item -LiteralPath $RbOrt -Force -ErrorAction SilentlyContinue
        Check (-not (Test-Path -LiteralPath $RbOrt)) "rekordbox's onnxruntime.dll deleted"
    }
    'junction' {
        Put-Bridge
        Rename-Item -LiteralPath $RbDir -NewName (Split-Path -Leaf $real)
        New-Item -ItemType Junction -Path $RbDir -Target $real | Out-Null
        Check ([bool]((Get-Item -LiteralPath $RbDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) "rekordbox's version folder is a junction ($RbDir -> $real)"
    }
    'unjunction' {
        if (Test-Path -LiteralPath $real) {
            [IO.Directory]::Delete($RbDir)   # the junction itself, never its target
            Rename-Item -LiteralPath $real -NewName (Split-Path -Leaf $RbDir)
        }
        Check (-not ((Get-Item -LiteralPath $RbDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) "rekordbox's version folder is a folder again"
        Put-Bridge
    }
    default { Check $false "no set-up named $($VmRun.Step)" }
}
Done
