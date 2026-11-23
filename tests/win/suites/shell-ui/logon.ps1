# K4, UN2, UN6: each of the three real after-exit copies must disappear at the same logon.
# (not $dirs = foreach ...: that would also collect Expect's PASS lines as folders)
$dirs = @()
foreach ($case in 'un2', 'un6', 'k4') {
    $file = Join-Path $ShellWork "$case.txt"
    Expect (Test-Path -LiteralPath $file) "$case recorded its exact temp folder"
    $dirs += (Get-Content -Raw -LiteralPath $file).Trim()
}
"recorded: $($dirs -join ' | ')"
Expect (@($dirs | Sort-Object -Unique).Count -eq 3) 'the three uninstall rounds recorded different folders'
Expect (Test-Path -LiteralPath $ShellUnc -PathType Container) 'the UNC share is reachable after logon'
foreach ($dir in $dirs) {
    Check (Wait-Until { -not (Test-Path -LiteralPath $dir) } 60) "RunOnce removed $dir at logon"
    $name = (Split-Path -Leaf $dir).Substring(15)
    Check (-not (@(Get-CleanupRunOnce) -match "^RB Stems Plus cleanup $name = ")) "no RunOnce value for $dir remains"
}
Done
