# Round 2: the complete uninstall as Settings > Apps starts it (the stub with --uninstall) after a
# quiet start (welcomed already), with cmd.exe blocked and the UsageLogs entry a link
# (block-cmd.ps1). Then: everything gone as uninstall/complete.ps1 checks; the link left (a plain
# file only is deleted); cmd couldn't start, so the after-exit's temp folder stays and one RunOnce
# value "RB Stems Plus cleanup <16 hex>" (UN13) names it with rd /s /q, read back whole. That it
# goes at the next logon stays by hand (by-hand.md). A plain entry planted in CLR_v4.0_32's
# UsageLogs, next to the link, is deleted (review WA9).
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$log = Join-Path $l.Local 'Microsoft\CLR_v4.0\UsageLogs\RB Stems Plus.exe.log'
if (-not (Test-Reparse $log)) { Skip "the UsageLogs entry isn't a link (block-cmd failed: its log)" }
Stop-App
$plain = Join-Path $l.Local 'Microsoft\CLR_v4.0_32\UsageLogs\RB Stems Plus.exe.log'
New-Item -ItemType Directory -Force (Split-Path $plain) | Out-Null
Set-Content -LiteralPath $plain -Value 'planted' -Encoding ASCII
Check (Test-Path -LiteralPath $plain -PathType Leaf) "a plain UsageLogs entry planted next to the link ($plain)"
Start-AppQuiet @('--uninstall')
if (-not $script:app) { Done }
$app = $script:app
Complete-Uninstall $app
Test-UninstallGone
Check (Test-Reparse $log) 'the UsageLogs entry that is a link is left'
Check (Wait-Until { -not (Test-Path -LiteralPath $plain) } 30) "the plain one in CLR_v4.0_32 is deleted ($plain)"
$tmp = Join-Path $l.Local 'Temp'
$copies = @()
Check (Wait-Until { $script:copies = Get-AfterExitCopies $tmp; $script:copies.Count -ge 1 } 30) "the after-exit temp folder stays ($($copies -join ', '))"
$cleanup = @()
Check (Wait-Until { $script:cleanup = @(Get-CleanupRunOnce); $script:cleanup.Count -ge 1 } 60) 'a RunOnce cleanup value written (cmd.exe could not start)'
foreach ($c in $cleanup) { "RunOnce: $c" }
# the whole value, as ax_runonce_data writes it (case ignored): "<cmd>" /d /q /s /c "rd /s /q "<dir>""
# <dir> as the app's %TEMP% names it: the long path, or its 8.3 form (a user name over 8
# characters gives a TEMP of C:\Users\GABRIE~1\...: the value then names the same folder that way)
$cmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
$short = try { (New-Object -ComObject Scripting.FileSystemObject).GetFolder($tmp).ShortPath } catch { $tmp }
$named = @($copies | Where-Object { $n = $_; $wants = @($tmp, $short) | Select-Object -Unique | ForEach-Object { "RB Stems Plus cleanup $($n.Substring(15)) = `"$cmd`" /d /q /s /c `"rd /s /q `"$(Join-Path $_ $n)`"`"" }; @($cleanup | Where-Object { $c = $_; @($wants | Where-Object { $c -ieq $_ }).Count -eq 1 }).Count -eq 1 })
Check ($copies.Count -ge 1 -and $named.Count -eq $copies.Count) "one per temp folder, named by its 16 hex, the whole value `"$cmd`" /d /q /s /c `"rd /s /q `"$tmp\<folder>`"`""
Done
