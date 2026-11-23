# Part B3: a payload whose windows.app.version isn't the zip's app (signed with the test key,
# served from bad-version/ of the served TEST build's folder: dist-test/, or the run's --serve DIR,
# made there by stage-dist.sh): refused by Test-RbspAppVersion, the apps folder as it was. Needs a
# TEST build whose bootstrap.ps1 has Test-RbspAppVersion (w6/win-bootstrap-app)
$pj = Get-Served 'bad-version/payload.json'
if ($null -eq $pj) { Skip 'the served folder has no bad-version/payload.json (stage-dist.sh: a payload naming another app version, signed with the test key)' }
if (-not (Test-Served 'bad-version/payload.json.sig')) { Skip "the served folder has no bad-version/payload.json.sig (the bad-version payload's signature)" }
$bs = Get-Served 'bad-version/bootstrap.ps1'
if ($null -eq $bs) { Skip 'the served folder has no bad-version/bootstrap.ps1 (the TEST build bootstrap.ps1 the one-liner fetches from bad-version/)' }
$zip = [string]($pj | ConvertFrom-Json).windows.app.zip
if (-not $zip -or $zip -match '[\\/]' -or $zip.StartsWith('.')) { Check $false "bad-version's payload.json names a plain windows.app.zip (got '$zip')"; Done }
if (-not (Test-Served "bad-version/$zip")) { Skip "the served folder has no bad-version/$zip (the zip the bad-version payload names)" }
if (-not $bs.Contains('Test-RbspAppVersion')) { Skip "bad-version/bootstrap.ps1 has no Test-RbspAppVersion: a TEST build with w6/win-bootstrap-app is needed" }
$before = Get-AppTree
$out = Invoke-OneLiner ($VmRun.Url + 'bad-version/')
"got: $out"
Check ($out -match "wasn't installed: the app's version \(") "refused: the app's version isn't the payload's"
Check ((Get-AppTree) -ceq $before) 'the apps folder as it was'
Stop-App
Done
