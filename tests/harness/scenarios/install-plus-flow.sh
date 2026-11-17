# scenario: Install Demucs v4 from the TEST build's local server (no password), then Uninstall
# Demucs v4 (rekordbox's own model back, no password): the Manage tab through both actions, the
# lights, the buttons, the status line and the log's progress
# commit: flow:install, flow:uninstall, 23403a9, 0a63096, 90b6955
# tag: first
# compare: properties
# server: dist-test
# allow: c.status.str
# mask: Downloading the Demucs v4 model… \d+%
# must-show: c.spinner.anim=1>0>1>0
# must-show: c.plusIcon.img=off>on>off
# The download's progress lines depend on the transfer's timing: the status line is compared by
# its final value and as a subsequence of A's (compare: properties, for that field only); every
# other field exactly; the spinner and the light must show both actions.
setup() { fact_welcomed; }
steps() {
  launch
  settle
  click c.installPlusBtn
  next_dialog 5000; settle
  answer Install
  wait_for 300000 "c.plusIcon.img=on"
  wait_for 30000 "c.spinner.anim=0"
  settle
  check '[ -f "$SUP/installed" ] && grep -q "model=1" "$SUP/installed"' "Demucs v4 chosen"
  click c.uninstallPlusBtn
  next_dialog 5000; settle
  answer Uninstall
  wait_for 60000 "c.plusIcon.img=off"
  wait_for 30000 "c.spinner.anim=0"
  settle
}
