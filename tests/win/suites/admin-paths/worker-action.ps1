# Shared app actions for the worker regression suites. The guest's existing worker/prep.ps1
# sets promptless elevation for the reverted snapshot. rbui is scoped to the application PID.
# a prompt that stays: under the promptless policy consent.exe still starts for each elevation and
# ends within a moment, so a consent process seen once isn't a prompt; the same one 3 s later is
function Test-ConsentHeld {
    $first = @(Get-Process -Name 'consent' -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    if ($first.Count -eq 0) { return $false }
    Start-Sleep -Seconds 3
    return @(Get-Process -Name 'consent' -ErrorAction SilentlyContinue | Where-Object { $first -contains $_.Id }).Count -gt 0
}

function Assert-WorkerPromptless {
    $policy = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
    if ($policy.EnableLUA -ne 1 -or $policy.ConsentPromptBehaviorAdmin -ne 0) {
        throw 'The worker suite needs the promptless fixture policy from worker/prep.ps1'
    }
}

function Get-BinWorker([string]$sha) {
    if ($sha -notmatch '^[0-9a-f]{64}$') { throw "Invalid worker fixture hash: $sha" }
    return Join-Path $CacheRoot ('bin\' + $sha.Substring(0, 12) + '\RB Stems Plus.exe')
}

function Get-AppWorker {
    $layout = Get-Layout
    $version = (Get-Content -LiteralPath $layout.Current -Raw).Trim()
    return Join-Path $layout.Apps "$version\admin\RB Stems Plus.exe"
}

function Invoke-WorkerButton([int]$appId, [string]$text) {
    return (Invoke-Rbui invoke --pid $appId $text --type Button --timeout (Get-WaitSecs 30)) -eq 0
}

function Invoke-WorkerCacheAction([string]$button, [string]$confirm, [string]$result, [switch]$DismissFailure, [scriptblock]$settled) {
    Assert-WorkerPromptless
    $from = Get-AppLogSize
    $script:app = Get-AppWindow
    if (-not $script:app) { Start-AppQuiet }
    if (-not $script:app) { return }
    try {
        $pressed = Invoke-WorkerButton $script:app.Id (Get-CoreText $button)
        Check $pressed "the app's $button button pressed"
        if (-not $pressed) { return }
        $pressed = Invoke-WorkerButton $script:app.Id (Get-CoreText $confirm)
        Check $pressed "the app's $confirm confirmation pressed"
        if (-not $pressed) { return }
        $ended = Wait-Until { (Read-AppLog $from) -match $result -or (Test-ConsentHeld) } 150
        $consent = Test-ConsentHeld
        Check (-not $consent) 'Windows stayed off the consent desktop'
        if ($consent) { return }
        $log = Read-AppLog $from
        "app.log since the action:`n$log"
        Check ($ended -and $log -match $result) "app.log contains $result"
        if ($settled) { Check (Wait-Until $settled 45) 'the app committed the resulting Stems Cache choice' }
        if ($DismissFailure -and $ended) {
            Check (Invoke-WorkerButton $script:app.Id (Get-CoreText 'T_OK')) 'the app alert closed'
        }
    } finally {
        if (-not (Test-ConsentHeld)) { Stop-App }
    }
}
