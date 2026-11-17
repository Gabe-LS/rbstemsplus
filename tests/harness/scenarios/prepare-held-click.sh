# scenario: Prepare clicked while the quiet re-check (coming back) runs waits for it, shows
# "Checking…", and fires once the new estimate leaves the button on, in front and with no sheet
# commit: 61a5393, 5764bd4
# tag: Prepare
# compare: exact
# allow: prepare.prepareBtn.en prepare.info.str
# must-show: prepare.prepareBtn.en=0>1>0
# must-show: prepare.info.str=Checking the playlist…
# (a run's last lines reach the window either one by one (the pipe's handler) or together (the
# termination handler): A shows one intermediate frame of these two more or fewer at the run's
# end; they are compared by final values and order)
# verbs: both
setup() { fixture_prepare_ready; fixture_library; estimate_friday; run_friday run; }
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  select_playlist 2
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
  finder_front
  estimate_friday back
  activate
  settle
  click prepare.prepareBtn             # held: the check under way decides
  settle
  check '! grep -q "run-2" "$FAKE/log/argv.log"' "no run before the new estimate"
  for i in 1 2 3 4; do helper_go "back$i"; settle 250; done
  wait_for 5000 "prepare.pauseBtn.hid=0"     # the held click fired: the run is going
  settle
  click prepare.stopBtn
  wait_for 8000 "prepare.prepareBtn.hid=0"
  settle
  check 'grep -q "run-2" "$FAKE/log/argv.log"' "the held Prepare started the run"
}
