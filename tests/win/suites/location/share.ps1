# As SYSTEM: an SMB share of this VM (\\127.0.0.1\rbn3) the user maps as a network drive
$d = Join-Path $LocDir 'share'
New-Item -ItemType Directory -Force $d | Out-Null
& icacls.exe $d /grant '*S-1-1-0:(OI)(CI)M' | Out-Null
if (Get-SmbShare -Name 'rbn3' -ErrorAction SilentlyContinue) { Remove-SmbShare -Name 'rbn3' -Force }
New-SmbShare -Name 'rbn3' -Path $d -FullAccess 'Everyone' | Out-Null
Check ([bool](Get-SmbShare -Name 'rbn3' -ErrorAction SilentlyContinue)) 'the share rbn3'
Done
