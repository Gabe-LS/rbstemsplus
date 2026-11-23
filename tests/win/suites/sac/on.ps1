# As SYSTEM (plan row 46): Smart App Control switched On from Evaluation (or Off): the Code
# Integrity policy's VerifiedAndReputablePolicyState set to 1 and the policies refreshed
# (CiTool.exe --refresh), read back; then Windows restarts 15 s after this step (shutdown.exe,
# after the sign-out mark the suite's "wait ... logon" reads), so it boots with the state, and
# signs the user in by itself (no autologon of ours: "SAC On" is a snapshot, never taken with one)
"before: VerifiedAndReputablePolicyState $(Get-SacState), Defender's SmartAppControlState $(Get-MpSac)"
$wl = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon' -ErrorAction SilentlyContinue
# only whether Windows signs a user in by itself after the restart (never the password)
"Winlogon AutoAdminLogon '$($wl.AutoAdminLogon)', DefaultUserName '$($wl.DefaultUserName)', a DefaultPassword set: $([bool]$wl.DefaultPassword)"
if ((Get-SacState) -ne 1) { Set-ItemProperty -LiteralPath $SacKey -Name VerifiedAndReputablePolicyState -Value 1 -Type DWord }
Check ((Get-SacState) -eq 1) 'VerifiedAndReputablePolicyState is 1 (On)'
$citool = "$env:WINDIR\System32\CiTool.exe"
if (Test-Path -LiteralPath $citool) {
    $o = & $citool --refresh 2>&1 | Out-String
    "CiTool --refresh: exit $LASTEXITCODE $o"
} else { 'no CiTool.exe: the restart applies it' }
"after the refresh: Defender's SmartAppControlState $(Get-MpSac)"
New-Item -ItemType Directory -Force 'C:\rbtest' | Out-Null
Set-Content -LiteralPath 'C:\rbtest\signout.mark' -Value ((Get-Date).ToUniversalTime().Ticks) -Encoding ASCII
& "$env:WINDIR\System32\shutdown.exe" /r /t 15 /d p:4:1 /c 'sac.suite: Smart App Control On'
Check ($LASTEXITCODE -eq 0) "the restart is under way (shutdown.exe exit $LASTEXITCODE)"
Done
