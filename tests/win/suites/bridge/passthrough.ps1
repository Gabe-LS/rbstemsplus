# T7: the TEST bridge with the 1.17.0 fake as its real library (bridge/win/tests/run.ps1's passthrough)
$run = Need 'bridge-win/run.ps1' 'bridge/win/tests/stage.sh'
. $run -Group passthrough -Dir (Share 'bridge-win')
Done
