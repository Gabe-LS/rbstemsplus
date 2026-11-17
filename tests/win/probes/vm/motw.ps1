# Mark of the Web and the Smart App Control read (run as the user, through winps.sh --user).
$d = "$env:LOCALAPPDATA\rbsp-motw"
Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $d | Out-Null
$url = 'https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/bootstrap.sh'
$zip = 'https://github.com/Gabe-LS/rbstemsplus/archive/refs/tags/v1.3.0.zip'
"PowerShell $($PSVersionTable.PSVersion) $($PSVersionTable.PSEdition), language $($ExecutionContext.SessionState.LanguageMode)"
"== downloads"
try { Invoke-WebRequest $url -OutFile "$d\iwr.sh" -UseBasicParsing } catch { "iwr failed: $_" }
try { Invoke-RestMethod $url -OutFile "$d\irm.sh" } catch { "irm failed: $_" }
try { (New-Object Net.WebClient).DownloadFile($url, "$d\webclient.sh") } catch { "webclient failed: $_" }
try { Invoke-WebRequest $zip -OutFile "$d\src.zip" -UseBasicParsing; Expand-Archive "$d\src.zip" "$d\unz" -Force } catch { "zip failed: $_" }
& curl.exe -sSL -o "$d\curl.sh" $url
"== streams (Zone.Identifier means Mark of the Web)"
foreach ($f in Get-ChildItem $d -File) {
  $s = (Get-Item $f.FullName -Stream * | Where-Object Stream -ne ':$DATA' | ForEach-Object Stream) -join ','
  "{0,-14} {1,8} bytes, streams: {2}" -f $f.Name, $f.Length, $(if ($s) { $s } else { '(none)' })
}
$ex = Get-ChildItem "$d\unz" -Recurse -File | Select-Object -First 200
$marked = @($ex | Where-Object { Get-Item $_.FullName -Stream Zone.Identifier -ErrorAction SilentlyContinue }).Count
"extracted from src.zip: $($ex.Count) files checked, $marked with Zone.Identifier"
"== control: a file marked by hand, as a browser would"
Set-Content "$d\marked.txt" 'x'
Set-Content "$d\marked.txt" -Stream Zone.Identifier "[ZoneTransfer]`r`nZoneId=3"
"marked.txt streams: " + ((Get-Item "$d\marked.txt" -Stream * | ForEach-Object Stream) -join ',')
"== irm | iex runs a string (no file, no execution policy)"
"irm|iex: " + (Invoke-Expression ([string]'"ran " + (1+1)'))
"== Smart App Control read, FullLanguage"
"VerifiedAndReputablePolicyState = " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy').VerifiedAndReputablePolicyState
"== the same reads in ConstrainedLanguage (a child powershell.exe 5.1)"
$cl = @'
$ExecutionContext.SessionState.LanguageMode = 'ConstrainedLanguage'
"language: " + $ExecutionContext.SessionState.LanguageMode
"SAC: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy').VerifiedAndReputablePolicyState
"SAC (Get-ItemPropertyValue): " + (Get-ItemPropertyValue 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -Name VerifiedAndReputablePolicyState)
"build: " + (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber
"NDP Release: " + (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full').Release
"OSVersion.Build (I74): [" + [Environment]::OSVersion.Version.Build + "]"
'@
$enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cl))
& powershell.exe -NoProfile -NonInteractive -EncodedCommand $enc 2>&1 | ForEach-Object { "  $_" }
Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
