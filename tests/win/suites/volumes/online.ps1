# As SYSTEM: the disk online again, its volume back by the same GUID path (and in its mount folder)
# for the steps after
$fs = Get-VolFs
$i = Read-VolInfo $fs
$d = Get-DiskImage -ImagePath (Get-VolVhd $fs) -ErrorAction SilentlyContinue | Get-Disk -ErrorAction SilentlyContinue
if (-not $d) { Skip "the $fs VHDX isn't attached (the steps before failed: their logs)" }
Set-Disk -Number $d.Number -IsOffline $false
Check (-not (Get-Disk -Number $d.Number).IsOffline) "the $fs VHDX's disk $($d.Number) is online"
Check (Wait-Until { [IO.Directory]::Exists($i.Guid) } 30) "its volume answers by its GUID path again ($($i.Guid))"
Done
