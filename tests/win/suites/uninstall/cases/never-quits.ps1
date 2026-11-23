# UN3: exercise the native after-exit copy with the real app left running through the
# two-minute parent wait. Launch the copy directly so the app's earlier uninstall steps
# do not change the state that after-exit is meant to preserve.
Add-Type @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class AfterExitFixture {
    [StructLayout(LayoutKind.Sequential)]
    struct StartupInfo {
        public int Size;
        public IntPtr Reserved, Desktop, Title;
        public int X, Y, Width, Height, Cols, Rows, Fill, Flags;
        public short Show, ReservedBytes;
        public IntPtr ReservedPtr, In, Out, Err;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInfo {
        public IntPtr Process, Thread;
        public int Pid, Tid;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, ExactSpelling = true)]
    static extern bool CreateProcessW(string exe, StringBuilder command, IntPtr pa, IntPtr ta,
        bool inherit, uint flags, IntPtr env, string folder, ref StartupInfo start, out ProcessInfo info);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    public static Process Start(int app, string exe, string local) {
        IntPtr parent = OpenProcess(0x00100000, true, app); // SYNCHRONIZE
        if (parent == IntPtr.Zero) throw new Win32Exception();
        try {
            var command = new StringBuilder("\"" + exe + "\" --after-exit " +
                parent.ToInt64().ToString("x") + " \"" + local + "\"");
            var start = new StartupInfo { Size = Marshal.SizeOf(typeof(StartupInfo)) };
            ProcessInfo info;
            if (!CreateProcessW(exe, command, IntPtr.Zero, IntPtr.Zero, true, 0, IntPtr.Zero,
                    System.IO.Path.GetDirectoryName(exe), ref start, out info)) throw new Win32Exception();
            try {
                var process = Process.GetProcessById(info.Pid);
                if (process.Handle == IntPtr.Zero) throw new InvalidOperationException("No after-exit process handle");
                return process;
            } finally { CloseHandle(info.Thread); CloseHandle(info.Process); }
        } finally { CloseHandle(parent); }
    }
}
'@

$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'the round 6 install is missing' }
$entry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$usage = Join-Path $l.Local 'Microsoft\CLR_v4.0\UsageLogs\RB Stems Plus.exe.log'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('rbsp-uninstall-' + [guid]::NewGuid().ToString('N').Substring(0, 16))
$copy = Join-Path $temp 'RB Stems Plus.exe'
$ax = $null
try {
    Stop-App
    Start-AppQuiet
    if (-not $script:app) { Done }
    Set-UsageLogEntry $usage
    $tree = Get-AppTree
    $run = (Get-ItemProperty -LiteralPath $runKey -Name 'RB Stems Plus Watcher').'RB Stems Plus Watcher'
    $records = (Get-ItemProperty -LiteralPath $entry).UninstallString
    $shortcut = (Get-FileHash -LiteralPath $l.Shortcut).Hash
    $usageHash = (Get-FileHash -LiteralPath $usage).Hash
    $version = (Get-Content -Raw -LiteralPath $l.Current).Trim()
    $folder = Join-Path $l.Apps $version
    $arch = @(Get-TestArchs)[0]
    New-Item -ItemType Directory -Path $temp | Out-Null
    Copy-Item -LiteralPath (Join-Path $folder "stub\$arch\RB Stems Plus.exe") -Destination $copy
    Copy-Item -LiteralPath (Join-Path $folder "$arch\rbsp-core.dll") -Destination $temp

    Start-DebugLog
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $ax = [AfterExitFixture]::Start($script:app.Id, $copy, $l.Local)
    $ended = $ax.WaitForExit((Get-WaitSecs 145) * 1000)
    $seconds = $clock.Elapsed.TotalSeconds
    Check ($ended -and $seconds -ge 115 -and $seconds -le 145) "the after-exit parent wait lasted $([int]$seconds) s (expected 120 s)"
    Check ($ended -and $ax.ExitCode -eq 2) 'the after-exit returns AX_PARENT_RUNS (2)'
    Check (-not $script:app.HasExited) 'the original app is still running'
    Check ((Get-AppTree) -ceq $tree) 'the installed app tree and current are intact'
    $runAfter = Get-ItemProperty -LiteralPath $runKey -Name 'RB Stems Plus Watcher' -ErrorAction SilentlyContinue
    $entryAfter = Get-ItemProperty -LiteralPath $entry -ErrorAction SilentlyContinue
    Check ($runAfter.'RB Stems Plus Watcher' -ceq $run -and $entryAfter.UninstallString -ceq $records) 'the watcher Run value and Apps & features entry are intact'
    Check ((Test-Path -LiteralPath $l.Shortcut) -and (Get-FileHash -LiteralPath $l.Shortcut).Hash -eq $shortcut) 'the shortcut is intact'
    Check ((Test-Path -LiteralPath $usage) -and (Get-FileHash -LiteralPath $usage).Hash -eq $usageHash) 'the UsageLogs entry is intact'
    Check (Test-Path -LiteralPath (Join-Path $l.Support 'welcomed')) 'the support folder is intact'
    if ($script:DebugOn) {
        $lines = @(Get-DebugLinesOf $ax.Id)
        $lines | ForEach-Object { "debug: $_" }
        $waiting = [Array]::IndexOf($lines, 'after-exit: waiting for RB Stems Plus to quit')
        $refused = [Array]::IndexOf($lines, "after-exit: RB Stems Plus didn't quit: nothing deleted")
        Check ($waiting -ge 0 -and $refused -gt $waiting) 'DBWIN records the wait then the parent timeout'
    }
    Check (Wait-Until { -not (Test-Path -LiteralPath $temp) } 70) 'the after-exit removes its own temp folder'
} finally {
    Stop-DebugLog
    if ($ax) {
        if (-not $ax.HasExited) { $ax.Kill(); [void]$ax.WaitForExit(5000) }
        $ax.Dispose()
    }
    Stop-App
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
Done
