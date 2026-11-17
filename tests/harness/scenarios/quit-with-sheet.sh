# scenario: Quit with an action's sheet open: under a main-thread confirmation (Install Demucs v4?)
# and under a work-queue alert ("Download failed", no server) the app menu's Quit is off, as every
# menu item is under an app-modal dialog (mac-core §3.7: "Under a dialog, Quit is disabled, as
# today"); once the dialog is answered, Quit quits
# commit: flow:quit-sheet
# tag: first
# compare: exact
setup() { fact_welcomed; }
refused() {
  if QUIET_FAIL=1 menu "menu.App.Quit RB Stems Plus"; then fail "Quit worked under $1"
  else check '[ "${ACK#err disabled}" != "$ACK" ]' "Quit is off under $1"; fi
}
steps() {
  launch
  settle
  click c.installPlusBtn
  next_dialog 5000; settle
  refused "the confirmation"
  check 'pgrep -x "RB Stems Plus" >/dev/null' "the app still runs"
  answer Install
  next_dialog 30000; settle              # Download failed, from the work queue
  refused "the work queue's alert"
  answer OK
  wait_for 10000 "c.spinner.anim=0"
  settle
  menu "menu.App.Quit RB Stems Plus"
  local i
  for i in $(seq 1 100); do pgrep -x "RB Stems Plus" >/dev/null || break; sleep 0.1; done
  check '! pgrep -x "RB Stems Plus" >/dev/null' "Quit quits once the dialog is answered"
}
