# watch-lib.sh (sourced after kit-lib.sh): the dialog watch, as win/kit/watch-lib.ps1. The on-screen
# windows (CGWindowList, through RBKit Screen) of the watched processes, each "PROCESS | TITLE | ID",
# against a baseline: a window not in it (another ID, even with the same title) is a STOP. Each
# watch_ function sets WATCH_LINE (its STOP line, "" when all is well) and prints nothing; it
# returns 1 with a STOP. Only screen and shot_line are used: vm-session-test.sh runs these with
# them stubbed.
# Watched: rekordbox, its agent, our app, and macOS's own dialogs (alerts, privacy and permission
# prompts, Gatekeeper, password prompts, the crash reporter); the test window is RBKit Screen's. Layers 0-24
# only: 25 and up are the menu bar's status items and the system's overlays. Windows under 40x20
# are left out (no dialog is that small).
WATCHED='rekordbox|rekordboxAgent|RB Stems Plus|UserNotificationCenter|universalAccessAuthWarn|CoreServicesUIAgent|SecurityAgent|Problem Reporter|RBKit Screen'
TEST_PROC='RBKit Screen'
WATCH_BASE="$B/watch-base.txt"   # run's baseline, for quit
BASE=""
WL=""
WINDOW_ROWS=""

# winlist: the watched windows now into WL (sorted, one a line, "" when none); 1 when the list failed
# (no answer, or a line that isn't "PROCESS | TITLE | WxH | LAYER | ID": never a guess)
winlist() {
  local o
  WL=""
  WINDOW_ROWS=""
  o="$(screen windows)" || return 1
  WINDOW_ROWS="$(awk -F' [|] ' -v w="^($WATCHED)\$" '
    NR == 1 { if ($0 !~ /^ok [0-9]+$/) { bad = 1; exit }; n = substr($0, 4) + 0; next }
    NF != 5 || $3 !~ /^[0-9]+x[0-9]+$/ || $4 !~ /^-?[0-9]+$/ || $5 !~ /^[0-9]+$/ { bad = 1; exit }
    { split($3, s, "x") }
    $1 ~ w && $4 >= 0 && $4 < 25 && s[1] >= 40 && s[2] >= 20 { print }
    END { exit (bad || NR - 1 != n) }' <<< "$o")" || return 1
  WL="$(awk -F' [|] ' 'NF { print $1 " | " $2 " | " $5 }' <<< "$WINDOW_ROWS" | LC_ALL=C sort -u)"
}
in_list() { [ -n "$2" ] && grep -qxF -- "$1" <<< "$2"; }
watch_line() {   # WHAT NEW: the STOP line for new windows
  local l test=1 w
  l="$1: STOP: new window(s): $(tr '\n' ';' <<< "$2" | sed 's/;$//; s/;/; /g')"
  while IFS= read -r w; do [[ "$w" == "$TEST_PROC | "* ]] || test=0; done <<< "$2"
  [ "$test" = 0 ] || l="$l (watch test)"
  WATCH_LINE="$l; $(shot_line)"
}
watch_failed() { WATCH_LINE="$1: STOP: the window list failed (DJ logged out, or RBKit Screen not answering?)"; }
# watch_start WHAT [allow] [rekordbox]: the baseline taken. allow: a window already open would never
# look new, so match the calibrated titles and the main window's size. Allow at most one of each,
# and require the main window with "rekordbox".
watch_start() {
  local what="$1" allow="${2:-}" rbk="${3:-}"
  WATCH_LINE=""
  winlist || { watch_failed "$what"; return 1; }
  BASE="$WL"
  [ "$allow" = allow ] || return 0
  if [[ "${MAIN_TITLE:-}" != *[![:space:]]* || "${NEWS_TITLE:-}" != *[![:space:]]* ]]; then
    WATCH_LINE="$what: STOP: screen.conf needs MAIN_TITLE and NEWS_TITLE (README: the first session)"
    return 1
  fi
  if ! MAIN_TITLE="$MAIN_TITLE" NEWS_TITLE="$NEWS_TITLE" awk -F' [|] ' -v rbk="$rbk" '
    function same_title(a, b) { return ("=" a) == ("=" b) }
    !NF { next }
    { split($3, s, "x"); count[$1]++ }
    $1 == "rekordbox" && same_title($2, ENVIRON["MAIN_TITLE"]) && s[1] >= 800 && s[2] >= 500 { next }
    $1 == "RB Stems Plus" && $2 == "RB Stems Plus" { next }
    $1 == "rekordboxAgent" && same_title($2, ENVIRON["NEWS_TITLE"]) { next }
    { bad = 1; exit }
    END {
      for (p in count) if (count[p] > 1) bad = 1
      exit (bad || (rbk == "rekordbox" && count["rekordbox"] != 1))
    }' <<< "$WINDOW_ROWS"; then
    WATCH_LINE="$what: STOP: not just rekordbox's$([ "$rbk" = rekordbox ] || echo ' (if open)'), its agent's news and our app's main windows at the start: $(tr '\n' ';' <<< "$WL" | sed 's/;$//; s/;/; /g'); $(shot_line)"
    return 1
  fi
  return 0
}
watch_stop() {
  local what="$1" nw="" w
  WATCH_LINE=""
  winlist || { watch_failed "$what"; return 1; }
  while IFS= read -r w; do
    [ -n "$w" ] || continue
    in_list "$w" "$BASE" || nw="${nw:+$nw$'\n'}$w"
  done <<< "$WL"
  [ -z "$nw" ] && return 0
  watch_line "$what" "$nw"; return 1
}
watch_save() { printf '%s\n' "$BASE" > "$WATCH_BASE"; }
# quit's check: against run's baseline when there is one, else the start rule
watch_quit() {
  if [ -f "$WATCH_BASE" ]; then BASE="$(grep . "$WATCH_BASE")"; watch_stop quit; return; fi
  watch_start quit allow rekordbox
}
