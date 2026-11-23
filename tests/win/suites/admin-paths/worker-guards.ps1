if (-not (Test-Path -LiteralPath $GuardFixture)) { Skip 'needs the SYSTEM fixture step' }
$fixture = Get-Content -Raw -LiteralPath $GuardFixture | ConvertFrom-Json
$p = Read-TestPayload
if ($VmRun.Step -eq 'first-worker') {
    if ($fixture.Case -ne 'plant-dlls') { throw 'Missing planted DLL fixture' }
    foreach ($name in $GuardDllNames) {
        if (-not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $fixture.App) $name))) {
            throw "Missing planted DLL: $name"
        }
    }
    Invoke-WorkerCacheAction T_BTN_INSTALL_CACHE T_INSTALL '(?m)ok: install\r?$' -Settled { Test-CacheOn }
    Check (Test-CacheOn) 'Stems Cache is on'
    Check ((Get-Sha $RbOrt) -eq $p.windows.bridge.sha256) 'the released bridge is live'
} else {
    if (-not (Test-CacheOn)) { Skip 'the first worker did not finish installing Stems Cache' }
    if ($VmRun.Step -eq 'retry') {
        if ($fixture.Case -ne 'trace-retry') { throw 'Missing retry trace' }
        Invoke-WorkerCacheAction T_BTN_UNINSTALL_CACHE T_UNINSTALL '(?m)ok: uninstall\r?$' -Settled { -not (Test-CacheOn) }
        Check (-not (Test-CacheOn)) 'Stems Cache is off after retry'
        Check ((Get-Sha $RbOrt) -eq (Read-WorkerFacts).PioneerSha) "Pioneer's DLL is back"
    } else {
        if ($fixture.Case -ne ($VmRun.Step -replace '^refuse-', '')) { throw 'Wrong worker guard fixture' }
        if ($fixture.Holder -and -not ((Test-Path -LiteralPath $GuardReady) -and (Get-Process -Id $fixture.Holder -ErrorAction SilentlyContinue))) {
            throw 'The blocking handle is missing'
        }
        $target = [regex]::Escape($fixture.Target)
        $why = "installed worker isn't admin-only: $target"
        if ($fixture.Case -eq 'empty-dacl') { $why = "couldn't check the installed worker" }
        Invoke-WorkerCacheAction T_BTN_UNINSTALL_CACHE T_UNINSTALL "(?ms)$why\s+.*FAILED: uninstall \[exit 4\]" -DismissFailure
        Check (Test-CacheOn) 'Stems Cache is still on'
        Check ((Get-Sha $RbOrt) -eq $p.windows.bridge.sha256) 'the live bridge was left in place'
        if ($fixture.Holder) {
            Check ([bool](Get-Process -Id $fixture.Holder -ErrorAction SilentlyContinue)) 'the blocking handle stayed open through the refusal'
        }
    }
}
Done
