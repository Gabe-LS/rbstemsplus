#!/bin/bash
# Tests of the rekordbox toolkit's Mac driver that need no VM: shell syntax, the Swift tools built
# here (the title matching on real OCR slips, Vision's self-test), the kit's checksum check, the
# dialog watch and the runner (midi-auto.sh as written, over a model of rekordbox), and
# vm-session.sh's argument handling and what it sends, with tart, ssh, scp, xcrun, codesign, vm.sh
# and the notifier stubbed. Run by make check. Nothing here starts a MIDI device, a screenshot or
# rekordbox on this Mac. (win/pc-session-test.sh checks all of tests/rekordbox for anything personal.)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbkit-mac-test.XXXXXX")"
trap 'rm -rf "${tmp:?}"' EXIT
pass=0; fail=0
ok() { pass=$((pass + 1)); }
no() { fail=$((fail + 1)); echo "FAIL: $*"; }
has() { if grep -qF -- "$2" "$3" 2> /dev/null; then ok; else no "$1: no '$2' in $(basename "$3")"; fi; }
hasnt() { if grep -qF -- "$2" "$3" 2> /dev/null; then no "$1: '$2' in $(basename "$3")"; else ok; fi; }

# --- syntax
for f in "$here/vm-session.sh" "$0" "$here"/kit/*.sh; do bash -n "$f" && ok || no "bash -n ${f##*/}"; done

# --- the Swift tools, built as stage builds them
rbk=""
if xcrun --sdk macosx --show-sdk-path > /dev/null 2>&1; then
  sw() { xcrun swiftc -O -module-cache-path "$tmp/swift-cache" -target arm64-apple-macos13.0 "$@"; }
  if sw "$here/kit/rbkit.swift" -o "$tmp/rbkit" > "$tmp/build.log" 2>&1; then ok; rbk="$tmp/rbkit"; else no "rbkit doesn't build:"; sed 's/^/    /' "$tmp/build.log"; fi
  sw "$here/kit/rbkit-screen.swift" -o "$tmp/rbkit-screen" > "$tmp/build2.log" 2>&1 && ok || { no "rbkit-screen doesn't build:"; sed 's/^/    /' "$tmp/build2.log"; }
else
  echo "SKIP: no macOS SDK: the Swift tools not built, the matching, Vision and the runner not run"
fi
if [ -n "$rbk" ]; then
  # match-lib.ps1's rules on the v1 corpus's names, with OCR slips seen on the PC (wi5 for w15)
  printf '%s\n' 01-mp3-cbr320 02-mp3-vbr2 03-mp3-48k 04-aac-256 05-aac-128 06-aac-48k 07-alac-16 08-alac-24-96k 09-flac-16 10-flac-24-48k \
    11-flac-24-96k 12-wav-16 13-wav-24-48k 14-wav-f32 15-wav-f32-96k 16-aiff-16 17-aiff-24-48k 18-aiff-24-96k 19-mp3-cbr128 20-wav-16-30s > "$tmp/rows"
  m() { local want="$1" got; shift; got="$("$rbk" "$@" 2>&1)"; [ "$got" = "$want" ] && ok || no "rbkit $* gave '$got', want '$want'"; }
  m 0 match "$tmp/rows" 01-mp3-cbr320
  m 14 match "$tmp/rows" 'l5-wav-f32-96k'
  m 0 match "$tmp/rows" '01-mр3-cbr32O'          # a Cyrillic letter, an O for a 0
  m 18 match "$tmp/rows" '19-MP3-CBR128'
  m -1 match "$tmp/rows" rekordbox
  m -1 match "$tmp/rows" ''
  printf '%s\n' abc-one abd-one > "$tmp/rows2"
  m -1 match "$tmp/rows2" 'abx-one'               # one edit from both: not 2 closer than the other
  m -1 match "$tmp/rows2" 'abc-one'              # exact, but the other row is only 1 edit away
  m 14 vote "$tmp/rows" 'wi5-wav-f32-96k' '15-wav-f32-96k' '' 'x'
  m -1 vote "$tmp/rows" 01-mp3-cbr320 02-mp3-vbr2
  m -1 vote "$tmp/rows" 01-mp3-cbr320
  m -1 vote "$tmp/rows" 01-mp3-cbr320 01-mp3-cbr320 02-mp3-vbr2 02-mp3-vbr2
  m 1 vote "$tmp/rows" 02-mp3-vbr2 02-mp3-vbr2 01-mp3-cbr320
  m rbsptests20tracks norm 'RBSP Tests (20 Tracks)'
  for a in bogus midi 'ocr x 1 2 3'; do "$rbk" $a > /dev/null 2>&1; [ $? = 2 ] && ok || no "rbkit $a: not exit 2"; done
  # Vision: rekordbox-like light text on black, read back (macOS's text recognition needs its
  # services: inside a sandbox that blocks them it can't run)
  o="$("$rbk" ocrtest "$tmp" 2>&1)"
  if grep -q 'Vision: ' <<< "$o"; then echo "SKIP: Vision not reachable here ($o): run unsandboxed for the OCR self-test"
  else
    grep -qxF 'read 0: RBSP Tests (20 Tracks)' <<< "$o" && ok || no "Vision: the header: $o"
    [ "$("$rbk" vote "$tmp/rows" "$(sed -n 's/^read 1: //p' <<< "$o")" "$(sed -n 's/^read 1: //p' <<< "$o")")" = 0 ] && ok || no "Vision: 01-mp3-cbr320 not matched: $o"
    [ "$("$rbk" ocr "$tmp/ocrtest.png" 0 120 900 60 3)" = 15-wav-f32-96k ] && ok || no "rbkit ocr: the third line at x3"
  fi
fi

# --- verify-kit.sh
vk="$tmp/vk"; mkdir -p "$vk/corpus/v1"; cp "$here/kit/verify-kit.sh" "$vk/"; echo a > "$vk/corpus/v1/01-x.mp3"
sums() { (cd "$vk" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done > SHA256SUMS); }
verify() { bash "$vk/verify-kit.sh" > "$tmp/vo" 2>&1; [ $? = "$1" ] && ok || { no "verify-kit: $2 (exit $?)"; sed 's/^/    /' "$tmp/vo"; }; }
: > "$vk/SHA256SUMS"; verify 1 'empty manifest'
sums; verify 0 'complete manifest'
echo b > "$vk/corpus/v1/02-y.mp3"; verify 1 'an unlisted file'; rm "$vk/corpus/v1/02-y.mp3"
echo c > "$vk/corpus/v1/01-x.mp3"; verify 1 'a changed file'; echo a > "$vk/corpus/v1/01-x.mp3"; verify 0 'restored'
mv "$vk/corpus/v1/01-x.mp3" "$tmp/"; verify 1 'a missing file'; mv "$tmp/01-x.mp3" "$vk/corpus/v1/"
ln -s 01-x.mp3 "$vk/corpus/v1/l.mp3"; verify 1 'a symbolic link'; rm "$vk/corpus/v1/l.mp3"
touch "$vk/corpus/.hidden"; verify 1 'a dot file'; rm "$vk/corpus/.hidden"
sed -n 1p "$vk/SHA256SUMS" >> "$vk/SHA256SUMS"; verify 1 'a repeated line'; sums
echo 'not a line' >> "$vk/SHA256SUMS"; verify 1 'a bad line'; sums; verify 0 'clean again'
printf '%s' 'bad final line' >> "$vk/SHA256SUMS"; verify 1 'a bad final line without a newline'; sums
printf '%s' "$(tail -1 "$vk/SHA256SUMS")" >> "$vk/SHA256SUMS"; verify 1 'a repeated final line without a newline'; sums
echo a > "$tmp/outside"; outside_hash="$(shasum -a 256 "$tmp/outside" | cut -d' ' -f1)"
for rel in ../outside corpus/../../outside "$tmp/outside"; do
  printf '%s  %s\n' "$outside_hash" "$rel" >> "$vk/SHA256SUMS"
  verify 1 "outside path $rel"; has "outside path" 'bad manifest path:' "$tmp/vo"; sums
done
mkdir "$tmp/find"
cat > "$tmp/find/find" << 'EOF'
#!/bin/bash
/usr/bin/find "$@"
case "$*: $TEST_FIND" in *'-quit: first' | '. -type f: second') exit 1 ;; esac
EOF
chmod +x "$tmp/find/find"
TEST_FIND=first PATH="$tmp/find:$PATH" verify 1 'first find failed'
TEST_FIND=second PATH="$tmp/find:$PATH" verify 1 'second find failed'

