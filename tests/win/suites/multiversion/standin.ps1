# As SYSTEM. VMs never start rekordbox (owner, 2026-11-17), so V5's "each EXE loads the bridge
# from its own folder" uses a stand-in host: bridge/win's harness.exe (it imports onnxruntime.dll
# by ordinal only, as rekordbox.exe does), copied INTO each rekordbox folder as rbsp-standin.exe,
# so Windows resolves its onnxruntime.dll exactly as for that folder's rekordbox.exe (the
# application folder first). standin-put copies it in; standin-clean removes it again.
#   standin-put    both folders get rbsp-standin.exe (from the share's bridge-win\harness.exe)
#   standin-clean  both lose it, and nothing else is in the folders that wasn't there before
$put = $VmRun.Step -like '*standin-put'
$src = Need 'bridge-win/harness.exe' 'bridge/win/tests/stage.sh'
foreach ($v in $MvVersions) {
    $d = Get-RbDirOf $v
    if (-not (Test-Path -LiteralPath $d)) { "rekordbox $v isn't installed"; continue }
    $dst = Join-Path $d 'rbsp-standin.exe'
    if ($put) {
        Copy-Item -LiteralPath $src -Destination $dst -Force
        Check ((Get-Sha $dst) -eq (Get-Sha $src)) "the stand-in host in $d"
    } else {
        Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
        Check (-not (Test-Path -LiteralPath $dst)) "no stand-in host left in $d"
    }
}
if ($put) {
    $m = Need 'bridge-win/model/hdemucs.onnx' 'bridge/win/tests/stage.sh'
    New-Item -ItemType Directory -Force 'C:\rbtest\mv-model' | Out-Null
    Copy-Item -LiteralPath $m -Destination 'C:\rbtest\mv-model\hdemucs.onnx' -Force
    Check (Test-Path -LiteralPath 'C:\rbtest\mv-model\hdemucs.onnx') 'the harness test model copied in'
}
Done
