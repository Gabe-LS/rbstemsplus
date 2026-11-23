#!/bin/bash
# The G3 tart run's steps inside the VM (tests/mac/g3.sh runs each over SSH, as root; release.md
# §6.1). The new app (a TEST build: it has the probe) is driven through the harness's verbs
# (tests/harness/lib.sh); the old apps (the tag TEST builds, no probe) through Accessibility (the G3
# Driver, tests/mac/g3ax.swift), or by the owner where that isn't allowed (g3.sh asks). Each step
# appends "ok …" or "FAIL …" lines to RUN/result, one RUN folder per scenario, in the shared folder.
#
#   g3-guest.sh STEP RUN [ARGS…]
#   facts                        the guest: macOS, SIP, the driver's Accessibility
#   prepare                      run.sh setup's pristine copies; the probe's folders; rekordbox's
#                                tree before (RUN/rekordbox-before.txt)
#   ax-grant                     Accessibility for the G3 Driver (a row in the system's TCC.db,
#                                as root): "granted" or "refused" (then the owner clicks)
#   bootstrap URL V              the build served at URL installed with its own bootstrap as DJ
#                                (welcome already seen); the app is V, opens, no quarantine. URL
#                                "real": the real one-liner, no override (after publishing)
#   emulate                      App Management allowed for the app now in /Applications (DJ's
#                                TCC row) and sudo without a password: the clicks and the password
#                                a user gives, emulated (recorded as such)
#   ax VERB ARGS…                the driver as DJ (g3ax.swift's verbs), its answer printed; its
#                                exit status (3: no Accessibility)
#   cond NAME ARGS…              exit 0 when NAME holds: cache-on, notice V (the old app logged
#                                the update notice), updated V (and a new app's probe started:
#                                more traces than RUN/traces-before.txt), version V, quit
#   before-update                the old app's watcher registration and the log's length recorded
#   switch                       the old app quit, the log's length recorded, its saved payload.json
#                                removed, the probe on for the new app, the old app opened again;
#                                an old app with the probe (1.4.0 on): its trace awaited, the count
#                                kept (RUN/traces-before.txt)
#   payload SHA                  the app saved the served payload.json (its SHA-256 is SHA), within
#                                120 s: proof that it read the new payload
#   after-update V               the new app (its probe): V, updated, no quarantine, verifies, the
#                                watcher registered again (1.0's --watch LaunchAgent replaced),
#                                run by a WatchPaths trigger
#   offer CHANGED                the Stems Cache offer only when CHANGED=1 (then Update, and Stems
#                                Cache on); App Management allowed for the new code first
#   manage cache|plus            Reinstall Stems Cache; Install (or Reinstall) Demucs v4
#   uninstall                    Uninstall RB Stems Plus…, then the trace audit (RELEASING.md step 5)
#   faillogs                     a failed scenario's logs, background items, LaunchAgent, processes
#                                into RUN/fail-logs (g3.sh keeps them before the clone goes)
#   new-start                    the app now in /Applications opened, its first window
#   carried RELV APPV CHANGED    the carried app (the previous release's, or 1.0.0's after its
#                                update; it has the probe) is APPV; relaunched (a new log
#                                segment): "started: RB Stems Plus RELV (Mac app APPV", no update
#                                notice, the Stems Cache offer only when CHANGED=1 (Not Now), About
#                                "Version RELV" (the driver, or the probe's trace; else FAIL), the
#                                report's summary "RB Stems Plus: RELV (Mac app APPV" (Create
#                                Report through the probe)
#   lights                       the switch-over's report lights (the core's, REPORT_FACTS): the
#                                window's (the trace's c.plusIcon and c.cacheIcon accessibility
#                                texts: on green, "installed, not working right now" yellow, off
#                                red, anything else a FAIL) green and green after both installs;
#                                Create Report through the probe; the summary's "lights:" the same
#   kept 1|2                     the prepared playlists' check, now the core's watcher flow: one kept
#                                set that isn't ready (its one file gone), one rekordbox session per
#                                step (ended as a user quits rekordbox, rekordboxAgent gone with
#                                it), the watcher idle before and after each (at most 120 s). 1:
#                                its first watcher.log line for the set a check (never "couldn't
#                                be checked", "being prepared", "not checked"), kept.noticed
#                                written naming the file, one notification; 2: checked again,
#                                kept.noticed unchanged, no notification. The counts from a copy of
#                                usernoted's database, compared only when DJ's ncprefs authorize the
#                                watcher at the session's end (read again then, so an Allow click on
#                                kept 1's prompt counts); else, or when Allow came during the session
#                                and the count didn't move, "not proven", never a FAIL
#   queued                       a queued action (the busy marker of an install that didn't finish)
#                                with rekordbox open: "Action interrupted" (Continue), then
#                                "rekordbox open" (Try Again), no Prepare started at launch;
#                                rekordbox quit, Try Again: the install runs (installed rewritten,
#                                both choices logged), the marker gone, Stems Cache in place
#   quit                         the app quit
#   dialog                       g3.sh smoke's press: a dialog of macOS's own (display dialog, in an
#                                applet osacompile builds here, given a bundle ID for the driver),
#                                opened as DJ; the G3 Driver presses its OK; the applet gone and
#                                its answer "button returned:OK"
# Exit 0 when the step's checks passed; 1 when one failed; 2 when the step couldn't run; 3 (ax):
# no Accessibility; 4 (kept, queued: rekordbox open): rekordbox shows a licence, sign-in or
# activation text (rb_guard: a STOP line, nothing pressed; g3.sh keeps the clone running).
set -u
H=/Users/Shared/rbsp-harness
step="${1:-}"; RUN="${2:-}"
[ -n "$step" ] && [ -n "$RUN" ] || { sed -n '2,78p' "$0"; exit 2; }
mkdir -p "$RUN"; touch "$RUN/result"
# shellcheck source=../harness/lib.sh
. "$H/lib.sh"
DRIVER="$H/G3 Driver.app"
DRIVER_ID=io.github.rbstemsplus.g3driver
AGENT="gui/$UID_/io.github.rbstemsplus.watcher"
note() { echo "note $*" >> "$RUN/result"; }
ok() { echo "ok $*" >> "$RUN/result"; }
# the app's version, as its Info.plist says
appv() { defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null; }
loglines() { if [ -f "$LOGS/app.log" ]; then wc -l < "$LOGS/app.log" | tr -d ' '; else echo 0; fi; }
since() { tail -n +"$(( $(cat "$RUN/applog-lines-before.txt" 2>/dev/null || echo 0) + 1 ))" "$LOGS/app.log" 2>/dev/null; }
rb_tree() {
  (cd "/Applications/rekordbox 7" && find . \( -type f -exec shasum -a 256 {} + \) -o \( -type l -exec sh -c 'for l; do echo "link $l -> $(readlink "$l")"; done' sh {} + \) -o \( -type d -exec stat -f 'dir %N %Su %Sp' {} + \) | LC_ALL=C sort)
  shasum -a 256 "$MODELDIR/hdemucs.onnx"
}
probe_on() {
  rm -rf "$OUT"; mkdir -p "$OUT" "$CONF_DIR"; own "$OUT"
  printf 'mode=trace\nout=%s\n' "$OUT" > "$CONF_DIR/probe.conf"; own "$CONF_DIR"
}
open_app() { as_user open "$APP"; }   # as a user opens it
# version_in FILE V: FILE shows "Version V", not followed by more of a version (1.4.1 isn't 1.4.10)
version_in() { grep -qE "Version ${2//./\\.}([^0-9.]|\.[^0-9]|\.?\$)" "$1"; }
# kept_verdict DELTA AUTH0 AUTH1 N0 N1: a kept session's notification count, N0 -> N1 (usernoted's,
# "?" unread), the watcher's grant at its start and end (yes/no): "no" (not authorized at the end),
# "unread", "late" (Allow came during the session, the count unchanged: not proven), "ok" (N1 is
# N0 + DELTA), else "fail"
kept_verdict() {
  if [ "$3" != yes ]; then echo no
  elif [ "$4" = "?" ] || [ "$5" = "?" ]; then echo unread
  elif [ "$2" != yes ] && [ "$5" = "$4" ]; then echo late
  elif [ "$5" = $(($4 + $1)) ]; then echo ok
  else echo fail; fi
}
# rekordbox's own process (rbpid), and any process of its bundle (rbany: rekordboxAgent and its
# helpers too)
RBID=com.pioneerdj.rekordboxdj
rbpid() { pgrep -f 'rekordbox\.app/Contents/MacOS/rekordbox( |$)' | head -1; }
rbany() { pgrep -f 'rekordbox\.app/Contents/' | head -1; }
# drive VERB ARGS…: the driver as DJ (as the ax step does), quietly; 0 when it answered ok; its
# answer in DRIVE_OUT
drive() {
  DRIVE_OUT="no driver"; [ -d "$DRIVER" ] || return 1
  local o; mkdir -p "$H/ax"; chmod 777 "$H/ax"; o="$H/ax/q-$$-$RANDOM"
  as_user open -W -n -g "$DRIVER" --args "$o" "$@" >/dev/null 2>&1
  DRIVE_OUT="$(cat "$o" 2>/dev/null)"; rm -f "$o"; [ "$(head -1 <<< "$DRIVE_OUT" | cut -d' ' -f1)" = ok ]
}
# rb_guard: rekordbox's windows read; never a press into a licence, sign-in or activation dialog,
# not even an informational OK. Any such text (RB_LICENCE) stops G3 at once: "STOP" into
# RUN/result, the texts into RUN/rekordbox-licence.txt, exit 4 (g3.sh then keeps the clone
# running and tells the owner). The automatic activation's notice ("This computer was activated
# automatically") means the clone counted as a new computer: its MAC address (vm.sh pins the
# template's) didn't hold, and a seat was used. 0: rekordbox's texts in RB_TEXTS (none: not running)
RB_LICENCE='activat|sign[ -]?in|log[ -]?in|licen[cs]e|subscri|authori[sz]'
rb_guard() {
  RB_TEXTS=""
  drive texts "$RBID" || return 0
  RB_TEXTS="$(tail -n +2 <<< "$DRIVE_OUT")"
  grep -Eqi "$RB_LICENCE" <<< "$RB_TEXTS" || return 0
  printf '%s\n' "$RB_TEXTS" > "$RUN/rekordbox-licence.txt"
  if grep -Eqi 'activated automatically|computers (have been|can be) activated' <<< "$RB_TEXTS"; then
    echo "STOP rekordbox's automatic activation notice (a seat used: the clone's MAC address didn't hold): nothing pressed, the clone kept running (rekordbox-licence.txt)" >> "$RUN/result"
  else
    echo "STOP rekordbox shows a licence, sign-in or activation text: nothing pressed, the clone kept running (rekordbox-licence.txt: $(grep -Eim1 "$RB_LICENCE" <<< "$RB_TEXTS"))" >> "$RUN/result"
  fi
  exit 4
}
# rb_notices: rekordbox's startup dialogs that aren't about its licence, as a user closes them, and
# only after rb_guard found no licence text: the firmware tool's offer (Later), a "What's new" or
# tips window (Close). rekordbox's INFORMATION window (its news, shown at each start: seen
# 2026-11-17 in a clone) stays open: it never refuses the Quit. Nothing else is pressed
rb_notices() {
  rb_guard
  if grep -q "FirmwareUpdateManager" <<< "$RB_TEXTS"; then drive press "$RBID" 3000 Later; fi
  if grep -Eqi "what.s new|tips" <<< "$RB_TEXTS"; then drive press "$RBID" 3000 Close; fi
  true
}
# tcc_prompts: macOS's privacy prompts (TCC's, shown by UserNotificationCenter), read through the
# driver; each prompt's line ("“X” would like to …", "… wants to …"): rekordbox's (or its agent's)
# known ones (TCC_DENY: microphone, camera, Bluetooth, the local network, contacts, calendars,
# reminders, photos, files and volumes, location) answered Don't Allow, never Allow; the watcher's
# notification prompt (it names RB Stems Plus: the owner's Allow) left alone, and while it is
# there nothing is pressed (a press takes the first such button of any prompt); any other prompt
# or alert: a FAIL that quotes it, nothing pressed (return 1)
UNC=com.apple.UserNotificationCenter
TCC_DENY='microphone|camera|bluetooth|local network|contacts|calendar|reminders|photo|files|volume|location'
tcc_prompts() {
  drive texts "$UNC" || return 0
  local tx line known=0 watcher=0 unknown="" kl=""
  tx="$(tail -n +2 <<< "$DRIVE_OUT" | grep -v '^[[:space:]]*$')"
  [ -n "$tx" ] || return 0
  while IFS= read -r line; do
    grep -Eqi 'would like to|wants to' <<< "$line" || continue
    if grep -qi "RB Stems Plus" <<< "$line" && grep -qi notif <<< "$line"; then watcher=1
    elif grep -qi rekordbox <<< "$line" && grep -Eqi "$TCC_DENY" <<< "$line"; then known=$((known + 1)); kl+="${kl:+; }$line"
    else unknown+="${unknown:+; }$line"; fi
  done <<< "$tx"
  [ $known = 0 ] && [ $watcher = 0 ] && [ -z "$unknown" ] && unknown="$(head -1 <<< "$tx")"
  if [ -n "$unknown" ]; then fail "an unknown macOS prompt (nothing pressed): $unknown"; return 1; fi
  [ $known -gt 0 ] || return 0
  if [ $watcher = 1 ]; then note "rekordbox's privacy prompt left open: the watcher's notification prompt is there too"; return 0; fi
  if drive press "$UNC" 3000 "Don’t Allow?" "Don't Allow?" && grep -q "^pressed: Don" <<< "$DRIVE_OUT"; then note "rekordbox's privacy prompt answered Don't Allow ($known: $kl)"
  else note "rekordbox's privacy prompt: Don't Allow couldn't be pressed ($(head -1 <<< "$DRIVE_OUT"))"; fi
  true
}
# rb_started: rekordbox just opened: its first 20 s watched (rb_guard), its startup notices and
# the privacy prompts answered
rb_started() {
  local i
  for i in 1 2 3 4; do rb_guard; sleep 5; done
  rb_notices; tcc_prompts
}
# rb_ask_quit: rekordbox asked to quit as a user does (its Quit, an Apple Event; not a kill: a
# killed rekordbox leaves rekordboxAgent running, a quit one doesn't); 0 when it accepted; prints
# osascript's error
rb_ask_quit() { as_user osascript -e 'with timeout of 20 seconds' -e "tell application id \"$RBID\" to quit" -e 'end timeout' 2>&1 >/dev/null; }
# rb_quit WHO: rekordbox quit as a user quits it (first and after each refusal: rb_guard, the
# privacy prompts; refused: its notices closed, asked again, 4 asks at most), then every process of its bundle gone within 30 s, rekordboxAgent too; else a FAIL
# that says which, and they are killed. Each refusal (osascript's error, rekordbox's window texts)
# into RUN/rb-quit-WHO.txt
rb_quit() {
  local who="$1" i k e asked=no log="$RUN/rb-quit-$1.txt"
  : > "$log"
  rb_guard; tcc_prompts
  for i in 1 2 3 4; do
    if e="$(rb_ask_quit)"; then
      asked=yes
      for k in $(seq 1 30); do [ -z "$(rbany)" ] && return 0; sleep 1; done
      break
    fi
    drive texts "$RBID"; printf '== ask %s refused: %s\n%s\n' "$i" "$e" "$DRIVE_OUT" >> "$log"
    tcc_prompts; rb_notices; sleep 2
  done
  if [ "$asked" = no ]; then fail "$who: rekordbox refused its Quit 4 times (${e:-no error}; its windows' texts: rb-quit-$who.txt)"
  elif [ -n "$(rbpid)" ]; then fail "$who: rekordbox didn't quit within 30 s of its Quit"
  else fail "$who: rekordbox quit, but $(ps -o comm= -p "$(rbany)" 2>/dev/null | sed 's|.*/||') still running 30 s later"; fi
  pkill -9 -f 'rekordbox\.app/Contents/'; return 1
}
bridge_in_rekordbox() { grep -aq "rbstems bridge: " "$RBLIB"; }
# action BUTTON VERB: a Manage button, its confirmation, its end (the spinner on, then off)
action() {
  click "$1" && next_dialog 5000 && answer "$2" || return 1
  wait_for 60000 "c.spinner.anim=1"; wait_for 480000 "c.spinner.anim=0"; settle 1000
}

