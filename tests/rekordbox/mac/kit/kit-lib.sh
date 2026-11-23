# kit-lib.sh (sourced by midi-auto.sh): the Mac kit's paths and the calls into DJ's session, in the
# VM. The kit lives in /Users/Shared/rbsp-kit (staged by vm-session.sh with SHA256SUMS); what it
# writes goes to /Users/Shared/rbst (logs, screenshots, the MIDI daemon's files), owned by DJ.
# Everything the runner does outside bash goes through a function here: vm-session-test.sh swaps
# this file for a model of rekordbox (the runner and watch-lib.sh run as written).
K="${RBKIT_KIT:-/Users/Shared/rbsp-kit}"
B="${RBKIT_B:-/Users/Shared/rbst}"
U="${RBKIT_USER:-dj}"
UID_="$(id -u "$U" 2> /dev/null || echo 0)"
UH="/Users/$U"
LOG="$B/midi-auto.log"
LOGS="$UH/Library/Logs/rbstemsplus"
BRIDGE="$LOGS/bridge.log"
RB="/Applications/rekordbox 7/rekordbox.app"
RBID=com.pioneerdj.rekordboxdj
RBLIB="$RB/Contents/Frameworks/libonnxruntime.1.18.0.dylib"
RBSET="$UH/Library/Application Support/Pioneer/rekordbox6"
MAPPING="$RBSET/MidiMappings/RBSP Test.midi.csv"
SCREEN_APP="$K/RBKit Screen.app"
RBKIT="$K/bin/rbkit"
MIDI_LABEL=io.github.rbstemsplus.rbkit.midi
mkdir -p "$B/shots" && chown "$U" "$B" "$B/shots" 2> /dev/null
[ -f "$K/screen.conf" ] && . "$K/screen.conf"

say() { local l; l="$(date +%H:%M:%S)  $*"; printf '%s\n' "$l"; printf '%s\n' "$l" >> "$LOG"; }
now() { date +%s; }
nap() { sleep "$1"; }
# within SECS CMD...: CMD, killed after SECS (exit 142 then)
within() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV or exit 127' "$s" "$@"; }
as_user() { launchctl asuser "$UID_" sudo -u "$U" -H -- "$@"; }

# screen VERB ARGS...: RBKit Screen.app as DJ (it holds the Screen Recording permission); its answer
# printed, its exit status returned (142: no answer in 30 s)
screen() {
  local o rc; o="$B/screen-$$-$RANDOM.out"; rm -f "$o"
  within 30 launchctl asuser "$UID_" sudo -u "$U" -H -- open -W -n -g "$SCREEN_APP" --args "$o" "$@" > /dev/null 2>&1; rc=$?
  if [ -s "$o" ]; then cat "$o"; else echo "FAIL no answer from RBKit Screen (exit $rc)"; [ "$rc" = 0 ] && rc=1; fi
  rm -f "$o"; return "$rc"
}
# shot: a screenshot of DJ's screen, its path printed ("" when there is none: shot_err says why).
# Without the permission, macOS shows its own prompt (universalAccessAuthWarn) once: the watch
# sees it, nothing answers it.
shot() {
  local p r; p="$B/shots/$(date +%Y%m%d-%H%M%S)-$RANDOM.png"
  r="$(screen shot "$p")"
  if [ "$r" = "ok shot $p" ] && [ -s "$p" ]; then rm -f "$B/shot.err"; echo "$p"; else printf '%s\n' "$r" > "$B/shot.err"; echo ""; fi
}
shot_err() { head -1 "$B/shot.err" 2> /dev/null; }
# shot_line: a screenshot for a STOP line ("shot PATH" or "no screenshot (WHY)")
shot_line() { local p; p="$(shot)"; if [ -n "$p" ]; then echo "shot $p"; else echo "no screenshot ($(shot_err))"; fi; }
drop() { [ -z "${1:-}" ] || rm -f "$1"; }

# the test window (the dialog watch's own test): opened as DJ, closed by its pid
testwindow() {
  local o; o="$B/testwindow.out"; rm -f "$o"
  launchctl asuser "$UID_" sudo -u "$U" -H -- open -n -g "$SCREEN_APP" --args "$o" testwindow > /dev/null 2>&1
  for _ in $(seq 1 40); do grep -q '^ok shown [0-9]' "$o" 2> /dev/null && break; sleep 0.25; done
  if grep -q '^ok shown [0-9]' "$o" 2> /dev/null; then sed -n 's/^ok shown \([0-9]*\).*/\1/p' "$o" > "$B/testwindow.pid"; echo "testwindow: shown"
  else echo "testwindow: FAIL not shown ($(cat "$o" 2> /dev/null))"; fi
}
closetest() {
  local p; p="$(cat "$B/testwindow.pid" 2> /dev/null)"
  if [[ "$p" =~ ^[0-9]+$ ]] && ps -p "$p" -o comm= 2> /dev/null | grep -q rbkit-screen; then kill "$p"; rm -f "$B/testwindow.pid"; echo "closetest: ok"
  else echo "closetest: no test window"; fi
}