# --- start with the real seat check and rb_start, with the external effects stubbed
g="$tmp/guest"; mkdir -p "$g/rbst"
cp "$here/kit/"{midi-auto.sh,watch-lib.sh} "$g/"
cat > "$g/kit-lib.sh" << 'EOF'
. "$TEST_KIT_LIB"
ifconfig() {
  echo "ifconfig $*" >> "$B/calls"
  printf '%s\n' "${TEST_EN0}"
  return "${TEST_EN0_RC:-0}"
}
rb_running() { return 1; }
rb_version() { echo test; }
midi_up() { echo midi_up >> "$B/calls"; }
midi_clear() { :; }
first_midi() { return 0; }
nap() { :; }
as_user() { echo "$*" >> "$B/calls"; }
screen() { echo 'ok 0'; }
shot_line() { echo 'shot /s.png'; }
EOF
guest_start() {   # NAME RC EN0-ADDRESS [EXTRA START ARGS...]
  local name="$1" want="$2" en0="$3" rc; shift 3
  : > "$g/rbst/calls"; echo old > "$g/rbst/watch-base.txt"
  TEST_KIT_LIB="$here/kit/kit-lib.sh" TEST_EN0="ether $en0" RBKIT_B="$g/rbst" RBKIT_KIT="$g" \
    bash "$g/midi-auto.sh" start "$@" > "$g/out" 2>&1
  rc=$?
  [ "$rc" = "$want" ] && ok || { no "$name (exit $rc, expected $want)"; cat "$g/out"; }
  if [ "$want" = 0 ]; then
    [ "$(head -1 "$g/rbst/calls")" = 'ifconfig en0' ] && ok || no "$name: en0 wasn't checked before MIDI or open"
    has "$name" 'open -a' "$g/rbst/calls"
  else
    has "$name" 'start: STOP:' "$g/out"
    has "$name" '(rekordbox not started)' "$g/out"
    hasnt "$name" 'open -a' "$g/rbst/calls"
    hasnt "$name" 'midi_up' "$g/rbst/calls"
    [ ! -e "$g/rbst/caffeinate.pid" ] && grep -qx old "$g/rbst/watch-base.txt" && ok || no "$name: start changed guest state"
  fi
  rm -f "$g/rbst/caffeinate.pid"
}
echo aa:bb:cc:dd:ee:01 > "$g/template-mac"
guest_start 'guest seat matches by hand' 0 AA:BB:CC:DD:EE:01
guest_start 'guest seat and host reading match' 0 AA:BB:CC:DD:EE:01 --mac aa:bb:cc:dd:ee:01
guest_start 'guest seat differs from template' 2 aa:bb:cc:dd:ee:02
guest_start 'guest seat differs from host' 2 aa:bb:cc:dd:ee:01 --mac aa:bb:cc:dd:ee:02
guest_start 'missing host MAC' 2 aa:bb:cc:dd:ee:01 --mac
guest_start 'malformed host MAC' 2 aa:bb:cc:dd:ee:01 --mac bad
guest_start 'missing en0 MAC' 2 ''
guest_start 'malformed en0 MAC' 2 malformed
TEST_EN0_RC=1 guest_start 'en0 read fails even with matching stdout' 2 aa:bb:cc:dd:ee:01
echo malformed > "$g/template-mac"
guest_start 'malformed staged MAC' 2 aa:bb:cc:dd:ee:01
rm "$g/template-mac"
guest_start 'missing staged MAC' 2 aa:bb:cc:dd:ee:01

# --- launchd must own the live MIDI PID, and quit may signal only DJ's caffeinate
guest_midi_up() (
  RBKIT_B="$g/rbst"; . "$here/kit/kit-lib.sh"
  launchctl() {
    echo "$*" >> "$B/launchctl"
    case "$1" in
      print)
        [ "$2" = "gui/$UID_/$MIDI_LABEL" ] || return 1
        if [ -f "$B/bootstrapped" ]; then printf '\tpid = %s\n' "$$"
        else
          case "$midi_job" in
            absent) return 1 ;;
            other) printf '\tpid = %s0\n' "$$" ;;
            failed) printf '\tpid = %s\n' "$$"; return 1 ;;
            matching) printf '\tpid = %s\n' "$$" ;;
          esac
        fi ;;
      bootstrap) echo "ready $$" > "$B/midi-ready"; touch "$B/bootstrapped" ;;
    esac
  }
  sleep() { :; }
  midi_up
)
mkfifo "$g/rbst/midi-ctl"
for midi_job in absent other failed matching; do
  rm -f "$g/rbst/bootstrapped" "$g/rbst/launchctl"
  echo "ready $$" > "$g/rbst/midi-ready"  # the PID belongs to this live test shell
  guest_midi_up > "$g/out" && ok || no "midi_up: launchctl $midi_job: $(cat "$g/out")"
  if [ "$midi_job" = matching ]; then
    hasnt 'MIDI job with matching live PID' 'bootstrap' "$g/rbst/launchctl"
  else
    has "midi_up detects live unrelated PID ($midi_job)" 'bootstrap gui/' "$g/rbst/launchctl"
  fi
done

guest_caffeinate_down() (
  RBKIT_B="$g/rbst" RBKIT_USER=dj; . "$here/kit/kit-lib.sh"
  # Record any signal without sending it.
  kill() { printf '%s\n' "$*" > "$B/signal"; }
  ps_status="$1"
  ps() { [ "$*" = '-p 42 -o user=,comm=' ] && printf '%s\n' "$ps_status"; }
  caffeinate_down
)
for process in 'dj /usr/bin/caffeinate' 'root /usr/bin/caffeinate' 'dj /bin/sh' ''; do
  echo 42 > "$g/rbst/caffeinate.pid"; rm -f "$g/rbst/signal"
  guest_caffeinate_down "$process"
  [ ! -e "$g/rbst/caffeinate.pid" ] && ok || no "quit left caffeinate.pid for '$process'"
  case "$process" in
    dj\ /usr/bin/caffeinate) has "quit signals DJ's caffeinate" 42 "$g/rbst/signal" ;;
    *) [ ! -e "$g/rbst/signal" ] && ok || no "quit signaled '$process'" ;;
  esac
done

# The sort check also treats a failed vote command as an inconclusive read.
out="$(
  RBKIT_B="$g/rbst"; . "$here/kit/kit-lib.sh"
  RBKIT=/usr/bin/false
  vote 'alpha-one'
)"
[ "$out" = -1 ] && ok || no "failed vote should report -1 (got '$out')"

