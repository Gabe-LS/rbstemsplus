# As the user: the disk offline: "not-connected" from location_check (by its GUID path) and from
# location_gone (its GUID root, any=0); through its mount folder, said only
$fs = Get-VolFs
$i = Read-VolInfo $fs
Test-Loc "$fs, its disk offline, its GUID path" @('--check', "$($i.Guid)$CacheName", '0') 'not-connected'
Test-Loc "$fs, its disk offline, location_gone" @('--gone', $i.Guid, '0') 'not-connected'
Test-Loc "$fs, its disk offline, through its mount folder" @('--check', "$(Get-VolMountFolder $fs)\$CacheName", '0') ''
Done
