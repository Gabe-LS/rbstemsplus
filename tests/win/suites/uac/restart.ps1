# As SYSTEM: Windows restarts a few seconds after this step (shutdown.exe /r /t 0 from a SYSTEM
# task: the apps are asked, not forced), after the sign-out mark; the suite's next line is
# "wait SECS NAME logon" (autologon signs the user back in)
Start-Restart
Check (Test-Path -LiteralPath $SignoutMark) "the sign-out mark written ($SignoutMark)"
Done
