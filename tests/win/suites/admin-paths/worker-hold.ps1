param([string]$Path, [string]$Kind, [string]$Ready, [string]$Stop)
$ErrorActionPreference = 'Stop'
Add-Type -Namespace RbspGuard -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFileW(
    string path, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
[DllImport("advapi32.dll")]
public static extern uint GetSecurityInfo(Microsoft.Win32.SafeHandles.SafeFileHandle h, int kind, uint info,
    IntPtr owner, IntPtr group, out IntPtr dacl, IntPtr sacl, out IntPtr sd);
[DllImport("advapi32.dll")]
public static extern uint SetSecurityInfo(Microsoft.Win32.SafeHandles.SafeFileHandle h, int kind, uint info,
    IntPtr owner, IntPtr group, IntPtr dacl, IntPtr sacl);
[DllImport("advapi32.dll")]
public static extern bool GetSecurityDescriptorControl(IntPtr sd, out ushort control, out uint revision);
[DllImport("advapi32.dll")]
public static extern bool InitializeAcl(IntPtr acl, uint size, uint revision);
[DllImport("kernel32.dll")]
public static extern IntPtr LocalFree(IntPtr p);
'@
# OPEN_EXISTING, share read/write/delete. Request WRITE_DATA or DELETE, leaving read/hash probes
# available but conflicting with the core's read-share-only hold.
$aclCase = $Kind -in 'null-dacl', 'empty-dacl'
$access = if ($aclCase) { 0x60000 } elseif ($Kind -eq 'writer') { 2 } else { 0x10000 }
$sd = $oldDacl = $newDacl = $zero = [IntPtr]::Zero
$changed = $false
$h = [RbspGuard.Native]::CreateFileW($Path, $access, 7, [IntPtr]::Zero, 3, 0x02000000, [IntPtr]::Zero)
try {
    if ($h.IsInvalid) { throw "Open failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    if ($aclCase) {
        # Keep READ_CONTROL | WRITE_DAC on this very file so even an empty DACL can be restored.
        $err = [RbspGuard.Native]::GetSecurityInfo($h, 1, 4, $zero, $zero, [ref]$oldDacl, $zero, [ref]$sd)
        if ($err) { throw "GetSecurityInfo: $err" }
        [uint16]$control = 0
        [uint32]$rev = 0
        if (-not [RbspGuard.Native]::GetSecurityDescriptorControl($sd, [ref]$control, [ref]$rev)) {
            throw 'GetSecurityDescriptorControl failed'
        }
        if ($Kind -eq 'empty-dacl') {
            $newDacl = [Runtime.InteropServices.Marshal]::AllocHGlobal(8)
            if (-not [RbspGuard.Native]::InitializeAcl($newDacl, 8, 2)) { throw 'InitializeAcl failed' }
        }
        $err = [RbspGuard.Native]::SetSecurityInfo($h, 1, 2147483652, $zero, $zero, $newDacl, $zero)
        if ($err) { throw "SetSecurityInfo ($Kind): $err" }
        $changed = $true
    }
    Set-Content -LiteralPath $Ready -Value $PID
    $until = (Get-Date).AddMinutes(7)
    while ((Get-Date) -lt $until -and -not (Test-Path -LiteralPath $Stop)) { Start-Sleep -Milliseconds 200 }
} finally {
    try {
        if ($changed) {
            $restore = if ($control -band 0x1000) { 2147483652 } else { 536870916 }
            $err = [RbspGuard.Native]::SetSecurityInfo($h, 1, $restore, $zero, $zero, $oldDacl, $zero)
            if ($err) { throw "Restoring the worker DACL: $err" }
        }
    } finally {
        if ($sd -ne $zero) { [void][RbspGuard.Native]::LocalFree($sd) }
        if ($newDacl -ne $zero) { [Runtime.InteropServices.Marshal]::FreeHGlobal($newDacl) }
        $h.Dispose()
    }
}
