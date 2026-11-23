# As SYSTEM: Smart App Control is On, as Windows reports it: the Code Integrity policy's
# VerifiedAndReputablePolicyState 1 and Defender's SmartAppControlState "On"; and it acts: a fresh
# copy of our unsigned hidden.exe (its bytes in a new file: the copy the run's prepare made before
# SAC was On runs still, SAC keeps its verdict on a file) is blocked by an Application Control
# policy (bootstrap-sac.suite's user steps go through user-signed for that reason)
$st = Get-SacState
Check ($st -eq 1) "VerifiedAndReputablePolicyState is 1 ($st)"
$mp = Get-MpSac
Check ($mp -eq 'On') "Defender's SmartAppControlState is On ($mp)"
if (Test-Path -LiteralPath 'C:\rbtest\hidden.exe') {
    $fresh = "C:\rbtest\sac-probe-$($VmRun.Run).exe"
    Remove-Item -LiteralPath $fresh -Force -ErrorAction SilentlyContinue
    # Get-Content/Set-Content (cmdlets, as ConstrainedLanguage allows): a new file, no copied stream
    Set-Content -LiteralPath $fresh -Value (Get-Content -LiteralPath 'C:\rbtest\hidden.exe' -Encoding Byte -Raw) -Encoding Byte
    $why = ''
    try {
        $p = Start-Process -FilePath $fresh -PassThru -WindowStyle Hidden -ErrorAction Stop
        "the fresh copy started (pid $($p.Id))"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    } catch { $why = [string]$_; "the fresh copy didn't start: $why" }
    Check ($why -match 'Application Control policy') 'a fresh copy of the unsigned hidden.exe is blocked (an Application Control policy)'
    Remove-Item -LiteralPath $fresh -Force -ErrorAction SilentlyContinue
} else { Check $false 'C:\rbtest\hidden.exe is there to copy (the run copies it in from \\Mac\win\probe)' }
Done
