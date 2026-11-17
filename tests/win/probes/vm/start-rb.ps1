# Starts rekordbox in the user's session (run through winps.sh --user), waits, and saves a
# screenshot to \\Mac\win\shot.png. $wait (seconds) is written by the caller on the first line.
if (-not $wait) { $wait = 45 }
Start-Process 'C:\Program Files\rekordbox\rekordbox 7.2.19\rekordbox.exe'
Start-Sleep $wait
$p = Get-Process rekordbox -ErrorAction SilentlyContinue
if ($p) { "rekordbox running, pid $($p.Id), responding $($p.Responding), title '$($p.MainWindowTitle)'" } else { "rekordbox NOT running" }
. \\Mac\win\p0\shot.ps1
