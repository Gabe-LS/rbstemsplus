# As the user, with any=0 (T3c case 3): the location made at the VHDX's root through its drive
# letter, then "ok" through its drive path, its GUID path and the GUID path in upper case (each
# pinned to that volume, in each architecture's run); --gone on its mounted GUID root: "folder" (location_why(ENOENT)); on NTFS, a
# junction on the VHDX (<Letter>:\jx, to C:\rbtest\vol\jt on the system volume) on the way:
# "folder" (the volume isn't the system's, so the walk reaches the reparse point). exFAT holds no
# reparse points: an INFO line there.
$fs = Get-VolFs
$i = Read-VolInfo $fs
$drive = "$($i.Letter):\$CacheName"
$gdir = "$($i.Guid)$CacheName"
Test-Loc 'made through its drive path' @('--make', $drive) 'made'
Test-Loc "$fs, its drive path" @('--check', $drive, '0') 'ok' $i.Guid
Test-Loc "$fs, its GUID path" @('--check', $gdir, '0') 'ok' $i.Guid
Test-Loc "$fs, its GUID path in upper case" @('--check', (ConvertTo-UpperGuid $gdir), '0') 'ok' $i.Guid
Test-Loc "$fs, --gone on its mounted GUID root" @('--gone', $i.Guid, '0') 'folder'
if ($fs -eq 'exfat') { 'INFO exFAT holds no junction: the junction case is NTFS only' }
else {
    $jt = Join-Path $VolDir 'jt'
    New-Item -ItemType Directory -Force (Join-Path $jt $CacheName) | Out-Null
    $j = "$($i.Letter):\jx"
    if (Test-Path -LiteralPath $j) { [IO.Directory]::Delete($j) }   # the junction itself, never its target
    New-Item -ItemType Junction -Path $j -Target $jt | Out-Null
    Check ([bool]((Get-Item -LiteralPath $j -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) "a junction $j -> $jt"
    Test-Loc "$fs, a junction on it on the way" @('--check', "$j\$CacheName", '0') 'folder'
}
Done
