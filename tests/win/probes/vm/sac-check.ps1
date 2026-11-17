# Smart App Control test, the system side (run as SYSTEM; cmdlets only): SAC state and policies,
# the worker stand-in run elevated, the bridge's log, Code Integrity and AppLocker-style events of
# the last $since minutes, and the notifications Windows queued.
if (-not $since) { $since = 10 }
"SAC state: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy').VerifiedAndReputablePolicyState
"language mode (SYSTEM): " + $ExecutionContext.SessionState.LanguageMode
try {
  $j = (& CiTool.exe --list-policies --json | Out-String) | ConvertFrom-Json
  $j.Policies | Where-Object { $_.FriendlyName -like 'VerifiedAndReputable*' } | ForEach-Object { "policy {0}: enforced {1}" -f $_.FriendlyName, $_.IsEnforced }
} catch { "CiTool: $_" }
$w = 'C:\Program Files\RB Stems Plus\bin\0123456789ab\RB Stems Plus.exe'
try {
  $p = Start-Process -FilePath $w -ArgumentList 'C:\rbtest\worker-report.txt' -PassThru -Wait -ErrorAction Stop
  "worker (unsigned, elevated): exit 0x{0:X8}, report: {1}" -f $p.ExitCode, ((Get-Content C:\rbtest\worker-report.txt -ErrorAction SilentlyContinue | Select-Object -First 1) -join '')
} catch { "worker (unsigned, elevated): REFUSED: $($_.Exception.Message)" }
Remove-Item C:\rbtest\worker-report.txt -ErrorAction SilentlyContinue
"== bridge log (C:\rbtest\ortprobe.log)"
if (Test-Path C:\rbtest\ortprobe.log) { Get-Content C:\rbtest\ortprobe.log | Select-Object -First 2 | ForEach-Object { $_.Substring(0, [Math]::Min(160, $_.Length)) } } else { "(no log: our onnxruntime.dll never ran)" }
"== Code Integrity events, last $since min"
Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; StartTime = (Get-Date).AddMinutes(-$since) } -ErrorAction SilentlyContinue |
  Where-Object { $_.Id -in 3033, 3034, 3076, 3077, 3089, 3114, 3118 } | Select-Object -First 30 |
  ForEach-Object { "{0:HH:mm:ss} id {1}: {2}" -f $_.TimeCreated, $_.Id, ((($_.Message -replace '\s+', ' ')) -replace '^(.{0,300}).*$', '$1') }
"== AppLocker / SmartScreen / Defender events, last $since min"
foreach ($log in 'Microsoft-Windows-AppLocker/MSI and Script', 'Microsoft-Windows-AppLocker/EXE and DLL', 'Microsoft-Windows-SmartScreen/Debug', 'Microsoft-Windows-Windows Defender/Operational') {
  Get-WinEvent -FilterHashtable @{ LogName = $log; StartTime = (Get-Date).AddMinutes(-$since) } -ErrorAction SilentlyContinue | Select-Object -First 8 |
    ForEach-Object { "{0} {1:HH:mm:ss} id {2}: {3}" -f ($log -replace '^Microsoft-Windows-', ''), $_.TimeCreated, $_.Id, ((($_.Message -replace '\s+', ' ')) -replace '^(.{0,200}).*$', '$1') }
}
"== Application errors, last $since min"
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1002; StartTime = (Get-Date).AddMinutes(-$since) } -ErrorAction SilentlyContinue |
  ForEach-Object { "{0:HH:mm:ss} id {1}: {2}" -f $_.TimeCreated, $_.Id, ((($_.Message -replace '\s+', ' ')) -replace '^(.{0,200}).*$', '$1') }
