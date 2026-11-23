# As SYSTEM: stick C unplugged, stick A plugged in again on another letter
$st = Read-Sticks
$rc = LocDiskPart "select vdisk file=`"$(Get-StickVhd 'C')`"`ndetach vdisk"
Check ($rc -eq 0) "stick C detached (exit $rc)"
$l = Get-FreeLetter @('W', 'V', 'U', 'M', 'L')
$rc = LocDiskPart "select vdisk file=`"$(Get-StickVhd 'A')`"`nattach vdisk"
Check ($rc -eq 0) "stick A attached again (exit $rc)"
Check (Wait-Until { Get-StickGuid 'A' } 30) "its volume is back"
$v = Get-DiskImage -ImagePath (Get-StickVhd 'A') | Get-Disk | Get-Partition | Where-Object { $_.Type -ne 'Reserved' } | Select-Object -First 1
$old = [string]$v.DriveLetter
if ($old) { $rc = LocDiskPart "select volume $old`nremove letter=$old`nassign letter=$l" } else { $v | Set-Partition -NewDriveLetter $l; $rc = 0 }
Check ($rc -eq 0) "it came back as ${old}: and now has ${l}: (exit $rc)"
Check (Wait-Until { Test-Path "${l}:\" } 30) "stick A is ${l}: now (was $($st.A):)"
Check ((Get-StickGuid 'A') -ieq $st.GA) "the same GUID path ($(Get-StickGuid 'A'))"
$h = @{}; foreach ($p in $st.PSObject.Properties) { $h[$p.Name] = $p.Value }
$h.A2 = $l
Save-Sticks $h
Done
