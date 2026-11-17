# scenario: P16: Create Report masking a 30-minute log (the app's log as half an hour of a session
# leaves it), from the menu's command to "Report saved" (tests/perf/stats.py), 5 times a launch
# (PERF_ROUNDS); the app's rusage before the command and after the dialog: P16.own leaves out the
# report's waits on its children (log show, codesign, ditto…), whose time follows the host's load
# tag: first
# mode: perf
# metrics: P16
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1"; fact_engine 0002; fact_long_log; }
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  local i
  for i in $(seq 1 "${PERF_ROUNDS:-5}"); do
    mark "report $i"
    rusage_now "report-$i-before"
    menu "menu.Help.Create Report"
    next_dialog 120000
    rusage_now "report-$i-after"
    answer Done
    wait_no_dialog 5000
    rm -f "$UH/Desktop/RB Stems Plus report "*.zip
    CMD_WAIT_S=10 cmd quiet 500 8000
  done
  flush
}
