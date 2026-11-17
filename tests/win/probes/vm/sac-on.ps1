# Turns Smart App Control On in the throwaway VM state (reverted to the snapshot afterwards) and
# refreshes the Code Integrity policy, to see what SAC On does to the unsigned files (run as SYSTEM).
# $step: 'set' (registry + refresh), 'check' (state, run the unsigned worker spike, CI events).
$k = 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy'
if ($step -eq 'set') {
  "before: " + (Get-ItemProperty $k).VerifiedAndReputablePolicyState
  Set-ItemProperty $k VerifiedAndReputablePolicyState 1 -Type DWord
  & CiTool.exe --refresh --json 2>&1 | Select-Object -First 5
  "after: " + (Get-ItemProperty $k).VerifiedAndReputablePolicyState
}
if ($step -eq 'check') {
  "state: " + (Get-ItemProperty $k).VerifiedAndReputablePolicyState
  & CiTool.exe --list-policies --json 2>&1 | Out-String | Select-String -Pattern 'VerifiedAndReputable|Smart|IsEnforced|FriendlyName' -AllMatches | ForEach-Object { $_.Matches.Value } | Group-Object | ForEach-Object { "$($_.Name) x$($_.Count)" }
  $d = 'C:\rbtest\sacspike'
  New-Item -ItemType Directory -Force $d | Out-Null
  Copy-Item C:\rbtest\p0\bin\workerspike.exe, C:\rbtest\p0\bin\cpuprobe-x64.exe $d -Force
  $r = & "$d\cpuprobe-x64.exe" 2>&1 | Select-Object -First 1
  "unsigned cpuprobe-x64.exe: $r (exit $LASTEXITCODE)"
  $p = Get-Process rekordbox -ErrorAction SilentlyContinue
  if ($p) { "rekordbox running, pid $($p.Id)" } else { "rekordbox NOT running" }
  if (Test-Path C:\rbtest\ortprobe.log) { "ortprobe.log lines: " + (Get-Content C:\rbtest\ortprobe.log).Count; Get-Content C:\rbtest\ortprobe.log | Select-Object -First 3 }
  "== Code Integrity events in the last $since minutes"
  Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; StartTime = (Get-Date).AddMinutes(-$since) } -ErrorAction SilentlyContinue |
    Where-Object { $_.Id -in 3033, 3034, 3076, 3077 } | Select-Object -First 15 |
    ForEach-Object { "{0} id {1}: {2}" -f $_.TimeCreated, $_.Id, ((($_.Message -replace '\s+', ' ')) -replace '^(.{0,330}).*$', '$1') }
  "== Application errors"
  Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1002; StartTime = (Get-Date).AddMinutes(-$since) } -ErrorAction SilentlyContinue |
    ForEach-Object { "{0} id {1}: {2}" -f $_.TimeCreated, $_.Id, ((($_.Message -replace '\s+', ' ')) -replace '^(.{0,250}).*$', '$1') }
}
