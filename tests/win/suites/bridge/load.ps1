# T7: the TEST bridge with Pioneer's onnxruntime.dll (the newest rekordbox in Program Files) as its
# real library: 1.18, hooked (bridge/win/tests/run.ps1's load); SKIP when rekordbox isn't installed
$run = Need 'bridge-win/run.ps1' 'bridge/win/tests/stage.sh'
. $run -Group load -Dir (Share 'bridge-win')
Done