case "$step" in
facts)
  {
    echo "macos $(sw_vers -productVersion)"
    echo "sip $(csrutil status 2>/dev/null | sed -n 's/.*status: *//p' | tr -d '.')"
    echo "console $(stat -f %Su /dev/console)"
    echo "rekordbox $(defaults read "$RB/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null || echo none)"
    if [ -d "$DRIVER" ]; then
      rm -f "$H/ax/trusted"; mkdir -p "$H/ax"; chmod 777 "$H/ax"
      as_user open -W -n -g "$DRIVER" --args "$H/ax/trusted" trusted >/dev/null 2>&1
      echo "accessibility $(sed -n '1s/^ok //p' "$H/ax/trusted" 2>/dev/null || echo unknown)"
    else
      echo "accessibility no-driver"
    fi
  } > "$RUN/facts.txt"
  cat "$RUN/facts.txt" ;;
prepare)
  bash "$H/run.sh" setup >/dev/null || { echo "FAIL the pristine copies (run.sh setup)" >> "$RUN/result"; exit 2; }
  probe_on
  rb_tree > "$RUN/rekordbox-before.txt" || { echo "FAIL rekordbox's tree couldn't be read" >> "$RUN/result"; exit 2; }
  ok "prepared: rekordbox's tree recorded ($(wc -l < "$RUN/rekordbox-before.txt" | tr -d ' ') lines)" ;;
