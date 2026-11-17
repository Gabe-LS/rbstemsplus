# scenario: P3 (activation trigger) and P4: coming back to the app, 10 times a launch (PERF_ROUNDS)
# tag: first
# mode: perf
# metrics: P3 P4
# Each round: Finder in front, then the app activated from outside (VERB, default reopen: the
# Dock-click path), then quiet. P3 from trigger.activate to the next refresh.end; P4 to the last
# field written before the round's mark.
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=20"; fact_engine 0002; }
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  local i
  for i in $(seq 1 "${PERF_ROUNDS:-10}"); do
    finder_front
    CMD_WAIT_S=10 cmd quiet 500 5000
    activate
    CMD_WAIT_S=10 cmd quiet 800 8000
    mark "round $i"
  done
  flush
}
