# scenario: P14: the in-app update, from the confirmation's Update to the new app's window drawn
# (the build under test repacked with its patch number plus one, served locally); the two processes' records joined through their
# kernel start times (pids.txt, tests/perf/stats.py)
# tag: first
# mode: perf
# metrics: P14
# server: make update --app-version next
setup() { fact_welcomed; }
steps() {
  launch
  wait_for 30000 "c.updateItem.hid=0"
  CMD_WAIT_S=20 cmd quiet 1000 15000
  mark settled
  record_pids
  menu c.updateItem
  next_dialog 5000
  local before; before="$(traces | wc -l | tr -d ' ')"
  answer Update
  wait_new_trace "$before" 1800 || return 1
  wait_event 30000 windowDrawn 1
  record_pids
  CMD_WAIT_S=20 cmd quiet 1000 15000
  flush
}
