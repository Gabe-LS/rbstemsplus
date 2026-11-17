# Defender and Smart App Control on our unsigned replacement onnxruntime.dll (run as SYSTEM, with
# rekordbox running on the probe). Real-time protection stays on throughout.
$rb = 'C:\Program Files\rekordbox\rekordbox 7.2.19'
"realtime: " + (Get-MpComputerStatus).RealTimeProtectionEnabled
"== on-demand scans of the unsigned DLLs"
foreach ($f in "$rb\onnxruntime.dll", 'C:\rbtest\p0\stub\onnxruntime.dll', 'C:\rbtest\p0\ort\onnxruntime.dll', 'C:\rbtest\p0\bin\workerspike.exe') {
  Start-MpScan -ScanType CustomScan -ScanPath $f
  "scanned $f, still there: $(Test-Path $f)"
}
"== detections"
$d = Get-MpThreatDetection
if ($d) { $d | Format-List ThreatID, InitialDetectionTime, ProcessName, Resources, ActionSuccess } else { "(none)" }
"== Defender operational events 1006/1007/1015/1116/1117 (last 2 hours)"
Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Windows Defender/Operational'; Id = 1006, 1007, 1015, 1116, 1117; StartTime = (Get-Date).AddHours(-2) } -ErrorAction SilentlyContinue |
  ForEach-Object { "{0} {1}: {2}" -f $_.TimeCreated, $_.Id, ($_.Message -split "`n")[0..6] -join ' | ' }
"== Code Integrity events naming our files (SAC audit/enforce: 3033, 3034, 3076, 3077, 3089, 3118)"
Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational' -MaxEvents 4000 -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match 'onnxruntime|rbtest|rbsp-spike|workerspike|cpuprobe|mfcheck|llcheck|llcaller' } |
  Select-Object -First 30 | ForEach-Object { "{0} id {1}: {2}" -f $_.TimeCreated, $_.Id, (($_.Message -split "`n")[0]).Trim() }
"== Code Integrity event ids in the last 2 hours"
Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; StartTime = (Get-Date).AddHours(-2) } -ErrorAction SilentlyContinue |
  Group-Object Id | ForEach-Object { "id {0}: {1}" -f $_.Name, $_.Count }