ax-grant)
  # Accessibility lives in the system's TCC.db (SIP-protected): root over SSH may write it in the
  # template (its sshd has Full Disk Access) or not; refused means the owner clicks instead
  db="/Library/Application Support/com.apple.TCC/TCC.db"
  codesign -d -r- "$DRIVER" 2>&1 | sed -n 's/^\(# \)\{0,1\}designated => //p' | csreq -r- -b "$H/driver.req" 2>/dev/null \
    || { echo "refused (no code requirement for the driver)"; exit 1; }
  req="$(xxd -p "$H/driver.req" | tr -d '\n')"
  if sqlite3 "$db" "insert or replace into access (service, client, client_type, auth_value, auth_reason, auth_version, csreq, flags, indirect_object_identifier) values ('kTCCServiceAccessibility', '$DRIVER_ID', 0, 2, 4, 1, X'$req', 0, 'UNUSED')" 2>"$H/ax-grant.err"; then
    pkill -x tccd 2>/dev/null; sleep 1
    echo granted
  else
    echo "refused ($(head -1 "$H/ax-grant.err"))"; exit 1
  fi ;;
bootstrap)
  url="$3"; v="$4"
  fact_welcomed
  if [ "$url" = real ]; then line="curl -fsSL https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/bootstrap.sh | bash"
  else line="curl -fsSL '${url}bootstrap.sh' | RBSTEMSPLUS_BASE_URL='$url' bash"; fi
  echo "$line" > "$RUN/bootstrap.cmd"
  as_user /bin/bash -c "$line" > "$RUN/bootstrap.log" 2>&1 \
    || { echo "FAIL the bootstrap failed (bootstrap.log)" >> "$RUN/result"; exit 1; }
  check '[ "$(appv)" = "$v" ]' "the bootstrap installed $v"
  check '[ -z "$(xattr -r "$APP" 2>/dev/null | grep com.apple.quarantine)" ]' "no quarantine on any file of the app (after the bootstrap)"
  for _ in $(seq 1 100); do pgrep -x "RB Stems Plus" >/dev/null && break; sleep 0.2; done
  check 'pgrep -x "RB Stems Plus" >/dev/null' "the app opened"
  ! grep -q '^FAIL' "$RUN/result" ;;
