# cleanup-runners.ps1: the kit's own stale runners on the admin login, stopped. A runner is
# "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand X" (exactly), where X
# decodes to the kit's runner command ("# rbkit-runner" first), to the earlier stdin bootstrap with the
# job watchdog, or to the earliest stdin bootstrap.
$ErrorActionPreference = 'Stop'
$cutoff = [DateTime]::UtcNow.AddMinutes(-20)   # above the longest kitps deadline (admin-worker, 900 s)
$exe = Join-Path $PSHOME 'powershell.exe'
$executables = @('powershell', 'powershell.exe', $exe, ('"' + $exe + '"'))
$flags = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand '
$legacy = '$ProgressPreference = "SilentlyContinue"; $s = [Console]::In.ReadToEnd(); & ([scriptblock]::Create($s)); exit $LASTEXITCODE'
function Is-KitCommand([string]$encoded) {
    if ($encoded -notmatch '^[A-Za-z0-9+/]+={0,2}$') { return $false }
    try { $text = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($encoded)) } catch { return $false }
    # the encoding must round-trip exactly (no lower-cased or altered copies)
    if ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($text)) -cne $encoded) { return $false }
    return $text.StartsWith("# rbkit-runner`n", [StringComparison]::Ordinal) -or $text -ceq $legacy -or
        ($text.Contains('RbkitWatchdog') -and $text.Contains('[Console]::In.ReadToEnd()'))
}
function Is-StaleRunner($process) {
    if ($process.ProcessId -eq $PID -or -not $process.CreationDate -or $process.CreationDate.ToUniversalTime() -ge $cutoff) { return $false }
    foreach ($prefix in $executables) {
        if ($process.CommandLine -and $process.CommandLine.StartsWith($prefix + ' ' + $flags, [StringComparison]::OrdinalIgnoreCase)) {
            return Is-KitCommand $process.CommandLine.Substring($prefix.Length + 1 + $flags.Length)
        }
    }
    return $false
}
Trace-KitPhase 'cleanup-runners: before process listing'
$runners = @(Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" | Where-Object { Is-StaleRunner $_ })
Trace-KitPhase 'cleanup-runners: after process listing'
foreach ($runner in $runners) {
    $id = $runner.ProcessId
    $target = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $target) { continue }
    try {
        # Pin the process handle and recheck the CIM row to guard against PID reuse.
        [void]$target.Handle
        $current = Get-CimInstance Win32_Process -Filter "ProcessId = $id"
        if ($current -and $current.CreationDate -eq $runner.CreationDate -and (Is-StaleRunner $current)) {
            Stop-Process -InputObject $target -Force
            "STOPPED kit runner PID $id (started $($runner.CreationDate.ToUniversalTime().ToString('o')))"
        }
    } catch {
        if (-not $target.HasExited) { throw }
    } finally { $target.Dispose() }
}
