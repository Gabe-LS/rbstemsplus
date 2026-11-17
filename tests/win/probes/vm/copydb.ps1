# Copies the VM user's rekordbox library (master.db and its WAL) to \\Mac\win\facts\db\ for the
# FolderPath probe, plus the updater's file facts (run as SYSTEM, rekordbox closed).
$p = Get-Process rekordbox -ErrorAction SilentlyContinue
if ($p) { "rekordbox still running (pid $($p.Id))" } else { "rekordbox not running" }
"== crash/hang events for rekordbox in the last hour"
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001, 1002; StartTime = (Get-Date).AddHours(-1) } -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match 'rekordbox' } | ForEach-Object { "{0} id {1}: {2}" -f $_.TimeCreated, $_.Id, ((($_.Message -replace '\s+', ' ')) -replace '^(.{0,300}).*$', '$1') }
$u = (Get-CimInstance Win32_ComputerSystem).UserName.Split('\')[1]
$src = "C:\Users\$u\AppData\Roaming\Pioneer\rekordbox"
New-Item -ItemType Directory -Force \\Mac\win\facts\db | Out-Null
Get-ChildItem $src -Filter 'master.db*' | ForEach-Object { Copy-Item $_.FullName \\Mac\win\facts\db\ -Force; "copied $($_.Name) $($_.Length)" }
"== the updater"
$rb = 'C:\Program Files\rekordbox\rekordbox 7.2.19'
$up = Get-Item "$rb\Upmgr rekordbox.exe"
"Upmgr: $($up.VersionInfo.FileDescription) / $($up.VersionInfo.CompanyName) / $($up.VersionInfo.ProductName) $($up.VersionInfo.ProductVersion)"
"signature: " + (Get-AuthenticodeSignature $up.FullName).SignerCertificate.Subject
Copy-Item $up.FullName '\\Mac\win\facts\Upmgr.exe' -Force
Copy-Item "$rb\Uninstall rekordbox.exe" '\\Mac\win\facts\Uninstall.exe' -Force
Get-ChildItem "C:\Users\$u\AppData\Roaming\Pioneer" -Directory | ForEach-Object { "roaming Pioneer\" + $_.Name }
Get-ChildItem "C:\Users\$u\AppData\Local" -Directory | Where-Object Name -match 'Pioneer|AlphaTheta|rekordbox' | ForEach-Object { "local " + $_.Name }
Get-ChildItem 'C:\ProgramData' -Directory | Where-Object Name -match 'Pioneer|AlphaTheta|rekordbox' | ForEach-Object { "programdata " + $_.Name; Get-ChildItem $_.FullName | ForEach-Object { "  " + $_.Name } }
