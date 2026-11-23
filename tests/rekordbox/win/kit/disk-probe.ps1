# Read the requested drive on the verified PC. An empty reader can have a letter without
# a partition, so Windows can still report Removable and not ready when bus type is unknown.
Trace-KitPhase 'disk-probe: before drive lookup'
$root = "${Drive}:\"
$volume = @([IO.DriveInfo]::GetDrives() | Where-Object { $_.Name -ieq $root })
if ($volume.Count -ne 1) { "drive $root is unavailable"; exit 1 }
$v = $volume[0]
$disk = @(Get-Partition -DriveLetter $Drive -ErrorAction SilentlyContinue | Get-Disk -ErrorAction SilentlyContinue)
$bus = if ($disk.Count -eq 1) { [string]$disk[0].BusType } else { 'unknown' }
if ($v.DriveType -notin 'Fixed', 'Removable' -or
    ($bus -ne 'USB' -and ($v.DriveType -ne 'Removable' -or $bus -ne 'unknown'))) {
    "drive $root is not a USB disk or removable reader (type $($v.DriveType), bus $bus)"
    exit 1
}
$ready = $v.IsReady
Trace-KitPhase 'disk-probe: after drive lookup'
$fs = if ($ready) { $v.DriveFormat } else { 'none' }
"DISK ${Drive}: type $($v.DriveType) bus $bus ready $ready fs $fs"
# its disk's key, base64 of "SERIAL|FriendlyName" (not the UniqueId: USB bridges share them, the
# owner's T5 and backup SSD both answer 5000000000000001), what rbstemsplus.pcProtectedDisks holds for
# a disk the kit must never change or write to (read only here)
if ($disk.Count -eq 1 -and ([string]$disk[0].SerialNumber).Trim()) { "ID $([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(([string]$disk[0].SerialNumber).Trim() + '|' + $disk[0].FriendlyName))) NAME $($disk[0].FriendlyName)" }
exit 0
