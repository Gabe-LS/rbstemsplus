# scenario: P11: the watcher per start: launch to exit, CPU time, peak footprint, and no watcher
# process left after its rounds; 6 starts a launch (PERF_ROUNDS)
# tag: first
# mode: perf
# metrics: P11
# The watcher's own probe writes watcher-<pid>.json at exit (app/Probe/Probe.swift, startWatcher).
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=20"; fact_engine 0002; }
steps() {
  local i
  for i in $(seq 1 "${PERF_ROUNDS:-6}"); do watcher_start; done
}
