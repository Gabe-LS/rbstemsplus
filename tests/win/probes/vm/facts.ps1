# Phase 0, read-only facts of the VM (run as SYSTEM): KnownDLLs, Smart App Control, Defender,
# rekordbox's folder, its ACLs, its uninstall entry, App Paths, the signer, Prism settings.
# Prints no library key and nothing about tracks.
Copy-Item \\Mac\win\probe\hidden.exe C:\rbtest\hidden.exe -Force
"== OS"
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
"{0} build {1}.{2} ({3})" -f $cv.ProductName, $cv.CurrentBuildNumber, $cv.UBR, $cv.DisplayVersion
$env:PROCESSOR_ARCHITECTURE
"== KnownDLLs"
$k = Get-Item 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs'
$k.GetValueNames() | Sort-Object | ForEach-Object { "{0}={1}" -f $_, $k.GetValue($_) }
"== SAC (CI\Policy)"
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' | Format-List *
"== Defender"
Get-MpComputerStatus | Select-Object AMRunningMode, AntivirusEnabled, RealTimeProtectionEnabled, BehaviorMonitorEnabled, IoavProtectionEnabled, OnAccessProtectionEnabled, IsTamperProtected, AMProductVersion, AntivirusSignatureVersion, AntivirusSignatureLastUpdated | Format-List
Get-MpPreference | Select-Object MAPSReporting, SubmitSamplesConsent, CloudBlockLevel, DisableRealtimeMonitoring, EnableControlledFolderAccess, PUAProtection | Format-List
"== rekordbox folder"
$rb = 'C:\Program Files\rekordbox\rekordbox 7.2.19'
Get-ChildItem $rb -File | Sort-Object Name | ForEach-Object { "{0,12} {1}" -f $_.Length, $_.Name }
Get-ChildItem $rb -Directory | ForEach-Object { "<dir> " + $_.Name }
"== icacls"
icacls 'C:\Program Files\rekordbox'
icacls $rb
icacls "$rb\onnxruntime.dll"
icacls "$rb\rekordbox.exe"
(Get-Acl $rb).Owner
(Get-Acl "$rb\onnxruntime.dll").Owner
"== uninstall entries"
Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' |
  ForEach-Object { $p = Get-ItemProperty $_.PSPath; if ("$($p.DisplayName)$($p.Publisher)" -match 'rekordbox|AlphaTheta|Pioneer') { "-- " + $_.PSPath; $p | Select-Object DisplayName, DisplayVersion, Publisher, InstallLocation, UninstallString, QuietUninstallString, WindowsInstaller, DisplayIcon, NoModify, NoRepair, EstimatedSize | Format-List } }
"== App Paths"
Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths' | Where-Object { $_.PSChildName -match 'rekordbox' } | ForEach-Object { $_.PSChildName; Get-ItemProperty $_.PSPath | Format-List '(default)', Path }
"== signatures"
foreach ($f in 'rekordbox.exe', 'onnxruntime.dll', 'DirectML.dll', 'Demucs3.dll', 'libmpg123.dll') {
  $s = Get-AuthenticodeSignature "$rb\$f" -ErrorAction SilentlyContinue
  if ($s) { "{0}: {1} | {2} | issuer {3} | serial {4} | thumbprint {5}" -f $f, $s.Status, $s.SignerCertificate.Subject, $s.SignerCertificate.Issuer, $s.SignerCertificate.SerialNumber, $s.SignerCertificate.Thumbprint }
}
"== versions"
foreach ($f in Get-ChildItem $rb -Include *.exe, *.dll -Recurse -Depth 0) { "{0}: {1} / {2}" -f $f.Name, $f.VersionInfo.FileVersion, $f.VersionInfo.ProductVersion }
"== Prism / AppCompat"
foreach ($h in 'HKCU', 'HKLM') {
  foreach ($p in "${h}:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers") {
    if (Test-Path $p) { "-- $p"; Get-ItemProperty $p | Format-List * }
  }
}
$u = (Get-CimInstance Win32_ComputerSystem).UserName
$sid = (New-Object System.Security.Principal.NTAccount($u)).Translate([System.Security.Principal.SecurityIdentifier]).Value
$p = "Registry::HKEY_USERS\$sid\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
if (Test-Path $p) { "-- user $u Layers"; Get-ItemProperty $p | Format-List * } else { "-- user $u has no AppCompatFlags\Layers" }
"== services/tasks named like rekordbox/AlphaTheta"
Get-ScheduledTask | Where-Object { $_.TaskName -match 'rekordbox|alphatheta|pioneer' } | Select-Object TaskPath, TaskName, State
Get-Service | Where-Object { $_.Name -match 'rekordbox|alphatheta|pioneer' -or $_.DisplayName -match 'rekordbox|alphatheta|pioneer' } | Select-Object Name, DisplayName, Status
Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run' | Format-List *
"== copies to the share"
New-Item -ItemType Directory -Force \\Mac\win\facts | Out-Null
Copy-Item "$rb\libmpg123.dll" \\Mac\win\facts\ -ErrorAction SilentlyContinue
Get-ChildItem \\Mac\win\facts
