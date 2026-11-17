# cmd.exe unblocked again, as SYSTEM (block-cmd.ps1's IFEO key removed), whatever the step before
# did; the RunOnce value stays for the audit (the VM is reverted after the run)
$k = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\cmd.exe'
Remove-Item -LiteralPath $k -Recurse -Force -ErrorAction SilentlyContinue
Check (-not (Test-Path -LiteralPath $k)) 'cmd.exe unblocked (no IFEO key)'
$p = Start-Process -FilePath "$env:WINDIR\System32\cmd.exe" -ArgumentList '/c', 'exit 7' -PassThru -WindowStyle Hidden
Check ($p.WaitForExit(30000) -and $p.ExitCode -eq 7) 'cmd.exe starts again'
Done
