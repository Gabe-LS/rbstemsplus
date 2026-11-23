# V7, from two stock folders. The TEST fault is inherited by the elevated worker from HKCU.
$name = $VmRun.Step
Open-App
if (-not $script:app) { Done }
$app = $script:app
Clear-MvOffer $app.Id 3 -Fail

if ($name -in 'v7-reset', 'v7-remove', 'v8-reset') {
    Invoke-AppButton 'T_BTN_UNINSTALL_CACHE' 'T_UNINSTALL'
    Check ($script:Pressed -and (Wait-Until {
        $stock = @($MvVersions | Where-Object { (Get-Sha (Get-RbOrtOf $_)) -eq $MvOrtSha[$_] })
        $stock.Count -eq $MvVersions.Count
    } 120)) 'Uninstall Stems Cache leaves both folders stock'
    Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_BTN_INSTALL_CACHE') 120) 'the app is idle and Install Stems Cache is enabled'
    Done
}

Start-MvBatch $name
$from = [long](Read-MvValue "$name.from")
if ($name -eq 'v7-retry') {
    Invoke-AppButton 'T_BTN_INSTALL_CACHE' 'T_INSTALL'
    Test-MvRows $app.Id @((Rx (Get-SummaryText 2 2)), (Rx (Get-RowText 'T_WIN_ROW_ON' '7.2.20'))) 120
    $runs = Stop-MvBatch $name
    $log = Read-AppLog $from
    Check ($script:Pressed -and $runs -eq 1 -and $log -match '@target 7\.2\.20 0 installed' -and $log -notmatch '@target 7\.2\.19 ') 'retry installed only 7.2.20 in one admin run'
    Done
}

$fault = 'RBSP_TEST_WORKER_FAIL_TARGET'
Set-ItemProperty -LiteralPath 'HKCU:\Environment' -Name $fault -Value '7.2.20' -Type String
try {
    Invoke-AppButton 'T_BTN_INSTALL_CACHE' 'T_INSTALL'
    $message = (Get-CoreText 'T_WIN_CACHE_PARTIAL_INSTALLED').Replace('{done}', '7.2.19').Replace('{failed}', '7.2.20').Replace('{why}', (Get-CoreText 'T_ROOT_ROLLED_BACK'))
    Check ($script:Pressed -and (Wait-Until { (Get-MvNames $app.Id) -contains $message } 120)) "partial-install alert reads: $message"
    Save-MvShot 'partial-install'
    Check (Invoke-MvDialogButton $app.Id (Rx $message) (Get-CoreText 'T_OK') 30) 'the partial-install alert answered OK'
} finally {
    Remove-ItemProperty -LiteralPath 'HKCU:\Environment' -Name $fault -ErrorAction SilentlyContinue
}
$runs = Stop-MvBatch $name
$log = Read-AppLog $from
Check ($runs -eq 1 -and $log -match 'FAILED: install \[exit 16\]') 'one worker reports exit 16'
Check ($log -match '@target 7\.2\.19 0 installed' -and $log -match '@target 7\.2\.20 6 put-back') 'worker records 7.2.19 installed and 7.2.20 rolled back'
Test-MvRows $app.Id @((Rx (Get-SummaryText 1 2)), (Rx (Get-RowText 'T_WIN_ROW_ON' '7.2.19')), (Rx (Get-RowText 'T_WIN_ROW_READY' '7.2.20'))) 120
Done