# --- the dialog watch (watch-lib.sh as midi-auto.sh sources it), screen and shot_line stubbed:
# each watch_ function sets WATCH_LINE and prints nothing
cat > "$tmp/watch.sh" << 'EOF'
B="$1"; . "$2"
screen() { cat "$B/next"; [ "$(head -1 "$B/next")" != FAIL ]; }
shot_line() { echo "shot /s.png"; }
W() { { echo "ok $#"; printf '%s\n' "$@"; } > "$B/next"; }
T() {   # NAME WANTRC PATTERN (WATCH_LINE, a glob; "" for none)
  local rc="$4"
  if [ "$rc" = "$2" ] && { [ -z "$3" ] && [ -z "$WATCH_LINE" ] || [ -n "$3" ] && [[ "$WATCH_LINE" == $3 ]]; }; then echo "ok $1"
  else echo "bad $1: rc $rc, line '$WATCH_LINE'"; fi
}
MAIN_TITLE=rekordbox; NEWS_TITLE='rekordbox News'
rb="rekordbox | $MAIN_TITLE | 1600x900 | 0 | 10"; dock='Dock |  | 1600x60 | 20 | 2'; app='RB Stems Plus | RB Stems Plus | 800x600 | 0 | 20'
agent="rekordboxAgent | $NEWS_TITLE | 600x400 | 0 | 30"; dlg='rekordbox | Sign in | 1600x900 | 0 | 40'; test='RBKit Screen | RBSP watch test | 480x200 | 0 | 50'
activation='rekordboxAgent | Activation required | 600x400 | 0 | 31'
W; o="$(watch_start t; echo "rc=$?")"; [ "$o" = rc=0 ] && echo "ok nothing printed" || echo "bad printed: $o"
W; watch_start t; T 'empty list' 0 '' $?
[ -z "$BASE" ] && echo "ok empty baseline" || echo "bad empty baseline: $BASE"
W "$rb" "$dock"; watch_start t allow rekordbox; T 'start' 0 '' $?
W "$rb" "$dock"; watch_stop t; T 'no change' 0 '' $?
W "$rb" 'rekordbox |  | 400x300 | 0 | 11'; watch_stop t; T 'another window of rekordbox' 1 't: STOP: new window(s): rekordbox |  | 11; shot /s.png' $?
W "rekordbox | $MAIN_TITLE | 1600x900 | 0 | 12"; watch_stop t; T 'the main window replaced' 1 't: STOP: new window(s): rekordbox | rekordbox | 12;*' $?
echo 'FAIL no answer from RBKit Screen' > "$B/next"; watch_stop t; T 'failed poll' 1 't: STOP: the window list failed*' $?
echo 'ok 1' > "$B/next"; watch_stop t; T 'truncated window list' 1 't: STOP: the window list failed*' $?
printf 'ok 0\n%s\n' "$dock" > "$B/next"; watch_stop t; T 'unexpected extra window row' 1 't: STOP: the window list failed*' $?
W "$rb" "$test"; watch_stop t; T 'test window marked' 1 '*: RBKit Screen | RBSP watch test | 50 (watch test); shot /s.png' $?
W "$rb" "$test" "$dlg"; watch_stop t; T 'test window and a dialog' 1 '*RBSP watch test | 50; rekordbox | Sign in | 40; shot*' $?
W "$rb" 'rekordbox | Sign in | 500x400 | 0'; watch_stop t; T 'four fields' 1 't: STOP: the window list failed*' $?
W "$rb" 'rekordbox | Sign in | 500x400 | 0 | 0x40'; watch_stop t; T 'a bad window number' 1 't: STOP: the window list failed*' $?
W "$rb" 'rekordbox | Sign in | big | 0 | 41'; watch_stop t; T 'a bad size' 1 't: STOP: the window list failed*' $?
W "$rb" 'UserNotificationCenter |  | 520x200 | 0 | 60'; watch_stop t; T "a macOS prompt" 1 '*new window(s): UserNotificationCenter |  | 60;*' $?
# macOS's own permission prompt, raised by a screenshot without the permission (seen in the VM)
W "$rb" 'universalAccessAuthWarn |  | 461x181 | 0 | 34'; watch_stop t; T "macOS's permission prompt" 1 '*new window(s): universalAccessAuthWarn |  | 34;*' $?
# the VM's menu bar and its status items, as they are listed there
W "$rb" 'Window Server | Menubar | 1920x30 | 24 | 17' 'Control Center |  | 42x30 | 25 | 25' 'rekordboxAgent |  | 30x22 | 25 | 61'; watch_stop t; T "the menu bar and the status items" 0 '' $?
W "$rb" 'rekordbox |  | 10x10 | 0 | 62'; watch_stop t; T 'a tiny window' 0 '' $?
W "$rb" 'Finder | Downloads | 800x600 | 0 | 63'; watch_stop t; T 'an unwatched app' 0 '' $?
W "$rb" "$dlg"; watch_start t allow rekordbox; T 'a dialog at the start' 1 't: STOP: not just*Sign in*' $?
W "$dlg"; watch_start t allow rekordbox; T 'lone sign-in at the start' 1 't: STOP: not just*Sign in*' $?
W "$rb" "$activation"; watch_start t allow rekordbox; T 'agent activation at the start' 1 't: STOP: not just*Activation required*' $?
W 'rekordbox | 1 | 1600x900 | 0 | 10'; MAIN_TITLE=001 watch_start t allow rekordbox
T 'numeric-looking titles still match as text' 1 't: STOP: not just*' $?
W 'rekordbox |  | 1600x900 | 0 | 10' 'rekordboxAgent |  | 600x400 | 0 | 30'
watch_start t allow rekordbox; T 'blank titles at the start' 1 't: STOP: not just*' $?
W "rekordbox | $MAIN_TITLE | 799x500 | 0 | 10"; watch_start t allow rekordbox; T 'main title on too narrow a window' 1 't: STOP: not just*' $?
W "rekordbox | $MAIN_TITLE | 800x499 | 0 | 10"; watch_start t allow rekordbox; T 'main title on too short a window' 1 't: STOP: not just*' $?
W; watch_start t allow rekordbox; T 'no rekordbox at the start' 1 't: STOP: not just*' $?
W "$app"; watch_start t allow; T 'the app alone' 0 '' $?
W "$rb" "$app" 'RB Stems Plus |  | 400x200 | 0 | 21'; watch_start t allow; T 'two app windows' 1 't: STOP: not just*' $?
W "$rb" "$app" "$agent"; watch_start t allow rekordbox; T "rekordbox, the app and the agent's news" 0 '' $?
W "$rb" "$agent" 'rekordboxAgent |  | 300x200 | 0 | 31'; watch_start t allow rekordbox; T 'two agent windows' 1 't: STOP: not just*' $?
W "$rb" "$app"; watch_start t allow rekordbox; watch_save
W "$rb" "$app" "$dlg"; watch_quit; T "quit, run's baseline" 1 'quit: STOP: new window(s): rekordbox | Sign in | 40;*' $?
rm -f "$WATCH_BASE"; W "$rb"; watch_quit; T 'quit, no baseline' 0 '' $?
W "$rb" "$dlg"; watch_quit; T 'quit, no baseline, a dialog' 1 'quit: STOP: not just*' $?
W "$rb" "$activation"; watch_quit; T 'quit, no baseline, agent activation' 1 'quit: STOP: not just*Activation required*' $?
W "$rb"; NEWS_TITLE= watch_quit; T 'quit, no baseline, uncalibrated' 1 'quit: STOP: screen.conf needs*' $?
EOF
mkdir -p "$tmp/w"
bash "$tmp/watch.sh" "$tmp/w" "$here/kit/watch-lib.sh" > "$tmp/wo" 2>&1
n="$(grep -c '^ok ' "$tmp/wo")"; pass=$((pass + n))
[ "$n" = 37 ] && ! grep -qv '^ok ' "$tmp/wo" || { no "the dialog watch ($n of 37 OK):"; grep -v '^ok ' "$tmp/wo" | sed 's/^/    /'; }

# --- midi-auto.sh's verbs as written, over a model of rekordbox (kit-lib.sh replaced; watch-lib.sh
# real; the matching by the rbkit built above). The model: the list (Up/Down/SortSong/Load) gives
# deck 1's title; a Load writes the bridge's lines; sleeps advance a clock in tenths of a second.
# SIM_EV: KEY=ACTION,... where KEY is a press and its count (Load#2) or said~TEXT (a line said),
# and ACTION dialog (a new window), stop (midi-stop), write (a bridge line on each sleep for 10 s),
# lose (that press lost), silent, near or diagnostic (what Load logs), unreadable (the deck OCR
# can't match the track), gone (rekordbox quits); desc: the list starts Z to A.
# SIM_MODE miss: a Load writes a miss.
if [ -n "$rbk" ]; then
  k="$tmp/mk"; mkdir -p "$k/corpus/v1"; cp "$here/kit/midi-auto.sh" "$here/kit/watch-lib.sh" "$k/"
  touch "$k/corpus/v1/alpha-one.mp3" "$k/corpus/v1/bravo-two.flac" "$k/corpus/v1/charlie-three.wav" "$k/corpus/v1/notes.txt"
  cat > "$k/kit-lib.sh" << 'EOF'
