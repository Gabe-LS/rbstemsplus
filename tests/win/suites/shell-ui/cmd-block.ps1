# Only our own IFEO value is set or removed. The fixture forces the UN2 RunOnce fallback.
if ($VmRun.Step -eq 'block-cmd') {
    Expect (-not (Test-Path -LiteralPath $ShellCmdIfeo)) 'no preexisting cmd.exe IFEO configuration'
    Expect (-not (Test-Path -LiteralPath $ShellCmdBlock)) 'the IFEO debugger path does not exist'
    New-Item -Path $ShellCmdIfeo | Out-Null
    Set-ItemProperty -LiteralPath $ShellCmdIfeo -Name Debugger -Value $ShellCmdBlock
    "With --keep, undo after an interrupted run: Remove-ItemProperty -LiteralPath '$ShellCmdIfeo' -Name Debugger"
    Check ((Get-ItemProperty -LiteralPath $ShellCmdIfeo).Debugger -eq $ShellCmdBlock) 'cmd.exe is blocked for the cleanup fixture'
} else {
    if ((Get-ItemProperty -LiteralPath $ShellCmdIfeo -ErrorAction SilentlyContinue).Debugger -eq $ShellCmdBlock) {
        Remove-ItemProperty -LiteralPath $ShellCmdIfeo -Name Debugger
        Remove-Item -LiteralPath $ShellCmdIfeo
    }
    $p = Start-Process -FilePath "$env:WINDIR\System32\cmd.exe" -ArgumentList '/d', '/c', 'exit 7' -PassThru -WindowStyle Hidden
    Check ($p.WaitForExit(15000) -and $p.ExitCode -eq 7) 'cmd.exe starts again before restart'
}
Done
