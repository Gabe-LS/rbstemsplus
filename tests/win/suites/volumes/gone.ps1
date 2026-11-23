# As the user: the volume detached: "not-connected" from location_check (its GUID path) and from
# location_gone (its GUID root, any=0), and through the mount folder left with nothing mounted
# (what GetDriveType calls DRIVE_NO_ROOT_DIR: no volume holds the path, so not-connected, as the
# VM rounds of 2026-11-17 found on both file systems and architectures)
$fs = Get-VolFs
$i = Read-VolInfo $fs
Test-Loc "$fs, detached, its GUID path" @('--check', "$($i.Guid)$CacheName", '0') 'not-connected'
Test-Loc "$fs, detached, location_gone" @('--gone', $i.Guid, '0') 'not-connected'
Test-Loc "$fs, detached, through its empty mount folder (DRIVE_NO_ROOT_DIR)" @('--check', "$(Get-VolMountFolder $fs)\$CacheName", '0') 'not-connected'
Done