K="$RBKIT_KIT"; B="$SIM/rbst"; mkdir -p "$B/shots"; LOG="$B/midi-auto.log"; BRIDGE="$SIM/bridge.log"; U=dj; RBKIT="$SIM_RBKIT"
DECK1="1 1 1 1"; HEADER="1 1 1 1"; WALK="AreaLeft 5 250; wait 800; AreaRight 1 250"
MAIN_TITLE=rekordbox; NEWS_TITLE='rekordbox News'
[ -z "${SIM_NOCONF:-}" ] || { WALK=""; MAIN_TITLE=""; NEWS_TITLE=""; }
st() { cat "$SIM/$1" 2> /dev/null || echo "${2:-0}"; }
put() { printf '%s\n' "$2" > "$SIM/$1"; }
ev() {
  local e kk a r="" IFS=,
  for e in ${SIM_EV:-}; do
    [[ "$e" == *=* ]] || continue
    kk="${e%%=*}"; a="${e#*=}"
    if [ "$kk" = "$1" ] || { [[ "$kk" == said~* ]] && [[ "$1" == said:* ]] && [[ "$1" == *"${kk#said~}"* ]]; }; then
      case "$a" in
        dialog) touch "$SIM/dialog" ;; stop) touch "$B/midi-stop" ;; gone) touch "$SIM/gone" ;;
        write) put write-until $(($(st clock) + 100)) ;; lose | silent | near | diagnostic | unreadable) r="$a" ;;
      esac
    fi
  done
  echo "$r"
}
say() { local l="10:00:00  $*"; printf '%s\n' "$l"; printf '%s\n' "$l" >> "$LOG"; ev "said:$*" > /dev/null; }
now() { echo $(($(st clock) / 10)); }
nap() { put clock $(($(st clock) + $(awk -v s="$1" 'BEGIN { printf "%d", s * 10 + 0.5 }'))); [ "$(st clock)" -ge "$(st write-until)" ] || echo w >> "$BRIDGE"; }
within() { shift; "$@"; }
screen() {
  case "$1" in
    access) echo "ok screen ${SIM_ACCESS:-yes}" ;;
    windows)
      local -a w=('rekordbox | rekordbox | 1600x900 | 0 | 10' 'Dock |  | 1600x60 | 20 | 2')
      [ ! -e "$SIM/dialog" ] || w+=('rekordbox | Sign in | 500x400 | 0 | 40')
      [ ! -e "$SIM/testwin" ] || w+=('RBKit Screen | RBSP watch test | 480x200 | 0 | 50')
      printf '%s\n' "ok ${#w[@]}" "${w[@]}" ;;
  esac
}
shot() { local p="$B/shots/$RANDOM$RANDOM.png"; : > "$p"; echo "$p"; }
shot_line() { echo "shot $(shot)"; }
shot_err() { echo "no permission"; }
drop() { [ -z "${1:-}" ] || rm -f "$1"; }
testwindow() { touch "$SIM/testwin"; echo "testwindow: shown"; }
closetest() { rm -f "$SIM/testwin"; echo "closetest: ok"; }
rb_running() { [ ! -e "$SIM/gone" ]; }
seat_ok() { return 0; }
rb_version() { echo 7.2.19; }
rb_start() { echo "started rekordbox 7.2.19"; }
rb_quit() { echo quit >> "$SIM/presses"; touch "$SIM/gone"; }
caffeinate_down() { :; }
cache_state() { echo "${SIM_CACHE:-Stems Cache installed}"; }
lines() { if [ -f "$BRIDGE" ]; then wc -l < "$BRIDGE" | tr -d ' '; else echo 0; fi; }
lines_since() { tail -n +"$(($1 + 1))" "$BRIDGE"; }
midi_up() { echo "the MIDI device RBSP Test is up"; }
midi_clear() { :; }
first_midi() { true; }
play_light() { echo "${SIM_PLAY:-5 0}"; }
press() {
  local n t="${2:-1}" a rows=() l
  case "$1" in Down | Load | Up | Play | SortSong | AreaLeft | AreaRight) ;; *) echo "unknown control $1"; return 1 ;; esac
  n=$(($(st "count-$1") + 1)); put "count-$1" "$n"; echo "$1#$n x$t" >> "$SIM/presses"
  a="$(ev "$1#$n")"; [ "$a" = lose ] && return 0
  [ -f "$B/rows.txt" ] || return 0
  while IFS= read -r l; do rows+=("$l"); done < "$B/rows.txt"
  local p; p="$(st pos)"
  case "$1" in
    Up) p=$((p - t)); [ "$p" -ge 0 ] || p=0; put pos "$p" ;;
    Down) p=$((p + t)); [ "$p" -lt "${#rows[@]}" ] || p=$((${#rows[@]} - 1)); put pos "$p" ;;
    SortSong) if [ "$(st desc)" = 1 ]; then put desc 0; else put desc 1; fi ;;
    Load)
      put deck-ocr "$a"
      if [ "$(st desc)" = 1 ]; then put deck "${rows[$((${#rows[@]} - 1 - p))]}"; else put deck "${rows[$p]}"; fi
      case "$a" in
        silent) ;;
        near) echo '2026-11-19 10:00:00  near 0123456789ab for 0123456789ac: served' >> "$BRIDGE" ;;
        diagnostic) echo '2026-11-19 10:00:00  diagnostic' >> "$BRIDGE" ;;
        *)
          if [ "${SIM_MODE:-}" = miss ]; then echo '2026-11-19 10:00:00  miss 0123456789ab (441000 samples)  [0 hits, 0 near, 1 misses]' >> "$BRIDGE"
          else echo '2026-11-19 10:00:00  hit  0123456789ab (441000 samples)  [1 hits, 0 near, 0 misses]' >> "$BRIDGE"; fi ;;
      esac ;;
  esac
}
press_note() { echo "note $1" >> "$SIM/presses"; }
ocr_deck1() { local d; d="$(st deck '')"; [ "$(st deck-ocr)" != unreadable ] || d='unreadable text'; printf '%s\n' "$d" "$d" "$d" ""; }
ocr_header() { echo "${SIM_HEADER:-RBSP Tests ($(grep -c . "$B/rows.txt") Tracks)}"; }
vote() { "$RBKIT" vote "$B/rows.txt" "$@"; }
norm() { [ -z "${SIM_NORM_FAIL:-}" ] && "$RBKIT" norm "$1"; }
EOF
  sim() {   # NAME EVENTS VERB ARGS...: its output in $tmp/sim-NAME/out (presses in presses), exit in rc
    local d="$tmp/sim-$1" ev="$2"; shift 2; rm -rf "$d"; mkdir -p "$d/rbst"; : > "$d/presses"
    [ "${ev#desc}" = "$ev" ] || echo 1 > "$d/desc"
    [ "${ev#dialog0}" = "$ev" ] || touch "$d/dialog"
    SIM="$d" SIM_EV="$ev" SIM_RBKIT="$rbk" RBKIT_KIT="$k" bash "$k/midi-auto.sh" "$@" > "$d/out" 2>&1; echo $? > "$d/rc"
  }
  simck() {   # NAME RC [+TEXT|-TEXT]...: the exit, and TEXT in (+) or not in (-) its output and presses
    local n="$1" d="$tmp/sim-$1" want="$2" c; shift 2
    [ "$(cat "$d/rc")" = "$want" ] && ok || { no "midi-auto $n: exit $(cat "$d/rc"), expected $want"; sed 's/^/    /' "$d/out"; }
    for c in "$@"; do
      if grep -qF -- "${c:1}" "$d/out" "$d/presses"; then [ "${c:0:1}" = + ] && ok || no "midi-auto $n: '${c:1}' found"
      else [ "${c:0:1}" = - ] && ok || { no "midi-auto $n: no '${c:1}'"; sed 's/^/    /' "$d/out"; }; fi
    done
  }
  r1=(run --start-wait 1 --quiet 1 --max-track 4)
  sim clean '' "${r1[@]}"
  simck clean 0 '+row 1 alpha-one : read back OK' '+row 3 charlie-three : read back OK' '-STOP' '+run: expect any: 3 rows OK, 0 not' '+AreaLeft#1 x5' '-notes'
  sim desc 'desc' "${r1[@]}"
  simck desc 0 "+run: row 1 reads 'charlie-three / charlie-three / charlie-three', not alpha-one: SortSONG (try 1)" '+SortSong#1' '+run: expect any: 3 rows OK, 0 not'
  # RKM19: retry OCR in the current direction. A matched other row permits one flip.
  sim unreadable 'Load#1=unreadable' "${r1[@]}"
  simck unreadable 0 '+Load#2' '+run: expect any: 3 rows OK, 0 not' '-SortSong#'
  sim unreaddesc 'desc,Load#1=unreadable,Load#3=unreadable' "${r1[@]}"
  simck unreaddesc 0 "+run: row 1 reads 'charlie-three / charlie-three / charlie-three', not alpha-one: SortSONG (try 2)" '+Load#4' '+run: expect any: 3 rows OK, 0 not' '-SortSong#2'
  sim unreadtwice 'Load#1=unreadable,Load#2=unreadable' "${r1[@]}"
  simck unreadtwice 1 '+Load#2' '+run: STOP: could not put row 1 first' '-SortSong#' '-Load#3'
  sim lostflip 'desc,SortSong#1=lose' "${r1[@]}"
  simck lostflip 1 '+SortSong#1' '+run: STOP: could not put row 1 first' '-SortSong#2'
  sim lostload 'Load#2=lose' "${r1[@]}"
  simck lostload 0 '+row 2 bravo-two : deck 1 reads' '+trying Load again' '+run: expect any: 3 rows OK, 0 not'
  sim lostdown 'Down#1=lose' "${r1[@]}"
  simck lostdown 0 '+deck 1 still has alpha-one: stepping again' '+row 2 bravo-two : read back OK' '+run: expect any: 3 rows OK, 0 not'
  sim resetrow 'Down#1=lose,Down#2=lose' "${r1[@]}"
  simck resetrow 0 '+finding the row from the top again' '+run: expect any: 3 rows OK, 0 not'
  sim sortdlg 'Load#1=dialog' "${r1[@]}"
  simck sortdlg 1 '+run: STOP: new window(s): rekordbox | Sign in | 40; shot ' '-Load#2' '-quit'
  # RKM10: a dialog appears during navigation, after the earlier window check.
  sim updlg 'Up#1=dialog' "${r1[@]}"
  simck updlg 1 '+Up#1' '+run: STOP: new window(s): rekordbox | Sign in' '-Load#1'
  sim downdlg 'Down#1=dialog' "${r1[@]}"
  simck downdlg 1 '+Down#1' '+run: STOP: new window(s): rekordbox | Sign in' '-Load#2'
  sim recoverdlg 'Down#1=lose,Down#2=dialog' "${r1[@]}"
  simck recoverdlg 1 '+Down#2' '+run: STOP: new window(s): rekordbox | Sign in' '-Load#4'
  sim stoprow 'Load#2=stop' "${r1[@]}"
  simck stoprow 1 '+run: STOP: midi-stop' '-Load#3'
  sim stoplast 'said~row 3 charlie-three : read back=stop' "${r1[@]}"
  simck stoplast 1 '+row 3 charlie-three : read back OK' '+run: STOP: midi-stop'
  sim gone 'Load#2=gone' "${r1[@]}"
  simck gone 1 '+run: STOP: rekordbox is gone' '-Load#3'
  sim timeout 'Load#2=write' run --start-wait 1 --quiet 2 --max-track 5
  simck timeout 1 '+run: STOP: row 2 bravo-two : the bridge still writing after 5 s' '-Load#3'
  SIM_MODE=miss sim misses '' "${r1[@]}" --expect hits
  simck misses 3 '+row 1 alpha-one : EXPECT FAIL: 1 misses, 0 refused, 0 hits' '+run: expect hits: 0 rows OK, 3 not' '-STOP'
  sim silent 'Load#2=silent' "${r1[@]}"
  simck silent 3 '+row 2 bravo-two : EXPECT FAIL: loaded (read back), but no bridge activity in 1 s' '+run: expect any: 2 rows OK, 1 not'
  sim silentprobe 'desc,Load#1=silent' "${r1[@]}" --expect hits
  simck silentprobe 3 "+row 3 charlie-three : EXPECT FAIL (the sort check's load): no bridge activity in 1 s" '+run: expect hits: 3 rows OK, 1 not'
  # RKM20: bridge activity alone is insufficient, including for a recovery probe.
  sim nearprobe 'desc,Load#1=near' "${r1[@]}" --expect hits
  simck nearprobe 3 "+row 3 charlie-three : EXPECT FAIL (the sort check's load): 0 misses, 0 refused, 0 hits" '+run: expect hits: 3 rows OK, 1 not'
  sim diagprobe 'Down#1=lose,Load#2=diagnostic' "${r1[@]}" --expect hits
  simck diagprobe 3 "+row 1 alpha-one : EXPECT FAIL (the sort check's load): 0 misses, 0 refused, 0 hits" '+run: expect hits: 3 rows OK, 1 not'
  # Inconclusive OCR forces two loads of the same selection. Each has its own hit.
  sim repeathit 'Load#1=unreadable' "${r1[@]}" --expect hits --max 1
  simck repeathit 0 '+Load#1' '+Load#2' '-Load#3' '-Down#' '-SortSong#' '+0 misses, 1 hits, 0 refused' '+run: expect hits: 1 rows OK, 0 not'
  [ "$(grep -cE '  hit  [^ ]+ \([0-9]+ samples' "$tmp/sim-repeathit/bridge.log")" = 2 ] && ok || no "midi-auto repeathit: expected two hits for two loads of alpha-one"
  sim repeatnohit 'Load#1=unreadable,Load#2=diagnostic' "${r1[@]}" --expect hits --max 1
  simck repeatnohit 3 '+row 1 alpha-one : EXPECT FAIL: 0 misses, 0 refused, 0 hits' '+run: expect hits: 0 rows OK, 1 not'
  sim inject '' "${r1[@]}" --inject-at 2
  simck inject 1 '+run: STOP: new window(s): RBKit Screen | RBSP watch test | 50 (watch test);' '+run: closetest: ok' '-Load#2'
  sim max '' "${r1[@]}" --max 2
  simck max 0 '+run: --max 2 loads reached' '+run: expect any: 2 rows OK, 0 not' '-Load#3'
  sim range '' "${r1[@]}" --from bravo-two --to bravo-two
  simck range 0 '+run: rows 2-2 of 3 (bravo-two .. bravo-two)' '+row 2 bravo-two : read back OK' '+run: expect any: 1 rows OK, 0 not'
  SIM_HEADER='Collection (99 Tracks)' sim header '' "${r1[@]}"
  simck header 1 "+run: STOP: the list header reads 'Collection (99 Tracks)', not 'RBSP Tests (3 Tracks)'" '-Load#1'
  SIM_NORM_FAIL=1 sim normfail '' "${r1[@]}"
  simck normfail 1 '+run: STOP: the list header reads' '-Load#1'
  sim startdlg 'dialog0' "${r1[@]}"
  simck startdlg 1 "+run: STOP: not just rekordbox's, its agent's news and our app's main windows at the start" '-AreaLeft'
  sim badfrom '' "${r1[@]}" --from nope; simck badfrom 2 "+run: no row named 'nope'" '-AreaLeft'
  sim badinject '' "${r1[@]}" --inject-at 9; simck badinject 2 '+run: --inject-at 9: not in rows 1-3' '-AreaLeft'
  sim injectmax '' "${r1[@]}" --inject-at 3 --max 2; simck injectmax 2 '+past --max 2 (it would be load 3)'
  SIM_NOCONF=1 sim noconf '' "${r1[@]}"; simck noconf 2 '+run: STOP: screen.conf needs WALK, MAIN_TITLE, NEWS_TITLE' '-AreaLeft'
  SIM_ACCESS=no sim noaccess '' "${r1[@]}"; simck noaccess 2 '+run: STOP: RBKit Screen needs Screen Recording' '-AreaLeft'
  for expectation in any hits; do
    SIM_CACHE=pioneer sim "nocache-$expectation" '' ready --expect "$expectation"
    simck "nocache-$expectation" 2 '+ready: STOP: Stems Cache must be installed' '-AreaLeft'
  done
  sim notrunning 'Load#9=x' run; touch "$tmp/sim-notrunning/gone"
  SIM="$tmp/sim-notrunning" SIM_RBKIT="$rbk" RBKIT_KIT="$k" bash "$k/midi-auto.sh" run > "$tmp/sim-notrunning/out" 2>&1; echo $? > "$tmp/sim-notrunning/rc"
  simck notrunning 2 '+run: rekordbox not running'
  sim quit '' quit; simck quit 0 '+quit: deck 1 not playing (5 PLAY light changes in 2 s)' '+quit: rekordbox closed' '+quit' '-Play#'
  SIM_PLAY='1 1' sim quitplay '' quit; simck quitplay 0 '+quit: deck 1 is playing: PlayPause first' '+Play#1 x1' '+quit: rekordbox closed'
  sim quitdlg 'dialog0' quit; simck quitdlg 1 "+quit: STOP: not just rekordbox's" '-Play#'
  grep -qx quit "$tmp/sim-quitdlg/presses" && no "quit asked rekordbox to quit over a dialog" || ok
  sim watchtest '' watchtest; simck watchtest 0 '+watchtest: STOP: new window(s): RBKit Screen | RBSP watch test | 50 (watch test); shot ' '+watchtest: PASS'
  sim watchdlg 'dialog0' watchtest; simck watchdlg 0 '+watchtest: PASS'
  sim learn '' learn --note 74 --repeat 5; simck learn 0 '+learn: note 74 pressed 3 times over 5 s' '+note 74'
  sim learnbad '' learn --note 200 --repeat 5; simck learnbad 2 '+learn: --note 0-127'
  sim press '' press --keys 'Down x3,Load'; simck press 0 '+Down#1 x3' '+Load#1 x1' '+press Down x3,Load: shot '
  sim pressbad '' press --keys 'Sideways'; simck pressbad 1 '+press: STOP: unknown control Sideways'
  sim prepare '' prepare; simck prepare 2 '+prepare: not on the Mac driver yet'
  sim unknown '' bogus; simck unknown 2
  sim overflow '' run --minutes 99999999999999999999
  simck overflow 2 '+--minutes: expected 1-1440' '-AreaLeft'
