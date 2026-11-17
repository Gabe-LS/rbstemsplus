# Phase 0 spikes that don't need rekordbox running (run as SYSTEM, i.e. elevated):
#  - G1 item 6: the worker's imports vs KnownDLLs, planted DLLs next to it in a user folder;
#  - R3/R1: llcheck (loader-lock check + stub under the lock);
#  - Prism CPU features (x64 and arm64), rekordbox's libmpg123 decoders;
#  - T1: Media Foundation decode with the llvm-mingw build.
$p0 = '\\Mac\win\p0'
New-Item -ItemType Directory -Force C:\rbtest | Out-Null
Remove-Item C:\rbtest\planted.log, C:\rbtest\stub.log -ErrorAction SilentlyContinue
$u = (Get-CimInstance Win32_ComputerSystem).UserName.Split('\')[1]
$dir = "C:\Users\$u\AppData\Local\Temp\rbsp-spike"
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dir | Out-Null

"== worker spike: alone, from a user folder"
Copy-Item "$p0\bin\workerspike.exe" $dir
Start-Process -Wait -FilePath "$dir\workerspike.exe" -ArgumentList "`"$dir\report-alone.txt`""
Get-Content "$dir\report-alone.txt"

"== worker spike: with planted DLLs next to it"
Copy-Item "$p0\planted\*.dll" $dir
Get-ChildItem $dir -Filter *.dll | ForEach-Object { "planted " + $_.Name }
$pr = Start-Process -Wait -PassThru -FilePath "$dir\workerspike.exe" -ArgumentList "`"$dir\report-planted.txt`""
"exit code " + $pr.ExitCode
if (Test-Path "$dir\report-planted.txt") { Get-Content "$dir\report-planted.txt" } else { "no report" }
"planted.log:"
if (Test-Path C:\rbtest\planted.log) { Get-Content C:\rbtest\planted.log -Encoding Unicode } else { "(none: no planted DLL was loaded)" }

"== mfcheck (planted DLLs still next to nothing: run from the share copy)"
$m = "C:\rbtest\p0"
Remove-Item -Recurse -Force $m -ErrorAction SilentlyContinue
Copy-Item -Recurse $p0 $m
& "$m\bin\mfcheck-x64.exe" "$m\media\tone.m4a"
& "$m\bin\mfcheck-arm64.exe" "$m\media\tone.m4a"
& "$m\bin\mfcheck-x64.exe" "$m\media\tone.wav"

"== llcheck (stub under and outside the loader lock)"
& "$m\bin\llcheck.exe" "$m\stub\onnxruntime.dll" "$m\bin\llcaller.dll"
"exit $LASTEXITCODE"
Get-Content C:\rbtest\stub.log

"== cpuprobe x64 (under Prism)"
& "$m\bin\cpuprobe-x64.exe" --mpg123 'C:\Program Files\rekordbox\rekordbox 7.2.19\libmpg123.dll'
"== cpuprobe arm64"
& "$m\bin\cpuprobe-arm64.exe"
