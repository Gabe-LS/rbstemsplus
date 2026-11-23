# As SYSTEM (by-hand.md's app.md section 7 case, plan row 4): the USB stick vm-run.conf assigns to
# this VM (vm-run.sh's "need usb": assigned before the VM started; its id in the job folder's
# usb.assigned) seen by Windows as a USB disk with a drive letter, which goes to
# C:\rbtest\usb.letter: the wintests steps then run winc-app-wintest's external-drive case
# (--external) on it, unelevated and elevated. No stick assigned: SKIP.
$assigned = "\\Mac\win\job\$($VmRun.Run)\usb.assigned"
if (-not (Test-Path -LiteralPath $assigned)) { Skip 'no USB stick is assigned to this VM (.orchestration/vm-run.conf: "usb VM DEVICE-ID", a stick left in a Mac port)' }
"assigned: $((Get-Content -LiteralPath $assigned -TotalCount 1).Trim())"
Remove-Item -LiteralPath 'C:\rbtest\usb.letter' -Force -ErrorAction SilentlyContinue
# the USB disks with a drive letter: "N:LETTER:FriendlyName:SerialNumber" each
function Get-UsbLettered {
    foreach ($d in @(Get-Disk -ErrorAction SilentlyContinue | Where-Object { $_.BusType -eq 'USB' })) {
        foreach ($p in @($d | Get-Partition -ErrorAction SilentlyContinue | Where-Object { [string]$_.DriveLetter -match '^[A-Z]$' })) {
            [pscustomobject]@{ Number = $d.Number; Letter = [string]$p.DriveLetter; Name = $d.FriendlyName; Serial = $d.SerialNumber }
        }
    }
}
$seen = Wait-Until { @(Get-UsbLettered).Count -gt 0 } 120
Get-Disk -ErrorAction SilentlyContinue | Format-Table -AutoSize Number, FriendlyName, SerialNumber, BusType, Size, IsOffline | Out-String -Width 200
$usb = @(Get-UsbLettered)
foreach ($u in $usb) { "USB disk $($u.Number): $($u.Letter): '$($u.Name)' serial '$($u.Serial)'" }
Check $seen "Windows sees the stick as a USB disk with a drive letter$(if (-not $seen) { ': it may connect only when the VM starts, or isn''t formatted' })"
# one stick, never a guess between several (Parallels may hand the VM another USB disk)
Check ($usb.Count -le 1) "one USB disk with a letter (ambiguous: $($usb.Count) USB disks$(if ($usb.Count -gt 1) { ': ' + (($usb | ForEach-Object { "$($_.Letter): $($_.Name)" }) -join ', ') }))"
if ($usb.Count -eq 1) {
    $letter = $usb[0].Letter
    New-Item -ItemType Directory -Force 'C:\rbtest' | Out-Null
    Set-Content -LiteralPath 'C:\rbtest\usb.letter' -Value $letter -Encoding ASCII
    "C:\rbtest\usb.letter: $letter (assigned id above, this disk '$($usb[0].Name)' serial '$($usb[0].Serial)')"
}
Done
