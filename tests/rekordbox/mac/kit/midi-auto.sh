#!/bin/bash
# midi-auto.sh: rekordbox driven by MIDI in the tart VM, unattended (the Mac twin of
# win/kit/midi-auto.ps1). Run as root over SSH (vm-session.sh: sudo bash midi-auto.sh VERB ...);
# what needs DJ's session (rekordbox, the MIDI device, screenshots, the window list) goes through
# kit-lib.sh. Each verb is also usable by hand:
#   ensure                the kit's tools, Screen Recording for RBKit Screen, DJ's session, the MIDI
#                         device, the mapping, rekordbox and its settings, the corpus, Vision, the
#                         screen's regions, the screen saver: one OK/BAD line each (rekordbox not started)
#   ports                 every MIDI source and destination DJ's session sees
#   learn --note N --repeat S   press note N every 2 s for S s (for rekordbox's MIDI LEARN)
#   start [--mac MAC] | quit    start on the staged template's MAC and wait for first MIDI | quit it
#   shot | windows | access     a screenshot | the watched windows | RBKit Screen's permission
#   press --keys 'Down x3,Load' presses, then a screenshot
#   top                   sort by title, row 1, a screenshot
#   ready --expect any|hits    check screen.conf, Screen Recording and Stems Cache before a run
#   run [--from NAME] [--to NAME] [--max N] [--expect any|hits] [--inject-at N] [--minutes N]
#                         to the playlist, row 1 checked, then each row loaded on deck 1, read back
#                         by OCR and waited for in the bridge's log (as midi-auto.ps1's run)
#   watchtest             the dialog watch's own test: a test window opened, seen, closed
#   bundle                the log, screenshots, the app's logs and the mapping zipped ("BUNDLE <path>")
#   bundled --keys ZIP    the screenshots in that bundle deleted (once the Mac has unpacked it)
#   prepare | uninstallcache    not on the Mac driver yet (exit 2: README)
# Stops on its own (a "<step>: STOP:" line): /Users/Shared/rbst/midi-stop exists; a new window of
# rekordbox, rekordboxAgent, our app or macOS's dialogs (never answered, never closed; a screenshot
# is taken; watch-lib.sh), or such a window already open when run starts; deck 1 not the expected
# track; a track's stems not done in --max-track s; rekordbox gone; an error of the kit.
# Exit 0 done; 1 a STOP or a failed step; 2 refused (bad arguments, wrong state); 3 an expectation
# (--expect hits) not met. Log: /Users/Shared/rbst/midi-auto.log.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
RBKIT_KIT="${RBKIT_KIT:-$here}"
. "$here/kit-lib.sh" || { echo "midi-auto: STOP: kit-lib.sh didn't load"; exit 1; }
. "$here/watch-lib.sh" || { echo "midi-auto: STOP: watch-lib.sh didn't load"; exit 1; }
# rekordbox refuses these (deny icon); rows not imported at all: none known on the Mac yet
REFUSED=" "
STOP="$B/midi-stop"
CORPUS="$K/corpus"

Do="${1:-}"; [ $# -gt 0 ] && shift
FROM="" TO="" MAX=0 EXPECT=any INJECT=0 MINUTES="" KEYS="" NOTE="" REPEAT="" PLAYLIST="RBSP Tests"
unset MAC
START_WAIT=25 QUIET=10 MAX_TRACK=420
num() { [[ "$1" =~ ^[0-9]+$ ]]; }
while [ $# -gt 0 ]; do
  case "$1" in
    --mac) [ "$Do" = start ] || { echo "--mac: only for start"; exit 2; }; MAC="${2:-}"; [ "$#" -lt 2 ] || shift ;;
    --from) FROM="${2:-}"; shift ;; --to) TO="${2:-}"; shift ;;
    --max) MAX="${2:-}"; num "$MAX" || { echo "--max: a number"; exit 2; }; shift ;;
    --expect) EXPECT="${2:-}"; [[ "$EXPECT" =~ ^(any|hits)$ ]] || { echo "--expect any|hits"; exit 2; }; shift ;;
    --inject-at) INJECT="${2:-}"; num "$INJECT" || { echo "--inject-at: a row number"; exit 2; }; shift ;;
    --minutes)
      MINUTES="${2:-}"
      [[ "$MINUTES" =~ ^[1-9][0-9]{0,3}$ ]] && [ "$MINUTES" -le 1440 ] || { echo "--minutes: expected 1-1440"; exit 2; }
      shift ;;
    --keys) KEYS="${2:-}"; shift ;; --note) NOTE="${2:-}"; shift ;; --repeat) REPEAT="${2:-}"; shift ;;
    --playlist) PLAYLIST="${2:-}"; shift ;;
    --start-wait) START_WAIT="${2:-}"; shift ;; --quiet) QUIET="${2:-}"; shift ;; --max-track) MAX_TRACK="${2:-}"; shift ;;
    *) echo "midi-auto: unknown option $1"; exit 2 ;;
  esac
  shift
