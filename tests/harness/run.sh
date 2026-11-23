#!/bin/bash
# The harness's runner in the tart VM, as root (tests/harness/vm.sh starts it over SSH).
#
#   run.sh setup                     once per VM clone: the pristine copies of rekordbox's files
#                                    the scenarios change, and the harness's tools in place
#   run.sh install-app ZIP           the TEST build to /Applications (the user's own copy)
#   run.sh scenario NAME DIR [VERB]  one run of scenarios/NAME.sh; its trace, the fake helper's
#                                    argv log and the result into DIR. VERB: reopen | activate |
#                                    appleevent | click (activation scenarios; default reopen)
# MODE=trace (default) or perf in the environment: the probe's mode.
set -u
H=/Users/Shared/rbsp-harness
here="$(cd "$(dirname "$0")" && pwd)"

case "${1:-}" in
setup)
  mkdir -p "$H/pristine" "$H/bin"
  # shellcheck source=lib.sh
  . "$here/lib.sh"
  [ -f "$H/pristine/libonnxruntime.1.18.0.dylib" ] || cp -p "$RBLIB" "$H/pristine/"
  grep -q "rbstems bridge: " "$H/pristine/libonnxruntime.1.18.0.dylib" && { echo "rekordbox's library already has a bridge: start from a clean clone" >&2; exit 1; }
  [ -d "$H/pristine/demucs3_model" ] || ditto "$MODELDIR" "$H/pristine/demucs3_model"
  [ -f "$H/pristine/Info.plist" ] || cp -p "$RB/Contents/Info.plist" "$H/pristine/Info.plist"
  # the two other files a real Stems Cache install re-signs (its executable and CodeResources), so
  # a scenario that installs it for real leaves the next one Pioneer's rekordbox (reset_all)
  [ -f "$H/pristine/rekordbox" ] || cp -p "$RB/Contents/MacOS/rekordbox" "$H/pristine/rekordbox"
  [ -f "$H/pristine/CodeResources" ] || cp -p "$RB/Contents/_CodeSignature/CodeResources" "$H/pristine/CodeResources"
  echo "setup: pristine copies in $H/pristine"
  ;;
install-app)
  zip="$2"
  pkill -x "RB Stems Plus" 2>/dev/null; sleep 1
  rm -rf "/Applications/RB Stems Plus.app" "$H/unzip" && mkdir -p "$H/unzip"
  ditto -x -k "$zip" "$H/unzip" && ditto "$H/unzip/RB Stems Plus.app" "/Applications/RB Stems Plus.app"
  chown -R "${HARNESS_USER:-dj}":staff "/Applications/RB Stems Plus.app"
  rm -rf "$H/unzip"
  grep -q "TEST BUILD" "/Applications/RB Stems Plus.app/Contents/MacOS/RB Stems Plus" || { echo "not a TEST build" >&2; exit 1; }
  # kept: a scenario that deletes or updates the app (uninstall-everything, update-flow) gets it back
  [ "$zip" = "$H/app.zip" ] || cp "$zip" "$H/app.zip"
  shasum -a 256 "/Applications/RB Stems Plus.app/Contents/MacOS/RB Stems Plus" | cut -d' ' -f1 > "$H/app.sha256"
  echo "installed: $(defaults read "/Applications/RB Stems Plus.app/Contents/Info.plist" CFBundleShortVersionString)"
  ;;
scenario)
  name="$2"; RUN="$3"; VERB="${4:-reopen}"
  export VERB
  mkdir -p "$RUN"; : > "$RUN/result"
  # the app as pushed: an earlier scenario may have deleted it (the complete uninstall) or replaced
  # it (an update)
  exe="/Applications/RB Stems Plus.app/Contents/MacOS/RB Stems Plus"
  if [ -f "$H/app.zip" ] && { [ ! -f "$exe" ] || [ "$(shasum -a 256 "$exe" 2>/dev/null | cut -d' ' -f1)" != "$(cat "$H/app.sha256" 2>/dev/null)" ]; }; then
    pkill -x "RB Stems Plus" 2>/dev/null
    bash "$0" install-app "$H/app.zip" >&2 || { echo "FAIL the app couldn't be installed again" >> "$RUN/result"; exit 1; }
  fi
  # shellcheck source=lib.sh
  . "$here/lib.sh"
  # shellcheck disable=SC1090
  . "$here/scenarios/$name.sh"
  reset_all
  setup
  steps
  pgrep -x "RB Stems Plus" >/dev/null && quit_app
  sleep 0.3
  for f in "$OUT"/app-*.jsonl "$OUT"/perf-*.tsv "$OUT"/fields.tsv "$OUT"/watcher-*.json; do [ -f "$f" ] && cp "$f" "$RUN/"; done
  # P14: the probe's afterExit stamps (perf mode), each file's modification time in the VM's
  # clock, seconds with ns ("afterexit-<pid>-start|work <s.ns>"; tests/perf/stats.py)
  for f in "$OUT"/afterexit-*; do [ -f "$f" ] && echo "${f##*/} $(stat -f %Fm "$f")"; done > "$RUN/afterexit.txt"
  [ -s "$RUN/afterexit.txt" ] || rm -f "$RUN/afterexit.txt"
  # the order the copies started in (record.sh pairs the k-th of two runs)
  traces | sed "s|.*/||" > "$RUN/traces.txt"
  [ -f "$FAKE/log/argv.log" ] && cp "$FAKE/log/argv.log" "$RUN/helper-argv.log"
  # the app's own log, for reading a failure (never compared: it holds clock times)
  [ -f "$LOGS/app.log" ] && cp "$LOGS/app.log" "$RUN/app.log"
  # the result file decides: a FAIL from a subshell ($(count_of …)) is there though $FAILS missed it
  nfail="$(grep -c '^FAIL' "$RUN/result")"
  echo "$name $VERB $MODE: $nfail failed" >> "$RUN/result"
  grep '^FAIL' "$RUN/result" >&2
  ! grep -q '^FAIL' "$RUN/result"
  ;;
*)
  sed -n '2,12p' "$0"; exit 2 ;;
esac
