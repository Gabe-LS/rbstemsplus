# The owner's PC black window (2026-11-19), the session's moves: as SYSTEM, the app's session
# disconnected (WTSDisconnectSession, as RB-KIT's session down) and brought back to the console
# (tscon as SYSTEM: no password, as RB-KIT's session up), 10 times; the app must live through all
# of them. Run by the suite with the window minimized and restored (10 moves each), and once as
# churn-unlock after churn-lock.ps1 locked the workstation (1 move: the healthy unlock).
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RbspChurn {
    [DllImport("wtsapi32.dll", SetLastError = true)] public static extern bool WTSDisconnectSession(IntPtr server, int session, bool wait);
    [DllImport("wtsapi32.dll", SetLastError = true)] static extern bool WTSQuerySessionInformationW(IntPtr server, int session, int cls, out IntPtr buf, out int bytes);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr p);
    // WTS_CONNECTSTATE_CLASS: 0 active, 4 disconnected; -1 unknown
    public static int State(int session) {
        IntPtr b; int n;
        if (!WTSQuerySessionInformationW(IntPtr.Zero, session, 8, out b, out n)) return -1;
        try { return n >= 4 ? Marshal.ReadInt32(b) : -1; } finally { WTSFreeMemory(b); }
    }
}
'@
$cycles = if ($VmRun.Step -eq 'churn-unlock') { 1 } else { 10 }
Expect (Test-Path -LiteralPath $ShellChurnPid) 'churn-start left the app running'
$appId = [int](Get-Content -LiteralPath $ShellChurnPid)
$app = Get-Process -Id $appId -ErrorAction SilentlyContinue
Expect ($null -ne $app) "the app (pid $appId) is running"
$sid = $app.SessionId
Expect ([RbspChurn]::State($sid) -eq 0) "its session $sid is active on the console"
$tscon = Join-Path $env:SystemRoot 'System32\tscon.exe'
for ($i = 1; $i -le $cycles; $i++) {
    Expect ([RbspChurn]::WTSDisconnectSession([IntPtr]::Zero, $sid, $true)) "cycle ${i}: session $sid disconnected"
    Expect (Wait-Until { [RbspChurn]::State($sid) -eq 4 } 20) "cycle ${i}: session $sid reads as disconnected"
    Start-Sleep -Seconds 3
    & $tscon $sid /dest:console | Out-Null
    Expect ($LASTEXITCODE -eq 0) "cycle ${i}: tscon $sid /dest:console (exit $LASTEXITCODE)"
    Expect (Wait-Until { [RbspChurn]::State($sid) -eq 0 } 20) "cycle ${i}: session $sid is active on the console again"
    Start-Sleep -Seconds 4
    Expect (-not $app.HasExited) "cycle ${i}: the app is still running"
}
Check $true "$cycles disconnects and reconnects of session $sid, the app running through them"
Done
