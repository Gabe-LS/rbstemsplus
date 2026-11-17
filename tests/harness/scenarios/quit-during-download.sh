# scenario: Quit (the app menu's Quit, as Cmd+Q) during Install Demucs v4's model download (a
# slow local server): the download isn't a step that changes rekordbox, so the quit doesn't wait;
# the busy marker stays, and the next launch offers to finish the install ("Action interrupted"),
# Cancel drops it; rekordbox's model is untouched
# commit: flow:quit-download, 1c0ee4e
# tag: first
# compare: properties
# server: dist-test --rate 15000
# allow: c.status.str
# mask: … \d+%
# (the download's percentages depend on the transfer: masked, and the status line by its final
# value and as a subsequence of A's; every other field exactly)
setup() { fact_welcomed; }
steps() {
  launch
  settle
  click c.installPlusBtn
  next_dialog 5000; settle
  answer Install
  wait_for 10000 "c.spinner.anim=1"
  local i
  for i in $(seq 1 300); do ls "$SUP"/payload/*/*.part >/dev/null 2>&1 && break; sleep 0.1; done
  check 'ls "$SUP"/payload/*/*.part >/dev/null 2>&1' "the model's download started"
  sleep 3                                 # well inside the download (about 20 s at this rate)
  menu "menu.App.Quit RB Stems Plus"
  for i in $(seq 1 100); do pgrep -x "RB Stems Plus" >/dev/null || break; sleep 0.1; done
  check '! pgrep -x "RB Stems Plus" >/dev/null' "the app quit at once"
  check 'grep -q "install-plus" "$SUP/busy"' "the busy marker names the install"
  check 'cmp -s "$MODELDIR/hdemucs.onnx" "$H/pristine/demucs3_model/hdemucs.onnx"' "rekordbox's model untouched"
  launch
  next_dialog 10000; settle              # Action interrupted
  answer Cancel; wait_no_dialog 5000; settle
  check '[ ! -e "$SUP/busy" ]' "Cancel dropped the busy marker"
}
