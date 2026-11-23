# Faults (release.md section 8): a byte appended to the zip, to payload.json or to its signature
# (stage-dist.sh's fault-zip, fault-json, fault-sig in the served TEST build's folder: dist-test/,
# or the run's --serve DIR when stage --serve DIR made them there): each refused, nothing changed
$before = Get-AppTree
$any = $false
foreach ($f in @('zip', 'json', 'sig')) {
    $rel = "fault-$f/payload.json"
    if (-not (Test-Served $rel)) { Missing "the served folder has no ${rel} (stage-dist.sh): the $f fault"; continue }
    $any = $true
    $out = Invoke-OneLiner ($VmRun.Url + "fault-$f/")
    "fault-${f}: $out"
    $why = @{ zip = 'damaged'; json = "doesn't verify"; sig = "doesn't verify" }[$f]
    Check ($out -match "wasn't installed: .*$([regex]::Escape($why))") "a byte appended to the $f`: refused ($why)"
    Check ((Get-AppTree) -ceq $before) "the $f fault changed nothing"
}
if (-not $any) { Skip 'no fault copies in the served folder: tests/win/suites/stage-dist.sh (vm-run.sh stage --serve DIR)' }
Stop-App
Done
