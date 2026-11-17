# rekordbox's open TCP connections (remote end, state), sampled every 5 s for $dur seconds while
# a quitrun runs (run as SYSTEM): which servers stay connected through the quit. Prints a line
# only when the set changes.
if (-not $dur) { $dur = 300 }
$last = ''
for ($t = 0; $t -lt $dur; $t += 5) {
  $p = Get-Process rekordbox -ErrorAction SilentlyContinue
  $now = if ($p) {
    (Get-NetTCPConnection -OwningProcess $p.Id -ErrorAction SilentlyContinue | Where-Object { $_.RemoteAddress -notin '0.0.0.0', '::', '127.0.0.1', '::1' } |
      Sort-Object RemoteAddress, RemotePort | ForEach-Object { "{0}:{1} {2}" -f $_.RemoteAddress, $_.RemotePort, $_.State }) -join '; '
  } else { 'rekordbox not running' }
  if ($now -ne $last) { "{0:HH:mm:ss} {1}" -f (Get-Date), $(if ($now) { $now } else { '(no connections)' }); $last = $now }
  if (-not $p -and $t -gt 60) { break }
  Start-Sleep 5
}
