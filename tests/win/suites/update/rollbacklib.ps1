# The updater writes UTF-8 without a BOM and uses LF even on Windows (update_pending_format).
function New-RollbackFixture($layout, [string]$from, $release, [string]$source, [uint32]$parentId, [long]$started) {
    $to = $release.windows.app.version
    $zipName = $release.windows.app.zip
    $sha = $release.windows.app.sha256
    $version = '^(?:0|[1-9][0-9]{0,8})(?:\.(?:0|[1-9][0-9]{0,8})){2}$'
    Expect ($from -match $version -and $to -match $version -and [version]$to -gt [version]$from) 'the rollback fixtures name a newer app'
    Expect ($zipName -match '^[A-Za-z0-9_-][A-Za-z0-9._-]*\.zip$' -and $sha -match '^[a-f0-9]{64}$') 'the candidate payload names a plain zip and SHA-256'
    $zip = Join-Path $source $zipName
    Expect ((Get-FileHash -LiteralPath $zip).Hash -eq $sha) 'the candidate zip matches its payload'

    $folder = Join-Path $layout.Apps $to
    $records = Join-Path $layout.Support 'update'
    Expect (-not (Test-Path -LiteralPath $folder)) 'no staged candidate folder exists yet'
    foreach ($name in 'pending', 'started', 'result') {
        Expect (-not (Test-Path -LiteralPath (Join-Path $records $name))) "no stale $name record"
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $folder
    $exe = Join-Path $folder 'RB Stems Plus.exe'
    Expect (Test-Path -LiteralPath $exe -PathType Leaf) 'the extracted candidate has its shell'
    New-Item -ItemType Directory -Path $records -Force | Out-Null
    foreach ($name in 'payload.json', 'payload.json.sig') {
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination $records
    }
    [IO.File]::WriteAllText((Join-Path $records 'pending'), "$from`n$to`n$parentId`n$started`n", [Text.Encoding]::ASCII)
}

function Confirm-RollbackResult($app, [string]$why, [string]$result) {
    $title = Get-CoreText 'T_UPDATE_FAILED'
    $message = (Get-CoreText 'T_UPDATE_FAILED_MSG').Replace('{why}', $why)
    Expect (Find-UiText $app.Id ([regex]::Escape($message)) 60) "the recovered app said: $message"
    Use-UpdateUi invoke --pid $app.Id --title $title (Get-CoreText 'T_OK') --type Button --timeout 20
    Check (-not (Test-Path -LiteralPath $result)) 'the update result was consumed'
}
