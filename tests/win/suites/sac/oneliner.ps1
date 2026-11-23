# As the user, through user-signed (BS1 under Smart App Control On): the real one-liner
# (irm ... | iex) in a child powershell.exe against the served TEST build, plain (the "install"
# step) or in ConstrainedLanguage ("cl-refusal"): the SAC line alone (cl: it or the language
# line), and nothing installed (no apps folder made). ConstrainedLanguage-safe (sac/lib.ps1): the
# output comes back through the console's code page, so the line is compared up to its first
# non-ASCII character (the U+203A of "Windows Security > ...").
$cl = $VmRun.Step -like 'cl-*'
Check ((Test-SacHighToken) -eq ($VmRun.Step -eq 'install-high')) 'the launcher supplied the expected token'
Check ((Get-SacState) -eq 1) 'SAC is on during the one-liner'
$apps = Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus'
$had = Test-Path -LiteralPath $apps
"the apps folder before: $had"
$src = Get-Content -LiteralPath '\\Mac\win\repo\scripts\bootstrap.ps1' -ErrorAction SilentlyContinue
if (-not $src) { Skip 'no \\Mac\win\repo\scripts\bootstrap.ps1 (vm-run.sh stage)' }
$line = [string](@($src | Where-Object { $_ -match '^\s*SacOn\s*=\s*"' }) | Select-Object -First 1)
$sacText = $line -replace '^\s*SacOn\s*=\s*"', '' -replace '"\s*$', ''
$sacHead = ($sacText -split '\$Gt', 2)[0].TrimEnd()
$langLine = [string](@($src | Where-Object { $_ -match '^\s*Language\s*=\s*"' }) | Select-Object -First 1)
$langText = $langLine -replace '^\s*Language\s*=\s*"', '' -replace '"\s*$', ''
"expected: the SAC line, from '$sacHead'$(if ($cl) { " (or '$langText')" })"
$cmd = "`$ProgressPreference = 'SilentlyContinue'; "
if ($cl) { $cmd += "`$ExecutionContext.SessionState.LanguageMode = 'ConstrainedLanguage'; " }
$cmd += "irm '$($VmRun.Url)bootstrap.ps1' | iex"
$env:RBSTEMSPLUS_BASE_URL = $VmRun.Url
$eap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$out = (& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -OutputFormat Text -Command $cmd 2>&1 | Out-String)
$ErrorActionPreference = $eap
$out = ($out -replace "$([char]27)\[[0-9;?]*[A-Za-z]", '').Trim()
"got: $out"
$sac = $sacHead -and $out.StartsWith($sacHead) -and $out.Length -le $sacText.Length + 8 -and ($out -split "`n").Count -eq 1
$lang = $cl -and $langText -and $out -ceq $langText
Check ($sac -or $lang) "the SAC line alone$(if ($cl) { ' (or the language line)' })"
Check ((Test-Path -LiteralPath $apps) -eq $had) 'nothing installed (the apps folder as it was)'
Done
