# The user quits rekordbox and starts it again right away, while the old process still lingers
# (run through winps.sh --user). Reports both processes and whether a window comes up.
$exe = 'C:\Program Files\rekordbox\rekordbox 7.2.19\rekordbox.exe'
$p = Start-Process $exe -PassThru
$h = $p.Handle
$t = 0
while ($t -lt 240) { $p.Refresh(); if ($p.MainWindowTitle -eq 'rekordbox') { break }; Start-Sleep 1; $t++ }
"first window after $t s"
Start-Sleep 60
$tc = Get-Date
"close: " + $p.CloseMainWindow()
Start-Sleep 10
$q = Start-Process $exe -PassThru
$h2 = $q.Handle
"second start at +10 s: pid $($q.Id) (first pid $($p.Id), exited: $($p.HasExited))"
for ($i = 0; $i -lt 180; $i += 5) {
  Start-Sleep 5
  $p.Refresh(); $q.Refresh()
  $s = "+{0,3:N0} s: first {1}, second {2} title '{3}'" -f ((Get-Date) - $tc).TotalSeconds,
    $(if ($p.HasExited) { "exited 0x{0:X8}" -f $p.ExitCode } else { 'running' }),
    $(if ($q.HasExited) { "exited 0x{0:X8}" -f $q.ExitCode } else { 'running' }), $q.MainWindowTitle
  $s
  if ($p.HasExited -and ($q.HasExited -or $q.MainWindowTitle -eq 'rekordbox')) { break }
}
. \\Mac\win\p0\shot.ps1
