# Opens a URI or program in the user's session (run through winps.sh --user), waits, and saves a
# screenshot. $target is written by the caller, e.g. 'windowsdefender://appbrowser'.
Get-Process rekordbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Process $target
Start-Sleep 8
. \\Mac\win\p0\shot.ps1
