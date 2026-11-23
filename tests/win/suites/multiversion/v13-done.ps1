# V13, as SYSTEM: no elevation happened in the refused batch; UAC elevates without a prompt again
# (worker/prep.ps1's value) for the rest of the suite; receipts unchanged since v7.
Test-MvUac 'v13' 0
$pol = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
Set-ItemProperty -LiteralPath $pol -Name ConsentPromptBehaviorAdmin -Value 0 -Type DWord
Check ((Get-ItemProperty $pol).ConsentPromptBehaviorAdmin -eq 0) 'UAC elevates without a prompt again'
foreach ($v in $MvVersions) { Check (Test-Path -LiteralPath (Join-Path $CacheRoot "receipts\$v") -PathType Leaf) "receipts\$v still there" }
Done
