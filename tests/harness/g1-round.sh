#!/bin/bash
# G1 item 1 (mac-core.md §9) from an UNMODIFIED tag TEST build: the owner's round. Those builds have
# no probe, so the owner clicks in the VM's window (tests/harness/g1-round.md, step by step);
# everything else is done here (lane L1, tart). One fresh clone of rb26-template per starting
# point. Run outside the sandbox. Every check runs as root in the VM (DJ's Library is 700).
#
#   tests/harness/g1-round.sh stage FROM          FROM: v1.0.0 | v1.3.0. A fresh clone (its window),
#                                                 DJ's screen saver off, rekordbox's file tree
#                                                 recorded, the old build served and installed with
#                                                 its bootstrap (as DJ, no password); the owner then
#                                                 installs Stems Cache and Demucs v4 (checklist 1)
#   tests/harness/g1-round.sh switch FROM         the old app's watcher registration recorded, the
#                                                 new build served instead, the old app quit, its
#                                                 saved payload.json removed, the app opened: it
#                                                 offers 1.4.0 (checklist 2: Update)
#   tests/harness/g1-round.sh verify-update FROM  the new app is the core build, updated, no
#                                                 quarantine, its watcher registered again for the
#                                                 new helper (its identity changed, "watcher:
#                                                 installed" logged by the new app) and run by a
#                                                 WatchPaths trigger (checklist 3 next)
#   tests/harness/g1-round.sh verify-allow FROM   checklist 3's Reinstall Stems Cache: App
#                                                 Management refused it, then the owner's Allow,
#                                                 Later, Try Again in the SAME process succeeded
#   tests/harness/g1-round.sh note FROM TEXT      the owner's observation into verify.txt (e.g.
#                                                 whether the notification appeared)
#   tests/harness/g1-round.sh verify-uninstall FROM   after the complete uninstall: rekordbox byte
#                                                 for byte as before, nothing of ours left
#   tests/harness/g1-round.sh done FROM           the server stopped, the clone deleted
# Each verify step prints "verified" and exits 0 only when every one of its checks passed; any FAIL
# makes it exit 1. build/harness/g1-FROM/verify.txt collects every line: the record to archive.
# The builds: OLD = build/g1-FROM/dist-test (the tag's TEST build, mac-A's test key; also in
# gates-archive/tag-FROM), NEW = build/g1-core/dist-test (w2/spike-mac's core-linked 1.4.0, its
# commit in build/g1-core.commit).
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
vm="${G1_VM:-$root/tests/harness/vm.sh}"      # G1_VM: the selftest's stub
from="${2:-}"; case "$from" in v1.0.0|v1.3.0) ;; *) sed -n '2,33p' "$0"; exit 2 ;; esac
old="$root/build/g1-$from/dist-test"; new="$root/build/g1-core/dist-test"
st="${G1_STATE:-$root/build/harness/g1-$from}"; mkdir -p "$st"   # G1_STATE: the selftest's
out="$st/verify.txt"
SUP="/Users/dj/Library/Application Support/rbstemsplus"
LOGS=/Users/dj/Library/Logs/rbstemsplus
APPP="/Applications/RB Stems Plus.app"
die() { echo "g1-round.sh: $*" | tee -a "$out" >&2; exit 1; }
fails=0
# check 'COMMAND' TEXT: COMMAND runs in the VM as admin (put sudo in it for DJ's files); its
# outcome goes into verify.txt; returns 1 on FAIL
check() {
  if "$vm" sh "$1" >/dev/null 2>&1; then echo "ok   $2" | tee -a "$out"; return 0; fi
  echo "FAIL $2" | tee -a "$out"; fails=$((fails + 1)); return 1
}
verdict() {   # verdict STEP NEXT: "verified" only with no FAIL
  if [ "$fails" = 0 ]; then echo "verified: $1 ($(date '+%F %T')). $2" | tee -a "$out"; exit 0; fi
  echo "NOT VERIFIED: $1, $fails checks failed ($(date '+%F %T')): stop, and write down what the window said" | tee -a "$out"; exit 1
}
serve() {
  [ -f "$st/server.pid" ] && kill "$(cat "$st/server.pid")" 2>/dev/null
  nohup python3 -I "$root/tests/harness/serve.py" "$1" > "$st/server.log" 2>&1 & echo $! > "$st/server.pid"
  local i; for i in $(seq 1 50); do curl -s -o "$st/served.json" --max-time 2 http://192.168.64.1:8765/payload.json && break; sleep 0.1; done
  cmp -s "$st/served.json" "$1/payload.json" || die "the server on 8765 doesn't serve $1"
}
as_dj() { "$vm" sh "sudo launchctl asuser \$(id -u dj) sudo -u dj -H bash -c $(printf '%q' "$1")"; }
tree() {
  "$vm" sh 'cd "/Applications/rekordbox 7" && sudo find . \( -type f -exec shasum -a 256 {} + \) -o \( -type l -exec sh -c '"'"'for l; do echo "link $l -> $(readlink "$l")"; done'"'"' sh {} + \) -o \( -type d -exec stat -f "dir %N %Su %Sp" {} + \) | LC_ALL=C sort; sudo shasum -a 256 "/Users/dj/Library/Application Support/Pioneer/rekordbox6/models/demucs3_model/hdemucs.onnx"'
}
agent='gui/$(id -u dj)/io.github.rbstemsplus.watcher'

