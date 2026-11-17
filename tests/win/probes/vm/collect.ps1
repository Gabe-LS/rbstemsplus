# After a quitrun (run as SYSTEM): crash events of the last $since minutes, new crash dumps and
# the probe log copied to \\Mac\win\inv\$label\, and a copy of the library (master.db and its
# WAL) for the Mac's integrity check. $label is written by the caller.
if (-not $since) { $since = 10 }
$out = "\\Mac\win\inv\$label"
New-Item -ItemType Directory -Force $out | Out-Null
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1002; StartTime = (Get-Date).AddMinutes(-$since) } -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match 'rekordbox' } | ForEach-Object { "{0:HH:mm:ss} id {1}: {2}" -f $_.TimeCreated, $_.Id, ((($_.Message -replace '\s+', ' ')) -replace '^(.{0,260}).*$', '$1') }
Get-ChildItem C:\rbtest\dumps -Filter *.dmp -ErrorAction SilentlyContinue | ForEach-Object {
  "dump $($_.Name)"; Move-Item $_.FullName $out -Force
}
foreach ($l in 'C:\rbtest\stub.log', 'C:\rbtest\ortprobe.log') { if (Test-Path $l) { Copy-Item $l $out -Force } }
$u = (Get-CimInstance Win32_ComputerSystem).UserName.Split('\')[1]
$lib = "C:\Users\$u\AppData\Roaming\Pioneer\rekordbox"
New-Item -ItemType Directory -Force "$out\db" | Out-Null
Get-ChildItem $lib -Filter 'master.db*' | ForEach-Object { Copy-Item $_.FullName "$out\db\" -Force; "library copy: $($_.Name) $($_.Length)" }