emulate)
  fact_sudo_nopasswd
  fact_app_management allow
  note "emulated: App Management allowed for $(appv)'s code (DJ's TCC row) and sudo without a password (no password sheet)" ;;
ax)
  shift 2
  mkdir -p "$H/ax"; chmod 777 "$H/ax"; o="$H/ax/out-$$-$RANDOM"
  as_user open -W -n -g "$DRIVER" --args "$o" "$@" >/dev/null 2>&1
  rc=1
  case "$(head -1 "$o" 2>/dev/null)" in ok*) rc=0 ;; "FAIL no Accessibility permission") rc=3 ;; esac
  cat "$o" 2>/dev/null || echo "FAIL the driver wrote nothing"
  echo "ax $*: $(head -1 "$o" 2>/dev/null)" >> "$RUN/ax.log"
  rm -f "$o"; exit $rc ;;
cond)
  case "$3" in
    cache-on) bridge_in_rekordbox && grep -q "cache=1" "$SUP/installed" 2>/dev/null && [ ! -e "$SUP/busy" ] ;;
    updated) [ "$(appv)" = "$4" ] && [ "$(traces | wc -l | tr -d ' ')" -gt "$(cat "$RUN/traces-before.txt" 2>/dev/null || echo 0)" ] ;;
    notice) since | grep -q "RB Stems Plus $4 is available" ;;
    version) [ "$(appv)" = "$4" ] ;;
    quit) ! pgrep -x "RB Stems Plus" >/dev/null ;;
    *) exit 2 ;;
  esac ;;
before-update)
  check "launchctl print $AGENT >/dev/null 2>&1" "the old app's watcher registered before the update"
  launchctl print "$AGENT" > "$RUN/watcher-before.txt" 2>&1
  cat "$SUP/watcher.registered" > "$RUN/registered-before.txt" 2>/dev/null || echo none > "$RUN/registered-before.txt"
  cp "$UH/Library/LaunchAgents/io.github.rbstemsplus.watcher.plist" "$RUN/agent-before.plist" 2>/dev/null
  loglines > "$RUN/applog-lines-before.txt"
  ok "the app log's length before the update: $(cat "$RUN/applog-lines-before.txt") lines"
  ! grep -q '^FAIL' "$RUN/result" ;;
switch)
  pkill -x "RB Stems Plus"; for _ in $(seq 1 50); do pgrep -x "RB Stems Plus" >/dev/null || break; sleep 0.2; done
  rm -f "$SUP/payload.json" "$SUP/payload.json.sig" "$SUP/payload/payload.json" "$SUP/payload/payload.json.sig"
  loglines > "$RUN/applog-lines-before.txt"
  probe_on
  open_app
  # an old app with the probe (an archived 1.4.0 or later): its own trace first, so the new app's is
  # told apart (cond updated, after-update)
  if grep -aq rbstemsplus-probe "$APP/Contents/MacOS/RB Stems Plus"; then
    wait_new_trace 0 1200 || { fail "switch: the old app's probe didn't start"; exit 1; }
    wait_event 30000 windowDrawn 1
  fi
  traces | wc -l | tr -d ' ' > "$RUN/traces-before.txt"
  ok "switched: the old app opened again with the new build served ($(cat "$RUN/traces-before.txt") traces before)" ;;
payload)
  # the served payload's SHA-256 in a variable: check evals its test inside a function, where $3 isn't this step's
  want_ps="$3"
  for _ in $(seq 1 120); do
    [ -f "$SUP/payload/payload.json" ] && [ "$(shasum -a 256 "$SUP/payload/payload.json" | cut -c1-64)" = "$want_ps" ] && break
    sleep 1
  done
  check '[ -f "$SUP/payload/payload.json" ] && [ "$(shasum -a 256 "$SUP/payload/payload.json" | cut -c1-64)" = "$want_ps" ]' "the app saved the served payload.json (it read the new payload)"
  ! grep -q '^FAIL' "$RUN/result" ;;
after-update)
  v="$3"
  wait_new_trace "$(cat "$RUN/traces-before.txt" 2>/dev/null || echo 0)" 6000 || exit 1
  wait_event 60000 windowDrawn 1; settle 2000
  check '[ "$(appv)" = "$v" ]' "the app is $v"
  check 'grep -aq "TEST BUILD" "$APP/Contents/MacOS/RB Stems Plus"' "it is the TEST build under test"
  check 'since | grep -q "update to $v: updated"' "the update script says updated"
  check '[ -z "$(xattr -r "$APP" 2>/dev/null | grep com.apple.quarantine)" ]' "no quarantine on any file of the new app"
  check 'codesign --verify --deep --strict "$APP" 2>/dev/null' "the new app verifies (ad hoc, signed by the update)"
  cat "$SUP/watcher.registered" > "$RUN/registered-after.txt" 2>/dev/null || echo none > "$RUN/registered-after.txt"
  check '! grep -qx none "$RUN/registered-after.txt" && ! cmp -s "$RUN/registered-before.txt" "$RUN/registered-after.txt"' "the watcher's registration record changed (the new helper's code)"
  check 'since | grep -q "watcher: installed"' "the new app registered the watcher (\"watcher: installed\" after the update)"
  check 'launchctl print "$AGENT" 2>/dev/null | grep -q "Helpers/RB Stems Plus Watcher.app"' "the agent starts the new app's watcher helper"
  # the agent's arguments, exactly: the new app's watcher helper, then --watch (installWatcher writes
  # the same in 1.0.0, 1.3.0 and 1.4.0: app/Sources/Actions.swift), one per line, nothing more
  ap="$UH/Library/LaunchAgents/io.github.rbstemsplus.watcher.plist"; i=0; : > "$RUN/agent-args-after.txt"
  while [ $i -lt 10 ] && a="$(/usr/libexec/PlistBuddy -c "Print :ProgramArguments:$i" "$ap" 2>/dev/null)"; do echo "$a" >> "$RUN/agent-args-after.txt"; i=$((i + 1)); done
  printf '%s\n' "$APP/Contents/Helpers/RB Stems Plus Watcher.app/Contents/MacOS/RB Stems Plus Watcher" --watch > "$RUN/agent-args-want.txt"
  check 'cmp -s "$RUN/agent-args-want.txt" "$RUN/agent-args-after.txt"' "the LaunchAgent runs the new app's watcher helper with --watch, nothing else (agent-args-after.txt)"
  runs() { launchctl print "$AGENT" 2>/dev/null | sed -n 's/^[[:space:]]*runs = //p' | tr -d ' '; }
  r0="$(runs)"; touch "$RB/Contents/Info.plist"; r1=""
  for _ in $(seq 1 30); do sleep 2; r1="$(runs)"; [ -n "$r1" ] && [ "$r1" != "$r0" ] && break; done
  check '[ -n "$r1" ] && [ "$r1" != "$r0" ]' "a WatchPaths trigger ran the new watcher (launchd runs ${r0:-?} -> ${r1:-?})"
  ! grep -q '^FAIL' "$RUN/result" ;;
