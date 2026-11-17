# As the user: the volume with no drive letter, mounted at C:\mnt\x-<fs>\: refused through the
# mount folder ("folder": its mount point is a reparse point the walk won't cross), "ok" through
# its GUID path (what the app stores)
$fs = Get-VolFs
$i = Read-VolInfo $fs
$mf = Get-VolMountFolder $fs
Test-Loc "$fs, through its mount folder" @('--check', "$mf\$CacheName", '0') 'folder'
Test-Loc "$fs, no drive letter, its GUID path" @('--check', "$($i.Guid)$CacheName", '0') 'ok'
Done
