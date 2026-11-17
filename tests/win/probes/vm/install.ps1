# Puts a probe in rekordbox's place (run as SYSTEM). The variant is the first line's $which,
# written by the caller: 'ort' (pass-through, real ORT in C:\Program Files\RB Stems Plus\ort\),
# 'stub' (the R1 refusal stub, no real ORT), or 'orig' (Pioneer's file back).
# $flags (optional): flag files to create in C:\rbtest (fault injection, stub modes); every other
# fail-*/stub-* flag is removed. Also keeps hidden.exe and WER crash dumps (C:\rbtest\dumps) set up.
$rb = 'C:\Program Files\rekordbox\rekordbox 7.2.19'
Get-Process rekordbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
New-Item -ItemType Directory -Force C:\rbtest\orig, C:\rbtest\dumps | Out-Null
if (-not (Test-Path C:\rbtest\hidden.exe)) { Copy-Item \\Mac\win\probe\hidden.exe C:\rbtest\hidden.exe }
$k = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\rekordbox.exe'
New-Item -Force $k | Out-Null
Set-ItemProperty $k DumpFolder C:\rbtest\dumps
Set-ItemProperty $k DumpType 1 -Type DWord
Set-ItemProperty $k DumpCount 10 -Type DWord
if (-not (Test-Path C:\rbtest\orig\onnxruntime.dll)) { Copy-Item "$rb\onnxruntime.dll" C:\rbtest\orig\ }
$ort = 'C:\Program Files\RB Stems Plus\ort'
New-Item -ItemType Directory -Force $ort | Out-Null
Copy-Item C:\rbtest\orig\onnxruntime.dll "$ort\onnxruntime.dll" -Force
switch ($which) {
  'orig' { Copy-Item C:\rbtest\orig\onnxruntime.dll "$rb\onnxruntime.dll" -Force }
  default { Copy-Item "\\Mac\win\p0\$which\onnxruntime.dll" "$rb\onnxruntime.dll" -Force }
}
Get-ChildItem C:\rbtest -File | Where-Object Name -match '^(fail|stub)-' | Remove-Item
foreach ($f in $flags) { if ($f) { New-Item -ItemType File -Force "C:\rbtest\$f" | Out-Null } }
Remove-Item C:\rbtest\ortprobe.log, C:\rbtest\stub.log -ErrorAction SilentlyContinue
"installed '$which': " + (Get-FileHash "$rb\onnxruntime.dll").Hash + ", flags: " + ((Get-ChildItem C:\rbtest -File | Where-Object Name -match '^(fail|stub)-' | ForEach-Object Name) -join ',')
"signature: " + (Get-AuthenticodeSignature "$rb\onnxruntime.dll").Status