offer)
  TRACE="$(traces | tail -1)"; LAST_DLG=0
  [ -n "$TRACE" ] && [ -p "$OUT/cmd" ] || { fail "offer: the new app's probe isn't running"; exit 1; }
  if [ "$3" = 1 ]; then
    fact_app_management allow
    note "emulated: App Management allowed for the new app's code before its first root run"
    if next_dialog 30000 && grep -q '"app.modal":"[^"]*Stems Cache' "$TRACE"; then
      ok "the bridge changed: the new app offers the Stems Cache update"
      answer Update
      wait_for 60000 "c.spinner.anim=1"; wait_for 480000 "c.spinner.anim=0"; settle 1000
      check 'grep -q "\"c.cacheIcon.img\":\"on\"" "$TRACE"' "Stems Cache on after its update"
    else
      fail "the bridge changed, but no Stems Cache update offer"
    fi
  else
    settle 3000
    check '[ "$(count_of dialog)" = 0 ]' "the same bridge: no Stems Cache update offer"
    fact_app_management allow
    note "emulated: App Management allowed for the new app's code before its first root run"
  fi
  ! grep -q '^FAIL' "$RUN/result" ;;
manage)
  TRACE="$(traces | tail -1)"
  [ -n "$TRACE" ] && [ -p "$OUT/cmd" ] || { fail "manage: the new app's probe isn't running"; exit 1; }
  LAST_DLG="$(count_of dialog)"
  if [ "$3" = cache ]; then
    action c.reinstallCacheBtn Reinstall
    check 'bridge_in_rekordbox && grep -q "cache=1" "$SUP/installed"' "Reinstall Stems Cache with the new app"
  else
    if grep -q "model=1" "$SUP/installed" 2>/dev/null; then action c.reinstallPlusBtn Reinstall; else action c.installPlusBtn Install; fi
    check 'grep -q "cache=1" "$SUP/installed" && grep -q "model=1" "$SUP/installed"' "Install (or Reinstall) Demucs v4 with the new app: both installed"
  fi
  ! grep -q '^FAIL' "$RUN/result" ;;
uninstall)
  TRACE="$(traces | tail -1)"
  [ -n "$TRACE" ] && [ -p "$OUT/cmd" ] || { fail "uninstall: the new app's probe isn't running"; exit 1; }
  LAST_DLG="$(count_of dialog)"
  menu "menu.App.Uninstall RB Stems Plus…"
  next_dialog 10000; answer Uninstall
  next_dialog 540000; settle; answer Done
  for _ in $(seq 1 300); do [ -e "$APP" ] || break; sleep 0.1; done
  # the trace audit (docs/RELEASING.md step 5), as root
  rb_tree > "$RUN/rekordbox-after.txt"
  check 'cmp -s "$RUN/rekordbox-before.txt" "$RUN/rekordbox-after.txt"' "rekordbox byte for byte as before (its tree, and Pioneer's model back)"
  check 'codesign --verify --deep --strict "$RB" 2>/dev/null' "rekordbox verifies (AlphaTheta's signature)"
  for _ in $(seq 1 20); do [ -e "$UH/Library/Preferences/io.github.rbstemsplus.app.plist" ] || break; sleep 1; done
  check '[ ! -e "$APP" ] && [ ! -e "$SUP" ] && [ ! -e "$LOGS" ] && [ ! -e "$UH/Library/Caches/rbstemsplus" ] && [ ! -e "$ROOTDIR" ]' "nothing of RB Stems Plus left: the app, its folders, the caches, the root folder"
  check '[ ! -e "$UH/Library/Preferences/io.github.rbstemsplus.app.plist" ] && [ ! -e "$UH/Library/LaunchAgents/io.github.rbstemsplus.watcher.plist" ]' "no preferences file, no LaunchAgent plist"
  check '! launchctl print "$AGENT" >/dev/null 2>&1' "no watcher registered"
  if sfltool dumpbtm >/dev/null 2>&1; then
    # macOS's background-task daemon drops the agent's record once it is booted out and its plist
    # deleted (as the uninstall rounds found), on its own time: polled for up to 60 s
    for _ in $(seq 1 30); do sfltool dumpbtm 2>/dev/null | grep -qi rbstemsplus || break; sleep 2; done
    sfltool dumpbtm > "$RUN/dumpbtm-after.txt" 2>&1; chmod a+r "$RUN/dumpbtm-after.txt"
    check '! grep -qi rbstemsplus "$RUN/dumpbtm-after.txt"' "no background-item record (sfltool dumpbtm, within 60 s: dumpbtm-after.txt)"
  else
    note "no sfltool dumpbtm on macOS $(sw_vers -productVersion): background items not listed"
  fi
  check '[ "$(tcc_rows)" = 0 ]' "no privacy permission of the app left (TCC rows)"
  ! grep -q '^FAIL' "$RUN/result" ;;
