# The shell's VM smoke (as SYSTEM, through winps.sh): copies build/shell/app from the share into
# the user's per-user app folder, the release layout's version folder
# (%LOCALAPPDATA%\Programs\RB Stems Plus\<version>\, app.md §5.10), owned by the user. Prints
# what it installed. Test only.
$ErrorActionPreference = "Stop"
Remove-Item -Recurse -Force \\Mac\win\shell\results -ErrorAction SilentlyContinue
$account = (Get-CimInstance Win32_ComputerSystem).UserName
$u = $account.Split('\')[1]
# the user's own profile folder (ProfileList by SID), the one smoke.ps1's %LOCALAPPDATA% is in
$sid = (New-Object System.Security.Principal.NTAccount($account)).Translate([System.Security.Principal.SecurityIdentifier]).Value
$userDir = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$sid").ProfileImagePath
# a copy left running by an earlier run holds its files
Get-Process "RB Stems Plus" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
# the release build and the TEST build (TestHooks.cs), each in its own folder
foreach ($k in @(@("app", "shell-smoke"), @("app-test", "shell-smoke-test"))) {
  $base = "$userDir\AppData\Local\Programs\RB Stems Plus\" + $k[1]
  Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $base | Out-Null
  Copy-Item -Recurse ("\\Mac\win\shell\" + $k[0] + "\*") "$base\"
  icacls $base /setowner $u /T /Q | Out-Null
  icacls $base /grant "${u}:(OI)(CI)F" /T /Q | Out-Null
  Get-ChildItem -Recurse -File $base | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 16), ($k[1] + "\" + $_.FullName.Substring($base.Length + 1))
  }
}
