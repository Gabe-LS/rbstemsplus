Assert-WorkerPromptless
$layout = Get-Layout
if (-not (Test-Path -LiteralPath $layout.Current)) { Skip 'needs the bootstrapped app' }
$p = Read-TestPayload
$app = Get-AppWorker
$worker = Get-BinWorker $p.windows.app.admin_sha256
$folder = Split-Path -Parent $worker
$case = $VmRun.Step
New-Item -ItemType Directory -Path $WorkerTraceDir -Force | Out-Null
Remove-Item -LiteralPath $GuardFixture, $GuardReady, $GuardStop -ErrorAction SilentlyContinue
$fixture = @{ Case = $case; App = $app; Worker = $worker; Target = ''; Sddl = ''; Holder = 0 }
if ($case -eq 'plant-dlls') {
    if ((Get-Sha $app) -ne $p.windows.app.admin_sha256 -or (Test-Path -LiteralPath $CacheRoot)) {
        throw 'The fixture needs the first app-folder worker and an empty worker root'
    }
    foreach ($name in $GuardDllNames) {
        $target = Join-Path (Split-Path -Parent $app) $name
        if (Test-Path -LiteralPath $target) { throw "The fixture file already exists: $target" }
        # Passive, existing Windows DLLs. No custom DLL entry point or proxy is built.
        Copy-Item -LiteralPath (Join-Path $env:WINDIR "System32\$name") -Destination $target
    }
} else {
    if ((Get-Sha $worker) -ne $p.windows.app.admin_sha256) { throw 'The first worker was not installed in bin' }
    switch ($case) {
        'writable' {
            $fixture.Target = $folder
            $acl = Get-Acl -LiteralPath $folder
            $fixture.Sddl = $acl.Sddl
        }
        { $_ -in 'writer', 'null-dacl', 'empty-dacl' } { $fixture.Target = $worker }
        'delete-handle' { $fixture.Target = $folder }
    }
}
$fixture | ConvertTo-Json | Set-Content -LiteralPath $GuardFixture -Encoding UTF8
Start-WorkerTrace
if ($case -eq 'writable') {
    $users = [Security.Principal.SecurityIdentifier]'S-1-5-32-545'
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($users, 'Modify', 'Allow'))
    Set-Acl -LiteralPath $folder -AclObject $acl
    Check ((Get-Acl -LiteralPath $folder).Sddl -ne $fixture.Sddl) 'the worker folder now grants Users Modify'
} elseif ($case -in 'writer', 'delete-handle', 'null-dacl', 'empty-dacl') {
    $helper = Need 'repo/tests/win/suites/admin-paths/worker-hold.ps1' 'vm-run.sh stage'
    $args = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$helper`" `"$($fixture.Target)`" $case `"$GuardReady`" `"$GuardStop`""
    $proc = Start-Process -FilePath "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList $args -WindowStyle Hidden -PassThru
    $fixture.Holder = $proc.Id
    $fixture | ConvertTo-Json | Set-Content -LiteralPath $GuardFixture -Encoding UTF8
    Check (Wait-Until { (Test-Path -LiteralPath $GuardReady) -or $proc.HasExited } 45) 'the holder responded'
    Check ((Test-Path -LiteralPath $GuardReady) -and -not $proc.HasExited) "$case fixture active on $($fixture.Target)"
}
Check (Test-Path -LiteralPath $GuardFixture) "$case fixture recorded"
Done
