# As the user (plan row 7's stand-in for an empty card reader, which stays by hand): the VM's
# CD-ROM drive with no disc (Parallels' cdrom0 disconnected): DRIVE_CDROM, not ready
# (ERROR_NOT_READY on its root); a location on it is "not-connected" from location_check, and its
# root from location_gone. An empty card reader is DRIVE_REMOVABLE, the case this stands in for.
$cd = @(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType = 5' -ErrorAction SilentlyContinue)
if (-not $cd.Count) { Skip 'no CD-ROM drive with a letter in this VM (a Parallels CD-ROM, disconnected, is the case)' }
$empty = @($cd | Where-Object { -not (New-Object IO.DriveInfo "$($_.DeviceID)\").IsReady })
foreach ($c in $cd) { "CD-ROM $($c.DeviceID): ready $((New-Object IO.DriveInfo "$($c.DeviceID)\").IsReady)" }
if (-not $empty.Count) { Skip 'every CD-ROM drive holds a disc (prlctl set VM --device-disconnect cdrom0 empties it)' }
$l = $empty[0].DeviceID
$t = (New-Object IO.DriveInfo "$l\").DriveType
Check ($t -eq 'CDRom') "$l is a CD-ROM drive ($t) with no medium"
Test-Loc "a location on the empty CD-ROM drive $l" @('--check', "$l\$CacheName", '0') 'not-connected'
Test-Loc "location_gone on the empty CD-ROM drive's root $l\" @('--gone', "$l\", '0') 'not-connected'
Done
