# As SYSTEM: two SMB shares of the VM's own (by-hand.md's IN1): rbnet, which the location step maps
# through 127.0.0.1 when \\Mac\win can't be mapped from its session (a real DRIVE_REMOTE either
# way), and rbgone, which net-remote maps and then removes under its mapping. Everyone may change
# them (share and folder); the revert takes them away.
$svc = Get-Service LanmanServer -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -ne 'Running') { Start-Service LanmanServer }
Check ((Get-Service LanmanServer -ErrorAction SilentlyContinue).Status -eq 'Running') 'the Server service (LanmanServer) runs'
$everyone = (New-Object Security.Principal.SecurityIdentifier 'S-1-1-0').Translate([Security.Principal.NTAccount]).Value
foreach ($n in 'rbnet', 'rbgone') {
    $d = "C:\rbtest\smb\$n"
    New-Item -ItemType Directory -Force $d | Out-Null
    [void](Invoke-Exe "$env:WINDIR\System32\icacls.exe" @($d, '/grant', '*S-1-1-0:(OI)(CI)M'))
    Get-SmbShare -Name $n -ErrorAction SilentlyContinue | Remove-SmbShare -Force
    New-SmbShare -Name $n -Path $d -FullAccess $everyone | Out-Null
    $s = Get-SmbShare -Name $n -ErrorAction SilentlyContinue
    Check ($s -and $s.Path -eq $d) "the share $n is $d ($(if ($s) { $s.Path }))"
}
Done