fi

# --- vm-session.sh in a copy of the toolkit in its own git repository, everything it calls stubbed
r="$tmp/repo"; mkdir -p "$r/tests/rekordbox"; cp -R "$here" "$r/tests/rekordbox/mac"
git -C "$r" init -q
S="$r/tests/rekordbox/mac/vm-session.sh"
F="$tmp/fake"; mkdir -p "$F/bin" "$tmp/state/ssh" "$tmp/tart/vms/rb26-template"
echo rbkit-test > "$tmp/state/clone"; : > "$tmp/state/ssh/id"
echo '{"macAddress": "AA:BB:CC:DD:EE:01", "os": "darwin"}' > "$tmp/tart/vms/rb26-template/config.json"
# ssh: the remote command logged (one line), answered by what it does: $FAKE/NAME-out (else a
# default) printed, exit $FAKE/NAME-rc (else 0)
cat > "$F/bin/ssh" << 'EOF'
#!/bin/bash
remote="${@: -1}"
printf '%s\n' "$remote" >> "$FAKE/calls"
[ -z "${FAKE_RC:-}" ] || exit "$FAKE_RC"
answer() { if [ -f "$FAKE/$1-out" ]; then cat "$FAKE/$1-out"; else printf '%b' "$2"; fi; exit "$(cat "$FAKE/$1-rc" 2> /dev/null || echo 0)"; }
case "$remote" in
  *'echo reached'*) answer reached 'reached 26.6.2\n' ;;
  *'ifconfig en0'*) answer mac 'aa:bb:cc:dd:ee:01\n' ;;
  *'tar -xof -'*)
    cat > "$FAKE/tar"
    if [ -d "$FAKE/guest-kit" ]; then
      for part in corpus dist; do
        [[ "$remote" != *"sudo rm -rf /Users/Shared/rbsp-kit/$part &&"* ]] || rm -rf "$FAKE/guest-kit/$part"
      done
      tar -xf "$FAKE/tar" -C "$FAKE/guest-kit" || exit 1
    fi
    answer tar '' ;;
  *'cat /Users/Shared/rbsp-kit/SHA256SUMS'*)
    if [ -d "$FAKE/guest-kit" ]; then cat "$FAKE/guest-kit/SHA256SUMS"; exit; fi
    answer sums '' ;;
  *'verify-kit.sh'*)
    if [ -d "$FAKE/guest-kit" ]; then exec bash "$FAKE/guest-kit/verify-kit.sh"; fi
    answer verify 'SHA256SUMS: 10 files present, 0 bad\n' ;;
  *'midi-auto.sh ensure'*) answer ensure '10:00:00  ensure: 0 not OK\n' ;;
  *'midi-auto.sh ready'*) answer ready '10:00:00  ready: OK\n' ;;
  *'midi-auto.sh start'*) answer start '10:00:00  start: ready\n' ;;
  *'midi-auto.sh run '*)
    if [ -f "$FAKE/stall" ]; then exec perl -e 'sleep 3'; fi
    answer run '10:00:00  run: end\n' ;;
  *'midi-auto.sh quit'*) answer quit '10:00:00  quit: rekordbox closed after 3 s\n' ;;
  *'midi-auto.sh watchtest'*) answer watchtest '10:00:00  watchtest: STOP: new window(s): RBKit Screen | RBSP watch test | 50 (watch test); shot x\n10:00:00  watchtest: PASS\n' ;;
  *'midi-auto.sh bundled '*) [[ "$remote" =~ --keys\ collect-[0-9]{8}-[0-9]{6}\.zip$ ]] || exit 2 ;;
  *'midi-auto.sh bundle'*) printf 'BUNDLE /Users/Shared/rbst/collect-20261119-100000.zip\n' ;;
  *'midi-auto.sh shot'*) answer shot '10:00:00  shot /Users/Shared/rbst/shots/20261119-100000-1.png\n' ;;
  *'midi_down'*) answer down 'pioneer\n' ;;
