# Smart App Control test, set-up (run as SYSTEM, after install.ps1 with $which = $set, a folder of never-seen copies): puts
# never-seen unsigned stand-ins where our files will live, and lets an administrator's elevation
# go through without the secure-desktop prompt (synthesized input can't click it). Throwaway VM
# state: the snapshot is restored afterwards.
if (-not $set) { $set = 'sac2' }
$src = "\\Mac\win\p0\$set"
$u = (Get-CimInstance Win32_ComputerSystem).UserName.Split('\')[1]
$app = "C:\Users\$u\AppData\Local\Programs\RB Stems Plus\9.9.9"
New-Item -ItemType Directory -Force "$app\stub" | Out-Null
Copy-Item "$src\app.exe" "$app\RB Stems Plus.exe" -Force
Copy-Item "$src\corehost.exe", "$src\rbsp-core.dll" $app -Force
Copy-Item "$src\stub\onnxruntime.dll" "$app\stub\" -Force
$bin = 'C:\Program Files\RB Stems Plus\bin\0123456789ab'
New-Item -ItemType Directory -Force $bin | Out-Null
Copy-Item "$src\worker.exe" "$bin\RB Stems Plus.exe" -Force
$k = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
Set-ItemProperty $k ConsentPromptBehaviorAdmin 0 -Type DWord
Set-ItemProperty $k PromptOnSecureDesktop 0 -Type DWord
"stand-ins in $app and $bin; UAC prompt off for admins"
if ($motw) { # $motw: mark every stand-in and the bridge as downloaded from the internet
  $files = @("$app\RB Stems Plus.exe", "$app\corehost.exe", "$app\rbsp-core.dll", "$app\stub\onnxruntime.dll", "$bin\RB Stems Plus.exe",
             'C:\Program Files\rekordbox\rekordbox 7.2.19\onnxruntime.dll')
  foreach ($f in $files) { Set-Content -Path $f -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3" }
  "Mark of the Web set on $($files.Count) files"
}
"SAC state: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy').VerifiedAndReputablePolicyState
