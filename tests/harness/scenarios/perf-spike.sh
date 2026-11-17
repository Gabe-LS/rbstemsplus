# scenario: the G1 API spike (mac-core §9 G1.7), B's side of P6, P7 and P10 against mac-A's
# perf-start (P10.idle, P10.after7) and perf-playlist-5000 (P6, P7) in the same launch slot
# (G1.7, 2026-11-10, with the former abab.sh --scenarios-b): a core-linked TEST build's spike table (app/Spike)
# beside the app's window, fed through core_helper_line by a reader thread per helper run; the app
# idle, then a 5,000-track playlist chosen at full speed (playlist 50, P10.after7 after the first),
# then at 1 line/ms (51), PERF_ROUNDS times a launch (default 4, as perf-playlist-5000; the first
# of a launch printed apart, @first), with perf-playlist-5000's fake-helper lines. Only for a TEST
# build with the core (mac-A has no spike: setup fails there).
# tag: Prepare
# mode: perf
# metrics: P6 P7 P10.idle P10.after7
# P10's idle form waits PERF_IDLE_S after the start (60 s, as perf-start).
# The spike starts at `mark settled` (app/Probe: probeSpikeStartOnSettled): P1-P4 have only the
# app's window, as on mac-A; P10.idle, P6 and P7 come after it.
setup() {
  fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=200"; fact_engine 0002
  spike_on
  spike_scripts 50:5000 51:5000
}
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  mark "idle.start"
  top_idle "${PERF_IDLE_S:-60}" start
  mark "idle.start end"
  footprint_now idle
  local i
  for i in $(seq 1 "${PERF_ROUNDS:-4}"); do
    mark "P6 full $i"
    select_playlist 50
    CMD_WAIT_S=70 cmd quiet 1500 60000
    if [ "$i" = 1 ]; then footprint_now after7; fi
    mark "P6 paced $i"
    select_playlist 51
    CMD_WAIT_S=70 cmd quiet 1500 60000
  done
  mark "end"
  flush
}
