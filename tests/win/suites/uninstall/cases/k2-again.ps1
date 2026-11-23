# Round 4's end (K2, as K1's end): with the other session's watcher ended (k2-stop.ps1), the
# uninstall from Settings (--uninstall) runs again and completes: everything gone.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'nothing kept to uninstall again (k2-uninstall removed the app: its log)' }
Stop-App
Start-AppQuiet @('--uninstall')
if (-not $script:app) { Done }
Complete-Uninstall $script:app
Test-UninstallGone
Done
