# As SYSTEM (T3c case 3's rest, plan row 5): the VHDX's disk set offline (Set-Disk -IsOffline, as
# diskpart's "offline disk"), the volume mounted at C:\mnt\x-<fs>\ with no letter: its volume no
# longer answers by its GUID path
$fs = Get-VolFs
$i = Read-VolInfo $fs
$d = Get-DiskImage -ImagePath (Get-VolVhd $fs) -ErrorAction SilentlyContinue | Get-Disk -ErrorAction SilentlyContinue
if (-not $d) { Skip "the $fs VHDX isn't attached (the steps before failed: their logs)" }
Set-Disk -Number $d.Number -IsOffline $true
Check ((Get-Disk -Number $d.Number).IsOffline) "the $fs VHDX's disk $($d.Number) is offline"
Check (Wait-Until { -not [IO.Directory]::Exists($i.Guid) } 30) "its volume doesn't answer by its GUID path ($($i.Guid))"
Done
