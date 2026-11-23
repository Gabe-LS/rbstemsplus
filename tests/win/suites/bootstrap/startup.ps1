# The install step has closed the app and left the installed version and its watcher.
$arch = @(Get-TestArchs)[0]
$l = Get-Layout
$ver = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$dir = Join-Path $l.Apps $ver
$stub = Join-Path $dir "stub\$arch\RB Stems Plus.exe"
$shell = Join-Path $dir 'RB Stems Plus.exe'
$core = Join-Path $dir "$arch\rbsp-core.dll"
$watch = Join-Path $dir "rbsp-watch-$arch.exe"
if ($VmRun.TestArch -eq 'x64' -and
    (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE -eq 'ARM64') {
    $forced = Need 'vm/x64/forced-shell.exe' 'stage --build'
    Copy-Item -LiteralPath $forced -Destination $shell -Force
}
Get-AppProcesses | Where-Object { $_.ProcessName -like 'rbsp-watch-*' } | ForEach-Object {
    Stop-Process -Id $_.Id
    $_.WaitForExit(10000)
} | Out-Null
Copy-Item -LiteralPath $stub -Destination $l.Stub -Force
Start-Process -FilePath $l.Stub -ArgumentList '--watcher'
Start-Process -FilePath $l.Stub
Check (Wait-Until { @(Get-AppProcesses | Where-Object { $_.Path -ieq $watch }).Count -eq 1 } 30) "the $arch stub launched the $arch watcher"
$app = $null
Check (Wait-Until { $script:app = Get-AppWindow; $null -ne $script:app } 60) "the $arch stub launched the installed shell"
if ($app) {
    Check (@($app.Modules | Where-Object { $_.FileName -ieq $core }).Count -eq 1) "the shell loaded the installed $arch core"
}
Stop-App
Done
