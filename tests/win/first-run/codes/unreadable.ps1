# a child powershell.exe given a -File that isn't there: it exits -196608 (0xFFFD0000), which this
# step passes on; the wrapper must map it to 1
#fake: rc -196608
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -File 'C:\rbtest\first-run-no-such-file.ps1'
"the child exited $LASTEXITCODE"
exit $LASTEXITCODE
