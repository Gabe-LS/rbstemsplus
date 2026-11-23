# As the interactive user through signed conhost, at High integrity. Compare the saved and live
# files before/after the actual served one-liner. Keep all operations ConstrainedLanguage-safe.
$pf = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion').ProgramW6432Dir
$save = Join-Path $pf 'RB Stems Plus\pioneer\7.2.19'
$saved = Join-Path $save 'onnxruntime.dll'
$live = Join-Path $pf 'rekordbox\rekordbox 7.2.19\onnxruntime.dll'
if (-not (Test-Path -LiteralPath $saved -PathType Leaf)) { Skip 'needs the saved set from restore-fixture' }
$want = (Get-FileHash -LiteralPath $saved).Hash
$before = (Get-FileHash -LiteralPath $live).Hash
Check (Test-SacHighToken) 'the interactive user runs at High integrity'
Check ((Get-SacState) -eq 1) 'SAC is on during Restore'
Check ($before -ne $want -and $before -eq (Get-FileHash -LiteralPath "$env:WINDIR\System32\version.dll").Hash) 'the foreign DLL is live before Restore'
$env:RBSTEMSPLUS_RESTORE = '1'
$out = & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -OutputFormat Text -Command "irm '$($VmRun.Url)bootstrap.ps1' | iex"
"Restore output: $out"
Check (($out -join "`n").Trim() -ceq "Pioneer's file is back in rekordbox 7.2.19.") 'the served one-liner reports a successful Restore'
Check ((Get-FileHash -LiteralPath $live).Hash -eq $want) "Pioneer's DLL is live again ($want)"
Check ((Get-FileHash -LiteralPath $saved).Hash -eq $want) 'the saved DLL is unchanged'
Check (-not (Test-Path -LiteralPath "$live.rbsp-new")) 'Restore removed the temporary DLL'
Done
