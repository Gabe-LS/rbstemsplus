# K1 ends with the same Settings route after reboot. The ordinary app confirmation and
# completion still show. There must be no after-exit kept dialog on this successful retry.
Expect (@(Get-AppProcesses | Where-Object ProcessName -eq 'w2-held').Count -eq 0) 'the second image is gone before retry'
Start-AfterExitWatch
Start-SettingsUninstall
$ax = Wait-AfterExit 10
if ($ax) {
    Check ($ax.WaitForExit(30000) -and $ax.ExitCode -eq 0) 'the after-exit returned AX_DONE without a kept dialog'
}
Test-UninstallGone
$dialogs = @(Get-Process -Name $ShellName -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -match '\\rbsp-uninstall-[0-9a-f]{16}\\' -and $_.MainWindowHandle -ne [IntPtr]::Zero })
Check ($dialogs.Count -eq 0) 'no after-exit dialog remains'
Done
