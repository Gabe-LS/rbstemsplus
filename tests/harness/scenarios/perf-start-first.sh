# scenario: perf-start's start without its Prepare part, for an app with only the Manage tab (the
# Windows app's WB plan, thresholds.txt's @launches WB): P1, P2, P10 idle 60 s after the start with
# P12.start and its wakeups (the CPU over that same window). No Prepare step; every event counts
# (no first-event split); the gated ones here are P1, P2 and P10.idle.
# tag: first
# mode: perf
# metrics: P1 P2 P10.idle P12.start
# P10's idle form waits IDLE_S after the start (60 s in the full form; PERF_IDLE_S overrides it
# for a smoke run). perf-start's facts; its playlist scripts are left out (no Prepare step reads them).
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=200"; fact_engine 0002; }
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  mark "idle.start"
  top_idle "${PERF_IDLE_S:-60}" start
  mark "idle.start end"
  footprint_now idle
  mark "end"
  flush
}