# rekordbox's own process (not rekordboxAgent)
rb_pid() { pgrep -f 'rekordbox\.app/Contents/MacOS/rekordbox( |$)' | head -1; }
rb_running() { [ -n "$(rb_pid)" ]; }
# The staged template and, for a host-driven start, the fresh host reading must both match en0.
# On refusal, print the reason for midi-auto's STOP.
seat_ok() {
  local t g mac fmt='^([0-9a-f]{2}:){5}[0-9a-f]{2}$'
  if ! t="$(cat "$K/template-mac" 2> /dev/null)" || ! [[ "$t" =~ $fmt ]]; then
    echo "template-mac is missing or malformed (run stage)"
    return 1
  fi
  if ! g="$(ifconfig en0 2> /dev/null)"; then echo "couldn't read en0"; return 1; fi
  mac="$(awk '$1 == "ether" {print tolower($2)}' <<< "$g")"
  if ! [[ "$mac" =~ $fmt ]]; then echo "en0's MAC address is missing or malformed"; return 1; fi
  if [ "$mac" != "$t" ]; then echo "en0's MAC address $mac differs from the staged template's $t"; return 1; fi
  if [ "$#" -gt 0 ]; then
    g="$(printf '%s' "$1" | tr 'A-F' 'a-f')"
    if ! [[ "$g" =~ $fmt ]]; then echo "--mac is missing or malformed"; return 1; fi
    if [ "$mac" != "$g" ]; then echo "en0's MAC address $mac differs from the host reading $g"; return 1; fi
  fi
}
rb_version() { defaults read "$RB/Contents/Info.plist" CFBundleShortVersionString 2> /dev/null; }
rb_start() { as_user open -a "$RB" > /dev/null 2>&1 && echo "started rekordbox $(rb_version)"; }
# asked to quit as a user quits it (an Apple event; a killed rekordbox leaves rekordboxAgent running)
rb_quit() { as_user osascript -e 'with timeout of 20 seconds' -e "tell application id \"$RBID\" to quit" -e 'end timeout' 2>&1 > /dev/null; }
caffeinate_down() {
  local p state user command
  p="$(cat "$B/caffeinate.pid" 2> /dev/null)"
  if [[ "$p" =~ ^[1-9][0-9]*$ ]] && state="$(ps -p "$p" -o user=,comm= 2> /dev/null)"; then
    read -r user command <<< "$state"
    if [ "$user" = "$U" ] && [[ "$command" == caffeinate || "$command" == */caffeinate ]]; then
      kill "$p" 2> /dev/null
    fi
  fi
  rm -f "$B/caffeinate.pid"
}
# Stems Cache in rekordbox: our bridge in place of Pioneer's onnxruntime
cache_state() { if [ ! -f "$RBLIB" ]; then echo "no rekordbox library"; elif grep -aq "rbstems bridge: " "$RBLIB"; then echo "Stems Cache installed"; else echo "pioneer"; fi; }
setting() { grep -o "name=\"$1\" val=\"[^\"]*\"" "$RBSET/rekordbox3.settings" 2> /dev/null | head -1 | sed 's/.*val="//; s/"$//'; }

# the bridge's log: its length in lines, and its lines from line N0 on (bridge.log.1 first when it
# rotated meanwhile; nothing and exit 1 when those lines are gone)
lines() { if [ -f "$BRIDGE" ]; then wc -l < "$BRIDGE" | tr -d ' '; else echo 0; fi; }
lines_since() {
  local n; n="$(lines)"
  if [ "$n" -ge "$1" ]; then tail -n +"$(($1 + 1))" "$BRIDGE" 2> /dev/null; return 0; fi
  [ -f "$BRIDGE.1" ] && [ "$(wc -l < "$BRIDGE.1" | tr -d ' ')" -ge "$1" ] || return 1
  tail -n +"$(($1 + 1))" "$BRIDGE.1"; cat "$BRIDGE" 2> /dev/null
}