done

# the playlist's rows in rekordbox's title order: the corpus's file names (v1/, and mac/ when there
# is one), without their extension, sorted
rows() {
  local d f
  for d in "$CORPUS/v1" "$CORPUS/mac"; do
    [ -d "$d" ] || continue
    for f in "$d"/*.*; do
      [ -f "$f" ] || continue
      case "$f" in *.txt) continue ;; esac
      f="${f##*/}"; echo "${f%.*}"
    done
  done | LC_ALL=C sort -fu
}
refused() { [[ "$REFUSED" == *" $1 "* ]]; }
# a press that fails is a STOP (the MIDI daemon gone)
P() { local r; r="$(press "$@")" || { say "$Do: STOP: $r"; exit 1; }; }
ready() {
  local field missing="" access cache
  for field in DECK1 HEADER WALK MAIN_TITLE NEWS_TITLE; do
    [[ "${!field-}" == *[![:space:]]* ]] || missing="${missing:+$missing, }$field"
  done
  if [ -n "$missing" ]; then say "$Do: STOP: screen.conf needs $missing (README: the first session)"; return 2; fi
  if ! access="$(screen access)" || [ "$access" != "ok screen yes" ]; then
    say "$Do: STOP: RBKit Screen needs Screen Recording ($access; README: the owner allows it)"
    return 2
  fi
  if ! cache="$(cache_state)" || [ "$cache" != "Stems Cache installed" ]; then
    say "$Do: STOP: Stems Cache must be installed in rekordbox ($cache; README: Setting up once)"
    return 2
  fi
}

case "$Do" in
ensure)
  bad=0
  row() { if [ "$1" = 0 ]; then say "OK   $2"; else say "BAD  $2"; bad=$((bad + 1)); fi; }
  [ "$(norm 'RBSP Tests (1 Tracks)' 2> /dev/null)" = rbsptests1tracks ]; row $? "the kit's tools run (rbkit)"
  a="$(screen access)"
  [ "$a" = "ok screen yes" ]; row $? "Screen Recording for RBKit Screen ($a; without it: no screenshots, no OCR, no window titles: the owner allows it once, README)"
  c="$(stat -f %Su /dev/console 2> /dev/null)"
  [ "$c" = "$U" ] && ! ioreg -n Root -d1 | grep -q '"IOConsoleLocked" = Yes'; row $? "$U's session is on the screen, unlocked (console: $c)"
  r="$(midi_up)"; row $? "$r"
  p="$(as_user "$RBKIT" midi ports 2>&1)"
  grep -qx 'source RBSP Test' <<< "$p" && grep -qx 'destination RBSP Test' <<< "$p"; row $? "the MIDI device RBSP Test seen in $U's session ($(grep -c . <<< "$p") endpoints)"
  if [ -f "$MAPPING" ]; then
    grep -qi '9040' "$MAPPING" && grep -qi '9041' "$MAPPING"; row $? "rekordbox's mapping 'RBSP Test.midi.csv' (Browse Down 9040, Load 9041)"
    grep -qi '904A' "$MAPPING" && grep -qi '904B' "$MAPPING"; row $? "the mapping's Area Left/Right (904A, 904B: learned by the owner)"
  else row 1 "no rekordbox mapping '$MAPPING' (the owner learns it: README)"; fi
  v="$(rb_version)"; [ -n "$v" ]; row $? "rekordbox ${v:-not installed}; its onnxruntime: $(cache_state)"
  q="$(setting PartAnalysisQuality)"; [ "$q" = 0 ]; row $? "rekordbox's STEMS analysis: sound quality (PartAnalysisQuality ${q:-unset}; speed mode never reaches the bridge)"
  q="$(setting AutoAnalysis)"; [ "$q" = 0 ]; row $? "rekordbox's Auto Analysis off (AutoAnalysis ${q:-unset}; on, new tracks bring a dialog: the owner turns it off in Preferences)"
  rows > "$B/rows.txt"; n="$(grep -c . "$B/rows.txt")"; [ "$n" -gt 0 ]; row $? "the corpus: $n rows ($CORPUS)"
  o="$("$RBKIT" ocrtest "$B" 2>&1)"; grep -qx 'read 0: RBSP Tests (20 Tracks)' <<< "$o"; row $? "Vision reads rekordbox-like text ($(grep -m1 '^read 0: ' <<< "$o" || head -1 <<< "$o"))"
  [ -n "${DECK1:-}" ] && [ -n "${HEADER:-}" ]; row $? "the screen's regions for deck 1 and the list header (screen.conf: ${DECK1:-not measured} / ${HEADER:-not measured})"
  [ -n "${WALK:-}" ]; row $? "the walk to the playlist (screen.conf: ${WALK:-not learned})"
  as_user defaults -currentHost write com.apple.screensaver idleTime -int 0
  [ "$(as_user defaults -currentHost read com.apple.screensaver idleTime 2> /dev/null)" = 0 ] && pmset -g | grep -Eq '^ *displaysleep +0$'
  row $? "the screen saver and display sleep off"
  say "ensure: $bad not OK"
  [ "$bad" = 0 ] || exit 1 ;;
