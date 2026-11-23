# From the clean Windows snapshot. Use the VM's own SMB server for UN6's UNC TEMP.
Expect (Install-Uia) 'rbui is staged locally for the user steps'
New-Item -ItemType Directory -Path $ShellWork | Out-Null
$facts = Join-Path $ShellWork 'tmp'
New-Item -ItemType Directory -Path $facts | Out-Null
$account = (Get-CimInstance Win32_ComputerSystem).UserName
& icacls.exe $ShellWork /grant "${account}:(OI)(CI)M" | Out-Null
Expect ($LASTEXITCODE -eq 0) 'the console account can write the suite state and UNC TEMP'
New-SmbShare -Name 'rbsp-shell-ui-tmp' -Path $facts -FullAccess $account | Out-Null
Check ((Get-SmbShare -Name 'rbsp-shell-ui-tmp').Path -eq $facts) "UN6's local SMB share is available at $ShellUnc"
Done
