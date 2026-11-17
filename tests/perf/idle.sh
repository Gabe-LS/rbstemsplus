# shellcheck shell=bash
# The idle guard of the performance sessions (mac-core §2.2: the Mac left alone):
# tests/perf/abab.sh (the app, in tart) and bridge/tests/perf.sh gate (on the host). Sourced.
#   idle_secs              the seconds since the last keyboard, mouse or trackpad input (ioreg's
#                          HIDIdleTime, read to its end: no SIGPIPE into ioreg)
#   idle_try               sets idle to that; returns 1 when the reading isn't a number
#   idle_wait SECS WHY LOG until the Mac has had no input for SECS (a reading every
#                          PERF_IDLE_POLL seconds, 15); a line into LOG when it has to wait;
#                          returns 1 when a reading isn't a number (the caller stops)
# PERF_IDLE_CMD replaces the reader: the selftests' stub (the bridge's real gate refuses it).

idle_secs() {
  if [ -n "${PERF_IDLE_CMD:-}" ]; then "$PERF_IDLE_CMD"
  else ioreg -c IOHIDSystem | awk '/HIDIdleTime/ && !d { print int($NF / 1000000000); d = 1 }'; fi
}

idle_try() {
  idle="$(idle_secs)" || idle=""
  case "$idle" in ''|*[!0-9]*) return 1 ;; esac
}

idle_wait() {
  local n=0
  idle_try || return 1
  [ "$idle" -ge "$1" ] && return 0
  echo "idle_wait=$(date -u +%FT%TZ) $2: idle $idle s, waiting for $1 s" >> "$3"
  echo "waiting for the Mac to be idle ($2)" >&2
  while :; do
    sleep "${PERF_IDLE_POLL:-15}"
    idle_try || return 1
    [ "$idle" -ge "$1" ] && return 0
    n=$((n + 1)); [ $((n % 40)) = 0 ] && echo "still waiting for the Mac to be idle ($2): idle $idle s" >&2
  done
}

idle_display() {   # idle_display WHEN LOG: the display-sleep state, at the start and the end
  echo "display_$1=sleep_after_$(pmset -g 2>/dev/null | awk '$1 == "displaysleep" { print $2; exit }')min prevent_display_sleep=$(pmset -g assertions 2>/dev/null | awk '/PreventUserIdleDisplaySleep/ { print $2; exit }')" >> "$2"
}
