# Faults (release.md section 8): a byte appended to the zip, to payload.json or to its signature
# (stage-dist.sh's dist-test/fault-zip, fault-json, fault-sig): each refused, nothing changed
$before = Get-AppTree
$any = $false
foreach ($f in @('zip', 'json', 'sig')) {
    $rel = "dist-test/fault-$f/payload.json"
    if (-not (Test-Path -LiteralPath (Share $rel))) { Missing "no ${rel} (stage-dist.sh): the $f fault"; continue }
    $any = $true
    $out = Invoke-OneLiner ($VmRun.Url + "fault-$f/")
    "fault-${f}: $out"
    $why = @{ zip = 'damaged'; json = "doesn't verify"; sig = "doesn't verify" }[$f]
    Check ($out -match "wasn't installed: .*$([regex]::Escape($why))") "a byte appended to the $f`: refused ($why)"
    Check ((Get-AppTree) -ceq $before) "the $f fault changed nothing"
}
if (-not $any) { Skip 'no fault copies in the share: tests/win/suites/stage-dist.sh' }
Stop-App
Done
