# T7 (review TR71, BW3's row): the TEST bridge with Pioneer's onnxruntime.dll, a chosen location on
# the share mapped as a network drive (net use \\Mac\win on a free letter, removed after):
# unavailable ("not-local", where 4b11e563's own check gives "not-connected"), nothing made on the
# share (bridge/win/tests/run.ps1's network); SKIP when rekordbox isn't installed or the mapping fails
$run = Need 'bridge-win/run.ps1' 'bridge/win/tests/stage.sh'
. $run -Group network -Dir (Share 'bridge-win')
Done
