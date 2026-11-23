# kit-lib.ps1 (dot-source it): the kit's shared paths and helpers on the PC, for the test account.
# The kit lives in %TEMP%\rbsp-kit (staged by pc-session.sh with SHA256SUMS); everything it writes
# goes to %TEMP%\rbst (logs, screenshots, snapshots, the session task's in and out files).
$Kit = $PSScriptRoot
$B = Join-Path $env:TEMP 'rbst'
[void][IO.Directory]::CreateDirectory((Join-Path $B 'shots'))
$Log = Join-Path $B 'midi-auto.log'
$AppLogs = Join-Path $env:LOCALAPPDATA 'rbstemsplus\Logs'
$Bridge = Join-Path $AppLogs 'bridge.log'
$ProbeRows = 'C:\rbtest\rows.txt'
$TaskName = 'RBSP session'
New-Variable -Name RbCloseAcceptedPattern -Option Constant -Value '^\S+ close asked: True$'
# The kit writes only to the system drive (coordinator 2026-11-19: the owner's backup disk is
# never written to or chosen): PATH's full form must start with %SystemDrive%\ and not be a UNC or
# device path; a throw otherwise. Every step that writes outside %TEMP% calls it.
function Assert-SystemDrivePath([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    $sys = $env:SystemDrive.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ($full.StartsWith('\\') -or -not $full.StartsWith($sys, [StringComparison]::OrdinalIgnoreCase)) {
        throw "refused: $full is not on the system drive $sys (the kit writes nowhere else)"
    }
    return $full
}
# rekordbox's own onnxruntime.dll per version (Pioneer's file; Stems Cache replaces it and puts it back)
$OrtPioneer = @{
    '7.2.19' = 'e7a702e13bd3e487705204d518c96f1f116aa25b0ac584741838e20bc1ff6e9f'
    '7.2.20' = '3e55ab558136c0cf413c1e3873faff7c28807bf4b178712a2124e5efd4e03acd'
}
# the rekordbox MIDI mapping the runner drives (learned on the test account, see the README)
$Mapping = Join-Path $env:APPDATA 'Pioneer\rekordbox6\MidiMappings\Loopback B.midi.csv'

function Say([string]$s) { $l = (Get-Date).ToString('HH:mm:ss') + '  ' + $s; Write-Host $l; Add-Content -LiteralPath $Log -Value $l }
function Get-RbInstalls([string]$Version) {
    Get-ChildItem -LiteralPath (Join-Path $env:ProgramFiles 'rekordbox') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^rekordbox (\d+(\.\d+)+)$' -and
            (-not $Version -or $_.Name -eq "rekordbox $Version") -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'rekordbox.exe') -PathType Leaf) } |
        Sort-Object { [version]($_.Name -replace '^rekordbox ', '') } |
        ForEach-Object {
            @{ Dir = $_.FullName; Exe = (Join-Path $_.FullName 'rekordbox.exe'); Version = ($_.Name -replace '^rekordbox ', '') }
        }
}
# The requested install (or newest): @{ Dir; Exe; Version } ($null: none when unpinned).
function Get-Rb {
    $d = @(Get-RbInstalls $RbVersion)
    if ($RbVersion -and -not $d.Count) { throw "rekordbox $RbVersion is not installed" }
    if (-not $d.Count) { return $null }
    $d[-1]
}
function Get-RbProcess([Nullable[int]]$SessionId) {
    Get-Process -Name rekordbox -ErrorAction SilentlyContinue | Where-Object { $null -eq $SessionId -or $_.SessionId -eq $SessionId }
}
function Test-RbRunning([Nullable[int]]$SessionId) { @(Get-RbProcess $SessionId).Count -gt 0 }
function Assert-RbProcessVersion([int]$SessionId, $rb) {
    if (@(Get-RbProcess $SessionId | Where-Object { $_.Path -ne $rb.Exe }).Count) {
        throw "another rekordbox executable is running in the test session; expected rekordbox $($rb.Version)"
    }
}
# rekordbox's onnxruntime.dll: 'pioneer', 'other <sha8>', or 'unknown version <v>'
function Get-OrtState($rb = (Get-Rb)) {
    if (-not $rb) { return 'no rekordbox' }
    $sha = (Get-FileHash -LiteralPath (Join-Path $rb.Dir 'onnxruntime.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    $want = $OrtPioneer[$rb.Version]
    if (-not $want) { return "unknown version $($rb.Version) (sha $($sha.Substring(0, 8)))" }
    if ($sha -eq $want) { return 'pioneer' } else { return "other $($sha.Substring(0, 8))" }
}
function Get-AppSwapPendingQueue { return ,([Rbsp.BootDeletes]::Read()) }

# Mirror host_facts.c's root_pending_only using the boot queue as evidence for every entry.
function Test-AppSwapCacheRootPresent {
    $root = Join-Path $env:ProgramFiles 'RB Stems Plus'
    try { $attrs = [IO.File]::GetAttributes($root) }
    catch [IO.FileNotFoundException], [IO.DirectoryNotFoundException] { return $false }
    if (-not ($attrs -band [IO.FileAttributes]::Directory) -or
        ($attrs -band [IO.FileAttributes]::ReparsePoint)) { return $true }
    try {
        if (-not ('Rbsp.BootDeletes' -as [type])) {
            Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32.SafeHandles;

namespace Rbsp {
    public static class BootDeletes {
        private const int NotFound = 2, MoreData = 234, QueryValue = 1, MultiString = 7;
        private const int MaxValueBytes = 16 << 20, MaxDepth = 4, MaxNames = 256, MaxPathBytes = 1099;
        private static readonly Encoding Utf8 = new UTF8Encoding(false, true);
        private static readonly Regex NtPrefix = new Regex(@"\A(?:\*[0-9]+)?\\\?\?\\");
        private static readonly Regex ShortRoot = new Regex(@"\A(?:r|rb|rbs|rbst|rbste|rbstem)~[0-9]+\z");
        private static readonly Regex HashedRoot = new Regex(@"\Arb[0-9a-f]{1,4}~[0-9]+\z");

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern int RegOpenKeyExW(IntPtr key, string subKey, int options, int access,
            out SafeRegistryHandle result);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern int RegQueryValueExW(SafeRegistryHandle key, string name, IntPtr reserved,
            out int type, [Out] byte[] data, ref int size);

        // Read raw UTF-16. Empty destinations in these pairs denote deletes.
        public static string[] Read() {
            SafeRegistryHandle key;
            int rc = RegOpenKeyExW(new IntPtr(unchecked((int)0x80000002)),
                @"SYSTEM\CurrentControlSet\Control\Session Manager", 0, QueryValue, out key);
            if (rc != 0) throw new Win32Exception(rc);
            using (key) {
                return new[] { ReadValue(key, "PendingFileRenameOperations"),
                               ReadValue(key, "PendingFileRenameOperations2") };
            }
        }

        private static string ReadValue(SafeRegistryHandle key, string name) {
            for (int attempt = 0; attempt < 3; attempt++) {
                int size = 0, type;
                int rc = RegQueryValueExW(key, name, IntPtr.Zero, out type, null, ref size);
                if (rc == NotFound) return "";
                if (rc != 0) throw new Win32Exception(rc);
                if (type != MultiString || size < 0 || size > MaxValueBytes) throw new InvalidDataException();
                // A non-null buffer also detects growth when the size query returned zero.
                byte[] bytes = new byte[Math.Max(size, 1)];
                rc = RegQueryValueExW(key, name, IntPtr.Zero, out type, bytes, ref size);
                if (rc == MoreData) continue;
                if (rc == NotFound) return "";
                if (rc != 0) throw new Win32Exception(rc);
                if (type != MultiString) throw new InvalidDataException();
                char[] chars = new char[size / 2];
                Buffer.BlockCopy(bytes, 0, chars, 0, chars.Length * 2);
                return new string(chars);
            }
            throw new Win32Exception(MoreData);
        }

        private static string AsciiLower(string path) {
            char[] chars = path.ToCharArray();
            for (int i = 0; i < chars.Length; i++) {
                if (chars[i] >= 'A' && chars[i] <= 'Z') chars[i] = (char)(chars[i] + 'a' - 'A');
            }
            return new string(chars);
        }

        private static bool RenamesIntoRoot(string destination) {
            foreach (string part in destination.Split('\\')) {
                if (part == "rb stems plus" || (part.Length <= 8 && ShortRoot.IsMatch(part)) ||
                    (part.Length == 8 && HashedRoot.IsMatch(part))) return true;
            }
            return false;
        }

        public static bool ContainsOnlyDeletes(string root, string[] queue) {
            var deletes = new Dictionary<string, int>(StringComparer.Ordinal);
            int order = 0;
            foreach (string value in queue) {
                // Split preserves the empty destination of a delete. Discard the artificial
                // empty element after the final NUL before pairing sources with destinations.
                string[] names = value.Split('\0');
                int count = value.EndsWith("\0", StringComparison.Ordinal) ? names.Length - 1 : names.Length;
                for (int i = 0; i + 1 < count; i += 2, order++) {
                    string destination = names[i + 1];
                    if (destination.Length != 0) {
                        if (destination[0] == '!') destination = destination.Substring(1);
                        if (RenamesIntoRoot(AsciiLower(NtPrefix.Replace(destination, "")))) return false;
                    } else if (names[i].Length != 0) {
                        deletes[AsciiLower(NtPrefix.Replace(names[i], ""))] = order;
                    }
                }
            }
            int bound, visited = 0;
            return deletes.TryGetValue(AsciiLower(root), out bound) &&
                Walk(root, deletes, bound, 1, ref visited);
        }

        private static bool Walk(string dir, Dictionary<string, int> deletes, int bound, int depth, ref int visited) {
            FileAttributes attrs = File.GetAttributes(dir);
            if ((attrs & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != FileAttributes.Directory) return false;
            foreach (FileSystemInfo entry in new DirectoryInfo(dir).EnumerateFileSystemInfos()) {
                string path = Path.Combine(dir, entry.Name);
                int order;
                if (++visited > MaxNames || Utf8.GetByteCount(path) > MaxPathBytes ||
                    !deletes.TryGetValue(AsciiLower(path), out order) || order >= bound ||
                    (entry.Attributes & FileAttributes.ReparsePoint) != 0) return false;
                if ((entry.Attributes & FileAttributes.Directory) != 0 &&
                    (depth >= MaxDepth || !Walk(path, deletes, order, depth + 1, ref visited))) return false;
            }
            return true;
        }
    }
}
'@
        }
        return -not [Rbsp.BootDeletes]::ContainsOnlyDeletes($root, (Get-AppSwapPendingQueue))
    } catch {
        # A queue or directory that could not be read still counts as a root.
        return $true
    }
}

# A leftover root is safe to send to the admin uninstall only if every live runtime can be
# verified as Pioneer's. Check all version folders even when the swap is pinned to one.
function Get-AppSwapCacheState {
    if (-not (Test-AppSwapCacheRootPresent)) { return 'clear' }
    if ((Get-OrtState) -ne 'pioneer' -or (Test-RbRunning)) { return 'blocked' }
    foreach ($dir in Get-ChildItem -LiteralPath (Join-Path $env:ProgramFiles 'rekordbox') -Directory -Force) {
        if ($dir.Name -notmatch '^rekordbox (\d+\.\d+\.\d+)$') { continue }
        $version = $Matches[1]
        if ((Test-Path -LiteralPath (Join-Path $dir.FullName 'onnxruntime.dll')) -and
            (Get-OrtState @{ Dir = $dir.FullName; Version = $version }) -ne 'pioneer') { return 'blocked' }
    }
    return 'leftover'
}
# The session task through schtasks.exe: over SSH a standard account may not use the
# ScheduledTasks cmdlets (their CIM provider answers "Access denied" to a network logon).
# Get-TaskState: Ready, Running, Queued, ... ('' when there is no such task)
function Get-TaskState {
    # stderr redirected under the callers' 'Stop' would end them when the task is absent (PS 5.1)
    $o = @(& { $ErrorActionPreference = 'Continue'; & schtasks.exe /Query /TN $TaskName /FO CSV /NH 2> $null })
    if ($LASTEXITCODE -or -not $o.Count) { return '' }
    return ($o | ConvertFrom-Csv -Header Name, Next, Status | Select-Object -First 1).Status
}
function Start-SessionTask { $o = & schtasks.exe /Run /TN $TaskName 2>&1; if ($LASTEXITCODE) { throw "schtasks /Run: exit $LASTEXITCODE $o" } }
# one command run inside the test account's signed-in session by the scheduled task (SSH runs in
# another session: it can't show or close windows, take screenshots or drive UI Automation)
function Session([string]$cmd, [int]$wait = 30, [string]$arg = '') {
    if ($env:RBKIT_TIMING -ne '1') { return Invoke-SessionTask $cmd $wait $arg }
    $clock = Start-KitTiming
    try { Invoke-SessionTask $cmd $wait $arg }
    finally { Write-KitTiming "desktop-$cmd" $clock }
}

function Invoke-SessionTask([string]$cmd, [int]$wait, [string]$arg) {
    $none = "no answer from the session task in $wait s (the test account not signed in?)"
    # Only one caller owns the mailbox, including while it waits for the previous task to finish.
    try { $lock = [IO.File]::Open((Join-Path $B 'task-lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch [IO.IOException] { return "no answer from the session task (mailbox busy: $($_.Exception.Message))" }
    try {
        $end = (Get-Date).AddSeconds($wait)
        while ((Get-TaskState) -in 'Running', 'Queued') {
            if ((Get-Date) -ge $end) { return $none }
            Start-Sleep -Milliseconds 300
        }
        if ((Get-Date) -ge $end) { return $none }
        $o = Join-Path $B 'task-out.txt'
        if (Test-Path -LiteralPath $o) { Remove-Item -LiteralPath $o }
        $id = [guid]::NewGuid().ToString()
        $request = @($cmd, $arg, $id)
        if ($RbVersion) { $request += $RbVersion }
        Set-Content -LiteralPath (Join-Path $B 'task-cmd.txt') -Value $request
        Start-SessionTask
        do {
            if (Test-Path -LiteralPath $o) {
                $reply = @(Get-Content -LiteralPath $o -TotalCount 2)
                if ($reply.Count -eq 2 -and $reply[0] -eq $id) { return $reply[1] }
            }
            Start-Sleep -Milliseconds 300
        } while ((Get-Date) -lt $end)
        return $none
    } finally { $lock.Dispose() }
}
# the screenshot file a 'shot' answer names ('' if none)
function Shot-Path([string]$answer) { $p = ($answer -split ' shot ', 2)[1]; if ($p -and (Test-Path -LiteralPath $p)) { return $p } else { return '' } }
