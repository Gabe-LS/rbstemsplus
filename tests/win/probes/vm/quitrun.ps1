# One start-and-quit cycle of rekordbox in the user's session (run through winps.sh --user):
# start, wait for the main window, let the stems engine start ($settle s), close the main window
# (WM_CLOSE, nothing playing, so no dialog), and time the exit. Reports the exit code
# (0xC0000005 = the crash) and the library files before and after. $dumpat: also dump the
# process that many seconds after the close. $burn: load 5 CPU threads for that many seconds
# once the window is up.
if (-not $settle) { $settle = 90 }
$lib = "$env:APPDATA\Pioneer\rekordbox"
function dbinfo($when) {
  foreach ($n in 'master.db', 'master.db-wal', 'master.db-shm') {
    $f = Get-Item "$lib\$n" -ErrorAction SilentlyContinue
    if ($f) { "  $when $n {0} bytes, written {1:HH:mm:ss.fff}" -f $f.Length, $f.LastWriteTime } else { "  $when $n absent" }
  }
}
dbinfo 'before'
$t0 = Get-Date
$p = Start-Process 'C:\Program Files\rekordbox\rekordbox 7.2.19\rekordbox.exe' -PassThru
$h = $p.Handle # keeps the exit code readable
$t = 0
while ($t -lt 240) { $p.Refresh(); if ($p.HasExited -or $p.MainWindowTitle -eq 'rekordbox') { break }; Start-Sleep 1; $t++ }
"window after $t s (title '$($p.MainWindowTitle)')"
if ($burn) { # CPU load like the stems warm-up's (5 busy threads), to test a timing explanation
  $jobs = 1..5 | ForEach-Object { Start-Job -ScriptBlock { param($s) $e = (Get-Date).AddSeconds($s); while ((Get-Date) -lt $e) { } } -ArgumentList $burn }
  "burning 5 threads for $burn s"
}
Start-Sleep $settle
$p.Refresh()
if ($p.HasExited) { "rekordbox exited by itself, code 0x{0:X8}" -f $p.ExitCode; dbinfo 'after'; return }
$tc = Get-Date
"close sent at {0:HH:mm:ss.fff}: {1}" -f $tc, $p.CloseMainWindow()
if ($dumpat) { # a dump of the shutdown in progress, to the share (\\Mac\win\inv\)
  Start-Sleep $dumpat
  $f = "$env:LOCALAPPDATA\rbsp-shutdown.dmp"
  & \\Mac\win\p0\bin\dumpnow.exe $p.Id $f
  New-Item -ItemType Directory -Force \\Mac\win\inv | Out-Null
  Move-Item $f "\\Mac\win\inv\shutdown-$($p.Id).dmp" -Force
  "shutdown dump at +$dumpat s: shutdown-$($p.Id).dmp"
}
if (-not $p.WaitForExit(240000)) { "NOT exited 240 s after close" } else {
  "exited {0:N1} s after close, code 0x{1:X8}" -f ((Get-Date) - $tc).TotalSeconds, $p.ExitCode
}
Get-Process rekordboxAgent -ErrorAction SilentlyContinue | ForEach-Object { "rekordboxAgent still running: pid $($_.Id)" }
dbinfo 'after'
