# -Restore refused, rekordbox's DLL left as it was (release.md section 4). The steps that use this
# script: refused-unelevated (user: the filtered token; -Restore requires S-1-16-12288 in
# whoami /groups) and refused-junction (user-high, with rekordbox's version folder a junction to
# its real folder: a ReparsePoint in the destination path, I23).
Assert-RestoreServed
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
$bridge = ([string](Read-TestPayload).windows.bridge.sha256).ToLowerInvariant()
$high = Test-HighToken
if ($VmRun.Step -eq 'refused-unelevated') { Check (-not $high) 'this step runs unelevated (no High mandatory level)' }
else {
    Check $high 'this step runs elevated (High mandatory level)'
    Check ((Test-Path -LiteralPath $RbDir) -and [bool]((Get-Item -LiteralPath $RbDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) "rekordbox's version folder is a junction ($RbDir)"
}
$before = Get-LiveState $f $bridge
if ($before -ne 'the bridge') { Skip "rekordbox's DLL is $before, not the bridge (the step before failed: its log)" }
$out = Invoke-Restore
"-Restore: $out"
if ($VmRun.Step -eq 'refused-unelevated') {
    Check ($out -ceq (Get-BootText 'RestoreElevated')) '-Restore asked for an administrator PowerShell window'
} else {
    Check ($out -like '*reached through a link*') '-Restore refused because the folder is reached through a link'
}
Check ((Get-LiveState $f $bridge) -eq 'the bridge') "rekordbox's DLL left as it was (the bridge)"
Done
