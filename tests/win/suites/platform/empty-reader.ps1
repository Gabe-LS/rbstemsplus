# The VM should expose exactly one empty removable letter when the owner assigns the reader.
$assigned = Share "job/$($VmRun.Run)/usb-reader.assigned"
if (-not (Test-Path -LiteralPath $assigned)) {
    Skip 'no empty card reader is assigned to this VM (vm-run.conf: "usb-reader VM DEVICE-ID")'
}
"assigned empty reader: $((Get-Content -LiteralPath $assigned -TotalCount 1).Trim())"
$empty = @()
[void](Wait-Until {
    $script:empty = @([IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Removable' -and -not $_.IsReady })
    $script:empty.Count -gt 0
} 60)
Check ($empty.Count -eq 1) "one empty removable drive letter (found $($empty.Count): $($empty.Name -join ', '))"
if ($empty.Count -eq 1) {
    $root = $empty[0].Name
    Test-Loc "a location on the empty card reader $root" @('--check', "$root$CacheName", '0') 'not-connected'
    Test-Loc "location_gone on the empty card reader $root" @('--gone', $root, '0') 'not-connected'
}
Done
