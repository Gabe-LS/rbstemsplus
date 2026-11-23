# Round 3: the complete uninstall from the menu with Microsoft\CLR_v4.0 a junction (to a folder of
# the user's holding the real CLR_v4.0's contents, the app's UsageLogs entry among them). The
# uninstall opens CLR_v4.0 from LocalAppData's handle without following a link, so it is left:
# the junction stays and the entry behind it too. Everything else gone as complete.ps1 checks.
# A plain entry planted in CLR_v4.0_32's UsageLogs, next to the junction, is deleted (review WA9).
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
Stop-App
$clr = Join-Path $l.Local 'Microsoft\CLR_v4.0'
Set-UsageLogEntry (Join-Path $clr 'UsageLogs\RB Stems Plus.exe.log') 1   # (unlib: planted when the CLR wrote none)
$real = Join-Path $CaseDir 'clr-real'
Remove-Item -LiteralPath $real -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $CaseDir | Out-Null
Move-Item -LiteralPath $clr -Destination $real
New-Item -ItemType Junction -Path $clr -Target $real | Out-Null
$entry = Join-Path $real 'UsageLogs\RB Stems Plus.exe.log'
Check ((Test-Reparse $clr) -and (Test-Path -LiteralPath $entry)) "CLR_v4.0 is a junction ($clr -> $real), the entry behind it"
$plain = Join-Path $l.Local 'Microsoft\CLR_v4.0_32\UsageLogs\RB Stems Plus.exe.log'
New-Item -ItemType Directory -Force (Split-Path $plain) | Out-Null
Set-Content -LiteralPath $plain -Value 'planted' -Encoding ASCII
Check (Test-Path -LiteralPath $plain -PathType Leaf) "a plain UsageLogs entry planted next to the junction ($plain)"
Start-AppQuiet
if (-not $script:app) { Done }
$app = $script:app
Invoke-MenuUninstall $app
Complete-Uninstall $app
Test-UninstallGone
Check (Test-Reparse $clr) 'the CLR_v4.0 junction is left'
Check (Test-Path -LiteralPath $entry) 'and the UsageLogs entry behind it'
Check (Wait-Until { -not (Test-Path -LiteralPath $plain) } 30) "the plain one in CLR_v4.0_32 is deleted ($plain)"
Done
