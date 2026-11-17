# Round 1's cases, planted as the user before the menu's uninstall (app.md 5.9's VM cases):
#   a junction inside the apps folder, to a folder of the user's that must be kept;
#   a planted %TEMP%\ping.cmd that must never run (the after-exit's cmd loop names PING.EXE by its
#   System32 path), writing a marker if it ever does;
#   EnableExtensions=0 in HKCU's Command Processor key (the loop runs cmd /e:on);
#   an odd %TEMP% for the uninstall, holding ! % and ^ (menu.ps1 starts the app with it).
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
Stop-App
Remove-Item -LiteralPath $CaseDir -Recurse -Force -ErrorAction SilentlyContinue
$target = Join-Path $CaseDir 'junction-target'
New-Item -ItemType Directory -Force $target | Out-Null
Set-Content -LiteralPath (Join-Path $target 'keep.txt') -Value 'kept' -Encoding ASCII
$j = Join-Path $l.Apps 'rbtest-junction'
New-Item -ItemType Junction -Path $j -Target $target | Out-Null
Check (Test-Reparse $j) "a junction inside the apps folder ($j -> $target)"
$odd = Join-Path $l.Local 'Temp\rb!%^t'
New-Item -ItemType Directory -Force $odd | Out-Null
$marker = Join-Path $CaseDir 'ping-ran.txt'
$cmdText = "@echo off`r`necho ran > `"$marker`"`r`n"
foreach ($t in @((Join-Path $l.Local 'Temp'), $odd)) { [IO.File]::WriteAllText((Join-Path $t 'ping.cmd'), $cmdText) }
Check ((Test-Path -LiteralPath (Join-Path $odd 'ping.cmd')) -and -not (Test-Path -LiteralPath $marker)) "a planted ping.cmd in %TEMP% and in the odd one ($odd)"
$cp = 'HKCU:\Software\Microsoft\Command Processor'
if (-not (Test-Path -LiteralPath $cp)) { New-Item -Path $cp -Force | Out-Null }
Set-ItemProperty -LiteralPath $cp -Name EnableExtensions -Value 0 -Type DWord
Check ((Get-ItemProperty -LiteralPath $cp).EnableExtensions -eq 0) 'EnableExtensions=0 in HKCU'
Done
