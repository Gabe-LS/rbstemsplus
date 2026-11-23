# As SYSTEM: sticks A and B unplugged (detached); A's letter stays free until stick-c.ps1 (review
# N3R5: the letter-form config is measured while its letter is free first)
$null = Read-Sticks
foreach ($s in 'A', 'B') {
    $rc = LocDiskPart "select vdisk file=`"$(Get-StickVhd $s)`"`ndetach vdisk"
    Require-Stick ($rc -eq 0) "stick $s detached (exit $rc)"
    Require-Stick (Wait-Stick { -not (Get-StickGuid $s) }) "stick $s's volume is gone"
}
Done
