# scenario: one copy at a time: a second copy started while the app is open (open -n) waits for
# the lock, then shows "Already open" in a small window of its own (its own probe records it: the
# second trace) and quits on OK; the open window is left as it was
# commit: 13fff35
# tag: first
# compare: exact
# The second copy reads the probe's command FIFO too, so nothing is sent while it runs: its
# dialog is waited for in its record, and it is ended by SIGTERM, as the dialog's OK would (it only
# exits). Then the open app is brought back from outside (Finder first): which app macOS puts in
# front when the second copy quits isn't the app's doing.
setup() { fact_welcomed; }
steps() {
  launch
  settle
  local first; first="$(app_pid)"
  as_user open -n "$APP"
  local i second=""
  for i in $(seq 1 400); do
    second="$(traces | tail -1)"
    [ "$second" != "$TRACE" ] && grep -q '"e":"dialog"' "$second" 2>/dev/null && break
    sleep 0.05
  done
  check 'grep -q "Already open" "$second"' "the second copy says Already open"
  sleep 0.5                                # its window drawn and recorded
  local p; for p in $(pgrep -x "RB Stems Plus"); do [ "$p" != "$first" ] && kill "$p"; done
  for i in $(seq 1 100); do [ "$(pgrep -x "RB Stems Plus" | wc -l | tr -d ' ')" = 1 ] && break; sleep 0.1; done
  check '[ "$(pgrep -x "RB Stems Plus")" = "$first" ]' "only the first copy runs"
  finder_front
  VERB=reopen activate
  settle
}
