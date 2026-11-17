# scenario: a run: the rows stay the run's tracks, the progress line names the track and the bar
# shows how far, Pause/Resume and Stop take the buttons' place (nothing below moves), the Status
# tab shows the run's progress, the last run's line under the next estimate, each prepared
# track's note says whether it was separated now or saved already
# commit: 236a1c0, 5db560f, 6c7380f, d934b51, e47d1aa, 64325ad, b606a39
# tag: Prepare
# compare: exact
# allow: prepare.prepareBtn.en prepare.info.str
# (a run's last lines reach the window either one by one (the pipe's handler) or together (the
# termination handler): A shows one intermediate frame of these two more or fewer at the run's
# end; they are compared by final values and order)
setup() { fixture_prepare_ready; fixture_library; estimate_friday; run_friday run; }
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  select_playlist 2
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
  click prepare.prepareBtn
  wait_for 5000 "prepare.pauseBtn.hid=0"
  settle
  helper_go run1; settle 250
  helper_go run2; settle 250
  click prepare.pauseBtn; settle        # Pause: the menu says Resume
  click prepare.pauseBtn; settle        # Resume
  helper_go run3; settle 250
  tab Status; settle                    # the run's row and line on the Status tab
  helper_go run4; settle 250
  tab Prepare; settle
  estimate_friday                       # what is ready after the run
  helper_go run5
  wait_for 8000 "prepare.prepareBtn.hid=0"
  settle
  check 'grep -q "run-2" "$FAKE/log/argv.log"' "the run was started"
}
