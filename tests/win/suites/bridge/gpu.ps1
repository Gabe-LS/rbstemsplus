# T7 (review BW1): the TEST bridge with Pioneer's onnxruntime.dll, and harness run on the test model
# with a cached chunk's mix, then a pre-allocated x, marked as GPU memory: passed through, nothing
# stored (bridge/win/tests/run.ps1's gpu); SKIP when rekordbox isn't installed
$run = Need 'bridge-win/run.ps1' 'bridge/win/tests/stage.sh'
. $run -Group gpu -Dir (Share 'bridge-win')
Done