case "$1" in
stage)
  [ -f "$old/rbstemsplus-app.zip" ] && [ -f "$new/rbstemsplus-app.zip" ] || die "build the TEST builds first"
  if curl -s -o /dev/null --max-time 2 http://192.168.64.1:8765/; then die "something already serves 192.168.64.1:8765"; fi
  : > "$out"
  echo "G1 item 1 from $from to $(cat "$root/build/g1-core.commit" 2>/dev/null || echo "build/g1-core") ($(date '+%F %T'))" >> "$out"
  "$vm" up "g1-$from-$(date +%m%d%H%M)" || die "the VM didn't come up"
  tree > "$st/rekordbox-before.txt" || die "rekordbox's tree couldn't be read"
  serve "$old"
  as_dj 'curl -fsSL http://192.168.64.1:8765/bootstrap.sh | RBSTEMSPLUS_BASE_URL=http://192.168.64.1:8765/ bash' > "$st/bootstrap.log" 2>&1 || die "the bootstrap failed (see $st/bootstrap.log)"
  check "[ -d \"$APPP\" ] && ! xattr -r \"$APPP\" | grep -q quarantine" "$from installed by its bootstrap, no quarantine"
  verdict "staged" "The owner's checklist 1 now (tests/harness/g1-round.md)" ;;
switch)
  check "sudo launchctl print $agent" "$from's watcher registered before the update"
  "$vm" sh "sudo launchctl print $agent" > "$st/watcher-before.txt" 2>&1
  "$vm" sh "sudo cat \"$SUP/watcher.registered\" 2>/dev/null || echo none" > "$st/registered-before.txt"
  # the log's length as root (with sudo before wc, the shell's redirect would open the log without
  # root: DJ's Logs are 700), so
  # verify-update reads only the new app's lines; 0 only when there is no log yet
  "$vm" sh "sudo sh -c 'if [ -f \"$LOGS/app.log\" ]; then wc -l < \"$LOGS/app.log\"; else echo 0; fi'" | tr -d ' ' > "$st/applog-lines-before.txt"
  if grep -qx '[0-9][0-9]*' "$st/applog-lines-before.txt"; then echo "ok   the app log's length before the update: $(cat "$st/applog-lines-before.txt") lines" | tee -a "$out"
  else echo "FAIL the app log's length before the update couldn't be read" | tee -a "$out"; fails=$((fails + 1)); fi
  serve "$new"
  "$vm" sh "sudo pkill -x \"RB Stems Plus\"; sleep 2; sudo rm -f \"$SUP/payload.json\" \"$SUP/payload.json.sig\" \"$SUP/payload/payload.json\" \"$SUP/payload/payload.json.sig\""
  as_dj 'open "/Applications/RB Stems Plus.app"'
  verdict "switched" "The owner's checklist 2 now (Update)" ;;
