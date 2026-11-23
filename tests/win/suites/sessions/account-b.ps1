# UN8 (G6): a second local account B (rbB, made here: the VM is reverted after the run) installs
# and runs its own copy. As SYSTEM: B made with a password generated here (New-TestAccount: never
# printed, never on a command line, its file SYSTEM-only and deleted once B's task holds it); a
# task "whether the user is logged on or not" (LogonType Password: B's own logon session, in
# session 0) runs as-b.ps1 with vmlib.ps1 in front: the one-liner against the served TEST build
# into B's profile, B's app and watcher running from B's apps folder; it then waits, keeping B's
# logon session, until b-after.ps1 ends it. The desktop lap uses the existing B desktop. Here: B's checks passed on, and B's processes found by
# path and owner, recorded for b-after.ps1.
$src = Need 'repo/tests/win/suites/sessions/as-b.ps1' 'vm-run.sh stage'
$lib = Need 'repo/tests/win/suites/lib/vmlib.ps1' 'vm-run.sh stage'
$desktop = $VmRun.Step -eq 'account-b-desktop'
if ($desktop) {
    $sid = Get-BSid
    $bSession = Get-BDesktop
    $console = (Get-CimInstance Win32_ComputerSystem).UserName
    if (-not $sid -or -not $bSession -or -not $console -or $console -ieq "$env:COMPUTERNAME\$BName") {
        Skip 'needs the two-user snapshot: A on the console and rbB signed in on a separate desktop'
    }
} else {
    $sid = Get-BSid
    if ($sid) { Skip 'the clean Windows fixture must not already have an rbB account' }
}
# B's folder: B reads its script and writes its report and nothing else of the run's
New-Item -ItemType Directory -Force $BDir | Out-Null
if (-not $desktop) {
    $pwFile = New-TestAccount $BName
    $sid = Get-BSid
    if (-not $sid) { [void](Remove-TestSecrets); Check $false "the account $BName was not created"; Done }
}
$acl = Get-Acl -LiteralPath $BDir
$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier $sid), 'Modify', 'ContainerInherit, ObjectInherit', 'None', 'Allow')))
Set-Acl -LiteralPath $BDir -AclObject $acl
function Q([string]$s) { return "'" + $s.Replace("'", "''") + "'" }
$base = $VmRun.Url
if ($VmRun.Step -eq 'account-b-worker-choice') { $base += 'new/' }
$head = "`$ProgressPreference = 'SilentlyContinue'`r`n" +
"`$VmRun = @{ Url = $(Q $base); Run = $(Q $VmRun.Run); Share = $(Q $VmRun.Share); Results = $(Q $VmRun.Results); Step = 'as-$BName'; Secs = 1800 }`r`n" +
"`$BDir = $(Q $BDir)`r`n"
$run = Join-Path $BDir 'run.ps1'
[IO.File]::WriteAllText($run, $head + [IO.File]::ReadAllText($lib) + "`r`n" + [IO.File]::ReadAllText($src), (New-Object Text.UTF8Encoding $true))
$exe = "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"
$argv = "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$run`""
if ($desktop) {
    $principal = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\$BName" -LogonType Interactive -RunLevel Limited
    $action = New-ScheduledTaskAction -Execute $exe -Argument $argv
    Register-ScheduledTask -TaskName $BTask -Action $action -Principal $principal | Out-Null
    Start-ScheduledTask -TaskName $BTask
} else {
    try {
        Start-TaskAs "$env:COMPUTERNAME\$BName" $pwFile $exe $argv $BTask
    } finally {
        Check (Remove-TestSecrets) "B's password file deleted (Task Scheduler holds it now)"
    }
}
$ready = Join-Path $BDir 'ready.txt'
$got = Wait-Until { Test-Path -LiteralPath $ready } 480
Check $got "B's task reported ($ready)"
if (-not $got) {
    $i = Get-ScheduledTaskInfo -TaskName $BTask -ErrorAction SilentlyContinue
    "task ${BTask}: $((Get-ScheduledTask -TaskName $BTask -ErrorAction SilentlyContinue).State), last result 0x$('{0:X8}' -f [int64]$i.LastTaskResult)"
    Done
}
$n = 0
foreach ($x in [IO.File]::ReadAllLines($ready)) {
    if ($x -match '^(PASS|FAIL) (.*)$') { $n++; Check ($Matches[1] -eq 'PASS') "as ${BName}: $($Matches[2])" } else { "  ${BName}: $x" }
}
Check ($n -ge 1) "B's task checked something ($n)"
$bp = @()
Check (Wait-Until {
        $script:bp = Get-BProcesses
        @($script:bp | Where-Object { $_.Name -like 'rbsp-watch-*' }).Count -ge 1 -and @($script:bp | Where-Object { $_.Name -eq 'RB Stems Plus.exe' }).Count -ge 1
    } 60) "B's app and watcher run from B's apps folder"
$owners = @()
foreach ($p in $bp) {
    $o = Invoke-CimMethod -InputObject $p -MethodName GetOwner
    $owners += $o.User
    "  B: pid $($p.ProcessId) $($p.ExecutablePath), session $($p.SessionId), owner $($o.Domain)\$($o.User)"
}
Check ($bp.Count -ge 2 -and @($owners | Where-Object { $_ -ne $BName }).Count -eq 0) "each one is $BName's"
$expectedSession = if ($desktop) { $bSession } else { 0 }
Check ($bp.Count -ge 2 -and @($bp | Where-Object SessionId -ne $expectedSession).Count -eq 0) "every B process is in session $expectedSession"
[IO.File]::WriteAllLines((Join-Path $BDir 'pids.txt'), [string[]]@($bp | ForEach-Object { "$($_.ProcessId) $($_.ExecutablePath)" }))
Done
