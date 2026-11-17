# As the user: the location on the read-only volume, by its GUID path: "read-only"
$fs = Get-VolFs
$i = Read-VolInfo $fs
Test-Loc "$fs, read-only" @('--check', "$($i.Guid)$CacheName", '0') 'read-only'
Done
