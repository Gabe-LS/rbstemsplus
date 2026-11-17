# Waits up to 3 minutes for rekordbox to exit, then reports crash events since $since (minutes),
# crash dumps, and the tail of the stub's log (run as SYSTEM).
if (-not $since) { $since = 10 }
$t = 0
while ((Get-Process rekordbox -ErrorAction SilentlyContinue) -and $t -lt 180) { Start-Sleep 1; $t++ }
if (Get-Process rekordbox -ErrorAction SilentlyContinue) { "rekordbox STILL running after $t s" } else { "rekordbox gone after $t s" }
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1002; StartTime = (Get-Date).AddMinutes(-$since) } -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match 'rekordbox' } | ForEach-Object { "{0} id {1}: {2}" -f $_.TimeCreated, $_.Id, ((($_.Message -replace '\s+', ' ')) -replace '^(.{0,420}).*$', '$1') }
Get-ChildItem C:\rbtest\dumps -ErrorAction SilentlyContinue | ForEach-Object {
  "dump $($_.Name) $($_.Length)"
  New-Item -ItemType Directory -Force \\Mac\win\facts\dumps | Out-Null
  Copy-Item $_.FullName \\Mac\win\facts\dumps\ -Force
}
if (Test-Path C:\rbtest\stub.log) { "== stub.log tail"; Get-Content C:\rbtest\stub.log | Select-Object -Last 25 }
