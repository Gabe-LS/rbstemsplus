# As SYSTEM: stick A's label renamed while it is plugged in
$st = Read-Sticks
Set-Volume -DriveLetter $st.A -NewFileSystemLabel 'RBRENAMED'
Check ((Get-Volume -DriveLetter $st.A).FileSystemLabel -eq 'RBRENAMED') "stick A is RBRENAMED"
Set-Volume -DriveLetter $st.D -NewFileSystemLabel 'DLAB'   # the unlabelled NTFS stick named: does plat_volume_name answer then?
Check ((Get-Volume -DriveLetter $st.D).FileSystemLabel -eq 'DLAB') "stick D is DLAB"
Done