verify-update)
  n0="$(cat "$st/applog-lines-before.txt" 2>/dev/null || echo 0)"
  check "[ \"\$(defaults read \"$APPP/Contents/Info.plist\" CFBundleShortVersionString)\" = 1.4.0 ]" "the app is 1.4.0"
  check "grep -aq core_selftest \"$APPP/Contents/MacOS/RB Stems Plus\"" "it is the core-linked build (core_selftest in it; --swap ran its self-test)"
  check "sudo tail -n +$((n0 + 1)) \"$LOGS/app.log\" | grep -q 'update to 1.4.0: updated'" "the update script says updated"
  check "! xattr -r \"$APPP\" | grep -q com.apple.quarantine" "no quarantine"
  check "codesign --verify --deep --strict \"$APPP\"" "the new app verifies (ad hoc)"
  # the watcher registered again for the new helper: a new identity, logged by the new app
  "$vm" sh "sudo cat \"$SUP/watcher.registered\" 2>/dev/null || echo none" > "$st/registered-after.txt"
  if [ -s "$st/registered-after.txt" ] && ! grep -qx none "$st/registered-after.txt" && ! cmp -s "$st/registered-before.txt" "$st/registered-after.txt"; then
    echo "ok   the watcher's registration record changed (the new helper's code)" | tee -a "$out"
  else echo "FAIL the watcher's registration record didn't change (before: $(tail -1 "$st/registered-before.txt"), after: $(tail -1 "$st/registered-after.txt"))" | tee -a "$out"; fails=$((fails + 1)); fi
  check "sudo tail -n +$((n0 + 1)) \"$LOGS/app.log\" | grep -q 'watcher: installed'" "the new app registered the watcher (\"watcher: installed\" after the update)"
  check "sudo launchctl print $agent | grep -q 'Helpers/RB Stems Plus Watcher.app'" "the agent starts the new app's watcher helper"
  # a WatchPaths trigger (rekordbox's Info.plist touched; its content unchanged): the new helper runs
  runs0="$("$vm" sh "sudo launchctl print $agent | sed -n 's/^[[:space:]]*runs = //p'" 2>/dev/null | tr -d ' ')"
  "$vm" sh 'sudo touch "/Applications/rekordbox 7/rekordbox.app/Contents/Info.plist"'
  ran=""
  for _ in $(seq 1 30); do
    sleep 2
    runs1="$("$vm" sh "sudo launchctl print $agent | sed -n 's/^[[:space:]]*runs = //p'" 2>/dev/null | tr -d ' ')"
    [ -n "$runs1" ] && [ "$runs1" != "${runs0:-}" ] && { ran=1; break; }
  done
  if [ -n "$ran" ]; then echo "ok   a WatchPaths trigger ran the watcher (launchd runs ${runs0:-?} -> $runs1)" | tee -a "$out"
  else echo "FAIL a WatchPaths trigger didn't run the watcher within 60 s (runs ${runs0:-?})" | tee -a "$out"; fails=$((fails + 1)); fi
  check "sudo test -s \"$LOGS/watcher.log\"" "the watcher wrote its log"
  verdict "the update" "The owner's checklist 3 now (Reinstall, Install, Uninstall)" ;;
verify-allow)
  # App Management: the new build's root run refused, the owner's Allow, Later, then Try Again in
  # the same process succeeded (no "started:" line between the attempts)
  "$vm" sh "sudo cat \"$LOGS/app.log\"" > "$st/app.log" 2>/dev/null
  python3 -I - "$st/app.log" > "$st/allow.txt" <<'EOF'
import re, sys
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
start = max((i for i, l in enumerate(lines) if "started: RB Stems Plus 1.4.0" in l), default=None)
if start is None:
    print("FAIL no start of 1.4.0 in the log"); sys.exit(1)
run = [l for l in lines[start:] if "admin run (" in l or "Operation not permitted" in l or re.search(r"^\S+ \S+\s+ ok: ", l) or "user chose: Try Again" in l or "started:" in l]
refused = any("Operation not permitted" in l for l in run)
again = any("user chose: Try Again" in l for l in run)
ok_after = False
seen_try = False
for l in lines[start:]:
    if "started:" in l and seen_try:
        break                                   # a relaunch: not the same process
    if "user chose: Try Again" in l:
        seen_try = True
    if seen_try and re.search(r"\s ok: ", l):
        ok_after = True
        break
print(("ok  " if refused else "FAIL") + " App Management refused the new build's first root run (Operation not permitted)")
print(("ok  " if again else "FAIL") + " the owner chose Try Again")
print(("ok  " if ok_after else "FAIL") + " the root run succeeded after Try Again in the same process")
sys.exit(0 if refused and again and ok_after else 1)
EOF
  r=$?; sed 's/^ok  /ok   /' "$st/allow.txt" | tee -a "$out"
  [ $r = 0 ] || fails=$((fails + 1))
  verdict "App Management's Allow" "On with checklist 3" ;;
note)
  shift 2; echo "note $(date '+%T'): $*" | tee -a "$out" ;;
verify-uninstall)
  tree > "$st/rekordbox-after.txt"
  if cmp -s "$st/rekordbox-before.txt" "$st/rekordbox-after.txt"; then echo "ok   rekordbox byte for byte as before (tree and model)" | tee -a "$out"
  else echo "FAIL rekordbox differs (diff $st/rekordbox-before.txt $st/rekordbox-after.txt)" | tee -a "$out"; fails=$((fails + 1)); fi
  check 'codesign --verify --deep --strict "/Applications/rekordbox 7/rekordbox.app"' "rekordbox verifies (AlphaTheta's signature)"
  check "sudo test ! -e \"$APPP\" && sudo test ! -e \"$SUP\" && sudo test ! -e $LOGS && sudo test ! -e \"/Library/Application Support/rbstemsplus\" && sudo test ! -e /Users/dj/Library/LaunchAgents/io.github.rbstemsplus.watcher.plist" "nothing of RB Stems Plus left (checked as root)"
  check "! sudo launchctl print $agent" "no watcher registered"
  verdict "the complete uninstall" "Archive $out with the logs, then: g1-round.sh done $from" ;;
done)
  [ -f "$st/server.pid" ] && kill "$(cat "$st/server.pid")" 2>/dev/null; rm -f "$st/server.pid"
  "$vm" delete ;;
*) sed -n '2,33p' "$0"; exit 2 ;;
esac
