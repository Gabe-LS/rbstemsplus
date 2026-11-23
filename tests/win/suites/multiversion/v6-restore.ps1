# V6, as SYSTEM: 7.2.19's real rekordbox.exe back (any stand-in still running ended first), and
# V6's batch: no elevation.
$exe = Get-RbExeOf '7.2.19'
Get-CimInstance Win32_Process -Filter "Name = 'rekordbox.exe'" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 2
if (Test-Path -LiteralPath "$exe.mvreal") { Move-Item -LiteralPath "$exe.mvreal" -Destination $exe -Force }
Check ((Get-Sha $exe) -eq $MvExeSha['7.2.19']) "7.2.19's real rekordbox.exe is back ($exe)"
Test-MvUac 'v6' 0
Done
