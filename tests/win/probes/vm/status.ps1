# rekordbox's state, the probe logs, Defender's view of our file (run as SYSTEM).
$rb = 'C:\Program Files\rekordbox\rekordbox 7.2.19'
$p = Get-Process rekordbox -ErrorAction SilentlyContinue
if ($p) { "rekordbox running, pid $($p.Id), responding $($p.Responding), title '$($p.MainWindowTitle)', cpu $([int]$p.CPU) s, ws $([int]($p.WorkingSet64/1MB)) MB" } else { "rekordbox NOT running" }
Get-Process | Where-Object { $_.Path -like "$rb*" -or $_.Name -match 'rekordbox|Upmgr|edb_streamd|rbHttp|rbinit' } | ForEach-Object { "process {0} {1} {2}" -f $_.Id, $_.Name, $_.Path }
"live onnxruntime.dll: " + $(if (Test-Path "$rb\onnxruntime.dll") { (Get-FileHash "$rb\onnxruntime.dll").Hash } else { "MISSING" })
"== Defender detections"
Get-MpThreatDetection | Select-Object -First 10 ThreatID, InitialDetectionTime, ProcessName, Resources, ActionSuccess | Format-List
Get-MpThreat | Select-Object -First 10 ThreatName, SeverityID, IsActive, Resources | Format-List
foreach ($log in 'C:\rbtest\ortprobe.log', 'C:\rbtest\stub.log') {
  if (Test-Path $log) { "== $log"; Get-Content $log | Select-Object -First $lines }
}
