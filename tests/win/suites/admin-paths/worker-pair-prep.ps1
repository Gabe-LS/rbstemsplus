$pair = Get-WorkerReleasePair
foreach ($p in $pair.Old, $pair.New) {
    Check ([bool]$p.windows.app.admin_sha256) "fixture: app $($p.windows.app.version), worker $($p.windows.app.admin_sha256), bridge $($p.windows.bridge.sha256)"
}
Done
