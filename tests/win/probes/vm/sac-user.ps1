# With Smart App Control On: a never-seen unsigned exe run by the user, then rekordbox started
# with a never-seen unsigned onnxruntime.dll in place (run through winps.sh --user).
$d = "$env:LOCALAPPDATA\rbsp-sac"
New-Item -ItemType Directory -Force $d | Out-Null
Copy-Item \\Mac\win\p0\sac\cpuprobe-fresh.exe $d -Force
try { $o = & "$d\cpuprobe-fresh.exe" 2>&1; "fresh unsigned exe: exit $LASTEXITCODE, first line: " + ($o | Select-Object -First 1) } catch { "fresh unsigned exe REFUSED: $($_.Exception.Message)" }
Start-Process 'C:\Program Files\rekordbox\rekordbox 7.2.19\rekordbox.exe' -ErrorAction Continue
Start-Sleep 60
$p = Get-Process rekordbox -ErrorAction SilentlyContinue
if ($p) { "rekordbox running, pid $($p.Id), title '$($p.MainWindowTitle)'" } else { "rekordbox NOT running" }
. \\Mac\win\p0\shot.ps1