ports) as_user "$RBKIT" midi ports ;;
access) say "access: $(screen access)" ;;
shot) p="$(shot)"; if [ -n "$p" ]; then say "shot $p"; else say "shot: $(shot_err)"; exit 1; fi ;;
windows) if winlist; then say "windows: $(tr '\n' ';' <<< "$WL" | sed 's/;$//; s/;/; /g')"; else say "windows: the list failed"; exit 1; fi ;;
learn)
  [[ "$NOTE" =~ ^[0-9]+$ ]] && [ "$NOTE" -le 127 ] && [[ "$REPEAT" =~ ^[1-9][0-9]*$ ]] || { echo "learn: --note 0-127 --repeat SECS"; exit 2; }
  midi_up > /dev/null || { say "learn: STOP: the MIDI daemon didn't start"; exit 1; }
  end=$(($(now) + REPEAT)); k=0
  while [ "$(now)" -lt "$end" ]; do press_note "$NOTE" || { say "learn: STOP: the MIDI daemon didn't answer"; exit 1; }; k=$((k + 1)); nap 2; done
  say "learn: note $NOTE pressed $k times over $REPEAT s" ;;
press)
  [ -n "$KEYS" ] || { echo "press: --keys 'Down x3,Load'"; exit 2; }
  IFS=',' read -r -a ks <<< "$KEYS"
  for p in "${ks[@]}"; do w="${p%%x*}"; n="${p#*x}"; [ "$n" = "$p" ] && n=1; w="${w// /}"; n="${n// /}"; P "$w" "$n" 250; nap 0.4; done
  say "press $KEYS: $(shot_line)" ;;
top) P SortSong; nap 1; P Up 45 120; say "top: sorted, row 1; $(shot_line)" ;;
start)
  if rb_running; then say "start: rekordbox already running"; exit 0; fi
  r="$(seat_ok ${MAC+"$MAC"})" || { say "start: STOP: $r (rekordbox not started)"; exit 2; }
  rm -f "$WATCH_BASE"   # a new rekordbox: run's baseline is stale
  r="$(midi_up)" || { say "start: STOP: $r"; exit 1; }
  say "start: $r"; midi_clear
  # the screen kept awake while rekordbox runs (quit ends it)
  as_user /bin/sh -c 'nohup caffeinate -dis -t 21600 > /dev/null 2>&1 & echo $!' > "$B/caffeinate.pid" 2> /dev/null
  say "start: $(rb_start || echo 'open failed')"
  # rekordbox listens only after it has sent its first MIDI (its lights); its browser is built a
  # few seconds later: wait for both
  t=$(now)
  while ! first_midi && [ $(($(now) - t)) -lt 120 ]; do nap 0.5; done
  if ! first_midi; then winlist; say "start: STOP: no MIDI from rekordbox in 120 s (a dialog? the mapping?); windows: $(tr '\n' ';' <<< "$WL"); $(shot_line)"; exit 1; fi
  say "start: rekordbox's first MIDI after $(($(now) - t)) s; waiting 15 s for its browser"
  nap 15
  winlist; say "start: windows: $(tr '\n' ';' <<< "$WL" | sed 's/;$//; s/;/; /g')"; say "start: $(shot_line)"; say "start: ready" ;;
