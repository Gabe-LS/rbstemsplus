# SYSTEM owns the one Program Files rename. The move is paired with a restore step so the
# normal user's assertion never needs elevation or a security prompt.
$ready = Join-Path (Get-UserLocal) "rbtest-pf4-$($VmRun.Run).json"
$moved = "$RbDir.rbtest-pf4-$($VmRun.Run)"
if ($VmRun.Step -eq 'move-version') {
    if (-not (Test-Path -LiteralPath $ready)) { Skip 'the positive helper-path step did not finish' }
    $valid = (Test-Path -LiteralPath $RbExe -PathType Leaf) -and (-not (Test-Path -LiteralPath $moved))
    Check $valid 'the original version exists and its fixture destination is unused'
    if (-not $valid) { Done }
    Rename-Item -LiteralPath $RbDir -NewName (Split-Path -Leaf $moved)
    Check ((-not (Test-Path -LiteralPath $RbDir)) -and (Test-Path -LiteralPath (Join-Path $moved 'rekordbox.exe'))) 'the version folder was moved'
} elseif (Test-Path -LiteralPath $moved) {
    Rename-Item -LiteralPath $moved -NewName (Split-Path -Leaf $RbDir)
    Check (Test-Path -LiteralPath $RbExe -PathType Leaf) 'the version folder was restored'
} else {
    if (-not (Test-Path -LiteralPath $ready)) { Skip 'no version was moved' }
    Check (Test-Path -LiteralPath $RbExe -PathType Leaf) 'the original version folder is in place'
}
if ($VmRun.Step -eq 'restore-version') {
    Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
}
Done
