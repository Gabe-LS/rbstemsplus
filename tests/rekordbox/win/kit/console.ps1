# console.ps1: the test account's desktop session, for the owner's admin SSH login. pc-session.sh
# runs the hash-checked copy with $Mode and $Sid (the test account's SID) set by the prelude.
#   state  the sessions (by the WTS API: no localised quser text): "SESSION id user state [console]"
#   up     the test account's existing session put on the physical console, unlocked, by tscon run
#          as SYSTEM through a one-shot task "RBSP console" with its own watchdog.
#          Refused when another account is active on the console (the owner at the PC). Exit 3
#          when the account has no session (it must be signed in first: pc-session.sh session up).
#   down   the test account's sessions disconnected (WTS): its desktop is locked (only its
#          password reconnects it) and its programs stay as they are.
# Output ends with "CONSOLE ok id", "CONSOLE none", "CONSOLE down" or "REFUSED: why".
# Exit 0 done; 1 failed; 2 refused; 3 no session; 142 watchdog.
$ErrorActionPreference = 'Stop'
if ($Mode -notin 'state', 'up', 'down') { 'REFUSED: mode'; exit 2 }
if ($Sid -notmatch '^S-1-5-21-[0-9-]+$') { 'REFUSED: no test account SID'; exit 2 }
function Invoke-ConsoleStep([string]$phase, [int]$milliseconds, [scriptblock]$action) {
    # A PowerShell pipeline cannot cancel a blocked Windows RPC or compiler. Use the
    # runner's managed callback to terminate this job even while PowerShell is blocked.
    $timer = [Threading.Timer]::new($rbkitJobKiller, $null, $milliseconds, -1)
    try {
        Trace-KitPhase "console: before $phase"
        [Console]::Error.Flush()
        & $action
    } finally { $timer.Dispose() }
}
$sameAccount = Invoke-ConsoleStep 'admin identity check' 8000 {
    [Security.Principal.WindowsIdentity]::GetCurrent().User.Value -eq $Sid
}
if ($sameAccount) { 'REFUSED: this is the test account, not the admin login'; exit 2 }
$user = Invoke-ConsoleStep 'test account lookup' 8000 {
    ([Security.Principal.SecurityIdentifier]$Sid).Translate([Security.Principal.NTAccount]).Value.Split('\')[-1]
}
Invoke-ConsoleStep 'Add-Type' 15000 {
    Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class RbspWts {
  [StructLayout(LayoutKind.Sequential)] struct SI { public int Id; public IntPtr Name; public int State; }
  [DllImport("wtsapi32.dll", SetLastError = true)] static extern bool WTSEnumerateSessionsW(IntPtr h, int r, int v, out IntPtr p, out int n);
  [DllImport("wtsapi32.dll")] static extern bool WTSQuerySessionInformationW(IntPtr h, int id, int c, out IntPtr p, out int n);
  [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr p);
  [DllImport("wtsapi32.dll", SetLastError = true)] public static extern bool WTSDisconnectSession(IntPtr h, int id, bool wait);
  [DllImport("kernel32.dll")] public static extern int WTSGetActiveConsoleSessionId();
  static string Q(int id, int c) { IntPtr p; int n; if (!WTSQuerySessionInformationW(IntPtr.Zero, id, c, out p, out n)) return ""; string s = Marshal.PtrToStringUni(p); WTSFreeMemory(p); return s ?? ""; }
  // id, user (no domain), state (0 Active, 1 Connected, 4 Disconnected, ...)
  public static List<string> List() {
    var o = new List<string>(); IntPtr p; int n;
    if (!WTSEnumerateSessionsW(IntPtr.Zero, 0, 1, out p, out n)) throw new System.ComponentModel.Win32Exception();
    int sz = Marshal.SizeOf(typeof(SI));
    for (int i = 0; i < n; i++) { var s = (SI)Marshal.PtrToStructure(new IntPtr(p.ToInt64() + i * sz), typeof(SI)); o.Add(s.Id + "\t" + Q(s.Id, 5) + "\t" + s.State); }
    WTSFreeMemory(p); return o; }
}
"@
}
$names = @{ 0 = 'Active'; 1 = 'Connected'; 2 = 'ConnectQuery'; 3 = 'Shadow'; 4 = 'Disconnected'; 5 = 'Idle'; 6 = 'Listen'; 7 = 'Reset'; 8 = 'Down'; 9 = 'Init' }
function Sessions([int]$milliseconds = 8000) {
    Invoke-ConsoleStep 'WTS session snapshot' $milliseconds {
        $c = [RbspWts]::WTSGetActiveConsoleSessionId()
        foreach ($l in [RbspWts]::List()) {
            $id, $u, $st = $l -split "`t"
            if ($u) { [pscustomobject]@{ Id = [int]$id; User = $u; State = $names[[int]$st]; Console = ([int]$id -eq $c) } }
        }
    }
}
function Show($snapshot) {
    foreach ($row in $snapshot) {
        "SESSION $($row.Id) $(if ($row.User -eq $user) { 'test' } else { 'other' }) $($row.State)$(if ($row.Console) { ' console' })"
    }
}

function Wait-ConsoleSessions([scriptblock]$pending) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $remaining = 30000
    do {
        $current = @(Sessions ([Math]::Min(8000, $remaining)))
        if (-not (& $pending $current)) { break }
        $remaining = 30000 - $clock.ElapsedMilliseconds
        if ($remaining -le 0) { break }
        Start-Sleep -Milliseconds ([Math]::Min(500, $remaining))
        $remaining = 30000 - $clock.ElapsedMilliseconds
    } while ($remaining -gt 0)
    $current
}

