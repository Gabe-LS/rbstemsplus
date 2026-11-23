# As SYSTEM: stick A's letter removed, the volume mounted in C:\mnt\n3a instead (no letter at all)
$st = Read-Sticks
$mf = 'C:\mnt\n3a'
New-Item -ItemType Directory -Force $mf | Out-Null
$rc = LocDiskPart "select volume $($st.A2)`nremove letter=$($st.A2)`nassign mount=`"$mf`""
Check ($rc -eq 0) "stick A moved from $($st.A2): to $mf (exit $rc)"
Check (-not (Test-Path "$($st.A2):\")) "no $($st.A2): any more"
Check (Wait-Until { Test-Path -LiteralPath "$mf\Music-x64" } 30) "its folders through $mf"
Done
