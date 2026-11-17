# W0 (as SYSTEM): copy the spike from the share into the user's per-user app folder, the shape of
# the real install (%LOCALAPPDATA%\Programs\RB Stems Plus\<version>\, PLAN §2 Folders), and the
# windowless launcher the user jobs need after a revert. Prints what it installed (hashes).
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force C:\rbtest | Out-Null
Copy-Item \\Mac\win\probe\hidden.exe C:\rbtest\hidden.exe -Force
# the previous run's results: files the VM writes to the share are root's on the Mac
Remove-Item -Recurse -Force \\Mac\win\w0\results -ErrorAction SilentlyContinue
$u = (Get-CimInstance Win32_ComputerSystem).UserName.Split('\')[1]
$base = "C:\Users\$u\AppData\Local\Programs\RB Stems Plus W0"
Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
foreach ($k in "app", "app-release") {
  New-Item -ItemType Directory -Force "$base\$k" | Out-Null
  Copy-Item -Recurse "\\Mac\win\w0\$k\*" "$base\$k\"
}
# the user owns their own folder, as an installed app's
icacls $base /setowner $u /T /Q | Out-Null
icacls $base /grant "${u}:(OI)(CI)F" /T /Q | Out-Null
Get-ChildItem -Recurse -File $base | ForEach-Object {
  "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 16), $_.FullName.Substring($base.Length + 1)
}
$rel = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full').Release
".NET Framework 4 Release $rel (533320+ is 4.8.1)"
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
"{0} build {1}.{2} ({3}), {4}" -f $cv.ProductName, $cv.CurrentBuildNumber, $cv.UBR, $cv.DisplayVersion, $env:PROCESSOR_ARCHITECTURE
"SAC VerifiedAndReputablePolicyState: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy').VerifiedAndReputablePolicyState