quit)
  if ! rb_running; then say "quit: rekordbox not running"; rm -f "$WATCH_BASE"; caffeinate_down; exit 0; fi
  # a dialog open: nothing is touched (no Play, no quit)
  watch_quit || { say "$WATCH_LINE"; exit 1; }
  # deck 1's PLAY light: a loaded, paused deck blinks it every 0.5 s; playing is a light that
  # stays on. Watched for 2 s.
  nap 2; read -r changes on <<< "$(play_light 2)"
  watch_quit || { say "$WATCH_LINE"; exit 1; }
  if [ "${changes:-0}" -le 1 ] && [ "${on:-0}" = 1 ]; then say "quit: deck 1 is playing: PlayPause first"; P Play; nap 1
  else say "quit: deck 1 not playing (${changes:-0} PLAY light changes in 2 s)"; fi
  watch_quit || { say "$WATCH_LINE"; exit 1; }
  e="$(rb_quit)"; say "quit: asked${e:+ ($e)}"
  t=$(now); while rb_running && [ $(($(now) - t)) -lt 90 ]; do nap 1; done
  if rb_running; then winlist; say "quit: STOP: rekordbox still running after 90 s (a dialog?), left as it is; windows: $(tr '\n' ';' <<< "$WL"); $(shot_line)"; exit 1; fi
  rm -f "$WATCH_BASE"
  caffeinate_down
  say "quit: rekordbox closed after $(($(now) - t)) s" ;;
watchtest)
  # the dialog watch's own test, with nothing of rekordbox's touched: the test window must be seen
  # as new (the same STOP line a run writes, which the Mac turns into a notification), then closed
  # by us (it is ours: a real dialog is never closed)
  watch_start watchtest || { say "$WATCH_LINE"; exit 1; }
  say "watchtest: windows at the start: $(tr '\n' ';' <<< "$BASE" | sed 's/;$//; s/;/; /g')"
  say "watchtest: $(testwindow)"; nap 3
  seen=0; watch_stop watchtest || { [[ "$WATCH_LINE" == *": STOP: new window(s): "*" (watch test);"* ]] && seen=1; say "$WATCH_LINE"; }
  say "watchtest: $(closetest)"; nap 2
  winlist || { say "watchtest: FAIL: the window list failed after the test window was closed"; exit 1; }
  left="$(grep -c "^$TEST_PROC | " <<< "$WL")"
  if [ "$seen" = 1 ] && [ "$left" = 0 ]; then say "watchtest: PASS: the watch saw the test window, and it is closed"
  else say "watchtest: FAIL: the test window seen: $seen, still open: $left"; exit 1; fi ;;
bundle)
  stamp="$(date +%Y%m%d-%H%M%S)"; d="$B/collect-$stamp"
  mkdir -p "$d/shots" "$d/applogs"
  for f in "$LOG" "$B/midi-daemon.log" "$MAPPING"; do [ -f "$f" ] && cp "$f" "$d/"; done
  cp "$LOGS"/* "$d/applogs/" 2> /dev/null
  cp "$B"/shots/*.png "$d/shots/" 2> /dev/null
  (cd "$B" && zip -qr "collect-$stamp.zip" "collect-$stamp") || { say "bundle: zip failed"; exit 1; }
  rm -rf "$d"; chmod 644 "$B/collect-$stamp.zip"
  echo "BUNDLE $B/collect-$stamp.zip" ;;
bundled)
  [[ "$KEYS" =~ ^collect-[0-9]{8}-[0-9]{6}\.zip$ ]] || { echo "bundled: --keys collect-<time>.zip"; exit 2; }
  n=0
  while IFS= read -r e; do
    case "$e" in */shots/?*.png) rm -f "$B/shots/${e##*/}"; n=$((n + 1)) ;; esac
  done < <(unzip -Z1 "$B/$KEYS" 2> /dev/null)
  say "bundled: $n screenshots in $KEYS deleted from the VM" ;;