faillogs)
  # a failed scenario's evidence, before its clone is deleted: the app's and the watcher's logs,
  # the background-item records, the LaunchAgent (its plist and launchd's view), the processes
  d="$RUN/fail-logs"; rm -rf "$d"; mkdir -p "$d"
  [ -d "$LOGS" ] && cp -R "$LOGS" "$d/logs"
  sfltool dumpbtm > "$d/dumpbtm.txt" 2>&1
  cp "$UH/Library/LaunchAgents/io.github.rbstemsplus.watcher.plist" "$d/" 2>/dev/null
  launchctl print "$AGENT" > "$d/launchctl-print.txt" 2>&1
  ls -la "$SUP" "$SUP/kept" > "$d/support-ls.txt" 2>&1
  ps axo pid,ppid,etime,command | grep -i "rb stems\|rekordbox\|usernoted" | grep -v grep > "$d/ps.txt"
  chmod -R a+rX "$d"
  echo "kept: $(ls "$d" | tr '\n' ' ')" ;;
new-start)
  probe_on
  open_app
  wait_new_trace 0 1200 || exit 1
  wait_event 30000 windowDrawn 1; settle 1000
  ok "the new app started: its first window"
  ! grep -q '^FAIL' "$RUN/result" ;;
carried)
  relv="$3"; appv_="$4"; changed="$5"
  TRACE="$(traces | tail -1)"
  [ -n "$TRACE" ] && [ -p "$OUT/cmd" ] || { fail "carried: the carried app's probe isn't running"; exit 1; }
  check '[ "$(appv)" = "$appv_" ]' "the app is still $appv_ (carried)"
  # a new log segment, the new payload already read (the host's payload step): quit and relaunch
  loglines > "$RUN/applog-lines-before.txt"
  quit_app
  launch || exit 1
  for _ in $(seq 1 30); do since | grep -q "started: RB Stems Plus" && break; sleep 1; done
  check 'since | grep -q "started: RB Stems Plus $relv (Mac app $appv_"' "the log: started: RB Stems Plus $relv (Mac app $appv_"
  if [ "$changed" = 1 ]; then
    if next_dialog 30000 && grep -q '"app.modal":"[^"]*Stems Cache' "$TRACE"; then ok "the bridge changed: the Stems Cache update offer"; answer "Not Now"; settle
    else fail "the bridge changed, but no Stems Cache update offer"; fi
  else
    settle 2000
  fi
  sleep 15                                     # the payload checked at launch, its lines logged
  check '! since | grep -q "RB Stems Plus [0-9.]* is available"' "no update notice (the log, after the relaunch)"
  if [ "$changed" = 1 ]; then check 'since | grep -q "Stems Cache: an update is available"' "the offer in the log"
  else check '! since | grep -q "Stems Cache: an update is available"' "the same bridge: no Stems Cache update offer (the log)"; fi
  # About: opened through the probe; read by the driver, or from the probe's trace; else FAIL
  menu "menu.App.About RB Stems Plus"; sleep 1
  "$0" ax "$RUN" text "$BID" 10000 "Version $relv" > "$RUN/ax-about.txt" 2>&1; r=$?
  if [ "$r" = 0 ]; then ok "About says Version $relv (Accessibility)"
  elif version_in "$TRACE" "$relv"; then ok "About says Version $relv (the probe's trace)"
  elif [ "$r" = 3 ]; then fail "About not read (no Accessibility)"
  else fail "About doesn't say Version $relv (ax-about.txt)"; fi
  # the report: Create Report (Help) through the probe; its summary line
  rm -f "$UH/Desktop/RB Stems Plus report "*.zip
  menu "menu.Help.Create Report"
  z=""; for _ in $(seq 1 120); do z="$(ls "$UH/Desktop/RB Stems Plus report "*.zip 2>/dev/null | head -1)"; [ -n "$z" ] && break; sleep 1; done
  check '[ -n "$z" ] && unzip -p "$z" "*summary.txt" 2>/dev/null | grep -q "RB Stems Plus: $relv (Mac app $appv_"' "the report's summary: RB Stems Plus: $relv (Mac app $appv_"
  ! grep -q '^FAIL' "$RUN/result" ;;
lights)
  TRACE="$(traces | tail -1)"
  [ -n "$TRACE" ] && [ -p "$OUT/cmd" ] || { fail "lights: the new app's probe isn't running"; exit 1; }
  # the light's accessibility text (texts.def T_LIGHT_AX_*) to its colour; anything else, empty too, is "?"
  colour() { case "$1" in on) echo green ;; "installed, not working right now") echo yellow ;; off) echo red ;; *) echo "?" ;; esac; }
  last_img() { grep -o "\"c.$1.img\":\"[^\"]*\"" "$TRACE" | tail -1 | sed 's/^"c\.[A-Za-z]*\.img":"//; s/"$//'; }
  pi="$(last_img plusIcon)"; ci="$(last_img cacheIcon)"
  pc="$(colour "$pi")"; cc="$(colour "$ci")"
  check '[ "$pc" != "?" ] && [ "$cc" != "?" ]' "lights: the window's lights read (Demucs v4 \"$pi\", Stems Cache \"$ci\")"
  check '[ "$pc" = green ] && [ "$cc" = green ]' "lights: green and green after both installs (Demucs v4 $pc, Stems Cache $cc)"
  rm -f "$UH/Desktop/RB Stems Plus report "*.zip
  menu "menu.Help.Create Report"
  z=""; for _ in $(seq 1 120); do z="$(ls "$UH/Desktop/RB Stems Plus report "*.zip 2>/dev/null | head -1)"; [ -n "$z" ] && break; sleep 1; done
  line="$( [ -n "$z" ] && unzip -p "$z" "*summary.txt" 2>/dev/null | grep -m1 "^RB Stems Plus: .*; lights: ")"
  echo "$line" > "$RUN/report-lights.txt"
  check '[ -n "$line" ]' "lights: the report's summary has its lights line"
  check '[ "$pc" != "?" ] && [ "$cc" != "?" ] && grep -q "lights: Demucs v4 $pc (" "$RUN/report-lights.txt" && grep -q ", Stems Cache $cc (" "$RUN/report-lights.txt"' "lights: the report's lights are the window's (Demucs v4 $pc, Stems Cache $cc)"
  next_dialog 30000 && answer Done
  ! grep -q '^FAIL' "$RUN/result" ;;
