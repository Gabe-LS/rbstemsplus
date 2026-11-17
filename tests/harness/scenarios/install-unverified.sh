# scenario: Install Demucs v4 when the server's payload.json doesn't verify (changed after it was
# signed): "Download failed" says it couldn't be verified, not "check your connection"; nothing
# changed
# commit: 91baa8a
# tag: first
# compare: exact
# server: make unverified --bad-sig
setup() { fact_welcomed; }
steps() {
  launch
  settle
  click c.installPlusBtn
  next_dialog 5000; settle
  answer Install
  next_dialog 60000; settle              # Download failed (couldn't be verified)
  answer OK
  wait_for 10000 "c.spinner.anim=0"
  settle
  check 'cmp -s "$MODELDIR/hdemucs.onnx" "$H/pristine/demucs3_model/hdemucs.onnx"' "rekordbox's model untouched"
}
