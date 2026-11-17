# Smart App Control test, the user's side (run through winps.sh --user-signed; cmdlets only, so
# it works in ConstrainedLanguage): the language mode, our unsigned app and core DLL stand-ins,
# and rekordbox with our unsigned onnxruntime.dll.
$u = $env:USERNAME
$app = "$env:LOCALAPPDATA\Programs\RB Stems Plus\9.9.9"
Get-Process rekordbox -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item C:\rbtest\ortprobe.log -ErrorAction SilentlyContinue
"language mode: " + $ExecutionContext.SessionState.LanguageMode
"SAC state: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy').VerifiedAndReputablePolicyState
try { Add-Type 'public class SacProbe { public static int One() { return 1; } }' -ErrorAction Stop; "Add-Type: allowed" } catch { "Add-Type: refused ($($_.Exception.GetType().Name))" }
function run($label, $exe, $argList) {
  try {
    $o = @{ FilePath = $exe; PassThru = $true; Wait = $true; WindowStyle = 'Hidden'; RedirectStandardOutput = "$env:TEMP\sac-$($label.Length).txt"; ErrorAction = 'Stop' }
    if ($argList) { $o.ArgumentList = $argList }
    $p = Start-Process @o
    "{0}: exit 0x{1:X8}, output: {2}" -f $label, $p.ExitCode, ((Get-Content "$env:TEMP\sac-$($label.Length).txt" -ErrorAction SilentlyContinue | Select-Object -First 2) -join ' / ')
  } catch { "{0}: REFUSED: {1}" -f $label, $_.Exception.Message }
}
run 'app exe (unsigned)' "$app\RB Stems Plus.exe" @()
run 'core DLL host (unsigned exe loading 2 unsigned DLLs)' "$app\corehost.exe" @("`"$app\stub\onnxruntime.dll`"", "`"$app\rbsp-core.dll`"")
Start-Sleep 2
try { . \\Mac\win\p0\shot.ps1 } catch { "screenshot failed: $($_.Exception.Message)" } # a block's notification
"== rekordbox"
$t0 = Get-Date
try { $rb = Start-Process 'C:\Program Files\rekordbox\rekordbox 7.2.19\rekordbox.exe' -PassThru -ErrorAction Stop; $h = $rb.Handle } catch { "rekordbox start REFUSED: $($_.Exception.Message)" }
if ($rb) {
  $t = 0
  while ($t -lt 120) { $rb.Refresh(); if ($rb.HasExited -or $rb.MainWindowTitle -eq 'rekordbox') { break }; Start-Sleep 1; $t++ }
  if ($rb.HasExited) { "rekordbox EXITED after $t s, code 0x{0:X8}" -f $rb.ExitCode }
  else {
    "rekordbox window after $t s (title '$($rb.MainWindowTitle)')"
    $m = $rb.Modules | Where-Object ModuleName -eq 'onnxruntime.dll'
    "onnxruntime.dll in rekordbox: " + $(if ($m) { ($m | ForEach-Object FileName) -join ', ' } else { 'NOT loaded' })
  }
}