# --- MIDI: the daemon (rbkit midi serve) as a launchd job in DJ's session, presses through its FIFO
midi_ready() {
  local p job
  p="$(sed -n 's/^ready \([0-9]*\)$/\1/p' "$B/midi-ready" 2> /dev/null)"
  [[ "$p" =~ ^[1-9][0-9]*$ ]] && kill -0 "$p" 2> /dev/null && [ -p "$B/midi-ctl" ] || return 1
  job="$(launchctl print "gui/$UID_/$MIDI_LABEL" 2> /dev/null)" || return 1
  grep -Eq "^[[:space:]]*pid = $p[[:space:]]*$" <<< "$job"
}
midi_up() {
  midi_ready && { echo "the MIDI device RBSP Test is up"; return 0; }
  launchctl bootout "gui/$UID_/$MIDI_LABEL" > /dev/null 2>&1
  rm -f "$B/midi-ready"
  cat > "$B/midi.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>$MIDI_LABEL</string>
<key>ProgramArguments</key><array><string>$RBKIT</string><string>midi</string><string>serve</string><string>$B</string></array>
<key>RunAtLoad</key><true/>
<key>StandardErrorPath</key><string>$B/midi-daemon.log</string>
<key>StandardOutPath</key><string>$B/midi-daemon.log</string>
</dict></plist>
EOF
  chmod 644 "$B/midi.plist"; chown root "$B/midi.plist" 2> /dev/null
  launchctl bootstrap "gui/$UID_" "$B/midi.plist" 2>&1 || { echo "the MIDI daemon couldn't be started (launchctl bootstrap)"; return 1; }
  for _ in $(seq 1 40); do midi_ready && { echo "the MIDI device RBSP Test started"; return 0; }; sleep 0.25; done
  echo "the MIDI daemon didn't get ready in 10 s ($(tail -1 "$B/midi-daemon.log" 2> /dev/null))"; return 1
}
midi_down() {
  if midi_ready; then midi_call "q$RANDOM$RANDOM quit" 5 > /dev/null; fi
  launchctl bootout "gui/$UID_/$MIDI_LABEL" > /dev/null 2>&1; rm -f "$B/midi-ready"; true
}
# midi_call "ID VERB ARGS" SECS: one line to the daemon, its ack waited for SECS
midi_call() {
  local id="${1%% *}" t
  within 5 /bin/sh -c 'printf "%s\n" "$1" > "$2"' sh "$1" "$B/midi-ctl" || { echo "err the MIDI daemon isn't listening"; return 1; }
  t=$(($(date +%s) + $2))
  while [ "$(date +%s)" -le "$t" ]; do
    if grep -q "^$id " "$B/midi-ack" 2> /dev/null; then grep -m1 "^$id " "$B/midi-ack" | cut -d' ' -f2-; grep -q "^$id ok" "$B/midi-ack"; return; fi
    sleep 0.05
  done
  echo "err no answer from the MIDI daemon in $2 s"; return 1
}
# the mapping (README): notes on channel 1
note_of() {
  case "$1" in
    Down) echo 64 ;; Load) echo 65 ;; Up) echo 66 ;; Forward) echo 67 ;; Back) echo 68 ;; Area) echo 69 ;;
    Play) echo 70 ;; SortTrack) echo 71 ;; View) echo 72 ;; SortSong) echo 73 ;; AreaLeft) echo 74 ;; AreaRight) echo 75 ;;
    *) return 1 ;;
  esac
}
# press NAME [TIMES] [GAPMS]: 0, or a STOP-worthy failure (its reason printed)
press() {
  local n t="${2:-1}" g="${3:-250}" r
  n="$(note_of "$1")" || { echo "unknown control $1"; return 1; }
  r="$(midi_call "p$RANDOM$RANDOM press $n $t $g" $((t * (g + 80) / 1000 + 5)))" || { echo "press $1: $r"; return 1; }
}
press_note() { midi_call "p$RANDOM$RANDOM press $1 1 0" 5 > /dev/null; }
# rekordbox's first MIDI (its lights) since the daemon's log was cleared
first_midi() { [ -s "$B/midi-in.log" ]; }
midi_clear() { : > "$B/midi-in.log"; chown "$U" "$B/midi-in.log" 2> /dev/null; }
# deck 1's PLAY light (note 70, 46 hex): "CHANGES ON", CHANGES its messages in the last SECS, ON 1
# when its last value is lit (0 when dark or never seen)
play_light() {
  local since=$((($(date +%s) - $1) * 1000))
  awk -v s="$since" '($2 ~ /^[89]/) && $3 == "46" { on = ($2 ~ /^9/ && $4 != "00") ? 1 : 0; if ($1 >= s) n++ }
    END { printf "%d %d\n", n, on }' "$B/midi-in.log" 2> /dev/null || echo "0 0"
}

# --- reading the screen: the regions are screen.conf's (x y w h, pixels of the screenshot)
ocr() { "$RBKIT" ocr "$@" 2> /dev/null; }
# deck 1's title at the four scales, one reading a line
ocr_deck1() { local s; for s in 1 2 3 4; do printf '%s\n' "$(ocr "$1" $DECK1 "$s" | head -1)"; done; }
ocr_header() { local t; t="$(ocr "$1" $HEADER 2)"; [ -n "${t// /}" ] || t="$(ocr "$1" $HEADER 2 plain)"; echo "$t"; }
# A failed or missing vote is inconclusive, just like rbkit's -1.
vote() {
  local row
  row="$("$RBKIT" vote "$B/rows.txt" "$@")" && [[ "$row" =~ ^(-1|[0-9]+)$ ]] || row=-1
  printf '%s\n' "$row"
}
norm() { "$RBKIT" norm "$1"; }
