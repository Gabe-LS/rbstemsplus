# scenario: Stop during a run (the menu's Stop): the helper ends its chunk and writes its set
# (SIGTERM), the summary says stopped, the last run line says "stopped before the end"
# commit: d934b51, 9e34800, 38a4cbd
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
  helper_go run1; settle 250
  helper_go run2; settle 250
  estimate_friday
  menu "menu.Prepare.Stop"
  wait_for 8000 "prepare.prepareBtn.hid=0"
  settle
}
