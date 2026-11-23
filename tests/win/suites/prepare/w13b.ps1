# Run the existing W13b driver with staged TEST inputs on the user's desktop.
$part = $VmRun.Step -replace '^w13b-', ''
$native = [string](Get-TestArchs | Select-Object -First 1)
$build = if ($VmRun.TestArch -eq 'x64') { 'app-x64-test' } else { 'app-test' }
$src = Share "vm/shell/$build"
if (-not (Test-Path -LiteralPath (Join-Path $src 'RB Stems Plus.exe'))) { Skip "W13b needs the staged $build shell" }
$layout = Get-Layout
$probe = Join-Path $layout.Local 'rbstemsplus-probe\probe.conf'
if (Test-Path -LiteralPath $probe) { Skip 'W13b needs a clean probe profile' }
if (Get-Process 'RB Stems Plus' -ErrorAction SilentlyContinue) { Skip 'W13b needs RB Stems Plus closed' }

$app = Join-Path $env:LOCALAPPDATA "Programs\RB Stems Plus\w13b-$($VmRun.Run)-$part"
$out = Join-Path $env:TEMP "rbsp-w13b-$($VmRun.Run)-$part"
$results = Join-Path $VmRun.Results $VmRun.Step
$welcome = Join-Path $layout.Support 'welcomed'
$seedWelcome = -not (Test-Path -LiteralPath $welcome)
try {
    New-Item -ItemType Directory -Path $app -Force | Out-Null
    Get-ChildItem -LiteralPath $src | Copy-Item -Destination $app -Recurse
    if ($seedWelcome) {
        New-Item -ItemType Directory -Path $layout.Support -Force | Out-Null
        [IO.File]::WriteAllText($welcome, '')
    }
    $runner = Need 'repo/win/shell/vm/w13b.ps1' 'W13b runner'
    $trace = Need 'repo/tests/harness/traces/A/prepare-choose-estimate.jsonl' 'Mac A folder selection'
    $rbui = Need 'vm/uia/rbui/rbui.exe' 'FlaUI runner'
    & $runner -App $app -Out $out -Results $results -Part $part -Rbui $rbui -MacTrace $trace -Native $native
    $rc = $LASTEXITCODE
    Check ($rc -eq 0) "W13b $part driver exited $rc"
} finally {
    if ($seedWelcome) { Remove-Item -LiteralPath $welcome -ErrorAction SilentlyContinue }
    foreach ($dir in $app, $out) {
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    }
}
Done
