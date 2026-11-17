# Part A2: the one-liner in a ConstrainedLanguage powershell.exe against the served TEST build:
# the language refusal alone (SAC's on a PC with Smart App Control On), never "Cannot create
# type" (V33), and nothing installed
$l = Get-Layout
$had = Test-Path -LiteralPath $l.Apps
$out = Invoke-OneLiner $VmRun.Url -Constrained
"got: $out"
$lang = Get-BootText 'Language'
$sac = Get-BootText 'SacOn'
Check ($out -ceq $lang -or $out -ceq $sac) 'the language (or SAC) refusal alone'
Check (-not $out.Contains('Cannot create type')) 'no "Cannot create type" (V33)'
Check ((Test-Path -LiteralPath $l.Apps) -eq $had) 'nothing installed'
Done
