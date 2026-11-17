# T7: the TEST bridge with no real library: the refusal stub (bridge/win/tests/run.ps1's missing)
$run = Need 'bridge-win/run.ps1' 'bridge/win/tests/stage.sh'
. $run -Group missing -Dir (Share 'bridge-win')
Done
