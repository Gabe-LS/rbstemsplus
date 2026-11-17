# scenario: the G1 API spike at 20,000 rows (API-V1-NOTES open point 8: the spike measures 5k and
# 20k rows with the previous snapshot held): perf-spike's table and helper-line path, a
# 20,000-track playlist chosen, its estimate at full speed (playlist 150), then at 1 line/ms
# (151). mac-A has no 20,000-row scenario: read these runs with `tests/perf/stats.py samples
# RUNDIR` (P6, P7.*), never pooled with perf-spike's in one compare. TEST builds with the core only.
# tag: Prepare
# mode: perf
# metrics: P6 P7 P10.after7 (20,000 rows: reported, no A to compare with)
setup() {
  fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=200"; fact_engine 0002
  spike_on
  spike_scripts 150:20000 151:20000
  echo 20000 > "$RUN/p6-rows"            # stats.py's P6 ends at the reload with all 20,000 rows
}
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled                          # the spike starts here (app/Probe: probeSpikeStartOnSettled)
  CMD_WAIT_S=20 cmd quiet 1500 15000    # its start's turn and window, before P6's select
  mark "P6 full 20k"
  select_playlist 150
  CMD_WAIT_S=70 cmd quiet 1500 60000
  footprint_now after7
  mark "P6 paced 20k"
  select_playlist 151
  CMD_WAIT_S=100 cmd quiet 1500 90000
  mark "end"
  flush
}
