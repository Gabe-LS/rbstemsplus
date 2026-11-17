# As the user: the volume detached: "not-connected" from location_check (its GUID path) and from
# location_gone (its GUID root, any=0). The mount folder left with nothing mounted (what
# GetDriveType calls DRIVE_NO_ROOT_DIR) is said only: its outcome corrects plat_win.h:64's note
# (by-hand.md)
$fs = Get-VolFs
$i = Read-VolInfo $fs
Test-Loc "$fs, detached, its GUID path" @('--check', "$($i.Guid)$CacheName", '0') 'not-connected'
Test-Loc "$fs, detached, location_gone" @('--gone', $i.Guid, '0') 'not-connected'
Test-Loc "$fs, detached, through its empty mount folder (DRIVE_NO_ROOT_DIR)" @('--check', "$(Get-VolMountFolder $fs)\$CacheName", '0') ''
Done
