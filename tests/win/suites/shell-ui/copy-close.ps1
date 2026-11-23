# WC8: request Close with the real model copy staged and CriticalWork still held.
$l = Get-Layout
$modelDir = Join-Path $env:APPDATA 'Pioneer\rekordbox6\models\demucs3_model'
$model = Join-Path $modelDir 'hdemucs.onnx'
$original = Get-Sha $model
$copy = ([string](Read-TestPayload).model.sha256).ToLowerInvariant()
if (-not $original -or $original -eq $copy) { Skip 'the snapshot needs an original model to replace' }
Expect ($copy -match '^[0-9a-f]{64}$') 'the served TEST payload names the model hash'
Expect ((Get-Rekordbox).Count -eq 0) 'rekordbox is closed'
Stop-App
$hold = "Local\rbsp-test-copy-$PID"
$held = [Threading.EventWaitHandle]::new($false, 'ManualReset', "$hold-held")
$release = [Threading.EventWaitHandle]::new($false, 'ManualReset', "$hold-release")
try {
    $env:RBSP_TEST_COPY_HOLD = $hold
    try { Start-AppQuiet } finally { Remove-Item Env:RBSP_TEST_COPY_HOLD }
    if (-not $script:app) { Done }
    $app = $script:app
    $null = $app.Handle
    Use-ShellUi invoke --pid $app.Id (Get-CoreText 'T_BTN_INSTALL_PLUS') --type Button --timeout 75
    Use-ShellUi invoke --pid $app.Id (Get-CoreText 'T_INSTALL') --type Button --timeout 60
    $ready = Wait-Until { Assert-ShellDesktop; $held.WaitOne(0) } 300
    if (-not $ready) { Skip 'the TEST native model copy never reached the staged hold' }
    Expect ((Get-Sha $model) -eq $original) 'the live model is still original at the staged hold'
    Expect ($app.CloseMainWindow()) 'Close posted to the app while its model copy is held'
    Start-Sleep -Seconds 2
    Expect (-not $app.HasExited -and (Get-AppWindow).Id -eq $app.Id) 'Close was refused and the same window survives the hold'
    Expect ((Get-Sha $model) -eq $original) 'the hold still precedes the rename'
    [void]$release.Set()
    # the action's end first, then one hash: Get-FileHash opens the live model sharing read only,
    # so hashing it while the copy renames over it made that rename fail ("Resource device")
    $finished = Wait-UiEnabled $app.Id (Get-CoreText 'T_BTN_UNINSTALL_PLUS') 90
    $copied = (Get-Sha $model) -eq $copy
    if (-not ($finished -and $copied)) {
        "the live model's SHA-256 now: $(Get-Sha $model); app.log's last lines:"
        Get-Content -LiteralPath (Join-Path $l.Support 'Logs\app.log') -Tail 30 -ErrorAction SilentlyContinue
    }
    Expect $finished 'the action is finished'
    Expect $copied 'the official model copy finished after release'
    Expect (@(Get-ChildItem -LiteralPath $modelDir -Force -Filter '.hdemucs.new*').Count -eq 0) 'no staged model file remains'
    Start-Sleep -Seconds 15
    Expect (-not $app.HasExited -and (Get-AppWindow).Id -eq $app.Id) 'the refused Close did not cause a delayed quit'
    Close-ShellApp $app
} finally {
    [void]$release.Set()
    $held.Dispose()
    $release.Dispose()
}
Done
