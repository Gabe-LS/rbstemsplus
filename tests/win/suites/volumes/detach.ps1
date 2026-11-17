# As SYSTEM: the VHDX detached: its volume gone, its mount folder (C:\mnt\x-<fs>\) left behind
# with nothing mounted there
$fs = Get-VolFs
$vhd = Get-VolVhd $fs
$rc = DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk"
Check ($rc -eq 0) "diskpart detached the $fs VHDX (exit $rc)"
Check (Wait-Until { -not (Get-VolGuid $fs) } 30) 'its volume is gone'
Done
