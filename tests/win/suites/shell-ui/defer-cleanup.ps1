# UN2 and UN6 drive the registered stub directly. K1/UN4 drive Windows Settings.
$l = Get-Layout
$case = $VmRun.Step
$unc = $case -eq 'un6'
$tmp = if ($unc) { $ShellUnc } else { [IO.Path]::GetTempPath() }
Expect (Test-Path -LiteralPath $tmp -PathType Container) "the fixture TEMP exists ($tmp)"
if (-not $unc) {
    $debugger = (Get-ItemProperty -LiteralPath $ShellCmdIfeo -ErrorAction SilentlyContinue).Debugger
    Expect ($debugger -eq $ShellCmdBlock) 'UN2 has its cmd.exe block'
}
$before = @(Get-AfterExitCopies $tmp)
Expect (-not (Get-AppWindow)) 'the app is closed before the uninstall'
Assert-ShellDesktop
$savedTmp = $env:TMP
$savedTemp = $env:TEMP
try {
    if ($unc) { $env:TMP = $env:TEMP = $tmp }
    [void](Start-Process -FilePath $l.Stub -ArgumentList '--uninstall')
} finally {
    $env:TMP = $savedTmp
    $env:TEMP = $savedTemp
}
Wait-ShellApp
Press-UninstallDialogs $script:ShellApp
Test-UninstallGone
$deferredOk = Wait-Until {
    $script:Deferred = @(Get-AfterExitCopies $tmp | Where-Object { $_ -notin $before })
    $script:Deferred.Count -eq 1 -and (Test-ShellCleanupValue (Join-Path $tmp $script:Deferred[0]))
} 45
if (-not $deferredOk) {
    "new temp copies in ${tmp}: $($script:Deferred -join ', ') (before: $($before -join ', '))"
    Get-CleanupRunOnce | ForEach-Object { "RunOnce: $_" }
}
Expect $deferredOk "$case left one new temp copy and scheduled that copy's cleanup"
Save-ShellCleanup $case (Join-Path $tmp $script:Deferred[0])
Done
