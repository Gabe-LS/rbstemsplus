# scenario: a run's lines while a main-thread dialog is up: during Friday's run, the Status tab's
# Stop Keeping confirmation for another prepared playlist (Warm-up); the run's progress goes on
# under it (trace.py dialogs: what changed under it), Cancel, the run ends
# commit: dialog:under-run, 38a4cbd
# tag: Prepare
# compare: properties
# allow: prepare.prepareBtn.en prepare.info.str status.table.r0
# under-dialog: prepare.tracksTable prepare.bar status.runLine
# (a run's last lines reach the window one by one or together: one intermediate frame more or
# fewer of the first two at the run's end, as in prepare-run; the Status tab's row shows the set's
# name before or after its check's "Checking…" by timing: by final values and as a subsequence)
setup() {
  fixture_prepare_ready; fixture_library; estimate_friday; run_friday run
  fact_kept 3 4 "$(track warmup-a)" "$(track warmup-b)"
  helper estimate-3 <<EOF
{"estimate":2,"ready":2,"prepare":0,"fits":true,"fit_files":2,$ESTIMATE_END}
EOF
}
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
  tab Status
  wait_for 5000 "status.table.rows=1"
  settle
  cmd select-set 0; settle
  click status.stopBtn
  next_dialog 5000; settle               # Stop keeping "Warm-up"?
  helper_go run2; settle 250
  helper_go run3; settle 250
  answer Cancel; wait_no_dialog 5000; settle
  helper_go run4; settle 250
  estimate_friday
  helper_go run5
  wait_for 8000 "prepare.prepareBtn.hid=0"
  settle
}