kept)
  # one kept set (the helper's format, helper/kept.h), its one file not there: not ready. Two
  # steps, one rekordbox session each (kept 1, kept 2), each under g3.sh's limit; what session 2
  # compares with is in RUN (kept-n1, kept.noticed-1). The watcher's grant is read at each
  # session's start and again at its end: the prompt comes with kept 1's notification, so an Allow
  # click during the run is seen
  n="${3:-}"; case "$n" in 1|2) ;; *) fail "kept: the session is 1 or 2"; exit 2 ;; esac
  kstart=$SECONDS
  kd="$SUP/kept"; id=g3kept; gone="$UH/Music/rbsp-g3-gone.flac"
  WID=io.github.rbstemsplus.watcher
  # authorized: DJ's com.apple.ncprefs has the watcher's entry (its exact bundle id) with auth 7,
  # the value of an app allowed to notify; anything else: the notification can't be proven
  authorized() {
    local f="$UH/Library/Preferences/com.apple.ncprefs.plist" i=0 b
    while [ $i -lt 1000 ] && b="$(/usr/libexec/PlistBuddy -c "Print :apps:$i:bundle-id" "$f" 2>/dev/null)"; do
      [ "$b" = "$WID" ] && { [ "$(/usr/libexec/PlistBuddy -c "Print :apps:$i:auth" "$f" 2>/dev/null)" = 7 ]; return; }
      i=$((i + 1))
    done
    return 1
  }
  # notes: the watcher's notifications usernoted holds, read from a copy of its database ("?":
  # the copy or the query failed); the live database is never opened
  notes() {
    local src="$UH/Library/Group Containers/group.com.apple.usernoted/db2" x
    rm -rf "$RUN/nc"; mkdir -p "$RUN/nc"
    cp "$src/db" "$RUN/nc/db" 2>/dev/null || { echo "?"; return; }
    for x in db-wal db-shm; do [ ! -e "$src/$x" ] || cp "$src/$x" "$RUN/nc/$x" 2>/dev/null || { echo "?"; return; }; done
    sqlite3 -readonly "$RUN/nc/db" "select count(*) from record r join app a on r.app_id = a.app_id where a.identifier = '$WID'" 2>/dev/null || echo "?"
  }
  # watcher_idle WHEN: at most 120 s for the watcher to let go (no watcher.lock, its job not running)
  watcher_idle() {
    for _ in $(seq 1 60); do
      [ ! -d "$SUP/watcher.lock" ] && ! launchctl print "$AGENT" 2>/dev/null | grep -q 'state = running' && return 0
      sleep 2
    done
    fail "kept: the watcher still running $1 (watcher.lock or its job, after 120 s)"; return 1
  }
  if authorized; then auth0=yes; else auth0=no; fi
  if [ "$n" = 1 ]; then
    as_user mkdir -p "$kd"
    model="$(shasum -a 256 "$MODELDIR/hdemucs.onnx" | cut -c1-64)"
    as_user /bin/sh -c "printf 'rbstemsplus-kept 1\nuntil %s\n%s %s\n' \"\$(( \$(date +%s) + 86400 ))\" '$model' '$(printf a%.0s $(seq 1 64))' > '$kd/$id.kept'"
    as_user /bin/sh -c "printf 'rbstemsplus-manifest 1\nhelper 1\nmodel %s\nplaylist %s\nrate 0 0 4\nfile 1 2 1 0 %s\n' '$model' '$id' '$gone' > '$kd/$id.manifest'"
    rm -f "$SUP/kept.checked" "$SUP/kept.noticed" "$gone" "$RUN/kept-n1" "$RUN/kept.noticed-1"
    notes > "$RUN/kept-n0"
  else
    [ -s "$RUN/kept-n1" ] || { fail "kept 2: session 1's records aren't there"; exit 1; }
  fi
  # the session: the watcher idle, rekordbox opened, the watcher run, its first line of this
  # session for the set (or the whole check skipped) awaited, rekordbox quit, the watcher idle again
  watcher_idle "before session $n" || exit 1
  wl0="$(wc -l < "$LOGS/watcher.log" 2>/dev/null | tr -d ' ')"; [ -n "$wl0" ] || wl0=0
  as_user open -g "$RB"
  pid=""; for _ in $(seq 1 60); do pid="$(rbpid)"; [ -n "$pid" ] && break; sleep 1; done
  [ -n "$pid" ] || { fail "kept: rekordbox didn't start (session $n)"; exit 1; }
  rb_started || exit 1
  launchctl kickstart "$AGENT" >/dev/null 2>&1
  line=""
  for _ in $(seq 1 90); do
    line="$(tail -n +"$((wl0 + 1))" "$LOGS/watcher.log" 2>/dev/null | grep -m1 -e "prepared playlist $id: " -e "prepared playlists not checked")"
    [ -n "$line" ] && break; sleep 2
  done
  echo "$line" > "$RUN/kept-$n.txt"
  rb_guard; tcc_prompts || exit 1
  # session 1, the watcher not yet allowed to notify: its notification comes with macOS's prompt,
  # which only the owner can answer (the TCC row can't be scripted): the session stays open until
  # Allow, at most 300 s (and the step within g3.sh's 600 s); no Allow by then: as before (a note)
  if [ "$n" = 1 ] && [ "$auth0" = no ] && [ -n "$line" ]; then
    for k in $(seq 1 150); do authorized && break; [ $((SECONDS - kstart)) -lt 380 ] || break; [ $((k % 5)) = 0 ] && rb_guard; sleep 2; done
    if authorized; then note "kept: Allow clicked during session 1 (after $((SECONDS - kstart)) s)"; else note "kept: no Allow within session 1's wait ($((SECONDS - kstart)) s)"; fi
  fi
  rb_quit kept || exit 1
  watcher_idle "after session $n" || exit 1
  if authorized; then auth1=yes; else auth1=no; fi
  check '[ "$(cat "$SUP/kept.checked" 2>/dev/null)" = "$pid" ]' "kept: session $n seen (kept.checked is rekordbox's pid $pid)"
  check '[ -n "$line" ] && ! grep -q "couldn.t be checked\|being prepared\|not checked" "$RUN/kept-$n.txt"' "kept: session $n checked the set (${line:-no line for it in watcher.log})"
  if [ "$n" = 1 ]; then
    check '[ -s "$SUP/kept.noticed" ] && grep -q "rbsp-g3-gone.flac" "$SUP/kept.noticed"' "kept: kept.noticed written, not empty, naming the file not ready"
    cp "$SUP/kept.noticed" "$RUN/kept.noticed-1" 2>/dev/null
    notes > "$RUN/kept-n1"
    n0="$(cat "$RUN/kept-n0")"; n1="$(cat "$RUN/kept-n1")"
    case "$(kept_verdict 1 "$auth0" "$auth1" "$n0" "$n1")" in
      no) note "kept: notification not proven: the watcher isn't authorized to notify (session 1, no Allow by its end)" ;;
      unread) note "kept: notification not proven: usernoted's database couldn't be copied or read (session 1)" ;;
      late) note "kept: notification not proven: Allow came during session 1 and usernoted holds none ($n0 -> $n1; the watcher may have ended before the click)" ;;
      *) check '[ "$n1" = $((n0 + 1)) ]' "kept: one notification in session 1 ($n0 -> $n1; Allow $([ "$auth0" = yes ] && echo before it || echo during it))" ;;
    esac
  else
    check 'cmp -s "$SUP/kept.noticed" "$RUN/kept.noticed-1"' "kept: kept.noticed unchanged (the same file not ready)"
    n1="$(cat "$RUN/kept-n1")"; n2="$(notes)"
    case "$(kept_verdict 0 "$auth0" "$auth1" "$n1" "$n2")" in
      no) note "kept: silence not proven: the watcher isn't authorized to notify (session 2)" ;;
      unread) note "kept: silence not proven: usernoted's database couldn't be copied or read (session 2)" ;;
      late) note "kept: silence not proven: Allow came during session 2 ($n1 -> $n2)" ;;
      *) check '[ "$n2" = "$n1" ]' "kept: session 2 silent, no notification ($n1 -> $n2)" ;;
    esac
    rm -f "$kd/$id.kept" "$kd/$id.manifest"
  fi
  ! grep -q '^FAIL' "$RUN/result" ;;
