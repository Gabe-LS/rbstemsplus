# Hides or restores the STEMS Engine model (run as SYSTEM): $state 'hide' renames the user's
# rekordbox6\models\demucs3_model folder, as if the engine were never downloaded; 'restore'
# puts it back.
$u = (Get-CimInstance Win32_ComputerSystem).UserName.Split('\')[1]
$m = "C:\Users\$u\AppData\Roaming\Pioneer\rekordbox6\models"
Get-Process rekordbox -ErrorAction SilentlyContinue | Stop-Process -Force
if ($state -eq 'hide' -and (Test-Path "$m\demucs3_model")) { Rename-Item "$m\demucs3_model" 'demucs3_model.hidden' }
if ($state -eq 'restore' -and (Test-Path "$m\demucs3_model.hidden")) { Rename-Item "$m\demucs3_model.hidden" 'demucs3_model' }
Get-ChildItem $m | ForEach-Object { "models\" + $_.Name }
