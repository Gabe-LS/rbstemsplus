# K4: cmd.exe is available again. Restart without closing the kept dialog.
foreach ($case in 'un2', 'un6', 'k4') {
    Expect (Test-Path -LiteralPath (Join-Path $ShellWork "$case.txt")) "$case reached its cleanup checkpoint"
}
Expect (-not ((Get-ItemProperty -LiteralPath $ShellCmdIfeo -ErrorAction SilentlyContinue).Debugger)) 'cmd.exe is unblocked'
$kept = (Get-Content -Raw -LiteralPath (Join-Path $ShellWork 'k4.txt')).Trim()
$ax = @(Get-Process -Name $ShellName -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -ieq (Join-Path $kept "$ShellName.exe") })
# The user step checked the open window. SYSTEM is in session 0 and checks its owner is still alive.
Expect ($ax.Count -eq 1) 'the after-exit process whose dialog was left open is still running'
# Set autologon after the pre-restart UNC round has finished using this logon session.
Expect (Set-AutoLogon) 'the suite can resume after restarting Windows'
New-Item -ItemType File -Path (Join-Path $ShellWork 'autologon-set') | Out-Null
Start-Restart
Check (Test-Path -LiteralPath $SignoutMark) 'restart marked for the runner to wait for the next logon'
Done