$snapshot = @(Sessions)
Show $snapshot
$mine = @($snapshot | Where-Object User -eq $user)
switch ($Mode) {
    'state' { if ($mine.Count) { 'CONSOLE ' + $(if (@($mine | Where-Object { $_.Console -and $_.State -eq 'Active' }).Count) { "ok $($mine[0].Id)" } else { 'down' }) } else { 'CONSOLE none' } }
    'down' {
        $connected = { param($rows) $rows | Where-Object { $_.User -eq $user -and $_.State -ne 'Disconnected' } }
        $targets = @(& $connected $mine)
        if (-not $targets.Count) { 'CONSOLE down'; exit 0 }
        foreach ($s in $targets) {
            Invoke-ConsoleStep "WTSDisconnectSession $($s.Id)" 8000 {
                if (-not [RbspWts]::WTSDisconnectSession([IntPtr]::Zero, $s.Id, $false)) {
                    Trace-KitPhase "console: WTSDisconnectSession $($s.Id) returned false"
                    [Console]::Error.Flush()
                }
            }
        }
        $current = @(Wait-ConsoleSessions $connected)
        if (@(& $connected $current).Count) { Show $current; 'CONSOLE still connected'; exit 1 }
        'CONSOLE down'; exit 0
    }
    'up' {
        if (-not $mine.Count) { 'CONSOLE none'; exit 3 }
        if ($mine.Count -gt 1) { "REFUSED: $($mine.Count) sessions of the test account"; exit 2 }
        $s = $mine[0]
        if ($s.Console -and $s.State -eq 'Active') { "CONSOLE ok $($s.Id)"; exit 0 }
        $other = @($snapshot | Where-Object { $_.Console -and $_.User -ne $user -and $_.State -eq 'Active' })
        if ($other.Count) { 'REFUSED: another account is active on the console (someone at the PC): left as it is'; exit 2 }
        # The on-demand task runs outside the SSH job, and Task Scheduler bypasses
        # ExecutionTimeLimit on demand. Arm a separate job watchdog inside the action.
        $tn = 'RBSP console'
        Invoke-ConsoleStep 'register tscon task' 15000 {
            $actionScript = @'
$ErrorActionPreference = 'Stop'
function New-RbkitJobKiller {
__RBKIT_JOB_KILLER__
}
$global:RbkitWatchdog = [Threading.Timer]::new((New-RbkitJobKiller), $null, 20000, -1)
& (Join-Path ([Environment]::SystemDirectory) 'tscon.exe') __RBKIT_SESSION__ /dest:console
exit $LASTEXITCODE
'@
            $actionScript = $actionScript.Replace('__RBKIT_JOB_KILLER__', ${function:New-RbkitJobKiller}.ToString()).Replace('__RBKIT_SESSION__', [string]$s.Id)
            $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($actionScript))
            $a = New-ScheduledTaskAction -Execute (Join-Path ([Environment]::SystemDirectory) 'WindowsPowerShell\v1.0\powershell.exe') -Argument "-NoProfile -NonInteractive -EncodedCommand $encoded"
            $pr = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' -LogonType ServiceAccount -RunLevel Highest
            Register-ScheduledTask -TaskName $tn -Action $a -Principal $pr -Force | Out-Null
        }
        $ready = $false
        try {
            Invoke-ConsoleStep 'start tscon task' 8000 { Start-ScheduledTask -TaskName $tn }
            $onConsole = { param($rows) $rows | Where-Object { $_.Id -eq $s.Id -and $_.User -eq $user -and $_.Console -and $_.State -eq 'Active' } }
            $current = @(Wait-ConsoleSessions { param($rows) -not @(& $onConsole $rows).Count })
            $ready = [bool]@(& $onConsole $current).Count
            if (-not $ready) {
                $res = Invoke-ConsoleStep 'read tscon task result' 8000 {
                    (Get-ScheduledTaskInfo -TaskName $tn).LastTaskResult
                }
            }
        } finally {
            # Let an active task's own watchdog terminate its whole job.
            Invoke-ConsoleStep 'unregister tscon task' 8000 { Unregister-ScheduledTask -TaskName $tn -Confirm:$false }
        }
        Show $current
        if ($ready) { "CONSOLE ok $($s.Id)"; exit 0 } else { "CONSOLE failed (tscon result $res)"; exit 1 }
    }
}
