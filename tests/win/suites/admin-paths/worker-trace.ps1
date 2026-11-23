# Kernel ProcessStart and ImageLoad tracing. SYSTEM starts/stops the session around the user's
# app action, so a worker that exits before a process poll is still visible.
$WorkerTraceDir = "C:\rbtest\job\$($VmRun.Run)\worker-trace"
$WorkerTraceName = "rbsp-worker-$($VmRun.Run)"
$WorkerTraceFile = "$WorkerTraceDir\worker.etl"

$WorkerTraceSince = "$WorkerTraceDir\since.txt"

# Kernel-Process's ProcessStart has no command line, so the worker's verb comes from the Security
# log's 4688 (Process Creation audited with its command line: set here, in the reverted VM only)
function Start-WorkerTrace {
    New-Item -ItemType Directory -Path $WorkerTraceDir -Force | Out-Null
    Remove-Item -LiteralPath $WorkerTraceFile -Force -ErrorAction SilentlyContinue
    $audit = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Policies\System\Audit'
    New-Item -Path $audit -Force | Out-Null
    Set-ItemProperty -LiteralPath $audit -Name 'ProcessCreationIncludeCmdLine_Enabled' -Value 1 -Type DWord
    $rc = Invoke-Exe "$env:WINDIR\System32\auditpol.exe" @('/set', '/subcategory:{0CCE922B-69AE-11D9-BED3-505054503030}', '/success:enable')
    if ($rc -ne 0) { throw "Could not audit process creation (auditpol: $rc)" }
    [IO.File]::WriteAllText($WorkerTraceSince, (Get-Date).ToUniversalTime().AddSeconds(-5).ToString('o'))
    $rc = Invoke-Exe "$env:WINDIR\System32\logman.exe" @(
        'start', $WorkerTraceName, '-p', 'Microsoft-Windows-Kernel-Process', '0x50', '5',
        '-o', $WorkerTraceFile, '-ets'
    )
    if ($rc -ne 0) { throw "Could not start the worker trace (logman: $rc)" }
}

function Stop-WorkerTrace {
    $rc = Invoke-Exe "$env:WINDIR\System32\logman.exe" @('stop', $WorkerTraceName, '-ets')
    if ($rc -ne 0) { throw "Could not stop the worker trace (logman: $rc)" }
    $events = Get-WinEvent -Path $WorkerTraceFile -Oldest | Where-Object {
        $_.ProviderName -eq 'Microsoft-Windows-Kernel-Process' -and $_.Id -in 1, 5
    }
    # the command lines by pid and image file name (4688 since the trace started)
    $commands = @{}
    $since = [DateTime]::Parse((Get-Content -Raw -LiteralPath $WorkerTraceSince).Trim(), $null, 'RoundtripKind')
    $audits = @(Get-WinEvent -FilterHashtable @{ LogName = 'Security'; Id = 4688; StartTime = $since.ToLocalTime() } -ErrorAction SilentlyContinue)
    foreach ($a in $audits) {
        $d = @{}
        foreach ($field in ([xml]$a.ToXml()).Event.EventData.Data) { $d[$field.Name] = $field.InnerText }
        if ($d.NewProcessId -and $d.NewProcessName) {
            $commands["$([Convert]::ToInt32($d.NewProcessId, 16))|$(Split-Path -Leaf $d.NewProcessName)".ToLowerInvariant()] = $d.CommandLine
        }
    }
    foreach ($event in $events) {
        $data = @{}
        foreach ($field in ([xml]$event.ToXml()).Event.EventData.Data) { $data[$field.Name] = $field.InnerText }
        if ($data.ProcessID -and $data.ImageName) {
            $command = $commands["$($data.ProcessID)|$(Split-Path -Leaf $data.ImageName)".ToLowerInvariant()]
            [pscustomobject]@{ Id = $event.Id; Pid = [int]$data.ProcessID; Image = $data.ImageName; Command = $command }
        }
    }
}

# Process traces use DOS or NT device paths. The full path below the volume must match.
function Test-WorkerTracePath([string]$image, [string]$path) {
    if ($path -notmatch '^[A-Za-z]:\\') { return $false }
    return $image -ieq $path -or
        ($image -match '^\\Device\\HarddiskVolume[0-9]+\\' -and $image.EndsWith($path.Substring(2), [StringComparison]::OrdinalIgnoreCase))
}

function Get-WorkerStarts($events) {
    return @($events | Where-Object {
        $_.Id -eq 1 -and $_.Image -match '\\(?:bin\\[^\\]+|admin)\\RB Stems Plus\.exe$'
    })
}

function Test-WorkerRuns($starts, [string[]]$paths, [string[]]$verbs) {
    if ($starts.Count -ne $paths.Count -or $paths.Count -ne $verbs.Count) { return $false }
    for ($i = 0; $i -lt $paths.Count; $i++) {
        if (-not (Test-WorkerTracePath $starts[$i].Image $paths[$i])) { return $false }
        $verb = [regex]::Escape($verbs[$i])
        if ($starts[$i].Command -notmatch ('(?i)(?:^|\s)"?' + $verb + '"?\s+"?--proto"?\s')) { return $false }
    }
    return $true
}

function Test-WorkerDllSafe($events, [int]$workerPid, [string[]]$plants) {
    $images = @($events | Where-Object { $_.Id -eq 5 -and $_.Pid -eq $workerPid })
    if (-not @($images | Where-Object { $_.Image -match '\\ntdll\.dll$' }).Count) { return $false }
    foreach ($path in $plants) {
        if (@($images | Where-Object { Test-WorkerTracePath $_.Image $path }).Count) { return $false }
    }
    return $true
}
