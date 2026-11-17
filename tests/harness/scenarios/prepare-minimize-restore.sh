# scenario: the window miniaturized and restored: the tab in front checks again (quietly); a run
# that ends out of sight drops the old estimate
# commit: d8d5e64, 1ab82da
# tag: Prepare
# compare: exact
# allow: prepare.prepareBtn.en prepare.info.str
# (a run's last lines reach the window either one by one (the pipe's handler) or together (the
# termination handler): A shows one intermediate frame of these two more or fewer at the run's
# end; they are compared by final values and order)
# (no no-checking-flash property: a run that ended out of sight drops the old estimate, so restoring
# the window checks visibly, by design (d8d5e64); the quiet check is prepare-come-back-quiet's)
setup() { fixture_prepare_ready; fixture_library; estimate_friday; run_friday run; }
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  select_playlist 2
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
  miniaturize; settle
  deminiaturize; settle
  check '[ "$(grep -c "estimate-2" "$FAKE/log/argv.log")" -ge 2 ]' "restoring the window checked again"
  click prepare.prepareBtn
  wait_for 5000 "prepare.pauseBtn.hid=0"
  for i in 1 2 3 4; do helper_go "run$i"; settle 200; done
  miniaturize; settle
  helper_go run5                        # the run ends out of sight
  wait_for 8000 "prepare.prepareBtn.hid=0"
  settle
  deminiaturize; settle
}
