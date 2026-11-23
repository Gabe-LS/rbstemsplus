# After worker/prep.ps1 (the snapshot's facts, rekordbox's own onnxruntime.dll's hash saved), as
# SYSTEM: UAC prompts again as Windows ships it (worker/prep.ps1 turned the prompt off for its
# suite; here the prompt is the case: ConsentPromptBehaviorAdmin 5, consent for non-Windows
# programs, on the secure desktop), the UI driver installed (C:\rbtest\uia), autologon set (the
# sign-outs and restarts sign back in by themselves), rekordbox's stems model's hash added to the
# facts (WC10's "intact" case), and no sign-out mark or step value left from anything earlier.
$pol = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
Set-ItemProperty -LiteralPath $pol -Name ConsentPromptBehaviorAdmin -Value 5 -Type DWord
Set-ItemProperty -LiteralPath $pol -Name PromptOnSecureDesktop -Value 1 -Type DWord
$p = Get-ItemProperty $pol
Check ($p.EnableLUA -eq 1 -and $p.ConsentPromptBehaviorAdmin -eq 5 -and $p.PromptOnSecureDesktop -eq 1) "UAC prompts for consent on the secure desktop (EnableLUA $($p.EnableLUA), ConsentPromptBehaviorAdmin $($p.ConsentPromptBehaviorAdmin), PromptOnSecureDesktop $($p.PromptOnSecureDesktop))"
Remove-Item -LiteralPath $SignoutMark -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $UacDir -Recurse -Force -ErrorAction SilentlyContinue
Check (Install-Uia) "rbui and sysdesk in $UiaDir (vm/uia/rbui, vm/<arch>/sysdesk.exe)"
Check (Set-AutoLogon) 'autologon set (a new random password on the local account, in this reverted VM only)'
$f = Read-WorkerFacts
if (-not $f) { Check $false 'the prep step saved its facts'; Done }
$model = if ($RbModelDir) { Join-Path $RbModelDir 'hdemucs.onnx' } else { '' }
$sha = if ($model) { Get-Sha $model } else { $null }
"rekordbox's stems model: $model $(if ($sha) { $sha } else { '(none)' })"
$f | Add-Member -Force ModelSha ([string]$sha)
ConvertTo-Json -InputObject $f | Set-Content -LiteralPath $WorkerFacts -Encoding UTF8
Check ([bool](Read-WorkerFacts).PioneerSha) 'the facts saved again with the model hash'
Done
