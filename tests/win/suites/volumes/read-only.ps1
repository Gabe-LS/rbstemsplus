# As SYSTEM: the VHDX detached and attached again read-only (FILE_READ_ONLY_VOLUME); the GUID
# path saved again in case it changed
$fs = Get-VolFs
$i = Read-VolInfo $fs
$vhd = Get-VolVhd $fs
$rc = DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk`nattach vdisk readonly"
Check ($rc -eq 0) "diskpart attached the $fs VHDX read-only (exit $rc)"
$guid = $null
Check (Wait-Until { $script:guid = Get-VolGuid $fs; [bool]$script:guid } 30) "its volume is back ($guid)"
$v = Get-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Get-Disk -ErrorAction SilentlyContinue
Check ($v -and $v.IsReadOnly) "its disk is read-only ($(if ($v) { $v.IsReadOnly }))"
Save-VolInfo $fs @{ Letter = $i.Letter; Guid = $guid }
Done
