# As SYSTEM: a 64 MB VHDX of the step's file system (ntfs-attach, exfat-attach), made from scratch,
# attached with a free drive letter; its letter and volume GUID path saved for the user steps
$fs = Get-VolFs
$vhd = Get-VolVhd $fs
New-Item -ItemType Directory -Force $VolDir | Out-Null
if (Test-Path -LiteralPath $vhd) { [void](DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk"); Remove-Item -LiteralPath $vhd -Force }
$used = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object Name)
$letter = @('S', 'T', 'U', 'V', 'W', 'R', 'Q') | Where-Object { $used -notcontains $_ } | Select-Object -First 1
if (-not $letter) { Skip 'no free drive letter for the VHDX' }
$rc = DiskPart "create vdisk file=`"$vhd`" maximum=64 type=expandable`nselect vdisk file=`"$vhd`"`nattach vdisk`ncreate partition primary`nformat fs=$fs quick label=RB$($fs.ToUpper())`nassign letter=$letter"
Check ($rc -eq 0) "diskpart made and attached the $fs VHDX (exit $rc)"
Check (Wait-Until { Test-Path "${letter}:\" } 30) "it is ${letter}:"
$v = Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue
Check ($v -and $v.FileSystem -eq $(if ($fs -eq 'ntfs') { 'NTFS' } else { 'exFAT' })) "its file system is $fs ($(if ($v) { $v.FileSystem }))"
$guid = Get-VolGuid $fs
Check ($guid -match '^\\\\\?\\Volume\{[0-9a-fA-F-]{36}\}\\$') "its volume GUID path ($guid)"
Save-VolInfo $fs @{ Letter = $letter; Guid = $guid }
Done