prepare | uninstallcache) say "$Do: not on the Mac driver yet (README: the Mac driver)"; exit 2 ;;
ready) ready || exit "$?"; say "ready: OK" ;;
run)
  ready || exit "$?"
  rm -f "$STOP"
  rb_running || { say "run: rekordbox not running"; exit 2; }
  rows > "$B/rows.txt"
  ROWS=(); while IFS= read -r l; do ROWS+=("$l"); done < "$B/rows.txt"
  N=${#ROWS[@]}
  [ "$N" -gt 0 ] || { say "run: no corpus in $CORPUS"; exit 2; }
  idx() { local k; for k in "${!ROWS[@]}"; do [ "${ROWS[k]}" = "$1" ] && { echo "$k"; return; }; done; echo -1; }
  [ -n "$FROM" ] || FROM="${ROWS[0]}"
  first="$(idx "$FROM")"; [ "$first" -ge 0 ] || { say "run: no row named '$FROM'"; exit 2; }
  last=$((N - 1)); if [ -n "$TO" ]; then last="$(idx "$TO")"; fi
  [ "$last" -ge "$first" ] || { say "run: bad --to '$TO'"; exit 2; }
  # --inject-at: a row this run loads, else the test window would never be reached
  if [ "$INJECT" -gt 0 ]; then
    k=$((INJECT - 1))
    { [ "$k" -ge "$first" ] && [ "$k" -le "$last" ]; } || { say "run: --inject-at $INJECT: not in rows $((first + 1))-$((last + 1))"; exit 2; }
    refused "${ROWS[k]}" && { say "run: --inject-at $INJECT: ${ROWS[k]} is stepped over (rekordbox refuses it)"; exit 2; }
    n=0; for ((x = first; x <= k; x++)); do refused "${ROWS[x]}" || n=$((n + 1)); done
    { [ "$MAX" = 0 ] || [ "$n" -le "$MAX" ]; } || { say "run: --inject-at $INJECT: past --max $MAX (it would be load $n)"; exit 2; }
  fi
  want="$PLAYLIST ($N Tracks)"
  # deck 1's title, read from a screenshot at 4 scales into D_TS (D_T: joined); the screenshot is
  # kept only on a mismatch
  deck1() {
    nap 1.5
    D_PNG="$(shot)"; D_TS=(); D_ERR=""
    if [ -z "$D_PNG" ]; then D_ERR="no screenshot ($(shot_err))"; D_T=""; return; fi
    local r; r="$(ocr_deck1 "$D_PNG")"
    if [ -z "${r//[[:space:]]/}" ]; then   # one fresh screenshot before giving up
      drop "$D_PNG"; nap 1; D_PNG="$(shot)"; [ -n "$D_PNG" ] && r="$(ocr_deck1 "$D_PNG")"
    fi
    while IFS= read -r l; do D_TS+=("$l"); done <<< "$r"
    D_T="$(printf '%s / ' "${D_TS[@]+"${D_TS[@]}"}")"; D_T="${D_T% / }"
  }
  rowvote() { if [ ${#D_TS[@]} -gt 0 ]; then vote "${D_TS[@]}"; else echo -1; fi; }
  # the bridge after a load: none (no new line in START_WAIT s), done (then quiet for QUIET s),
  # timeout (still writing after MAX_TRACK s: its lines would be counted against the next row) or
  # stop (midi-stop created meanwhile): W_STATE, W_FIRST, W_DONE
  waitstems() {
    local n0="$1" t0="$2" t1 n m lastc
    W_STATE=""; W_FIRST=0; W_DONE=0
    while [ "$(lines)" = "$n0" ] && [ $(($(now) - t0)) -lt "$START_WAIT" ]; do
      [ -e "$STOP" ] && { W_STATE=stop; return; }
      nap 0.5
    done
    [ "$(lines)" = "$n0" ] && { W_STATE=none; return; }
    t1=$(now); n="$(lines)"; lastc=$(now)
    while [ $(($(now) - lastc)) -lt "$QUIET" ] && [ $(($(now) - t0)) -lt "$MAX_TRACK" ]; do
      [ -e "$STOP" ] && { W_STATE=stop; return; }
      nap 0.5; m="$(lines)"; [ "$m" != "$n" ] && { n="$m"; lastc=$(now); }
    done
    if [ $(($(now) - lastc)) -ge "$QUIET" ]; then W_STATE=done; else W_STATE=timeout; fi
    W_FIRST=$((t1 - t0)); W_DONE=$((lastc - t0))
  }
  # the bridge's lines since line N0, counted (match-lib.ps1's patterns): C_MISS C_HIT C_REF C_NEAR;
  # 1 when those lines are gone (the log rotated twice)
  count() {
    local l; l="$(lines_since "$1")" || return 1
    C_MISS="$(grep -cE '^[^ ]+ [^ ]+  miss ' <<< "$l")"; C_HIT="$(grep -cE '^[^ ]+ [^ ]+  hit  [^ ]+ \([0-9]+ samples' <<< "$l")"
    C_REF="$(grep -cE '^[^ ]+ [^ ]+  hit  [^ ]+ refused:' <<< "$l")"; C_NEAR="$(grep -cE '^[^ ]+ [^ ]+  near [^ ]+ for ' <<< "$l")"
    return 0
  }
  EXP_OK=0; EXP_BAD=0
  # a row's load said and checked: no bridge activity fails either --expect; hits wants hits, no
  # misses and no refused hits
  tally() {   # I NAME N0
    if [ "$W_STATE" = none ]; then EXP_BAD=$((EXP_BAD + 1)); say "row $(($1 + 1)) $2 : EXPECT FAIL: loaded (read back), but no bridge activity in $START_WAIT s"; return; fi
    count "$3" || { EXP_BAD=$((EXP_BAD + 1)); say "row $(($1 + 1)) $2 : unmeasured (bridge log rotated and its earlier lines are unavailable)"; return; }
    say "row $(($1 + 1)) $2 : read back OK, first $W_FIRST s, done $W_DONE s, $C_MISS misses, $C_HIT hits, $C_REF refused, $C_NEAR near"
    if [ "$EXPECT" = hits ] && { [ "$C_MISS" != 0 ] || [ "$C_REF" != 0 ] || [ "$C_HIT" = 0 ]; }; then
      EXP_BAD=$((EXP_BAD + 1)); say "row $(($1 + 1)) $2 : EXPECT FAIL: $C_MISS misses, $C_REF refused, $C_HIT hits (want 0 misses, 0 refused, hits)"
    elif [ "$EXPECT" = any ] && [ $((C_HIT + C_MISS + C_REF + C_NEAR)) = 0 ]; then
      EXP_BAD=$((EXP_BAD + 1)); say "row $(($1 + 1)) $2 : EXPECT FAIL: no classified bridge result"
    else EXP_OK=$((EXP_OK + 1)); fi
  }
  # A probe doesn't count toward the requested rows. With --expect hits it still needs a hit
  # and must have no misses or refusals.
  probetally() {   # J N0
    [ "$EXPECT" = hits ] || return 0
    local r why=""
    if [ "$1" -ge 0 ]; then r="$(($1 + 1)) ${ROWS[$1]}"; else r="? (deck 1 '$D_T')"; fi
    if [ "$W_STATE" = none ]; then why="no bridge activity in $START_WAIT s"
    elif [ "$W_STATE" = done ]; then
      count "$2" || { EXP_BAD=$((EXP_BAD + 1)); say "row $r : unmeasured (the probe's earlier bridge lines are unavailable)"; return; }
      { [ "$C_MISS" != 0 ] || [ "$C_REF" != 0 ] || [ "$C_HIT" = 0 ]; } && why="$C_MISS misses, $C_REF refused, $C_HIT hits"
    fi
    [ -z "$why" ] || { EXP_BAD=$((EXP_BAD + 1)); say "row $r : EXPECT FAIL (the sort check's load): $why"; }
  }
  # --inject-at: the test window, opened once before its row; guard: the watch before a load (its
  # STOP line in WATCH_LINE, 1); at the --inject-at row, a miss of the test window is a STOP too
  injected=0; stoptest=0; rc=0
  inject() { if [ "$INJECT" = "$1" ] && [ "$injected" = 0 ]; then injected=1; say "run: the watch's test window before row $1: $(testwindow)"; nap 3; fi; }
  guard() {
    local s=0
    watch_stop run || s=1
    if [ "$INJECT" = "$1" ]; then
      if [ "$s" = 1 ] && [[ "$WATCH_LINE" == *"(watch test);"* ]]; then stoptest=1
      elif [ "$s" = 0 ]; then WATCH_LINE="run: STOP: the watch missed its test window before row $1 (watch test FAILED); $(shot_line)"; s=1; fi
    fi
    return "$s"
  }
  halt() { say "$1"; [ "$INJECT" = 0 ] || say "run: $(closetest)"; rc=1; }
  # the watch's baseline before any MIDI: only rekordbox's main window (and our app's, and its
  # agent's news) open
  watch_start run allow rekordbox || { say "$WATCH_LINE"; exit 1; }
  watch_save; say "run: windows at the start: $(tr '\n' ';' <<< "$BASE" | sed 's/;$//; s/;/; /g')"
  deadline=""; [ -n "$MINUTES" ] && deadline=$(($(now) + MINUTES * 60))
  while :; do
    # to the playlist (screen.conf's WALK: "NAME TIMES GAPMS" or "wait MS" steps, ";" between)
    IFS=';' read -r -a steps <<< "$WALK"
    for s in "${steps[@]}"; do
      read -r a1 a2 a3 <<< "$s"
      [ -n "${a1:-}" ] || continue
      if [ "$a1" = Load ]; then halt "run: STOP: WALK must navigate without Load"; break 2; fi
      if [ "$a1" = wait ]; then nap "$(awk -v m="$a2" 'BEGIN { printf "%.3f", m / 1000 }')"; else P "$a1" "${a2:-1}" "${a3:-250}"; fi
    done
    png="$(shot)"; hdr=""; [ -n "$png" ] && hdr="$(ocr_header "$png")"
    if ! { h="$(norm "$hdr")" && [ -n "$h" ] && [ "$h" = "$(norm "$want")" ]; }; then
      say "run: STOP: the list header reads '$hdr', not '$want' (kept: ${png:-no screenshot: $(shot_err)})"; rc=1; break
    fi
    say "run: on the playlist (header '$hdr')"; drop "$png"
    # row 1 must be ROWS[0] (A to Z). Give OCR two reads per direction and flip at most once.
    ok=0; halted=0; flipped=0; try=0
    while [ "$try" -lt 2 ]; do
      try=$((try + 1))
      watch_stop run || { halt "$WATCH_LINE"; halted=1; break; }
      P Up 100 80; nap 0.7   # more than the list's rows: the top, whatever was selected
      inject 1; n0="$(lines)"
      guard 1 || { halt "$WATCH_LINE"; halted=1; break; }
      t0=$(now); P Load; deck1; drop "$D_PNG"
      waitstems "$n0" "$t0"; j="$(rowvote)"
      [ "$W_STATE" = stop ] && { halt "run: STOP: midi-stop"; halted=1; break; }
      [ "$W_STATE" = timeout ] && { halt "run: STOP: the sort check's load (deck 1 '$D_T'): the bridge still writing after $MAX_TRACK s"; halted=1; break; }
      if [ "$j" = 0 ]; then
        ok=1; say "run: row 1 is ${ROWS[0]} (read: '$D_T')"; R1_N0="$n0"; R1_STATE="$W_STATE"; R1_FIRST="$W_FIRST"; R1_DONE="$W_DONE"
        [ "$first" -gt 0 ] && probetally 0 "$n0"   # row 1 isn't counted below
        break
      fi
      probetally "$j" "$n0"
      if [ "$j" -lt 0 ]; then
        [ "$try" = 2 ] || say "run: row 1 read is inconclusive ('$D_T'); reloading without changing sort"
        continue
      fi
      [ "$flipped" = 0 ] || break
      say "run: row 1 reads '$D_T', not ${ROWS[0]}: SortSONG (try $try)"
      watch_stop run || { halt "$WATCH_LINE"; halted=1; break; }
      P SortSong; nap 1
      flipped=1; try=0
    done
    [ "$halted" = 1 ] && break
    [ "$ok" = 1 ] || { say "run: STOP: could not put row 1 first; $(shot_line)"; rc=1; break; }
    at=0   # the selected row
    say "run: rows $((first + 1))-$((last + 1)) of $N ($FROM .. ${ROWS[last]}), expect $EXPECT, bridge.log $(lines) lines"
    loads=0; halted=0
    for ((i = first; i <= last; i++)); do
      [ -e "$STOP" ] && { say "run: STOP: midi-stop"; rc=1; break; }
      [ -n "$deadline" ] && [ "$(now)" -gt "$deadline" ] && { say "run: STOP: --minutes $MINUTES reached"; rc=1; break; }
      rb_running || { say "run: STOP: rekordbox is gone"; rc=1; break; }
      name="${ROWS[i]}"
      refused "$name" && { say "row $((i + 1)) $name : stepped over (rekordbox refuses it)"; continue; }
      [ "$MAX" -gt 0 ] && [ "$loads" -ge "$MAX" ] && { say "run: --max $MAX loads reached"; break; }
      watch_stop run || { halt "$WATCH_LINE"; break; }
      if [ "$i" = 0 ]; then W_STATE="$R1_STATE"; W_FIRST="$R1_FIRST"; W_DONE="$R1_DONE"; tally 0 "$name" "$R1_N0"; loads=$((loads + 1)); continue; fi   # loaded by the sort check
      right=0; earlier=-1; j=-1
      for try in 1 2 3 4; do
        # a retry comes after a wait for the bridge: the windows again first
        if [ "$try" -gt 1 ]; then watch_stop run || { halt "$WATCH_LINE"; halted=1; break; }; fi
        if [ "$try" = 3 ] && [ "$j" -ge 0 ] && [ "$j" -lt "$i" ] && [ "$j" = "$earlier" ]; then
          say "row $((i + 1)) $name : deck 1 still has ${ROWS[j]}: stepping again"; at="$j"
        elif [ "$try" -ge 3 ]; then
          say "row $((i + 1)) $name : finding the row from the top again"; P Up 100 80; nap 0.7; at=0
        fi
        if [ "$i" -gt "$at" ]; then P Down $((i - at)) 300; nap 0.7; at="$i"; fi
        inject $((i + 1)); n0="$(lines)"
        guard $((i + 1)) || { halt "$WATCH_LINE"; halted=1; break; }
        t0=$(now); P Load; deck1
        j="$(rowvote)"
        if [ "$j" = "$i" ]; then right=1; drop "$D_PNG"; break; fi
        waitstems "$n0" "$t0"; probetally "$j" "$n0"
        [ "$W_STATE" = stop ] && { halt "run: STOP: midi-stop"; halted=1; break; }
        [ "$W_STATE" = timeout ] && { halt "run: STOP: row $((i + 1)) $name : the recovery load (deck 1 '$D_T'): the bridge still writing after $MAX_TRACK s"; halted=1; break; }
        [ "$try" = 1 ] && { earlier="$j"; say "row $((i + 1)) $name : deck 1 reads '$D_T': trying Load again"; }
        if [ "$try" -lt 4 ]; then drop "$D_PNG"; else say "row $((i + 1)) $name : deck 1 reads '$D_T' $D_ERR (kept: ${D_PNG:-none})"; fi
      done
      [ "$halted" = 1 ] && break
      [ "$right" = 1 ] || { say "run: STOP: deck 1 is not the expected track"; rc=1; break; }
      waitstems "$n0" "$t0"
      [ "$W_STATE" = stop ] && { halt "run: STOP: midi-stop"; break; }
      [ "$W_STATE" = timeout ] && { halt "run: STOP: row $((i + 1)) $name : the bridge still writing after $MAX_TRACK s (its lines would be counted against the next row)"; break; }
      loads=$((loads + 1)); tally "$i" "$name" "$n0"
    done
    break
  done
  # after the last row (or --max): midi-stop not created meanwhile, rekordbox still there, and no new window
  [ "$rc" = 0 ] && [ -e "$STOP" ] && { say "run: STOP: midi-stop"; rc=1; }
  if [ "$rc" = 0 ]; then
    if ! rb_running; then say "run: STOP: rekordbox is gone"; rc=1
    else watch_stop run || { say "$WATCH_LINE"; rc=1; }; fi
  fi
  [ "$INJECT" -gt 0 ] && [ "$stoptest" = 0 ] && [ "$rc" = 0 ] && { say "run: the watch test FAILED: no test STOP at row $INJECT"; rc=1; }
  say "run: expect $EXPECT: $EXP_OK rows OK, $EXP_BAD not"; [ "$EXP_BAD" -gt 0 ] && [ "$rc" = 0 ] && rc=3
  say "run: end"
  exit "$rc" ;;
*) sed -n '2,30p' "$0"; exit 2 ;;
esac
