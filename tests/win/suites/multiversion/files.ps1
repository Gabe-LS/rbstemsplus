# As SYSTEM, after a batch (its name: this step's before "-files"): the batch's UAC count (the
# elevations the Security log saw, app.log's admin runs beside them), and Stems Cache's root as the
# batch should have left it: a receipt per covered version (receipts\<X.Y.Z>), Pioneer's original
# saved per version (pioneer\<X.Y.Z>\onnxruntime.dll, its audited hash), ort\onnxruntime.dll
# 7.2.19's bytes (the one shared runtime, mv-decisions D2/D2a), each covered folder's
# onnxruntime.dll the bridge and each other folder's Pioneer's.
#   v1-files: covered {7.2.19}                v5-files: {7.2.19, 7.2.20}
#   v7-files: {7.2.19} (partial)              v9p-files: {7.2.20} (Put Back of 7.2.19)
#   v9i-files: {7.2.19, 7.2.20} (Install again)
$batch = $VmRun.Step -replace '-files$', ''
$cover = switch -Regex ($batch) { '^(v1|v7)$' { @('7.2.19') } '^v9p$' { @('7.2.20') } default { @('7.2.19', '7.2.20') } }
$want = 1
Test-MvUac $batch $want
$root = $CacheRoot
$b = Get-ServedBridge
Show-MvFolders $b
foreach ($v in $MvVersions) {
    if (-not (Test-Path -LiteralPath (Get-RbDirOf $v))) { "rekordbox $v isn't installed"; continue }
    $live = Get-Sha (Get-RbOrtOf $v)
    $rec = Join-Path $root "receipts\$v"
    $saved = Join-Path $root "pioneer\$v\onnxruntime.dll"
    if ($cover -contains $v) {
        Check ($live -eq $b) "rekordbox $v's onnxruntime.dll is the bridge"
        Check (Test-Path -LiteralPath $rec -PathType Leaf) "its receipt receipts\$v"
        if (Test-Path -LiteralPath $rec -PathType Leaf) { foreach ($x in (Get-Content -LiteralPath $rec)) { "  receipts\$($v): $x" } }
        Check ((Get-Sha $saved) -eq $MvOrtSha[$v]) "Pioneer's original saved as pioneer\$v\onnxruntime.dll ($(Get-Sha $saved))"
    } else {
        Check ($live -eq $MvOrtSha[$v]) "rekordbox $v's onnxruntime.dll is Pioneer's ($live)"
        Check (-not (Test-Path -LiteralPath $rec)) "no receipt for $v"
    }
}
Check ((Get-Sha (Join-Path $root 'ort\onnxruntime.dll')) -eq $MvOrtSha['7.2.19']) "ort\onnxruntime.dll is 7.2.19's bytes (the shared runtime)"
"root: $(@(Get-ChildItem -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($root.Length) }) -join '; ')"
$stray = @(Get-ChildItem -LiteralPath (Join-Path $env:ProgramFiles 'rekordbox') -Recurse -Force -Filter '*.rbsp-*' -ErrorAction SilentlyContinue)
Check ($stray.Count -eq 0) "no stray .rbsp-* file in the rekordbox folders ($(@($stray | ForEach-Object FullName) -join ', '))"
Done
