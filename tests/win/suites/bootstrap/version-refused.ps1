# Part B3: a payload whose windows.app.version isn't the zip's app (signed with the test key,
# served from dist-test/bad-version/): refused by Test-RbspAppVersion, the apps folder as it was.
# Needs a TEST build whose bootstrap.ps1 has Test-RbspAppVersion (w6/win-bootstrap-app), and the
# fixture made by hand from it: bad-version/ with that bootstrap.ps1, the payload.json, its .sig
# and the zip it names (nothing stages it)
$pj = Need 'dist-test/bad-version/payload.json' 'a payload naming another app version, signed with the test key'
[void](Need 'dist-test/bad-version/payload.json.sig' "the bad-version payload's signature")
$bs = Need 'dist-test/bad-version/bootstrap.ps1' 'the TEST build bootstrap.ps1 the one-liner fetches from bad-version/'
$zip = [string](Get-Content -Raw -LiteralPath $pj | ConvertFrom-Json).windows.app.zip
if (-not $zip -or $zip -match '[\\/]' -or $zip.StartsWith('.')) { Check $false "bad-version's payload.json names a plain windows.app.zip (got '$zip')"; Done }
[void](Need "dist-test/bad-version/$zip" 'the zip the bad-version payload names')
if (-not ([IO.File]::ReadAllText($bs)).Contains('Test-RbspAppVersion')) { Skip "bad-version/bootstrap.ps1 has no Test-RbspAppVersion: a TEST build with w6/win-bootstrap-app is needed" }
$before = Get-AppTree
$out = Invoke-OneLiner ($VmRun.Url + 'bad-version/')
"got: $out"
Check ($out -match "wasn't installed: the app's version \(") "refused: the app's version isn't the payload's"
Check ((Get-AppTree) -ceq $before) 'the apps folder as it was'
Stop-App
Done
