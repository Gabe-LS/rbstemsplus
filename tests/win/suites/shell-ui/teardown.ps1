# The runner still runs teardown after a failed step. Only remove this run's SMB fixture
# when no saved after-exit folder needs it for diagnosis under --keep.
if ((Test-Path -LiteralPath (Join-Path $ShellWork 'tmp')) -and
    @(Get-AfterExitCopies (Join-Path $ShellWork 'tmp')).Count -eq 0) {
    Remove-SmbShare -Name 'rbsp-shell-ui-tmp' -Force -ErrorAction SilentlyContinue
}
if (Test-Path -LiteralPath (Join-Path $ShellWork 'autologon-set')) { Clear-AutoLogon }
Check $true 'shell-ui teardown ran'
Done