queued)
  # an install that didn't finish (the busy marker of a process that is gone): the next launch asks
  # "Action interrupted" (Continue); with rekordbox open the resumed install asks "rekordbox open"
  # (Try Again), then runs once rekordbox quit
  modal() { grep -o '"app.modal":"[^"]*"' "$TRACE" | tail -1; }
  quit_app
  m0="$(stat -f %m "$SUP/installed" 2>/dev/null || echo none)"
  as_user /bin/sh -c "printf '99999\n1\ninstall-cache\nInstall Stems Cache\n' > '$SUP/busy'"
  as_user open -g "$RB"
  for _ in $(seq 1 60); do [ -n "$(rbpid)" ] && break; sleep 1; done
  [ -n "$(rbpid)" ] || { fail "queued: rekordbox didn't start"; exit 1; }
  rb_started || exit 1
  loglines > "$RUN/applog-lines-before.txt"
  launch || exit 1
  TRACE="$(traces | tail -1)"; LAST_DLG=0
  if next_dialog 60000 && modal | grep -q "Action interrupted"; then
    ok "queued: the unfinished install offered again (\"Action interrupted\")"
  else
    fail "queued: no \"Action interrupted\" question at launch ($(modal))"; exit 1
  fi
  answer Continue || { fail "queued: Continue couldn't be clicked"; exit 1; }
  if next_dialog 60000 && modal | grep -q "rekordbox open" && modal | grep -q "Try Again"; then
    ok "queued: the resumed install waits for rekordbox (\"rekordbox open\", Try Again)"
  else
    fail "queued: no \"rekordbox open\" question with Try Again after Continue ($(modal))"; exit 1
  fi
  check '! since | grep -q "prepare: "' "queued: no Prepare started at launch"
  rb_quit queued
  answer "Try Again" || { fail "queued: Try Again couldn't be clicked"; exit 1; }
  check 'wait_for 60000 "c.spinner.anim=1"' "queued: the install started after Try Again (the spinner on)"
  check 'wait_for 480000 "c.spinner.anim=0"' "queued: the install ended (the spinner off)"
  settle 1000
  # the "rekordbox open" question's Try Again isn't logged (1.3.0's readyForQueuedAction didn't
  # log it either; core/flows.c q_try_again): it shows as the install that ran after it (above)
  check 'since | grep -q "user chose: Continue"' "queued: the log has the choice (Continue)"
  check '[ "$(stat -f %m "$SUP/installed" 2>/dev/null || echo none)" != "$m0" ]' "queued: the install ran (installed rewritten)"
  check '[ ! -e "$SUP/busy" ]' "queued: the busy marker gone"
  check 'bridge_in_rekordbox && grep -q "cache=1" "$SUP/installed"' "queued: Stems Cache in place after the resumed install"
  ! grep -q '^FAIL' "$RUN/result" ;;
dialog)
  # osascript itself has no bundle ID (the driver finds apps by theirs): the same display dialog
  # runs in an applet, its answer written to a file; opened in front (a background applet's dialog
  # isn't shown)
  dd="$H/dialog"; da="$dd/G3 Dialog.app"; did=io.github.rbstemsplus.g3dialog
  rm -rf "$dd"; mkdir -p "$dd"; chown "$U" "$dd"
  as_user osacompile -o "$da" -e 'set r to display dialog "g3 smoke" buttons {"OK"} default button "OK"' \
    -e "do shell script \"echo button returned:\" & quoted form of (button returned of r) & \" > '$dd/answer'\"" 2>/dev/null \
    && as_user plutil -insert CFBundleIdentifier -string "$did" "$da/Contents/Info.plist" \
    && as_user codesign -f -s - "$da" 2>/dev/null || { fail "dialog: the applet couldn't be built (osacompile, plutil, codesign)"; exit 2; }
  as_user open "$da"
  drive press "$did" 30000 OK || { fail "dialog: the G3 Driver couldn't press OK ($(head -1 <<< "$DRIVE_OUT"))"; pkill -f "$da/"; exit 1; }
  for _ in $(seq 1 50); do pgrep -f "$da/" >/dev/null || break; sleep 0.2; done
  check '! pgrep -f "$da/" >/dev/null' "dialog: gone after the G3 Driver's press"
  check '[ "$(cat "$dd/answer" 2>/dev/null)" = "button returned:OK" ]' "dialog: it answered \"button returned:OK\" (Accessibility press)"
  pkill -f "$da/"
  ! grep -q '^FAIL' "$RUN/result" ;;
quit)
  pkill -x "RB Stems Plus"; for _ in $(seq 1 50); do pgrep -x "RB Stems Plus" >/dev/null || exit 0; sleep 0.2; done
  pkill -9 -x "RB Stems Plus"; true ;;
*) sed -n '2,78p' "$0"; exit 2 ;;
esac
