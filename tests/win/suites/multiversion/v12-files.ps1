# V12, as SYSTEM: one UAC; each folder's onnxruntime.dll is its own audited original again;
# Stems Cache's root gone; the account's Cache choice cleared (no rekordbox_model=1 left); the
# app's Uninstall entry and Run value gone.
Test-MvUac 'v12' 1
foreach ($v in $MvVersions) { Check ((Get-Sha (Get-RbOrtOf $v)) -eq $MvOrtSha[$v]) "rekordbox $v's onnxruntime.dll is its original ($(Get-Sha (Get-RbOrtOf $v)))" }
Test-MvRootGone 60
$l = Get-Layout
$cfg = Join-Path $l.Support 'config.ini'
Check (-not ((Test-Path -LiteralPath $cfg) -and (Select-String -LiteralPath $cfg -Pattern '^\s*rekordbox_model\s*=\s*1\s*$' -Quiet))) "the Cache choice is cleared ($cfg $(if (Test-Path -LiteralPath $cfg) { 'kept' } else { 'gone' }))"
$sid = (New-Object Security.Principal.NTAccount((Get-CimInstance Win32_ComputerSystem).UserName)).Translate([Security.Principal.SecurityIdentifier]).Value
Check (-not (Test-Path -LiteralPath "Registry::HKEY_USERS\$sid\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus")) 'the Apps entry is gone'
$stray = @(Get-ChildItem -LiteralPath (Join-Path $env:ProgramFiles 'rekordbox') -Recurse -Force -Filter '*.rbsp-*' -ErrorAction SilentlyContinue)
Check ($stray.Count -eq 0) "no stray .rbsp-* file ($(@($stray | ForEach-Object FullName) -join ', '))"
Done
