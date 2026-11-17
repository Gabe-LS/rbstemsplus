# scenario: P12: the app idle with its window open, CPU (top), 5 minutes in the full form; one
# launch per side (perf-start's 60 s windows are P12.start)
# tag: first
# mode: perf
# metrics: P12
# PERF_P12_S: 300 s. Information only, not in the gate's plan (owner decided, 2026-11-12):
# ABAB_IDLE_WAIT=0 tests/perf/abab.sh LABEL A.zip --scenarios perf-idle -n 1 runs it (a smoke run).
# Its own variable: PERF_IDLE_S is P10's 60 s idle (perf-start), never P12's window.
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=20"; fact_engine 0002; }
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  mark "idle"
  top_idle "${PERF_P12_S:-300}"
  mark "idle end"
  flush
}
