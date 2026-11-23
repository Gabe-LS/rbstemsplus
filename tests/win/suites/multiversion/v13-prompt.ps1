# V13, as SYSTEM: UAC prompts again as Windows ships it for this one case (uac/setup.ps1's
# values: ConsentPromptBehaviorAdmin 5 on the secure desktop), and the UI driver installed for the
# arm-no responder (rbui through sysdesk, the UAC suites' approved way). v13-done turns the
# prompt off again for the rest of the suite.
$pol = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
Set-ItemProperty -LiteralPath $pol -Name ConsentPromptBehaviorAdmin -Value 5 -Type DWord
Set-ItemProperty -LiteralPath $pol -Name PromptOnSecureDesktop -Value 1 -Type DWord
$p = Get-ItemProperty $pol
Check ($p.ConsentPromptBehaviorAdmin -eq 5 -and $p.PromptOnSecureDesktop -eq 1) 'UAC prompts for consent on the secure desktop'
Check (Install-Uia) "rbui and sysdesk in $UiaDir"
Done
