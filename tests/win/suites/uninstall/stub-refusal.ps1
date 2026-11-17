# T11: the installed stub started with --after-exit refuses (a `current` beside it: an install,
# never the after-exit there), exit 10, and deletes nothing
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$before = Get-AppTree
$p = Start-Process -FilePath $l.Stub -ArgumentList '--after-exit', '1', "`"$($l.Local)`"" -PassThru -WindowStyle Hidden
$done = $p.WaitForExit(30000)
Check ($done -and $p.ExitCode -eq 10) "the installed stub refuses --after-exit: exit 10 (got $(if ($done) { $p.ExitCode } else { 'still running' }))"
Check ((Get-AppTree) -ceq $before) 'nothing deleted'
Done
