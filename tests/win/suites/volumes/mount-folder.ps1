# As SYSTEM: the VHDX's drive letter removed and the volume mounted in a folder of the system
# volume instead (C:\mnt\x-<fs>\): no drive letter at all
$fs = Get-VolFs
$i = Read-VolInfo $fs
$mf = Get-VolMountFolder $fs
New-Item -ItemType Directory -Force $mf | Out-Null
$rc = DiskPart "select volume $($i.Letter)`nremove letter=$($i.Letter)`nassign mount=`"$mf`""
Check ($rc -eq 0) "diskpart moved the $fs volume from $($i.Letter): to $mf (exit $rc)"
Check (-not (Test-Path "$($i.Letter):\")) "no $($i.Letter): any more"
Check (Wait-Until { Test-Path -LiteralPath (Join-Path $mf $CacheName) } 30) "the location is there through $mf"
Check ((Get-VolGuid $fs) -ieq $i.Guid) "the same volume GUID path ($(Get-VolGuid $fs))"
Done
