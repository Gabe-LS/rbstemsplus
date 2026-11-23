# The same whole TEST release fixtures and current/ switch as update.suite. For worker choice
# the app must change and the worker must change, while the bridge is deliberately carried.
function Get-WorkerRelease([string]$name) {
    return Get-Content -LiteralPath (Need "dist-test/$name/payload.json" "the $name TEST release") -Raw | ConvertFrom-Json
}

function Test-WorkerReleasePair($old, $new) {
    if ($old.windows.app.version -notmatch '^\d+\.\d+\.\d+$' -or $new.windows.app.version -notmatch '^\d+\.\d+\.\d+$') { return $false }
    if ([version]$old.windows.app.version -ge [version]$new.windows.app.version) { return $false }
    if ($old.windows.app.admin_sha256 -notmatch '^[0-9a-f]{64}$' -or $new.windows.app.admin_sha256 -notmatch '^[0-9a-f]{64}$') { return $false }
    if ($old.windows.app.admin_sha256.Substring(0, 12) -eq $new.windows.app.admin_sha256.Substring(0, 12)) { return $false }
    if ($old.windows.bridge.file -notmatch '^[A-Za-z0-9_-][A-Za-z0-9._-]*\.dll$' -or $old.windows.bridge.file -cne $new.windows.bridge.file) { return $false }
    return ($old.windows.bridge.sha256 -match '^[0-9a-f]{64}$' -and $old.windows.bridge.sha256 -eq $new.windows.bridge.sha256)
}

function Get-WorkerReleasePair {
    $old = Get-WorkerRelease 'a'
    $new = Get-WorkerRelease 'new'
    if (-not (Test-WorkerReleasePair $old $new)) {
        Skip 'needs an older app in a/, a newer app in new/, distinct worker folders, and one carried bridge'
    }
    return @{ Old = $old; New = $new }
}