esac
exit 0
EOF
cat > "$F/bin/scp" << 'EOF'
#!/bin/bash
printf '%s\n' "$*" >> "$FAKE/scp"
src="${@: -2:1}"; dst="${@: -1}"
case "$src" in *.zip) cp "$FAKE/collect-20261119-100000.zip" "$dst/" ;; *.png) : > "$dst/${src##*/}" ;; esac
EOF
cat > "$F/bin/tart" << 'EOF'
#!/bin/bash
case "$1" in
  list)
    state="${FAKE_STATE:-running}"; [ ! -e "$FAKE/stopped" ] || state=stopped
    printf 'Source Name State\nlocal rbkit-test %s\nlocal other-clone running\n' "$state"
    exit "${FAKE_LIST_RC:-0}" ;;
  ip) echo ip >> "$FAKE/ip"; echo 127.0.0.1 ;;
esac
EOF
cat > "$F/bin/xcrun" << 'EOF'
#!/bin/bash
case "$*" in *--show-sdk-path*) echo /sdk ;; *swiftc*) o=""; while [ $# -gt 0 ]; do [ "$1" = -o ] && o="$2"; shift; done; echo "built $o" >> "$FAKE/built"; printf '#!/bin/sh\n' > "$o"; chmod +x "$o" ;; esac
EOF
printf '#!/bin/bash\ncase "$*" in *-dvvv*) echo "CDHash=0123abcd" >&2 ;; esac\n' > "$F/bin/codesign"
printf '#!/bin/bash\nprintf "%%s\\n" "$1" >> "$FAKE/notified"\n' > "$F/notify"
printf '#!/bin/bash\nexit 1\n' > "$F/notify-fails"
cat > "$F/vmsh" << 'EOF'
#!/bin/bash
printf '%s %s\n' "${VM_STATE:-}" "$*" >> "$FAKE/vmsh.log"
if [ "${VM_STATE:-}" = "${FAKE_EXPECT_VM_STATE:-$RBKIT_VM_STATE}" ] && [ -z "${FAKE_DOWN_STAYS_RUNNING:-}" ]; then
  touch "$FAKE/stopped"
fi
exit 0
EOF
chmod +x "$F/bin"/* "$F/notify" "$F/notify-fails" "$F/vmsh"
(cd "$tmp" && mkdir z && echo hello > z/midi-auto.log && cd z && zip -q ../fake/collect-20261119-100000.zip midi-auto.log)
export FAKE="$F" RBKIT_NOTIFY="$F/notify" RBKIT_OUT="$tmp/out" RBKIT_VM_STATE="$tmp/state" TART="$F/bin/tart" TART_HOME="$tmp/tart" RBKIT_VMSH="$F/vmsh" PATH="$F/bin:$PATH"
reset() { rm -f "$F"/{calls,ip,scp,notified,vmsh.log,tar,built,stopped,stall} "$F"/*-out "$F"/*-rc; }
expect() {   # RC NAME ARGS...: vm-session.sh exits with RC
  local want="$1" name="$2"; shift 2
  "$S" "$@" > "$tmp/o" 2>&1; local rc=$?
  if [ "$rc" = "$want" ]; then ok; else no "$name (exit $rc, expected $want)"; sed 's/^/    /' "$tmp/o"; fi
}
notified_once() {
  has "$1" "$2" "$F/notified"
  [ "$(grep -c . "$F/notified" 2> /dev/null)" = 1 ] && ok || no "$1: expected exactly one notification"
}

reset
expect 2 "no arguments"; expect 2 "unknown command" bogus
expect 2 "run without a kind" run; expect 2 "run with an unknown kind" run T3; expect 2 "run prepare" run prepare
expect 2 "--from with a quote" run T2 --from "a'b"; expect 2 "--from with a semicolon" run T2 --from 'a;b'
expect 2 "--max not a number" run T2 --max x; expect 2 "--max 0" run T2 --max 0; expect 2 "--minutes 08" run T2 --minutes 08
expect 2 "--minutes above 1440" run T2 --minutes 1441; expect 2 "--minutes overflow" run hits --minutes 99999999999999999999
expect 2 "--keep-open on watchtest" run watchtest --keep-open; expect 2 "--from without a value" run T2 --from
expect 2 "--dist on preflight" preflight --dist x; expect 2 "stage --corpus without v1" stage --corpus "$tmp"
expect 2 "--keep-running on run" run T2 --keep-running
RBKIT_VM_STATE="$tmp/none" expect 2 "no VM state" preflight
echo rb26-template > "$tmp/state/clone"; expect 2 "the template as the clone" preflight; echo rbkit-test > "$tmp/state/clone"
echo 'a b' > "$tmp/state/clone"; expect 2 "a clone name with a space" preflight; echo rbkit-test > "$tmp/state/clone"
[ ! -s "$F/calls" ] && ok || no "ssh was called for a refused command line"

reset; expect 0 "run T2" run T2 --from 01-mp3-cbr320 --max 3 --inject-at 3
has "run T2" "midi-auto.sh start --mac aa:bb:cc:dd:ee:01" "$F/calls"
has "run T2" "midi-auto.sh ready --expect any" "$F/calls"
[ "$(grep -c '^ip$' "$F/ip")" = 1 ] && ok || no "run T2: expected one Tart IP lookup"
has "run T2" "midi-auto.sh run --expect any --from '01-mp3-cbr320' --max 3 --inject-at 3 --minutes 90" "$F/calls"
has "run T2" "midi-auto.sh quit" "$F/calls"; has "run T2 as root" "sudo /bin/bash /Users/Shared/rbsp-kit/midi-auto.sh" "$F/calls"
[ ! -s "$F/notified" ] && ok || no "run T2: a notification without a STOP"
reset; expect 0 "run hits --keep-open" run hits --keep-open; has "run hits" "midi-auto.sh ready --expect hits" "$F/calls"; hasnt "run hits --keep-open" 'midi-auto.sh quit' "$F/calls"
reset; expect 0 "run T2, upper minutes limit" run T2 --minutes 1440
has "upper minutes limit" 'run --expect any --minutes 1440' "$F/calls"
reset; echo '10:00:00  ready: STOP: screen.conf needs MAIN_TITLE' > "$F/ready-out"; echo 2 > "$F/ready-rc"
expect 2 "run hits, prerequisites missing" run hits
has "missing prerequisites" 'run hits: screen.conf needs MAIN_TITLE' "$F/notified"
hasnt "missing prerequisites" 'midi-auto.sh start' "$F/calls"
# one seat: another MAC address, and rekordbox isn't started
reset; echo 'aa:bb:cc:dd:ee:02' > "$F/mac-out"; expect 1 "run T2 with another MAC address" run T2
hasnt "another MAC" 'midi-auto.sh start' "$F/calls"; has "another MAC" "run T2: the VM's MAC address aa:bb:cc:dd:ee:02 is not rb26-template's aa:bb:cc:dd:ee:01" "$F/notified"
reset; echo 255 > "$F/mac-rc"; expect 1 "run T2, MAC lookup failed with matching stdout" run T2
hasnt "failed MAC lookup" 'midi-auto.sh start' "$F/calls"
has "failed MAC lookup" "the VM's MAC address unread" "$F/notified"
reset; cfg="$tmp/tart/vms/rb26-template/config.json"; mv "$cfg" "$cfg.saved"; : > "$F/mac-out"
expect 1 "run T2, unreadable template and empty en0 MAC" run T2
has "unreadable template and empty en0" "run T2: the VM's MAC address unread is not rb26-template's unread" "$F/notified"
hasnt "unreadable template and empty en0" 'midi-auto.sh start' "$F/calls"
mv "$cfg.saved" "$cfg"
# a STOP, or an unexpected exit: rekordbox is left alone (no quit), the owner notified at once
reset; printf '10:00:00  run: STOP: new window(s): rekordbox | Sign in | 40; shot x\n' > "$F/run-out"; echo 1 > "$F/run-rc"
expect 1 "run T2 with a STOP" run T2; has "run T2 STOP" "Sign in" "$F/notified"; hasnt "run T2 STOP" 'midi-auto.sh quit' "$F/calls"

reset
shot='/Users/Shared/rbst/shots/auto.png'
printf '10:00:00  run: STOP: new window(s): rekordbox | Sign in | 40; file %s; shot %s\n' \
  'C:\Users\rbtest\Music\track.wav' "$shot" > "$F/run-out"
echo 1 > "$F/run-rc"
RBKIT_OUT="$tmp/Users/logs" expect 1 "VM run T2, STOP with screenshot" run T2
notified_once "VM STOP reason" 'run T2: new window(s): rekordbox | Sign in | 40'
hasnt "VM STOP alert leaks screenshot path" '/Users/' "$F/notified"
hasnt "VM STOP alert leaks Windows path" ':\Users' "$F/notified"
grep -F '(notified:' "$tmp/o" | grep -F -- "$shot" | grep -qF -- "$tmp/Users/logs/" && ok || no "VM STOP: notification log lost the paths"

reset; printf '10:00:00  run: STOP: unexpected dialog %0180d\n' 0 > "$F/run-out"; echo 1 > "$F/run-rc"
expect 1 "VM run T2, long STOP reason" run T2
alert="$(cat "$F/notified")"
[ "${#alert}" -le 160 ] && ok || no "VM STOP alert exceeds 160 characters"

reset; echo 1 > "$F/run-rc"; expect 1 "run T2, exit 1 without a STOP line" run T2
has "run T2 exit 1" "exit 1 without a STOP line" "$F/notified"; hasnt "run T2 exit 1" 'midi-auto.sh quit' "$F/calls"
reset; echo 142 > "$F/run-rc"; : > "$F/run-out"
expect 1 "run T2, exit 142 without a STOP line" run T2
notified_once "run exit 142" 'no end in the time allowed'
hasnt "run exit 142" 'midi-auto.sh quit' "$F/calls"
# Shorten the idle interval in the test copy only, preserving the overall cap.
sed -i '' 's/vssh "$secs" 600 --/vssh "$secs" 1 --/' "$S"
reset; touch "$F/stall"; expect 1 "run T2, no output" run T2
notified_once "idle run" 'no output for 1 s'
hasnt "idle run" 'midi-auto.sh quit' "$F/calls"
reset; printf '10:00:00  run: STOP: error\n' > "$F/run-out"; expect 1 "run T2, exit 0 with a STOP line" run T2; hasnt "run T2 exit 0 STOP" 'midi-auto.sh quit' "$F/calls"
reset; printf '10:00:00  start: STOP: no MIDI from rekordbox in 120 s\n' > "$F/start-out"; expect 1 "run T2, start STOPs" run T2
has "start STOP" "no MIDI from rekordbox" "$F/notified"; hasnt "start STOP" 'midi-auto.sh run ' "$F/calls"
reset; printf '10:00:00  run: STOP: new window(s): RBKit Screen | RBSP watch test | 50 (watch test); shot x\n' > "$F/run-out"; echo 1 > "$F/run-rc"
expect 1 "run T2 --inject-at's STOP" run T2 --inject-at 2; has "inject-at" "run T2: [watch test, nothing to do]" "$F/notified"
reset; echo 3 > "$F/run-rc"; expect 3 "run hits with an expectation not met" run hits; has "run hits exit 3" 'midi-auto.sh quit' "$F/calls"
reset; printf '10:00:00  quit: STOP: new window(s): rekordbox | Sign in | 40; shot x\n' > "$F/quit-out"; echo 1 > "$F/quit-rc"
expect 1 "run T2, the quit STOPs" run T2; has "quit STOP" "run T2 quit: new window(s)" "$F/notified"
reset; FAKE_RC=255 expect 1 "run T2, the VM unreachable" run T2; has "unreachable" "the VM's MAC address unread" "$F/notified"
reset; expect 0 "run watchtest" run watchtest; has "watchtest" "run watchtest: [watch test, nothing to do]" "$F/notified"
reset; RBKIT_NOTIFY="$F/notify-fails" expect 1 "run watchtest, the notifier fails" run watchtest
reset; printf '10:00:00  watchtest: FAIL: the window list failed\n' > "$F/watchtest-out"; echo 1 > "$F/watchtest-rc"; expect 1 "run watchtest, no STOP line" run watchtest
reset; FAKE_RC=255 expect 1 "run watchtest, the VM unreachable" run watchtest; has "watchtest unreachable" "can't be reached" "$F/notified"

# preflight
reset; expect 0 "preflight" preflight; has "preflight" "OK    the VM's MAC address is rb26-template's" "$tmp/o"; has "preflight" "midi-auto.sh ensure" "$F/calls"
reset; FAKE_STATE=stopped expect 1 "preflight, the VM stopped" preflight; has "preflight stopped" "isn't running: tests/harness/vm.sh up rbkit-test" "$tmp/o"
reset; echo 'aa:bb:cc:dd:ee:02' > "$F/mac-out"; expect 1 "preflight, another MAC address" preflight
reset; echo 9 > "$F/verify-rc"; expect 1 "preflight, no kit" preflight; has "no kit" "no kit in the VM: run stage" "$tmp/o"; hasnt "no kit" 'midi-auto.sh ensure' "$F/calls"
reset; echo 1 > "$F/ensure-rc"; expect 1 "preflight, ensure not OK" preflight
reset; RBKIT_NOTIFY="$tmp/none" expect 1 "preflight, no notifier" preflight

# stage: the tools built, the kit hashed, checked here, copied into /Users/Shared/rbsp-kit, checked there
mkdir -p "$tmp/corpus/v1"; echo a > "$tmp/corpus/v1/01-x.mp3"
reset; expect 0 "stage" stage --corpus "$tmp/corpus"
st="$(sed -n 's/^stage: [0-9][0-9]* files in \(.*\) (RBKit.*/\1/p' "$tmp/o")"
(cd "$st" && shasum -a 256 -c --quiet SHA256SUMS) && ok || no "stage: SHA256SUMS doesn't check"
for f in midi-auto.sh kit-lib.sh watch-lib.sh verify-kit.sh screen.conf template-mac bin/rbkit 'RBKit Screen.app/Contents/MacOS/rbkit-screen' 'RBKit Screen.app/Contents/Info.plist' corpus/v1/01-x.mp3; do
  grep -qF "  $f" "$st/SHA256SUMS" && ok || no "stage: $f not in SHA256SUMS"
  tar -tf "$F/tar" 2> /dev/null | grep -qxF "./$f" && ok || no "stage: $f not sent"
done
[ "$(cat "$st/template-mac")" = aa:bb:cc:dd:ee:01 ] && ok || no "stage: template-mac differs from template"
has "stage" "code hash 0123abcd" "$tmp/o"; has "stage" "RBKit Screen built anew" "$tmp/o"
has "stage" "sudo rm -rf /Users/Shared/rbsp-kit/corpus && sudo tar -xof - -C /Users/Shared/rbsp-kit" "$F/calls"
has "stage" "verify-kit.sh" "$F/calls"; has "stage" "midi-auto.sh ensure" "$F/calls"
# again: nothing rebuilt (RBKit Screen's permission kept), the VM's corpus kept when none is brought
reset; { (cd "$st" && grep '  corpus/' SHA256SUMS); printf '%064d  dist/payload.json\n' 1; } > "$F/sums-out"; expect 0 "stage again" stage
kept="$(sed -n 's/^stage: [0-9][0-9]* files in \(.*\) (RBKit.*/\1/p' "$tmp/o")"
has "stage kept corpus" '  corpus/v1/01-x.mp3' "$kept/SHA256SUMS"; has "stage kept dist" '  dist/payload.json' "$kept/SHA256SUMS"
[ ! -e "$kept/corpus" ] && ok || no "stage recopied the kept corpus"
[ ! -e "$F/built" ] && ok || no "stage rebuilt unchanged tools: $(cat "$F/built")"
hasnt "stage again" "rm -rf /Users/Shared/rbsp-kit/corpus" "$F/calls"; hasnt "stage again" "built anew" "$tmp/o"
reset
mkdir -p "$F/guest-kit/corpus/v1" "$F/guest-kit/dist" "$tmp/replacement/v1" "$tmp/dist"
echo old > "$F/guest-kit/corpus/v1/old.mp3"; echo old > "$F/guest-kit/dist/old.json"
(cd "$F/guest-kit" && shasum -a 256 corpus/v1/old.mp3 dist/old.json > SHA256SUMS)
echo new > "$tmp/replacement/v1/new.mp3"; echo '{}' > "$tmp/dist/payload.json"
expect 0 "stage replaces corpus and dist, real verify-kit" stage --corpus "$tmp/replacement" --dist "$tmp/dist"
has "restaged kit" 'files present, 0 bad' "$tmp/o"
has "restaged corpus" '  corpus/v1/new.mp3' "$F/guest-kit/SHA256SUMS"
hasnt "restaged corpus" '  corpus/v1/old.mp3' "$F/guest-kit/SHA256SUMS"
hasnt "restaged dist" '  dist/old.json' "$F/guest-kit/SHA256SUMS"
rm -rf "$F/guest-kit"
touch "$r/tests/rekordbox/mac/kit/rbkit.swift"; sleep 1; touch "$r/tests/rekordbox/mac/kit/rbkit.swift"
reset; expect 0 "stage after an rbkit change" stage; has "rbkit rebuilt" "bin/rbkit" "$F/built"; hasnt "rbkit rebuilt" "rbkit-screen" "$F/built"
reset; mkdir "$tmp/corpus/v1/.hidden"; expect 2 "stage refuses a dot path" stage --corpus "$tmp/corpus"; [ ! -e "$F/tar" ] && ok || no "stage sent a dot path"; rmdir "$tmp/corpus/v1/.hidden"
reset; ln -s 01-x.mp3 "$tmp/corpus/v1/l.mp3"; expect 2 "stage refuses a symlink" stage --corpus "$tmp/corpus"; rm "$tmp/corpus/v1/l.mp3"
reset; ln -s corpus "$tmp/linked"; expect 2 "stage refuses a linked corpus with a trailing slash" stage --corpus "$tmp/linked/"; rm "$tmp/linked"
reset; echo 1 > "$F/verify-rc"; expect 1 "stage, the kit doesn't verify" stage; hasnt "stage unverified" "midi-auto.sh ensure" "$F/calls"
reset; echo 1 > "$F/ensure-rc"; expect 1 "stage, ensure fails" stage
reset; FAKE_RC=255 expect 1 "stage, the VM unreachable" stage

# shot and collect
reset; expect 0 "shot" shot --out "$tmp/sh"; [ -f "$tmp/sh/20261119-100000-1.png" ] && ok || no "shot: not copied"
reset; echo 1 > "$F/shot-rc"; printf '10:00:00  shot: FAIL no permission\n' > "$F/shot-out"; expect 1 "shot, none taken" shot --out "$tmp/sh"
reset; expect 0 "collect" collect --out "$tmp/col"
[ -f "$tmp/col/midi-auto.log" ] && ok || no "collect: the bundle not unzipped"
has "collect" "admin@127.0.0.1:/Users/Shared/rbst/collect-20261119-100000.zip" "$F/scp"; has "collect" "midi-auto.sh bundled --keys collect-20261119-100000.zip" "$F/calls"
reset; expect 2 "collect --out in the checkout" collect --out "$r/tests/out"
reset; RBKIT_OUT="$r/res" expect 2 "RBKIT_OUT in the checkout" preflight
printf 'ign/\n' > "$r/.gitignore"; reset; RBKIT_OUT="$r/ign" expect 0 "RBKIT_OUT in the checkout, git-ignored" preflight

# cleanup: rekordbox quit, the MIDI device removed, the VM stopped (vm.sh down)
reset; expect 0 "cleanup" cleanup; has "cleanup" "midi-auto.sh quit" "$F/calls"; has "cleanup" "midi_down" "$F/calls"; has "cleanup" down "$F/vmsh.log"
has "cleanup" "NOTE  rekordbox's onnxruntime: pioneer" "$tmp/o"
reset; expect 0 "cleanup --keep-running" cleanup --keep-running; [ ! -e "$F/vmsh.log" ] && ok || no "cleanup --keep-running stopped the VM"
reset; FAKE_DOWN_STAYS_RUNNING=1 expect 1 "cleanup, down returned 0 while clone still running" cleanup
has "still running" 'BAD   the VM rbkit-test is still running' "$tmp/o"
notified_once "still running" 'cleanup: 1 step(s) not OK'
reset; FAKE_LIST_RC=1 expect 1 "cleanup, tart list failed with stopped stdout" cleanup
has "list failed" 'BAD   tart list failed after down' "$tmp/o"
notified_once "list failed" 'cleanup: 1 step(s) not OK'
mkdir -p "$tmp/other-state"; cp -R "$tmp/state/." "$tmp/other-state/"
reset; RBKIT_VM_STATE="$tmp/other-state" VM_STATE="$tmp/state" FAKE_EXPECT_VM_STATE="$tmp/other-state" \
  expect 0 "cleanup, RBKIT_VM_STATE overrides VM_STATE" cleanup
has "state override" "$tmp/other-state down" "$F/vmsh.log"
reset; RBKIT_VM_STATE= VM_STATE="$tmp/other-state" FAKE_EXPECT_VM_STATE="$tmp/other-state" \
  expect 0 "cleanup, falls back to VM_STATE" cleanup
has "state fallback" "$tmp/other-state down" "$F/vmsh.log"
reset; echo 1 > "$F/down-rc"; : > "$F/down-out"
expect 1 "cleanup, midi_down failed" cleanup --keep-running
has "midi_down failed" 'BAD   the MIDI daemon (exit 1)' "$tmp/o"
notified_once "midi_down failed" 'cleanup: 1 step(s) not OK'
reset; printf '10:00:00  quit: STOP: new window(s): rekordbox | Sign in | 40; shot x\n' > "$F/quit-out"; echo 1 > "$F/quit-rc"
expect 1 "cleanup, the quit STOPs" cleanup; [ ! -e "$F/vmsh.log" ] && ok || no "cleanup stopped the VM after a STOP"; hasnt "cleanup STOP" midi_down "$F/calls"
has "cleanup STOP" "cleanup: new window(s)" "$F/notified"

echo "vm-session-test: $pass passed, $fail failed"
[ "$fail" = 0 ]
