# scenario: the Stems Cache light follows the bridge's own state while rekordbox runs: bridge-state
# rewritten (by a rename, as the bridge does) while the app is in front: still checking, not using
# this Mac (read-only), then using it again; each shows without coming back to the app
# commit: 12ec063
# tag: first
# compare: exact
setup() { fact_welcomed; fact_plus_on; fact_cache_on; fact_chosen 1 1; fact_config "rekordbox_model=1"; fact_engine 0002; mkdir -p "$LOGS"; own "$LOGS"; }
steps() {
  fact_rekordbox_open
  launch
  settle
  fact_bridge_state pending ""; settle 500
  fact_bridge_state unavailable read-only; settle 500
  fact_bridge_state available ""; settle 500
  fact_rekordbox_closed
}
