# UP12: give the installed app a result from a stopped swap. The first startup must surface
# and consume it, and the next rendered, interactive startup must not repeat it.
$l = Get-Layout
$dir = Join-Path $l.Support 'update'
$result = Join-Path $dir 'result'
foreach ($name in 'pending', 'started', 'result') {
    Expect (-not (Test-Path -LiteralPath (Join-Path $dir $name))) "no stale $name record"
}
New-Item -ItemType Directory -Path $dir -Force | Out-Null
[IO.File]::WriteAllText($result, "failed swap`n", [Text.Encoding]::ASCII)
[void](Start-Process -FilePath $l.Stub)
Wait-UpdateApp
Confirm-RollbackResult $UpdateApp (Get-CoreText 'T_UPDATE_R_SWAP') $result
Stop-App
[void](Start-Process -FilePath $l.Stub)
Wait-UpdateApp
$title = [regex]::Escape((Get-CoreText 'T_UPDATE_FAILED'))
Check (-not (Find-UiText $UpdateApp.Id "^$title$" 10)) 'the next launch did not repeat the update failure dialog'
Test-UpdateAbout $UpdateApp (Get-UpdateRelease 'a').payload_version
Stop-App
Done
