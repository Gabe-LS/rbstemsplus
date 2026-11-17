# T7 (review BX6; a regression row for the reparse refusal, BW3's row is network.ps1): the TEST
# bridge with Pioneer's onnxruntime.dll, a chosen location on the vhd step's NTFS VHDX whose final
# component is a junction to a folder on C:: unavailable ("folder"), nothing written through it
# (bridge/win/tests/run.ps1's junction); SKIP when rekordbox isn't installed or the VHDX isn't there
$run = Need 'bridge-win/run.ps1' 'bridge/win/tests/stage.sh'
. $run -Group junction -Dir (Share 'bridge-win')
Done
